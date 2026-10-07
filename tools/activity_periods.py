"""Read activity periods from the official containers, schedules and UI packs.

Navigation alone does not transfer a deadline. Only a verified tab binding or
an active main-panel resource can attach time evidence to a reward module.
"""
from __future__ import annotations

from datetime import datetime
import hashlib
import re
import xml.etree.ElementTree as ET
import zipfile

from activity_reward_structures import apply_activity_period, period_point

PERIOD_PARSER_VERSION = 3


def calendar_range(value):
    """The overview's MwoUtil accepts dotted/slashed full or two-digit years.

    getEndTime adds DAY_TIME: its stated last day is inclusive, unlike the
    TabFrame isOnline range whose end is an exact 02:00 closing boundary.
    """
    value = (value or '').replace('年', '.').replace('月', '.').replace('日', '')
    day = r'(\d{2}|\d{4})[./-](\d{1,2})[./-](\d{1,2})'
    match = re.fullmatch(day + r'\s*[-~—至]\s*' + day, value)
    if not match:
        return None
    parts = [int(v) for v in match.groups()]
    try:
        dates = [datetime(y + 2000 if y < 100 else y, m, d) for y, m, d in (parts[:3], parts[3:])]
    except ValueError:
        return None
    if dates[0] > dates[1]:
        return None
    return {'startTime': dates[0].strftime('%Y%m%d'), 'endTime': dates[1].strftime('%Y%m%d')}


def tab_range(value):
    if not isinstance(value, str):
        return None
    # AQTabDefine.validateCase -> DateUtil.isAfterTimeWithDelayClose.
    date = r'(?:\d{8}|\d{4}-\d{2}-\d{2})(?: \d{2}:\d{2}:\d{2})?'
    match = re.fullmatch(r'(' + date + r')?\s*[-~—至]\s*(' + date + r')?', value)
    parts = list(match.groups()) if match else [value]
    if len(parts) > 2 or not any(parts):
        return None
    result = {}
    for key, point in zip(('startTime', 'endTime'), parts):
        if point:
            point += ' 02:00:00' if ' ' not in point else ''
            try:
                period_point(point)
            except ValueError:
                return None
            result[key] = point
    return result


def period_hints(strings):
    return any(value in strings for value in ('TabFrame', 'setTabArray', 'timeRange'))


def period_metadata(scripts, activity):
    from public_activity_exchange_updater import parse_tables
    tables, constants = parse_tables(scripts)
    text = '\n'.join(scripts.values())
    alias = activity.get('activityAlias', '')
    tabs, schedules = [], []
    for match in re.finditer(r'\.setTabArray\(\s*([\w.]+)\s*\)([^;]*);', text):
        table = match[1]
        rows = tables.get(table, ('', []))[1]
        if not isinstance(rows, list):
            continue
        order = re.search(r'\.setTabSeq\(\s*([\w.]+)\s*\)', match[2])
        selected = constants.get(order[1]) if order else None
        for row in rows:
            if not isinstance(row, dict) or type(row.get('id')) is not int:
                continue
            if isinstance(selected, list) and row['id'] not in selected:
                continue
            target = row.get('actName', '')
            if not isinstance(target, str) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9]+(?:@\d{8})?', target):
                continue
            action = row.get('actParams')
            # A route to a nested page is not evidence for the whole module.
            full_panel = action == 'showMainPanel' or action == {'action': 'showMainPanel'}
            tabs.append({'route': f'btnNewAct_{alias}_showMainPanel_{row["id"]}',
                         'targetAlias': target.split('@')[0], 'fullPanel': full_panel,
                         'period': tab_range(row.get('isOnline')) or {},
                         'table': table})
    # Verify the date-only inclusive-end contract in the actual consumer.
    inclusive = (re.search(r'return\s+[\w.]+\.parseDateToMs\([\w.]+\.splitTimeRange\(\w+\)\[1\]\)\s*\+\s*[\w.]+\.DAY_TIME\s*;', text) and
                 re.search(r'DAY_TIME\s*:\s*Number\s*=\s*24\s*\*\s*60\s*\*\s*60\s*\*\s*1000', text) and
                 re.search(r'DateUtil\.parseDate\(\w+\)', text) and
                 (re.search(r'_timeRange\s*=\s*\w+(?:\["timeRange"\]|\.timeRange)', text) or
                  re.search(r'this\["_"\s*\+\s*\w+\]\s*=\s*\w+\.initField\(this,\w+,\w+\)', text)))
    if inclusive:
        for table, (filename, rows) in tables.items():
            if not isinstance(rows, list) or not re.search(re.escape(table) + r'\.forEach\(', text):
                continue
            for row in rows:
                if not isinstance(row, dict):
                    continue
                route = row.get('jumpStr', '')
                period = calendar_range(row.get('timeRange', ''))
                if period and isinstance(route, str) and re.fullmatch(r'btnNewAct_[A-Za-z0-9]+_showMainPanel(?:_\d+)?', route):
                    schedules.append({'route': route, 'period': period, 'table': table,
                                      'file': filename, 'description': row.get('name', '')})
    return {'tabs': tabs, 'schedules': schedules}


def entry_opening(scripts, activity, constants):
    """A return in initActivity before opening any panel gates the whole entry."""
    from public_activity_exchange_updater import _balanced
    cls = activity.get('cls', '').rsplit('.', 1)[-1]
    candidates = set()
    for filename, text in scripts.items():
        if cls and filename.rsplit('/', 1)[-1] != cls + '.as':
            continue
        match = re.search(r'function\s+initActivity\([^)]*\)\s*:\s*void\s*\{', text)
        if not match:
            continue
        body, _ = _balanced(text, match.end() - 1)
        for guard in re.finditer(r'if\(DateUtil\.getServerTimeInMS\(\)\s*<\s*DateUtil\.(?:parse|parseDate)\(("[^"\n]+"|[\w.]+),\s*(true|false)\)\.getTime\(\)\)\s*\{([^{}]*)\}', body):
            if not re.search(r'\breturn\s*;', guard[3]):
                continue
            raw = guard[1]
            value = raw[1:-1] if raw.startswith('"') else constants.get(raw, constants.get(cls + '.' + raw))
            if not isinstance(value, str):
                continue
            value += (' 02:00:00' if guard[2] == 'true' else ' 00:00:00') if ' ' not in value else ''
            try:
                period_point(value)
                candidates.add(value)
            except ValueError:
                pass
    return max(candidates, key=period_point) if candidates else ''


def table_deadlines(scripts, tables, constants):
    """A shop-section guard only closes the tables consumed behind that guard.

    A similarly named unused predicate, a sale banner or another section's
    countdown does not close every reward in the containing activity.
    """
    from pathlib import Path
    from public_activity_exchange_updater import _balanced, as_methods
    predicates = {}
    for filename, text in scripts.items():
        cls = Path(filename).stem
        for match in re.finditer(r'static\s+function\s+(?:get\s+)?(\w+)\(\)\s*:\s*Boolean\s*\{\s*return\s+!DateUtil\.isAfterTimeWithDelayClose\(("[^"\n]+"|[\w.]+)\);\s*\}', text):
            raw = match[2]
            value = raw[1:-1] if raw.startswith('"') else constants.get(raw, constants.get(cls + '.' + raw))
            if isinstance(value, str):
                value += ' 02:00:00' if ' ' not in value else ''
                try:
                    period_point(value)
                    predicates[cls + '.' + match[1]] = value
                except ValueError:
                    pass
    deadlines = {}
    for text in scripts.values():
        for _, body in as_methods(text):
            for predicate, end in predicates.items():
                for guard in re.finditer(r'if\(\s*!' + re.escape(predicate) + r'(?:\(\))?\s*\)\s*\{', body):
                    block, position = _balanced(body, guard.end() - 1)
                    if not re.search(r'\breturn\s*;\s*\}$', block):
                        continue
                    guarded = body[position:]
                    for table in tables:
                        if re.search(r'\b' + re.escape(table) + r'\s*(?:\[|\.forEach\()', guarded):
                            deadlines.setdefault(table, []).append(end)
    return {table: min(ends, key=period_point) for table, ends in deadlines.items()}


def panel_bindings(scripts, module):
    """Only locally bound MainPanel packages, never task/dialog/unused assets."""
    result = {}
    directory = module.rsplit('/', 1)[0] + '/'
    for filename, source in scripts.items():
        expressions = []
        if filename.endswith('MainPanel.as'):
            component = re.search(r'\bCLS\s*:\s*String\s*=\s*(?:\w+\.cls\()?"([A-Za-z0-9_]+)"', source)
            expressions += [(owner, resource, component[1] if component else 'MainPanel') for owner, resource in
                            re.findall(r'\bURL\s*:\s*String\s*=\s*(\w+)\.url\("([A-Za-z0-9_]+)\.fui"\)', source)]
        # Container pages often have no MainPanel class of their own.
        expressions += re.findall(r'new\s+TabFrame\(\)\.setUrl\((\w+)\.url\("([A-Za-z0-9_]+)\.fui"\)\)\.setCls\((?:\w+\.cls\()?"([A-Za-z0-9_]+)"', source)
        for owner, resource, component in expressions:
            config = next((s for f, s in scripts.items() if f.replace('\\', '/').endswith('/' + owner + '.as') or f == owner + '.as'), '')
            if '"' + directory + '"' in config:
                result.setdefault(directory + resource, set()).add(component)
    return {key: next(iter(names)) for key, names in result.items() if len(names) == 1}


def panel_resources(scripts, module):
    return sorted(panel_bindings(scripts, module))


def fui_period(path, release, component_name='MainPanel'):
    """An activity-time label must be reachable from the exported MainPanel.

    For labels without a year, the official release day must fall in exactly
    one candidate range. The local current year is never a fallback.
    """
    try:
        anchor = datetime.strptime(release, '%Y%m%d')
    except ValueError:
        return {}
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) > 1024 or sum(i.file_size for i in archive.infolist()) > 8 * 1024 * 1024:
            raise ValueError('活动面板元数据过大')
        package = ET.fromstring(archive.read('package.xml'))
        resources = {node.get('id'): node for node in package.iter() if node.get('id')}
        todo = [node.get('id') for node in resources.values() if node.tag == 'component' and node.get('name') == component_name and node.get('exported') == 'true']
        seen, labels = set(), set()
        while todo:
            key = todo.pop()
            if key in seen:
                continue
            seen.add(key)
            resource = resources.get(key)
            if resource is None:
                continue
            label = resource.get('name', '')
            if label.startswith('活动时间'):
                labels.add(label)
            if resource.tag != 'component' or key + '.xml' not in archive.namelist():
                continue
            component = ET.fromstring(archive.read(key + '.xml'))
            for node in component.findall('./displayList/*'):
                if node.get('visible') == 'false' or node.get('pkg'):
                    continue
                if node.get('text', '').startswith('活动时间'):
                    labels.add(node.get('text'))
                if node.get('src'):
                    todo.append(node.get('src'))
    candidates = []
    for label in sorted(labels):
        raw = re.sub(r'^活动时间\s*[:：]\s*', '', label)
        raw = raw.replace('年', '.').replace('月', '.').replace('日', '')
        full = calendar_range(raw)
        if full:
            candidates.append({**full, 'label': label})
            continue
        match = re.fullmatch(r'(\d{1,2})[./](\d{1,2})\s*[-~—至]\s*(\d{1,2})[./](\d{1,2})', raw)
        if not match:
            continue
        sm, sd, em, ed = map(int, match.groups())
        options = []
        for year in (anchor.year - 1, anchor.year):
            try:
                start = datetime(year, sm, sd)
                end = datetime(year + ((em, ed) < (sm, sd)), em, ed)
            except ValueError:
                continue
            if start <= anchor <= end and (end - start).days < 180:
                options.append({'startTime': start.strftime('%Y%m%d'), 'endTime': end.strftime('%Y%m%d'),
                                'label': label, 'yearAnchor': release})
        if len(options) == 1:
            candidates.extend(options)
    ranges = {(p['startTime'], p['endTime']) for p in candidates}
    return candidates[0] if len(ranges) == 1 else {}


def load_panel_periods(updater, versions, scripts, module, activity, previous):
    results = []
    bindings = panel_bindings(scripts, module)
    old = {r['key']: r for r in previous.get('panelPeriods', [])}
    for key in panel_resources(scripts, module):
        version = versions.get(key)
        release = activity.get('releaseVersion', '')
        if not version:
            results.append({'key': key, 'version': None, 'release': release, 'period': {}})
            continue
        if old.get(key, {}).get('version') == version and old[key].get('release') == release and old[key].get('parserVersion') == PERIOD_PARSER_VERSION and not old[key].get('error'):
            results.append(old[key])
            continue
        record = {'key': key, 'version': version, 'release': release, 'parserVersion': PERIOD_PARSER_VERSION, 'period': {}}
        try:
            path = updater.scratch / ('period-panel-' + hashlib.sha256(key.encode()).hexdigest()[:20] + '.fui')
            record['source'] = updater.fetch('https://aoqi.100bt.com/play/' + key + '~' + version + '.fui', path)
            record['component'] = bindings.get(key, 'MainPanel')
            record['period'] = fui_period(path, release, record['component'])
        except Exception as error:
            if old.get(key, {}).get('period'):
                # A temporary resource failure must not resurrect an expired
                # reward by erasing the last verified deadline.
                record = {**old[key], 'requestedVersion': version, 'requestedRelease': release}
            record['error'] = str(error)
        results.append(record)
    return results


def panel_periods_reusable(record, versions, activity):
    return all(row.get('version') == versions.get(row['key']) and (not row.get('version') or row.get('parserVersion') == PERIOD_PARSER_VERSION) and row.get('release') == activity.get('releaseVersion', '') and
               not row.get('error') for row in record.get('panelPeriods', []))


def apply_graph_periods(modules, discovery):
    from public_activity_exchange_updater import resolve_alias
    routes, schedules = {}, {}
    for module, record in modules.items():
        metadata = record.get('periodMetadata', {})
        for tab in metadata.get('tabs', []):
            routes.setdefault(tab['route'], []).append((module, tab))
        for schedule in metadata.get('schedules', []):
            schedules.setdefault(schedule['route'], []).append((module, schedule))
    for route in schedules:
        direct = re.fullmatch(r'btnNewAct_([A-Za-z0-9]+)_showMainPanel', route)
        if direct:
            routes.setdefault(route, [('overview-direct', {'targetAlias': direct[1], 'fullPanel': True, 'period': {}})])
    evidence, parents = {}, {}
    for route, bindings in routes.items():
        if len(bindings) != 1:
            continue
        parent, binding = bindings[0]
        if not binding['fullPanel']:
            continue
        target = resolve_alias(binding['targetAlias'], discovery)
        if not target or target[0] not in modules:
            continue
        if parent != 'overview-direct':
            parents.setdefault(target[0], set()).add(parent)
        periods = []
        if binding['period']:
            periods.append({**binding['period'], 'kind': 'tab-online', 'module': parent, 'route': route, 'table': binding['table'],
                            'stale': modules[parent].get('status') == 'failed'})
        ranges = schedules.get(route, [])
        unique = {(r['period']['startTime'], r['period']['endTime']) for _, r in ranges}
        # Several campaigns can reuse the same tab. Do not give an old prize
        # a later campaign's period, or close the whole tab at the first date.
        if len(unique) == 1:
            owner, schedule = ranges[0]
            periods.append({**schedule['period'], 'kind': 'overview-range', 'module': owner, 'route': route,
                            'table': schedule['table'], 'description': schedule.get('description', ''),
                            'stale': modules[owner].get('status') == 'failed'})
        evidence.setdefault(target[0], []).extend(periods)
    def own_periods(module):
        record = modules[module]
        activity = record.get('activity', {})
        result = list(activity.get('periodEvidence', [])) + evidence.get(module, [])
        result += [{**row['period'], 'kind': 'main-panel-label', 'resource': row['key'], 'version': row['version'],
                    'stale': bool(row.get('error'))}
                   for row in record.get('panelPeriods', []) if row.get('period')]
        return result

    def inherited(module, seen):
        if module in seen:
            return []
        owners = parents.get(module, set())
        # Multiple entry containers can host different campaigns. Do not
        # transfer one campaign's expiry to a separately reachable activity.
        if len(owners) != 1:
            return []
        owner = next(iter(owners))
        if owner not in modules or owner in seen:
            return []
        return [{**p, 'viaContainer': owner} for p in own_periods(owner) + inherited(owner, seen | {module})]

    for module, record in modules.items():
        activity = dict(record.get('activity', {}))
        periods = own_periods(module) + inherited(module, set())
        activity['periodEvidence'] = periods
        for shop in record.get('shops', []):
            for good in shop.get('goods', []):
                if 'periodBounds' in good.get('source', {}):
                    apply_activity_period(good, activity)
                    if good.get('catalogStale') or any(p.get('stale') for p in periods):
                        good['availableKnown'] = False
