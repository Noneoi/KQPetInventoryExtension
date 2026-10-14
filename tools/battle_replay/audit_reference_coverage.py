"""Audit a supplied replay catalog; never fill definitions or infer ID bridges.

All unresolved items are returned (no display/sample limit). Reference existence,
empty official text, unresolved pet identity and unmatched textual terms are
different findings. A same-number entry in another namespace is not a match.
The function does not read files, fetch data, or mutate the supplied catalog.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from copy import deepcopy
import json
from pathlib import Path


STATUSES = ('missing-reference', 'official-text-empty', 'identity-unresolved',
            'term-unmatched', 'reference-unclassified')
CATEGORIES = {
    'pet-skill-reference': '精灵技能引用',
    'legend-reference': '旧传说技能引用',
    'huan-reference': '寰力量与异形族引用',
    'mechanism-plan-reference': '气运与溯灵方案引用',
    'unclassified-reference': '命名空间未证实',
    'pet-identity': '精灵原名映射',
    'skill-text': '技能官方文本',
    'buff-text': 'Buff 官方文本',
    'official-term': '官方术语',
}


def _table(value, context):
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f'{context} must be an object')
    result = {str(key): row for key, row in value.items()}
    if len(result) != len(value):
        raise ValueError(f'{context} has duplicate string IDs')
    if any(not isinstance(row, dict) for row in result.values()):
        raise ValueError(f'{context} contains a non-object definition')
    return result


def _object(value, context):
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f'{context} must be an object')
    return value


def _list(value, context):
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f'{context} must be an array')
    return value


def _identifier(value, context):
    if isinstance(value, bool) or not isinstance(value, (str, int)) or not str(value).strip():
        raise ValueError(f'{context} is missing an explicit reference ID')
    # Preserve the literal namespace key. Never remove prefixes or shift IDs.
    return str(value).strip()


def _order(identifier):
    return (0, int(identifier), identifier) if identifier.lstrip('-').isdigit() else (1, identifier)


def _blank(value):
    return value is None or (isinstance(value, str) and not value.strip())


def _pointer(*parts):
    return '/' + '/'.join(str(p).replace('~', '~0').replace('/', '~1') for p in parts)


def _sources(row):
    return {key: deepcopy(row[key]) for key in
            ('source', 'textSource', 'configSource', 'configSourceId',
             'nameSource', 'descriptionSource', 'sourcePath') if key in row}


def _pet_name(pets, identifier):
    pet = pets.get(identifier, {})
    identity = _object(pet.get('identity'), f'pets[{identifier}].identity')
    if identity.get('status') in ('native', 'skin') and identity.get('canonicalName'):
        return identity['canonicalName']
    return f'原名待核实 [{identifier}]'


def audit_reference_coverage(catalog):
    """Return schemaVersion/summary/items using only the passed catalog.

    ``items`` contains findings, aggregated by category + target ID + status;
    every referring owner/field is retained in ``references``. ``summary`` also
    records successful and zero-gap checks. ``reference-unclassified`` keeps
    explicit fields whose consumer namespace has not yet been established.
    """
    catalog = _object(catalog, 'catalog')
    pets = _table(catalog.get('pets'), 'pets')
    mechanics = _table(catalog.get('petMechanics'), 'petMechanics')
    skills = _table(catalog.get('skills'), 'skills')
    buffs = _table(catalog.get('buffs'), 'buffs')
    legend_root = _object(catalog.get('legendMechanics'), 'legendMechanics')
    legends = _table(legend_root.get('definitions'), 'legendMechanics.definitions')
    huan = _table(catalog.get('huanMechanics'), 'huanMechanics')
    rules = _object(catalog.get('mechanismRules'), 'mechanismRules')
    terms = _table(catalog.get('officialEntryMentions'), 'officialEntryMentions')
    items = {}
    checks = {key: {'checkedReferences': 0, 'resolvedReferences': 0,
                    'unmatchedReferences': 0, 'uniqueTargetIds': set(),
                    'unmatchedTargetIds': set()}
              for key in ('petSkills', 'legacyLegend', 'huan', 'mechanismPlans')}
    unclassified_refs = 0
    unclassified_ids = set()
    no_family = 0

    def add(category, identifier, status, name, reason, references, evidence):
        key = category, identifier, status
        if key not in items:
            items[key] = {'name': name, 'category': category,
                'categoryLabel': CATEGORIES[category], 'id': identifier, 'status': status,
                'reasonLabel': reason, 'references': [], 'evidence': deepcopy(evidence)}
        items[key]['references'].extend(deepcopy(references))

    def owner(race, pet, field, value, **extra):
        result = {'namespace': 'petMechanics', 'id': race, 'name': _pet_name(pets, race),
                  'field': field, 'value': value, 'source': deepcopy(pet.get('source')), **extra}
        raw_field = field.rsplit('.', 1)[-1]
        if raw_field == 'unclassifiedLegendSkillNo':
            raw_field = 'legendSkillNo'
        if field == 'mechanismFamily + qiYunType':
            raw_field = 'qiYunType'
        readings = [reading for reading in _list(pet.get('referenceConsumption'), 'referenceConsumption')
                    if reading.get('field') == raw_field and str(reading.get('id')) == str(value)]
        if readings:
            result['referenceConsumption'] = deepcopy(readings)
        return result

    def check(which, target, found):
        row = checks[which]
        row['checkedReferences'] += 1
        row['uniqueTargetIds'].add(target)
        row['resolvedReferences' if found else 'unmatchedReferences'] += 1
        if not found:
            row['unmatchedTargetIds'].add(target)

    for race in sorted(mechanics, key=_order):
        pet = mechanics[race]
        for index, link in enumerate(_list(pet.get('skillLinks'), f'petMechanics[{race}].skillLinks')):
            link = _object(link, f'petMechanics[{race}].skillLinks[{index}]')
            identifier = _identifier(link.get('skillId'), 'skillLinks.skillId')
            found = identifier in skills
            check('petSkills', identifier, found)
            if not found:
                add('pet-skill-reference', identifier, 'missing-reference',
                    f'未匹配技能 [{identifier}]', '精灵明确引用的技能编号未在技能表找到',
                    [owner(race, pet, link.get('field', ''), identifier,
                           fieldLabel=link.get('label', ''), linkIndex=index)],
                    {'lookupNamespace': 'skills', 'catalogPath': _pointer('skills', identifier),
                     'rule': '只检查 skillLinks 指定的技能命名空间；不采用旧 resolved 缓存。'})
        other = _object(pet.get('otherReferences'), f'petMechanics[{race}].otherReferences')
        for field, namespace, target_table, category, which, label in (
            ('legendSkillNo', 'legendMechanics.definitions', legends,
             'legend-reference', 'legacyLegend', '旧传说技能'),
            ('huanSkillNo', 'huanMechanics', huan,
             'huan-reference', 'huan', '寰力量／异形族资料'),
        ):
            if field not in other:
                continue
            identifier = _identifier(other[field], field)
            found = identifier in target_table
            check(which, identifier, found)
            if not found:
                add(category, identifier, 'missing-reference', f'未匹配{label} [{identifier}]',
                    f'精灵明确引用的编号未在{label}表找到',
                    [owner(race, pet, 'otherReferences.' + field, identifier)],
                    {'lookupNamespace': namespace, 'referenceField': field,
                     'rule': '其他命名空间的同编号条目不能用于补齐此引用。'})
        if 'unclassifiedLegendSkillNo' in other:
            identifier = _identifier(other['unclassifiedLegendSkillNo'], 'unclassifiedLegendSkillNo')
            unclassified_refs += 1
            unclassified_ids.add(identifier)
            add('unclassified-reference', identifier, 'reference-unclassified',
                f'命名空间待核关联 [{identifier}]', '关联字段的官方使用分支尚未确认',
                [owner(race, pet, 'otherReferences.unclassifiedLegendSkillNo', identifier,
                       candidates=deepcopy(pet.get('legendReferenceCandidates', [])))],
                {'referenceField': 'rawLegendSkillNo', 'lookupNamespace': None,
                 'rule': '候选说明须有相同命名空间的官方消费证据才算匹配；同编号不是已证实关联。'})
        family = pet.get('mechanismFamily', '')
        if not family:
            no_family += 1
            continue
        identifier = _identifier(pet.get('qiYunType'), 'qiYunType')
        family_rules = _object(rules.get(family), f'mechanismRules[{family}]')
        plans = _table(family_rules.get('plans'), f'mechanismRules[{family}].plans')
        target = family + ':' + identifier
        found = identifier in plans
        check('mechanismPlans', target, found)
        if not found:
            family_label = {'qiYun': '气运', 'suLing': '溯灵'}.get(family, family)
            reason = f'{family_label}配置方案编号未在该类别的方案表找到'
            if identifier == '0':
                reason += '（原记录为 0，不推断默认方案）'
            add('mechanism-plan-reference', target, 'missing-reference',
                f'{family_label}方案 [{identifier}]', reason,
                [owner(race, pet, 'mechanismFamily + qiYunType', identifier, family=family)],
                {'lookupNamespace': f'mechanismRules.{family}.plans',
                 'family': family, 'planId': identifier, 'nameIsGeneratedLabel': True,
                 'rule': '按已解析的类别和编号检索；不跨类别回退，不推断本场达到阈值。'})

    identity_counts = Counter()
    for identifier in sorted(pets, key=_order):
        pet = pets[identifier]
        identity = _object(pet.get('identity'), f'pets[{identifier}].identity')
        status = identity.get('status', 'not-provided')
        identity_counts[status] += 1
        if status != 'unresolved':
            continue
        reason = ('缺少官方精确原名映射' if identity.get('reason') == 'missingExactSkinMapping'
                  else '官方精灵原名映射尚未确认')
        add('pet-identity', identifier, 'identity-unresolved', _pet_name(pets, identifier), reason,
            [{'namespace': 'pets', 'id': identifier, 'field': 'identity'}],
            {'catalogPath': _pointer('pets', identifier, 'identity'), 'identity': identity,
             'catalogDisplayName': pet.get('name', ''), 'sources': _sources(pet),
             'rule': '有精灵定义或显示名不等于已确认原名；不根据同名、相邻编号或皮肤分组猜测。'})

    text_checks = {}
    for table_name, table, name_field, desc_field, category, label in (
        ('skills', skills, 'name', 'description', 'skill-text', '技能'),
        ('buffs', buffs, 'nm', 'mess', 'buff-text', 'Buff'),
    ):
        counts = {'definitionsChecked': len(table), 'namePresent': 0, 'nameEmpty': 0,
                  'descriptionPresent': 0, 'descriptionEmpty': 0, 'rowsWithAnyEmptyText': 0}
        for identifier in sorted(table, key=_order):
            row = table[identifier]
            no_name, no_desc = _blank(row.get(name_field)), _blank(row.get(desc_field))
            counts['nameEmpty' if no_name else 'namePresent'] += 1
            counts['descriptionEmpty' if no_desc else 'descriptionPresent'] += 1
            if not (no_name or no_desc):
                continue
            counts['rowsWithAnyEmptyText'] += 1
            missing = ([name_field] if no_name else []) + ([desc_field] if no_desc else [])
            reason = '名称和独立说明均未提供' if no_name and no_desc else '名称未提供' if no_name else '独立说明未提供'
            evidence = {'catalogPath': _pointer(table_name, identifier),
                        'emptyFields': missing, 'fieldPresence': {f: f in row for f in missing},
                        'rawText': {name_field: row.get(name_field), desc_field: row.get(desc_field)},
                        'sources': _sources(row), 'definitionPresent': True}
            if table_name == 'buffs':
                evidence['separateSupplementalFields'] = {key: deepcopy(row.get(key))
                                                          for key in ('beffect', 'bhs', 'configName')}
                evidence['rule'] = '独立 mess 为空不代表没有 Buff 定义，也不抹掉其他官方效果字样或词条。'
            else:
                evidence['configResolution'] = row.get('configResolution')
                evidence['rule'] = '现有技能条目的文本覆盖；说明缺失不等于引用编号缺表。'
            add(category, identifier, 'official-text-empty',
                row.get(name_field) if not no_name else f'无名称{label} [{identifier}]', reason,
                [{'namespace': table_name, 'id': identifier, 'fields': missing}], evidence)
        text_checks[table_name] = counts

    term_owners = defaultdict(list)
    for map_name, namespace, table, name_field in (
        ('entryMentionsByBuff', 'buffs', buffs, 'nm'),
        ('entryMentionsBySkill', 'skills', skills, 'name'),
    ):
        for identifier, labels in _object(catalog.get(map_name), map_name).items():
            for label in _list(labels, f'{map_name}[{identifier}]'):
                term_owners[label].append({'namespace': namespace, 'id': str(identifier),
                    'name': table.get(str(identifier), {}).get(name_field, ''), 'field': map_name})
    term_statuses = Counter()
    for identifier in sorted(terms):
        row = terms[identifier]
        status = row.get('status', 'not-provided')
        term_statuses[status] += 1
        if status == 'resolved':
            continue
        reason = ('术语参数暂未支持展开' if status == 'unsupported_arguments'
                  else '说明中的术语尚未匹配官方词条')
        add('official-term', identifier, 'term-unmatched', row.get('displayLabel') or identifier,
            reason + '（不等于缺失战斗机制）', term_owners.get(identifier, []),
            {'catalogPath': _pointer('officialEntryMentions', identifier),
             'lookupName': row.get('lookupName'), 'originalStatus': status,
             'arguments': row.get('arguments', []), 'diagnostic': row.get('diagnostic'),
             'sources': _sources(row), 'description': row.get('description', ''),
             'rule': '只报告文字引用解析状态，不把术语与同号技能、Buff 或养成项建立关联。'})

    findings = sorted(items.values(), key=lambda item:
                      (list(CATEGORIES).index(item['category']), _order(item['id']), item['status']))
    for item in findings:
        item['referenceCount'] = len(item['references'])
    for row in checks.values():
        row['uniqueTargetIds'] = sorted(row['uniqueTargetIds'], key=_order)
        row['uniqueTargets'] = len(row['uniqueTargetIds'])
        row['unmatchedTargetIds'] = sorted(row['unmatchedTargetIds'], key=_order)
        row['unmatchedTargets'] = len(row['unmatchedTargetIds'])
    summary = {
        'itemCount': len(findings), 'truncated': False,
        'byStatus': {status: sum(item['status'] == status for item in findings) for status in STATUSES},
        'byCategory': {category: sum(item['category'] == category for item in findings) for category in CATEGORIES},
        'referenceChecks': checks,
        'unclassifiedReferences': {'count': unclassified_refs, 'uniqueTargets': len(unclassified_ids),
                                   'targetIds': sorted(unclassified_ids, key=_order)},
        'mechanicsWithoutFamily': no_family,
        'textChecks': text_checks,
        'identityChecks': {'petsChecked': len(pets), 'statusCounts': dict(identity_counts),
                           'unresolved': identity_counts['unresolved']},
        'termChecks': {'mentionsChecked': len(terms), 'resolved': term_statuses['resolved'],
                       'unmatched': sum(count for status, count in term_statuses.items() if status != 'resolved'),
                       'originalStatusCounts': dict(term_statuses)},
        'inputSectionsPresent': {key: key in catalog and catalog[key] is not None for key in
            ('petMechanics', 'skills', 'buffs', 'pets', 'legendMechanics',
             'huanMechanics', 'mechanismRules', 'officialEntryMentions')},
    }
    return {'schemaVersion': 1, 'summary': summary, 'items': findings,
            'scope': '传入资料库的明确引用与文本完整性；全量明细，无截断。不是战报发生次数或服务器机制审计。',
            'boundaries': [
                '相同编号只在明确命名空间内匹配；不把说明词条、养成编号或属性编号当成战斗 Buff。',
                '技能名称或说明为空、Buff 独立 mess 为空，与缺失定义是不同状态。',
                '精灵原名待核与精灵定义缺失是不同状态。',
                '无机制类别的 qiYunType 不猜测归属；显式类别下的 0 号方案原样核对，不替换为默认方案。',
                '返回的名称可为审计标签，未向技能、Buff 或机制定义写入合成占位。',
                '未提供的输入表会在 summary 标明；零计数不代表已扫描未传入的表。',
            ]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit_reference_coverage(json.loads(args.catalog.read_text(encoding='utf-8-sig')))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({key: result['summary'][key] for key in ('itemCount', 'byStatus')}, ensure_ascii=False))
