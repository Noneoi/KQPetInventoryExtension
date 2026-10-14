"""Retain manifest-pinned external resources and report literal ID candidates.

An ID occurrence is only a search lead, never an inferred Buff name or meaning.
No downloaded code or SWF is executed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import xml.etree.ElementTree as ET
import zipfile

from fetch_reference import fetch, save_binary_payloads


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding='utf-8')


def download_h5(root):
    versions = json.loads((root / 'h5-version.json').read_text(encoding='utf-8'))
    selected = [(key, version) for key, version in versions.items()
                if key.startswith(('config/', 'configfuture/'))]

    def one(item):
        resource, version = item
        path = PurePosixPath(resource)
        url = 'https://aoqi.100bt.com/h5/' + str(path.with_name(path.stem + '~' + str(version) + path.suffix))
        result = {'resource': resource, 'version': str(version), 'url': url}
        try:
            data = fetch(url)
            dest = root / 'h5-all-config' / 'raw' / resource
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            result.update(file=dest.relative_to(root).as_posix(), bytes=len(data), sha256=digest(data))
            if path.suffix == '.aqz':
                members = []
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    for info in archive.infolist():
                        if info.is_dir():
                            continue
                        member = PurePosixPath(info.filename)
                        if member.is_absolute() or '..' in member.parts or '\\' in info.filename or ':' in info.filename:
                            raise ValueError('unsafe archive member: ' + info.filename)
                        content = archive.read(info)
                        out = root / 'h5-all-config' / 'members' / resource / info.filename
                        out.parent.mkdir(parents=True, exist_ok=True)
                        out.write_bytes(content)
                        members.append({'member': info.filename, 'file': out.relative_to(root).as_posix(),
                                        'bytes': len(content), 'sha256': digest(content)})
                result['members'] = members
        except Exception as error:
            result['error'] = str(error)
        return result

    with ThreadPoolExecutor(max_workers=6) as pool:
        rows = list(pool.map(one, selected))
    report = {'retrievedUtc': datetime.now(timezone.utc).isoformat(),
              'manifestSha256': digest((root / 'h5-version.json').read_bytes()), 'files': rows}
    write_json(root / 'h5-all-config' / 'provenance.json', report)
    return {'resources': len(rows), 'downloaded': sum('file' in r for r in rows),
            'members': sum(len(r.get('members', [])) for r in rows),
            'bytes': sum(r.get('bytes', 0) for r in rows),
            'errors': [r for r in rows if 'error' in r]}


def download_flash(root, resources):
    manifest = ET.fromstring((root / 'play-start.xml').read_bytes())
    base_path = root / 'flash-base-versions.json'
    if not base_path.exists():
        raise ValueError('Fetch the LoaderContainer versiondata package first; start.xml is only an override list')
    versions = json.loads(base_path.read_text(encoding='utf-8'))
    overrides = {r.get('n'): r.get('v') for r in manifest.iter('f')}
    versions.update(overrides)

    def one(resource):
        version = versions.get(resource, '2000')
        url = f'https://aoqi.100bt.com/play/{resource}~{version}.swf'
        result = {'resource': resource, 'version': version, 'url': url,
                  'resolution': 'start-override' if resource in overrides else 'versiondata' if resource in versions else 'VersManager-fallback-2000'}
        try:
            data = fetch(url)
            out = root / 'flash-resolved' / (resource.replace('/', '_') + '.swf')
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(data)
            result.update(file=out.relative_to(root).as_posix(), bytes=len(data), sha256=digest(data))
            save_binary_payloads(data, root / 'flash-resolved', resource, result)
        except Exception as error:
            result['error'] = str(error)
        return result

    with ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(one, resources))
    report = {'retrievedUtc': datetime.now(timezone.utc).isoformat(),
              'manifestSha256': digest((root / 'play-start.xml').read_bytes()),
              'versiondataSha256': digest((root / 'versiondata.swf').read_bytes()), 'files': rows}
    path = root / 'flash-resolved' / 'provenance.json'
    if path.exists():
        prior = json.loads(path.read_text(encoding='utf-8'))
        replaced = set(resources)
        report['files'] = [r for r in prior['files'] if r['resource'] not in replaced] + rows
    write_json(path, report)
    return {'resources': len(rows), 'downloaded': sum('file' in r for r in rows),
            'bytes': sum(r.get('bytes', 0) for r in rows), 'errors': [r for r in rows if 'error' in r]}


def scan_text(root, coverage, inputs):
    entries = json.loads(coverage.read_text(encoding='utf-8'))['buffs']['entries']
    ids = {key for key, row in entries.items() if not row['hasDescription']}
    unnamed = {key for key, row in entries.items() if row['status'] == 'officially_unnamed'}
    pattern = re.compile(r'(?<![0-9])(' + '|'.join(sorted(ids, key=lambda key: (-len(key), key))) + r')(?![0-9])')
    records, files, failures = [], [], []
    for folder in inputs:
        for path in sorted(folder.rglob('*')):
            if not path.is_file() or path.suffix.lower() not in ('.json', '.xml', '.as', '.txt', '.js'):
                continue
            data = path.read_bytes()
            try:
                text = data.decode('utf-8-sig')
            except UnicodeDecodeError:
                failures.append({'file': str(path.resolve()), 'reason': 'not-utf8'})
                continue
            counts = {}
            for match in pattern.finditer(text):
                key = match.group(1)
                counts[key] = counts.get(key, 0) + 1
                # All matches are counted. Representative context is capped,
                # while the entire original file and hash remain available.
                if counts[key] <= 20:
                    records.append({'file': str(path.resolve()), 'id': key, 'unnamedTarget': key in unnamed,
                                    'characterOffset': match.start(), 'line': text.count('\n', 0, match.start()) + 1,
                                    'context': text[max(0, match.start() - 170):match.end() + 170]})
            files.append({'file': str(path.resolve()), 'bytes': len(data), 'sha256': digest(data), 'matches': counts})
    report = {'scope': 'Literal search candidates only; namespaces and mapping must be verified separately.',
              'targetIds': sorted(ids, key=int), 'unnamedIds': sorted(unnamed, key=int),
              'files': files, 'decodeFailures': failures, 'candidates': records}
    write_json(root / 'external-text-candidates.json', report)
    return {'files': len(files), 'matchingFiles': sum(bool(f['matches']) for f in files),
            'candidates': len(records), 'decodeFailures': failures}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['h5', 'flash', 'scan'])
    parser.add_argument('root', type=Path)
    parser.add_argument('resources', nargs='*')
    parser.add_argument('--coverage', type=Path)
    args = parser.parse_args()
    if args.mode == 'h5':
        result = download_h5(args.root)
    elif args.mode == 'flash':
        result = download_flash(args.root, args.resources)
    else:
        result = scan_text(args.root, args.coverage, [Path(p) for p in args.resources])
    print(json.dumps(result, ensure_ascii=False))
