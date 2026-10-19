"""Navigation evidence from actual activity entry points and consumed tabs.

Rewards and entry availability remain independent. Never manufacture an action
from a module/file name, and never call a service method to obtain evidence.
"""
from __future__ import annotations

import re

NAVIGATION_VERSION = 1
ACT_ROUTE = re.compile(r'btnNewAct_([A-Za-z][A-Za-z0-9]{1,100})_([A-Za-z0-9]{1,64}(?:_[A-Za-z0-9]{1,64}){0,7})')
SERVICE_ROUTE = re.compile(r'btnService_[A-Za-z][A-Za-z0-9]{1,100}_(?:show|open)[A-Z][A-Za-z0-9]{0,60}(?:_-?\d{1,10}){0,6}')


def valid_route(value):
    return isinstance(value, str) and len(value) <= 256 and bool(ACT_ROUTE.fullmatch(value) or SERVICE_ROUTE.fullmatch(value))


def _methods(source):
    from public_routine_updater import _balanced
    result = {}
    for match in re.finditer(r'\bfunction\s+(\w+)\s*\(([^)]*)\)\s*(?::\s*[\w.*]+\s*)?\{', source):
        body, _ = _balanced(source, match.end() - 1)
        params = re.findall(r'(\w+)\s*:', match[2])
        result[match[1]] = (params, body)
    return result


def _tab_default(row, body, inventory, scripts):
    """Follow int tab argument -> panel field -> consumed array/fallback."""
    if '.split("_")' not in body:
        return None
    from public_activity_exchange_updater import parse_tables
    _, constants = parse_tables(scripts)
    for number in re.findall(r'\bvar\s+(\w+)\s*:\s*int\b', body):
        if not re.search(r'\b' + re.escape(number) + r'\s*=\s*parseInt\(', body):
            continue
        for callee in re.findall(r'\b(\w+)\(\s*' + re.escape(number) + r'\s*\)', body):
            params, called = _methods(row['source']).get(callee, ([], ''))
            if not params:
                continue
            for panel in re.findall(r'new\s+(\w+)\(\s*' + re.escape(params[0]) + r'\s*\)\.show\(', called):
                owner = inventory.get(row['imports'].get(panel, row['package'] + '.' + panel))
                if not owner:
                    continue
                params, constructor = _methods(owner['source']).get(panel, ([], ''))
                if not params:
                    continue
                fields = re.findall(r'this\.(\w+)\s*=\s*' + re.escape(params[0]) + r'\s*;', constructor)
                for method, (_, source) in _methods(owner['source']).items():
                    for field in fields:
                        for match in re.finditer(r'var\s+(\w+)\s*:\s*int\s*=\s*([\w.]+)\.indexOf\(this\.' + re.escape(field) + r'\)\s*;', source):
                            index, table = match.groups()
                            key = re.escape(index)
                            fallback = re.search(key + r'\s*=\s*' + key + r'\s*==\s*-1\s*\?\s*0\s*:\s*' + key, source)
                            consumer = re.search(r'new\s+BriefTabHost\([^;]*,\s*' + key + r'\s*,', source)
                            values = constants.get(table)
                            if fallback and consumer and isinstance(values, list) and values and all(type(v) is int and 0 <= v < 2**31 for v in values):
                                return {'action': 'showMainPanel_' + str(values[0]), 'file': owner['file'], 'method': method, 'table': table}
    return None


def _adapter_main(row, body, parameter, inventory):
    """String -> adapter.action -> reachable controller main-panel branch."""
    for field, adapter in re.findall(r'this\.(\w+)\s*=\s*new\s+(\w+)\(\s*' + re.escape(parameter) + r'\s*\)', body):
        owner = inventory.get(row['imports'].get(adapter, row['package'] + '.' + adapter))
        if not owner:
            continue
        params, constructor = _methods(owner['source']).get(adapter, ([], ''))
        if not params:
            continue
        match = re.search(r'(\w+)\s*=\s*String\(' + re.escape(params[0]) + r'\)\.match\(/\(show\.\+Panel\)\(_\(\.\+\)\)\?/\)', constructor)
        if not match:
            continue
        stored = re.search(r'this\.(\w+)\s*=\s*' + re.escape(match[1]) + r'\[1\]', constructor)
        if not stored:
            continue
        getter = re.search(r'function\s+get\s+(\w+)\(\)\s*:\s*String\s*\{\s*return\s+this\.' + re.escape(stored[1]) + r'\s*;', owner['source'])
        if not getter:
            continue
        access = re.escape(field + '.' + getter[1])
        if not re.search(r'this\.' + access + r'\.match\(/show\.\+Panel/\)', body):
            continue
        for callee in re.findall(r'this\.(\w+)\(\s*\)', body):
            called = _methods(row['source']).get(callee, ([], ''))[1]
            if (re.search(r'\b(?:this|self)\.' + access + r'\s*==\s*"showMainPanel"', called) and
                    re.search(r'\.(?:showMainPanel|show)\(\s*(?:this|self)\.' + re.escape(field) + r'\s*\)', called)):
                return {'file': owner['file'], 'method': adapter, 'kind': 'string-action-adapter'}
    return None


def navigation_metadata(scripts, activity, timing=None, strings=()):
    from activity_service_discovery import _inventory
    literals = set(strings)
    for source in scripts.values():
        literals.update(re.findall(r'"(btn(?:NewAct|Service)_[^"\n]+)"', source))
    result = {'schema': NAVIGATION_VERSION, 'main': False, 'prefix': False,
              'tabs': [], 'evidence': [],
              'references': sorted({s for s in literals if valid_route(s)})[:512]}
    if activity.get('serviceBody'):
        # A service body has no NewActivity alias. Only the exact reviewed HUD
        # opening method and bounded integer arguments can become a click key.
        route = 'btnService_' + activity.get('serviceRoute', '').replace('#', '_')
        cls = activity.get('cls', '')
        inventory = _inventory(scripts)
        pieces = activity.get('serviceRoute', '').split('#')
        if (len(pieces) >= 2 and SERVICE_ROUTE.fullmatch(route) and cls in inventory and
                pieces[1] in _methods(inventory[cls]['source'])):
            result['serviceRoute'] = route
            result['evidence'].append({'file': inventory[cls]['file'], 'method': pieces[1], 'kind': 'registered-hud-service'})
        return result
    alias, entry = activity.get('activityAlias', ''), activity.get('cls', '')
    inventory = _inventory(scripts)
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]{1,100}', alias) or entry not in inventory:
        return result
    todo, visited = [(entry, 'initActivity')], set()
    while todo and len(visited) < 24:
        cls, method = todo.pop()
        if (cls, method) in visited or cls not in inventory:
            continue
        visited.add((cls, method))
        row = inventory[cls]
        params, body = _methods(row['source']).get(method, ([], ''))
        if not params:
            continue
        parameter = params[0]
        if result['prefix'] and not result.get('mainAction'):
            fallback = _tab_default(row, body, inventory, scripts)
            if fallback:
                result['main'] = True
                result['mainAction'] = fallback.pop('action')
                result['evidence'].append({**fallback, 'kind': 'consumed-default-tab'})
        aliases = {parameter}
        # Some entries copy the argument before unwrapping an optional object
        # action. A String retains the original value on that path.
        aliases.update(re.findall(r'\b(\w+)\s*(?::\s*\w+)?\s*=\s*' + re.escape(parameter) + r'\s*;', body))
        key = r'(?:"showMainPanel"|NewActivityParamsKey\.SHOW_MAIN_PANEL)'
        for name in aliases:
            adapter = _adapter_main(row, body, name, inventory)
            if adapter:
                result['main'] = True
                result['evidence'].append(adapter)
            arg = re.escape(name)
            exact = re.search(r'if\s*\(\s*' + arg + r'\s*==\s*' + key + r'\s*\)\s*\{\s*[\w.()]+\.(?:show|open|tryShow|checkShow)\w*\(', body)
            if exact:
                result['main'] = True
                result['evidence'].append({'file': row['file'], 'method': method, 'kind': 'explicit-main-action'})
            prefix = re.search(arg + r'\.indexOf\(\s*' + key + r'\s*\)\s*==\s*0', body)
            if prefix and re.search(r'\.(?:show|tryShow|open)\w*\(\s*' + arg + r'(?:\.toString\(\))?\s*\)', body):
                result['prefix'] = True
                result['evidence'].append({'file': row['file'], 'method': method, 'kind': 'explicit-main-prefix'})
            # This official controller pattern accepts String panel actions,
            # then fetches current state before opening its main panel.
            panel_match = re.search(r'(?:String\(\s*' + arg + r'\s*\)|\(\s*' + arg + r'\s+as\s+String\s*\))\.match\("show\.\+Panel\.\*"\)', body)
            if panel_match:
                forwards = re.findall(r'this\.(\w+)\(\s*' + arg + r'\s*\)', body)
                methods = _methods(row['source'])
                if any(re.search(r'\.(?:showMainPanel|show)\(', methods.get(callee, ([], ''))[1]) for callee in forwards):
                    result['main'] = True
                    result['evidence'].append({'file': row['file'], 'method': method, 'kind': 'controller-panel-dispatch'})
            # The actual TabFrame instance must receive this same argument;
            # a table or unrelated handleServiceCall elsewhere is insufficient.
            for field in re.findall(r'\b(?:private|protected|public)\s+var\s+(\w+)\s*:\s*TabFrame\s*;', row['source']):
                construction = re.search(r'this\.' + re.escape(field) + r'\s*=\s*new\s+TabFrame\([^;]+;', row['source'])
                if (construction and
                        re.search(r'this\.' + re.escape(field) + r'\.handleServiceCall\(\s*' + arg + r'\s*\)', body)):
                    tables = set(re.findall(r'\.setTabArray\(\s*([\w.]+)\s*\)', construction[0]))
                    result['main'] = result['prefix'] = True
                    result['tabs'] = [dict(tab) for tab in (timing or {}).get('tabs', [])
                                      if tab.get('table') in tables and tab.get('fullPanel') and valid_route(tab.get('route'))]
                    result['evidence'].append({'file': row['file'], 'method': method, 'kind': 'tab-frame-dispatch'})
            # Follow only actual parameter-forwarding calls from initActivity,
            # never similarly named but unreachable controller methods.
            for owner, callee in re.findall(r'\b(\w+)\.(?:instance|getInstance)(?:\(\))?\.(\w+)\(\s*' + arg + r'(?:\.toString\(\))?\s*\)', body):
                target = row['imports'].get(owner, row['package'] + '.' + owner)
                todo.append((target, callee))
            for callee in re.findall(r'this\.(\w+)\(\s*' + arg + r'\s*\)', body):
                todo.append((cls, callee))
    return result


def apply_navigation_graph(modules, discovery):
    """Resolve current parent tabs and entry contracts after every module scan."""
    tabs, references = {}, {}
    for module, record in modules.items():
        alias = record.get('activity', {}).get('activityAlias', '')
        blocked = alias in discovery.get('blocked', []) or str(discovery.get('entries', {}).get(alias, {}).get('online', 'true')).lower() == 'false'
        if blocked or record.get('status') in ('failed', 'excluded') or record.get('discoveryRetained'):
            continue
        metadata = record.get('navigationMetadata', {})
        for tab in metadata.get('tabs', []):
            tabs.setdefault(tab['targetAlias'], []).append((tab['route'], module, tab['table']))
        for route in metadata.get('references', []):
            match = ACT_ROUTE.fullmatch(route)
            if match:
                references.setdefault(match[1], set()).add(route)
    for module, record in modules.items():
        activity, metadata = record.get('activity', {}), record.get('navigationMetadata', {})
        alias = activity.get('activityAlias', '')
        blocked = alias in discovery.get('blocked', []) or str(discovery.get('entries', {}).get(alias, {}).get('online', 'true')).lower() == 'false'
        for shop in record.get('shops', []):
            source = shop.setdefault('source', {})
            if source.get('navigationLinkSource') in ('entry', 'parent-tab', 'reference', 'service'):
                shop['navigationLink'] = ''
                source.pop('navigationEvidence', None)
                source['navigationLinkSource'] = ''
            if shop.get('navigationLink') or blocked or record.get('status') in ('failed', 'excluded') or record.get('discoveryRetained'):
                continue
            route, kind, evidence = '', '', []
            parents = sorted(set(tabs.get(alias, [])))
            # Multiple distinct parents can have different availability or
            # account gates; use the child's proved entry instead of guessing.
            if len({row[0] for row in parents}) == 1:
                route, parent, table = parents[0]
                kind, evidence = 'parent-tab', [{'module': parent, 'table': table}]
            elif metadata.get('serviceRoute'):
                route, kind = metadata['serviceRoute'], 'service'
            else:
                # A numbered link from an unrelated module may select only
                # that caller's tab. It does not identify this reward table.
                accepted = sorted(r for r in references.get(alias, ())
                                  if r == f'btnNewAct_{alias}_showMainPanel' and metadata.get('main'))
                if len(accepted) == 1:
                    route, kind = accepted[0], 'reference'
                elif metadata.get('main'):
                    route, kind = f'btnNewAct_{alias}_' + metadata.get('mainAction', 'showMainPanel'), 'entry'
            if valid_route(route):
                shop['navigationLink'] = route
                source['navigationLinkSource'] = kind
                source['navigationEvidence'] = evidence or metadata.get('evidence', [])
