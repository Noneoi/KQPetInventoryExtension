"""Extract verified CLIENT-only attribute/job/TianQi and text display rules.

No damage is calculated. The attribute column map and thresholds come from
RaceTypeConfigXML; primary jobs and exact signs come from the pet dictionary.
The separate description export preserves the official testing-pet sentinel.
Buff harm labels and formation names are display metadata, not new mechanics.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from generate_pet_detail_data import iter_create_args


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def parse_string_constant_array(text, name):
    declaration = re.search(r'\b' + re.escape(name) + r'\s*:\s*Array\s*=\s*', text)
    if not declaration:
        raise ValueError('Missing official string array: ' + name)
    tail = text[declaration.end():]
    try:
        values, end = json.JSONDecoder().raw_decode(tail)
    except json.JSONDecodeError as error:
        raise ValueError('Unsupported official string array: ' + name) from error
    if (not tail[end:].lstrip().startswith(';') or not isinstance(values, list) or not values or
            any(not isinstance(value, str) or not value for value in values) or len(set(values)) != len(values)):
        raise ValueError('Unverified official string array: ' + name)
    return values


def parse_formation_names(text, define):
    compact = re.sub(r'\s+', '', define)
    for required in ('functionFormationDefine(param1:int,param2:String,param3:Array,param4:String,',
                     'this.id=param1;', 'this.name=Translator.translate(param2);',
                     'this.data=param3;', 'this._attribute=param4;'):
        if required not in compact:
            raise ValueError('Unverified FormationDefine constructor: ' + required)
    body = re.search(r'\bDEFINES\s*:\s*Object\s*=\s*\{(.*?)\}\s*;', text, re.S)
    if not body or 'returnDEFINES[param1.toString()];' not in re.sub(r'\s+', '', text):
        raise ValueError('Unverified official formation ID lookup')
    string = r'"(?:[^"\\]|\\.)*"'
    constructor = re.compile(
        rf'"(?P<key>\d+)"\s*:\s*new\s+FormationDefine\(\s*(?P<id>\d+)\s*,\s*'
        rf'(?P<name>{string})\s*,\s*\[\s*(?:\d+\s*(?:,\s*\d+\s*)*)?\]\s*,\s*{string}'
        r'(?:\s*,\s*\d+\s*,\s*(?:true|false))?\s*\)')
    names = {}
    for match in constructor.finditer(body[1]):
        key, name = match['key'], json.loads(match['name'])
        if key != match['id'] or key in names or not name:
            raise ValueError('Inconsistent or duplicate official formation ID')
        # The fourth argument is not a battle outcome or an applied modifier.
        # Deliberately export only the independently verified display name.
        names[key] = {'name': name}
    remainder = constructor.sub('', body[1])
    if not names or re.sub(r'[\s,]', '', remainder):
        raise ValueError('Unsupported official formation definition')
    return names


def rebuild_presentation_scripts(source, auxiliary, java, ffdec):
    source, auxiliary = Path(source), Path(auxiliary)
    packages = {row['resource']: row for row in read_json(source / 'provenance.json')['packages']}
    exports = (
        ('library/interfaces', 'presentation-interfaces', (
            'mmo.interfaces.pet.data.PetJob', 'mmo.interfaces.formation.FormationImageConfig',
            'mmo.interfaces.formation.FormationDefine')),
        ('battle/battle', 'presentation-battle', ('mmo.battle.consts.BufferConsts',)),
    )
    for resource, directory, classes in exports:
        package = packages[resource]
        swf = source / package['file']
        if sha(swf.read_bytes()) != package['sha256']:
            raise ValueError('Pinned official SWF hash mismatch: ' + resource)
        exported = auxiliary / directory
        for class_name in classes:
            subprocess.run([str(java), '-Xmx512m', '-Djava.awt.headless=true', '-jar', str(ffdec),
                            '-selectclass', class_name, '-export', 'script', str(exported), str(swf)], check=True)
        proof = {'resource': resource, 'packageSha256': package['sha256'],
                 'exporterSha256': sha(Path(ffdec).read_bytes()),
                 'scripts': {name.rsplit('.', 1)[-1] + '.as':
                             sha((exported / 'scripts' / (name.replace('.', '/') + '.as')).read_bytes())
                             for name in classes}}
        (exported / 'extraction.json').write_text(json.dumps(proof, indent=2) + '\n', encoding='utf-8')


def extract_presentation_config(source, auxiliary):
    source, auxiliary = Path(source), Path(auxiliary)
    evidence = {}
    package_rows = read_json(source / 'provenance.json')['packages']
    packages = {row['resource']: row for row in package_rows}

    def consume(path, key, resource=None):
        data = path.read_bytes()
        item = {'file': str(path), 'sha256': sha(data)}
        if resource:
            package = packages[resource]
            item.update({k: package[k] for k in ('resource', 'version', 'url')})
            item['packageSha256'] = package['sha256']
        evidence[key] = item
        return data.decode('utf-8-sig')

    xml_path = source / 'xml/mmo.battleconfig.data.RaceTypeConfigXML__xmlClass.xml'
    root = ET.fromstring(consume(xml_path, 'attributeTable', 'battleconfig/battleconfig'))
    xml_proof = read_json(source / 'provenance.json')['xml']['mmo.battleconfig.data.RaceTypeConfigXML__xmlClass']
    if evidence['attributeTable']['sha256'] != xml_proof['sha256']:
        raise ValueError('Attribute table differs from the official extraction manifest')
    indices = {row.attrib['id']: int(row.attrib['index']) for row in root.findall('./map/m')}
    thresholds = {row.attrib['nm']: float(row.attrib['value']) for row in root.findall('./restraint_coefficient/r')}
    if not indices or set(thresholds) != {'弱效', '强效'} or thresholds['弱效'] >= thresholds['强效']:
        raise ValueError('Unverified attribute map or display thresholds')
    for row in root.findall('./race_type/s'):
        if row.attrib['id'] not in indices or any(index < 0 or index >= len(row.attrib['rcs'].split(',')) for index in indices.values()):
            raise ValueError('Attribute coefficient row cannot resolve every explicit column')
    race_define = consume(source / 'interfaces/scripts/mmo/interfaces/battleconfig/define/RaceTypeDefine.as',
                          'attributeConsumer', 'library/interfaces')
    if 'int(this._map[param1])' not in race_define:
        raise ValueError('Attribute ID-to-column consumer changed')
    priority_text = consume(source / 'interfaces/scripts/mmo/interfaces/battleconfig/consts/RaceTypePriority.as',
                            'attributePriorities', 'library/interfaces')
    priorities = dict(re.findall(r'const\s+(\w+)\s*:\s*int\s*=\s*(\d+)\s*;', priority_text))
    if not {'GOD', 'TRANSFORM'} <= priorities.keys():
        raise ValueError('Missing official attribute priority constants')
    consts = consume(auxiliary / 'battle/scripts/mmo/battle/consts/BufferConsts.as', 'ignoreBuffs', 'battle/battle')
    consts_proof = read_json(auxiliary / 'presentation-battle/extraction.json')
    battle_swf = source / packages['battle/battle']['file']
    fresh_consts = auxiliary / 'presentation-battle/scripts/mmo/battle/consts/BufferConsts.as'
    if (sha(battle_swf.read_bytes()) != packages['battle/battle']['sha256'] or
            consts_proof['packageSha256'] != packages['battle/battle']['sha256'] or
            consts_proof['scripts'].get('BufferConsts.as') != sha(fresh_consts.read_bytes()) or
            evidence['ignoreBuffs']['sha256'] != consts_proof['scripts'].get('BufferConsts.as')):
        raise ValueError('BufferConsts differs from the pinned battle export')
    harm_sources = parse_string_constant_array(consts, 'BUFF_HARM_SOURCE')
    evidence['buffHarmSources'] = dict(evidence['ignoreBuffs'], selector='BufferConsts.BUFF_HARM_SOURCE',
                                     extractionManifest=str(auxiliary / 'presentation-battle/extraction.json'))
    evidence['buffHarmSources']['rebuild'] = 'java -jar ffdec.jar -selectclass mmo.battle.consts.BufferConsts -export script <auxiliary>/presentation-battle <source>/battle_battle.swf'
    match = re.search(r'Ignore_Att_Restraint_BuffIds\s*:\s*Array\s*=\s*\[([\d,\s]+)\]', consts)
    if not match:
        raise ValueError('Unverified ignore-restraint Buff list')
    ignore = [int(part) for part in match[1].split(',')]
    consume(auxiliary / 'battle/scripts/mmo/battle/command/attack/common/PlayEffectCommand.as',
            'attackConsumer', 'battle/battle')
    consume(auxiliary / 'battle/scripts/mmo/battle/command/attack/representer/PlayRepresenterEffectCommand.as',
            'representerConsumer', 'battle/battle')
    consume(auxiliary / 'battle/scripts/mmo/battle/util/BattleUtil.as', 'attributeSelection', 'battle/battle')
    consume(auxiliary / 'interfaces/scripts/mmo/interfaces/battle/data/FighterInfo.as', 'fighterIdentity', 'library/interfaces')
    consume(auxiliary / 'interfaces/scripts/mmo/interfaces/battle/data/SkillNumResult.as', 'counterNumbers', 'library/interfaces')

    # Reproducible selective export from the same pinned library/interfaces SWF.
    interface_swf = source / packages['library/interfaces']['file']
    if sha(interface_swf.read_bytes()) != packages['library/interfaces']['sha256']:
        raise ValueError('Pinned interfaces SWF hash mismatch')
    job_path = auxiliary / 'presentation-interfaces/scripts/mmo/interfaces/pet/data/PetJob.as'
    job_text = consume(job_path, 'jobConsumer', 'library/interfaces')
    job_proof = read_json(auxiliary / 'presentation-interfaces/extraction.json')
    if (job_proof['packageSha256'] != packages['library/interfaces']['sha256'] or
            job_proof['scripts']['PetJob.as'] != evidence['jobConsumer']['sha256']):
        raise ValueError('PetJob extraction differs from the pinned interfaces export')
    evidence['jobConsumer']['rebuild'] = 'java -jar ffdec.jar -selectclass mmo.interfaces.pet.data.PetJob -export script <auxiliary>/presentation-interfaces <source>/library_interfaces.swf'
    compact_jobs = re.sub(r'\s+', '', job_text)
    if 'returnthis._attackType==AT_SUPER_PHYSICS||this._attackType==AT_SUPER_MAGIC;' not in compact_jobs:
        raise ValueError('Unverified PetJob.isGodJob implementation')
    jobs = {}
    for identifier, name, attack_type in re.findall(r'enum__\((\d+),"([^"]*)",(\w+)', compact_jobs):
        jobs[identifier] = {'name': name, 'god': attack_type in ('AT_SUPER_PHYSICS', 'AT_SUPER_MAGIC')}
    if not jobs:
        raise ValueError('No official primary jobs extracted')

    formation_texts = {}
    for filename, key in (('FormationImageConfig.as', 'formationNames'), ('FormationDefine.as', 'formationConsumer')):
        path = auxiliary / 'presentation-interfaces/scripts/mmo/interfaces/formation' / filename
        formation_texts[filename] = consume(path, key, 'library/interfaces')
        if job_proof['scripts'].get(filename) != evidence[key]['sha256']:
            raise ValueError('Formation extraction differs from the pinned interfaces export')
        evidence[key]['extractionManifest'] = str(auxiliary / 'presentation-interfaces/extraction.json')
        evidence[key]['rebuild'] = f'java -jar ffdec.jar -selectclass mmo.interfaces.formation.{path.stem} -export script <auxiliary>/presentation-interfaces <source>/library_interfaces.swf'
    formations = parse_formation_names(formation_texts['FormationImageConfig.as'], formation_texts['FormationDefine.as'])

    schema_path = auxiliary / 'pet-interface/scripts/mmo/interfaces/pet/data/PetDictionaryDataItem.as'
    schema = consume(schema_path, 'petConsumer', 'library/interfaces')
    compact = re.sub(r'\s+', '', schema)
    for required in ('vardesc:String=param7;', 'varjob:String=param10;', 'varsign:String=param29;',
                     'pddi._job=parseInt((atts=atts[0].split(","))[0]);',
                     'functionisTianQi():Boolean{returnthis.findSign("天启");}',
                     '_loc2_=this.sign.split(",");', 'if(_loc3_==param1)'):
        if required not in compact:
            raise ValueError('Unverified pet presentation selector: ' + required)
    dictionary_proof = read_json(auxiliary / 'pet-provenance.json')
    dictionary_packages = {row['resource']: row for row in dictionary_proof['packages']}
    pets, descriptions, base_timestamp = {}, {}, None
    for resource in ('pet/petdictionarydata', 'pet/petdictionarydataupdate'):
        package = dictionary_packages[resource]
        path = auxiliary / package['decompiledFile']
        data = path.read_bytes()
        if sha(data) != package['decompiledSha256']:
            raise ValueError('Pet dictionary differs from its official extraction manifest')
        text = data.decode('utf-8-sig')
        timestamp = int(re.search(r'const\s+timestamp\s*:\s*Number\s*=\s*(\d+)', text)[1])
        evidence[resource] = {'file': str(path), 'sha256': sha(data), 'resource': resource,
                              'version': package['version'], 'url': package['url'],
                              'packageSha256': package['sha256'], 'timestamp': timestamp}
        if base_timestamp is None:
            base_timestamp = timestamp
        elif timestamp <= base_timestamp:
            continue
        for args in iter_create_args(text):
            race_id = str(int(args[0]))
            job = json.loads(args[9])
            primary = re.match(r'^\s*([+-]?\d+)', job.split('-')[0].split(',')[0])
            sign = json.loads(args[28])
            pets[race_id] = {'isTianQi': '天启' in sign.split(',')}
            if primary:
                pets[race_id]['jobId'] = int(primary[1])
            descriptions[race_id] = json.loads(args[6])
    return {'config': {
        'schemaVersion': 1, 'scope': 'Official client display hints only; no damage or server outcome calculation.',
        'attributeIndex': indices, 'weakThreshold': thresholds['弱效'], 'strongThreshold': thresholds['强效'],
        'godPriorities': [int(priorities['GOD']), int(priorities['TRANSFORM'])],
        'representerGodPriorities': [int(priorities['GOD'])],
        'ignoreRestraintBuffIds': ignore, 'buffHarmSources': harm_sources, 'formations': formations,
        'jobs': jobs, 'pets': pets,
        'sourceEvidence': evidence,
    }, 'descriptions': descriptions,
        'report': {'attributes': len(indices), 'jobs': len(jobs), 'pets': len(pets),
                   'buffHarmSources': len(harm_sources), 'formations': len(formations),
                   'tianQiPets': sum(row['isTianQi'] for row in pets.values()),
                   'testingDescriptions': sum(desc == '内测' for desc in descriptions.values())}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--auxiliary-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--java')
    parser.add_argument('--ffdec')
    args = parser.parse_args()
    if bool(args.java) != bool(args.ffdec):
        parser.error('--java and --ffdec must be supplied together')
    if args.java:
        rebuild_presentation_scripts(args.source, args.auxiliary_source, args.java, args.ffdec)
    result = extract_presentation_config(args.source, args.auxiliary_source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False), encoding='utf-8')
    print(json.dumps(result['report'], ensure_ascii=False))
