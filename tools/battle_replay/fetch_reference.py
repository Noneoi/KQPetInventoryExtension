"""Download official packages and preserve all named DefineBinaryData payloads.

Only the official manifest determines the versioned URLs. Original bytes and
SHA-256 provenance are retained; no third-party replay tables are consumed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import struct
import urllib.request
import xml.etree.ElementTree as ET
import zlib
import io
import time
import zipfile
import re
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

BASE = 'https://aoqi.100bt.com/play/'
MAX_SWF_BYTES = 128 * 1024 * 1024


def fetch(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0', 'Cache-Control': 'no-cache',
                                                  'Referer': BASE + 'play.html'})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def binary_payloads(data):
    if len(data) < 9 or len(data) > MAX_SWF_BYTES:
        raise ValueError('invalid SWF package length')
    expected = struct.unpack_from('<I', data, 4)[0] - 8
    if not 1 <= expected <= MAX_SWF_BYTES - 8:
        raise ValueError('SWF expansion exceeds the limit')
    signature = data[:3]
    if signature == b'FWS':
        body = data[8:]
    elif signature == b'CWS':
        decoder = zlib.decompressobj()
        body = decoder.decompress(data[8:], expected + 1)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError('invalid or oversized compressed SWF stream')
    elif signature == b'ZWS':
        if len(data) < 18:
            raise ValueError('truncated LZMA SWF')
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=MAX_SWF_BYTES)
        body = decoder.decompress(data[12:17] + b'\xff' * 8 + data[17:], max_length=expected + 1)
        if not decoder.eof or decoder.unused_data:
            raise ValueError('invalid or oversized LZMA SWF stream')
    else:
        raise ValueError('not a SWF package')
    if len(body) != expected:
        raise ValueError('SWF decompressed length mismatch')
    offset = (5 + (body[0] >> 3) * 4 + 7) // 8 + 4
    symbols, blobs = {}, {}
    while offset + 2 <= len(body):
        record, = struct.unpack_from('<H', body, offset)
        offset += 2
        code, size = record >> 6, record & 63
        if size == 63:
            if offset + 4 > len(body):
                raise ValueError('truncated SWF long tag')
            size, = struct.unpack_from('<I', body, offset)
            offset += 4
        payload = body[offset:offset + size]
        if len(payload) != size:
            raise ValueError('truncated SWF tag')
        offset += size
        if code == 87:
            if len(payload) < 6:
                raise ValueError('truncated DefineBinaryData')
            symbol, = struct.unpack_from('<H', payload)
            blobs[symbol] = payload[6:]
        elif code == 76:
            if len(payload) < 2:
                raise ValueError('truncated SymbolClass')
            count, = struct.unpack_from('<H', payload)
            pos = 2
            for _ in range(count):
                if pos + 2 > len(payload):
                    raise ValueError('truncated SWF symbol')
                symbol, = struct.unpack_from('<H', payload, pos)
                pos += 2
                end = payload.index(0, pos)
                symbols[symbol] = payload[pos:end].decode('utf-8')
                pos = end + 1
        if code == 0:
            break
    return [(symbols.get(i, str(i)), blob) for i, blob in blobs.items()]


def save_binary_payloads(data, output, resource, evidence):
    """Keep even payloads that are not parseable XML; never silently omit them."""
    for name, blob in binary_payloads(data):
        if not re.fullmatch(r'[A-Za-z0-9_.$]+', name) or name in ('.', '..'):
            raise ValueError('unsafe SWF symbol name')
        filename = name.replace('/', '_').replace('\\', '_')
        raw = output / 'binary' / (filename + '.bin')
        raw.parent.mkdir(parents=True, exist_ok=True)
        raw.write_bytes(blob)
        record = {'file': raw.relative_to(output).as_posix(), 'package': resource,
                  'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}
        evidence.setdefault('binary', {})[name] = record
        try:
            ET.fromstring(blob)
        except (ET.ParseError, ValueError):
            continue
        path = output / 'xml' / (filename + '.xml')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
        evidence.setdefault('xml', {})[name] = {**record, 'file': path.relative_to(output).as_posix()}


def flash_versions(manifest, version_package):
    """LoaderContainer: versiondata.vconfig first, then start.u.f overrides.

    VersManager uses version 2000 for SWF resources absent from both maps;
    start.v is the versiondata package version, not a resource default.
    """
    payloads = {name: blob for name, blob in binary_payloads(version_package)}
    base = ET.fromstring(payloads['mmo.versiondata.DocClass_cls'])
    versions = {row.get('n'): row.get('v') for row in base.iter('f')}
    versions.update({row.get('n'): row.get('v') for row in ET.fromstring(manifest).findall('./u/f')})
    return versions


def saved_flash_versions(source, manifest_name='start.xml'):
    return flash_versions((source / manifest_name).read_bytes(), (source / 'versiondata.swf').read_bytes())


def fetch_flash_versions(output, manifest):
    version = ET.fromstring(manifest).findtext('v')
    url = BASE + 'versiondata~' + version + '.swf'
    data = fetch(url)
    (output / 'versiondata.swf').write_bytes(data)
    evidence = {'resource': 'versiondata', 'version': version, 'url': url,
                'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                'manifestSha256': hashlib.sha256(manifest).hexdigest()}
    save_binary_payloads(data, output / 'version-data', 'versiondata', evidence)
    (output / 'versiondata-provenance.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    return flash_versions(manifest, data), evidence


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_url = BASE + f'start.xml?replay_audit={int(time.time() * 1000)}'
    manifest = fetch(manifest_url)
    (args.output / 'start.xml').write_bytes(manifest)
    versions, version_evidence = fetch_flash_versions(args.output, manifest)
    evidence = {'retrievedUtc': datetime.now(timezone.utc).isoformat(), 'manifestUrl': manifest_url,
                'manifestSha256': hashlib.sha256(manifest).hexdigest(), 'versiondata': version_evidence,
                'packages': [], 'xml': {}}
    for resource in ('library/interfaces', 'battle/battle', 'battleconfig/battleconfig'):
        version = versions.get(resource, '2000')
        url = BASE + resource + '~' + version + '.swf'
        data = fetch(url)
        filename = resource.replace('/', '_') + '.swf'
        (args.output / filename).write_bytes(data)
        evidence['packages'].append({'resource': resource, 'version': version, 'url': url,
                                     'file': filename, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        save_binary_payloads(data, args.output, resource, evidence)
        print(resource, len(data), flush=True)
    (args.output / 'provenance.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    fetch_h5(args.output)
    print('XML payloads:', len(evidence['xml']))


def fetch_h5(output):
    base = 'https://aoqi.100bt.com/h5/'
    records = []
    def save(url, filename):
        data = fetch(url)
        (output / filename).write_bytes(data)
        records.append({'url': url, 'file': filename, 'sha256': hashlib.sha256(data).hexdigest()})
        return data
    start = json.loads(save(base + f'start~{int(time.time() * 1000)}.json', 'h5-start.json'))
    versions = json.loads(save(base + f'version~{start["version"]}.json', 'h5-version.json'))
    version = versions['config/battleconfig.aqz']
    data = save(base + f'config/battleconfig~{version}.aqz', 'h5-battleconfig.aqz')
    members = []
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            member = PurePosixPath(info.filename)
            if member.is_absolute() or '..' in member.parts or '\\' in info.filename:
                raise ValueError(f'unsafe archive member: {info.filename}')
            payload = archive.read(info)
            path = output / 'h5-tables' / Path(*member.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
            members.append({'member': info.filename, 'file': path.relative_to(output).as_posix(),
                            'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()})
        for name in ('skill_desc_config.json', 'skill_config.json'):
            matches = [n for n in archive.namelist() if n.rsplit('/', 1)[-1] == name]
            if len(matches) != 1:
                raise ValueError(f'ambiguous package member: {name}')
            (output / name).write_bytes(archive.read(matches[0]))
    (output / 'h5-provenance.json').write_text(json.dumps({
        'retrievedUtc': datetime.now(timezone.utc).isoformat(), 'version': str(version),
        'files': records, 'members': members}, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
