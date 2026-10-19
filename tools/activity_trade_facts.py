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


def cumulative_progress_rule(reward, cls, table, rows, scripts):
    """Resolve table -> ordered model array -> cumulative UI eligibility.

    A literal cost alone says nothing about a debit. Require the consumer's
    prefix sum, zero origin, sequential loop and progress comparison. A final
    direct-purchase branch is deliberately excluded from prefix-sum claims.
    """
    from public_activity_exchange_updater import _balanced, as_methods
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        return {}
    for model_file, model in scripts.items():
        model_class = Path(model_file).stem
        iterator = re.search(re.escape(table) + r'\.forEach\(function\((\w+):[^)]*\)\s*:[^{]+\{', model)
        if not iterator:
            continue
        init, _ = _balanced(model, iterator.end() - 1)
        push = re.search(r'(?:this\.)?(\w+)\.push\(new\s+' + re.escape(cls) + r'\(\)\.initConfig\(' + re.escape(iterator[1]) + r'\)', init)
        getter = re.search(r'function\s+get\s+(\w+)\(\)\s*:\s*Array\s*\{\s*return\s+this\.' + re.escape(push[1]) + r'\s*;', model) if push else None
        if not getter:
            continue
        for filename, consumer in scripts.items():
            model_vars = re.findall(r'\b(\w+)\s*:\s*' + re.escape(model_class) + r'\b', consumer)
            for model_var in model_vars:
                array = r'(?:this\.)?' + re.escape(model_var + '.' + getter[1])
                for method, body in as_methods(consumer):
                    for loop in re.finditer(r'while\(\s*(\w+)\s*<\s*' + array + r'\.length\s*\)\s*\{', body):
                        loop_body, _ = _balanced(body, loop.end() - 1)
                        index = loop[1]
                        if not re.search(re.escape(index) + r'\s*(?::\s*[\w*]+)?\s*=\s*0\s*;', body[:loop.start()]) or not re.search(re.escape(index) + r'\+\+\s*;', loop_body):
                            continue
                        entry = re.search(r'\b(\w+)\s*=\s*' + array + r'\[\s*' + re.escape(index) + r'\s*\]\s*;', loop_body)
                        accumulation = re.search(r'\b(\w+)\s*\+=\s*' + re.escape(entry[1]) + r'\.(\w+)\s*;', loop_body) if entry else None
                        if not accumulation:
                            continue
                        total, prop = accumulation.groups()
                        field = property_fields(reward).get(prop)
                        if not field or not re.search(re.escape(total) + r'\s*(?::\s*[\w*]+)?\s*=\s*0\s*;', body[:loop.start()]):
                            continue
                        compare = re.search(r'\b(\w+)\s*>=\s*' + re.escape(total) + r'\b', loop_body)
                        if not compare:
                            continue
                        progress = re.search(re.escape(compare[1]) + r'\s*(?::\s*\w+)?\s*=\s*(?:this\.)?' + re.escape(model_var) + r'\.(\w+)\s*;', body[:loop.start()])
                        if not progress:
                            continue
                        progress_getter = re.search(r'function\s+get\s+' + re.escape(progress[1]) + r'\(\)\s*:\s*int\s*\{\s*return\s+this\.(\w+)\s*;', model)
                        state = re.search(r'this\.' + re.escape(progress_getter[1]) + r'\s*=\s*\w+\["(\w+)"\]\s*;', model) if progress_getter else None
                        if not state:
                            continue
                        excluded = set()
                        special = re.search(r'if\(\s*' + re.escape(index) + r'\s*==\s*' + array + r'\.length\s*-\s*1\s*\)\s*\{', loop_body)
                        if special and special.start() < compare.start():
                            special_body, _ = _balanced(loop_body, special.end() - 1)
                            if re.search(r'\bbreak\s*;', special_body):
                                excluded.add(len(rows) - 1)
                        thresholds, amount = [], 0
                        for position, row in enumerate(rows):
                            increment = integer(row.get(field))
                            if increment is None or increment < 0:
                                thresholds = []
                                break
                            amount += increment
                            if position not in excluded and not row.get('_linkedPurchase'):
                                identity = {key: row[key] for key in ('serverIndex', 'index') if type(row.get(key)) is int}
                                if not identity:
                                    thresholds = []
                                    break
                                thresholds.append({'identity': identity, 'amount': amount, 'increment': increment})
                        if thresholds:
                            return {'aggregation': 'prefix-sum', 'field': field, 'thresholds': thresholds,
                                    'progressProperty': progress[1], 'statePath': [state[1]],
                                    'consumer': Path(filename).stem + '.' + method,
                                    'model': model_class + '.' + getter[1], 'label': '累计进度'}
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
            r'([\w.]+|new\s+ActRewardsData\(\))\.init\(\s*([\w.]+)(?:\(\s*\))?\s*,\s*\w+\s*,\s*(\w+)\s*\)', source)]
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
            # A boolean claim state needs a real getter, server assignment,
            # negative UI gate and a typed consumer marking it after claiming.
            linked = tables[table][1]
            needs_claim = isinstance(linked, list) and any(isinstance(row, dict) and row.get('_linkedPurchase') for row in linked)
            for getter in re.finditer(r'function\s+get\s+(\w+)\(\)\s*:\s*Boolean\s*\{\s*return\s+this\.(\w+)\s*;\s*\}', reward if needs_claim else ''):
                prop, member = getter.groups()
                state = re.search(r'this\.' + re.escape(member) +
                    r'\s*=\s*\w+\["([A-Za-z]+)"\s*\+\s*_?serverIndex\]\["([A-Za-z]+)"\s*\+\s*_?serverIndex\]\s*;', reward)
                if not state:
                    continue
                gated, marked = False, False
                for consumer in scripts.values():
                    variables = re.findall(r'\b(\w+)\s*:\s*' + re.escape(cls) + r'\b', consumer)
                    for variable in variables:
                        access = re.escape(variable + '.' + prop)
                        gated |= bool(re.search(r'!\s*' + access + r'\b', consumer))
                        marked |= bool(re.search(r'\b' + access + r'\s*=\s*true\s*;', consumer))
                if gated and marked:
                    contract['booleanClaim'] = {'property': prop, 'prefixes': list(state.groups())}
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
            contract['cumulativeProgress'] = cumulative_progress_rule(reward, cls, table, tables[table][1], scripts)
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
    # Compile the official ordered program, keeping date/account predicates.
    # Runtime must evaluate it against current facts, not an update-time price.
    public = row.get('publicPriceRaw')
    if isinstance(public, dict):
        from activity_price_rules import shared_price_rule, price_rule_description
        binding = row.get('publicPriceBinding', '')
        rule = shared_price_rule(public, binding, row.get('publicPriceContext'))
        item.update(cost='', costKnown=False, exchangeKind='diamond', priceRule=rule,
                    costDescription=price_rule_description(rule))
        item.pop('priceOptions', None)  # Legacy alternatives lack condition order.
        item['costRaw']['publicPrice'] = public
        item['costRaw']['binding'] = binding
        item['unlock'] = '公共养成；按所选精灵当前培养状态、报价条件与支付方式判断'
        evidence['price'] = {'consumer': 'StrengthenComboPriceDefine.currentPrice', 'binding': binding,
                             'ruleSchema': 1, 'unresolved': rule['unresolved']}
    cumulative = contract.get('cumulativeProgress', {})
    for threshold in cumulative.get('thresholds', []):
        if all(row.get(key) == value for key, value in threshold['identity'].items()) and not row.get('_linkedPurchase'):
            requirement = {key: value for key, value in cumulative.items() if key != 'thresholds'}
            requirement.update(amount=threshold['amount'], increment=threshold['increment'])
            item.update(acquisitionKind='progress', exchangeKind='activity', cost='', costKnown=False,
                        manualSelectionRequired=True,
                        progressRequirement=requirement,
                        costDescription=f'累计进度门槛：{threshold["amount"]}；按前序各档累加，达成后领取，不是本次扣费',
                        unlock=f'累计进度达到 {threshold["amount"]} 且尚未领取')
            item.pop('priceOptions', None)
            evidence['progress'] = requirement
            break
    # Preserve known progress thresholds instead of a generic placeholder.
    thresholds = [('num', '累计进度'), ('progressNeeded', '累计进度'), ('needUsedScore', '累计消耗积分'),
                  ('day', '签到天数'), ('days', '签到天数'), ('month', '充值月数'), ('daibi', '累计代币')]
    if not is_exchange and not item.get('progressRequirement'):
        details = [f'{label}：{integer(row[field])}' for field, label in thresholds
                   if integer(row.get(field)) is not None and integer(row[field]) > 0]
        if details:
            item['costDescription'] = '；'.join(details) + '；达成后领取，当前进度与领取状态以活动内为准'
    purchase = row.get('_linkedPurchase')
    if isinstance(purchase, dict):
        # forNormal goes to the web payment dialog. Its integer price is not a
        # MaterialData amount and the reward row's cost can be a charge tier.
        item.update(cost='', costKnown=False, exchangeKind='activity',
                    costDescription='商城付费购买后领取；支付金额以商城结算为准',
                    unlock='购买后激活领取资格')
        item['costRaw']['externalPurchase'] = purchase
        evidence['purchase'] = purchase
        claim = contract.get('booleanClaim')
        if claim:
            identifier = row.get('serverIndex', row.get('index'))
            item.update(limitCount=1, limitKey='', limitLabel='领取', limitIndex=-1,
                        quotaKnown=True, quotaCycleKnown=False,
                        quotaDescription='领取限 1 次；当前购买及领取状态以活动内为准')
            item['unlock'] += '；已领取的奖励不能重复领取'
            item['quotaRaw']['claimOnce'] = claim
            evidence['claim'] = {**claim, 'maximum': 1}
            if type(identifier) is int and identifier >= 0:
                evidence['claim']['statePath'] = [prefix + str(identifier) for prefix in claim['prefixes']]
    service = row.get('_serviceTradeFacts')
    if isinstance(service, dict):
        currency = service.get('activityCurrency', {})
        amount = integer(currency.get('count'))
        if isinstance(currency.get('name'), str) and currency['name'] and amount is not None and amount > 0:
            item.update(cost='', costKnown=True, exchangeKind='activity',
                        activityCosts=[{'name': currency['name'], 'count': amount}],
                        costDescription=f'{currency["name"]} ×{amount}')
            evidence['price'] = currency
        limit = service.get('limit', {})
        maximum = integer(limit.get('maximum'))
        if maximum is not None and maximum > 0:
            item.update(limitCount=maximum, limitKey='', limitLabel='活动', limitIndex=-1,
                        quotaKnown=True, quotaCycleKnown=False,
                        quotaDescription=f'限 {maximum} 次；当前已用次数与重置周期以活动内为准')
            evidence['limits'] = [limit]
        claim = service.get('claim', {})
        if claim.get('once') is True:
            item.update(limitCount=1, limitKey='', limitLabel='领取', limitIndex=-1,
                        quotaKnown=True, quotaCycleKnown=False,
                        quotaDescription='领取限 1 次；当前领取状态以活动内为准')
            evidence['claim'] = claim
        entries = service.get('entryConditions', [])
        descriptions = list(dict.fromkeys(control['description'] for entry in entries
            for control in entry.get('controls', []) if isinstance(control.get('description'), str) and control['description']))
        if entries:
            incomplete = any(control.get('descriptionComplete') is False for entry in entries for control in entry.get('controls', []))
            notices = ['按钮资格提示：“' + description + '”' for description in descriptions]
            if incomplete:
                notices.append('按钮资格提示尚未完整解析')
            item['unlock'] = '；'.join(notices + ['还需满足前置入口条件，当前账号资格尚未确认', '已领取的奖励不可重复领取'])
            evidence['entryConditions'] = entries
        if service.get('payment'):
            evidence['payment'] = service['payment']
        entitlement = service.get('purchaseEntitlement')
        if isinstance(entitlement, dict) and entitlement.get('kind') == 'previous-purchase':
            item.update(cost='', costKnown=False, exchangeKind='activity',
                        costDescription='已购养成套餐的领取阶段；需先取得对应购买资格，当前资格和领取状态以活动内为准')
            item['unlock'] = '需先取得对应养成套餐的购买资格；' + item.get('unlock', '')
            evidence['purchaseEntitlement'] = entitlement
        if isinstance(service.get('compensation'), str):
            evidence['compensation'] = service['compensation']
        # There is no read-only protocol contract here. Do not synthesize a
        # live balance, claim flag, or quota observation from a write callback.
        item['displayIdentityOnly'] = True
    if evidence:
        item['source']['tradeFacts'] = evidence
