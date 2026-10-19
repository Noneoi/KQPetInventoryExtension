"""Extract literal reward facts from verified consumers, without running AS.

Names, dates, species and reward values are data. Only the public service call
and a consumed static map establish the interpretation of those data.
"""
from __future__ import annotations

import copy
from pathlib import Path
import re


def literal_enum_constants(sources):
    """Read enum properties assigned directly from literal factory arguments."""
    from public_activity_exchange_updater import LiteralReader, _arguments, _balanced
    constants = {}
    for filename, source in sources.items():
        cls = Path(filename).stem
        getters = {m[2]: m[1] for m in re.finditer(
            r'function\s+get\s+(\w+)\(\)\s*:\s*\w+\s*\{\s*return\s+this\.(\w+)\s*;', source)}
        for factory in re.finditer(r'static\s+function\s+(\w+)\(([^)]*)\)\s*:\s*' + re.escape(cls) + r'\s*\{', source):
            body, _ = _balanced(source, factory.end() - 1)
            created = re.search(r'\b(\w+)\s*=\s*new\s+' + re.escape(cls) + r'\(\s*\)', body)
            if not created or not re.search(r'return\s+' + re.escape(created[1]) + r'\s*;', body):
                continue
            parameters = [p.split(':', 1)[0].strip() for p in _arguments(factory[2])]
            fields = {}
            for assignment in re.finditer(re.escape(created[1]) + r'\.(\w+)\s*=\s*(\w+)\s*;', body):
                if assignment[1] in getters and assignment[2] in parameters:
                    if len(re.findall(re.escape(created[1] + '.' + assignment[1]) + r'\s*=', body)) == 1:
                        fields[getters[assignment[1]]] = parameters.index(assignment[2])
            if not fields:
                continue
            for declaration in re.finditer(r'\bconst\s+(\w+)\s*:\s*' + re.escape(cls) + r'\s*=\s*' + re.escape(factory[1]) + r'\s*\(', source):
                try:
                    encoded, _ = _balanced(source, declaration.end() - 1)
                    args = [LiteralReader(value).parse() for value in _arguments(encoded[1:-1])]
                    values = {name: args[index] for name, index in fields.items()
                              if index < len(args) and type(args[index]) in (str, int, bool)}
                    if values:
                        constants[cls + '.' + declaration[1]] = values
                except (ValueError, IndexError):
                    continue
    return constants


def verify_link_contracts(sources):
    from activity_evolution_selector import _method
    from public_activity_exchange_updater import LiteralReader, _arguments, _balanced
    from public_routine_updater import _declaration

    def compact(value):
        return re.sub(r'\s+', '', value)

    def matches(cls, method, expected):
        try:
            return compact(_method(sources.get(cls, ''), method)) == compact(expected)
        except ValueError:
            return False

    direct = matches('DirectBuyInfo', 'forNormal', '''{
        if(param5!=-1){this.extArg.ci=param5;} if(param6!=-1){this.extArg.bi=param6;}
        this.goodsName=param2; this.goodsType=DirectBuyInfo.GT_NORMAL;
        this.goodsId=param1; this.goodsPrice=param3; this.goodsNum=param4; return this;
    }''') and matches('WealthManagerService', 'buyDirectly', '''{
        JsProxy.getValue("showPayDialog",null,param1.toWebObj());
    }''')
    panel = matches('StrengthenComboModel', 'setPrimeRaces', '''{
        var _loc2_:int=0; var _loc3_:Array=null;
        if(param1.length==1){
            _loc2_=StrengthenComboUtil.findOriginal(param1[0]);
            if(StrengthenComboContents.getPetContentByOriginalRace(_loc2_)==null){
                _loc3_=(forSvc(IPetDataService) as IPetDataService).getPetOrderEvoLink(_loc2_);
                _loc2_=int(_loc3_[_loc3_.length-1]);
            }
            this._primeRace=[_loc2_];
        }else{this._primeRace=param1.concat();}
        this._allAvailableRace=StrengthenComboContents.getAllPetRaces(this._primeRace);
    }''') and matches('StrengthenComboUtil', 'findOriginal', '''{
        var _loc2_:int=param1;
        var _loc3_:IChangePetSkinService=forSvc(IChangePetSkinService);
        var _loc4_:Array=_loc3_.getSwitchSkinPetRaceIds(_loc2_);
        if(_loc4_.length>0){_loc2_=int(_loc4_[0]);}
        var _loc5_:IMultipleFormPetService=forSvc(IMultipleFormPetService);
        var _loc6_:Array=_loc5_.getSwitchPetRaceIds(_loc2_);
        if(_loc6_.length>0){_loc2_=int(_loc6_[0]);} return _loc2_;
    }''')
    redirects = []
    try:
        table = _declaration(sources.get('StrengthenComboRedirection', ''), 'ObjRedirectionPetData', '{')
        for entry in _arguments(table[1:-1]):
            if not entry.strip():
                continue
            match = re.match(r'"([1-9]\d*)"\s*:\s*new\s+StrengthenComboRedirectionPetDefine\s*', entry)
            if not match:
                raise ValueError('unknown redirection')
            call, end = _balanced(entry, match.end())
            args = [LiteralReader(a).parse() for a in _arguments(call[1:-1])]
            if entry[end:].strip() or len(args) < 3 or args[0] != int(match[1]):
                raise ValueError('unknown redirection')
            redirects.append(args[0])
    except (ValueError, IndexError):
        panel = False
    return {'directBuyVerified': direct, 'sharedPanelRuleVerified': panel,
            'publicEnumConstants': literal_enum_constants(sources),
            'sharedRedirectRaces': sorted(set(redirects))}


def linked_purchase_tables(scripts, tables, constants, issue, support=None):
    """Join a consumed DirectBuyInfo to the row selected by its branch.

    The purchase config is not another reward. A matching number/name alone
    never proves the join, and an external payment is not a diamond cost.
    """
    from public_activity_exchange_updater import LiteralReader, _arguments, _balanced, as_methods
    result = copy.deepcopy(tables)
    proposals = {}
    if not (support or {}).get('directBuyVerified'):
        return result
    for filename, source in scripts.items():
        if 'DirectBuyInfo' not in source or '.buyDirectly' not in source:
            continue
        for _, method in as_methods(source):
            guards = []
            for guard in re.finditer(r'\bif\s*\(\s*(\w+)\s*==\s*([\w.]+)\.length\s*-\s*([1-9]\d*)\s*\)\s*\{', method):
                body, end = _balanced(method, guard.end() - 1)
                guards.append((guard, body, end))
            for guard, body, end in guards:
                selector, table, offset = guard[1], guard[2], int(guard[3])
                rows = result.get(table, ('', None))[1]
                if not isinstance(rows, list) or not 1 <= offset <= len(rows):
                    continue
                row_index = len(rows) - offset
                if not isinstance(rows[row_index], dict):
                    continue
                before = method[:guard.start()]
                # The same selected table must supply the UI reward, not just
                # happen to have the same length as an unrelated list.
                if not re.search(r'\bvar\s+\w+\s*:\s*[\w.*]+\s*=\s*' + re.escape(table) +
                                 r'\[\s*' + re.escape(selector) + r'\s*\]\s*;', before):
                    continue
                for call in re.finditer(r'\b(\w+)\s*=\s*new\s+DirectBuyInfo\(\)\.forMyself\(\)\.forNormal\s*\(', body):
                    encoded, call_end = _balanced(body, call.end() - 1)
                    args = _arguments(encoded[1:-1])
                    info = call[1]
                    consumed = re.search(r'\b\w+\.buyDirectly\(\s*' + re.escape(info) + r'\s*\)', body[call_end:])
                    if len(args) < 4 or not consumed:
                        continue
                    between = body[call_end:call_end + consumed.start()]
                    if re.search(r'\b' + re.escape(info) + r'(?:\.|\[|\s*=(?!=))', between):
                        continue
                    accesses = [re.fullmatch(r'([\w.]+)\["(\w+)"\]', value.strip()) for value in args[:3]]
                    if any(m is None for m in accesses) or len({m[1] for m in accesses}) != 1:
                        continue
                    owner = accesses[0][1]
                    bindings = list(re.finditer(r'\bvar\s+' + re.escape(owner) + r'\s*:\s*[\w.*]+\s*=\s*([\w.]+)\s*;', before))
                    config_key = owner
                    if bindings:
                        binding = bindings[-1]
                        config_key = binding[1]
                        if re.search(r'\b' + re.escape(owner) + r'(?:\.[\w]+|\[[^]]+\])?\s*=(?!=)', before[binding.end():] + body[:call.start()]):
                            continue
                    config = constants.get(config_key)
                    if not isinstance(config, dict):
                        continue
                    if any(re.search(re.escape(config_key) + r'(?:\.\w+|\[[^]]+\])\s*=(?!=)', text)
                           for text in scripts.values()):
                        continue
                    goods_id, label, price = (config.get(m[2]) for m in accesses)
                    try:
                        quantity = LiteralReader(args[3].strip()).parse()
                    except (ValueError, IndexError):
                        continue
                    if (type(goods_id) is not int or goods_id <= 0 or not isinstance(label, str) or not label or
                            type(price) is not int or price <= 0 or type(quantity) is not int or quantity <= 0):
                        continue
                    row = rows[row_index]
                    if not isinstance(row.get('rewardParams'), str) or not (
                            row.get('rewardType') == 'Strengthen' or row['rewardParams'].startswith('Strengthen:')):
                        continue
                    evidence = {'consumer': 'DirectBuyInfo.forNormal -> IWealthManagerService.buyDirectly',
                                'configTable': config_key, 'rewardTable': table, 'rowIndex': row_index,
                                'selection': f'{selector} == {table}.length - {offset}',
                                'goodsId': goods_id, 'goodsName': label, 'goodsPrice': price,
                                'goodsNum': quantity, 'paymentKind': 'external', 'configFile': filename}
                    proposals.setdefault((table, row_index), []).append(evidence)
    for (table, index), candidates in proposals.items():
        if any(candidate != candidates[0] for candidate in candidates[1:]):
            issue(table, '同一奖励关联了不同商城购买入口，保留待核对状态', result[table][1][index])
            continue
        evidence = candidates[0]
        result[table][1][index]['_linkedPurchase'] = evidence
        # Retain the original reward's identity and source. Keep the consumed
        # config in evidence, instead of emitting a duplicate pseudo-offer.
        result.pop(evidence['configTable'], None)
    return result


def call_reward_tables(scripts, tables, constants, issue, support=None):
    from public_activity_exchange_updater import LiteralReader, _arguments, _balanced
    from activity_reward_structures import CODE, enhancement_description, explicit_races, strengthen, symbolic

    result = copy.deepcopy(tables)
    constants = {**(support or {}).get('publicEnumConstants', {}), **constants}
    all_text = '\n'.join(scripts.values())

    def resolve(value, namespace, bindings=None, seen=()):
        bindings = bindings or {}
        if len(seen) > 32:
            return {'$ref': 'reference-depth'}
        if isinstance(value, list):
            return [resolve(v, namespace, bindings, seen) for v in value]
        if not isinstance(value, dict):
            return value
        if set(value) == {'$ref'}:
            key = value['$ref']
            if key in bindings:
                return bindings[key]
            # A public enum alias can expose a getter-backed literal field,
            # e.g. Config.RANGE.year -> YearPayRange.THE_2026.year.
            parts = key.split('.')
            for length in range(len(parts) - 1, 0, -1):
                prefix = '.'.join(parts[:length])
                if prefix not in constants and prefix not in bindings:
                    continue
                base = resolve({'$ref': prefix}, namespace, bindings, (*seen, key))
                for field in parts[length:]:
                    if not isinstance(base, dict) or field not in base:
                        break
                    base = base[field]
                else:
                    return resolve(base, namespace, bindings, (*seen, key))
            key = key if key in constants else namespace + '.' + key
            return resolve(constants[key], namespace, bindings, (*seen, key)) if key in constants and key not in seen else value
        if set(value) == {'$index', '$key'}:
            container = resolve(value['$index'], namespace, bindings, seen)
            key = resolve(value['$key'], namespace, bindings, seen)
            if isinstance(container, dict) and isinstance(key, str) and key in container:
                return resolve(container[key], namespace, bindings, seen)
            if isinstance(container, list) and type(key) is int and 0 <= key < len(container):
                return resolve(container[key], namespace, bindings, seen)
            return value
        return {k: resolve(v, namespace, bindings, seen) for k, v in value.items()}

    def expression(raw, namespace, bindings=None):
        try:
            return resolve(LiteralReader(raw.strip()).parse(), namespace, bindings)
        except (ValueError, IndexError):
            return {'$ref': raw}

    def methods(source):
        for match in re.finditer(r'\bfunction\s+(\w+)\s*\([^)]*\)\s*:\s*[\w.*<>]+\s*\{', source):
            try:
                body, end = _balanced(source, match.end() - 1)
                yield match[1], match.start(), end, body
            except ValueError:
                continue

    def scopes(source, at):
        # Ignore braces inside quoted strings/comments. A declaration in a
        # completed sibling callback/branch cannot supply the active call.
        stack = []
        for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', source[:at]):
            if token[0] == '{':
                stack.append(token.start())
            elif token[0] == '}' and stack:
                stack.pop()
        return tuple(stack)

    def local_bindings(source, begin, at, namespace):
        prefix = source[begin:at]
        call_scope = scopes(prefix, len(prefix))
        declarations = []
        for declaration in re.finditer(r'\b(?:var|const)\s+(\w+)\s*:\s*[\w.*<>]+\s*=\s*([^;]+);', prefix):
            scope = scopes(prefix, declaration.start())
            if call_scope[:len(scope)] == scope:
                declarations.append(declaration)
        bindings = {}
        for declaration in declarations:
            name, raw = declaration[1], declaration[2].strip()
            tail = prefix[declaration.end():]
            # No speculative branch execution or array mutations. Aliases
            # inherit unknown values through expression(), never stale copies.
            if re.search(r'\b' + re.escape(name) + r'\s*(?:=(?!=)|\[[^]]*\]\s*=|\.(?:push|pop|shift|unshift|splice|sort|reverse)\s*\()', tail):
                bindings[name] = {'$ref': 'mutated-local:' + name}
                continue
            parsed = item_constructor(raw, namespace, bindings)
            bindings[name] = parsed if parsed else expression(raw, namespace, bindings)
        return bindings

    def item_constructor(raw, namespace, bindings):
        constructor = re.match(r'StrengthenComboItem\.createByString\s*\(', raw.strip())
        if not constructor:
            return None
        construction, end = _balanced(raw.strip(), constructor.end() - 1)
        if raw.strip()[end:].strip() not in ('', '.autoFillDisplayType()'):
            return None
        contents = _arguments(construction[1:-1])
        if not contents:
            return None
        return {'$strengthenItem': expression(contents[0], namespace, bindings),
                'effectExpression': contents[0].strip(),
                'compensation': expression(contents[1], namespace, bindings) if len(contents) > 1 else ''}

    def payment_facts(source, owner_body, payment, namespace):
        match = re.match(r'StrengthenComboPaymentFactory\.getSimple\s*\(', payment.strip())
        if not match:
            return {}
        encoded, _ = _balanced(payment.strip(), match.end() - 1)
        args = _arguments(encoded[1:-1])
        if not args:
            return {}
        raw = args[0].strip()
        callback_source = raw if raw.startswith('function') else owner_body
        pattern = (r'^function' if raw.startswith('function') else r'\b' + re.escape(raw) + r'\s*=\s*function')
        callback = re.search(pattern + r'\([^)]*\)\s*:\s*[\w*]+\s*\{', callback_source)
        if not callback:
            return {}
        callback_body, _ = _balanced(callback_source, callback.end() - 1)
        traces = [('inline' if raw.startswith('function') else raw, callback_body)]
        local_methods = {name: body for name, _, _, body in methods(source)}
        seen = set()
        for _ in range(3):
            added = []
            for _, body in traces:
                for name, implementation in local_methods.items():
                    if name not in seen and re.search(r'(?<![\w.])(?:this\.|self\.)?' + re.escape(name) + r'\s*\(', body):
                        added.append((name, implementation))
                        seen.add(name)
            if not added:
                break
            traces.extend(added)
        targets, successful_callbacks = [], []
        for via, trace in traces:
            for call in re.finditer(r'\b(?:this\.|self\.)?(\w+)\.(\w+)\s*\(', trace):
                field = re.search(r'\b' + re.escape(call[1]) + r'\s*:\s*(\w+)\s*(?:;|=)', source)
                if not field:
                    continue
                client = next((s for f, s in scripts.items() if Path(f).stem == field[1]), '')
                body = next((body for name, _, _, body in methods(client) if name == call[2]), '')
                request = re.search(r'\b(?:this\.)?request\(\s*([\w.]+)\s*,', body)
                if not request:
                    continue
                command = expression(request[1], field[1])
                symbolic_client_write = (request[1] == 'ClientSA.CMD_GAIN_BONUS' and re.search(r'extends\s+ClientSA\b', client))
                if not (isinstance(command, str) and re.fullmatch(r'\d[\w_]*', command)) and not symbolic_client_write:
                    continue
                invocation, _ = _balanced(trace, call.end() - 1)
                values = _arguments(invocation[1:-1])
                header = re.search(r'function\s+' + re.escape(call[2]) + r'\(([^)]*)\)', client)
                parameters = [p.split(':', 1)[0].strip() for p in _arguments(header[1])] if header else []
                bound = {name: expression(value, namespace) for name, value in zip(parameters, values)}
                selectors = {assignment[1]: bound[assignment[2]] for assignment in re.finditer(
                    r'\w+\["(\w+)"\]\s*=\s*(\w+)\s*;', body)
                    if type(bound.get(assignment[2])) in (str, int, bool)}
                target = {'factory': 'StrengthenComboPaymentFactory.getSimple', 'callback': traces[0][0],
                          'via': via, 'clientClass': field[1], 'clientMethod': call[2], 'commandReference': request[1]}
                if isinstance(command, str):
                    target['command'] = command
                if selectors:
                    target['selectors'] = selectors
                if target not in targets:
                    targets.append(target)
                successful_callbacks.append(invocation)
        if len(targets) != 1:
            return {}
        facts = {'payment': targets[0]}
        for marker in re.finditer(r'\.([A-Za-z_]\w*)\(\s*\)\s*;', '\n'.join(successful_callbacks)):
            for filename, model in scripts.items():
                body = next((body for name, _, _, body in methods(model) if name == marker[1]), '')
                assigned = re.fullmatch(r'\s*\{\s*this\.(\w+)\s*=\s*true\s*;\s*(?:this\.(?:dispatchEventX|notify)\([^;]+\);\s*)?\}\s*', body)
                if not assigned:
                    continue
                getter = re.search(r'function\s+(?:get\s+)?(\w+)\(\)\s*:\s*Boolean\s*\{\s*return\s+this\.' + re.escape(assigned[1]) + r';', model)
                if getter:
                    facts['claim'] = {'once': True, 'getter': getter[1], 'setter': marker[1],
                                      'stateMember': assigned[1], 'model': Path(filename).stem,
                                      'requestCommand': targets[0].get('command', targets[0]['commandReference'])}
        return facts

    def entry_facts(namespace, owner):
        def end_of_if_chain(source, begin):
            first = re.match(r'if\s*\(', source[begin:])
            if not first:
                return begin
            _, condition_end = _balanced(source, begin + first.end() - 1)
            start = condition_end + len(source[condition_end:]) - len(source[condition_end:].lstrip())
            if source[start:start+1] != '{':
                return start
            _, end = _balanced(source, start)
            alternative = re.match(r'\s*else\s*', source[end:])
            if not alternative:
                return end
            other = end + alternative.end()
            if source[other:other+1] == '{':
                return _balanced(source, other)[1]
            return end_of_if_chain(source, other)

        def enclosing_branches(source, position):
            predicates = []
            for branch in re.finditer(r'\bif\s*\(', source[:position]):
                try:
                    condition, condition_end = _balanced(source, branch.end() - 1)
                    start = condition_end + len(source[condition_end:]) - len(source[condition_end:].lstrip())
                    if source[start:start+1] != '{':
                        continue
                    _, end = _balanced(source, start)
                    if start < position < end:
                        predicates.append({'expression': condition[1:-1].strip(), 'value': True})
                    else:
                        alternative = re.match(r'\s*else\s*', source[end:])
                        if alternative:
                            other_start = end + alternative.end()
                            other_end = _balanced(source, other_start)[1] if source[other_start:other_start+1] == '{' else end_of_if_chain(source, other_start)
                            if other_start < position < other_end:
                                predicates.append({'expression': condition[1:-1].strip(), 'value': False})
                except (ValueError, IndexError):
                    continue
            return predicates

        facts = []
        for filename, source in scripts.items():
            ancestors, current = [source], source
            for _ in range(8):
                parent = re.search(r'\bclass\s+\w+\s+extends\s+(\w+)', current)
                inherited = next((s for f, s in scripts.items() if parent and Path(f).stem == parent[1]), '')
                if not inherited or inherited in ancestors:
                    break
                ancestors.append(inherited)
                current = inherited
            controller_fields = re.findall(r'\b(\w+)\s*:\s*' + re.escape(namespace) + r'\b', '\n'.join(ancestors))
            if not controller_fields:
                continue
            for mapping in re.finditer(r'\b\w+\["([^"\n]+)"\]\s*=\s*function\([^)]*\)\s*:\s*[\w*]+\s*\{', source):
                body, _ = _balanced(source, mapping.end() - 1)
                if not any(re.search(r'\b(?:self\.|this\.)?' + re.escape(field + '.' + owner) + r'\(', body) for field in controller_fields):
                    continue
                button = mapping[1]
                controls = []
                for control in re.finditer(r'\b(setEnabled|setVisible|setFrame)\s*\(', source):
                    encoded, _ = _balanced(source, control.end() - 1)
                    args = _arguments(encoded[1:-1])
                    path = expression(args[0], Path(filename).stem) if len(args) >= 2 else None
                    if (not isinstance(path, (list, str)) or isinstance(path, list) and any(not isinstance(p, str) for p in path)) and args[0].strip().startswith('['):
                        parts = _arguments(args[0].strip()[1:-1])
                        path = [value if isinstance(value := expression(part, Path(filename).stem), str) else '{' + part.strip() + '}' for part in parts]
                    path = [path] if isinstance(path, str) else path
                    if not isinstance(path, list) or not path or not all(isinstance(p, str) for p in path):
                        continue
                    record = {'kind': control[1], 'path': path, 'expression': args[1].strip(),
                              'when': enclosing_branches(source, control.start())}
                    if len(args) >= 3:
                        tip = expression(args[2], Path(filename).stem)
                        if isinstance(tip, str):
                            record.update(description=tip, descriptionComplete=True)
                    if args[1].strip().startswith('{'):
                        for field in _arguments(args[1].strip()[1:-1]):
                            entry = re.match(r'\s*"(enabled|disabledTips)"\s*:\s*([\s\S]+)', field)
                            if not entry:
                                continue
                            record[entry[1] + 'Expression'] = entry[2].strip()
                            if entry[1] == 'disabledTips':
                                # Preserve the exact raw expression separately;
                                # display only statically known text fragments.
                                pieces = [expression(part.strip(), Path(filename).stem) for part in
                                          re.findall(r'"(?:\\.|[^"\\])*"|[^+]+', entry[2]) if part.strip()]
                                record['descriptionComplete'] = bool(pieces) and all(type(part) in (str, int) for part in pieces)
                                if record['descriptionComplete']:
                                    record['description'] = ''.join(str(part) for part in pieces)
                    controls.append(record)
                paths = [control['path'] for control in controls if button in control['path']]
                selected = [control for control in controls if any(path[:len(control['path'])] == control['path'] for path in paths)]
                if selected:
                    facts.append({'button': button, 'consumer': Path(filename).stem,
                                  'evaluation': 'static-control-evidence-only', 'controls': selected})
        return facts

    def purchase_entitlement(owner_body, item, entries, claim, namespace):
        effect = re.fullmatch(r'(\w+)\["(\w+)"\]', item.get('effectExpression', ''))
        if not effect or not claim:
            return {}
        declaration = re.search(r'\bvar\s+' + re.escape(effect[1]) + r'\s*:\s*\w+\s*=\s*([\w.]+)\[([^]\n]+)\]\s*;', owner_body)
        if not declaration or declaration[1] not in tables:
            return {}
        index = expression(declaration[2], namespace)
        rows = tables[declaration[1]][1]
        if type(index) is not int or not isinstance(rows, list) or not 0 <= index < len(rows):
            return {}
        row = rows[index]
        if not isinstance(row, dict) or row.get(effect[2]) != item.get('$strengthenItem'):
            return {}
        for consumer in scripts.values():
            amount = re.search(r'\b(\w+)\s*=\s*int\(\s*' + re.escape(declaration[1]) + r'\[[^]\n]+\]\["(\w+)"\]\s*\)', consumer)
            if not amount or not re.search(r'ActUtil\.getMyDiamond\(\)\s*<\s*' + re.escape(amount[1]) + r'\b', consumer):
                continue
            price = row.get(amount[2])
            if type(price) is not int or price <= 0:
                continue
            for entry in entries:
                for control in entry['controls']:
                    if control['kind'] != 'setFrame':
                        continue
                    frame = re.fullmatch(r'([\w.]+)\s*\?\s*\d+\s*:\s*\(\s*([\w.]+)\s*\?\s*\d+\s*:\s*\d+\s*\)', control['expression'])
                    if not frame or frame[1].split('.')[-1] != claim['getter']:
                        continue
                    return {'kind': 'previous-purchase', 'table': declaration[1], 'rowIndex': index,
                            'effectField': effect[2], 'purchasePriceField': amount[2],
                            'purchaseCurrency': '8:2', 'purchaseAmount': price,
                            'qualificationExpression': frame[2], 'claimedExpression': frame[1],
                            'qualificationEvidence': 'purchase/claim/claimed frame selector',
                            'accountStateKnown': False, 'claimDebitKnown': False}
        return {}

    def enum_service_rows(source, owner_body, args, filename):
        effect = re.fullmatch(r'(\w+)\.(\w+)', args[0].strip())
        target = re.fullmatch(r'(\w+)\.(\w+)', args[2].strip())
        if not effect or not target or effect[1] != target[1]:
            return False
        variable = effect[1]
        declaration = re.search(r'\b' + re.escape(variable) + r'\s*:\s*(\w+)\b', owner_body)
        if not declaration:
            return False
        cls = declaration[1]
        enum = next((s for f, s in scripts.items() if Path(f).stem == cls), '')
        if not re.search(r'this\["_"\s*\+\s*(\w+)\]\s*=\s*\w+\.initField\(this,\w+,\1\)', enum):
            return False
        table_binding = re.search(r'([\w.]+)\.forEach\((\w+)\)', enum)
        creator = next((body for name, _, _, body in methods(enum) if table_binding and name == table_binding[2]), '')
        if not table_binding or not re.search(r'new\s+' + re.escape(cls) + r'\(\s*\w+\s*\)', creator):
            return False
        table = table_binding[1]
        if table not in result or not isinstance(result[table][1], list):
            return False
        getters = {}
        for getter in re.finditer(r'function\s+get\s+(\w+)\(\)\s*:\s*[\w.*<>]+\s*\{', enum):
            getters[getter[1]], _ = _balanced(enum, getter.end() - 1)
        effect_field = re.search(r'StrengthenComboItem\.createByString\(this\._(\w+)\s*[,)]', getters.get(effect[2], ''))
        target_field = re.search(r'return\s+this\._(\w+)(?:\.concat\(\))?\s*;', getters.get(target[2], ''))
        if not effect_field or not target_field:
            return False
        fields = {}
        for prop, body in getters.items():
            member = re.fullmatch(r'\s*\{\s*return\s+this\._(\w+)\s*;\s*\}\s*', body)
            if member:
                fields[prop] = member[1]
        trade = {}
        debit = re.search(r'\b[\w.]+\.(\w+)\s*-=\s*' + re.escape(variable) + r'\.(\w+)\s*;', owner_body)
        if debit and debit[2] in fields:
            labels = set()
            for consumer in scripts.values():
                for typed in re.findall(r'\b(\w+)\s*:\s*' + re.escape(cls) + r'\b', consumer):
                    guard = re.search(r'[\w.]+\.' + re.escape(debit[1]) + r'\s*<\s*' + re.escape(typed + '.' + debit[2]) + r'\s*\)\s*\{', consumer)
                    if guard:
                        body, _ = _balanced(consumer, guard.end() - 1)
                        label = re.search(r'"disabledTips"\s*:\s*"([^"\n]+)不足"', body)
                        if label:
                            labels.add(label[1])
            if len(labels) == 1:
                trade['activityCurrency'] = {'name': labels.pop(), 'field': fields[debit[2]]}
        for prop, field in fields.items():
            for consumer in scripts.values():
                typed = re.findall(r'\b(\w+)\s*:\s*' + re.escape(cls) + r'\b', consumer)
                if any(re.search(r'\b[\w.]+\(\s*' + re.escape(name) + r'\s*\)\s*>=\s*' + re.escape(name + '.' + prop) + r'\b', consumer) for name in typed):
                    trade['limit'] = {'field': field, 'cycleKnown': False}
        projected = []
        count = 0
        for index, row in enumerate(result[table][1]):
            if not isinstance(row, dict) or effect_field[1] not in row:
                projected.append(row)
                continue
            codes, races = row[effect_field[1]], row.get(target_field[1])
            if not isinstance(codes, str) or not re.fullmatch(CODE, codes) or not explicit_races(races):
                issue(table, '枚举服务调用的养成参数或精灵范围待适配', row)
                projected.append(row)
                continue
            row_trade = copy.deepcopy(trade)
            for kind, value_key in (('activityCurrency', 'count'), ('limit', 'maximum')):
                if kind in row_trade:
                    value = row.get(row_trade[kind]['field'])
                    if type(value) is int and value >= 0:
                        row_trade[kind][value_key] = value
                    else:
                        row_trade.pop(kind)
            projected.append(strengthen({**row, 'index': index,
                'basicDescription': row.get('name') or enhancement_description(codes, support) or '养成奖励',
                '_rewardField': 'enum-service/' + str(index), '_acquisitionKind': 'exchange',
                '_serviceTradeFacts': row_trade,
                '_callEvidence': {'consumer': 'IStrengthenComboService.showStrengthenPanel',
                    'enumClass': cls, 'inputTable': table, 'effectGetter': effect[2], 'targetGetter': target[2],
                    'effectField': effect_field[1], 'targetField': target_field[1]},
                'unlock': '按活动货币和兑换限次领取；可用精灵按实际养成效果判断'},
                codes, races, structure='enum-service-strengthen', manual=True))
            count += 1
        if count:
            result[table] = result[table][0], projected
        return count > 0

    for filename, source in scripts.items():
        namespace = Path(filename).stem
        # A static map projects raw config fields into official ActReward
        # fields. Accept only an explicitly consumed method, matching output
        # object and hasOwnProperty guard; do not execute a callback or infer
        # a target from a different tab in the same module.
        for method, start, end, body in methods(source):
            key = namespace + '.' + method
            if not re.search(re.escape(key) + r'\s*\(\s*\)', all_text):
                continue
            mapping = re.search(r'\breturn\s+([\w.]+)\.map\(function\(\s*(\w+)\s*:[^)]*\)\s*:\s*[\w.*<>]+\s*\{', body)
            if not mapping:
                continue
            rows = expression(mapping[1], namespace)
            if not isinstance(rows, list):
                continue
            callback, _ = _balanced(body, mapping.end() - 1)
            parameter = mapping[2]
            declaration = re.search(r'\bvar\s+(\w+)\s*:\s*[\w.*<>]+\s*=\s*\{', callback)
            if not declaration:
                continue
            output = declaration[1]
            raw, literal_end = _balanced(callback, declaration.end() - 1)
            guard = re.search(r'if\s*\(\s*' + re.escape(parameter) + r'\.hasOwnProperty\("(\w+)"\)\s*\)\s*\{', callback)
            if not guard or not re.search(r'\breturn\s+' + re.escape(output) + r'\s*;', callback):
                continue
            branch, branch_end = _balanced(callback, guard.end() - 1)
            if (callback[literal_end:guard.start()].strip(' \t\r\n;') or
                    not re.fullmatch(r'\s*return\s+' + re.escape(output) + r'\s*;\s*\}', callback[branch_end:])):
                issue(key, '奖励转换存在额外分支或修改，尚未完整解析')
                continue
            fields = {}
            for fragment in _arguments(raw[1:-1]):
                field = re.fullmatch(r'\s*"(\w+)"\s*:\s*(.+)', fragment, re.S)
                if field:
                    fields[field[1]] = field[2]
            assignments = dict(re.findall(re.escape(output) + r'\["(\w+)"\]\s*=\s*([^;]+);', branch))
            if assignments.get('rewardType', '').strip() != '"Strengthen"' or not re.fullmatch(
                    re.escape(parameter) + r'\["' + re.escape(guard[1]) + r'"\]', assignments.get('rewardParams', '').strip()):
                continue
            if 'filter' not in assignments:
                issue(key, '奖励转换未提供可验证的精灵范围')
                continue
            projected = []
            for row in rows:
                if not isinstance(row, dict) or guard[1] not in row:
                    continue
                bindings = {parameter: row}
                values = {field: expression(value, namespace, bindings) for field, value in {**fields, **assignments}.items()}
                values = {field: value for field, value in values.items() if not symbolic(value)}
                if not isinstance(values.get('rewardParams'), str) or not re.fullmatch(CODE + r'(?:\?[^?\r\n]+)?', values['rewardParams']):
                    issue(key, '奖励转换中的养成内容尚未完整解析', row)
                    continue
                if not explicit_races(values.get('filter')):
                    issue(key, '奖励转换中的指定精灵范围尚未完整解析', row)
                    continue
                values['_callEvidence'] = {'consumer': 'ActReward.map', 'method': key, 'inputTable': mapping[1]}
                projected.append(values)
            if projected:
                result[key] = filename, projected

        # Direct service calls can bind a separate effect constant and species
        # constant without any reward object. Reading an icon or an unused
        # constant alone never establishes an offer.
        for match in re.finditer(r'\.showStrengthenPanel\s*\(', source):
            try:
                call, _ = _balanced(source, match.end() - 1)
                args = _arguments(call[1:-1])
                if len(args) < 3:
                    continue
                containing = next(((name, begin, end, body) for name, begin, end, body in methods(source) if begin <= match.start() < end), ('direct', 0, len(source), source))
                owner, begin, _, owner_body = containing
                if enum_service_rows(source, owner_body, args, filename):
                    continue
                bindings = local_bindings(source, begin, match.start(), namespace)
                item = item_constructor(args[0].strip(), namespace, bindings) or expression(args[0], namespace, bindings)
                if not isinstance(item, dict) or '$strengthenItem' not in item:
                    continue
                codes = item['$strengthenItem']
                target = expression(args[2], namespace, bindings)
                if symbolic(codes):
                    continue  # Runtime forwarding functions have no independent offer.
                key = namespace + '.' + owner + '.directStrengthen'
                if not isinstance(codes, str) or not re.fullmatch(CODE + r'(?:\?[^?\r\n]+)?', codes):
                    issue(key, '公共养成调用的奖励参数尚未完整解析')
                    continue
                if not explicit_races(target):
                    issue(key, '公共养成调用的指定精灵范围尚未完整解析')
                    continue
                identity = re.sub(r'\s+', '', item['effectExpression'] + '/' + args[2])
                facts = payment_facts(source, owner_body, args[1], namespace)
                entries = entry_facts(namespace, owner)
                if entries:
                    facts['entryConditions'] = entries
                claim_getter = facts.get('claim', {}).get('getter')
                # A success marker alone does not prove a one-time quota.
                # The exact control must become hidden/disabled when that
                # marker's getter is true, rather than only decorating an icon.
                verified_claim = bool(claim_getter and any(
                    entry['button'] in control['path'] and (
                        (control['expression'] == 'false' and any(predicate['value'] is True and re.fullmatch(
                            r'[\w.]+\.' + re.escape(claim_getter) + r'(?:\(\s*\))?', predicate['expression'])
                            for predicate in control['when'])) or
                        (control['kind'] == 'setEnabled' and re.fullmatch(r'!\s*[\w.]+\.' + re.escape(claim_getter) + r'(?:\(\s*\))?', control['expression'])))
                    for entry in entries for control in entry['controls']))
                if not verified_claim:
                    facts.pop('claim', None)
                else:
                    entitlement = purchase_entitlement(owner_body, item, entries, facts['claim'], namespace)
                    if entitlement:
                        facts['purchaseEntitlement'] = entitlement
                if isinstance(item.get('compensation'), str) and item['compensation']:
                    facts['compensation'] = item['compensation']
                row = strengthen({'basicDescription': '指定精灵：' + (enhancement_description(codes.split('?')[0], support) or '养成奖励'),
                    '_rewardField': 'service/' + identity, '_acquisitionKind': 'reward' if verified_claim else 'unknown',
                    '_serviceTradeFacts': facts,
                    '_callEvidence': {'consumer': 'IStrengthenComboService.showStrengthenPanel',
                                      'effect': item['effectExpression'], 'target': args[2].strip(), **facts},
                    'unlock': '需满足官方领取入口条件，已领取的奖励不可重复领取' if verified_claim else '养成内容与指定精灵已识别；购买、领取条件请在活动内确认'},
                    codes, target, structure='direct-strengthen-call', manual=True)
                existing = result.setdefault(key, (filename, []))[1]
                if row not in existing:
                    existing.append(row)
            except (ValueError, IndexError, TypeError):
                continue
    return result
