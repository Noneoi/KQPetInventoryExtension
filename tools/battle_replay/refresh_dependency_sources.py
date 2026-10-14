"""Refresh the exact official dependencies; preserve bytes and version evidence."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET

from fetch_reference import BASE, fetch, saved_flash_versions, save_binary_payloads


def refresh(latest: Path, output: Path, catalog_path: Path):
    catalog = json.loads(catalog_path.read_text(encoding='utf-8'))
    previous = {r['resource']: r for r in catalog['provenance']['versionChainRecheck']['packages']}
    versions = saved_flash_versions(latest)
    overrides = {r.get('n') for r in ET.parse(latest / 'start.xml').getroot().iter('f')}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'flash-resolved').mkdir(exist_ok=True)
    shutil.copyfile(latest / 'start.xml', output / 'play-start.xml')
    shutil.copyfile(latest / 'versiondata.swf', output / 'versiondata.swf')
    resources = list(dict.fromkeys([*previous,
        'multipleformpet/multipleformpetservice', 'pettitle/pettitle',
        'stargodv3/stargodservice', 'petbadge/petbadgeservice', 'astrolabe/astrolabeservice',
        'pet/petskillmesservice', 'changepetskinv2/changepetskinservice']))
    def retrieve(resource):
        version = versions.get(resource, '2000')
        url = BASE + resource + '~' + version + '.swf'
        relative = 'flash-resolved/' + resource.replace('/', '_') + '.swf'
        row = {'resource': resource, 'version': version, 'url': url, 'file': relative,
               'resolution': 'start-override' if resource in overrides else 'versiondata' if resource in versions else 'default-2000'}
        try:
            payload = fetch(url)
            (output / relative).write_bytes(payload)
            row.update(bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
            if resource in previous:
                row['matchesPreviouslyVerifiedBytes'] = row['sha256'] == previous[resource]['sha256']
            save_binary_payloads(payload, output / 'flash-resolved', resource, row)
        except Exception as error:
            row['error'] = str(error)
        return row
    with ThreadPoolExecutor(max_workers=4) as pool:
        records = list(pool.map(retrieve, resources))
    proof = {'retrievedUtc': datetime.now(timezone.utc).isoformat(),
             'manifestSha256': hashlib.sha256((latest / 'start.xml').read_bytes()).hexdigest(),
             'versiondataSha256': hashlib.sha256((latest / 'versiondata.swf').read_bytes()).hexdigest(),
             'files': records}
    (output / 'flash-resolved/provenance.json').write_text(json.dumps(proof, ensure_ascii=False, indent=2), encoding='utf-8')
    # Reuse decompiled text only when the package bytes are identical. No merge
    # of source snapshots is permitted merely because a version label matches.
    old_source = catalog_path.parent.parent / 'build-battle-reference/latest-tables'
    old_packages = {r['resource']: r for r in catalog['provenance']['packages']}
    new_packages = json.loads((latest / 'provenance.json').read_text(encoding='utf-8'))['packages']
    reused = []
    for row in new_packages:
        if row['sha256'] != old_packages[row['resource']]['sha256']:
            continue
        folder = {'library/interfaces': 'interfaces', 'battle/battle': 'battle', 'battleconfig/battleconfig': 'battleconfig'}[row['resource']]
        origin = old_source / folder
        if origin.is_dir():
            shutil.copytree(origin, latest / folder, dirs_exist_ok=True)
            reused.append({'folder': folder, 'package': row['resource'], 'packageSha256': row['sha256'],
                           'files': [{'file': p.relative_to(origin).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                                     for p in sorted(origin.rglob('*.as'))]})
    if (old_source / 'h5-scripts').is_dir():
        # H5 consumer hashes are independently retained; this does not claim
        # new H5 script bytes have been downloaded.
        shutil.copytree(old_source / 'h5-scripts', latest / 'h5-scripts', dirs_exist_ok=True)
    (latest / 'decompiled-reuse-evidence.json').write_text(json.dumps(reused, ensure_ascii=False, indent=2), encoding='utf-8')
    return proof


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('latest', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('catalog', type=Path)
    args = parser.parse_args()
    result = refresh(args.latest, args.output, args.catalog.resolve())
    print(json.dumps({'resources': len(result['files']), 'unchanged': sum(r.get('matchesPreviouslyVerifiedBytes') is True for r in result['files']),
                      'failures': [r for r in result['files'] if 'error' in r]}, ensure_ascii=False))
