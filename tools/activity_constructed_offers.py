"""Connect typed offer tables to their actual display and payment consumers.

This is a bounded static data-flow adapter, not an ActionScript interpreter.
Class names, field names, row IDs and species are discovered from consumers.
"""
from __future__ import annotations

import copy
from pathlib import Path
import re


def constructed_offer_tables(scripts, tables, constants, issue):
    from activity_reward_structures import explicit_races, strengthen, symbolic
    from public_activity_exchange_updater import as_methods, LiteralReader, _arguments, _balanced

    result = copy.deepcopy(tables)
    methods = [(filename, method, body) for filename, text in scripts.items()
               for method, body in as_methods(text)]

    def literal(raw, cls):
        value = LiteralReader(raw).parse()
        def resolve(value):
            if isinstance(value, dict) and set(value) == {'$ref'}:
                return constants.get(value['$ref'], constants.get(cls + '.' + value['$ref'], value))
            if isinstance(value, list):
                return [resolve(v) for v in value]
            return value
        return resolve(value)

    for config_file, lookup, lookup_body in methods:
        cls = Path(config_file).stem
        # A row returned only when its identifier equals the lookup argument.
        match = re.search(r'(\w+)\s*=\s*(\w+)\[\w+\]\s*;\s*if\s*\(\1\.(\w+)\s*==\s*param1\)\s*\{\s*return\s+\1\s*;', lookup_body)
        if not match:
            continue
        variable, array, id_field = match.groups()
        table = cls + '.' + array
        rows = constants.get(table)
        if not isinstance(rows, list) or not rows or any(not isinstance(row, dict) or symbolic(row) for row in rows):
            continue
        table_type = re.search(r'\b' + re.escape(array) + r'\s*:\s*Array\s*=\s*\[\s*new\s+(\w+)\(', scripts[config_file])
        if not table_type:
            continue
        row_type = table_type[1]
        # A typed navigation/tab table is not an offer table. Establish a
        # purchase consumer before diagnosing its identifiers as reward IDs.
        consumed = False
        for _, _, candidate_body in methods:
            alias = re.search(r'var\s+(\w+)\s*:\s*' + re.escape(row_type) + r'\s*=\s*param\d+\s*;', candidate_body)
            if alias and re.search(r'StrengthenComboItem\.createByString\(\s*' + re.escape(alias[1]) + r'\.', candidate_body):
                consumed = True
                break
        if not consumed:
            continue
        row_map = {row.get(id_field): row for row in rows if type(row.get(id_field)) is int}
        if len(row_map) != len(rows):
            issue(table, '构造器商品表的标识无效或重复')
            continue
        lookup_name = cls + '.' + lookup
        bindings = []
        for filename, buy_method, body in methods:
            definition = re.search(r'var\s+(\w+)\s*:\s*' + re.escape(row_type) + r'\s*=\s*param\d+\s*;', body)
            if not definition:
                continue
            definition = definition[1]
            create = re.search(r'(\w+)\s*:\s*StrengthenComboItem\s*=\s*StrengthenComboItem\.createByString\(\s*' + re.escape(definition) + r'\.(\w+)\s*\)', body)
            if not create:
                continue
            panel = re.search(r'\.showStrengthenPanel\s*\(', body)
            if not panel:
                continue
            try:
                call, _ = _balanced(body, panel.end() - 1)
                args = _arguments(call[1:-1])
                races = explicit_races(literal(args[2], Path(filename).stem))
            except (ValueError, IndexError):
                continue
            if len(args) < 3 or args[0] != create[1] or not races or not args[1].startswith('StrengthenComboPaymentFactory.getSimple('):
                continue
            # The click handler must select IDs from an actual static list,
            # resolve that same typed row, and pass it into this purchase path.
            selections = []
            for _, _, click_body in methods:
                selection = re.search(r'(\w+)\s*(?::\s*int)?\s*=\s*int\(' + re.escape(cls) + r'\.(\w+)\[\w+\]\)\s*;', click_body)
                if not selection:
                    continue
                selected = re.search(r'(\w+)\s*:\s*' + re.escape(row_type) + r'\s*=\s*' + re.escape(lookup_name) + r'\(' + re.escape(selection[1]) + r'\)\s*;', click_body)
                if not selected or not re.search(r'\.' + re.escape(buy_method) + r'\([\s\S]*?,\s*' + re.escape(selected[1]) + r'\s*\)', click_body):
                    continue
                ids = constants.get(cls + '.' + selection[2])
                if isinstance(ids, list) and all(type(v) is int and v in row_map for v in ids) and len(set(ids)) == len(ids):
                    selections.append(ids)
            if not selections or any(ids != selections[0] for ids in selections):
                issue(table, '培养购买入口没有唯一可验证的界面项目列表')
                continue
            # A typed confirmation method consumes the same definition and
            # checks its diamond price before passing its ID to the client.
            payments = []
            for pay_file, pay_method, pay_body in methods:
                if pay_file != filename or not re.search(r'\b' + re.escape(pay_method) + r'\(', body):
                    continue
                alias = re.search(r'var\s+(\w+)\s*:\s*' + re.escape(row_type) + r'\s*=\s*param1\s*;', pay_body)
                if not alias:
                    continue
                price = re.search(r'ActUtil\.checkEnoughtMoney\(' + re.escape(alias[1]) + r'\.(\w+)\)', pay_body)
                if price and re.search(r'\b' + re.escape(alias[1] + '.' + id_field) + r'\b', pay_body):
                    payments.append(price[1])
            if len(set(payments)) != 1:
                issue(table, '培养商品的钻石扣费字段无法从确认路径唯一关联')
                continue
            bindings.append((selections[0], create[2], races, payments[0], body, definition, filename, buy_method))
        if not bindings:
            continue
        if len(bindings) != 1:
            issue(table, '构造器商品存在多个不同购买路径')
            continue
        ids, code_field, races, price_field, body, definition, filename, method = bindings[0]
        conditional = re.search(r'if\(Boolean\((\w+)\.(\w+)\)\)', body)
        variant = None
        if conditional:
            target, flag = conditional.groups()
            substitute = re.search(re.escape(target) + r'\s*=\s*' + re.escape(lookup_name) + r'\(' + re.escape(definition) + r'\.(\w+)\)', body)
            needs = re.search(r'(\w+)\s*=\s*\(forSvc\(ISacredEquipmentService\)[\s\S]*?\.getStageUpgradeMaterial\((\w+)\.defineId,\2\.stageUpgradePlanId,\2\.stageLevel\)\.some\(function\(param1:String,\s*\.\.\.\s*rest\):Boolean\s*\{\s*return\s+new MaterialCommonData\(param1\)\.type\s*==\s*MaterialTypes\.EQUIPMENT4PET_ITEM;\s*\}\);', body)
            if substitute and needs and re.search(r'if\(!' + re.escape(needs[1]) + r'\)\s*\{\s*' + re.escape(target) + r'\s*=', body):
                variant = flag, substitute[1]
            else:
                issue(table, '培养条件替代路径尚未完整识别')
                continue
        normalized = []
        for identifier in ids:
            row = row_map[identifier]
            amount = row.get(price_field)
            try:
                if type(amount) is not int or amount <= 0:
                    raise ValueError('培养商品价格不是明确的正数')
                good = strengthen({**row, 'serverId': identifier, '_acquisitionKind': 'exchange',
                    'cost': f'8:2:{amount}',
                    '_callEvidence': {'consumer': 'IStrengthenComboService.showStrengthenPanel',
                                      'file': filename, 'method': method, 'displayIds': ids}},
                    row.get(code_field), races, structure='constructed-cultivation')
                if variant and row.get(variant[0]):
                    other = row_map.get(row.get(variant[1]))
                    if not isinstance(other, dict) or type(other.get(price_field)) is not int or other[price_field] <= 0:
                        raise ValueError('培养条件替代项目缺少有效价格或目标')
                    # Validate both effects, keep the alternate inside the
                    # visible item; it is not a second selectable store row.
                    alternate = strengthen({}, other.get(code_field), races, structure='conditional-cultivation')
                    good['cost'] = ''
                    good['_offerPriceRule'] = {'schema': 1, 'kind': 'constructed-cultivation', 'currency': '8:2',
                        'cases': [{'amount': amount, 'when': {'type': 'selected-pet-sacred-stage-material', 'requiresEquipment': True},
                                   'rewardCode': row[code_field], 'itemId': identifier},
                                  {'amount': other[price_field], 'when': {'type': 'selected-pet-sacred-stage-material', 'requiresEquipment': False},
                                   'rewardCode': alternate['_typedCodes'], 'itemId': other[id_field]}]}
                normalized.append(good)
            except ValueError as error:
                issue(table, str(error), row)
        result[table] = config_file, normalized
    return result
