"""Normalize public reward structures, without executing client code.

Adapters are selected by fields and their official consumers, never by event
names or dates. Unresolved targeting is a manual reward, not an all-pets rule.
"""
from __future__ import annotations

import copy
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re

CODE = r'[1-9]\d*(?:\$[1-9]\d*)?(?:-[1-9]\d*(?:\$[1-9]\d*)?)*'
MATERIALS = r'[1-9]\d*:[1-9]\d*(?::[1-9]\d*)?(?:#[1-9]\d*:[1-9]\d*(?::[1-9]\d*)?)*'
CHOICE_TYPES = {'NChoose1', 'ArbitraryChoice', 'Choice', 'SelectPrizes'}
ORDINARY_TYPES = {'Material', 'BatchMaterial', 'Pet', 'PetOpen', 'PetGain', 'PetSkin', 'PetEvolve', 'PetItem',
                  'Clothes', 'DigitalCode', 'Item', 'Skin', 'Equipment', 'NicknameCard',
                  '皮肤', '物品', '批量物品', '精灵开启', '精灵获得', '精灵第二只', '租借精灵'}


def period_point(value, *, end=False):
    """Date-only expiry includes that day; explicit timestamps are boundaries."""
    from generate_shop_exchange_data import official_date
    if not isinstance(value, str):
        raise ValueError('活动有效期不是可确认的时间文本')
    compact = re.sub(r'^(\d{4})-(\d{2})-(\d{2})', r'\1\2\3', value)
    day = official_date(compact)
    point = datetime.strptime(day, '%Y%m%d').replace(tzinfo=timezone(timedelta(hours=8)))
    if ' ' in compact:
        clock = datetime.strptime(compact.split(' ', 1)[1], '%H:%M:%S').time()
        point = point.replace(hour=clock.hour, minute=clock.minute, second=clock.second)
    elif end:
        point += timedelta(days=1)
    return point


def apply_activity_period(item, activity):
    bounds = item['source']['periodBounds']
    evidence = activity.get('periodEvidence', [])
    starts = [value for value in (bounds.get('rowStart'), bounds.get('moduleStart'), activity.get('startTime'),
                                 *(p.get('startTime') for p in evidence)) if value]
    ends = [value for value in (bounds.get('rowEnd'), bounds.get('tableEnd'), bounds.get('moduleEnd'), activity.get('endTime'),
                               *(p.get('endTime') for p in evidence)) if value]
    start = max(starts, key=period_point) if starts else ''
    end = min(ends, key=lambda v: period_point(v, end=True)) if ends else ''
    start_at = period_point(start) if start else None
    end_at = period_point(end, end=True) if end else None
    item.update({'shelfTime': start_at.strftime('%Y%m%d') if start_at else '',
                 'removalTime': (end_at - timedelta(microseconds=1)).strftime('%Y%m%d') if end_at else '',
                 'startsAt': start_at.isoformat(timespec='seconds') if start_at else '',
                 'endsAt': end_at.isoformat(timespec='seconds') if end_at else '',
                 'officialShelfTime': start, 'officialRemovalTime': end,
                 'availableKnown': bool(start),
                 'availabilityClosed': bool(start_at and end_at and start_at >= end_at)})
    item['source']['activityPeriodEvidence'] = copy.deepcopy(evidence)


def module_deadline(scripts, constants):
    """Only a consumed, explicit whole-activity end/online predicate is a gate.

    DateUtil.parse(..., true) appends the verified 02:00 server closing time.
    Sale countdowns, unrelated dates and mini-game timers do not qualify.
    """
    all_text = '\n'.join(scripts.values())
    deadlines = set()
    for filename, text in scripts.items():
        cls = Path(filename).stem
        values = dict(constants)
        for m in re.finditer(r'(?:const|var)\s+(\w+)\s*:\s*Number\s*=\s*DateUtil\.(?:parse|parseDate)\("([^"\n]+)",\s*(true|false)\)\.getTime\(\)', text):
            values[cls + '.' + m[1]] = m[2] + (' 02:00:00' if m[3] == 'true' and ' ' not in m[2] else '')
        for m in re.finditer(r'static\s+function\s+(?:get\s+)?(isActEnd|isActivityEnd|isActOnline|isActivityOnline)\(\s*\)\s*:\s*Boolean\s*\{\s*return\s+DateUtil\.getServerTimeInMS\(\)\s*([<>]=?)\s*([^;]+);\s*\}', text):
            name, operator, expression = m.groups()
            if (name.endswith('Online') and not operator.startswith('<')) or (not name.endswith('Online') and not operator.startswith('>')):
                continue
            if not re.search(re.escape(cls + '.' + name) + r'(?:\(\s*\)|\b)', all_text):
                continue
            parsed = re.fullmatch(r'DateUtil\.(?:parse|parseDate)\(\s*([\w.]+|"[^"\n]+")\s*,\s*(true|false)\s*\)\.getTime\(\)', expression.strip())
            token = parsed[1] if parsed else expression.strip()
            raw = token[1:-1] if token.startswith('"') else values.get(token, values.get(cls + '.' + token))
            if not isinstance(raw, str):
                continue
            if ' ' not in raw:
                raw += ' 02:00:00' if parsed and parsed[2] == 'true' else ' 00:00:00'
            try:
                period_point(raw)
                deadlines.add(raw)
            except ValueError:
                continue
        for m in re.finditer(r'static\s+function\s+(?:get\s+)?(isActEnd|isActivityEnd|isActOnline|isActivityOnline)\(\s*\)\s*:\s*Boolean\s*\{\s*return\s+(!?)DateUtil\.isAfterTimeWithDelayClose\(\s*([\w.]+|"[^"\n]+")\s*\);\s*\}', text):
            if bool(m[2]) != m[1].endswith('Online') or not re.search(re.escape(cls + '.' + m[1]) + r'\b', all_text):
                continue
            raw = m[3][1:-1] if m[3].startswith('"') else values.get(m[3], values.get(cls + '.' + m[3]))
            if not isinstance(raw, str):
                continue
            raw += ' 02:00:00' if ' ' not in raw else ''
            try:
                period_point(raw)
                deadlines.add(raw)
            except ValueError:
                continue
    return next(iter(deadlines)) if len(deadlines) == 1 else ''


def description_parameters(source, basic_types):
    """Read direct official description substitutions; do not execute AS."""
    result = {}
    pattern = (r'((?:case\s+StrengthenComboBasicType\.\w+\s*:\s*)+)'
               r'return\s+\w+\.replace\("#level#",\s*\w+\.getParamsInInt\(\w+,\s*(\d+),\s*(\d+)\)\);')
    for match in re.finditer(pattern, source):
        for name in re.findall(r'StrengthenComboBasicType\.(\w+)', match[1]):
            definition = basic_types.get(name)
            if definition:
                result[str(definition['id'])] = {'index': int(match[2]), 'default': int(match[3])}
    return result


def enhancement_description(codes, support):
    if not isinstance(codes, str) or not re.fullmatch(CODE, codes):
        return ''
    support = support or {}
    labels = []
    for token in codes.split('-'):
        code, _, parameter = token.partition('$')
        label = support.get('descriptions', {}).get(code)
        if not isinstance(label, str) or not label:
            return ''
        if '#level#' in label:
            rule = support.get('descriptionParameters', {}).get(code)
            if not isinstance(rule, dict):
                return ''
            # The supported token has a single parameter. Other parameter
            # positions use the official default rather than reusing it.
            level = parameter if parameter and rule['index'] == 0 else str(rule['default'])
            label = label.replace('#level#', level)
        if re.search(r'#[A-Za-z]+#', label):
            return ''
        labels.append(label)
    return ' / '.join(labels)


def cultivation_reward_description(simple, codes, support):
    if not codes and isinstance(simple, str) and simple.startswith('Strengthen,'):
        codes = simple.split(',', 2)[1]
    label = enhancement_description(codes, support)
    if not label:
        return ''
    label = '指定精灵：' + label
    if isinstance(simple, str) and simple.lstrip('!').startswith('Choice,|'):
        choices = simple.split('|')[1:]
        if len(choices) == 2 and any(c.lstrip('!').startswith('Material,') for c in choices):
            label += '（与材料奖励二选一）'
        elif len(choices) > 1:
            label += '（任选奖励之一）'
    return label


def upgrade_cultivation_description(description, simple, codes, support):
    if not isinstance(description, str):
        return description
    heading, separator, suffix = description.partition(' · ')
    generic = re.fullmatch(r'(?:指定(?:精灵)?|精灵|通用|专属|完美|超完美|至尊|奖励|项目|养成|培养|强化|提升|套餐|\s)*', heading)
    bundle = heading.endswith(('极品', '极品养成')) and not any(mark in heading for mark in ('#', '（', '('))
    alternative = isinstance(simple, str) and simple.removeprefix('!').startswith('Choice,|')
    if alternative and '与材料奖励二选一' in description:
        return description
    if not generic and not bundle and not alternative and heading not in ('任选奖励', '养成内容待确认'):
        return description
    label = cultivation_reward_description(simple, codes, support)
    if not label:
        return description
    if bundle or alternative and not generic:
        label = heading + '（' + label.removeprefix('指定精灵：') + '）'
    return label + (separator + suffix if separator else '')


def reward_hints(strings):
    """A prefilter may avoid export, but may never decide unsupported = empty."""
    return any(value in {'simpleParams', 'rewardType', 'rewardTypes', 'Strengthen', 'CommonEnhancePrize',
                         'NChoose1', 'ArbitraryChoice', 'EXTENDED_PRIZE_MAP'} or
               re.search(r'(?:CommonEnhancePrize|Material|Choice|SelectPrizes),|StrengthenCombo|IStrengthen', value)
               for value in strings)


def symbolic(value):
    if isinstance(value, dict):
        return any(key.startswith('$') for key in value) or any(symbolic(v) for v in value.values())
    return isinstance(value, list) and any(symbolic(v) for v in value)


def explicit_races(value):
    if type(value) is int:
        value = [value]
    if isinstance(value, str) and re.fullmatch(r'[1-9]\d*(?:#[1-9]\d*)*(?::(?:false|0))?', value):
        value = value.split(':')[0].split('#')
    if isinstance(value, list) and value and all(
            (type(v) is int or isinstance(v, str) and re.fullmatch(r'[1-9]\d*', v)) and
            0 < int(v) <= 2**31 - 1 for v in value):
        return list(dict.fromkeys(int(v) for v in value))
    return []


def strengthen(row, params, target, *, structure, manual=False):
    if not isinstance(params, str) or not re.fullmatch(CODE + r'(?:\?[^?\r\n]+)?', params):
        raise ValueError('强化奖励参数包含未支持的表达式')
    codes = params.split('?')[0]
    races = explicit_races(target)
    result = {**row, '_structure': structure, '_targeting': target,
              '_manualReward': manual or not races,
              '_rawReward': json.dumps({'rewardType': 'Strengthen', 'rewardParams': params, 'filter': target},
                                      ensure_ascii=False, sort_keys=True)}
    if races:
        result['simpleParams'] = 'CommonEnhancePrize,,,' + codes + ',' + '#'.join(map(str, races))
        result['_typedCodes'] = codes
    else:
        # Exact effect is known; the filter can be a runtime predicate or an
        # exclusion list. Keep it visible without making an applicability rule.
        result['simpleParams'] = 'Strengthen,' + codes + ',' + json.dumps(target, ensure_ascii=False, sort_keys=True)
        result['_targetScopeUnknown'] = True
    return result


def normalize_tables(scripts, tables, constants, support=None):
    from public_activity_exchange_updater import LiteralReader, _arguments, _balanced

    tables = copy.deepcopy(tables)
    text = '\n'.join(scripts.values())
    support = support or {}
    files = {Path(filename).stem: filename for filename in scripts}
    issues = []

    def issue(table, reason, row=None):
        item = {'table': table, 'reason': reason}
        if row:
            item['itemId'] = next((row[k] for k in ('serverIndex', 'serverId', 'bi', 'index', 'id')
                                  if type(row.get(k)) is int), None)
        if item not in issues:
            issues.append(item)

    def resolved(value, namespace=''):
        if isinstance(value, dict):
            if set(value) == {'$ref'}:
                return constants.get(value['$ref'], constants.get(namespace + '.' + value['$ref'], value))
            if set(value) == {'$index', '$key'}:
                target, key = resolved(value['$index'], namespace), resolved(value['$key'], namespace)
                if isinstance(target, dict) and isinstance(key, str) and key in target:
                    return target[key]
                if isinstance(target, list) and type(key) is int and 0 <= key < len(target):
                    return target[key]
                return value
            return {k: resolved(v, namespace) for k, v in value.items()}
        if isinstance(value, list):
            return [resolved(v, namespace) for v in value]
        return value

    def expression(raw, namespace=''):
        try:
            return resolved(LiteralReader(raw).parse(), namespace)
        except (ValueError, IndexError):
            return {'$ref': raw}

    # Partly dynamic tables must not discard their independently static rows.
    for key, value in constants.items():
        if key not in tables and isinstance(value, (list, dict)) and key.split('.')[0] in files:
            tables[key] = files[key.split('.')[0]], copy.deepcopy(value)

    # Follow explicit default filter assignments used by ActRewardsData and
    # ActReward subclasses. The table-local binding wins over a module default.
    filters, defaults = {}, []
    for match in re.finditer(r'([\w.]+)\.setConsistentConfig\s*\(\s*\{', text):
        try:
            obj, end = _balanced(text, text.index('{', match.start()))
            config = expression(obj)
            target = config.get('filter') if isinstance(config, dict) else None
            use = re.search(re.escape(match[1]) + r'\.init\(\s*([\w.]+)', text[end:end+600])
            if target is not None and use:
                filters[use[1]] = target
        except (ValueError, IndexError):
            pass
    for match in re.finditer(r'\w+\["filter"\]\s*=\s*([\w.]+)\s*;', text):
        target = expression(match[1])
        if not symbolic(target):
            defaults.append(target)
    for match in re.finditer(r'\.showStrengthenPanel\s*\(', text):
        try:
            call, _ = _balanced(text, match.end()-1)
            args = _arguments(call[1:-1])
            if len(args) >= 3:
                target = expression(args[2])
                if not symbolic(target):
                    defaults.append(target)
        except (ValueError, IndexError):
            pass
    unique_defaults = {json.dumps(v, sort_keys=True, ensure_ascii=False): v for v in defaults}
    default = next(iter(unique_defaults.values())) if len(unique_defaults) == 1 else None
    consumed_fields = set(re.findall(r'StrengthenComboItem\.createByString\([^,;\n]*?\["(\w+)"\]', text))

    def target_for(row, table):
        if 'filter' in row:
            return row['filter']
        return filters.get(table, default)

    def typed(row, table):
        kind = row.get('rewardType')
        params = row.get('rewardParams')
        if kind == 'Strengthen':
            return strengthen(row, params, target_for(row, table), structure='ActReward.Strengthen')
        if kind in CHOICE_TYPES:
            if not isinstance(params, str) or not params or symbolic(row.get('nChoose1Config')):
                raise ValueError('任选奖励内容尚未完整解析')
            options = row.get('nChoose1Config')
            if isinstance(options, list):
                if not options or any(not isinstance(v, dict) or not v.get('rewardType') or
                                      not isinstance(v.get('rewardParams'), str) or symbolic(v) for v in options):
                    raise ValueError('任选奖励选项表不完整')
            elif kind == 'NChoose1':
                options = params.split(';')
            result = {**row, '_structure': 'ActReward.' + kind, '_manualReward': True,
                      '_rewardOptions': options or [],
                      'simpleParams': 'Choice,' + kind + ',' + params}
            if kind == 'ArbitraryChoice':
                result['_choiceScopeExternal'] = True
            return result
        if kind in {'Material', 'BatchMaterial'} and isinstance(params, str) and re.fullmatch(MATERIALS, params):
            return {**row, 'simpleParams': 'Material,' + params, '_structure': 'ActReward.Material'}
        if kind not in ORDINARY_TYPES and isinstance(kind, str) and kind:
            issue(table, '未支持的官方奖励类型：' + kind, row)
        return row

    # A custom name is meaningful only if official executable code uses that
    # exact comparison to select a reward handler (not because it says 任选).
    choice_handlers = {}
    for match in re.finditer(r'function\s+get\s+(\w+)\([^)]*\)\s*:\s*Boolean\s*\{\s*return\s+this\.name\.indexOf\("([^"\n]+)"\)\s*!=\s*-1;', text):
        name, label = match[1], match[2]
        if re.search(r'if\([^\n]*\.' + re.escape(name) + r'\)', text):
            choice_handlers[label] = name

    def normalize(value, table):
        if isinstance(value, list):
            result = []
            for v in value:
                child = normalize(v, table)
                if isinstance(child, dict) and set(child) == {'normalizedProgressRewards'}:
                    result.extend(child['normalizedProgressRewards'])
                else:
                    result.append(child)
            return result
        if not isinstance(value, dict) or symbolic(value) and any(k.startswith('$') for k in value):
            return value
        row = copy.deepcopy(value)
        try:
            # Existing SimpleActReward and verified legacy Strengthen builders
            # already carry stronger cost/counter evidence. Keep their rules.
            if 'simpleParams' in row or row.get('type') == 'Strengthen' and 'params' in row:
                return row
            raw = row.get('rewardParams')
            if isinstance(raw, str) and raw.startswith('Strengthen:'):
                return strengthen(row, raw[len('Strengthen:'):], target_for(row, table), structure='tagged-strengthen')
            if row.get('type') == 'Strengthen' and 'goodsId' in row and 'params' not in row:
                issue(table, '强化商品通过公共商城商品编号提供，尚缺少该商品的奖励配置', row)
            if isinstance(row.get('rewardType'), str):
                return typed(row, table)
            for field in consumed_fields:
                if (isinstance(row.get(field), str) and re.fullmatch(CODE, row[field]) and
                    any(isinstance(row.get(k), str) for k in ('desc', 'name', 'basicDescription'))):
                    return strengthen(row, row[field], target_for(row, table), structure='consumed-strengthen-field')
            # Progress reward constructors expose parallel, typed fields.
            if all(isinstance(row.get(k), str) for k in ('rewardIds', 'rewardTypes', 'rewardNames', 'putRaceIds')):
                ids, kinds, names = (row[k].split('#') for k in ('rewardIds', 'rewardTypes', 'rewardNames'))
                if len(ids) != len(kinds) or len(names) != len(ids):
                    raise ValueError('进度奖励的类型、内容、名称数量不一致')
                projected = []
                for index, (param, kind, name) in enumerate(zip(ids, kinds, names)):
                    # EB_Util and matching future utilities explicitly map
                    # these two localized type values to Strengthen.
                    if kind in ('强化', '养成') and re.search(r'indexOf\("' + kind + r'"\)[\s\S]{0,80}?return "Strengthen"', text):
                        context = f" · 阶段 {row['campaignId']}" if type(row.get('campaignId')) is int else ''
                        if type(row.get('needToken')) is int:
                            context += f" · 进度 {row['needToken']}"
                        projected.append(strengthen({**row, 'basicDescription': name + context,
                            '_rewardField': 'progress/' + str(index), '_acquisitionKind': 'progress'},
                            param, row['putRaceIds'], structure='typed-progress', manual=True))
                return {'normalizedProgressRewards': projected}
            if isinstance(row.get('prizes'), str) and 'putPetFilter' in row and re.search(r'StrengthenComboItem\.createByString\(\w+\.prizes\)', text):
                return strengthen({**row, '_acquisitionKind': 'progress', '_rewardField': 'personal'},
                                  row['prizes'], row['putPetFilter'], structure='personal-strengthen', manual=True)
            # Constructor-backed exchange table; property and actual service
            # consumer establish the effect and target, not the class name.
            if isinstance(row.get('info'), str) and 'compensateInfo' in row and re.search(r'StrengthenComboItem\.createByString\(\w+\.info\s*,\s*\w+\.compensateInfo\)', text):
                return strengthen({**row, '_acquisitionKind': 'exchange', '_rewardField': 'constructed'},
                                  row['info'], target_for(row, table), structure='constructed-strengthen', manual=True)
            # VIP selection encodes the effect and the literal filter in a
            # tuple. Verify both indexed accesses in the official getters.
            if isinstance(row.get('prizes'), list) and len(row['prizes']) >= 2:
                params, target = row['prizes'][:2]
                if isinstance(params, str) and re.fullmatch(CODE, params) and explicit_races(target) and (
                        'getPerfectStrengthenParams' in text and '["prizes"][1]' in text and 'showStrengthenPanel' in text):
                    return strengthen({**row, '_acquisitionKind': 'progress', '_rewardField': 'vip'},
                                      params, target, structure='strengthen-tuple', manual=True)
            label = row.get('name')
            if isinstance(label, str) and any(token in label for token in choice_handlers):
                return {**row, 'simpleParams': 'Choice,Service,' + label, '_manualReward': True,
                        '_rewardField': 'personal-choice', '_acquisitionKind': 'progress',
                        '_structure': 'official-choice-handler', '_choiceScopeExternal': True}
            if isinstance(row.get('mcds'), str) and isinstance(row.get('gainType'), str) and 'openDiamond' in row:
                # The actual branch dispatches the multi-material reward to
                # the official selection panel; do not infer from its name.
                if row['mcds'].count('#') > 0 and 'showChoose' in text and re.fullmatch(MATERIALS, row['mcds']):
                    return {**row, 'simpleParams': 'Choice,Materials,' + row['mcds'],
                            '_rewardField': 'personal-choice', '_manualReward': True,
                            '_acquisitionKind': 'progress', '_structure': 'personal-material-choice'}
            return {k: normalize(v, table) for k, v in row.items()}
        except (ValueError, TypeError, IndexError) as error:
            issue(table, str(error), row)
            return row

    for table, (filename, value) in list(tables.items()):
        tables[table] = filename, normalize(value, table)

    # A direct enum array passed to createByBasic has the same semantics as
    # createByString. Resolve enum IDs from the current official interface.
    for filename, source in scripts.items():
        cls = Path(filename).stem
        for match in re.finditer(r'StrengthenComboItem\.createByBasic\(\s*(\w+)\s*[,)]', source):
            declarations = list(re.finditer(r'\b' + re.escape(match[1]) + r'\s*:\s*Array\s*=\s*(\[[^;]+\])\s*;', source[:match.start()]))
            if not declarations:
                continue
            values = expression(declarations[-1][1], cls)
            if not isinstance(values, list):
                continue
            codes, names = [], []
            for value in values:
                enum = value.get('$ref', '').rsplit('.', 1)[-1] if isinstance(value, dict) else ''
                definition = support.get('basicTypes', {}).get(enum)
                if definition:
                    codes.append(str(definition['id']))
                    names.append(definition['name'])
                elif isinstance(value, str) and re.fullmatch(r'[1-9]\d*', value) and codes:
                    codes[-1] += '$' + value
                else:
                    codes = []
                    break
            if not codes:
                issue(cls + '.directRewards', '强化枚举奖励需要当前公共接口定义，尚未完整解析')
                continue
            row = strengthen({'basicDescription': ' / '.join(names), '_rewardField': 'direct/' + match[1]},
                             '-'.join(codes), default, structure='enum-strengthen', manual=True)
            tables[cls + '.directRewards/' + match[1]] = filename, [row]

    # Generic cultivation panel links point into a separately versioned public
    # catalog. Join only the explicitly requested race with its own packages.
    for filename, source in scripts.items():
        cls = Path(filename).stem
        requested = set()
        for match in re.finditer(r'\.showPanelCombo\s*\(', source):
            try:
                call, _ = _balanced(source, match.end()-1)
                raw = re.sub(r'\.toString\(\)$', '', call[1:-1].strip())
                requested.update(explicit_races(expression(raw, cls)))
            except (ValueError, IndexError):
                pass
        rewards = []
        for race in sorted(requested):
            pet = support.get('pets', {}).get(str(race))
            if not isinstance(pet, dict) or str(pet.get('testOnly')) != '0':
                issue(cls + '.sharedCultivation', '公共养成配置读取失败：' + support['error'] if support.get('error') else
                      '公共养成面板尚无该入口精灵的可验证独立配置')
                continue
            contents = pet.get('priorContains') or pet.get('formulaicContains')
            if not isinstance(contents, str):
                issue(cls + '.sharedCultivation', '公共养成套餐关联格式待适配')
                continue
            for group in contents.split('#'):
                parts = group.split('-')
                if len(parts) > 2:
                    issue(cls + '.sharedCultivation', '公共养成套餐分组格式待适配')
                    continue
                if len(parts) == 2 and parts[1] == str(support.get('hiddenDisplayId')):
                    continue
                for binding in parts[0].split('&'):
                    price_id = binding.split('$')[0]
                    price = support.get('prices', {}).get(price_id)
                    if not isinstance(price, dict):
                        issue(cls + '.sharedCultivation', '公共养成套餐缺少奖励定义：' + price_id)
                        continue
                    code = price.get('strengthenParams')
                    label = enhancement_description(code, support) or '养成内容待确认'
                    try:
                        row = strengthen({'basicDescription': label,
                            '_rewardField': f'shared/{race}/{price_id}', '_acquisitionKind': 'exchange',
                            'publicPriceRaw': price, 'publicPriceBinding': binding,
                            'unlock': '公共养成套餐；折扣、生效条件和费用请在游戏养成面板确认'},
                            code, [race], structure='shared-cultivation', manual=True)
                        rewards.append(row)
                    except ValueError as error:
                        issue(cls + '.sharedCultivation', str(error))
        if rewards:
            tables[cls + '.sharedCultivation'] = filename, rewards

    # Passes sometimes store a ! placeholder in a lane and its selectable
    # content in a second map. Only live lane keys qualify; orphan icon/tips
    # entries (including old new-player pass leftovers) must stay excluded.
    for key, (filename, mapping) in list(tables.items()):
        if not key.endswith('.EXTENDED_PRIZE_MAP') or not isinstance(mapping, dict):
            continue
        cls = key.split('.')[0]
        level = re.search(r'return\s*\(this\.\w+\s*\?\s*"extreme_"\s*:\s*"normal_"\)\s*\+\s*this\.\w+\.(\w+)', text)
        if not level:
            continue
        live = {}
        for other, (_, rows) in tables.items():
            if not other.startswith(cls + '.') or not isinstance(rows, list):
                continue
            for row in rows:
                if isinstance(row, dict) and type(row.get(level[1])) is int:
                    for lane, field in [('extreme_', 'prizeExtreme'), ('normal_', 'prizeNormal')]:
                        if isinstance(row.get(field), str) and row[field].startswith('!'):
                            live[lane + str(row[level[1]])] = row
        rewards = []
        for subkey, definition in mapping.items():
            if subkey not in live or not isinstance(definition, dict):
                continue
            kind = definition.get('type')
            if kind not in ('starGodSelection', 'equipmentSelection', 'petSelection', 'skinSelection', 'petSelectionSingle'):
                continue
            options = definition.get('defines')
            if not isinstance(options, list) or not options or symbolic(options):
                issue(key, '通行证任选奖励选项尚未解析')
                continue
            rewards.append({**live[subkey], 'basicDescription': definition.get('name') or definition.get('tips') or '任选奖励',
                'simpleParams': 'Choice,' + kind + ',' + json.dumps(options, ensure_ascii=False, sort_keys=True),
                '_rewardField': subkey, '_manualReward': True, '_acquisitionKind': 'progress',
                '_structure': 'pass-extended-choice', '_rewardOptions': options})
        if rewards:
            tables[key + '.selectedRewards'] = filename, rewards
    return tables, issues
