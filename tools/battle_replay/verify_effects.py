"""Independently check readable effect relations against the raw battle logs.

This deliberately does not duplicate the C++ snapshot scheduler. It verifies
the complete multiset of target, value, Buff, progress and form records for
every action, including their source, recipient, original JSON and log delta.
The native audit checks moment indices and numeric/Buff snapshot continuity.
Calibration and actual appearance depend on that scheduler and are excluded
from this independent digest; the original summon records are included.

Native effectsSha256 protocol: for each action with moments, add path + LF,
then the bytewise-sorted compact UTF-8 JSON arrays below, each followed by LF:
  [kind, cause, source, target, causeId, metric, path, raw, delta-or-null]
Include all value/target/team-target/buff/progress/transform/attributes effects and
observation effects whose cause is record. Include delta only for progress
and for value effects whose cause is not record. JSON object keys are sorted.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path


METRICS = {'hps': '\u751f\u547d', 'shps': '\u62a4\u76fe', 'vgs': '\u6c14\u52bf'}
LIMIT = 9007199254740991


def key(team, player, fighter):
    return f'{team}:{len(player.encode("utf-16-le")) // 2}:{player}:{fighter}'


def number(value):
    if isinstance(value, bool):
        raise ValueError('Boolean is not an integer observation')
    parsed = int(value)
    if isinstance(value, float) and parsed != value:
        raise ValueError('fractional observation')
    if not -LIMIT <= parsed <= LIMIT:
        raise ValueError('observation exceeds exact JSON integer range')
    return parsed


def csv(value):
    if value is None or value == '':
        return []
    if not isinstance(value, str):
        raise ValueError('expected string-encoded numeric/Buff entries')
    return [v for v in value.split(',') if v]


def skill_number(value):
    try:
        result = number(value)
        return str(result) if result > 0 else ''
    except (TypeError, ValueError):
        return ''


def projection_value(value):
    # QJsonDocument writes an integral JSON double as an integer (0.0 -> 0).
    # Normalize only the comparison projection; original bytes/hash stay exact.
    if isinstance(value, float) and value.is_integer() and abs(value) <= LIMIT:
        return int(value)
    if isinstance(value, list):
        return [projection_value(item) for item in value]
    if isinstance(value, dict):
        return {key: projection_value(item) for key, item in value.items()}
    return value


def audit(raw, catalog):
    doc = json.loads(raw)
    base = '/br' if 'br' in doc else ''
    battle = doc.get('br', doc)
    state = {}
    normal_skills = {}
    player_info = {}
    progress_state = {}
    progress_caps = {}
    for team in battle['or']['team']:
        for player in team['players']:
            buffs = csv(player.get('pcbfs'))
            if player.get('ppbf') and 'pbid' in player['ppbf']:
                buffs.insert(0, str(player['ppbf']['pbid']))
                if player['ppbf'].get('cp') is not None:
                    progress_key = (key(team['id'], player['name'], '@team'), str(player['ppbf']['pbid']))
                    progress_state[progress_key] = number(player['ppbf']['cp'])
                    if player['ppbf'].get('mp') is not None:
                        progress_caps[progress_key] = number(player['ppbf']['mp'])
            state[key(team['id'], player['name'], '@team')] = buffs
            player_info[key(team['id'], player['name'], '@team')] = player
            for fighter in player['cts']:
                identity = key(team['id'], player['name'], fighter['id'])
                state[identity] = csv(fighter.get('bfs'))
                normal_skills[identity] = skill_number(fighter.get('nskill'))

    digest = hashlib.sha256()
    counts = collections.Counter()
    progress = collections.Counter()
    for bi, block in enumerate(battle['fr']):
        for ai, action in enumerate(block.get('rl', [])):
            action_path = f'{base}/fr/{bi}/rl/{ai}'
            team_action = action.get('orrt') is not None and action['orrt'] != 1
            actor = key(action['cti'], action['cpn'], '@team' if team_action else action['cci'])
            skill_id = str(action['epbi'] if team_action else action['si'])
            rows = []

            def emit(kind, cause, source, target, cause_id, metric, path, original, delta=None):
                rows.append(json.dumps(projection_value([kind, cause, source, target, cause_id, metric, path, original, delta]),
                                       ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8'))
                counts['projectedEffects'] += 1
                counts['kind:' + kind] += 1
                counts['cause:' + cause] += 1
                if not source:
                    counts['unknownApplierEffects'] += 1

            records = []
            targets = []
            if not team_action and isinstance(action.get('cn'), dict):
                records.append((actor, action['cn'], 'source', {}, action_path + '/cn', action['cti'], action['cpn']))
            for field in ('trl', 'prl'):
                for ti, target in enumerate(action.get(field, [])):
                    identity = key(target['tti'], target['tpn'], '@team' if target.get('tci') is None else target['tci'])
                    path = f'{action_path}/{field}/{ti}'
                    if field == 'trl':
                        targets.append((identity, path, target))
                    else:
                        emit('team-target', 'record', '', identity, '', '', path, target)
                    if isinstance(target.get('tn'), dict):
                        records.append((identity, target['tn'], field, target, path + '/tn', target['tti'], target['tpn']))

            # Only official definitions determine the direct-effect route.
            if team_action:
                config = catalog['bufferExecution'].get(skill_id, {})
                effects = int(config.get('actionResId', 0)) > 0
                playback_skill = ''
                if effects:
                    field = 'skillId'
                    dragons = action.get('eps')
                    confirmed = dragons is None or isinstance(dragons, list)
                    if isinstance(dragons, list) and dragons:
                        confirmed = False
                        for dragon in player_info.get(actor, {}).get('drs', []):
                            if dragon.get('di') != dragons[0]:
                                continue
                            gene = dragon.get('ge')
                            if gene is not None and not isinstance(gene, str):
                                break
                            field = 'lightedSkillId' if (gene or '').startswith('aa') else 'skillId'
                            confirmed = True
                            break
                    if confirmed:
                        playback_skill = skill_number(config.get(field))
                close = False
            else:
                playback_skill = skill_id
                for combo in catalog.get('combos', []):
                    if combo['skillId'] == playback_skill:
                        for buff, replacement in reversed(list(zip(csv(combo['buffs']), csv(combo['targetSkillIds'])))):
                            if buff in state.get(actor, []):
                                playback_skill = skill_number(replacement) or playback_skill
                                break
                        break
                config = catalog['skills'][playback_skill]
                if int(config.get('playOrderType', 0)) != 0:
                    raise ValueError('independent sample audit requires ordinary ordinal-group scheduling')
                close = config.get('attackType') == '\u8fd1\u8eab'
                base_type = config.get('skillBaseType')
                skill_type = base_type if base_type in ('\u53ec\u5524\u6280', '\u8d85\u6740\u6280', '\u8d85\u5fc5\u6740\u6280') else config.get('skillType')
                effects = int(catalog['execution'][str(action['ses'])]['type']) != 2 and (close or skill_type != '\u53ec\u5524\u6280')
            for identity, path, target in targets:
                emit('target', 'target', actor, identity, playback_skill or skill_id, '', path, target)

            def phase(prefix, display, timing):
                memberships = []
                progress_updates = []
                form_updates = []
                for ri, (identity, nums, role, _, path, host_team, host_player) in enumerate(records):
                    group_ids = set()
                    for field in (prefix + '_hps', prefix + '_shps', prefix + '_vgs', display):
                        for token in csv(nums.get(field)):
                            parts = token.split('_')
                            if len(parts) != 3:
                                raise ValueError('malformed group entry')
                            first, second, group = map(number, parts)
                            group_ids.add(group)
                            if field == display:
                                if first not in (0, 1):
                                    raise ValueError('unknown Buff membership operation')
                                emit('buff', 'buff', '', identity, str(second), '\u0042uff \u8bb0\u5f55\u6570', path + '/' + field, token)
                            elif not identity.endswith(':@team') and (field.endswith('_vgs') or role == 'source' or identity != actor):
                                emit('value', 'buff', '', identity, str(first), METRICS[field.split('_', 1)[1]], path + '/' + field, token, second)
                    for ui, unit in enumerate(nums.get('rl', [])):
                        if unit.get('o') != timing:
                            continue
                        group_ids.add(number(unit['changeset']))
                        p = f'{path}/rl/{ui}'
                        kind = number(unit['t'])
                        if kind == 1:
                            emit('transform', 'record', '', identity, '', '', p, unit)
                        elif kind == 2:
                            emit('value', 'record', '', identity, '', METRICS['hps'], p, unit)
                            emit('value', 'record', '', identity, '', '\u751f\u547d\u4e0a\u9650', p, unit)
                        elif kind == 3:
                            summoned = key(unit['ti'], unit['pn'], unit['ci'])
                            emit('observation', 'record', '', summoned, '', '', p, unit)
                        elif kind == 4:
                            target = key(host_team, host_player, '@team')
                            emit('progress', 'record', '', target, str(unit['pbid']), '\u961f\u4f0d\u8fdb\u5ea6', p, unit,
                                 number(unit['vc']) if unit.get('vc') is not None else None)
                            progress['records'] += 1
                            progress['timing:' + str(timing)] += 1
                            progress['max:' + str(unit.get('mp'))] += 1
                            if all(unit.get(k) is not None for k in ('prob', 'proa', 'vc')):
                                if number(unit['proa']) - number(unit['prob']) != number(unit['vc']):
                                    progress['reportedDeltaDiffersFromEndpoints'] += 1
                        elif kind == 6:
                            emit('attributes', 'record', '', identity, '', '', p, unit)
                        elif kind not in (5, 100, 101):
                            raise ValueError(f'unverified special unit type {kind}')
                    ordinal = {g: index for index, g in enumerate(sorted(group_ids))}
                    for ui, unit in enumerate(nums.get('rl', [])):
                        if unit.get('o') == timing and unit.get('t') == 1:
                            form_updates.append((ordinal[number(unit['changeset'])], ri, ui, identity, unit))
                        if unit.get('o') == timing and unit.get('t') == 4:
                            progress_updates.append((ordinal[number(unit['changeset'])], ri, ui,
                                                     key(host_team, host_player, '@team'), unit))
                    for ti, token in enumerate(csv(nums.get(display))):
                        operation, bid, group = map(number, token.split('_'))
                        memberships.append((ordinal[group], ri, ti, identity, operation, str(bid)))
                # Buff operations affect playback-skill choice in later actions.
                for _, _, _, identity, operation, bid in sorted(memberships):
                    buffs = state.setdefault(identity, [])
                    if operation == 1:
                        buffs.append(bid)
                    elif bid in buffs:
                        buffs.remove(bid)
                for _, _, _, identity, unit in sorted(form_updates, key=lambda item: item[:3]):
                    normal_skills[identity] = skill_number(unit.get('cn'))
                group_targets = collections.Counter()
                for group_index, _, _, target, unit in sorted(progress_updates, key=lambda item: item[:3]):
                    progress_key = (target, str(unit['pbid']))
                    group_targets[(group_index, *progress_key)] += 1
                    before, after = number(unit['prob']), number(unit['proa'])
                    if progress_key in progress_state and progress_state[progress_key] != before:
                        progress['priorObservedValueDiffersFromReportedBefore'] += 1
                        if progress_state[progress_key] == progress_caps.get(progress_key) and before > progress_state[progress_key]:
                            progress['fullDisplayThenOverflowRecord'] += 1
                            if after == 0:
                                progress['fullDisplayThenOverflowReset'] += 1
                        else:
                            progress['otherEndpointDiscontinuities'] += 1
                    progress_state[progress_key] = after
                    progress_caps[progress_key] = number(unit['mp'])
                progress['sameCommandGroupMultipleUpdates'] += sum(v - 1 for v in group_targets.values() if v > 1)

            def attack(record, prefix, per_token, source):
                identity, nums, _, _, path, *_ = record
                if identity.endswith(':@team'):
                    return
                cause = 'counter' if prefix == 'sb' else 'team' if team_action else 'skill'
                cause_id = normal_skills.get(source, '') if prefix == 'sb' else playback_skill or skill_id
                for suffix, metric in METRICS.items():
                    field = prefix + '_' + suffix
                    values = csv(nums.get(field))
                    if not values:
                        continue
                    if per_token:
                        for token in values:
                            emit('value', cause, source, identity, cause_id, metric, path + '/' + field, token, number(token))
                    else:
                        emit('value', cause, source, identity, cause_id, metric, path + '/' + field, nums[field], sum(map(number, values)))

            phase('bb', 'bd_bfs', 1)
            effective = [r for r in records if r[2] == 'trl' and r[3].get('isse') is True]
            if effects and effective:
                for record in records:
                    if record[2] == 'source':
                        attack(record, 'att', True, actor)
                for record in effective:
                    attack(record, 'att', False, actor)
                if close and len(effective) == 1 and effective[0][3].get('issb') is True:
                    counter_source = effective[0][0]
                    attack(effective[0], 'sb', True, counter_source)
                    for record in records:
                        if record[2] == 'source':
                            attack(record, 'sb', False, counter_source)
                    counts['counterActions'] += 1
            phase('ab', 'ad_bfs', 2)
            digest.update((action_path + '\n').encode('utf-8'))
            for row in sorted(rows):
                digest.update(row + b'\n')
            counts['actions'] += 1
    return digest.hexdigest(), counts, progress


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--cpp-report', type=Path, help='Native --audit report containing effectsSha256 for every file.')
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding='utf-8'))
    cpp = json.loads(args.cpp_report.read_text(encoding='utf-8')) if args.cpp_report else None
    native = {f['file']: f for f in cpp['files']} if cpp else {}
    totals = collections.Counter()
    progress = collections.Counter()
    files, errors = [], []
    paths = sorted(p for p in args.samples.iterdir() if p.is_file() and p.suffix.lower() in ('.txt', '.json'))
    for path in paths:
        try:
            raw = path.read_bytes()
            expected, counts, checked_progress = audit(raw, catalog)
            totals.update(counts)
            progress.update(checked_progress)
            raw_hash = hashlib.sha256(raw).hexdigest()
            files.append(dict(file=path.name, sha256=raw_hash, effectsSha256=expected, counts=dict(counts)))
            if cpp is not None:
                actual = native.get(path.name, {})
                if actual.get('sha256') != raw_hash or actual.get('effectsSha256') != expected:
                    errors.append(dict(file=path.name, error='raw-file hash or readable-effect projection differs'))
        except (KeyError, TypeError, ValueError, OSError) as exc:
            errors.append(dict(file=path.name, error=str(exc)))
    if cpp is not None and set(native) != {p.name for p in paths}:
        errors.append(dict(error='native report and sample directory contain different file sets'))
    result = dict(files=len(files), nativeCompared=cpp is not None, counts=dict(totals), progress=dict(progress),
                  errors=errors, entries=files,
                  scope='Every action: exact raw file, source/recipient identity, cause, original record and logged delta. '
                        'Order, numerical before/after and actual appearances require native snapshot checks.')
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'entries'}, ensure_ascii=True))
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
