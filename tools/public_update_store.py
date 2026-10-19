"""Six-category candidate publication. Account files are never update inputs.

Catalog JSON lives in immutable version directories. A single atomic pointer
selects a complete catalog; unchanged and rejected categories retain their old
bytes. Images are disposable caches, outside the version manifest.
"""
from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import tempfile
import uuid

from public_update_support import Operations, UpdateCancelled, atomic_bytes, canonical, child, file_sha, plain, sha

COMPONENTS = {'pets': 'pet-detail-data.json', 'skills': 'pet-skill-data.json', 'shop': 'shop-exchange-data.json',
              'images': 'pet-image-index.json', 'icons': 'public-icon-index.json', 'routines': 'routine-overview.json'}
ATTACHMENTS = ('activity-exchange-data.json', 'activity-evolution-data.json', 'official-versions.json')
FIELDS = {'pets': ('pets', dict), 'skills': ('skills', dict), 'shop': ('shops', list),
          'images': ('faces', dict), 'icons': ('stargods', dict), 'routines': ('tasks', list)}
RULE_FILES = {
    'pets': ('generate_pet_detail_data.py', 'public_names_updater.py'),
    'skills': ('public_skill_updater.py', 'generate_pet_skill_data.py', 'public_pet_relations.py'),
    'shop': ('public_activity_exchange_updater.py', 'generate_shop_exchange_data.py', 'activity_reward_structures.py',
             'activity_call_rewards.py', 'activity_evolution_selector.py', 'activity_periods.py', 'activity_navigation.py', 'activity_trade_facts.py',
             'activity_constructed_offers.py', 'activity_price_rules.py', 'activity_service_discovery.py'),
    'images': ('generate_stargod_icons.py',),
    'icons': ('public_icon_updater.py', 'generate_stargod_icons.py'),
    'routines': ('public_routine_updater.py',),
}
SHA = re.compile(r'[a-f0-9]{64}\Z')


def now():
    return datetime.now(timezone.utc).isoformat()


def read_json(path, limit=32 * 1024 * 1024):
    path = plain(path)
    if path.stat().st_size > limit:
        raise ValueError('公共资料文件过大')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('公共资料 JSON 出现重复键')
            result[key] = value
        return result
    def invalid_constant(value):
        raise ValueError('公共资料 JSON 含非有限数值：' + value)
    value = json.loads(path.read_bytes().decode('utf-8-sig'), object_pairs_hook=unique, parse_constant=invalid_constant)
    if not isinstance(value, dict):
        raise ValueError('公共资料 JSON 根节点无效')
    return value


def pointer(root):
    path = child(root, 'catalog/current.json')
    if not path.is_file():
        return None
    value = read_json(path, 8192)
    if value.get('schema') != 1 or not SHA.fullmatch(value.get('revision', '')):
        raise ValueError('当前公共资料版本指针无效')
    return value


def version_files(root, revision):
    if not SHA.fullmatch(revision):
        raise ValueError('公共资料版本身份无效')
    directory = child(root, 'catalog/versions/' + revision)
    manifest_path = directory / 'manifest.json'
    if file_sha(manifest_path) != revision:
        raise ValueError('公共资料版本清单校验失败')
    manifest = read_json(manifest_path, 2 * 1024 * 1024)
    if manifest.get('schema') != 1 or set(manifest.get('components', {})) != set(COMPONENTS):
        raise ValueError('公共资料版本未完整登记六类资料')
    files = {}
    for name, record in manifest.get('files', {}).items():
        if name not in {*COMPONENTS.values(), *ATTACHMENTS} or not isinstance(record, dict):
            raise ValueError('公共资料清单包含未知文件')
        path = child(directory, name)
        if path.stat().st_size > 32 * 1024 * 1024 or path.stat().st_size != record.get('bytes') or file_sha(path) != record.get('sha256'):
            raise ValueError('公共资料文件校验失败：' + name)
        files[name] = path
    for key, name in COMPONENTS.items():
        record = manifest['components'][key]
        if record.get('file') != name or record.get('available') is not (name in files):
            raise ValueError('公共资料类别与实际文件清单不一致')
    return files, manifest


def catalog_path(root, name):
    current = pointer(root)
    if current:
        files, _ = version_files(root, current['revision'])
        return files.get(name, child(root, 'catalog/.unavailable/' + name))
    # The later local version is imported only from its verified object manifest.
    foreign = child(root, 'public/current.json')
    if foreign.is_file():
        value = read_json(foreign, 8192)
        revision = value.get('revision', '')
        if not SHA.fullmatch(revision):
            raise ValueError('已有公共资料版本身份无效，未退回旧目录')
        manifest_path = child(root, 'public/releases/' + revision + '/manifest.json')
        if file_sha(manifest_path) != revision:
            raise ValueError('已有公共资料清单校验失败')
        manifest = read_json(manifest_path, 2 * 1024 * 1024)
        key = next((key for key, file in COMPONENTS.items() if name == file), None)
        record = manifest.get('components', {}).get(key) if key else manifest.get('attachments', {}).get(name)
        if record:
            digest = record.get('sha256', '')
            if not SHA.fullmatch(digest) or record.get('format') != 'json':
                raise ValueError('已有公共资料对象格式不兼容')
            path = child(root, 'public/objects/' + digest + '/data')
            if file_sha(path) != digest:
                raise ValueError('已有公共资料对象校验失败')
            return path
        return child(root, 'catalog/.unavailable/' + name)
    return child(root, 'catalog/' + name)


def rule_identity(component):
    directory = Path(__file__).parent
    names = ('public_data_updater.py', 'public_data_quality.py', *RULE_FILES[component])
    return sha(canonical({name: file_sha(directory / name) for name in names}))


def validate(component, path):
    value = read_json(path)
    field, kind = FIELDS[component]
    if not isinstance(value.get(field), kind):
        raise ValueError('资料缺少有效的 ' + field)
    if component in ('pets', 'skills'):
        for key, row in value[field].items():
            if not key.isdecimal() or int(key) <= 0 or not isinstance(row, dict):
                raise ValueError('精灵或技能的记录身份无效')
        if not value[field]:
            raise ValueError('精灵或技能资料为空')
    if component == 'shop':
        for shop in value['shops']:
            if not isinstance(shop, dict) or not isinstance(shop.get('goods'), list):
                raise ValueError('商店商品结构无效')
            identities = set()
            for good in shop['goods']:
                if not isinstance(good, dict):
                    raise ValueError('商店商品记录无效')
                identity = good.get('sourceKey') or str(good.get('id', ''))
                if identity and identity in identities:
                    raise ValueError('商店商品身份重复')
                identities.add(identity)
    return value


def changes(component, before, after):
    tables = {'pets': ('pets', 'stargods', 'badges', 'sacredEquipment', 'astrolabe', 'items', 'attributes', 'jobs', 'money'),
              'skills': ('pets', 'skills', 'entries', 'buffs', 'relationRules'), 'images': ('faces', 'images'),
              'icons': ('stargods', 'attributes'), 'routines': ('tasks', 'activities')}
    def records(value):
        result = {}
        if component == 'shop':
            for shop in value.get('shops', []):
                prefix = str(shop.get('sourceKey') or shop.get('shopId', ''))
                result[prefix + ':shop'] = {key: row for key, row in shop.items() if key != 'goods'}
                for good in shop.get('goods', []):
                    key = str(good.get('sourceKey') or good.get('itemServerId', good.get('id', '')))
                    result[prefix + ':' + key] = good
        else:
            for table in tables[component]:
                rows = value.get(table, {})
                if isinstance(rows, list):
                    rows = {str(row.get('id', row.get('key', index))): row for index, row in enumerate(rows) if isinstance(row, dict)}
                if isinstance(rows, dict):
                    result.update({table + ':' + str(key): row for key, row in rows.items()})
        return result
    old, new = records(before), records(after)
    items = []
    for key in sorted(set(old) | set(new)):
        if old.get(key) == new.get(key):
            continue
        kind = 'added' if key not in old else 'removed' if key not in new else 'changed'
        row = new.get(key, old.get(key))
        items.append(dict(id=key, kind=kind, name=str(row.get('name', row.get('description', key))) if isinstance(row, dict) else key))
    result = dict(before=len(old), after=len(new), added=sum(x['kind'] == 'added' for x in items),
                  removed=sum(x['kind'] == 'removed' for x in items), changed=sum(x['kind'] == 'changed' for x in items),
                  items=items[:300], truncated=len(items) > 300)
    if component == 'shop':
        for label, is_shop in (('shops', True), ('goods', False)):
            result[label] = {kind: sum(x['kind'] == kind and x['id'].endswith(':shop') == is_shop for x in items)
                             for kind in ('added', 'changed', 'removed')}
    return result


def seed(root, baseline, workspace):
    catalog = workspace / 'catalog'
    catalog.mkdir(parents=True)
    for name in (*COMPONENTS.values(), *ATTACHMENTS):
        source = catalog_path(root, name)
        if not source.is_file():
            source = baseline / name
        if source.is_file():
            read_json(source)
            shutil.copyfile(source, catalog / name)


def seed_images(root, workspace, selected):
    # Existing pictures remain available while the candidate is isolated. Only
    # selected image categories are copied; account data is never traversed.
    directories = []
    if 'images' in selected:
        directories.append('images/pets')
    if 'icons' in selected:
        directories.extend(('images/stargods', 'images/attributes'))
    for relative in directories:
        source = child(root, relative)
        if source.is_dir():
            for path in source.iterdir():
                if path.is_file() and path.suffix.lower() in ('.png', '.json'):
                    plain(path)
                    if path.stat().st_size > 16 * 1024 * 1024:
                        continue
                    target = child(workspace, relative + '/' + path.name)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, target)
    index = workspace / 'catalog/public-icon-index.json'
    if 'icons' in selected and index.is_file():
        value = read_json(index)
        for group in ('stargods', 'attributes'):
            for key, record in value.get(group, {}).items():
                if not str(key).isdecimal() or not isinstance(record, dict):
                    continue
                source = record.get('file', '')
                if source.startswith(('images/', 'public/objects/')):
                    path = child(root, source)
                    target = child(workspace, 'images/' + group + '/' + str(key) + '.png')
                    if path.is_file() and not target.exists() and path.stat().st_size <= 16 * 1024 * 1024:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, target)


def publish_images(root, workspace, results):
    index = workspace / 'catalog/public-icon-index.json'
    if results.get('icons', {}).get('status') not in (None, 'failed', 'retained') and index.is_file():
        value = read_json(index)
        for group in ('stargods', 'attributes'):
            for key, record in value.get(group, {}).items():
                if not str(key).isdecimal():
                    continue
                source = child(workspace, 'images/' + group + '/' + str(key) + '.png')
                if source.is_file():
                    content = source.read_bytes()
                    name = 'images/public/' + sha(content) + '.png'
                    output = child(root, name)
                    if not output.exists():
                        atomic_bytes(output, content)
                    elif file_sha(output) != sha(content):
                        raise ValueError('公共图标内容缓存损坏')
                    record['file'] = name
        atomic_bytes(index, canonical(value))
    if results.get('images', {}).get('status') not in (None, 'failed', 'retained'):
        directory = workspace / 'images/pets'
        if directory.is_dir():
            for path in directory.iterdir():
                if path.suffix.lower() not in ('.png', '.json'):
                    continue
                output = child(root, 'images/pets/' + path.name)
                if output.exists() and output.stat().st_size == path.stat().st_size and output.stat().st_mtime_ns == path.stat().st_mtime_ns:
                    continue
                atomic_bytes(output, path.read_bytes())


def run_update(root, baseline, requested=None, *, attempt=None, verify_all=False, emit=lambda *a, **k: None,
               updater_factory=None, prepare_only=False):
    from public_data_updater import Updater
    root, baseline = plain(root), plain(baseline)
    selected = [key for key in COMPONENTS if requested is None or key in requested]
    if not selected or requested is not None and set(requested) - set(COMPONENTS):
        raise ValueError('数据更新类别无效')
    attempt = attempt or uuid.uuid4().hex
    if not re.fullmatch(r'[a-f0-9]{32}', attempt):
        raise ValueError('更新任务身份无效')
    directory = child(root, 'catalog/update-tasks/' + attempt)
    directory.mkdir(parents=True, exist_ok=True)
    report_path = directory / 'result.json'
    if report_path.exists():
        raise ValueError('更新任务已经有结果，不能重复执行')
    latest = child(root, 'catalog/public-update-status.json')
    previous_status = read_json(latest) if latest.is_file() else {}
    report = dict(schema=2, attempt=attempt, state='preparing', createdUtc=now(), requested=selected,
                  components={key: value for key, value in previous_status.get('components', {}).items() if key in COMPONENTS},
                  diskCommitted=False, sessionApplied=False, canCancel=True)
    operations = Operations(emit, cancelled=lambda: (directory / 'cancel').exists())
    def save():
        report['timings'] = operations.report()
        atomic_bytes(report_path, canonical(report))
        atomic_bytes(latest, canonical(report))
        visible = json.loads(canonical(report))
        visible['timings'].pop('operations', None)
        for row in visible.get('components', {}).values():
            if isinstance(row.get('changes'), dict):
                row['changes']['items'] = row['changes'].get('items', [])[:12]
            if 'pendingActivities' in row:
                row['pendingActivities'] = row['pendingActivities'][:12]
            for notice in row.get('notices', []):
                notice['itemCount'] = len(notice.get('items', []))
                notice['items'] = notice.get('items', [])[:12]
        emit('report', report=visible)
    save()
    before_pointer = None
    previous_manifest = {}
    try:
        before_pointer = pointer(root)
        pointer_path = child(root, 'catalog/current.json')
        before_token = pointer_path.read_bytes() if pointer_path.is_file() else b'null'
        previous_manifest = version_files(root, before_pointer['revision'])[1] if before_pointer else {}
        with tempfile.TemporaryDirectory(prefix='workspace-', dir=directory) as temporary:
            workspace = plain(temporary)
            with operations.measure('prepare'):
                seed(root, baseline, workspace)
                seed_images(root, workspace, selected)
            old = {key: read_json(workspace / 'catalog' / file) for key, file in COMPONENTS.items() if (workspace / 'catalog' / file).is_file()}
            # Preserve a verified rollback version before the first managed
            # publication, including imports from a later local installation.
            if not previous_manifest:
                initial_files = {name: dict(sha256=file_sha(workspace / 'catalog' / name), bytes=(workspace / 'catalog' / name).stat().st_size)
                                 for name in (*COMPONENTS.values(), *ATTACHMENTS) if (workspace / 'catalog' / name).is_file()}
                previous_manifest = dict(schema=1, files=initial_files,
                    components={key: dict(file=name, available=name in initial_files) for key, name in COMPONENTS.items()})
                raw = canonical(previous_manifest)
                initial_revision = sha(raw)
                initial = child(root, 'catalog/versions/' + initial_revision)
                initial.mkdir(parents=True, exist_ok=True)
                for name in initial_files:
                    path = initial / name
                    if not path.exists():
                        shutil.copyfile(workspace / 'catalog' / name, path)
                if not (initial / 'manifest.json').exists():
                    atomic_bytes(initial / 'manifest.json', raw)
                version_files(root, initial_revision)
            else:
                initial_revision = before_pointer['revision']
            backup = workspace / 'before'
            shutil.copytree(workspace / 'catalog', backup)
            scratch = workspace / 'scratch'
            scratch.mkdir()
            updater = (updater_factory or Updater)(workspace, baseline, scratch, authoritative=True,
                        runtime_root=root / 'data-tools', operations=operations, verify_all=verify_all)
            identities = {key: rule_identity(key) for key in selected}
            updater.force_components = {key for key in selected if previous_manifest.get('components', {}).get(key, {}).get('rulesSha256') != identities[key]}
            report['state'] = 'running'
            save()
            results = updater.run(set(selected))
            operations.check()
            with operations.measure('validate'):
                updater.sources.assert_stable()
                for key in selected:
                    row = results[key]
                    if row['status'] == 'failed':
                        row['retryable'] = True
                        continue
                    try:
                        candidate = validate(key, workspace / 'catalog' / COMPONENTS[key])
                        from public_data_quality import validate_semantic_transition
                        validate_semantic_transition(key, old.get(key, {}), candidate)
                        row['changes'] = changes(key, old.get(key, {}), candidate)
                        if candidate == old.get(key):
                            row['status'] = 'unchanged'
                    except (OSError, ValueError, TypeError, KeyError) as error:
                        row.update(status='failed', error=str(error), retryable=True)
                blocked = {key for key in selected if results[key]['status'] == 'failed'}
                if blocked.intersection(('pets', 'skills', 'shop')):
                    blocked.update(set(selected).intersection(('pets', 'skills', 'shop')))
                if 'pets' in blocked:
                    blocked.update(set(selected).intersection(('images', 'icons')))
                for key in blocked:
                    results[key].update(status='retained', retryable=True, changes={},
                                        error=results[key].get('error') or '关联资料未通过检查，本次沿用原资料')
                    names = [COMPONENTS[key], *ATTACHMENTS] if key == 'shop' else [COMPONENTS[key]]
                    for name in names:
                        target, source = workspace / 'catalog' / name, backup / name
                        if source.is_file():
                            shutil.copyfile(source, target)
                        elif target.is_file():
                            target.unlink()
                for key, row in results.items():
                    row['checkedUtc'] = now()
                    row['appliedUtc'] = previous_status.get('components', {}).get(key, {}).get('appliedUtc')
                    if key not in blocked and row.get('error'):
                        row['status'] = 'partial'
                        row['retryable'] = bool(row.get('retryable') or
                            (key == 'shop' and getattr(updater, 'activity_exchange_failures', [])))
            operations.check()
            report.update(state='applying', canCancel=False)
            emit('phase', phase='applying')
            save()
            publish_images(root, workspace, results)
            with operations.measure('save'):
                files = {}
                for name in (*COMPONENTS.values(), *ATTACHMENTS):
                    path = workspace / 'catalog' / name
                    if path.is_file():
                        read_json(path)
                        files[name] = dict(sha256=file_sha(path), bytes=path.stat().st_size)
                components = dict(previous_manifest.get('components', {}))
                for key, filename in COMPONENTS.items():
                    value = dict(components.get(key, {}), file=filename, available=filename in files)
                    if filename in files and (not value.get('contentUpdatedUtc') or
                            files[filename]['sha256'] != previous_manifest.get('files', {}).get(filename, {}).get('sha256')):
                        value['contentUpdatedUtc'] = now()
                    if key in selected and key not in blocked:
                        value['rulesSha256'] = identities[key]
                    components[key] = value
                manifest = dict(schema=1, components=components, files=files)
                manifest_raw = canonical(manifest)
                revision = sha(manifest_raw)
                target = child(root, 'catalog/versions/' + revision)
                if not target.exists():
                    prepared = directory / 'prepared'
                    prepared.mkdir()
                    for name in files:
                        shutil.copyfile(workspace / 'catalog' / name, prepared / name)
                    atomic_bytes(prepared / 'manifest.json', manifest_raw)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    prepared.rename(target)
                version_files(root, revision)
                current_token = pointer_path.read_bytes() if pointer_path.is_file() else b'null'
                if current_token != before_token:
                    raise ValueError('现用资料已被其他任务切换，请重新检查')
                previous_revision = initial_revision
                if before_pointer and before_pointer['revision'] == revision:
                    previous_revision = before_pointer.get('previousRevision', '')
                next_pointer = dict(schema=1, revision=revision, attempt=attempt, previousRevision=previous_revision)
                combined = dict(report['components']); combined.update(results)
                report.update(state='prepared', revision=revision, previousRevision=next_pointer['previousRevision'], components=combined,
                              expectedPointerSha256=sha(before_token), checkedUtc=now(),
                              complete=all(row['status'] in ('updated', 'unchanged') and not row.get('error') for row in results.values()))
                save()
                if not prepare_only:
                    atomic_bytes(child(root, 'catalog/current.json'), canonical(next_pointer))
            if prepare_only:
                save()
                return report
            finished = now()
            for key, row in results.items():
                row['checkedUtc'] = finished
                previous = previous_status.get('components', {}).get(key, {})
                row['appliedUtc'] = (previous.get('appliedUtc') or finished) if row['status'] == 'unchanged' else (
                    finished if key not in blocked else previous.get('appliedUtc'))
                if row.get('error') and key not in blocked:
                    row['status'] = 'partial'
                    # Unsupported structures need parser work; network failures can retry.
                    row['retryable'] = bool(row.get('retryable') or (key == 'shop' and getattr(updater, 'activity_exchange_failures', [])))
            combined = {key: value for key, value in previous_status.get('components', {}).items() if key in COMPONENTS}
            combined.update(results)
            report.update(state='diskCommitted', diskCommitted=True, checkedUtc=finished, components=combined,
                          complete=all(row['status'] in ('updated', 'unchanged') for row in results.values()), canCancel=False)
            save()
            return report
    except UpdateCancelled:
        for key in selected:
            report['components'][key] = dict(report['components'].get(key, {}), status='cancelled', retryable=True, changes={})
        report.update(state='cancelled', checkedUtc=now(), error='检查已取消，现用资料保留', canCancel=False)
        save()
        return report
    except Exception as error:
        # A process can fail after an atomic pointer commit but before its receipt.
        try:
            active = pointer(root)
        except (OSError, ValueError):
            active = None
        committed = bool(active and active.get('attempt') == attempt)
        for key in selected:
            report['components'][key] = dict(report['components'].get(key, {}), status='failed', error=str(error), retryable=True, changes={})
        report.update(state='diskCommitted' if committed else 'failed', diskCommitted=committed,
                      checkedUtc=now(), error=str(error), canCancel=False)
        save()
        return report
