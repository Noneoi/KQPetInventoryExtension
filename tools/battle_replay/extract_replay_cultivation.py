"""Read official cultivation reference data without inferring battle equipment.

``extract_replay_cultivation(source, h5_source, consumer_source=None)`` accepts
an effect-origin audit root (with ``flash-resolved``), an ``h5-all-config`` root,
and optionally a separate audit root containing the verified AS consumers.
It returns ``stargods``, ``badges`` and ``astrolabe`` groups; each has an ID-keyed
``definitions`` map with readable fields, ordered levels, raw fields and sources.
The caller owns catalog writes. Neither SWF/AS/JS nor description expressions
are executed. IDs in these namespaces are never joined to battle Buff IDs.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

from fetch_reference import binary_payloads


TABLES = {
    'stargods': ('星神', 'stargodv3/stargodservice',
                 'mmo.stargodv3.other.Sgsv3Config_sXMLClass',
                 'config/stargod/stargodconfig.json'),
    'badges': ('元魂', 'petbadge/petbadgeservice',
               'mmo.petbadge.config.PetBadgeConfig_sXMLClass',
               'config/petbadge/petbadgeconfig.json'),
    'astrolabe': ('天迹星轮', 'astrolabe/astrolabeservice',
                  'mmo.astrolabe.config.AstrolabeConfig_sXMLClass',
                  'config/astrolabe/astrolabeconfig.json'),
}
LABELS = {
    'defineId': '定义编号', 'name': '名称', 'pinYinChain': '官方检索拼音',
    'supplyExp': '提供经验', 'level': '等级', 'exp': '等级经验',
    'battlePower': '配置战斗力', 'battleProperty': '属性配置', 'desc': '效果说明',
    'effectDesc': '效果说明', 'propsStr': '属性配置',
    'materialStrToActivate': '激活材料', 'materialStrToUpgrade': '升级材料',
    'iconId': '图标资源编号', 'isTBD': '待开放标记', 'allSigns': '官方分类',
    'locatedTypeId': '位置类型编号', 'exclusive': '专属标记',
    'basicBuff': '点亮属性', 'twinkleBuff': '选中效果说明',
    'twinkleProps': '选中属性', 'lightUpCost': '点亮材料',
}
BASE_FIELDS = {
    'stargods': {'defineId', 'name', 'pinYinChain', 'supplyExp'},
    'badges': {'defineId', 'name', 'type'},
    'astrolabe': {'defineId', 'iconId', 'name', 'isTBD', 'allSigns',
                  'locatedTypeId', 'exclusive', 'basicBuff', 'twinkleBuff',
                  'lightUpCost', 'battlePower', 'twinkleProps'},
}
LEVEL_FIELDS = {
    'stargods': {'level', 'exp', 'battlePower', 'battleProperty', 'desc'},
    'badges': {'level', 'name', 'battlePower', 'effectDesc', 'propsStr',
               'materialStrToUpgrade'},
}
EXCLUSIVE_FIELDS = {'effectDesc', 'propsStr', 'battlePower', 'materialStrToActivate'}
APPLICABILITY = {
    'status': 'reference-only-until-equipment-snapshot',
    'label': '官方养成资料',
    'missingInputLabel': '战报未记录装备快照，不能确认本场装备、等级或是否触发。',
    'requiredInputs': {
        'stargods': ['有效星神槽', '星神定义编号', '星神等级'],
        'badges': ['元魂职业定义编号', '元魂等级', '专属元魂编号', '专属激活状态'],
        'astrolabe': ['星迹定义编号', '点亮状态', '选中状态'],
    },
    'battleBuffBridge': 'not-established',
    'boundary': '同编号、名称相似、属性值或战报共现均不能证明养成项与战斗 Buff 的对应关系。',
}


def _read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _source(path, **extra):
    path = Path(path)
    data = path.read_bytes()
    return {'file': path.resolve().as_posix(), 'sha256': _sha(data),
            'bytes': len(data), **extra}


def _verified_file(base, record):
    """Both existing manifests use paths relative to their parent directory."""
    filename = record.get('file', record.get('path'))
    if not filename or not record.get('sha256'):
        raise ValueError('source manifest is missing file/sha256')
    path = Path(filename)
    if not path.is_absolute():
        candidates = [Path(base) / path, Path(base).parent / path]
        matches = [p for p in candidates if p.is_file()]
        if len(matches) != 1:
            raise ValueError(f'ambiguous or missing source: {filename}')
        path = matches[0]
    data = path.read_bytes()
    if _sha(data) != record['sha256']:
        raise ValueError(f'source hash mismatch: {path}')
    return path, data


def _id(value, label):
    if not isinstance(value, str) or not re.fullmatch(r'0|[1-9][0-9]*', value):
        raise ValueError(f'invalid {label}: {value!r}')
    return value


def _required(row, fields, context):
    missing = fields - row.keys()
    if missing:
        raise ValueError(f'{context}: missing fields {sorted(missing)}')


def _scalar(value):
    if value is None:
        return 'null'
    if isinstance(value, bool):
        return 'true' if value else 'false'
    return str(value)


def _normal(value):
    if isinstance(value, dict):
        return {k: _normal(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_normal(v) for v in value]
    return _scalar(value)


def _xml_rows(text):
    root = ET.fromstring(text)
    if root.tag != 'config' or any(row.tag != 's' for row in root):
        raise ValueError('unsupported cultivation XML root/row schema')
    rows = []
    for row in root:
        if any(child.tag != 'l' or len(child) for child in row):
            raise ValueError('unsupported cultivation child schema')
        item = dict(row.attrib)
        if len(row):
            item['l'] = [dict(level.attrib) for level in row]
        rows.append(item)
    return rows


def compare_h5(rows, h5_rows, label):
    """Compare complete rows by explicit defineId, preserving level order."""
    if not isinstance(h5_rows, list):
        raise ValueError(f'{label}: H5 table is not an array')
    maps = []
    for values in (rows, _normal(h5_rows)):
        result = {}
        for row in values:
            if not isinstance(row, dict):
                raise ValueError(f'{label}: invalid H5 row')
            key = _id(row.get('defineId'), 'defineId')
            if key in result:
                raise ValueError(f'{label}: duplicate defineId {key}')
            # H5 may spell the empty child collection explicitly.
            result[key] = {k: v for k, v in row.items() if not (k == 'l' and v == [])}
        maps.append(result)
    different = [key for key in sorted(set(maps[0]) | set(maps[1]), key=int)
                 if maps[0].get(key) != maps[1].get(key)]
    if different:
        raise ValueError(f'{label}: Flash/H5 mismatch for IDs {different[:12]}')
    return {'status': 'all-fields-match', 'definitionsCompared': len(maps[0]),
            'levelRowsCompared': sum(len(row.get('l', [])) for row in rows)}


def parse_property_types(text, source):
    """Only literal official constructors; no AS evaluation or alias guessing."""
    pattern = re.compile(r'public static const (\w+):PetBattlePropertyType\s*=\s*'
                         r'PetBattlePropertyType\.(create|createContainer)\(\s*'
                         r'(-?\d+)\s*,\s*("(?:[^"\\]|\\.)*")')
    result = {}
    for match in pattern.finditer(text):
        name = json.loads(match[4])
        if name in result:
            raise ValueError(f'duplicate battle property name: {name}')
        result[name] = {'id': match[3], 'name': name, 'constant': match[1],
                        'kind': match[2], 'source': {**source,
                        'line': text.count('\n', 0, match.start()) + 1}}
    if not result or len(result) != len(re.findall(
            r'public static const \w+:PetBattlePropertyType\s*=', text)):
        raise ValueError('unsupported PetBattlePropertyType schema')
    return result


def property_readings(raw, *, equipment_style=False, property_types=None):
    """Retain exact terms; resolve property names, never compute battle totals."""
    if raw in ('', 'null'):
        return []
    result = []
    for token in raw.split('#'):
        row = {'raw': token, 'namespace': 'battle-property'}
        match = re.fullmatch(r'([^+#\-]+)(\+?-|\+)([0-9]+(?:\.[0-9]+)?)', token)
        if not match:
            row['status'] = 'unparsed-property-token'
        else:
            name, operator, number = match.groups()
            # Official addSingle handles a minus first, then removes a preceding
            # plus from the property name (e.g. 全属性率+-0.2).
            operator = '-' if '-' in operator else '+'
            lookup = name + '装备' if equipment_style and '率' not in name else name
            row.update(name=name, operator=operator, value=number, lookupName=lookup)
            entry = (property_types or {}).get(lookup)
            row['status'] = 'resolved' if entry else 'property-name-not-resolved'
            if entry:
                row.update(propertyId=entry['id'], source=entry['source'])
        result.append(row)
    return result


def _readings(raw, kind):
    result = []
    for field, value in raw.items():
        if field == 'l':
            continue
        if field == 'type' and kind == 'badges':
            label, value = '元魂类型', {'0': '职业元魂', '1': '专属元魂'}[value]
        else:
            label = LABELS.get(field, field)
            if field in ('isTBD', 'exclusive'):
                value = {'0': '否', '1': '是'}.get(value, value)
            elif value == '':
                value = '未配置'
            elif field in ('battleProperty', 'propsStr', 'twinkleProps') and value == 'null':
                value = '未配置（官方值 null）'
        result.append({'label': label, 'value': value})
    return result


def parse_cultivation_table(kind, text, source, *, property_types=None):
    """Pure parser used by the verified extractor and small regression fixtures."""
    if kind not in TABLES:
        raise ValueError(f'unknown cultivation namespace: {kind}')
    definitions = {}
    for index, raw in enumerate(_xml_rows(text), 1):
        _required(raw, BASE_FIELDS[kind], f'{kind} row {index}')
        identifier = _id(raw['defineId'], 'defineId')
        if identifier in definitions:
            raise ValueError(f'{kind}: duplicate defineId {identifier}')
        levels = raw.get('l', [])
        if kind == 'badges':
            if raw['type'] not in ('0', '1'):
                raise ValueError(f'unknown official badge type: {raw["type"]}')
            if raw['type'] == '1':
                _required(raw, EXCLUSIVE_FIELDS, f'badges {identifier}')
                if levels:
                    raise ValueError('exclusive badge unexpectedly has levels')
            elif not levels:
                raise ValueError('job badge has no level definitions')
        if kind == 'astrolabe' and levels:
            raise ValueError('astrolabe unexpectedly has levels')
        here = {**source, 'row': index, 'path': f'/config/s[{index}]'}
        row = {'id': identifier, 'namespace': kind, 'name': raw['name'],
               'description': raw.get('effectDesc', raw.get('twinkleBuff', '')),
               'raw': raw, 'source': here, 'readings': _readings(raw, kind),
               'levels': {}, 'references': [], 'propertyReadings': [],
               'requiredInputs': list(APPLICABILITY['requiredInputs'][kind])}
        if kind == 'badges':
            row['type'] = 'job' if raw['type'] == '0' else 'exclusive'
            row['condition'] = ('按战报所记录的元魂等级读取。' if raw['type'] == '0'
                                else '需记录专属元魂编号及其激活状态；未激活不计入该项属性。')
        elif kind == 'stargods':
            row['condition'] = '需记录有效星神槽中的定义编号和等级。'
        else:
            row['condition'] = '点亮后读取基础属性；选中时另读取选中属性。'
            row['propertyReadings'] = [
                {'field': field, 'condition': condition, 'terms': property_readings(
                    raw[field], equipment_style=True, property_types=property_types)}
                for field, condition in (('basicBuff', 'lighted'), ('twinkleProps', 'lighted-and-selected'))]
            row['references'].extend([
                {'namespace': 'astrolabe-icon', 'id': raw['iconId'] or identifier,
                 'field': 'iconId', 'status': 'resource-not-in-this-table',
                 'rule': '空值使用自身 defineId；图标编号不指向战斗 Buff。'},
                {'namespace': 'astrolabe-location', 'id': raw['locatedTypeId'],
                 'field': 'locatedTypeId', 'status': 'consumer-enum'}])
        if kind == 'badges' and raw['type'] == '1':
            row['propertyReadings'] = [{'field': 'propsStr', 'condition': 'exclusive-activated',
                'terms': property_readings(raw['propsStr'], equipment_style=True,
                                          property_types=property_types)}]
        for pos, level in enumerate(levels, 1):
            _required(level, LEVEL_FIELDS[kind], f'{kind} {identifier} level {pos}')
            number = _id(level['level'], 'level')
            # The official consumers index the level vector by level - 1.
            if int(number) != pos:
                raise ValueError(f'{kind} {identifier}: non-sequential or duplicate level {number}')
            field = 'battleProperty' if kind == 'stargods' else 'propsStr'
            row['levels'][number] = {
                'level': number, 'name': level.get('name', raw['name']),
                'description': level.get('desc', level.get('effectDesc', '')),
                'raw': level, 'source': {**here, 'path': here['path'] + f'/l[{pos}]'},
                'readings': _readings(level, kind),
                'propertyReadings': property_readings(level[field],
                    equipment_style=kind == 'badges', property_types=property_types),
            }
        if levels:
            row['description'] = '\n'.join(f'{number}级：{level["description"]}'
                                            for number, level in row['levels'].items())
        definitions[identifier] = row
    return definitions


def verify_consumers(texts):
    """Fail closed when the verified field/condition interpretation changes."""
    guards = {
        'stargod-service': ('param1.isAvailable()', 'addByStandardString(',
            'Sgsv3Config.getStarGodExtInfo(param1.defineId).levelInfo(param1.level).battleProperties'),
        'stargod-level': ('_level = param1["level"]', '_battleProperties = param1["battleProperty"]',
                          '_desc = param1["desc"]'),
        'stargod-item': ('getStarGodItemById(this._defineId)',),
        'badge-service': ('getBadgeJobLevelDefine(param1.jobId,param1.jobLevel).propsStr',
                          'if(param1.isActivateExclusive)', 'getBadgeExclusiveDefine(param1.exclusiveId).propsStr'),
        'badge-constants': ('BUFF_TYPE_JOB:int = 0', 'BUFF_TYPE_EXCLUSIVE:int = 1'),
        'astrolabe-service': ('filter(PetAstrolabe.FT_TRACE_LIGHT)',
            'getTraceDefine().basicProps', 'if(param1.isTwinkled)', 'getTraceDefine().twinkleProps'),
        'astrolabe-definition': ('_basicBuff = param1["basicBuff"]',
            '_twinkleBuff = param1["twinkleBuff"]', '_twinkleProps = param1["twinkleProps"]',
            'function get basicProps() : String', 'return this.basicBuff;'),
        'property-source': ('param1.indexOf("率") != -1 ? param1 : param1 + "装备"',
                             'PetBattlePropertyType.getByName(_loc4_)'),
    }
    for key, needles in guards.items():
        text = texts.get(key, '')
        compact = re.sub(r'\s+', '', text)
        for needle in needles:
            if re.sub(r'\s+', '', needle) not in compact:
                raise ValueError(f'unsupported cultivation consumer {key}: {needle}')


def _consumers(root, package_hashes):
    """Read recorded exports, verifying both consumer bytes and package identity."""
    root = Path(root)
    texts, sources = {}, {}
    identities = _read(root / 'stargod/source-identities.json')
    if identities['flashPackages'][0]['sha256'] != package_hashes['stargodv3/stargodservice']:
        raise ValueError('stargod consumers belong to a different package')
    chain = _read(root / 'stargod/official-consumer-chain.json')
    star_names = {'StarGodService.as': 'stargod-service',
                  'Sgsv3StarGodLevelInfo.as': 'stargod-level',
                  'Sgsv3StarGodImpl.as': 'stargod-item'}
    for record in chain:
        item = record['source']
        key = star_names.get(Path(item.get('path', '')).name)
        if key is None:
            continue
        path, data = _verified_file(root, item)
        texts[key] = data.decode('utf-8-sig')
        sources[key] = _source(path, packageSha256=package_hashes['stargodv3/stargodservice'])
    manifest = _read(root / 'soul-wheel/decompile-provenance.json')
    names = {'PetBadgeService.as': 'badge-service', 'PetBadgeConst.as': 'badge-constants',
             'AstrolabeService.as': 'astrolabe-service', 'AstrolabeTraceDefine.as': 'astrolabe-definition',
             'BattlePropertySource.as': 'property-source', 'PetBattlePropertyType.as': 'property-types',
             'AstrolabeTraceLocatedType.as': 'astrolabe-location'}
    expected = {'petbadge_petbadgeservice.swf': package_hashes['petbadge/petbadgeservice'],
                'astrolabe_astrolabeservice.swf': package_hashes['astrolabe/astrolabeservice']}
    if 'library/interfaces' in package_hashes:
        expected['library_interfaces.swf'] = package_hashes['library/interfaces']
    for package in manifest['packages']:
        package_name = Path(package['package']).name
        if package_name in expected and package['sha256'] != expected[package_name]:
            raise ValueError(f'cultivation consumers belong to a different {package_name}')
        for item in package['files']:
            key = names.get(Path(item['file']).name)
            if key is None:
                continue
            path, data = _verified_file(root, item)
            if key in texts:
                raise ValueError(f'duplicate consumer export: {key}')
            texts[key] = data.decode('utf-8-sig')
            sources[key] = _source(path, packageSha256=package['sha256'])
    verify_consumers(texts)
    if 'property-types' not in texts or 'astrolabe-location' not in texts:
        raise ValueError('missing official property/location type consumer')
    return texts, sources


def extract_replay_cultivation(source, h5_source, *, consumer_source=None):
    source, h5_source = Path(source), Path(h5_source)
    flash = source / 'flash-resolved'
    manifest = _read(flash / 'provenance.json')
    h5_manifest = _read(h5_source / 'provenance.json')
    packages = {r['resource']: r for r in manifest['files']}
    h5_records = {r['resource']: r for r in h5_manifest['files']}
    xmls, sources, comparisons, hashes = {}, {}, {}, {}
    for key, (_, resource, symbol, h5_resource) in TABLES.items():
        package = packages[resource]
        package_path, data = _verified_file(flash, package)
        hashes[resource] = _sha(data)
        payloads = dict(binary_payloads(data))
        if symbol not in payloads:
            raise ValueError(f'{resource}: embedded XML symbol not found: {symbol}')
        xml_path, xml_data = _verified_file(flash, package['xml'][symbol])
        if xml_data != payloads[symbol]:
            raise ValueError(f'{resource}: saved XML differs from actual SWF payload')
        h5_record = h5_records[h5_resource]
        h5_path, h5_data = _verified_file(h5_source, h5_record)
        rows = _xml_rows(xml_data)
        comparisons[key] = compare_h5(rows, json.loads(h5_data.decode('utf-8-sig')), key)
        xmls[key] = xml_data
        sources[key] = {
            'flash': _source(xml_path, resource=resource, symbol=symbol,
                version=str(package['version']), packageSha256=hashes[resource],
                url=package['url'], packageFile=package_path.resolve().as_posix(),
                payloadVerified=True),
            'h5': _source(h5_path, resource=h5_resource, version=str(h5_record['version']),
                          url=h5_record['url']),
        }
    # A fresh full reference bundle also supplies interfaces. Verify its bytes
    # before reusing the old property and equipment-slot consumer exports.
    interface_source = None
    if 'library/interfaces' in packages:
        interface_path, interface_data = _verified_file(flash, packages['library/interfaces'])
        hashes['library/interfaces'] = _sha(interface_data)
        interface_source = _source(interface_path, resource='library/interfaces',
            version=str(packages['library/interfaces']['version']),
            url=packages['library/interfaces']['url'])
    texts, consumers = _consumers(consumer_source or source, hashes)
    properties = parse_property_types(texts['property-types'], consumers['property-types'])
    result = {'schemaVersion': 1, 'sources': sources, 'consumers': consumers,
              'verification': {'flashH5': comparisons,
                'mode': 'static-only', 'consumerGuards': 'passed',
                'reusedConsumerPackageHashes': hashes,
                'currentInterfacesPackage': interface_source,
                'flashManifest': _source(flash / 'provenance.json'),
                'h5Manifest': _source(h5_source / 'provenance.json')},
              'battleApplicability': APPLICABILITY, 'links': [], 'propertyTypes': properties}
    item_record = h5_records['config/materialdata/stargoditem.json']
    item_path, item_data = _verified_file(h5_source, item_record)
    items = json.loads(item_data.decode('utf-8-sig'))
    result['sources']['stargodItems'] = _source(item_path,
        resource=item_record['resource'], version=str(item_record['version']), url=item_record['url'])
    locations = {match[2]: {'constant': match[1], 'name': json.loads(match[3])}
                 for match in re.finditer(r'public static const (\w+):AstrolabeTraceLocatedType\s*'
                     r'=\s*enum__\((\d+)\s*,\s*("(?:[^"\\]|\\.)*")\)', texts['astrolabe-location'])}
    if not locations:
        raise ValueError('unsupported astrolabe location enum')
    result['astrolabeLocationTypes'] = locations
    counts = {}
    unmatched = []
    for key, (name, _, _, _) in TABLES.items():
        definitions = parse_cultivation_table(key, xmls[key], sources[key]['flash'],
                                              property_types=properties)
        result[key] = {'name': name, 'definitions': definitions}
        for identifier, row in definitions.items():
            if key == 'stargods':
                item = items.get(identifier)
                ref = {'namespace': 'stargod-item', 'id': identifier, 'field': 'defineId',
                       'status': 'resolved' if item is not None else 'missing-item-definition',
                       'source': result['sources']['stargodItems'],
                       'consumer': consumers['stargod-item']}
                row['references'].append(ref)
                if item is not None:
                    # Positional material data is preserved; its columns are not guessed.
                    row['itemMetadataRaw'] = item
            elif key == 'astrolabe':
                location_ref = row['references'][1]
                location = locations.get(location_ref['id'])
                location_ref['status'] = 'resolved' if location else 'unknown-location-type'
                location_ref['source'] = consumers['astrolabe-location']
                if location:
                    row['readings'].append({'label': '位置类型',
                        'value': location['name'] + '（' + location_ref['id'] + '）'})
            for ref in row['references']:
                result['links'].append({'fromNamespace': key, 'fromId': identifier, **ref})
                if ref['status'] not in ('resolved', 'resource-not-in-this-table'):
                    unmatched.append({'namespace': key, 'id': identifier, 'reference': ref})
            for field in row['propertyReadings']:
                for term in field['terms']:
                    if term['status'] != 'resolved':
                        unmatched.append({'namespace': key, 'id': identifier, 'field': field['field'], 'term': term})
            for level, value in row['levels'].items():
                for term in value['propertyReadings']:
                    if term['status'] != 'resolved':
                        unmatched.append({'namespace': key, 'id': identifier, 'level': level, 'term': term})
        counts[key] = {'definitions': len(definitions),
            'levels': sum(len(r['levels']) for r in definitions.values()),
            'withoutDescription': sum(not r['description'] for r in definitions.values())}
    result['summary'] = {'groups': counts, 'explicitReferenceLinks': len(result['links']),
        'referenceStatusCounts': dict(Counter(r['status'] for r in result['links'])),
        'referenceNamespaceCounts': dict(Counter(r['namespace'] for r in result['links'])),
        'unmatched': unmatched, 'unmatchedCount': len(unmatched),
        'battleBuffLinks': 0,
        'scope': '完整官方养成配置及明确资料引用；不认定本场装备，不推算服务器结算，不推断战斗 Buff 来源。'}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--h5-source', type=Path, required=True)
    parser.add_argument('--consumer-source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = extract_replay_cultivation(args.source, args.h5_source,
                                     consumer_source=args.consumer_source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps(data['summary'], ensure_ascii=False))
