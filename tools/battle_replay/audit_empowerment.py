"""Conservatively match initial Yuan-neng targets against official configuration.

This is a configuration/log cross-check, not a decoded meaning for `ibat`.
It never treats attribute changes, clones or chasers alone as Yuan-neng.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def attributes(value):
    items = value if isinstance(value, list) else str(value).split(',')
    result = [str(item).strip() for item in items if str(item).strip()]
    return tuple(sorted(result)) if result and all(x.isdecimal() for x in result) else None


def separate_self_transform(change, element_result):
    fighter, _, initial, own_skill = change
    if initial == element_result or not own_skill or own_skill['transformType'] != 2:
        return False
    if fighter.get('ias') or fighter.get('icr') or fighter.get('raceId') != fighter.get('oRaceId', fighter.get('raceId')):
        return False
    for effect in own_skill['effectArr']:
        kind, parameter, selector, selector_parameter = effect.split(':')
        if kind == 'SELF_RACE_TYPE' and selector == 'SELF' and not selector_parameter \
                and attributes(parameter) == initial:
            return effect
    return False


def inspect_player(player, catalog):
    pets, configs = catalog['pets'], catalog['transformSkills']
    providers, changed, unknown = [], [], []
    for fighter in player['cts']:
        pet = pets.get(str(fighter.get('raceId')))
        if not pet:
            unknown.append(fighter.get('id'))
            continue
        transform_id = pet.get('transformSkillNo')
        if isinstance(transform_id, bool) or not isinstance(transform_id, int) or transform_id < 0:
            unknown.append(fighter.get('id'))
            continue
        sid = str(transform_id)
        skill = configs.get(sid)
        if sid != '0' and not skill:
            unknown.append(fighter.get('id'))
        if skill and skill['transformType'] in (0, 1):
            providers.append((fighter, skill, sid))
        native, initial = attributes(pet.get('nativeAttributes')), attributes(fighter.get('attr'))
        if native is None or initial is None:
            unknown.append(fighter.get('id'))
        elif native != initial:
            changed.append((fighter, native, initial, skill))

    def outcome(status, reason, **details):
        return {'status': status, 'reason': reason,
                'providers': [{'id': f['id'], 'name': f['name'], 'skillId': sid,
                               'transformType': s['transformType']} for f, s, sid in providers],
                'attributeChanges': [{'id': f['id'], 'name': f['name'],
                                      'native': list(old), 'initial': list(new),
                                      'rawIbat': f.get('ibat')} for f, old, new, _ in changed],
                **details}

    if unknown:
        return outcome('no_evidence', 'missing_official_reference', unknownIds=unknown)
    if not providers:
        return outcome('no_evidence', 'no_nonself_transform_provider')
    if len(providers) != 1:
        return outcome('ambiguous', 'multiple_nonself_transform_providers')
    provider, skill, sid = providers[0]
    if skill['transformType'] != 0:
        return outcome('no_evidence', 'energize_target_not_in_log')
    effects = skill['effectArr']
    if len(effects) != 1:
        return outcome('no_evidence', 'non_single_effect_definition')
    effect, parameter, selector, selector_parameter = effects[0].split(':')
    if effect != 'ARR_RACE_TYPE' or selector != 'ALL' or selector_parameter:
        return outcome('no_evidence', 'unsupported_target_selector')
    element_result = attributes(parameter)
    independent_self = [change for change in changed if separate_self_transform(change, element_result)]
    candidates = [change for change in changed if not separate_self_transform(change, element_result)]
    if not candidates:
        return outcome('no_evidence', 'no_initial_attribute_difference')
    if len(candidates) != 1:
        return outcome('ambiguous', 'multiple_initial_attribute_differences')
    target, native, initial, own_skill = candidates[0]
    if target['id'] == provider['id']:
        return outcome('no_evidence', 'provider_is_target')
    if own_skill and own_skill['transformType'] == 2:
        return outcome('ambiguous', 'target_has_self_transform_skill')
    if target.get('ias') or target.get('icr') or target.get('raceId') != target.get('oRaceId', target.get('raceId')):
        return outcome('ambiguous', 'target_has_special_initial_identity')
    if initial != element_result:
        return outcome('no_evidence', 'attribute_difference_does_not_match_provider')
    # Extra consistency guard only: this raw flag also occurs on self-transform
    # targets, so it is never sufficient and its name is not assigned a meaning.
    if target.get('ibat') is not True:
        return outcome('no_evidence', 'initial_raw_marker_not_confirmed')
    return outcome('matched', 'official_configuration_and_log_unique_match',
                   sourceId=provider['id'], sourceName=provider['name'],
                   targetId=target['id'], targetName=target['name'],
                   targetSlot=target['x'] + 3 * target['y'] + 1,
                   targetSummoned=target.get('ics') is True,
                   independentSelfTransforms=[{'id': item[0]['id'], 'name': item[0]['name'],
                                               'initial': list(item[2]),
                                               'skillId': str(pets[str(item[0]['raceId'])]['transformSkillNo']),
                                               'effect': separate_self_transform(item, element_result)}
                                              for item in independent_self],
                   skillId=sid, basis='官方配置与日志唯一匹配')


def fighter_key(team, player, fighter):
    # QString::size counts UTF-16 code units, not Python Unicode code points.
    return f'{team}:{len(player.encode("utf-16-le")) // 2}:{player}:{fighter}'


def compare_cpp_report(players, report_path):
    native = json.loads(report_path.read_text(encoding='utf-8-sig'))
    actual = {row['file']: row for row in native['files']}
    expected, hashes = {}, {}
    for row in players:
        hashes[row['file']] = row['sha256']
        expected.setdefault(row['file'], [])
        if row['status'] == 'matched':
            expected[row['file']].append((fighter_key(row['team'], row['player'], row['sourceId']),
                                          fighter_key(row['team'], row['player'], row['targetId']),
                                          row['skillId']))
    mismatches = []
    for filename in sorted(expected.keys() | actual.keys()):
        cpp = actual.get(filename)
        if filename not in expected or cpp is None or 'empowerment' not in cpp:
            mismatches.append({'file': filename, 'reason': 'missing file or empowerment array'})
            continue
        if cpp.get('error') or cpp.get('sha256') != hashes[filename]:
            mismatches.append({'file': filename, 'reason': 'input hash or native parse error'})
            continue
        parsed = [(item['source'], item['target'], str(item['skillId'])) for item in cpp['empowerment']]
        if Counter(parsed) != Counter(expected[filename]):
            mismatches.append({'file': filename, 'reason': 'different recognition result',
                               'python': expected[filename], 'cpp': parsed})
    return {'filesCompared': len(expected), 'mismatchCount': len(mismatches), 'mismatches': mismatches}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('samples', type=Path)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--cpp-report', type=Path)
    args = parser.parse_args()
    catalog_bytes = args.catalog.read_bytes()
    catalog = json.loads(catalog_bytes)
    results, totals, reasons = [], Counter(), Counter()
    file_count, matched_files = 0, set()
    for path in sorted(args.samples.glob('*.txt')):
        raw = path.read_bytes()
        battle = json.loads(raw.decode('utf-8-sig'))['br']
        file_count += 1
        for team in battle['or']['team']:
            for player in team['players']:
                result = inspect_player(player, catalog)
                totals[result['status']] += 1
                reasons[result['reason']] += 1
                if result['status'] == 'matched':
                    matched_files.add(path.name)
                results.append({'file': path.name, 'sha256': hashlib.sha256(raw).hexdigest(),
                                'team': team['id'], 'player': player['name'], **result})
    report = {'format': 1, 'ruleVersion': 2, 'catalogSha256': hashlib.sha256(catalog_bytes).hexdigest(),
              'basis': '官方配置与日志唯一匹配（非ibat字段释义）',
              'limitations': ['Uses downloaded official configuration; historical package identity is not known.',
                              'Ambiguous providers/targets, self transforms and pure energize targets are not inferred.'],
              'files': file_count, 'filesWithMatch': len(matched_files),
              'playerOutcomes': dict(totals), 'reasons': dict(reasons), 'players': results}
    if args.cpp_report:
        report['cppComparison'] = compare_cpp_report(results, args.cpp_report)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'players'}, ensure_ascii=False))
    if report.get('cppComparison', {}).get('mismatchCount'):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
