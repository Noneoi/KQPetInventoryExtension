"""Static SWF inventory and exact-ID constant scan; never executes ActionScript.

Presence in an integer/string pool is a candidate for manual inspection, not an
ID-to-description mapping. The report deliberately makes no semantic matches.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import lzma
from pathlib import Path
import re
import struct
import zlib


def decompress_swf(data: bytes) -> bytes:
    signature = data[:3]
    if signature == b'FWS':
        body = data[8:]
    elif signature == b'CWS':
        body = zlib.decompress(data[8:])
    elif signature == b'ZWS':
        body = lzma.decompress(data[12:17] + b'\xff' * 8 + data[17:], format=lzma.FORMAT_ALONE)
    else:
        raise ValueError('not SWF')
    if len(body) != struct.unpack_from('<I', data, 4)[0] - 8:
        raise ValueError('decompressed length mismatch')
    return body


def tags(data: bytes, offset: int = 0, prefix: str = ''):
    index = 0
    while offset + 2 <= len(data):
        start = offset
        record = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        code, length = record >> 6, record & 63
        if length == 63:
            length = struct.unpack_from('<I', data, offset)[0]
            offset += 4
        end = offset + length
        if end > len(data):
            raise ValueError('truncated SWF tag')
        payload = data[offset:end]
        location = f'{prefix}{index}'
        yield location, code, start, payload
        if code == 39:
            yield from tags(payload, 4, location + '/')
        offset = end
        index += 1
        if code == 0:
            break


class Reader:
    def __init__(self, data: bytes):
        self.data, self.offset = data, 0

    def take(self, length: int) -> bytes:
        result = self.data[self.offset:self.offset + length]
        if len(result) != length:
            raise ValueError('truncated field')
        self.offset += length
        return result

    def uint(self):
        result = 0
        for shift in range(0, 35, 7):
            value = self.take(1)[0]
            result |= (value & 127) << shift
            if not value & 128:
                return result & 0xffffffff
        raise ValueError('invalid ABC variable integer')

    def cstring(self):
        end = self.data.index(0, self.offset)
        result = self.data[self.offset:end].decode('utf-8')
        self.offset = end + 1
        return result


def abc_pools(data: bytes):
    reader = Reader(data)
    minor, major = struct.unpack('<HH', reader.take(4))
    if major != 46 or minor not in (0, 16):
        raise ValueError(f'unsupported ABC version {major}.{minor}')
    numbers, strings = [], []
    for kind in ('int', 'uint'):
        for index in range(1, reader.uint()):
            value = reader.uint()
            if kind == 'int' and value >= 0x80000000:
                value -= 0x100000000
            numbers.append((kind, index, value))
    for index in range(1, reader.uint()):
        numbers.append(('double', index, struct.unpack('<d', reader.take(8))[0]))
    for index in range(1, reader.uint()):
        strings.append((index, reader.take(reader.uint()).decode('utf-8')))
    return {'major': major, 'minor': minor, 'constantPoolBytesRead': reader.offset}, numbers, strings


def edit_text(data: bytes):
    reader = Reader(data)
    character_id = struct.unpack('<H', reader.take(2))[0]
    rect_bits = data[reader.offset] >> 3
    reader.take((5 + 4 * rect_bits + 7) // 8)
    first, second = reader.take(2)
    if first & 1:  # HasFont
        reader.take(2)
    if second & 128:  # HasFontClass
        reader.cstring()
    if first & 1:
        reader.take(2)
    if first & 4:  # HasTextColor
        reader.take(4)
    if first & 2:  # HasMaxLength
        reader.take(2)
    if second & 32:  # HasLayout
        reader.take(9)
    variable = reader.cstring()
    text = reader.cstring() if first & 128 else ''
    return {'characterId': character_id, 'variableName': variable, 'initialText': text}


def scan_swf(data: bytes, targets: set[str]) -> dict:
    body = decompress_swf(data)
    offset = (5 + (body[0] >> 3) * 4 + 7) // 8 + 4
    records = list(tags(body, offset))
    exact_number = {int(value): value for value in targets}
    pattern = re.compile(r'(?<![0-9])(?:' + '|'.join(sorted(targets, key=len, reverse=True)) + r')(?![0-9])')
    symbols, abc, texts, binaries, binary_inventory, errors = {}, [], [], [], [], []
    for location, code, _, payload in records:
        if code == 76:
            reader = Reader(payload)
            for _ in range(struct.unpack('<H', reader.take(2))[0]):
                character = struct.unpack('<H', reader.take(2))[0]
                symbols[character] = reader.cstring()
    for location, code, _, payload in records:
        try:
            if code in (72, 82):
                reader = Reader(payload)
                name = ''
                if code == 82:
                    reader.take(4)
                    name = reader.cstring()
                metadata, numbers, strings = abc_pools(payload[reader.offset:])
                integer_hits = [{'pool': kind, 'index': index, 'id': exact_number[value]}
                                for kind, index, value in numbers if value in exact_number]
                string_hits = [{'index': index, 'ids': sorted(set(pattern.findall(text)), key=int), 'text': text}
                               for index, text in strings if pattern.search(text)]
                abc.append({'tag': location, 'name': name, **metadata,
                            'integerHits': integer_hits, 'stringHits': string_hits,
                            'stringCount': len(strings), 'strings': strings})
            elif code == 37:
                texts.append({'tag': location, **edit_text(payload)})
            elif code == 87:
                character = struct.unpack_from('<H', payload)[0]
                blob = payload[6:]
                inventory = {'tag': location, 'characterId': character, 'symbol': symbols.get(character, ''),
                             'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob),
                             'firstBytes': blob[:12].hex()}
                try:
                    text = blob.decode('utf-8-sig')
                except UnicodeDecodeError:
                    inventory['encoding'] = 'not_utf8'
                    inventory['asciiIdHits'] = sorted({part.decode('ascii') for part in
                        re.findall(rb'(?<![0-9])[0-9]+(?![0-9])', blob) if part.decode('ascii') in targets}, key=int)
                    binary_inventory.append(inventory)
                    continue
                inventory['encoding'] = 'utf8'
                binary_inventory.append(inventory)
                hits = sorted(set(pattern.findall(text)), key=int)
                if hits:
                    binaries.append({'tag': location, 'characterId': character, 'symbol': symbols.get(character, ''),
                                     'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob),
                                     'ids': hits, 'text': text})
        except (ValueError, IndexError, struct.error, UnicodeDecodeError) as exc:
            errors.append({'tag': location, 'code': code, 'error': str(exc)})
    return {'uncompressedBytes': len(body) + 8, 'tagCounts': dict(Counter(str(t[1]) for t in records)),
            'abc': abc, 'editTexts': texts, 'binaryHits': binaries, 'binaryInventory': binary_inventory, 'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('targets', type=Path, help='JSON object keyed by Buff ID')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    targets = set(json.loads(args.targets.read_text(encoding='utf-8')))
    args.output.mkdir(parents=True, exist_ok=True)
    packages, hashes = [], {}
    files = sorted(args.source.rglob('*.swf')) if args.source.is_dir() else [args.source]
    for path in files:
        data = path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if sha in hashes:
            hashes[sha]['aliases'].append(str(path.resolve()))
            continue
        item = {'file': str(path.resolve()), 'aliases': [], 'sha256': sha, 'bytes': len(data)}
        try:
            result = scan_swf(data, targets)
            detail = args.output / (sha + '.json')
            detail.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            item.update({'detail': detail.name, 'tagCounts': result['tagCounts'], 'errors': result['errors'],
                         'abcIds': sorted({hit['id'] for abc in result['abc'] for hit in abc['integerHits']}, key=int),
                         'stringIds': sorted({id for abc in result['abc'] for hit in abc['stringHits'] for id in hit['ids']}, key=int),
                         'binaryIds': sorted({id for blob in result['binaryHits'] for id in blob['ids']}, key=int),
                         'editTextCount': len(result['editTexts'])})
        except (ValueError, IndexError, struct.error, zlib.error, lzma.LZMAError) as exc:
            item['error'] = str(exc)
        packages.append(item)
        hashes[sha] = item
    report = {'source': str(args.source.resolve()), 'targetIds': sorted(targets, key=int),
              'files': len(files), 'uniquePackages': len(packages), 'packages': packages,
              'limits': ['Integer/string occurrence is not proof of a Buff ID or textual meaning.',
                         'Static glyph text (DefineText/DefineText2) is counted but requires font decoding separately.',
                         'Non-UTF8 binary records are inventoried and ASCII-ID scanned, not semantically decoded.',
                         'No ActionScript instructions or game resource code are executed.']}
    (args.output / 'scan.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'files': len(files), 'uniquePackages': len(packages),
                      'failedPackages': sum('error' in p for p in packages),
                      'tagParseErrors': sum(len(p.get('errors', [])) for p in packages),
                      'integerHitPackages': sum(bool(p.get('abcIds')) for p in packages),
                      'stringHitPackages': sum(bool(p.get('stringIds')) for p in packages),
                      'binaryHitPackages': sum(bool(p.get('binaryIds')) for p in packages)}))


if __name__ == '__main__':
    main()
