"""Independent sample audit of shield conservation and Buff membership.

Uses raw logs + official XML attributes. No C++ intermediate events are read.
The digest covers EVERY action, identity, shield and Buff multiplicity, not just
the last battle state. It intentionally does not claim to verify animation time.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path


def key(team, player, fid):
    return f'{team}:{len(player.encode("utf-16-le")) // 2}:{player}:{fid}'


def csv(value):
    return [s for s in (value or '').split(',') if s]


def audit(raw, catalog):
    doc = json.loads(raw)
    base = '/br' if 'br' in doc else ''
    battle = doc.get('br', doc)
    state = {}
    for team in battle['or']['team']:
        for player in team['players']:
            buffs = csv(player.get('pcbfs'))
            if player.get('ppbf') is not None:
                buffs.insert(0, str(player['ppbf']['pbid']))
            state[key(team['id'], player['name'], '@team')] = [None, buffs]
            for f in player['cts']:
                state[key(team['id'], player['name'], f['id'])] = [f.get('shp'), csv(f.get('bfs'))]

    digest = hashlib.sha256()
    counters = collections.Counter()
    for bi, block in enumerate(battle['fr']):
        for ai, action in enumerate(block['rl']):
            representer = action.get('orrt', 1) != 1
            source = key(action['cti'], action['cpn'], '@team' if representer else action['cci'])
            records = []
            if 'cn' in action:
                records.append((source, action['cn'], 'source', {}))
            for field in ('trl', 'prl'):
                for target in action.get(field, []):
                    if 'tn' in target:
                        records.append((key(target['tti'], target['tpn'], target.get('tci', '@team')), target['tn'], field, target))
            assert all(k in state for k, *_ in records), 'new identity requires explicit audit support'
            if representer:
                effects = int(catalog['bufferExecution'].get(str(action['epbi']), {}).get('actionResId', 0)) > 0
                close = False
            else:
                skill = str(action['si'])
                for combo in catalog['combos']:
                    if combo['skillId'] == skill:
                        for buff, target in reversed(list(zip(csv(combo['buffs']), csv(combo['targetSkillIds'])))):
                            if buff in state[source][1]:
                                skill = target
                                break
                        break
                cfg = catalog['skills'][skill]
                assert int(cfg.get('playOrderType', 0)) == 0, 'this independent sample ledger covers ordinary group scheduling'
                close = cfg['attackType'] == '近身'
                base_type = cfg.get('skillBaseType')
                type2 = base_type if base_type in ('召唤技', '超杀技', '超必杀技') else cfg.get('skillType')
                effects = int(catalog['execution'][str(action['ses'])]['type']) != 2 and (close or type2 != '召唤技')

            def phase(prefix, display, timing):
                membership = []
                for ri, (identity, nums, role, _) in enumerate(records):
                    # Shield deltas commute without clamping; independent sum
                    # also detects duplicated or omitted execution in C++.
                    if role == 'source' or identity != source:
                        for token in csv(nums.get(prefix + '_shps')):
                            _, amount, _ = token.split('_')
                            if state[identity][0] is not None:
                                state[identity][0] += int(amount)
                            counters['buffShieldEntries'] += 1
                    group_ids = set()
                    for field in (prefix + '_hps', prefix + '_shps', prefix + '_vgs', display):
                        group_ids.update(int(t.split('_')[2]) for t in csv(nums.get(field)))
                    group_ids.update(u['changeset'] for u in nums.get('rl', []) if u['o'] == timing)
                    ordinal = {g: i for i, g in enumerate(sorted(group_ids))}
                    for ti, token in enumerate(csv(nums.get(display))):
                        op, bid, group = token.split('_')
                        membership.append((ordinal[int(group)], ri, ti, identity, op, bid))
                for _, _, _, identity, op, bid in sorted(membership):
                    if op == '1':
                        state[identity][1].append(bid)
                    elif op == '0':
                        if bid in state[identity][1]:
                            state[identity][1].remove(bid)
                    else:
                        raise ValueError(f'unknown Buff operation {op}')
                    counters['buffTransitions'] += 1

            def attack(record, field):
                identity, nums, _, _ = record
                changes = [int(t) for t in csv(nums.get(field))]
                if state[identity][0] is not None:
                    state[identity][0] += sum(changes)
                counters[field] += len(changes)

            phase('bb', 'bd_bfs', 1)
            effective = [r for r in records if r[2] == 'trl' and r[3].get('isse')]
            if effects and effective:
                for record in records:
                    if record[2] == 'source':
                        attack(record, 'att_shps')
                for record in effective:
                    attack(record, 'att_shps')
                if close and len(effective) == 1 and effective[0][3].get('issb'):
                    attack(effective[0], 'sb_shps')
                    for record in records:
                        if record[2] == 'source':
                            attack(record, 'sb_shps')
                    counters['counters'] += 1
            phase('ab', 'ad_bfs', 2)
            digest.update(f'{base}/fr/{bi}/rl/{ai}\n'.encode())
            for identity, (shield, buffs) in sorted(state.items()):
                balance = '?' if shield is None else str(shield)
                digest.update(f'{identity}\t{balance}\t{",".join(sorted(buffs))}\t1\n'.encode())
            counters['actions'] += 1
    return digest.hexdigest(), counters


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('samples', type=Path)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('cpp_report', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding='utf-8'))
    cpp = json.loads(args.cpp_report.read_text(encoding='utf-8'))
    errors = []; totals = collections.Counter()
    for entry in cpp['files']:
        raw = (args.samples / entry['file']).read_bytes()
        expected, counts = audit(raw, catalog)
        totals.update(counts)
        if expected != entry.get('stateLedgerSha256') or hashlib.sha256(raw).hexdigest() != entry['sha256']:
            errors.append(entry['file'])
    result = dict(files=len(cpp['files']), counts=dict(totals), mismatches=errors,
                  scope='Every action: raw file hash, shield balance and current Buff multiplicity for every identity.')
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
