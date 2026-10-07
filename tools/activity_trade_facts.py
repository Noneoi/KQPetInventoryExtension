"""Table-scoped price, quota and claim facts from official reward consumers.

No event/class names or dates select an adapter. A numeric field only acquires
meaning through its getter, payment call, limit check or ActRewardsData binding.
Static facts do not grant permission to query an unrelated account protocol.
"""
from __future__ import annotations

import re
from pathlib import Path


def integer(value):
    if type(value) is int:
        return value
    if isinstance(value, str) and re.fullmatch(r'\d+', value):
        return int(value)
    return None


def property_fields(source):
    """Resolve simple getters to their literal config field (no evaluation)."""
    fields = {}
    for m in re.finditer(r'function\s+get\s+(\w+)\(\)\s*:\s*\w+\s*\{\s*return\s+([^;]+);\s*\}', source):
        prop, expression = m.groups()
        frag = re.fullmatch(r'fragmentCollection\.getFrag\("(\w+)"\)\.configInfo\.asInt', expression)
        if frag:
            fields[prop] = frag[1]
            continue
        member = re.fullmatch(r'this\.(\w+)', expression)
        if member:
            assignment = re.search(r'this\.' + re.escape(member[1]) + r'\s*=\s*\w+\["(\w+)"\]\s*;', source)
            if assignment:
                fields[prop] = assignment[1]
    return fields


def dynamic_price_rules(reward, cls, table, scripts):
    from public_activity_exchange_updater import _balanced, as_methods
    text = '\n'.join(scripts.values())
    for match in re.finditer(r'function\s+(?:get\s+)?(\w+)\(\)\s*:\s*int\s*\{', reward):
        method = match[1]
        body, _ = _balanced(reward, match.end() - 1)
        paid = False
        for consumer in scripts.values():
            variables = re.findall(r'\b(\w+)\s*:\s*' + re.escape(cls) + r'\b', consumer)
            if consumer == reward:
                variables.append('this')
            paid |= any(re.search(r'\.diamondSelf\(\s*' + re.escape(v + '.' + method) + r'(?:\(\))?\s*,', consumer) for v in variables)
        if not paid:
            continue
        base = re.search(r'var\s+(\w+):int\s*=\s*int\(fragmentCollection\.getFrag\("(\w+)"\)\.configInfo\.asInt\)', body)
        if base:
            step = re.search(r'return this\.(\w+) == 1 \? int\(' + re.escape(base[1]) + r' - (\d+)\) : \(this\.\1 >= 2 \? int\(' + re.escape(base[1]) + r' - (\d+)\) : ' + re.escape(base[1]) + r'\);', body)
            if step:
                counter = re.search(r'this\.' + re.escape(step[1]) + r'\s*=\s*\w+\["(\w+)"\]\s*;', reward)
                if counter:
                    return {'field': base[2], 'offsets': [0, int(step[2]), int(step[3])], 'path': [counter[1]]}
        # Table row -> price vector -> saturating purchase-count index.
        row = re.search(r'var\s+(\w+):Object\s*=\s*' + re.escape(table) + r'\[serverIndex\]', body)
        prices = re.search(r'var\s+(\w+):Array\s*=\s*' + re.escape(row[1]) + r'\["(\w+)"\]\s+as Array', body) if row else None
        selector = re.search(r'return\s+' + re.escape(prices[1]) + r'\[([\w.]+)\([\w.]+\.model\.(\w+)\)\]', body) if prices else None
        if selector:
            owner, _, function = selector[1].rpartition('.')
            owner_source = reward if owner in ('', 'this') else next((s for f, s in scripts.items() if Path(f).stem == owner), '')
            implementation = next((b for n, b in as_methods(owner_source) if n == function), '')
            clamp = re.search(r'if\((\w+) >= (\d+)\)\s*\{\s*return \2;\s*\}\s*return Math.max\(0,\1\);', implementation)
            getter = re.search(r'function get ' + re.escape(selector[2]) + r'\(\)\s*:\s*int\s*\{\s*return this\.(\w+);', text)
            paths = set(re.findall(r'this\.' + re.escape(getter[1]) + r'\s*=\s*\w+\["(\w+)"\]', text)) if getter else set()
            if clamp and len(paths) == 1:
                return {'field': prices[2], 'slots': int(clamp[2]) + 1, 'path': [paths.pop()]}
    return {}


def table_contracts(scripts, tables, constants):
    from public_activity_exchange_updater import LiteralReader, _balanced
    files = {Path(f).stem: text for f, text in scripts.items()}
    text = '\n'.join(scripts.values())
    contracts = {}
    aliases = {}
    for cls, source in files.items():
        for match in re.finditer(r'static\s+function\s+get\s+(\w+)\(\)\s*:\s*Array\s*\{\s*return\s+(\w+)(?:\.concat\(\))?\s*;\s*\}', source):
            aliases[cls + '.' + match[1]] = cls + '.' + match[2]
    for source in scripts.values():
        bindings = [(m[1], m[2], m[3], m.start()) for m in re.finditer(
            r'([\w.]+|new\s+ActRewardsData\(\))\.init\(\s*([\w.]+)\s*,\s*\w+\s*,\s*(\w+)\s*\)', source)]
        for m in re.finditer(r'([\w.]+)\.forEach\(function\((\w+):[^)]*\)\s*:[^{]+\{', source):
            body, _ = _balanced(source, m.end() - 1)
            created = re.search(r'new\s+(\w+)\(\)\.initConfig\(' + re.escape(m[2]) + r'\)', body)
            if created:
                bindings.append(('', m[1], created[1], m.start()))
        for receiver, table, cls, position in bindings:
            table = aliases.get(table, table)
            reward = files.get(cls, '')
            if table not in tables or not re.search(r'extends\s+(?:Simple)?ActReward\b', reward):
                continue
            contract = {'rewardClass': cls, 'source': reward, 'defaults': {}}
            before = source[:position]
            resets = list(re.finditer(re.escape(receiver.removeprefix('this.')) + r'\s*=\s*new ActRewardsData\(\)', before)) if receiver else []
            if resets:
                before = before[resets[-1].end():]
            configs = list(re.finditer(re.escape(receiver) + r'\.setConsistentConfig\(\s*\{', before)) if receiver else []
            if configs:
                try:
                    raw, _ = _balanced(before, before.index('{', configs[-1].start()))
                    config = LiteralReader(raw).parse()
                    if isinstance(config, dict):
                        contract['defaults'] = config
                except ValueError:
                    pass
            claimed = list(re.finditer(re.escape(receiver) + r'\.setClaimedBiKey\("([A-Za-z0-9_%]+)"\)', before)) if receiver else []
            if claimed:
                contract['claimedKey'] = claimed[-1][1]
            # Payment evidence is scoped to variables of this reward class.
            uses = []
            for consumer in scripts.values():
                variables = re.findall(r'\b(\w+)\s*:\s*' + re.escape(cls) + r'\b', consumer)
                if consumer == reward:
                    variables.append('this')
                for variable in variables:
                    for prop, field in property_fields(reward).items():
                        access = re.escape(variable + '.' + prop)
                        if re.search(r'\.diamondSelf\(\s*' + access + r'\b|ActUtil\.getMyDiamond\(\)\s*[</]\s*' + access + r'\b', consumer):
                            uses.append(field)
            if len(set(uses)) == 1:
                contract['diamondField'] = uses[0]
            contract['dynamicPrice'] = dynamic_price_rules(reward, cls, table, scripts)
            # Resource labels accompany the actual insufficient-balance guard.
            # Keep an activity-scoped currency when no public material ID is given.
            for prop, field in property_fields(reward).items():
                if field not in ('price', 'cost'):
                    continue
                labels = set()
                for consumer in scripts.values():
                    variables = re.findall(r'\b(\w+)\s*:\s*' + re.escape(cls) + r'\b', consumer)
                    for variable in variables:
                        guarded = re.search(re.escape(variable + '.myTokenNum') + r'\s*>=\s*' + re.escape(variable + '.' + prop) +
                            r'[\s\S]{0,100}?"disabledTips"\s*:\s*([\w.]+)\s*\+\s*"不足"', consumer)
                        if guarded and isinstance(constants.get(guarded[1]), str):
                            labels.add(constants[guarded[1]])
                        guarded = re.search(r'model\.myToken\s*<\s*' + re.escape(variable + '.' + prop) + r'\s*\?\s*"([^"\n]+)不足"', consumer)
                        if guarded:
                            labels.add(guarded[1])
                if len(labels) == 1:
                    contract['activityCurrency'] = {'field': field, 'name': labels.pop()}
            if table not in contracts:
                contracts[table] = contract
            elif contracts[table] != contract:
                contracts[table] = {}  # Conflicting consumers are not evidence.
    # Some legacy tables are consumed directly, without ActRewardsData.
    for table in tables:
        direct = re.escape(table) + r'\[[^\]\n]+\]\["(\w+)"\]'
        for source in scripts.values():
            match = re.search(r'(\w+)\s*=\s*int\(\s*' + direct + r'\s*\)', source)
            if match and re.search(r'ActUtil\.getMyDiamond\(\)\s*<\s*' + re.escape(match[1]) + r'\b', source):
                contracts.setdefault(table, {'defaults': {}, 'source': source})['diamondField'] = match[2]
    return contracts


def enrich_reward_facts(item, row, table, contracts, profile):
    contract = contracts.get(table, {})
    source, text = contract.get('source', ''), profile['text']
    values = {**contract.get('defaults', {}), **row}
    evidence = {}
    is_exchange = item['acquisitionKind'] == 'exchange'
    field = contract.get('diamondField')
    price = integer(values.get(field)) if field else None
    if is_exchange and price is not None and price >= 0:
        item['costRaw'][field] = values[field]
        evidence['price'] = {'field': field, 'consumer': contract.get('rewardClass', table), 'currency': '8:2'}
        if price > 0:
            item.update(cost=f'8:2:{price}', costKnown=True, costDescription=f'钻石 ×{price}', exchangeKind='diamond')
        else:
            item.update(cost='', costKnown=False, costDescription='免费领取；需满足活动的前置条件')
    currency = contract.get('activityCurrency', {})
    amount = integer(values.get(currency.get('field')))
    if is_exchange and amount is not None and amount > 0:
        item.update(cost='', costKnown=True, activityCosts=[{'name': currency['name'], 'count': amount}],
                    costDescription=f'{currency["name"]} ×{amount}')
        evidence['price'] = currency
    dynamic = contract.get('dynamicPrice', {})
    if is_exchange and dynamic:
        value = values.get(dynamic['field'])
        prices = [value - offset for offset in dynamic['offsets']] if type(value) is int and 'offsets' in dynamic else value
        if isinstance(prices, list) and len(prices) == dynamic.get('slots', len(dynamic.get('offsets', []))) and all(type(v) is int and v > 0 for v in prices):
            options = []
            for index, price in enumerate(prices):
                option = {'cost': f'8:2:{price}', 'label': f'第 {index + 1} 档'}
                if profile['hasState'] and profile.get('stateReadUnambiguous') and profile.get('command') == '1019_0':
                    option['when'] = {'requestKey': 'state', 'path': dynamic['path'],
                                      'op': 'gte' if index == len(prices) - 1 else 'eq', 'value': index}
                    item['displayIdentityOnly'] = False
                options.append(option)
            item.update(cost='', costKnown=False, priceOptions=options, exchangeKind='diamond',
                        costDescription='钻石价格档位：' + ' / '.join(map(str, prices)) + '；按活动累计购买次数选择')
            evidence['price'] = dynamic
    # ActRewardFragmentLimited.validity is > 0. Zero means the limit is
    # inactive, never zero remaining exchanges.
    limits = []
    for index, (prefix, key, label) in enumerate((('day', 'dl', '日'), ('week', 'wl', '周'),
            ('month', 'ml', '月'), ('period', 'pl', '期'), ('total', 'tl', '总'))):
        field = prefix + '_limited'
        maximum = integer(values.get(field))
        if maximum is None or not contract or f'getFrag("{field}")' not in source:
            continue
        item['quotaRaw'][field] = values[field]
        if maximum > 0:
            limits.append({'maximum': maximum, 'key': key, 'label': label, 'index': index,
                           'serverKey': values.get(field + '_key'), 'field': field})
        elif maximum == 0:
            item['quotaDescription'] = '未设置该项总限次' if prefix == 'total' else f'未设置{label}限次'
    # Custom ActReward classes directly subtract/compare their limit fields.
    for field, label, key, index in [('totalLimit', '总', 'tl', 4), ('limit', '', '', -1),
                                     ('localLimit', '', '', -1), ('selfLimit', '', '', -1)]:
        maximum = integer(values.get(field))
        if maximum is not None and maximum > 0 and source and re.search(r'\["' + field + r'"\]', source):
            limits.append({'maximum': maximum, 'key': key, 'label': label, 'index': index, 'field': field})
    if not limits and contract.get('claimedKey'):
        limits.append({'maximum': 1, 'key': 'tl', 'label': '总', 'index': 4, 'field': 'claimedKey',
                       'serverKey': contract['claimedKey'], 'boolean': True})
    if limits:
        item['quotaDescription'] = '；'.join(f'{p["label"]}限 {p["maximum"]} 次' for p in limits)
        evidence['limits'] = limits
        # Several independent counters cannot be represented by one maximum.
        if len(limits) == 1:
            limit = limits[0]
            item.update(limitCount=limit['maximum'], limitKey=limit['key'], limitLabel=limit['label'],
                        limitIndex=limit['index'], quotaKnown=True, quotaCycleKnown=bool(limit['key']))
            identifier = row.get('serverIndex', row.get('index'))
            if type(identifier) is int and identifier >= 0 and profile['hasState'] and profile.get('stateReadUnambiguous') and profile.get('command') == '1019_0':
                server = limit.get('serverKey')
                path = None
                if isinstance(server, str) and re.fullmatch(r'[A-Za-z0-9_]*(?:%d)?[A-Za-z0-9_]*', server) and server:
                    path = ['b' + str(identifier), server.replace('%d', str(identifier))]
                elif re.search(r'\["b"\s*\+\s*_?serverIndex\]\["(b|l|oo)"\s*\+\s*_?serverIndex\]', source):
                    match = re.search(r'\["b"\s*\+\s*_?serverIndex\]\["(b|l|oo)"\s*\+\s*_?serverIndex\]', source)
                    path = ['b' + str(identifier), match[1] + str(identifier)]
                if path:
                    observation = {'requestKey': 'state', 'path': path, 'valueKind': 'used'}
                    if limit.get('boolean'):
                        observation.update(encoding='boolean-count', missingValue=0)
                    item['quotaObservation'] = observation
                    item['displayIdentityOnly'] = False
    # Shared cultivation has real price definitions, but discounts/vouchers
    # depend on the account and time. Show the exact base and alternatives.
    public = row.get('publicPriceRaw')
    if isinstance(public, dict):
        binding = row.get('publicPriceBinding', '').split('$', 1)
        override = binding[1] if len(binding) == 2 else ''
        candidates = override or public.get('discountPrice', '')
        if isinstance(candidates, str) and re.fullmatch(r'(?:[A-Za-z]+:)?\d+(?:/(?:[A-Za-z]+:)?\d+)*', candidates):
            amounts = list(dict.fromkeys(int(v.split(':')[-1]) for v in candidates.split('/')))
            item['priceOptions'] = [{'cost': f'8:2:{amount}', 'label': '公共养成基础价'} for amount in amounts if amount > 0]
            item.update(cost='', costKnown=False, exchangeKind='diamond',
                        costDescription='钻石基础价：' + ' / '.join(map(str, amounts)) + '；折扣、抵扣及适用条件以养成面板为准')
            item['costRaw']['publicPrice'] = public
            item['costRaw']['binding'] = row.get('publicPriceBinding', '')
            item['unlock'] = '公共养成；按所选精灵当前培养状态判断是否可购买'
            evidence['price'] = {'consumer': 'StrengthenComboPriceDefine', 'binding': row.get('publicPriceBinding', '')}
    # Preserve known progress thresholds instead of a generic placeholder.
    thresholds = [('num', '累计进度'), ('progressNeeded', '累计进度'), ('needUsedScore', '累计消耗积分'),
                  ('day', '签到天数'), ('days', '签到天数'), ('month', '充值月数'), ('daibi', '累计代币')]
    if not is_exchange:
        details = [f'{label}：{integer(row[field])}' for field, label in thresholds
                   if integer(row.get(field)) is not None and integer(row[field]) > 0]
        if details:
            item['costDescription'] = '；'.join(details) + '；达成后领取，当前进度与领取状态以活动内为准'
    if evidence:
        item['source']['tradeFacts'] = evidence
