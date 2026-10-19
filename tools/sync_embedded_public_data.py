"""Export only verified public catalog files into the shipped offline baseline."""
from __future__ import annotations

import argparse
import json
import re
from datetime import datetime
from pathlib import Path

from public_update_store import pointer, read_json, version_files
from public_update_support import atomic_bytes, canonical, file_sha, sha

FILES = ('pet-detail-data.json', 'pet-skill-data.json', 'shop-exchange-data.json',
         'activity-exchange-data.json', 'routine-overview.json', 'pet-image-index.json',
         'public-icon-index.json', 'activity-evolution-data.json', 'official-versions.json')
MANIFEST = 'public-data-snapshot.json'


def check(assets: Path):
    manifest = read_json(assets / MANIFEST)
    if manifest.get('schema') != 1 or set(manifest.get('files', {})) != set(FILES):
        raise ValueError('embedded public snapshot manifest is incomplete')
    if not re.fullmatch(r'[0-9a-f]{64}', manifest.get('sourceRevision', '')) or not datetime.fromisoformat(manifest.get('verifiedUtc', '').replace('Z', '+00:00')).tzinfo:
        raise ValueError('embedded public snapshot identity/time is invalid')
    for name, record in manifest['files'].items():
        path = assets / name
        if path.stat().st_size != record.get('bytes') or file_sha(path) != record.get('sha256'):
            raise ValueError('embedded public snapshot differs from its verified export: ' + name)
    skills = read_json(assets / 'pet-skill-data.json')
    rules = skills.get('relationRules', {})
    if rules.get('schema') != 1 or any(rules.get(kind, {}).get('available') is not True for kind in ('summon','carry')):
        raise ValueError('embedded summon/carry rules are incomplete')
    pets = read_json(assets / 'pet-detail-data.json').get('pets', {})
    if not pets or not set(pets).issubset(skills.get('pets', {})):
        raise ValueError('embedded skill and pet species do not describe a coherent snapshot')
    routines = read_json(assets / 'routine-overview.json')
    if not routines.get('tasks') or not isinstance(routines.get('activities'), list):
        raise ValueError('embedded routine catalog is incomplete')
    return manifest


def export(root: Path, assets: Path):
    current = pointer(root)
    if not current:
        raise ValueError('export requires an applied managed public snapshot')
    files, source_manifest = version_files(root, current['revision'])
    report = read_json(root / 'catalog/public-update-status.json')
    if report.get('state') != 'completed' or not report.get('diskCommitted') or not report.get('sessionApplied') or report.get('attempt') != current.get('attempt'):
        raise ValueError('public snapshot has not completed native validation/application')
    if any(name not in files for name in FILES):
        raise ValueError('public snapshot is missing an offline baseline component')
    payloads = {name: path.read_bytes() for name, path in files.items() if name in FILES}
    if any(sha(data) != source_manifest['files'][name]['sha256'] for name, data in payloads.items()) or pointer(root) != current:
        raise ValueError('public snapshot changed during export')
    verified = max((row.get('checkedUtc', '') for row in report.get('components', {}).values()), default='')
    if not verified:
        raise ValueError('public snapshot has no recorded verification time')
    manifest = {'schema': 1, 'sourceRevision': current['revision'], 'verifiedUtc': verified,
                'files': {name: {'sha256': sha(payloads[name]), 'bytes': len(payloads[name])} for name in FILES}}
    # All input files are verified before the first write; the manifest is the
    # last output, so a failed partial export cannot pass the release check.
    assets.mkdir(parents=True, exist_ok=True)
    for name, data in payloads.items():
        atomic_bytes(assets / name, data)
    atomic_bytes(assets / MANIFEST, canonical(manifest))
    return check(assets)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=Path(__file__).resolve().parents[1] / 'assets')
    parser.add_argument('--data-root', type=Path)
    args = parser.parse_args()
    value = export(args.data_root.resolve(), args.assets.resolve()) if args.data_root else check(args.assets.resolve())
    print(json.dumps({'sourceRevision': value['sourceRevision'], 'verifiedUtc': value['verifiedUtc'], 'files': len(value['files'])}))
