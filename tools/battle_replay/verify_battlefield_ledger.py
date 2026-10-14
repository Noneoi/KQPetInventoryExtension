"""Independently check current forms, skills, entries and player formations.

Reads raw samples and official catalog, never C++ events. Covers every replay
step, including segment shifts/deletions, with an identity-preserving digest.
Group order comes from the official ordinal changeset rule. This verifier
explicitly rejects sequential skill profiles rather than silently guessing.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
from verify_state_ledger import csv, key


def scalar(v):
    try:
        return str(int(v)) if v is not None else ''
    except (ValueError, TypeError):
        return ''


def audit(raw, catalog):
    root = json.loads(raw)
    base = '/br' if 'br' in root else ''
    battle = root.get('br', root)
    state = {}
    counters = collections.Counter()

    def create(team, player, f=None):
        f = f or {}
        who = key(team, player['name'], f.get('id', '@team'))
        s = dict(team=team, player=player['name'], race=scalar(f.get('raceId')), face=scalar(f.get('faceId')),
                 normal=scalar(f.get('nskill')), unique=scalar(f.get('uskills')), extra=scalar(f.get('suskill')),
                 attrs=csv(f.get('attr')), attrsKnown=isinstance(f.get('attr'), str),
                 summoned=bool(f.get('ics')), clone=bool(f.get('ias')), cloneCalled=bool(f.get('ibs')),
                 entered=(not f.get('ics') or bool(f.get('is'))) and (not f.get('ias') or bool(f.get('ibs'))),
                 x=player.get('x', -1), y=player.get('y', -1), removed=False, buffs=csv(f.get('bfs')))
        if not f:
            s['buffs'] = ([str(player['ppbf']['pbid'])] if player.get('ppbf') else []) + csv(player.get('pcbfs'))
        s['power'] = catalog['pets'].get(s['race'], {}).get('maxPower', '?')
        state[who] = s

    for team in battle['or']['team']:
        for player in team['players']:
            create(team['id'], player)
            for f in player['cts']:
                create(team['id'], player, f)
    digest = hashlib.sha256()

    def stamp(path):
        digest.update((path + '\n').encode('utf-8'))
        for who in sorted(state, key=lambda k: k.encode('utf-16-be')):
            s = state[who]
            fields = [who, s['race'], s['face'], s['normal'], s['unique'], s['extra'], ','.join(s['attrs']),
                      int(s['attrsKnown']), s['power'], int(s['entered']), int(s['cloneCalled']),
                      s['x'], s['y'], int(s['removed'])]
            digest.update(('\t'.join(map(str, fields)) + '\n').encode('utf-8'))

    stamp(base + '/or')
    for bi, block in enumerate(battle['fr']):
        path = base + f'/fr/{bi}'
        for deletion in block.get('pr', {}).get('dl', []):
            for s in state.values():
                if s['team'] == deletion['teamId'] and s['player'] == deletion['player']:
                    s['removed'] = True
            counters['playerRemovals'] += 1
        for move in block.get('pr', {}).get('ml', []):
            for s in state.values():
                if s['team'] == move['teamId'] and s['player'] == move['player']:
                    s['x'], s['y'] = move['x_to'], move['y_to']
            counters['playerShifts'] += 1
        stamp(path)
        for ai, action in enumerate(block.get('rl', [])):
            representer = action.get('orrt', 1) != 1
            source = key(action['cti'], action['cpn'], '@team' if representer else action['cci'])
            records = []
            if not representer and 'cn' in action:
                records.append((source, action['cn'], False))
            for field in ('trl', 'prl'):
                for target in action.get(field, []):
                    if 'tn' in target:
                        records.append((key(target['tti'], target['tpn'], target.get('tci', '@team')),
                                        target['tn'], field == 'trl' and bool(target.get('isse'))))
            summon = False
            if not representer:
                skill = str(action['si'])
                for combo in catalog['combos']:
                    if combo['skillId'] == skill:
                        for bid, target in reversed(list(zip(csv(combo['buffs']), csv(combo['targetSkillIds'])))):
                            if bid in state[source]['buffs']:
                                skill = target
                                break
                        break
                cfg = catalog['skills'][skill]
                assert int(cfg.get('playOrderType', 0)) == 0, 'sequential profile needs a separate verifier'
                base_type = cfg.get('skillBaseType')
                type2 = base_type if base_type in ('召唤技', '超杀技', '超必杀技') else cfg.get('skillType')
                summon = cfg['attackType'] != '近身' and type2 == '召唤技' and int(catalog['execution'][str(action['ses'])]['type']) != 2

            def phase(before):
                timing, prefix, display = (1, 'bb_', 'bd_bfs') if before else (2, 'ab_', 'ad_bfs')
                groups = []
                for _, nums, _ in records:
                    ids = set()
                    for field in (prefix + 'hps', prefix + 'shps', prefix + 'vgs', display):
                        ids.update(int(t.split('_')[2]) for t in csv(nums.get(field)))
                    ids.update(u['changeset'] for u in nums.get('rl', []) if u['o'] == timing)
                    groups.append(sorted(ids))
                for ordinal in range(max(map(len, groups), default=0)):
                    for ri, (who, nums, _) in enumerate(records):
                        if ordinal >= len(groups[ri]):
                            continue
                        group = groups[ri][ordinal]
                        s = state[who]
                        for token in csv(nums.get(display)):
                            op, bid, g = token.split('_')
                            if int(g) != group:
                                continue
                            if op == '1':
                                s['buffs'].append(bid)
                                if int(catalog['buffs'][bid].get('buffWaitType', 0)) > 0 and (s['summoned'] or (s['clone'] and s['cloneCalled'])):
                                    if not s['entered']:
                                        counters['buffAppearances'] += 1
                                    s['entered'] = True
                            elif bid in s['buffs']:
                                s['buffs'].remove(bid)
                    for ri, (who, nums, _) in enumerate(records):
                        if ordinal >= len(groups[ri]):
                            continue
                        group = groups[ri][ordinal]
                        s = state[who]
                        for u in nums.get('rl', []):
                            if u['o'] != timing or u['changeset'] != group:
                                continue
                            kind = u['t']
                            if kind in (1, 6):
                                s['attrs'], s['attrsKnown'] = csv(u.get('ca')), isinstance(u.get('ca'), str)
                                if kind == 1:
                                    s['race'], s['face'], s['normal'] = scalar(u.get('cr')), scalar(u.get('cf')), scalar(u.get('cn'))
                                    first = int(u['cu'].split(',')[0])
                                    s['unique'] = scalar(u.get('scu')) if first == 0 else str(first)
                                    s['power'] = catalog['pets'][s['race']]['maxPower']
                                counters['transforms' if kind == 1 else 'attributeChanges'] += 1
                            elif kind == 5:
                                target = state[key(s['team'], s['player'], u['ci'])]
                                target['clone'] = target['cloneCalled'] = target['entered'] = True
                                counters['cloneCalls'] += 1
                            elif kind == 3:
                                counters['summonMetadata'] += 1
                            else:
                                assert kind in (2, 4, 100, 101), f'unsupported special record {kind}'

            phase(True)
            if summon:
                for who, _, effective in records:
                    if effective:
                        state[who]['entered'] = True
                        counters['summonSkillTargets'] += 1
            phase(False)
            stamp(path + f'/rl/{ai}')
    stamp(base + '/win')
    return digest.hexdigest(), counters


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('samples', type=Path)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('native_report', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding='utf-8'))
    native = json.loads(args.native_report.read_text(encoding='utf-8'))
    failures, totals = [], collections.Counter()
    for item in native['files']:
        raw = (args.samples / item['file']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == item['sha256']
        digest, counts = audit(raw, catalog)
        totals.update(counts)
        if digest != item['battlefieldLedgerSha256']:
            failures.append(item['file'])
    report = dict(files=len(native['files']), actions=native['actions'], counts=dict(totals), mismatches=failures,
                  unknownPowerReferences=sum(f['unknownPowerReferenceObservations'] for f in native['files']))
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))
    raise SystemExit(bool(failures))
