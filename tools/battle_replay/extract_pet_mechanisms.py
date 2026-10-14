"""Read explicit pet-dictionary skill/plan references, never infer Buff causes."""
import hashlib
import copy
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from generate_pet_detail_data import iter_create_args


# One-based parameters of the verified PetDictionaryDataItem.create consumer.
FIELDS = {
    'norSkillNo': 15, 'ultSkillNo': 16, 'ultSuperSkillNo': 17,
    'heroSkillNo': 18, 'largeSkillNo': 32, 'dragonJobSkillNo': 36,
    'yuanLiSkillNo': 37, 'psychicsSkillNo': 38, 'legendSkillNo': 47,
    'transformSkillNo': 50, 'sign': 29, 'qiYunType': 60, 'groupRaceId': 43,
}
LABELS = {'norSkillNo': '普通技能', 'ultSkillNo': '超杀技能',
          'ultSuperSkillNo': '超级超杀技能', 'heroSkillNo': '英雄技能',
          'psychicsSkillNo': '通灵技能', 'yuanLiSkillNo': '元力技能'}
# NewSkillIconHelperV2 explicitly calls BattleConfig.getSkillDefineById for
# these fields. TransformSkillDefine remains a separate formation namespace.
BATTLE_SKILL_FIELDS = set(LABELS) | {'legendSkillNo', 'largeSkillNo', 'dragonJobSkillNo'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def verify_schema(text):
    for name, parameter in FIELDS.items():
        if not re.search(r'var\s+' + name + r'\s*:\s*(?:int|String)\s*=\s*param' + str(parameter) + r'\s*;', text):
            raise ValueError(f'Official dictionary schema changed: {name}')
    if not re.search(r'get\s+shenYunSkillNo\s*\(\)\s*:\s*int\s*\{\s*return\s+this\._legendSkillNo\s*;', text):
        raise ValueError('Unverified shenYunSkillNo alias')
    for method, sign in [('isShenYun', '神运'), ('isLingChu', '灵初'),
                         ('isHuan', '寰力量'), ('isYiXingZu', '异形族'), ('isSkin', '皮肤'),
                         ('isLegend', '传说'), ('isElite', '精英')]:
        if not re.search(r'function\s+' + method + r'\s*\(\)\s*:\s*Boolean\s*\{\s*return\s+this\.findSign\("' + sign + r'"\)', text):
            raise ValueError(f'Unverified era selector: {method}')
    compact = re.sub(r'\s+', '', text)
    for getter, condition, body in (
        ('yuanLiSkillNo', 'this.isHuan()||this.isYiXingZu()', 'return0;'),
        ('huanSkillNo', '!(this.isHuan()||this.isYiXingZu())', 'return0;'),
    ):
        expected = f'functionget{getter}():int{{if({condition}){{{body}}}returnthis._yuanLiSkillNo;}}'
        if expected not in compact:
            raise ValueError(f'Unverified Huan namespace selector: {getter}')


def parse_pet_rows(text, source, skills):
    result = {}
    for index, args in enumerate(iter_create_args(text)):
        if len(args) < max(FIELDS.values()):
            raise ValueError('Pet dictionary row has too few arguments')
        race = str(int(args[0]))
        sign = json.loads(args[FIELDS['sign'] - 1])
        signs = [item.strip() for item in sign.split(',') if item.strip()]
        family = 'suLing' if '灵初' in signs else 'qiYun' if '神运' in signs else ''
        links, other_references = [], {}
        raw_legend = str(int(args[FIELDS['legendSkillNo'] - 1]))
        legend_namespace = ('none' if int(raw_legend) <= 0 else
                            'battleSkill' if '神运' in signs or '灵初' in signs else
                            'legacyLegend' if '传说' in signs else 'unclassified')
        legend_candidates = []
        for field, parameter in FIELDS.items():
            if field in ('sign', 'qiYunType', 'groupRaceId'):
                continue
            identifier = str(int(args[parameter - 1]))
            if int(identifier) <= 0:
                continue
            if field == 'yuanLiSkillNo' and ('寰力量' in signs or '异形族' in signs):
                # The official getter suppresses yuanLiSkillNo and exposes
                # the same constructor value as huanSkillNo instead.
                other_references['huanSkillNo'] = identifier
                continue
            if field == 'legendSkillNo' and legend_namespace != 'battleSkill':
                # Pre-ShenYun legend skills use formation.LegendSkillDefine
                # variants selected by the equipped legend stone, not this
                # BattleConfig namespace (NewSkillIconHelperV2:713-738).
                if legend_namespace == 'legacyLegend':
                    other_references[field] = identifier
                else:
                    # The official legacy-skill UI is gated by isLegend().
                    # Elite rows reuse this raw constructor field, but a
                    # same-number battle definition alone is not proof of the
                    # consumer/namespace. Keep the candidate separate.
                    other_references['unclassifiedLegendSkillNo'] = identifier
                    if identifier in skills:
                        legend_candidates.append({'namespace': 'skills', 'id': identifier,
                            'status': 'unverifiedNamespace',
                            'reason': 'Exact ID has official battle-skill text, but no verified consumer selects this namespace for the raw field.'})
                continue
            if field not in BATTLE_SKILL_FIELDS:
                # A skill-number field may belong to a different configuration
                # namespace. In particular transformSkillNo is a formation
                # TransformSkillDefine, whose types 0/1/2 must stay distinct.
                other_references[field] = identifier
                continue
            label = LABELS.get(field, '关联技能')
            if field == 'legendSkillNo':
                label = '神运技' if '神运' in signs else '传说技能'
            links.append({'field': field, 'label': label, 'skillId': identifier,
                          'createParameter': parameter, 'resolved': identifier in skills})
        result[race] = {'raceId': race, 'name': json.loads(args[1]), 'sign': sign,
                        'groupRaceId': str(int(args[FIELDS['groupRaceId'] - 1])),
                        'skillLinks': links, 'otherReferences': other_references,
                        'rawLegendSkillNo': raw_legend,
                        'legendReferenceNamespace': legend_namespace,
                        'legendReferenceCandidates': legend_candidates,
                        'qiYunType': str(int(args[59])),
                        'mechanismFamily': family,
                        'source': {**source, 'createRow': index + 1}}
    return result


def parse_huan_rows(text, source):
    """HuanConfig groups repeated IDs; every threshold row must be retained."""
    definitions = {}
    for index, row in enumerate(ET.fromstring(text).iter('s')):
        identifier = str(int(row.attrib['id']))
        definition = definitions.setdefault(identifier, {'id': identifier, 'variants': []})
        definition['variants'].append({
            'name': row.attrib['name'], 'tip': row.attrib['tip'],
            'typeToTakeEffect': int(row.attrib['ttte']),
            'valueToTakeEffect': int(row.attrib['vtte']), 'icon': int(row.attrib['icon']),
            'source': {**source, 'row': index + 1}, 'rawAttributes': dict(row.attrib),
        })
    return definitions


def attach_reference_consumption(pets, evidence_path):
    """Attach bounded, source-verified consumer findings without resolving gaps.

    A lookup returning an empty wrapper/array is documented behavior, not a
    recovered skill definition. Bindings therefore leave every original
    namespace, candidate and resolved flag untouched.
    """
    if evidence_path is None:
        return {}
    path = Path(evidence_path)
    data = path.read_bytes()
    document = json.loads(data)
    if document.get('schemaVersion') != 1:
        raise ValueError('Unsupported reference consumer evidence schema')
    evidence = document.get('evidence', {})
    if not evidence:
        raise ValueError('Reference consumer evidence has no sources')
    for identifier, source in evidence.items():
        if sha(Path(source['file']).read_bytes()) != source['sha256']:
            raise ValueError('Reference consumer source differs from recorded provenance: ' + identifier)
    pending = {}
    for race, binding in document.get('pets', {}).items():
        pet = pets.get(race)
        if pet is None or pet['source']['sha256'] != binding['dictionarySourceSha256']:
            raise ValueError('Reference consumer dictionary source mismatch: ' + race)
        for selector in ('sign', 'legendReferenceNamespace', 'qiYunType', 'mechanismFamily'):
            if pet[selector] != binding[selector]:
                raise ValueError('Reference consumer selector mismatch: ' + race + '/' + selector)
        current = {link['field']: link['skillId'] for link in pet['skillLinks']}
        current.update(pet['otherReferences'])
        current['legendSkillNo'] = pet['rawLegendSkillNo']
        current['qiYunType'] = pet['qiYunType']
        items = binding['references']
        for item in items:
            if current.get(item['field']) != item['id']:
                raise ValueError('Reference consumer binding mismatch: ' + race + '/' + item['field'])
            if not all(isinstance(item.get(key), str) and item[key]
                       for key in ('label', 'summary', 'status', 'namespace')):
                raise ValueError('Reference consumer finding lacks a scoped description')
            evidence_ids = item.get('evidenceIds', []) + [identifier
                for branch in item.get('branches', []) for identifier in branch.get('evidenceIds', [])]
            if not evidence_ids or any(identifier not in evidence for identifier in evidence_ids):
                raise ValueError('Reference consumer finding has missing source evidence')
        pending[race] = copy.deepcopy(items)
    # Validate every binding before mutating any pet: a partial import would
    # make provenance failures difficult to detect downstream.
    for race, items in pending.items():
        pets[race]['referenceConsumption'] = items
    return {'schemaVersion': 1, 'scope': document['scope'], 'evidence': evidence,
            'summary': document.get('summary', {}),
            'importEvidence': {'file': str(path), 'sha256': sha(data)},
            'boundPets': len(pending), 'boundReferences': sum(len(items) for items in pending.values())}


def extract_pet_mechanisms(auxiliary, skills, rules_path=None, legend_path=None, huan_source=None,
                           consumer_evidence_path=None):
    auxiliary = Path(auxiliary)
    schema = auxiliary / 'pet-interface/scripts/mmo/interfaces/pet/data/PetDictionaryDataItem.as'
    schema_bytes = schema.read_bytes()
    verify_schema(schema_bytes.decode('utf-8-sig'))
    provenance = json.loads((auxiliary / 'pet-provenance.json').read_text(encoding='utf-8-sig'))
    pets, overrides, snapshots = {}, [], []
    packages = {record['resource']: record for record in provenance['packages']}
    for resource in ('pet/petdictionarydata', 'pet/petdictionarydataupdate'):
        record = packages[resource]
        path = auxiliary / record['decompiledFile']
        data = path.read_bytes()
        if sha(data) != record['decompiledSha256']:
            raise ValueError('Pet dictionary source differs from recorded provenance: ' + str(path))
        text = data.decode('utf-8-sig')
        timestamps = re.findall(r'const\s+timestamp\s*:\s*Number\s*=\s*(\d+)\s*;', text)
        if len(timestamps) != 1:
            raise ValueError('Missing or ambiguous official dictionary timestamp')
        snapshots.append((record, path, data, text, int(timestamps[0])))
    for record, path, data, text, timestamp in snapshots:
        # PetDictionaryDataService only applies a strictly newer update table.
        if record['resource'].endswith('update') and timestamp <= snapshots[0][4]:
            continue
        rows = parse_pet_rows(text, {
            'resource': record['resource'], 'file': str(path), 'sha256': sha(data),
        }, skills)
        overrides.extend(identifier for identifier in rows if identifier in pets)
        pets.update(rows)  # Same base-then-update order as the official service.
    rules = {}
    if rules_path:
        rules_path = Path(rules_path)
        data = rules_path.read_bytes()
        rules = json.loads(data)
        if rules.get('schemaVersion') != 1:
            raise ValueError('Unsupported official mechanism rule schema')
        for family in ('qiYun', 'suLing'):
            for identifier, plan in rules[family]['plans'].items():
                if str(plan['id']) != identifier:
                    raise ValueError('Mechanism plan ID mismatch')
                evidence = plan['source']
                if sha(Path(evidence['file']).read_bytes()) != evidence['sha256']:
                    raise ValueError('Mechanism source differs from recorded provenance')
                for channel in plan['channels']:
                    if len(channel['thresholds']) != len(channel['effects']):
                        raise ValueError('Mechanism threshold/effect count mismatch')
        rules['importEvidence'] = {'file': str(rules_path), 'sha256': sha(data)}
    legends = {}
    if legend_path:
        legend_path = Path(legend_path)
        data = legend_path.read_bytes()
        legends = json.loads(data)
        if legends.get('schemaVersion') != 1:
            raise ValueError('Unsupported legend mechanism schema')
        hashes = {}
        for identifier, definition in legends['definitions'].items():
            if str(definition['id']) != identifier:
                raise ValueError('Legend definition ID mismatch')
            for variant in definition['variants']:
                if str(variant['id']) != identifier:
                    raise ValueError('Legend variant ID mismatch')
                evidence = variant['source']
                file = evidence['file']
                if file not in hashes:
                    hashes[file] = sha(Path(file).read_bytes())
                if hashes[file] != evidence['sha256']:
                    raise ValueError('Legend source differs from recorded provenance')
        legends['importEvidence'] = {'file': str(legend_path), 'sha256': sha(data)}
    huan = {}
    if huan_source:
        huan_source = Path(huan_source)
        data = huan_source.read_bytes()
        huan = parse_huan_rows(data.decode('utf-8-sig'), {'file': str(huan_source), 'sha256': sha(data)})
    references = 0
    for pet in pets.values():
        family, plan_id = pet['mechanismFamily'], pet['qiYunType']
        pet['planResolved'] = bool(family and plan_id in rules.get(family, {}).get('plans', {}))
        pet['legendResolved'] = (pet['legendReferenceNamespace'] == 'legacyLegend' and
                                pet['otherReferences'].get('legendSkillNo', '') in legends.get('definitions', {}))
        pet['huanResolved'] = pet['otherReferences'].get('huanSkillNo', '') in huan
        references += len(pet['skillLinks'])
    consumers = attach_reference_consumption(pets, consumer_evidence_path)
    return {'pets': pets, 'rules': rules, 'legends': legends, 'huan': huan,
            'consumerEvidence': consumers, 'report': {
        'pets': len(pets), 'skillReferences': references, 'overriddenRaceIds': overrides,
        'sourceTimestamps': {record['resource']: timestamp for record, _, _, _, timestamp in snapshots},
        'updateApplied': snapshots[1][4] > snapshots[0][4],
        'unresolvedSkillReferences': sum(not link['resolved'] for pet in pets.values() for link in pet['skillLinks']),
        'resolvedMechanismPlans': sum(pet['planResolved'] for pet in pets.values()),
        'resolvedLegendReferences': sum(pet['legendResolved'] for pet in pets.values()),
        'legendReferenceNamespaces': {namespace: sum(pet['legendReferenceNamespace'] == namespace for pet in pets.values())
                                      for namespace in ('none', 'battleSkill', 'legacyLegend', 'unclassified')},
        'unclassifiedLegendReferences': sum(pet['legendReferenceNamespace'] == 'unclassified' for pet in pets.values()),
        'huanReferences': sum('huanSkillNo' in pet['otherReferences'] for pet in pets.values()),
        'resolvedHuanReferences': sum(pet['huanResolved'] for pet in pets.values()),
        'huanDefinitions': len(huan),
        'huanVariants': sum(len(row['variants']) for row in huan.values()),
        'consumerEvidencePets': consumers.get('boundPets', 0),
        'consumerEvidenceReferences': consumers.get('boundReferences', 0),
        'schemaSource': {'file': str(schema), 'sha256': sha(schema_bytes), 'parameters': FIELDS},
        'scope': 'Explicit current official dictionary references. Neither skill ownership nor a threshold description proves a battle event caused a Buff or activated a mechanism.',
    }}
