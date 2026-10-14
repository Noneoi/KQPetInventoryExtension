"""One-shot, complete replay catalog update using public official resources.

All research inputs are restored from a bounded, hash-checked source bundle.
Same-byte consumers can be reused; changing supported literal data does not
approve changing client code. Unknown consumer/schema changes fail the entire
replay component before activation. No game/account APIs or downloaded code run.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import inspect
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import zipfile

MODULES = Path(__file__).resolve().parent / 'battle_replay'
sys.path.insert(0, str(MODULES))
from catalog_pack import MAX_PACK_BYTES, MAX_JSON_BYTES, unpack_bytes, validate_catalog_bytes, pack_file
from fetch_reference import binary_payloads, flash_versions, save_binary_payloads
from update_source_bundle import extract_bundle
from extract_skill_tables import LiteralParser, parse_map, TEXT_TABLES
from fetch_pet_reference import parse_dictionary
from generate_pet_detail_data import iter_create_args
from public_resource_session import PublicResourceSession, H5_MAIN_SHA256
from audit_source_coverage import audit_source_coverage

FLASH = 'https://aoqi.100bt.com/play/'
H5 = 'https://aoqi.100bt.com/h5/'
PARSER_VERSION = 1
# Reviewed official js/main~202609301790706468.js: versionMap then start.files;
# mix alias takes precedence, otherwise Loader.DEFAULT_VERSION = 20211126.
MAX_DOWNLOAD = 32 * 1024 * 1024
DATA_PACKAGES = {'battleconfig/battleconfig', 'pet/petdictionarydata', 'pet/petdictionarydataupdate'}
SKIN_RESOURCE = 'changepetskinv3/changepetskinservice'
SKIN_CONFIG_CLASS = 'mmo/changepetskinv3/other/Cpsv3SkinConfig.as'
SKIN_CONSUMER_CLASSES = frozenset({
    'mmo/changepetskinv3/ChangePetSkinService.as',
    'mmo/changepetskinv3/other/Cpsv3AvailablePet.as',
    'mmo/changepetskinv3/other/Cpsv3Skin.as',
    'mmo/changepetskinv3/other/Cpsv3SkinSummary.as',
    'mmo/changepetskinv3/other/outer/Cpsv3SkinMultiFormImpl.as',
})
_VERSION = re.compile(r'(?:2000|[0-9]{8,20})\Z')
_RESOURCE = re.compile(r'[A-Za-z0-9_/-]+\Z')


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _read(path, limit=MAX_JSON_BYTES):
    path = Path(path)
    if path.stat().st_size > limit:
        raise ValueError('回放更新输入超过大小限制：' + path.name)
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError('回放更新输入在读取时超过限制：' + path.name)
    return data


def _json(path):
    return json.loads(_read(path).decode('utf-8-sig'))


def _write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False).encode('utf-8'))


def _walk(value):
    if isinstance(value, dict):
        yield value
        for item in value.values():
            yield from _walk(item)
    elif isinstance(value, list):
        for item in value:
            yield from _walk(item)


def dependency_records(paths):
    """Collect every explicit public-package dependency, including title assets.

    Different historical version URLs may identify the same approved bytes.
    Conflicting approved bytes require a reviewed bundle, never a best guess.
    """
    flash, h5_consumers = {}, {}
    for path in paths:
        if path.suffix.lower() != '.json':
            continue
        for node in _walk(_json(path)):
            if node.get('error') and not node.get('sha256'):
                # Historical negative probes are not parsed dependencies. Keep
                # them historical; never promote a 404 to official evidence.
                continue
            url = node.get('url', '')
            match = re.fullmatch(re.escape(FLASH) + r'([A-Za-z0-9_/-]+)~([0-9]+)\.swf', url)
            if match and match[1] != 'versiondata':
                resource, target = match[1], flash
            else:
                match = re.fullmatch(re.escape(H5) + r'(js/[A-Za-z0-9_/-]+)~([0-9]+)\.js', url)
                if not match:
                    continue
                resource, target = match[1] + '.js', h5_consumers
            digest = node.get('sha256', '')
            if not re.fullmatch('[0-9a-f]{64}', digest):
                raise ValueError('官方来源缺少有效校验：' + resource)
            if resource in target and target[resource]['sha256'] != digest:
                raise ValueError('同一官方资源存在冲突的解析依据：' + resource)
            target[resource] = dict(node, resource=resource)
    if not DATA_PACKAGES.issubset(flash) or not {'library/interfaces', 'battle/battle', 'loader'}.issubset(flash):
        raise ValueError('回放更新材料缺少官方依赖清单')
    return flash, h5_consumers


def _h5_data_approvals(paths):
    """Only explicit official producer URLs can establish same-package proof."""
    records, conflicting = {}, set()
    for path in paths:
        if path.suffix.lower() != '.json':
            continue
        for row in _walk(_json(path)):
            url = row.get('url', '')
            if not isinstance(url, str):
                continue
            match = re.fullmatch(re.escape(H5) + r'(config/[A-Za-z0-9_/-]+)~[0-9]+\.(json|aqz)', url)
            digest = row.get('sha256', '')
            if not match or not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
                continue
            resource = match[1] + '.' + match[2]
            if resource in records and records[resource]['sha256'] != digest:
                conflicting.add(resource)
            records[resource] = {'resource': resource, 'sha256': digest, 'url': row['url']}
    # Conflicting historical versions are not permission to select whichever
    # hash happens to match this run. They simply cannot prove this exception.
    return {key: row for key, row in records.items() if key not in conflicting}


def _audit_inputs(arguments, previous=None):
    names = inspect.signature(audit_source_coverage).parameters
    inputs = dict(arguments)
    if 'auxiliary' in inputs:
        inputs['auxiliary_source'] = inputs.pop('auxiliary')
    return audit_source_coverage(**{key: value for key, value in inputs.items() if key in names and key != 'previous'},
                                 previous=previous)


def _coverage_location(report, row):
    """Issue identity follows the role-relative input, never a scratch path."""
    if row.get('key'):
        return row['key']
    if row.get('parser'):
        return 'parser:' + row['parser']
    role = row.get('role', 'input')
    root = report.get('inputs', {}).get(role, {}).get('sourcePath')
    def relative(value):
        if not value:
            return '.'
        path = Path(value)
        if not path.is_absolute():
            return path.as_posix()
        if root:
            base = Path(root)
            if base.is_file():
                base = base.parent
            return Path(os.path.relpath(path, base)).as_posix()
        return path.as_posix()
    result = role + ':' + relative(row.get('file') or row.get('sourcePath'))
    if row.get('manifestPath'):
        result += ' @ ' + relative(row['manifestPath'])
    return result


def _coverage_identity(report, row):
    return row['kind'], _coverage_location(report, row), row.get('field', '')


def _source_release_findings(current, previous, *, approved, records, data_changes, controls):
    """Classify release risk without claiming that inventory gaps are understood.

    A same-byte producer can prove that a newly enumerated table was omitted
    from the historical subset. It does not turn that table into parsed data.
    All exceptions carry exact byte evidence; missing semantics remain gaps.
    """
    old_artifacts = {row['key']: row for row in previous['artifacts']}
    artifacts = {row['key']: row for row in current['artifacts']}
    old_issues = {_coverage_identity(previous, row): row for row in previous['issues']}
    current_issues = {_coverage_identity(current, row) for row in current['issues']}
    resources = {row['resource']: row for row in records if row.get('resource')}
    verified_paths = {str(Path(row['file']).resolve()): row for row in records if row.get('file')}
    receipts = {str(Path(row['sourcePath']).resolve()): row for row in data_changes}

    def producer_proof(artifact):
        if not artifact or artifact['byteVerification'] != 'local-bytes-match-declarations':
            return None
        for claim in artifact['provenanceClaims']:
            resource = claim.get('package') or claim.get('resource')
            # The archive member list is generated by _safe_aqz from this
            # exact package. Do not use a filename heuristic for other tables.
            if (artifact['role'] == 'source' and artifact['relativePath'].startswith('h5-tables/') and
                    Path(claim.get('manifestPath', '')).name == 'h5-provenance.json' and
                    claim.get('file') == artifact['relativePath']):
                resource = 'config/battleconfig.aqz'
            before, after = approved.get(resource), resources.get(resource)
            if before and after and before['sha256'] == after['sha256'] and claim['sha256'] == artifact['sha256']:
                return {'resource': resource, 'approvedProducerSha256': before['sha256'],
                        'currentProducerSha256': after['sha256'], 'inputSha256': artifact['sha256'],
                        'manifestPath': claim['manifestPath']}
        return None

    findings = []
    for row in current['reviewRequired']:
        kind, key = row['kind'], row.get('key')
        identity = _coverage_identity(current, row)
        artifact = artifacts.get(key)
        status = 'existing' if identity in old_issues else 'new'
        finding = {'kind': kind, 'path': _coverage_location(current, row),
                   'severity': row.get('severity', 'review'), 'baselineStatus': status,
                   'message': '本次输入结构或解析依赖有待核对的变化；未推断官方含义。'}
        if row.get('field'):
            finding['path'] += '#' + row['field']
        if (kind == 'declared-material-missing' and identity in old_issues and
                row.get('declaredSha256') == old_issues[identity].get('declaredSha256')):
            finding.update(severity='info', message='历史材料已经声明但未收录的导出文件；本次未将它当作新增解析错误。')
        elif kind == 'declared-material-missing' and Path(row.get('file', '')).is_absolute():
            path = Path(row['file']).resolve()
            proof = verified_paths.get(str(path))
            if proof and proof['sha256'] == row.get('declaredSha256') and path.is_file() and _sha(_read(path)) == proof['sha256']:
                finding.update(severity='info', message='本次资源文件位于另一输入根，路径及实际字节已与本次下载记录核对。',
                               byteProof={'sourcePath': str(path), 'sha256': proof['sha256']})
        elif kind in {'new-unmapped-table', 'new-unrecognized-field'}:
            proof = producer_proof(artifact)
            control = controls.get(key)
            if control and artifact and artifact['sha256'] == control['sha256']:
                finding.update(severity='info', message='本次版本加载器已校验的入口或版本清单；完整保留在覆盖审计中。',
                               byteProof=control)
            elif proof:
                finding.update(severity='info', message='本次新发现的输入来自与批准历史包完全相同的字节；属于旧材料未收录，覆盖缺口仍保留。',
                               producerProof=proof)
            else:
                finding['message'] = '本次新发现未接入的表或字段，旧材料未收录；没有同一批准包的字节证据，需核对，不能断言官方新增。'
        elif kind == 'consumer-script-changed' and artifact:
            receipt = receipts.get(str(Path(artifact['sourcePath']).resolve()))
            old = old_artifacts.get(key, {})
            if (receipt and receipt['beforeSha256'] == old.get('sha256') and receipt['afterSha256'] == artifact['sha256'] and
                    receipt['validation'] in {'skill-literal-skeleton-identical', 'dictionary-literal-skeleton-identical',
                                              'skin-config-literal-skeleton-identical'}):
                finding.update(severity='info', message='已通过现有纯字面量及代码骨架校验；这里只改变受支持的数据，未批准消费者代码变化。',
                               dataOnlyValidation=receipt)
        findings.append(finding)
    for identity, row in old_issues.items():
        if identity not in current_issues:
            findings.append({'kind': row['kind'], 'path': _coverage_location(previous, row), 'severity': 'info',
                             'baselineStatus': 'resolved', 'message': '历史材料中的这项输入问题在本次审计中已消失。'})
    for row in (current.get('diff') or {}).get('roleReassignments', []):
        findings.append({'kind': 'existing-input-role-reassigned', 'path': row['key'], 'severity': 'info',
                         'baselineStatus': 'existing', 'byteProof': row,
                         'message': '同一实际文件的字节未变，原已记录别名成为主归属；保留历史输入证据，未当作新增表。'})
    return findings


def _audit_refreshed_inputs(prepared, arguments, *, packages, h5_records, data_changes, controls):
    before = prepared['source_coverage_before']
    report = _audit_inputs(arguments, before)
    report['materialStage'] = 'refreshed-local-inputs-after-current-resource-byte-verification'
    records = [row for _, row in packages.values()] + h5_records
    report['currentResourceVerification'] = {'resources': records,
        'scope': 'Only the listed resources were retrieved and byte-checked this run; historical companion evidence is not promoted to current official data.'}
    report['validatedDataOnlyScriptChanges'] = data_changes
    report['releaseFindings'] = _source_release_findings(report, before,
        approved={**prepared['approved'], **prepared['h5_data_approved']}, records=records,
        data_changes=data_changes, controls=controls)
    _write(prepared['root'] / 'source-coverage-after.json', report)
    return report


def _download(updater, url, path):
    if not url.startswith((FLASH, H5)):
        raise ValueError('回放只允许官方公共资源地址')
    path.parent.mkdir(parents=True, exist_ok=True)
    updater.fetch(url, path)
    data = _read(path, MAX_DOWNLOAD)
    return data, {'url': url, 'sha256': _sha(data), 'bytes': len(data)}


def current_flash_chain(updater, root):
    """The official loader uses start.v, then versiondata, then start.u.f."""
    if not hasattr(updater, 'public_resources'):
        updater.public_resources = PublicResourceSession(root, updater.fetch)
    return updater.public_resources.flash_chain(root)


def _package(updater, resource, versions, root):
    if not _RESOURCE.fullmatch(resource) or '..' in resource or resource.startswith('/'):
        raise ValueError('无效的官方资源路径')
    version = versions.get(resource, '2000')
    if not _VERSION.fullmatch(version):
        raise ValueError('无效的官方资源版本：' + resource)
    path = root / (resource.replace('/', '_') + '.swf')
    cached = updater.resource_cache.get((resource, version))
    if cached and cached[0].is_file():
        payload = _read(cached[0], MAX_DOWNLOAD)
        if _sha(payload) != cached[1]['sha256']:
            raise ValueError('本次共享下载缓存校验失败：' + resource)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        row = dict(cached[1])
    else:
        _, row = _download(updater, FLASH + resource + '~' + version + '.swf', path)
    row.update(resource=resource, version=version, file=str(path))
    return path, row


def _literal_map_skeleton(text):
    match = re.search(r'\bpublic\s+static\s+const\s+MAP\s*:\s*Object\s*=', text)
    if not match:
        raise ValueError('技能文字表声明已变化')
    parser = LiteralParser(text, match.end())
    parser.value()
    parse_map(text)  # Pure literal objects; final catalog validation checks fields.
    return re.sub(r'\s+', '', text[:match.end()] + '<data>' + text[parser.pos:])


def _dictionary_skeleton(text, expected_fields=None):
    # Permit only the known literal data object and timestamp to change. Every
    # constructor token is data or a fixed enum/constant, not an expression.
    declaration = re.search(r'\bpublic\s+static\s+const\s+datas\s*:\s*Object\s*=\s*\{', text)
    if not declaration:
        raise ValueError('精灵字典数据声明已变化')
    string = r'"(?:[^"\\]|\\.)*"'
    token = rf'(?:{string}|-?\d+(?:\.\d+)?|NO_SEND_PLACE|NO_SKIN_FUSION|PetEvoableType_NULL|PetEvoableType\.[A-Z_]+)'
    row = re.compile(rf'\s*"(\d+)"\s*:\s*PetDictionaryDataItem\.create\(\s*({token}(?:\s*,\s*{token})*)\s*\)\s*')
    position, seen = declaration.end(), set()
    empty = re.match(r'\s*(?=\})', text[position:])
    if empty:
        position += empty.end()
    while True:
        if not seen and text[position:position + 1] == '}':
            break  # A newer, empty incremental dictionary is a legal no-op.
        match = row.match(text, position)
        if match is None:
            raise ValueError('精灵字典包含尚未支持的表达式或字段')
        if match[1] in seen:
            raise ValueError('精灵字典包含重复编号')
        seen.add(match[1])
        fields = next(iter_create_args('PetDictionaryDataItem.create(' + match[2] + ')'))
        if str(int(fields[0])) != match[1]:
            raise ValueError('精灵字典键与编号不一致')
        if expected_fields is None:
            expected_fields = len(fields)
        if len(fields) != expected_fields:
            raise ValueError('精灵字典字段数量变化，需先适配解析器')
        position = match.end()
        if text[position:position + 1] == ',':
            position += 1
            continue
        if text[position:position + 1] != '}':
            raise ValueError('精灵字典包含尚未支持的数据结构')
        break
    skeleton = text[:declaration.end()] + '<data>' + text[position:]
    skeleton, count = re.subn(r'(\bconst\s+timestamp\s*:\s*Number\s*=\s*)\d+(\s*;)', r'\g<1><timestamp>\2', skeleton)
    if count != 1:
        raise ValueError('精灵字典时间戳缺失或不唯一')
    return re.sub(r'\s+', '', skeleton), expected_fields


def _skin_config_skeleton(data):
    """Exact reviewed skin schema and two JSON arrays; preserve all other bytes.

    The official original-race/display-form readers consume these field types.
    A missing optional value is represented by JSON null, not a missing key.
    Strings, skin counts and valid IDs may change without inventing semantics.
    """
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('皮肤配置包含重复字段')
            result[key] = value
        return result
    def nonfinite(value):
        raise ValueError('皮肤配置含无效数字')
    decoder = json.JSONDecoder(object_pairs_hook=object_pairs, parse_constant=nonfinite)
    text = data.decode('utf-8')  # Retain any BOM in the exact surrounding bytes.
    arrays, spans = {}, []
    for name in ('SKIN_DEFINES_SRC', 'SKIN_GROUPS_SRC'):
        matches = list(re.finditer(r'\bprivate\s+static\s+var\s+' + name + r'\s*:\s*Array\s*=\s*', text))
        if len(matches) != 1:
            raise ValueError('皮肤配置的数据声明缺失或不唯一')
        start = matches[0].end()
        value, length = decoder.raw_decode(text[start:])
        if not text[start + length:].lstrip().startswith(';') or not isinstance(value, list):
            raise ValueError('皮肤配置包含暂不支持的数据表达式')
        arrays[name] = value
        spans.append((start, start + length, name))
    scalar = {
        'id': int, 'name': str, 'sale': bool, 'price': int, 'show': bool, 'new': bool,
        'voice': int, 'poster': bool, 'fuseFix': bool, 'fuseAtLeast': int,
        'xPosterRes': bool, 'xInteractivePosterRes': bool, 'interactive': bool,
        'tryingCardId': int, 'exchangingWeight': int, 'changeableWithMovie': bool, 'confTag': str,
    }
    nullable_strings = {'xChangeableInfo', 'exchangingMaterialStr'}
    nullable_lists = {'fuse': (int,), 'changeableRaceIds': (int,),
                      'changeableNames': (str, type(None)), 'changeableVoiceDiv': (str,)}
    keys = set(scalar) | nullable_strings | set(nullable_lists) | {'availablePets'}
    rows, ids = arrays['SKIN_DEFINES_SRC'], set()
    if not rows:
        raise ValueError('官方皮肤配置为空')
    for row in rows:
        if not isinstance(row, dict) or set(row) != keys:
            raise ValueError('皮肤配置字段结构变化，需先适配解析器')
        if any(type(row[key]) is not kind for key, kind in scalar.items()):
            raise ValueError('皮肤配置字段类型变化，需先适配解析器')
        if row['id'] <= 0 or row['id'] in ids:
            raise ValueError('皮肤配置编号无效或重复')
        ids.add(row['id'])
        for key in nullable_strings:
            if row[key] is not None and not isinstance(row[key], str):
                raise ValueError('皮肤文字资料的格式变化，需先适配')
        for key, types in nullable_lists.items():
            value = row[key]
            if value is not None and (not isinstance(value, list) or any(type(item) not in types for item in value)):
                raise ValueError('皮肤列表资料的格式变化，需先适配')
            if key in {'fuse', 'changeableRaceIds'} and value is not None and any(item <= 0 for item in value):
                raise ValueError('皮肤关联资料含无效编号')
        if not isinstance(row['availablePets'], list):
            raise ValueError('皮肤适用精灵字段类型变化')
        for pet in row['availablePets']:
            if (not isinstance(pet, dict) or set(pet) != {'no', 'raceId', 'raceIdNeed'} or
                    any(type(item) is not int for item in pet.values()) or
                    pet['no'] < 0 or pet['raceId'] <= 0 or pet['raceIdNeed'] < 0):
                # raceIdNeed == 0 occurs in official poster entries; it is not
                # silently promoted to a canonical pet identity.
                raise ValueError('皮肤适用精灵结构或编号变化，需先适配')
    for group in arrays['SKIN_GROUPS_SRC']:
        if (not isinstance(group, list) or not group or
                any(type(value) is not int or value not in ids for value in group)):
            raise ValueError('皮肤分组结构或编号无效')
    for start, end, name in sorted(spans, reverse=True):
        text = text[:start] + '<' + name + '>' + text[end:]
    return text.encode('utf-8'), {'skins': len(rows), 'groups': len(arrays['SKIN_GROUPS_SRC']),
                                 'literalArrays': sorted(arrays)}


def _export_skin_package(updater, package, target):
    from public_data_updater import runtime_entry
    if updater.runtimes is None:
        updater.tools.mkdir(parents=True, exist_ok=True)
        runtime_tools = getattr(updater, 'runtime_tools', updater.tools)
        updater.runtimes = (runtime_entry(runtime_tools, 'java', updater.scratch), runtime_entry(runtime_tools, 'ffdec', updater.scratch))
    java, ffdec = updater.runtimes
    command = [str(java), '-Xmx2g', '-Djava.awt.headless=true', '-jar', str(ffdec),
               '-selectclass', 'mmo.changepetskinv3.++', '-export', 'script', str(target), str(package)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode or not (target / 'scripts/mmo/changepetskinv3').is_dir():
        raise ValueError('官方皮肤配置静态解包失败，保留旧回放资料')


def refresh_skin_package(updater, arguments, paths, packages, approved):
    """Approve only this parser's exact consumers, never the whole skin module.

    The inspected package also changed two interactive-poster UI classes which
    are not consumed by this parser. Their code is not being approved here.
    Every changed package must re-prove all five actual readers byte-for-byte.
    """
    if SKIN_RESOURCE not in packages:
        return []
    package, current = packages[SKIN_RESOURCE]
    if current['sha256'] == approved[SKIN_RESOURCE]['sha256']:
        return []
    if _sha(_read(package, MAX_DOWNLOAD)) != current['sha256']:
        raise ValueError('本次皮肤包字节与来源记录不一致')
    expected = SKIN_CONSUMER_CLASSES | {SKIN_CONFIG_CLASS}
    saved = {}
    for path in paths:
        suffix = path.as_posix().split('/scripts/', 1)
        if len(suffix) == 2 and suffix[1].startswith('mmo/changepetskinv3/') and path.suffix == '.as':
            saved.setdefault(suffix[1], []).append(path)
    if set(saved) != expected or not arguments.get('skin_source'):
        raise ValueError('更新材料缺少完整已核验皮肤消费者，或消费范围已变化')
    config_path = Path(arguments['skin_source']) / 'scripts' / SKIN_CONFIG_CLASS
    if config_path.resolve() not in {path.resolve() for path in saved[SKIN_CONFIG_CLASS]}:
        raise ValueError('皮肤配置不在已批准的输入路径中')
    # This tree is temporary evidence, outside every catalog/audit input root.
    target = updater.scratch / 'fresh-skin'
    _export_skin_package(updater, package, target)
    replacements, consumers = [], []
    for class_name in sorted(expected):
        fresh = target / 'scripts' / class_name
        after = _read(fresh)
        for old in saved[class_name]:
            before = _read(old)
            if class_name != SKIN_CONFIG_CLASS:
                if before != after:
                    raise ValueError('官方皮肤读取规则变化，需先适配')
                consumers.append({'class': class_name, 'sourcePath': str(old.resolve()),
                                  'beforeSha256': _sha(before), 'afterSha256': _sha(after)})
                continue
            old_skeleton, old_counts = _skin_config_skeleton(before)
            new_skeleton, new_counts = _skin_config_skeleton(after)
            if old_skeleton != new_skeleton:
                raise ValueError('官方皮肤配置的非数据代码变化，需先适配')
            replacements.append((old, before, after, old_counts, new_counts, _sha(new_skeleton)))
    # No source input is mutated until every reader and both schemas passed.
    changes = []
    for old, before, after, old_counts, new_counts, skeleton_hash in replacements:
        old.write_bytes(after)
        changes.append({'sourcePath': str(old.resolve()), 'beforeSha256': _sha(before), 'afterSha256': _sha(after),
            'configurationChanged': before != after, 'skeletonSha256': skeleton_hash, 'schemaContract': 'skin-config-v1',
            'resource': SKIN_RESOURCE, 'approvedProducerSha256': approved[SKIN_RESOURCE]['sha256'],
            'producerSha256': current['sha256'], 'validation': 'skin-config-literal-skeleton-identical',
            'beforeCounts': old_counts, 'afterCounts': new_counts, 'validatedConsumers': consumers,
            'scope': 'Only the five saved replay consumers are byte-identical; both configuration arrays passed the reviewed literal schema. Other package UI classes are outside this parser contract, not approved as unchanged.'})
    return changes


def _export_battleconfig(updater, package, target):
    from public_data_updater import runtime_entry
    if updater.runtimes is None:
        updater.tools.mkdir(parents=True, exist_ok=True)
        runtime_tools = getattr(updater, 'runtime_tools', updater.tools)
        updater.runtimes = (runtime_entry(runtime_tools, 'java', updater.scratch), runtime_entry(runtime_tools, 'ffdec', updater.scratch))
    java, ffdec = updater.runtimes
    command = [str(java), '-Xmx2g', '-Djava.awt.headless=true', '-jar', str(ffdec),
               '-selectclass', 'mmo.battleconfig.++', '-export', 'script', str(target), str(package)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode or not (target / 'scripts/mmo/battleconfig').is_dir():
        raise ValueError('官方战斗配置解包失败，保留旧回放资料')


def refresh_data_packages(updater, arguments, paths, packages, approved):
    """Refresh only known literal table forms under unchanged consumer code."""
    source, auxiliary = arguments['source'], arguments['auxiliary']
    changes = []
    battle_package, row = packages['battleconfig/battleconfig']
    if row['sha256'] != approved['battleconfig/battleconfig']['sha256']:
        target = updater.scratch / 'fresh-battleconfig'
        _export_battleconfig(updater, battle_package, target)
        replacements = []
        for old in paths:
            suffix = old.as_posix().split('/scripts/', 1)
            if len(suffix) != 2 or not suffix[1].startswith('mmo/battleconfig/') or old.suffix != '.as':
                continue
            new = target / 'scripts' / suffix[1]
            before, after = _read(old), _read(new)
            if before != after:
                if old.stem not in TEXT_TABLES or _literal_map_skeleton(before.decode('utf-8-sig')) != _literal_map_skeleton(after.decode('utf-8-sig')):
                    raise ValueError('官方战斗解析代码变化，需先适配：' + old.name)
            replacements.append((old, before, after))
        if not replacements:
            raise ValueError('更新材料缺少战斗解析代码依据')
        for old, before, after in replacements:
            old.write_bytes(after)
            if before != after:
                changes.append({'sourcePath': str(old.resolve()), 'beforeSha256': _sha(before),
                    'afterSha256': _sha(after), 'resource': 'battleconfig/battleconfig',
                    'producerSha256': row['sha256'], 'validation': 'skill-literal-skeleton-identical'})
    provenance_path = auxiliary / 'pet-provenance.json'
    provenance = _json(provenance_path)
    base_record = next(item for item in provenance['packages'] if item['resource'] == 'pet/petdictionarydata')
    _, dictionary_fields = _dictionary_skeleton((auxiliary / base_record['decompiledFile']).read_text(encoding='utf-8-sig'))
    if dictionary_fields is None:
        raise ValueError('官方精灵基础字典为空')
    snapshots = []
    for record in provenance['packages']:
        resource = record['resource']
        package, current = packages[resource]
        path = auxiliary / record['decompiledFile']
        if current['sha256'] != approved[resource]['sha256']:
            class_name = 'mmo.pet.petdictionarydata.PetDictionaryDataContents' + ('Update' if resource.endswith('update') else '')
            fresh = updater.export(package, updater.scratch / ('fresh-' + resource.rsplit('/', 1)[-1]), class_name)
            before, after = _read(path), _read(fresh)
            old_text, text = before.decode('utf-8-sig'), after.decode('utf-8-sig')
            old_skeleton, field_count = _dictionary_skeleton(old_text, dictionary_fields)
            if _dictionary_skeleton(text, field_count)[0] != old_skeleton:
                raise ValueError('官方精灵字典读取代码变化，需先适配')
            path.write_bytes(after)
            if before != after:
                changes.append({'sourcePath': str(path.resolve()), 'beforeSha256': _sha(before),
                    'afterSha256': _sha(after), 'resource': resource, 'producerSha256': current['sha256'],
                    'validation': 'dictionary-literal-skeleton-identical', 'constructorFieldCount': field_count})
        text = path.read_text(encoding='utf-8-sig')
        entries = parse_dictionary(text, resource)
        timestamps = re.findall(r'\bconst\s+timestamp\s*:\s*Number\s*=\s*(\d+)\s*;', text)
        if (not entries and resource == 'pet/petdictionarydata') or len(timestamps) != 1:
            raise ValueError('官方精灵字典为空或时间戳不明确')
        record.update({key: current[key] for key in ('version', 'url', 'sha256')})
        record.update(entries=len(entries), decompiledSha256=_sha(path.read_bytes()))
        snapshots.append((resource, int(timestamps[0]), entries))
    by_resource = {key: (stamp, pets) for key, stamp, pets in snapshots}
    base, update = (by_resource[name] for name in ('pet/petdictionarydata', 'pet/petdictionarydataupdate'))
    pets = dict(base[1])
    if update[0] > base[0]:
        pets.update(update[1])
    previous = _json(auxiliary / 'pets.json')
    if len(pets) < 9000 or len(pets) < len(previous) * .9:
        raise ValueError('新精灵字典明显不完整，保留旧回放资料')
    _write(provenance_path, provenance)
    _write(auxiliary / 'pets.json', pets)
    return changes


def _safe_aqz(path, target):
    with zipfile.ZipFile(path) as archive:
        rows = archive.infolist()
        if len(rows) > 4096 or sum(row.file_size for row in rows) > 128 * 1024 * 1024:
            raise ValueError('官方配置压缩包超过展开限制')
        seen, output = set(), []
        for row in rows:
            if row.is_dir():
                continue
            member = PurePosixPath(row.filename)
            pieces = row.filename.split('/')
            reserved = {'con', 'prn', 'aux', 'nul', *(f'com{i}' for i in range(1, 10)),
                        *(f'lpt{i}' for i in range(1, 10))}
            if (row.orig_filename != row.filename or member.is_absolute() or '..' in member.parts or '\\' in row.orig_filename or
                    any(part in ('', '.', '..') or part.rstrip(' .') != part or
                        any(ord(c) < 32 or c in '<>:"|?*' for c in part) or
                        part.split('.')[0].casefold() in reserved for part in pieces) or
                    row.filename.casefold() in seen or ((row.external_attr >> 16) & 0o170000) == 0o120000):
                raise ValueError('官方配置压缩包包含不安全或重复的路径')
            seen.add(row.filename.casefold())
            if row.file_size > MAX_DOWNLOAD:
                raise ValueError('官方配置单表超过大小限制')
            with archive.open(row) as stream:
                data = stream.read(MAX_DOWNLOAD + 1)
            if len(data) != row.file_size:
                raise ValueError('官方配置表长度不一致')
            destination = target / Path(*member.parts)
            if not destination.resolve().is_relative_to(target.resolve()):
                raise ValueError('官方配置输出越过临时目录')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            output.append({'member': row.filename, 'file': 'h5-tables/' + row.filename,
                           'bytes': len(data), 'sha256': _sha(data)})
    return output


def refresh_h5(updater, arguments, consumers, now):
    source, cultivation = arguments['source'], arguments['cultivation_h5_source']
    chain = updater.public_resources.h5_chain(source)
    start, entry, base = chain['start'], chain['entry'], chain['manifest']
    records = [chain['main']]
    def retrieve(resource, path):
        _, record = updater.public_resources.h5_resource(resource, path)
        records.append(record)
        return record
    for resource, prior in sorted(consumers.items()):
        record = retrieve(resource, source / 'fresh-h5-consumers' / Path(resource))
        if record['sha256'] != prior['sha256']:
            raise ValueError('官方 H5 解析代码变化，需先适配：' + resource)
    aqz = source / 'h5-battleconfig.aqz'
    record = retrieve('config/battleconfig.aqz', aqz)
    # Old tables must not survive a missing table in the new archive.
    target = source / 'h5-tables'
    if target.exists():
        if not target.resolve().is_relative_to(source.resolve()):
            raise ValueError('H5 临时目录越界')
        shutil.rmtree(target)
    members = _safe_aqz(aqz, target)
    for name in ('skill_config.json', 'skill_desc_config.json'):
        matches = list(target.rglob(name))
        if len(matches) != 1:
            raise ValueError('官方 H5 必要表缺失或不唯一：' + name)
        shutil.copyfile(matches[0], source / name)
    _write(source / 'h5-provenance.json', {'retrievedUtc': now, 'version': record['version'],
        'files': [dict(entry, file='h5-start.json'), dict(base, file='h5-version.json'), dict(record, file='h5-battleconfig.aqz')],
        'members': members})
    proof = _json(cultivation / 'provenance.json')
    for row in proof['files']:
        resource = row['resource']
        if not re.fullmatch(r'config/[A-Za-z0-9_/]+\.json', resource):
            raise ValueError('未知的官方养成资料路径')
        path = cultivation / 'raw' / resource
        row.update(retrieve(resource, path))
        row['file'] = str(path)
        row.pop('matchesPreviouslyVerifiedBytes', None)
    proof.update(retrievedUtc=now, manifestSha256=entry['sha256'], baseManifestSha256=base['sha256'])
    _write(cultivation / 'provenance.json', proof)
    return records, start


def _validate_pack(path):
    raw = unpack_bytes(_read(path, MAX_PACK_BYTES))
    validate_catalog_bytes(raw)
    return json.loads(raw)


def _code_fingerprint(bundle):
    code = [Path(__file__), Path(__file__).with_name('public_data_updater.py'),
            Path(__file__).with_name('public_resource_session.py'),
            Path(__file__).with_name('public_catalog_quality.py'),
            Path(__file__).with_name('public_replay_fidelity.py'),
            Path(__file__).with_name('public_replay_regression.py'),
            Path(__file__).with_name('generate_pet_detail_data.py'), *sorted(MODULES.glob('*.py'))]
    return _sha(json.dumps([(p.name, _sha(_read(p))) for p in code] + [('sourceBundle', _sha(_read(bundle)))],
                           separators=(',', ':')).encode())


def prepare_replay_update(updater, *, bundle_path=None):
    from public_data_updater import emit
    bundle = Path(bundle_path) if bundle_path else MODULES / 'update-sources.zip'
    cached = getattr(updater, '_prepared_replay', None)
    if cached and cached['bundle'] == bundle:
        return cached
    if not bundle.is_file():
        raise ValueError('完整回放更新材料缺失，请使用包含回放更新器的新版工作台')
    baseline = _validate_pack(updater.baseline / 'battle-replay-catalog.brpack')
    root = updater.scratch / 'replay-update'
    root.mkdir(parents=True, exist_ok=True)
    emit('progress', message='正在校验并准备完整回放资料的官方解析依据')
    restored = extract_bundle(bundle, root / 'sources')
    arguments, paths = restored['arguments'], sorted(set(restored['pathMap'].values()))
    project_root = root / 'sources/project'
    approved_sources = {str(root / 'sources' / row['member']): row['sha256']
                        for row in restored['manifest']['files'] if row['member'].endswith(('.as', '.js'))}
    approved, h5_consumers = dependency_records(paths)
    source_coverage_before = _audit_inputs(arguments)
    source_coverage_before['materialStage'] = 'historical-bundle-input-subset'
    _write(root / 'source-coverage-before.json', source_coverage_before)
    result = dict(bundle=bundle, baseline=baseline, root=root, arguments=arguments, paths=paths,
                  project_root=project_root, approved_sources=approved_sources,
                  approved=approved, h5_consumers=h5_consumers,
                  h5_data_approved=_h5_data_approvals(paths), source_coverage_before=source_coverage_before)
    updater._prepared_replay = result
    return result


def replay_dependencies(prepared):
    rows = [{'kind': 'flash', 'resource': key, 'formats': ['swf'],
             'components': ['battleReplay'], 'reason': 'replay-official-provenance'}
            for key in prepared['approved']]
    keys = {*prepared['h5_consumers'], 'js/main.js', 'config/battleconfig.aqz'}
    proof = _json(prepared['arguments']['cultivation_h5_source'] / 'provenance.json')
    keys.update(row['resource'] for row in proof['files'])
    rows.extend({'kind': 'h5', 'resource': key, 'components': ['battleReplay'],
                 'reason': 'replay-official-provenance'} for key in keys)
    return rows


def update_battle_replay(updater, *, bundle_path=None):
    from public_data_updater import emit
    from public_replay_fidelity import audit_replay_fidelity
    from public_replay_regression import audit_replay_regression
    from build_catalog import build
    from update_generated_evidence import prepare_reference_and_race
    prepared = prepare_replay_update(updater, bundle_path=bundle_path)
    bundle, baseline, root = (prepared[k] for k in ('bundle', 'baseline', 'root'))
    arguments, paths, project_root = (prepared[k] for k in ('arguments', 'paths', 'project_root'))
    approved_sources, approved, h5_consumers = (prepared[k] for k in ('approved_sources', 'approved', 'h5_consumers'))
    now = datetime.now(timezone.utc).isoformat()
    emit('progress', message='正在按官方加载顺序核对回放资源版本')
    versions, start, chain = current_flash_chain(updater, root)
    updater.start_version = chain['versiondata']['version']
    overrides = {item.get('n') for item in ET.fromstring(start).findall('./u/f')}
    emit('progress', message=f'正在核对 {len(approved)} 项官方回放资源，包含技能、Buff、精灵与称号')
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda name: _package(updater, name, versions, project_root / 'current-official'), sorted(approved)))
    packages = {row['resource']: (path, row) for path, row in results}
    for name, (path, row) in packages.items():
        row['resolution'] = 'start-override' if name in overrides else 'versiondata' if name in versions else 'default-2000'
        if name not in DATA_PACKAGES and name != SKIN_RESOURCE and row['sha256'] != approved[name]['sha256']:
            raise ValueError('官方依赖已变化，需先适配回放解析器，已保留旧资料：' + name + '；新 SHA256 ' + row['sha256'])
    emit('progress', message='正在更新官方技能、Buff 与精灵配置，并检查解析代码是否兼容')
    data_changes = refresh_skin_package(updater, arguments, paths, packages, approved)
    data_changes.extend(refresh_data_packages(updater, arguments, paths, packages, approved))
    source, verified = arguments['source'], arguments['verified_source']
    source.joinpath('start.xml').write_bytes(start)
    shutil.copyfile(root / 'versiondata.swf', source / 'versiondata.swf')
    verified.joinpath('play-start.xml').write_bytes(start)
    shutil.copyfile(root / 'versiondata.swf', verified / 'versiondata.swf')
    proof = {'retrievedUtc': now, 'manifestSha256': chain['entry']['sha256'],
             'versiondataSha256': chain['versiondata']['sha256'], 'files': []}
    for name, (path, row) in packages.items():
        if name in {'stargodv3/stargodservice', 'petbadge/petbadgeservice', 'astrolabe/astrolabeservice'}:
            # Cultivation verifies the saved XML against the current SWF's
            # exact embedded payload. Recreate both files AND their proof.
            save_binary_payloads(_read(path, MAX_DOWNLOAD), verified / 'flash-resolved', name, row)
        proof['files'].append(dict(row))
    _write(verified / 'flash-resolved/provenance.json', proof)
    original = _json(source / 'provenance.json')
    provenance = {'retrievedUtc': now, 'manifestUrl': chain['entry']['url'],
                  'manifestSha256': chain['entry']['sha256'], 'versiondata': chain['versiondata'],
                  'packages': [], 'binary': {}, 'xml': {}}
    for directory in ('xml', 'binary'):
        target = source / directory
        if target.exists():
            if not target.resolve().is_relative_to(source.resolve()):
                raise ValueError('回放临时解包目录越界')
            shutil.rmtree(target)
    for previous in original['packages']:
        name = previous['resource']
        path, row = packages[name]
        destination = source / (name.replace('/', '_') + '.swf')
        shutil.copyfile(path, destination)
        provenance['packages'].append(dict(row, file=destination.name))
        save_binary_payloads(_read(path, MAX_DOWNLOAD), source, name, provenance)
    _write(source / 'provenance.json', provenance)
    h5_records, h5_start = refresh_h5(updater, arguments, h5_consumers, now)
    h5_chain = updater.public_resources.h5_chain()
    controls = {'source:start.xml': chain['entry'], 'verified_source:play-start.xml': chain['entry'],
                'source:h5-start.json': h5_chain['entry'], 'source:h5-version.json': h5_chain['manifest']}
    fingerprint = _sha(json.dumps(sorted((r['resource'], r['sha256']) for r in [*proof['files'], *h5_records]),
                                  separators=(',', ':')).encode())
    parser_sha = _code_fingerprint(bundle)
    active = updater.catalog / 'battle-replay-catalog.brpack'
    previous = None
    if active.is_file():
        try:
            previous = _validate_pack(active)
        except (OSError, ValueError):
            pass  # Rebuild a corrupt external package from verified inputs.
    def stable_versions():
        updater.public_resources.assert_stable(('flash', 'h5'))
    metadata = {'sourceFingerprint': fingerprint, 'parserSha256': parser_sha, 'format': PARSER_VERSION}
    if 'battleReplay' not in getattr(updater, 'force_components', ()) and previous and all(previous.get('publicUpdate', {}).get(key) == value for key, value in metadata.items()):
        coverage = _audit_refreshed_inputs(prepared, arguments, packages=packages, h5_records=h5_records,
                                          data_changes=data_changes, controls=controls)
        emit('progress', message='正在逐项核对官方原文，并用真实战报检查回放过程')
        fidelity = audit_replay_fidelity(previous, build_arguments=arguments, previous=previous)
        regression = audit_replay_regression(active, previous, report_directory=root)
        updater.assess_quality('battleReplay', previous, previous, source_coverage=coverage,
                               source_fidelity=fidelity, replay_regression=regression)
        stable_versions()
        return False, {'counts': {key: len(previous[key]) for key in ('pets', 'skills', 'buffs')},
                       'packBytes': active.stat().st_size, 'verifiedResources': len(packages) + len(h5_records)}
    emit('progress', message='正在重建完整回放关联资料，并检查所有必要表')
    arguments = prepare_reference_and_race(arguments, project_root=project_root,
        approved_sources=approved_sources, generated_utc=now,
        current_packages={name: row for name, (_, row) in packages.items()})
    arguments['exports'] = root / 'linked'
    catalog = build(**arguments)
    for key in ('pets', 'skills', 'buffs'):
        minimum_reference = max(len(baseline[key]), len(previous[key]) if previous else 0)
        if len(catalog[key]) < minimum_reference * .9:
            raise ValueError('新回放资料明显不完整，保留旧版本：' + key)
    catalog['publicUpdate'] = dict(metadata, verifiedUtc=now, verifiedResources=len(packages) + len(h5_records),
                                   h5Sources=h5_records,
                                   consumerPolicy='Changed unreviewed consumers require a parser update; no source semantics are guessed.')
    raw = json.dumps(catalog, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode('utf-8')
    validate_catalog_bytes(raw)
    candidate = root / 'candidate.json'
    candidate.write_bytes(raw)
    coverage = _audit_refreshed_inputs(prepared, arguments, packages=packages, h5_records=h5_records,
                                      data_changes=data_changes, controls=controls)
    emit('progress', message='正在逐项核对官方原文，并用真实战报检查回放过程')
    fidelity = audit_replay_fidelity(catalog, build_arguments=arguments, previous=previous if previous is not None else baseline)
    regression = audit_replay_regression(candidate, catalog, report_directory=root)
    updater.assess_quality('battleReplay', catalog, previous if previous is not None else baseline,
                           source_coverage=coverage, source_fidelity=fidelity, replay_regression=regression)
    # Readback and full validation happen inside pack_file BEFORE atomic replace.
    # The final version check precedes the only write to the active catalog.
    stable_versions()
    emit('progress', message='全部校验通过，正在保存压缩回放资料')
    pack_file(candidate, active)
    return True, {'counts': {key: len(catalog[key]) for key in ('pets', 'skills', 'buffs')},
                  'packBytes': active.stat().st_size, 'jsonBytes': len(raw),
                  'verifiedResources': len(packages) + len(h5_records)}
