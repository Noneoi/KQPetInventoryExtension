"""Trace current HUD services into the official activity graph.

Service names and SWF paths come from the shipped ServiceConfig registration.
Only a supported date selector chooses a dynamic adapter; unresolved selectors
produce coverage issues instead of scanning every historical adapter in a SWF.
No ActionScript is executed and no account/service method is called.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
import copy
import hashlib
import json
import re

from public_routine_updater import _arguments, _balanced, _declaration

SCHEMA = 2
MAX_SERVICES = 32
MAX_CLASSES = 3000
REGISTRY_RESOURCE = 'library/interfaces'
REGISTRY_CLASS = 'mmo.interfaces.ServiceConfig'
TIME_RESOURCE = 'library/util'
TIME_CLASS = 'mmo.common.DateUtil'
SHANGHAI = timezone(timedelta(hours=8))
IDENTIFIER = r'[A-Za-z][A-Za-z0-9_]{0,100}'
RESOURCE = re.compile(r'(?:[A-Za-z0-9_]+/)+[A-Za-z0-9_]+')
QUOTED = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def hud_service_entries(hud_text):
    pending = [json.loads(_declaration(hud_text, 'DATA', '{'))]
    result = {}
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            route = value.get('tryGetService', '')
            parts = route.split('#') if isinstance(route, str) else []
            if len(parts) >= 2 and parts[0] != 'NewActivityService':
                if not all(re.fullmatch(IDENTIFIER, part) for part in parts[:2]):
                    raise ValueError('官方 HUD 服务入口结构待适配')
                result[route] = {'service': parts[0], 'method': parts[1], 'arguments': parts[2:],
                    'name': value.get('name', ''), 'route': route,
                    'startTime': value.get('startTime', ''), 'endTime': value.get('endTime', '')}
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    return [result[key] for key in sorted(result)]


def parse_service_registry(source):
    """Read helper argument mappings from ServiceInfo construction itself."""
    helpers = {}
    for match in re.finditer(r'\bstatic\s+function\s+(\w+)\s*\(([^)]*)\)\s*:\s*ServiceInfo\s*\{', source):
        body, _ = _balanced(source, match.end() - 1)
        returned = re.fullmatch(r'\{\s*return\s+new\s+ServiceInfo\(([^;]*)\);\s*\}', body)
        if not returned:
            continue
        params = re.findall(r'(\w+)\s*:', match[2])
        values = _arguments(returned[1])
        if len(values) >= 4 and all(value in params for value in (values[0], values[2], values[3])):
            helpers[match[1]] = [params.index(values[index]) for index in (0, 2, 3)]
    registrations = {}
    for match in re.finditer(r'\bstatic\s+(?:const|var)\s+\w+\s*:\s*Array\s*=\s*\[', source):
        array, _ = _balanced(source, match.end() - 1)
        for item in _arguments(array[1:-1]):
            call = re.match(r'(\w+)\s*\(', item)
            if not call or call[1] not in helpers:
                continue
            args, end = _balanced(item, call.end() - 1)
            if item[end:].strip():
                raise ValueError('官方服务注册出现未支持的条件表达式')
            raw = _arguments(args[1:-1])
            try:
                service, cls, module = [json.loads(raw[index]) for index in helpers[call[1]]]
            except (IndexError, ValueError, TypeError):
                raise ValueError('官方服务注册字段不是明确的静态字符串') from None
            if not (isinstance(service, str) and re.fullmatch(IDENTIFIER, service) and
                    isinstance(cls, str) and re.fullmatch(r'(?:\w+\.)+\w+', cls) and
                    isinstance(module, str) and RESOURCE.fullmatch(module)):
                raise ValueError('官方服务注册资源或入口类无效')
            row = {'service': service, 'cls': cls, 'module': module}
            if service in registrations and registrations[service] != row:
                raise ValueError('官方服务注册同名条目不唯一：' + service)
            registrations[service] = row
    if not registrations:
        raise ValueError('官方服务注册表未识别')
    return registrations


def _code(source, strings=False):
    # Preserve strings while removing comments, so URLs are never mistaken for
    # line comments. Dependency tokens then exclude string contents as well.
    tokens = re.compile(QUOTED.pattern + r'|/\*[\s\S]*?\*/|//[^\n]*')
    return tokens.sub(lambda match: match[0] if match[0][0] in '\"\'' and strings else ' ', source)


def _inventory(scripts):
    result = {}
    for filename, source in scripts.items():
        package = re.search(r'\bpackage\s+([\w.]+)', source)
        cls = re.search(r'\b(?:class|interface)\s+(\w+)', _code(source))
        if not cls:
            continue
        qualified = (package[1] + '.' if package else '') + cls[1]
        result[qualified] = {'file': filename, 'source': source, 'name': cls[1],
            'package': package[1] if package else '',
            'imports': {name.rsplit('.', 1)[-1]: name for name in re.findall(r'\bimport\s+([\w.]+);', source)}}
    if len(result) > MAX_CLASSES:
        raise ValueError('独立服务类图超出可处理范围')
    return result


def _instant(value):
    if isinstance(value, datetime):
        return value.replace(tzinfo=SHANGHAI) if value.tzinfo is None else value.astimezone(SHANGHAI)
    return _instant(datetime.fromisoformat(value))


def unresolved_alias_status(alias, discovery):
    """Explain failed resolution without treating an optional reference as a shop."""
    seen = set()
    while alias not in seen:
        seen.add(alias)
        if alias in discovery.get('blocked', []):
            return 'blocked'
        row = discovery.get('entries', {}).get(alias)
        if row is None:
            return 'missing-registration'
        if str(row.get('online', 'true')).lower() == 'false':
            return 'offline'
        target = re.match(r'btnNewAct_([A-Za-z0-9]+)_', row.get('link', ''))
        if not target:
            return 'unrecognized-entry'
        alias = target[1]
    return 'cyclic-entry'


def service_module_entries(discovery, latest_release=''):
    """Bridge verified current service bodies to the ordinary reward pipeline."""
    result = {}
    for route, record in discovery.get('records', {}).items():
        registration = record.get('registration', {})
        if not registration.get('module') or not record.get('scriptHashes'):
            continue
        entry = record['entry']
        # This key is an internal graph node. It is never a fabricated
        # NewActivity alias or a guessed navigation command.
        key = 'service:' + route
        result[key] = {'module': registration['module'], 'trace': record,
            'signature': hashlib.sha256(json.dumps({'files': record['scriptHashes'],
                'adapterSelections': record.get('adapterSelections', [])}, sort_keys=True).encode()).hexdigest(),
            'activity': {'cls': registration['cls'], 'name': entry['service'], 'activityAlias': '',
                'serviceBody': True,
                'activityName': entry.get('name') or entry['service'], 'serviceRoute': route,
                'startTime': entry.get('startTime', ''), 'endTime': entry.get('endTime', ''),
                'navigationLink': '', 'activityEvidence': 'hud-service', 'activityEvidenceDate': latest_release}}
    return result


def select_service_scripts(scripts, trace):
    selected = {}
    for filename, expected in trace.get('scriptHashes', {}).items():
        source = scripts.get(filename)
        if source is None or hashlib.sha256(source.encode()).hexdigest() != expected:
            raise ValueError('服务本体与已确认的当前入口类图不一致，保留原目录')
        selected[filename] = source
    if not selected:
        raise ValueError('当前服务没有可确认的静态类图')
    return selected


def _date_selection(source, selector, moment, time_source):
    """Verify reverse declaration order / latest begun date enum selection."""
    match = re.search(r'\bfunction\s+' + re.escape(selector) + r'\(\)\s*:\s*\w+\s*\{', source)
    if not match:
        raise ValueError('服务适配器时间选择方法未找到')
    body, _ = _balanced(source, match.end() - 1)
    compact = re.sub(r'\s+', '', body)
    if not ('.getServerTimeInMS()' in compact and '.length-1;' in compact and
            re.search(r'if\(\w+>=\w+\[\w+\]\._beginTime\)\{return\w+\[\w+\];\}', compact) and
            re.search(r'\w+--;', compact)):
        raise ValueError('服务适配器时间选择规则待适配')
    closed = re.search(r'function\s+getServerClosedTimeStr\(\)\s*:\s*String\s*\{\s*return\s*"( \d{2}:\d{2}:\d{2})";\s*\}', time_source)
    if not closed or not re.search(r'\._beginTime\s*=\s*new Date\(param2\s*\+\s*DateUtil\.getServerClosedTimeStr\(\)\)\.getTime\(\)', source):
        raise ValueError('服务适配器起始时间边界尚未验证')
    declarations = []
    for match in re.finditer(r'\bconst\s+(\w+)\s*:\s*\w+\s*=\s*(\w+)\(\d+,\s*"(\d{4}/\d{2}/\d{2})"\)', source):
        declarations.append((match[1], datetime.strptime(match[3] + closed[1], '%Y/%m/%d %H:%M:%S').replace(tzinfo=SHANGHAI)))
    if not declarations:
        raise ValueError('服务适配器时间枚举未识别')
    active = [row for row in declarations if row[1] <= moment]
    if not active:
        raise ValueError('当前时间没有已开放的服务适配器')
    upcoming = [date for _, date in declarations if date > moment]
    return active[-1][0], min(upcoming).isoformat() if upcoming else ''


def selector_classes(scripts):
    """External enum definitions required by local dynamic adapter maps."""
    result = set()
    for row in _inventory(scripts).values():
        for match in re.finditer(r'\b\w+\[\s*(\w+)\.(\w+)\(\)\s*\]', row['source']):
            enum = row['imports'].get(match[1])
            if enum and enum.startswith('mmo.interfaces.'):
                result.add(enum)
    return sorted(result)


def _select_adapters(row, enum_sources, moment):
    source = row['source']
    selections, boundaries = [], []
    for match in list(re.finditer(r'\b(\w+)\[\s*(\w+)\.(\w+)\(\)\s*\]', source)):
        mapping, enum, selector = match.groups()
        assignment = re.compile(r'\b' + re.escape(mapping) + r'\[\s*' + re.escape(enum) + r'\.(\w+)\s*\]\s*=\s*(\w+)\s*;')
        candidates = list(assignment.finditer(source))
        if not candidates:
            continue
        if len(re.findall(r'\b' + re.escape(mapping) + r'\[[^\]]+\]\s*=', source)) != len(candidates):
            raise ValueError('服务适配器包含未识别的动态映射写入')
        enum_class = row['imports'].get(enum, row['package'] + '.' + enum)
        if enum_class not in enum_sources:
            raise ValueError('服务适配器的官方选择器定义缺失：' + enum_class)
        key, boundary = _date_selection(enum_sources[enum_class], selector, moment, enum_sources.get(TIME_CLASS, ''))
        selected = [item[2] for item in candidates if item[1] == key]
        if len(selected) != 1:
            raise ValueError('当前服务适配器未注册或不唯一：' + key)
        source = assignment.sub(lambda item: item[0] if item[1] == key else '', source)
        selections.append({'map': mapping, 'selector': enum_class + '.' + selector, 'value': key, 'adapter': selected[0]})
        if boundary:
            boundaries.append(boundary)
    return source, selections, boundaries


def _bound_array(owner, field):
    """Literal array or a static IIFE returning that array after validation."""
    from public_activity_exchange_updater import LiteralReader
    source = owner['source']
    match = re.search(r'\bstatic\s+(?:const|var)\s+' + re.escape(field) + r'\s*:\s*Array\s*=\s*', source)
    if not match:
        return None
    start = match.end()
    if source[start:start + 1] == '[':
        expression, _ = _balanced(source, start)
        return LiteralReader(expression, static_calls=True).parse()
    wrapped = re.match(r'\(function\(\)\s*:\s*Array\s*\{', source[start:])
    if not wrapped:
        return None
    body, _ = _balanced(source, start + wrapped.end() - 1)
    local = re.search(r'\bvar\s+(\w+)\s*:\s*[\w*]+\s*=\s*\[', body)
    if not local:
        return None
    expression, end = _balanced(body, local.end() - 1)
    remainder = body[end:]
    var = re.escape(local[1])
    if not re.fullmatch(r';\s*return\s+(?:' + var + r'|Assert\.checkElementFieldDuplicated\(' + var + r',\s*"id"\));\s*\}', remainder):
        return None
    return LiteralReader(expression, static_calls=True).parse()


def trace_service(scripts, entry_class, entry_method, enum_sources, reference_time):
    """Return explicit activity links from current local class reachability."""
    from activity_periods import tab_range
    from activity_reward_structures import period_point
    from public_activity_exchange_updater import as_methods
    moment = _instant(reference_time)
    inventory = _inventory(scripts)
    if entry_class not in inventory or not re.search(r'\bfunction\s+' + re.escape(entry_method) + r'\s*\(', inventory[entry_class]['source']):
        raise ValueError('官方注册服务入口方法未找到：' + entry_class + '.' + entry_method)
    todo, reached, selections, boundaries = [entry_class], {}, [], []
    while todo:
        name = todo.pop()
        if name in reached:
            continue
        row = inventory[name]
        source, selected, dates = _select_adapters(row, enum_sources, moment)
        reached[name] = {**row, 'source': source}
        selections += selected
        boundaries += dates
        code = _code(re.sub(r'\bimport\s+[^;]+;', '', source))
        tokens = set(re.findall(r'\b[A-Za-z_]\w*\b', code))
        for candidate in tokens:
            target = row['imports'].get(candidate, row['package'] + '.' + candidate)
            if target in inventory and target not in reached:
                todo.append(target)
    links, inactive, issues = {}, [], []
    wrappers = {}
    # Recognize parameter-forwarding loader helpers through their real body.
    for name, row in reached.items():
        for match in re.finditer(r'\bfunction\s+(\w+)\(([^)]*)\)\s*:\s*[\w*]+\s*\{', row['source']):
            body, _ = _balanced(row['source'], match.end() - 1)
            params = re.findall(r'(\w+)\s*:', match[2])
            forwards = re.findall(r'\.loadAndInitNormalActivity\(\s*(\w+)\s*,', body)
            if len(forwards) == 1 and forwards[0] in params:
                wrappers[row['name'] + '.' + match[1]] = params.index(forwards[0])
    def add(alias, kind, row, **extra):
        if re.fullmatch(IDENTIFIER, alias):
            links.setdefault(alias, []).append({'kind': kind, 'file': row['file'], **extra})
    for name, row in reached.items():
        source = row['source']
        for route in re.finditer(r'"btnNewAct_([A-Za-z][A-Za-z0-9]+)_([A-Za-z0-9_]+)"', source):
            add(route[1], 'navigation', row, route=route[0][1:-1])
        for method, body in as_methods(source):
            calls = list(re.finditer(r'\.loadAndInitNormalActivity\s*\(', body))
            for call in calls:
                args, _ = _balanced(body, call.end() - 1)
                first = _arguments(args[1:-1])[0]
                if first.startswith('"'):
                    try:
                        alias = json.loads(first)
                    except ValueError:
                        continue
                    if isinstance(alias, str):
                        add(alias, 'loader-call', row, method=method)
                elif re.fullmatch(r'\w+', first):
                    assignments = re.findall(r'\b' + re.escape(first) + r'\s*(?::\s*\w+)?\s*=\s*([^;]+);', body[:call.start()])
                    assignments = [value.strip() for value in assignments if value.strip() != 'null']
                    if len(set(assignments)) == 1 and re.fullmatch(r'"' + IDENTIFIER + r'"', assignments[0]):
                        add(json.loads(assignments[0]), 'bound-loader-call', row, method=method)
                    elif row['name'] + '.' + method not in wrappers:
                        issues.append({'file': row['file'], 'method': method, 'reason': '服务活动调用目标不是可确认的静态别名'})
                else:
                    issues.append({'file': row['file'], 'method': method, 'reason': '服务活动调用目标表达式待适配'})
            for wrapper, index in wrappers.items():
                for call in re.finditer(r'\b' + re.escape(wrapper) + r'\s*\(', body):
                    args, _ = _balanced(body, call.end() - 1)
                    args = _arguments(args[1:-1])
                    if index < len(args) and re.fullmatch(r'"' + IDENTIFIER + r'"', args[index]):
                        add(json.loads(args[index]), 'forwarded-loader-call', row, method=method, wrapper=wrapper)
                    else:
                        issues.append({'file': row['file'], 'method': method, 'reason': '服务活动转发调用目标待适配', 'wrapper': wrapper})
        consumers = re.findall(r'\.tabArray\s*=\s*(\w+)\.(\w+)|\.setTabArray\(\s*(\w+)\.(\w+)\s*\)', source)
        for fields in consumers:
            owner_name, field = fields[:2] if fields[0] else fields[2:]
            owner_name = row['imports'].get(owner_name, row['package'] + '.' + owner_name)
            owner = reached.get(owner_name)
            if owner is None:
                issues.append({'file': row['file'], 'reason': '服务页签表不在已确认的当前类图中', 'table': owner_name + '.' + field})
                continue
            values = _bound_array(owner, field)
            if not isinstance(values, list):
                issues.append({'file': row['file'], 'reason': '服务页签表静态结构待适配', 'table': owner_name + '.' + field})
                continue
            for value in values:
                if not isinstance(value, dict) or not value.get('actName'):
                    continue
                alias = value['actName']
                if not isinstance(alias, str) or not re.fullmatch(IDENTIFIER, alias):
                    issues.append({'file': owner['file'], 'reason': '服务页签活动别名不是明确字符串', 'table': field})
                    continue
                if value.get('actType') not in (None, {'$ref': 'AQTabDefine.DEFAULT_ACT_TYPE'}):
                    issues.append({'file': owner['file'], 'reason': '服务页签使用其他入口类型，待确认资源关联', 'table': field, 'alias': alias})
                    continue
                period = tab_range(value.get('isOnline')) or {}
                if value.get('isOnline') is not None and not period:
                    issues.append({'file': owner['file'], 'reason': '服务页签开放条件待适配', 'table': field, 'alias': alias})
                    continue
                opened = True
                for key, date in period.items():
                    point = period_point(date).replace(tzinfo=SHANGHAI)
                    if point > moment:
                        boundaries.append(point.isoformat())
                    if key == 'startTime' and moment < point or key == 'endTime' and moment >= point:
                        opened = False
                evidence = {'kind': 'consumed-tab', 'file': owner['file'], 'consumer': row['file'],
                    'table': owner_name + '.' + field, 'tabId': value.get('id'),
                    'name': value.get('desc', ''), 'period': period, 'action': value.get('actParams', '')}
                if opened:
                    links.setdefault(alias, []).append(evidence)
                else:
                    inactive.append({'alias': alias, **evidence})
    return {'aliases': sorted(links), 'links': links, 'inactive': inactive, 'issues': issues,
            'reachableClasses': sorted(reached), 'adapterSelections': selections,
            'nextSelectionTime': min(boundaries) if boundaries else '',
            'scriptHashes': {row['file']: hashlib.sha256(scripts[row['file']].encode()).hexdigest() for row in reached.values()}}


def discover_service_roots(updater, versions, discovery, previous=None, now=None):
    """Cache dependency-bound service traces, then merge explicit graph roots."""
    from public_activity_exchange_updater import export_scripts, emit
    from public_icon_updater import loader_default
    entries = discovery.get('serviceEntries', [])
    if not entries:
        return {'schema': SCHEMA, 'entries': [], 'records': {}, 'pending': [], 'roots': [], 'retainedRoots': []}
    moment = _instant(now or datetime.now(SHANGHAI))
    previous = previous or {}
    result = {'schema': SCHEMA, 'entries': entries, 'records': {}, 'pending': [], 'roots': [], 'retainedRoots': []}
    roots, retained = set(), set()
    try:
        swf, registry_resource = updater.resource(REGISTRY_RESOURCE, versions)
        path = updater.export(swf, updater.scratch / 'service-registry', REGISTRY_CLASS)
        registry = parse_service_registry(path.read_text(encoding='utf-8-sig'))
        result['registryResource'] = registry_resource
    except Exception as error:
        result['pending'] = [{'activityName': row['name'], 'module': '', 'service': row['service'],
            'reason': '当前官方服务入口未扫描：' + str(error)} for row in entries]
        current_routes = {entry['route'] for entry in entries}
        for route, prior in previous.get('records', {}).items():
            if route in current_routes:
                aliases = sorted(set(prior.get('aliases', []) + prior.get('retainedAliases', [])))
                result['records'][route] = {**copy.deepcopy(prior), 'status': 'pending', 'aliases': [],
                                            'retainedAliases': aliases, 'reason': str(error)}
        result['retainedRoots'] = sorted({alias for record in result['records'].values() for alias in record['retainedAliases']})
        return result
    for index, entry in enumerate(entries):
        service = entry['service']
        registration = registry.get(service)
        key = entry['route']
        base = {'entry': entry, 'registration': registration or {}}
        try:
            if index >= MAX_SERVICES:
                raise ValueError('当前服务入口超过可处理范围')
            if not registration:
                raise ValueError('当前 HUD 服务没有可确认的官方资源注册')
            module = registration['module']
            fallback = None
            if module not in versions:
                fallback = loader_default(updater, {'source': {'loaderDefault': result.get('loaderDefault', previous.get('loaderDefault', {}))}})
                result['loaderDefault'] = fallback
            dependencies = {name: versions.get(name, '') for name in (REGISTRY_RESOURCE, TIME_RESOURCE, module)}
            if fallback is not None:
                dependencies.update({module: fallback['defaultVersion'], 'loaderManifest': fallback['version'],
                                     'config/config': versions.get('config/config', '')})
            hashes = {name: versions.hashes[name] for name in dependencies if name in getattr(versions, 'hashes', {})}
            old = previous.get('records', {}).get(key, {})
            valid_time = (old.get('referenceTime') and moment >= datetime.fromisoformat(old['referenceTime']) and
                (not old.get('nextSelectionTime') or moment < datetime.fromisoformat(old['nextSelectionTime'])))
            reusable = (previous.get('schema') == SCHEMA and old.get('entry') == entry and
                old.get('registration') == registration and old.get('dependencies') == dependencies and
                all(old.get('dependencyHashes', {}).get(name) == value for name, value in hashes.items()) and
                valid_time and old.get('status') == 'scanned' and 'shop' not in getattr(updater, 'force_components', ()))
            if reusable:
                record = copy.deepcopy(old)
            else:
                emit('检查官方独立服务入口：' + (entry['name'] or service))
                if fallback is None:
                    swf, resource = updater.resource(module, versions)
                else:
                    swf = updater.scratch / ('service-' + hashlib.sha256(module.encode()).hexdigest()[:16] + '.swf')
                    resource = updater.fetch('https://aoqi.100bt.com/play/' + module + '~' + fallback['defaultVersion'] + '.swf', swf)
                scripts = export_scripts(updater, swf, updater.scratch / ('service-' + hashlib.sha256(module.encode()).hexdigest()[:16]))
                enums = {}
                for cls in selector_classes(scripts):
                    dependency, _ = updater.resource(REGISTRY_RESOURCE, versions)
                    path = updater.export(dependency, updater.scratch / 'service-selectors', cls)
                    enums[cls] = path.read_text(encoding='utf-8-sig')
                if enums:
                    dependency, _ = updater.resource(TIME_RESOURCE, versions)
                    path = updater.export(dependency, updater.scratch / 'service-time', TIME_CLASS)
                    enums[TIME_CLASS] = path.read_text(encoding='utf-8-sig')
                trace = trace_service(scripts, registration['cls'], entry['method'], enums, moment)
                record = {**base, **trace, 'source': resource, 'dependencies': dependencies, 'dependencyHashes': hashes,
                    'referenceTime': moment.isoformat(), 'status': 'partial' if trace['issues'] else 'scanned'}
            for issue in record.get('issues', []):
                result['pending'].append({'activityName': entry['name'], 'module': module, 'service': service, **issue})
            roots.update(record.get('aliases', []))
        except Exception as error:
            prior = previous.get('records', {}).get(key, {})
            retained_aliases = sorted(set(prior.get('aliases', []) + prior.get('retainedAliases', [])))
            retained.update(retained_aliases)
            record = {**copy.deepcopy(prior), **base, 'status': 'pending', 'reason': str(error), 'aliases': [], 'retainedAliases': retained_aliases}
            result['pending'].append({'activityName': entry['name'], 'module': (registration or {}).get('module', ''),
                'service': service, 'reason': '当前官方服务入口未扫描：' + str(error)})
        result['records'][key] = record
    result['roots'] = sorted(roots)
    result['retainedRoots'] = sorted(retained - roots)
    for record in result['records'].values():
        for alias in record.get('aliases', []):
            discovery.setdefault('rootEvidence', {}).setdefault(alias, {'kind': 'service-reference', 'date': discovery.get('latestRelease', '')})
            link = next((row for row in record.get('links', {}).get(alias, []) if row.get('kind') == 'consumed-tab'), None)
            if link and alias in discovery.get('entries', {}):
                row = discovery['entries'][alias]
                period = link.get('period', {})
                for key in ('startTime', 'endTime'):
                    if period.get(key) and not row.get(key):
                        row[key] = period[key]
                if not row.get('link') and link.get('action') == 'showMainPanel':
                    row['link'] = 'btnNewAct_' + alias + '_showMainPanel'
    return result
