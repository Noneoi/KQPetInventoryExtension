"""Consumer-backed shared cultivation quotes; no update-time account guesses.

The public catalog retains an ordered price program. Its result is evaluated
against a caller-supplied time and known account facts, never persisted as the
unconditional price of an offer. This module performs no account requests.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from fractions import Fraction
import hashlib
import re
from pathlib import Path

GAME_TZ = timezone(timedelta(hours=8))


def _point(value):
    if not value:
        return ''
    formats = ('%Y%m%d', '%Y%m%d %H:%M:%S', '%Y-%m-%d', '%Y-%m-%d %H:%M:%S')
    for fmt in formats:
        try:
            date = datetime.strptime(value.strip(), fmt)
            if '%H' not in fmt:
                date = date.replace(hour=2)
            return date.replace(tzinfo=GAME_TZ).isoformat()
        except ValueError:
            pass
    raise ValueError('公共养成日期格式待适配：' + value)


def price_condition(raw, *, description_default=True):
    """StrengthenComboConditionUtil's AND grammar, including desc fallback."""
    if not isinstance(raw, str):
        return {'type': 'unknown', 'raw': str(raw)}
    conditions = []
    for part in raw.split('&'):
        name, _, argument = part.partition('$')
        key = name.lower()
        if key == 'date':
            # A range uses compact dates; also accept the util's explicit times.
            point = r'(?:\d{8}|\d{4}-\d{2}-\d{2})(?: \d{2}:\d{2}:\d{2})?'
            match = re.fullmatch(r'(' + point + r')?-(' + point + r')?', argument)
            if argument == '':
                conditions.append({'type': 'always', 'value': True, 'raw': part})
            elif match:
                try:
                    starts, ends = (_point(value) for value in match.groups())
                    if starts and ends and starts >= ends:
                        raise ValueError('empty period')
                    conditions.append({'type': 'date', 'startsAt': starts, 'endsAt': ends, 'raw': part})
                except ValueError:
                    conditions.append({'type': 'unknown', 'raw': part})
            else:
                conditions.append({'type': 'unknown', 'raw': part})
        elif key == 'vip':
            conditions.append({'type': 'vip', 'vipType': 'rmb', 'raw': part})
        elif key == 'item' and re.fullmatch(r'[1-9]\d*:[1-9]\d*', argument):
            item, count = map(int, argument.split(':'))
            conditions.append({'type': 'item', 'materialType': 4, 'id': item, 'count': count, 'raw': part})
        elif key == 'item':
            conditions.append({'type': 'unknown', 'raw': part})
        elif key == 'false':
            conditions.append({'type': 'always', 'value': False, 'raw': part})
        elif description_default:
            # The actual fallback is ConditionByDesc: cType != "false".
            # A descriptive label is not an unverified account requirement.
            conditions.append({'type': 'always', 'value': True, 'raw': part})
        else:
            conditions.append({'type': 'unknown', 'raw': part})
    return {'type': 'all', 'conditions': conditions}


def _raw_static(source, member):
    from public_activity_exchange_updater import LiteralReader, _balanced
    match = re.search(r'\b' + re.escape(member) + r'\s*:\s*\w+\s*=\s*([\[{])', source)
    if not match:
        return None
    try:
        raw, _ = _balanced(source, match.end() - 1)
        return LiteralReader(raw).parse()
    except (ValueError, IndexError):
        return None


def shared_price_context(scripts):
    """Read shared consumers once; callers attach this to every shared row.

    Class names here are public service API types, never event identifiers.
    Unrecognized consumer changes preserve an unresolved rule, not a cheap
    fallback quote. Full source hashes are retained with the context.
    """
    from public_activity_exchange_updater import as_methods
    sources = {Path(name).stem: source for name, source in scripts.items()}
    price = sources.get('StrengthenComboPriceDefine', '')
    overlap = sources.get('StrengthenComboOverlapPrice', '')
    condition = sources.get('StrengthenComboConditionUtil', '')
    contents = sources.get('StrengthenComboContents', '')
    model = sources.get('StrengthenComboModel', '')
    config = sources.get('StrengthenComboConfig', '')
    ticket = sources.get('SCPaymentMultipleWithTicket', '')
    checks = {
        'orderedPrice': all(token in price for token in ('getActivatedCase()', 'this.tagPrice',
            'Math.ceil(_loc1_ * this.curGlobalDiscount)', 'this.discountIndex != -1 || this._overlapPrice.hasUnconditionalPrice()')),
        'orderedOverlap': all(token in overlap for token in ('for each(', 'this._cases', '_loc2_.isMeetChecker()', 'return _loc2_;')),
        'conditionGrammar': all(token in condition for token in ('param1.split("&")', 'param1.split("$")',
            'new StrengthenComboConditionByVip()', 'new StrengthenComboConditionByItem()', 'new StrengthenComboConditionByDate()', 'new StrengthenComboConditionByDesc(param1)')),
        'descriptionFallback': 'this._cType.toLowerCase() != "false"' in sources.get('StrengthenComboConditionByDesc', ''),
        'vipPredicate': '.isRmbVip()' in sources.get('StrengthenComboConditionByVip', ''),
        'itemPredicate': '.quantity >= _loc3_' in sources.get('StrengthenComboConditionByItem', ''),
        'datePredicate': 'StrengthenComboUtil.isInPeriod(param1)' in sources.get('StrengthenComboConditionByDate', '') and
            'isAfter(param1.split("-")[0]) && isBefore(param1.split("-")[1])' in sources.get('StrengthenComboUtil', ''),
        'dateBoundary': all(token in sources.get('DateUtil', '') for token in (
            'getServerTimeInMS() >= parseDate(param1).getTime()',
            'getServerTimeInMS() >= parseDate(param1 + " 02:00:00").getTime()')),
        'globalDiscount': 'StrengthenComboContents.globalDiscount.isOn() ? StrengthenComboContents.globalDiscount.discountRate() : 1' in price and
            'StrengthenComboConditionUtil.checkAllIsAvailable(this._checker)' in sources.get('StrengthenComboDiscount', ''),
        'paymentSelection': all(token in model for token in ('param1.basicContains.length > 1',
            'StrengthenComboConfig.isOnDiscountTicket()', 'new SCPaymentMultipleWithTicket(', 'new StrengthenComboInnerPayment(')),
        'casePredicate': 'StrengthenComboConditionUtil.checkAllIsAvailable(this._checker)' in sources.get('StrengthenComboCaseDefine', ''),
    }
    result = {'schema': 1, 'verified': all(checks.values()), 'checks': checks,
              'unresolved': ['公共价格消费者待核实：' + key for key, valid in checks.items() if not valid],
              'sources': {name: hashlib.sha256(source.encode('utf-8')).hexdigest()
                          for name, source in sorted(sources.items()) if name.startswith(('StrengthenCombo', 'SCPayment')) or name == 'DateUtil'}}
    discount = _raw_static(contents, 'GLOBAL_DISCOUNT')
    raw_cases = _raw_static(contents, 'OVERLAP_CASE_CONFIG')
    cases = {}
    if isinstance(raw_cases, dict):
        for key, value in raw_cases.items():
            args = value.get('$args') if isinstance(value, dict) and value.get('$new') == 'StrengthenComboCaseDefine' else None
            if isinstance(args, list) and len(args) in (2, 3) and args[0] == key and all(isinstance(a, str) for a in args):
                cases[key] = {'checker': args[1], 'getWay': args[2] if len(args) > 2 else ''}
            else:
                result['unresolved'].append('覆盖价格条件定义待适配：' + str(key))
    else:
        result['unresolved'].append('公共覆盖价格表待适配')
    result.update(globalDiscount=discount, cases=cases)
    body = next((body for name, body in as_methods(config) if name == 'isOnDiscountTicket'), '')
    fixed = re.fullmatch(r'\s*\{?\s*return\s+(true|false)\s*;\s*\}?\s*', body)
    checker = re.search(r'return\s+StrengthenComboConditionUtil\.checkAllIsAvailable\("([^"\n]*)"\)\s*;', body)
    if fixed:
        when = {'type': 'always', 'value': fixed[1] == 'true'}
    elif checker:
        when = price_condition(checker[1])
    else:
        when = {'type': 'unknown', 'raw': body.strip()}
    amount_body = next((body for name, body in as_methods(config) if name == 'ticketDeclineMax'), '')
    max_amount = re.search(r'return\s+(\d+)\s*;', amount_body)
    service = re.search(r'\.loadAndInitNormalActivity\("([A-Za-z0-9]+)",', ticket)
    result['payment'] = {'kind': 'diamond', 'ticket': {'when': when, 'minComponents': 2,
        'externalService': service[1] if service else '',
        'maxDeduction': int(max_amount[1]) if max_amount else None,
        'canUseDiamondCoupon': bool(re.search(r'"canUseDiamondCoupon"\s*:\s*true', ticket)),
        'canUseTicket': bool(re.search(r'"canUseTicket"\s*:\s*true', ticket)),
        'selectionRequired': True, 'deductAutomatically': False}}
    if not isinstance(discount, dict) or not isinstance(discount.get('checker'), str):
        result['unresolved'].append('公共全局折扣配置待适配')
    result['verified'] = result['verified'] and not result['unresolved']
    return result


def shared_price_rule(public, binding, context):
    """Compile data only. Never collapse a dated/account rule during update."""
    context = context if isinstance(context, dict) else {}
    unresolved = list(context.get('unresolved', []))
    if not context.get('verified'):
        unresolved.append('公共价格消费者尚未完整验证')
    rule = {'schema': 1, 'kind': 'shared-cultivation', 'currency': '8:2',
            'preset': [], 'overrideCases': [], 'unconditionalOverride': None,
            'componentCount': len(str(public.get('strengthenParams', '')).split('-')),
            'payment': context.get('payment', {}), 'unresolved': unresolved}
    prices = str(public.get('discountPrice', '')).split('/')
    checker = public.get('discountChecker', '')
    checkers = checker.split('/') if isinstance(checker, str) and checker else ['']
    if len(prices) != len(checkers):
        unresolved.append('公共预设价格与生效条件数量不一致')
    for price, check in zip(prices, checkers):
        if re.fullmatch(r'\d+', price):
            rule['preset'].append({'amount': int(price), 'when': price_condition(check), 'raw': check})
        elif price:
            unresolved.append('公共预设价格格式待适配：' + price)
    override = str(binding).partition('$')[2]
    for value in override.split('/') if override else []:
        key, separator, amount = value.partition(':')
        if not separator and re.fullmatch(r'\d+', key):
            if rule['unconditionalOverride'] is None:
                rule['unconditionalOverride'] = int(key)
        elif separator and re.fullmatch(r'\d+', amount):
            definition = context.get('cases', {}).get(key)
            when = price_condition(definition['checker']) if isinstance(definition, dict) and isinstance(definition.get('checker'), str) else {'type': 'unknown', 'raw': key}
            rule['overrideCases'].append({'key': key, 'amount': int(amount), 'when': when})
            if definition is None:
                unresolved.append('覆盖价格缺少对应条件：' + key)
        else:
            unresolved.append('公共覆盖价格格式待适配：' + value)
    raw_discount = context.get('globalDiscount')
    if isinstance(raw_discount, dict):
        try:
            fraction = Fraction(str(raw_discount['value']))
            if fraction <= 0 or fraction > 1:
                raise ValueError('invalid rate')
            rule['globalDiscount'] = {'numerator': fraction.numerator, 'denominator': fraction.denominator,
                                      'when': price_condition(raw_discount.get('checker'))}
        except (KeyError, ValueError, ZeroDivisionError):
            unresolved.append('全局折扣倍率待适配')
    else:
        unresolved.append('缺少公共全局折扣定义')
    rule['unresolved'] = list(dict.fromkeys(unresolved))
    return rule


def evaluate_condition(condition, referenceDate, account=None):
    """Return True / False / None. Missing account facts are never false."""
    account = account or {}
    kind = condition.get('type')
    if kind == 'always':
        return condition.get('value') if type(condition.get('value')) is bool else None
    if kind == 'all':
        values = [evaluate_condition(c, referenceDate, account) for c in condition.get('conditions', [])]
        return False if False in values else None if None in values else True
    if kind == 'vip':
        value = account.get('rmbVip')
        return value if type(value) is bool else None
    if kind == 'item':
        materials = account.get('materials', {})
        key = f'{condition.get("materialType")}:{condition.get("id")}'
        quantity = materials.get(key)
        if type(quantity) is not int:
            return False if account.get('materialsComplete') is True else None
        return quantity >= condition.get('count', 0)
    if kind == 'date':
        try:
            now = datetime.fromisoformat(referenceDate) if isinstance(referenceDate, str) else referenceDate
            if not isinstance(now, datetime) or now.tzinfo is None:
                return None
            start, end = condition.get('startsAt'), condition.get('endsAt')
            return (not start or now >= datetime.fromisoformat(start)) and (not end or now < datetime.fromisoformat(end))
        except (TypeError, ValueError):
            return None
    return None


def _first_active(cases, date, account):
    for case in cases:
        active = evaluate_condition(case.get('when', {}), date, account)
        if active is None:
            return None, None
        if active:
            return True, case.get('amount')
    return False, None


def evaluate_price_rule(rule, referenceDate, account=None):
    """Reference implementation for native runtime quote evaluation.

    Known amount is the diamond quote before any separately chosen ticket or
    coupon. External payment adjustments are explicitly not manufactured here.
    """
    if rule.get('schema') != 1 or rule.get('unresolved'):
        return {'state': 'unknown', 'reason': '价格规则或消费者待核实'}
    account = account or {}
    preset, tag = _first_active(rule.get('preset', []), referenceDate, account)
    unconditional = rule.get('unconditionalOverride')
    if unconditional is not None:
        tag = unconditional
    elif preset is False:
        return {'state': 'offline', 'reason': '当前时间或账号不满足公共养成开放条件'}
    elif preset is None:
        return {'state': 'unknown', 'reason': '公共养成开放条件需要账号资料'}
    selected, amount = _first_active(rule.get('overrideCases', []), referenceDate, account)
    if selected is None:
        return {'state': 'unknown', 'reason': '覆盖价格条件需要账号资料'}
    if selected is False:
        amount = tag
    discount = rule.get('globalDiscount', {})
    discounted = evaluate_condition(discount.get('when', {}), referenceDate, account)
    if discounted is None or type(amount) is not int or amount < 0:
        return {'state': 'unknown', 'reason': '全局折扣或报价需要补充资料'}
    if discounted:
        numerator, denominator = discount.get('numerator'), discount.get('denominator')
        if type(numerator) is not int or type(denominator) is not int or not 0 < numerator <= denominator:
            return {'state': 'unknown', 'reason': '全局折扣倍率无效'}
        amount = (amount * numerator + denominator - 1) // denominator
    payment = rule.get('payment', {})
    ticket = payment.get('ticket', {})
    ticket_active = False if rule.get('componentCount', 0) < ticket.get('minComponents', 2) else evaluate_condition(ticket.get('when', {}), referenceDate, account)
    return {'state': 'known', 'amount': amount, 'cost': f'8:2:{amount}' if amount else '',
            'globalDiscountApplied': discounted, 'ticketAvailable': ticket_active,
            'quoteBeforeOptionalPaymentSelection': ticket_active is not False}


def condition_description(condition):
    kind = condition.get('type')
    if kind == 'always':
        return '无附加条件' if condition.get('value') else '条件关闭'
    if kind == 'all':
        parts = [condition_description(c) for c in condition.get('conditions', [])
                 if not (c.get('type') == 'always' and c.get('value') is True)]
        return '且'.join(parts) if parts else '无附加条件'
    if kind == 'date':
        return (condition.get('startsAt') or '不限开始') + ' 至 ' + (condition.get('endsAt') or '不限结束') + '（结束时刻不含）'
    if kind == 'vip':
        return '充值VIP有效'
    if kind == 'item':
        return f'持有道具 {condition.get("id")} ×{condition.get("count")}'
    return '条件待核实：' + str(condition.get('raw', ''))


def price_rule_description(rule):
    parts = []
    if rule.get('unconditionalOverride') is not None:
        parts.append('覆盖基础价：钻石 ×' + str(rule['unconditionalOverride']))
    elif rule.get('preset'):
        parts.append('预设价（按条件顺序）' + ' / '.join(
            f'{entry["amount"]}钻〔{condition_description(entry["when"])}〕' for entry in rule['preset']))
    if rule.get('overrideCases'):
        parts.append('优先条件价（按条件顺序）' + ' / '.join(
            f'{entry["amount"]}钻〔{condition_description(entry["when"])}〕' for entry in rule['overrideCases']))
    discount = rule.get('globalDiscount')
    if discount:
        parts.append(f'全局倍率 {discount["numerator"]}/{discount["denominator"]}〔{condition_description(discount["when"])}〕，折后向上取整')
    ticket = rule.get('payment', {}).get('ticket', {})
    if ticket:
        parts.append('多项套餐券支付〔' + condition_description(ticket.get('when', {})) + '〕；券及优惠券需在支付面板选择，不自动抵扣')
    if rule.get('unresolved'):
        parts.append('；'.join(rule['unresolved']))
    return '；'.join(parts) or '公共养成价格规则待核实'
