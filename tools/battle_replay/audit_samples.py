"""Inventory original battle logs without importing any replay implementation."""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path


def inspect(folder: Path) -> dict:
    schema = {}
    files, errors = [], []
    counts = collections.Counter()
    features = set()
    node_count = 0

    def visit(value, path, filename):
        nonlocal node_count
        node_count += 1
        typename = ('null' if value is None else 'boolean' if isinstance(value, bool)
                    else 'object' if isinstance(value, dict) else 'array' if isinstance(value, list)
                    else 'number' if isinstance(value, (int, float)) else 'string')
        item = schema.setdefault(path or '/', {'types': set(), 'count': 0, 'exampleFile': filename})
        item['types'].add(typename)
        item['count'] += 1
        if isinstance(value, dict):
            for key, child in value.items():
                visit(child, path + '/' + key.replace('~', '~0').replace('/', '~1'), filename)
        elif isinstance(value, list):
            for child in value:
                visit(child, path + '/*', filename)

    for file in sorted(folder.glob('*')):
        if not file.is_file() or file.suffix.lower() not in ('.txt', '.json'):
            continue
        try:
            data = file.read_bytes()
            root = json.loads(data.decode('utf-8-sig'))
            br = root.get('br', root)
            if not isinstance(br.get('or'), dict) or not isinstance(br.get('fr'), list):
                raise ValueError('missing battle roster or flow')
            before_nodes = node_count
            visit(root, '', file.name)
            own_features = set()
            rounds = 0
            for frame in br['fr']:
                for action in frame.get('rl', []):
                    rounds += 1
                    counts['actions'] += 1
                    own_features.add('action:' + str(action.get('orrt', 1)))
                    if len(action.get('trl', [])) > 1:
                        own_features.add('multiple_targets')
                    if action.get('ses', 1) != 1:
                        own_features.add('execution:' + str(action['ses']))
                    for owner, num in ([('cn', action['cn'])] if 'cn' in action else []) + [
                        (group, target.get('tn', {}))
                        for group in ('trl', 'prl') for target in action.get(group, [])]:
                        counts[owner] += 1
                        for key in num:
                            own_features.add('number:' + key)
                        for unit in num.get('rl', []):
                            counts['unit:' + str(unit.get('t'))] += 1
                            own_features.add('unit:' + str(unit.get('t')))
                    for target in action.get('trl', []):
                        for flag in ('issb', 'isIm', 'iscr', 'isbl'):
                            if target.get(flag) is True:
                                counts[flag] += 1
                                own_features.add(flag)
            own_features.add('winner:' + str(br.get('win')))
            for team in br['or'].get('team', []):
                for player in team.get('players', []):
                    positions = collections.Counter()
                    for fighter in player.get('cts', []):
                        counts['fighters'] += 1
                        positions[(fighter.get('x'), fighter.get('y'))] += 1
                        for flag in ('ics', 'ias', 'ibs', 'icr'):
                            if fighter.get(flag) is True:
                                own_features.add(flag)
                    if any(count > 1 for count in positions.values()):
                        own_features.add('shared_cell')
            features |= own_features
            files.append({'file': file.name, 'sha256': hashlib.sha256(data).hexdigest(),
                          'bytes': len(data), 'nodes': node_count - before_nodes,
                          'actions': rounds, 'features': sorted(own_features)})
        except (ValueError, OSError, KeyError, TypeError) as error:
            errors.append({'file': file.name, 'error': str(error)})
    uncovered = set(features)
    representatives = []
    while uncovered:
        candidate = max(files, key=lambda f: (len(set(f['features']) & uncovered), -f['bytes']))
        found = set(candidate['features']) & uncovered
        if not found:
            break
        representatives.append({'file': candidate['file'], 'adds': sorted(found)})
        uncovered -= found
    return {'schemaVersion': 1, 'sourceDirectory': str(folder), 'files': files,
            'counts': dict(counts), 'errors': errors, 'representatives': representatives,
            'schema': {path: dict(item, types=sorted(item['types'])) for path, item in sorted(schema.items())}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = inspect(args.folder)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'files': len(result['files']), 'schemaPaths': len(result['schema']),
                      'counts': result['counts'], 'errors': result['errors'],
                      'representatives': result['representatives']}, ensure_ascii=False, indent=2))
