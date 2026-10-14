"""Check official lookup coverage of explicit skill/Buff IDs in raw logs.

This scans documented reference fields, not names or ID similarity. A reference
appearing in a record does not prove that its associated effect was executed.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path


def audit(samples, catalog_path):
    catalog = json.loads(catalog_path.read_text(encoding='utf-8-sig'))
    references = {'skills': defaultdict(lambda: {'occurrences': 0, 'roles': Counter(), 'examples': []}),
                  'buffs': defaultdict(lambda: {'occurrences': 0, 'roles': Counter(), 'examples': []})}
    invalid, files, markers = [], [], Counter()

    def add(kind, value, file, path, role='battle_record'):
        raw = str(value).strip()
        try:
            identifier = int(raw)
        except ValueError:
            invalid.append({'file': file, 'path': path, 'raw': value})
            return
        # -2 is an actual official Buff (maximum HP change), not a sentinel.
        if identifier in (0, -1) and str(identifier) not in catalog[kind]:
            markers[f'{kind}:{identifier}'] += 1
            return
        row = references[kind][str(identifier)]
        row['occurrences'] += 1
        row['roles'][role] += 1
        if len(row['examples']) < 3:
            row['examples'].append({'file': file, 'path': path, 'raw': value, 'role': role})

    def tokens(kind, value, file, path, role='battle_record'):
        if value is None or value == '':
            return
        for item in str(value).split(','):
            if item.strip(): add(kind, item, file, path, role)

    def numbers(value, file, path):
        for field in ('ba_bfs', 'bc_bfs', 'aa_bfs', 'ac_bfs'):
            tokens('buffs', value.get(field), file, path + '/' + field, 'legacy_unconsumed_field')
        for field in ('bb_hps', 'bb_shps', 'bb_vgs', 'ab_hps', 'ab_shps', 'ab_vgs', 'bd_bfs', 'ad_bfs'):
            if not value.get(field): continue
            for entry in str(value[field]).split(','):
                parts = entry.split('_')
                if len(parts) != 3:
                    invalid.append({'file': file, 'path': path + '/' + field, 'raw': entry})
                    continue
                add('buffs', parts[1 if field in ('bd_bfs', 'ad_bfs') else 0], file, path + '/' + field)
        for i, change in enumerate(value.get('rl', [])):
            here = f'{path}/rl/{i}'
            if change.get('t') == 4 and 'pbid' in change:
                add('buffs', change['pbid'], file, here + '/pbid')
            elif change.get('t') == 1:
                for field in ('cn', 'cu', 'scu'):
                    if field in change: tokens('skills', change[field], file, here + '/' + field)

    for path in sorted(samples.iterdir()):
        if path.suffix.lower() not in ('.txt', '.json') or not path.is_file(): continue
        data = path.read_bytes(); root = json.loads(data.decode('utf-8-sig'))
        battle = root.get('br', root)
        base = '/br' if 'br' in root else ''
        files.append({'file': path.name, 'sha256': hashlib.sha256(data).hexdigest()})
        # BattleResult.buffersNeeded is a resource list, not active membership.
        tokens('buffs', battle.get('abs'), path.name, base + '/abs', 'resource_preload')
        for t, team in enumerate(battle['or']['team']):
            for p, player in enumerate(team['players']):
                here = f'{base}/or/team/{t}/players/{p}'
                if 'hs' in player: add('skills', player['hs'], path.name, here + '/hs', 'hero_skill')
                for d, dragon in enumerate(player.get('drs', [])):
                    if 'dbfi' in dragon:
                        add('buffs', dragon['dbfi'], path.name, f'{here}/drs/{d}/dbfi', 'dragon_configuration')
                tokens('buffs', player.get('pcbfs'), path.name, here + '/pcbfs')
                if 'pbid' in player.get('ppbf', {}): add('buffs', player['ppbf']['pbid'], path.name, here + '/ppbf/pbid')
                for c, fighter in enumerate(player['cts']):
                    there = f'{here}/cts/{c}'
                    tokens('buffs', fighter.get('bfs'), path.name, there + '/bfs')
                    for field in ('nskill', 'uskills', 'suskill'):
                        if field in fighter: tokens('skills', fighter[field], path.name, there + '/' + field)
        for i, item in enumerate(battle.get('ir', {}).get('buffrs', [])):
            for t, target in enumerate(item.get('c', {}).get('tl', [])):
                tokens('buffs', target.get('bs'), path.name, f'{base}/ir/buffrs/{i}/c/tl/{t}/bs', 'item_result')
        for f, flow in enumerate(battle['fr']):
            for a, action in enumerate(flow.get('rl', [])):
                here = f'{base}/fr/{f}/rl/{a}'
                for field, kind in (('si', 'skills'), ('epbi', 'buffs')):
                    if field in action: add(kind, action[field], path.name, here + '/' + field)
                if isinstance(action.get('cn'), dict): numbers(action['cn'], path.name, here + '/cn')
                for group in ('trl', 'prl'):
                    for i, target in enumerate(action.get(group, [])):
                        if isinstance(target.get('tn'), dict): numbers(target['tn'], path.name, f'{here}/{group}/{i}/tn')

    result = {'files': files, 'catalogSha256': hashlib.sha256(catalog_path.read_bytes()).hexdigest(),
              'scope': 'Explicit raw reference fields, including initial state and special records; not a causality inference.',
              'nonPositiveMarkers': dict(markers), 'invalidReferences': invalid,
              'roles': {'battle_record': 'Explicit ID in an initial-state or action record; not proof of execution.',
                        'hero_skill': 'PlayerInfo.hs, the configured hero skill.',
                        'resource_preload': 'BattleResult.abs / buffersNeeded, not current Buff membership.',
                        'legacy_unconsumed_field': 'Legacy Buff list retained by SkillNumResult, not applied by current commands.',
                        'dragon_configuration': 'FightPetDragonInfo.dbfi, the configured execution Buff.',
                        'item_result': 'BufferItemTargetResult.bs, reported item-result Buff IDs.'},
              'excludedNamespaces': ['Player ea.1uw.wi belongs to leagueService arsenal Buffs, not battle Buffs.']}
    for kind, rows in references.items():
        counts = Counter(); entries = {}
        for identifier, row in sorted(rows.items(), key=lambda item: int(item[0])):
            entry = catalog[kind].get(identifier)
            status = 'missing_definition' if entry is None else 'named' if entry.get('name' if kind == 'skills' else 'nm') else 'officially_unnamed'
            counts[status] += 1
            has_text = bool(entry and entry.get('description' if kind == 'skills' else 'mess'))
            counts['with_description' if has_text else 'without_description'] += 1
            entries[identifier] = {**row, 'status': status, 'hasDescription': has_text,
                                   'nameSource': (entry.get('textSource') if kind == 'skills' else entry.get('nameSource')) if entry else None,
                                   'descriptionSource': (entry.get('textSource') if kind == 'skills' else entry.get('descriptionSource')) if entry else None}
        result[kind] = {'uniqueIds': len(rows), 'counts': dict(counts), 'entries': entries}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path); parser.add_argument('catalog', type=Path); parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = audit(args.samples, args.catalog)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False), encoding='utf-8')
    print(json.dumps({'files': len(result['files']), 'skills': result['skills']['counts'], 'buffs': result['buffs']['counts'],
                      'invalidReferences': len(result['invalidReferences'])}, ensure_ascii=False))
