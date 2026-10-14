"""Describe raw 300113 co-occurrence controls, without naming it or inferring cause.

Counts are per target/number record, not damage segment, turn, independent
experiment or Buff layer. Initial species identity is explicitly labeled.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

GROUP = {'300111', '300112', '300114', '300119', '300120'}
ATTACK_FIELDS = {'att_hps', 'att_shps', 'att_vgs'}


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transitions(nums, field):
    result = []
    for token in nums.get(field, '').split(','):
        if not token:
            continue
        op, bid, group = token.split('_')
        if op not in ('0', '1'):
            raise ValueError('Unknown operation: ' + token)
        result.append({'operation': int(op), 'buffId': bid, 'changeset': int(group), 'raw': token})
    return result


def scan(battle, source, catalog):
    units = {}
    for ti, team in enumerate(battle['or']['team']):
        for pi, player in enumerate(team['players']):
            for fi, fighter in enumerate(player.get('cts', [])):
                race = str(fighter.get('raceId', ''))
                identity = catalog['pets'].get(race, {}).get('identity', {})
                units[(team['id'], player['name'], fighter['id'])] = {
                    'initialRaceId': race, 'initialBaseRaceId': identity.get('baseRaceId', ''),
                    'initialCanonicalName': identity.get('canonicalName', ''),
                    'initialBuffCounts': dict(Counter(fighter.get('bfs', '').split(','))),
                    'initialPath': f'/br/or/team/{ti}/players/{pi}/cts/{fi}',
                    'initialRaw': fighter,
                }
    counts, flags, co_remove, races, contexts = Counter(), Counter(), Counter(), Counter(), Counter()
    observations = []
    for bi, block in enumerate(battle['fr']):
        for ai, action in enumerate(block.get('rl', [])):
            counts['actions'] += 1
            records = []
            if 'cn' in action:
                records.append(('cn', (action.get('cti'), action.get('cpn'), action.get('cci')), action['cn'], {}))
            for field in ('trl', 'prl'):
                for ri, target in enumerate(action.get(field, [])):
                    records.append((f'{field}/{ri}/tn', (target.get('tti'), target.get('tpn'), target.get('tci')), target.get('tn', {}), target))
            for suffix, key, nums, target in records:
                before, after = transitions(nums, 'bd_bfs'), transitions(nums, 'ad_bfs')
                ops = before + after
                added = sum(op['buffId'] == '300113' and op['operation'] == 1 for op in ops)
                removed = sum(op['buffId'] == '300113' and op['operation'] == 0 for op in ops)
                unit = units.get(key, {})
                initial_group = bool(GROUP.intersection(unit.get('initialBuffCounts', {})))
                nafe = unit.get('initialBaseRaceId') in ('5990', '7403')
                has_attack_fields = bool(ATTACK_FIELDS.intersection(nums))
                if not (added or removed or initial_group or nafe):
                    continue
                normal = action.get('orrt', 1) == 1
                flag_tuple = (normal, has_attack_fields, target.get('isse'), target.get('ishit'), target.get('isbl'), bool(added))
                if initial_group:
                    flags[json.dumps(flag_tuple)] += 1
                counts['focusedNumberRecords'] += 1
                if added:
                    counts['positiveRecords'] += 1
                    counts['addTuples'] += added
                    counts['removeTuplesInPositiveRecords'] += removed
                    counts['positiveWithBlockTrue' if target.get('isbl') is True else 'positiveWithoutBlockTrue'] += 1
                    counts['positiveWithHitTrue' if target.get('ishit') is True else 'positiveWithoutHitTrue'] += 1
                    co_remove[','.join(op['buffId'] for op in ops if op['operation'] == 0 and op['buffId'] in GROUP)] += 1
                    races[unit.get('initialRaceId', 'unmatched')] += 1
                    contexts[str(action.get('si', 'none'))] += 1
                if added or removed or has_attack_fields:
                    observations.append({
                        'source': source, 'path': f'/br/fr/{bi}/rl/{ai}/{suffix}',
                        'actionNumberInSegment': ai + 1,
                        'actionHeader': {k: action[k] for k in ('cti', 'cpn', 'cci', 'si', 'ses', 'csn', 'orrt', 'epbi') if k in action},
                        'recipient': unit, 'targetKey': list(key),
                        'flags': {k: target[k] for k in ('isse', 'ishit', 'isbl', 'iscr', 'issb') if k in target},
                        'hasAttackFields': has_attack_fields, 'has300113Add': bool(added),
                        'beforeTransitions': before, 'afterTransitions': after, 'rawNumbers': nums,
                    })
    return {'counts': counts, 'initialGroupFlagMatrix': flags, 'coRemovedGroupIds': co_remove,
            'positiveInitialRaceCounts': races, 'positiveSkillContexts': contexts}, observations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--samples', action='append', default=[], help='label=directory')
    parser.add_argument('--demos', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    catalog = read(args.catalog)
    cohorts = {}
    for spec in args.samples:
        label, directory = spec.split('=', 1)
        records = []
        for path in sorted(Path(directory).iterdir()):
            if not path.is_file() or path.suffix.lower() not in ('.txt', '.json'):
                continue
            doc = read(path)
            if not isinstance(doc, dict) or 'br' not in doc:
                raise ValueError('Non-battle in sample directory: ' + str(path))
            records.append((doc['br'], {'file': str(path.resolve()), 'sha256': sha(path), 'jsonPointer': '/br'}))
        cohorts[label] = records
    if args.demos:
        source = {'file': str(args.demos.resolve()), 'sha256': sha(args.demos)}
        cohorts['officialSkinDemo'] = [(row['br'], {**source, 'jsonPointer': '/' + str(identifier) + '/br'})
                                       for identifier, row in read(args.demos).items() if isinstance(row, dict) and 'br' in row]
    summaries, observations, inventory = {}, [], []
    for label, records in cohorts.items():
        total = {}
        for battle, source in records:
            result, detail = scan(battle, source, catalog)
            for key, values in result.items():
                total.setdefault(key, Counter()).update(values)
            observations.extend({'cohort': label, **row} for row in detail)
            inventory.append({'cohort': label, **source})
        summaries[label] = {'battleRecords': len(records), **total}
    report = {
        'buffId': '300113', 'catalogSha256': sha(args.catalog), 'cohorts': summaries,
        'flagMatrixColumns': ['normalAction', 'hasAttackFields', 'isse', 'ishit', 'isbl', 'has300113Add'],
        'scope': 'Raw-record co-occurrence. Skill context is not Buff source. Initial group/race is not current state. Repeated samples are not independent experiments.',
        'observationsFile': 'observations.json', 'sourceInventoryFile': 'sources.json',
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for filename, value in [('summary.json', report), ('observations.json', observations), ('sources.json', inventory)]:
        (args.output / filename).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
