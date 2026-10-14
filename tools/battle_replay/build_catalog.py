"""Join official replay tables by the client's verified references.

Raw rows, duplicate evidence, cross-client differences and joins are exported
separately. The embedded catalog is a lookup, not a guess at server causality.
"""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from extract_buff_tables import extract_buff_tables
from extract_skill_tables import extract_skill_tables
from link_battle_tables import link_battle_tables
from extract_entry_tables import extract_entry_tables, resolve_entry_mentions
from fetch_reference import saved_flash_versions
from extract_pet_mechanisms import extract_pet_mechanisms
from extract_pet_identity import extract_pet_identities
from extract_presentation_config import extract_presentation_config
from extract_pet_dragons import extract_pet_dragons
from extract_replay_supplements import extract_replay_supplements
from extract_replay_cultivation import extract_replay_cultivation
from audit_reference_coverage import audit_reference_coverage


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_json(path, value):
    data = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False).encode('utf-8')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return {'file': path.name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def verify_auxiliary(source, auxiliary, verified_source=None):
    """Reuse unrelated pet/formation data only when latest manifest pins agree."""
    versions = saved_flash_versions(verified_source or source, 'play-start.xml' if verified_source else 'start.xml')
    fresh = read_json(verified_source / 'flash-resolved' / 'provenance.json') if verified_source else {}
    fresh_packages = {item['resource']: item for item in fresh.get('files', [])}
    pets = read_json(auxiliary / 'pet-provenance.json')
    formation = read_json(auxiliary / 'formation-provenance.json')
    verified = []
    for item in pets['packages'] + [formation]:
        resource = item['resource']
        expected = versions.get(resource, '2000')
        if str(item['version']) != expected:
            current = fresh_packages.get(resource, {})
            if current.get('version') != expected or current.get('sha256') != item['sha256']:
                raise ValueError(f'{resource} changed in the latest manifest; refresh auxiliary data first')
        verified.append({'resource': resource, 'version': expected, 'sha256': item['sha256']})
    previous = read_json(auxiliary / 'provenance.json')
    current = read_json(source / 'provenance.json')
    old_hashes = {p['resource']: p['sha256'] for p in previous['packages']}
    for package in current['packages']:
        if verified_source:
            checked = fresh_packages.get(package['resource'])
            if not checked or checked.get('version') != versions.get(package['resource'], '2000') or checked.get('sha256') != package['sha256']:
                raise ValueError(f'{package["resource"]} changed bytes; refresh the parsed table snapshot')
        if package['resource'] in ('library/interfaces', 'battle/battle'):
            if old_hashes.get(package['resource']) != package['sha256']:
                raise ValueError(f'{package["resource"]} changed; revalidate the retained replay rules')
    return pets, formation, verified


def runtime_skills(skills):
    """Remove repeated text/evidence from the embedded lookup, not the exports.

    Flat fields are the verified Flash runtime projection consumed by replay.
    Full raw rows, H5 comparisons and runtime objects remain in skills.json.
    """
    result = {}
    for identifier, entry in skills.items():
        row = {key: value for key, value in entry.items()
               if key not in ('h5', 'flashText', 'runtimeDefine', 'descriptionAttributes')}
        metadata = entry.get('descriptionAttributes', {})
        if any(row.get(key) != value for key, value in metadata.items()):
            # Derived skills can inherit playback types different from their
            # own description metadata. Keep that distinction visible.
            row['descriptionAttributes'] = metadata
        for field in ('configSource', 'textSource'):
            if field in row:
                row[field] = {key: value for key, value in row[field].items()
                              if key not in ('path', 'sha256')}
        result[identifier] = row
    return result


def runtime_legends(legends):
    """Keep the official variants and their sources without duplicating audit text."""
    if not legends:
        return {}
    return {
        'schemaVersion': legends['schemaVersion'],
        'importEvidence': legends['importEvidence'],
        'definitions': {
            identifier: {
                'id': definition['id'], 'name': definition['name'],
                'variants': [
                    {key: value for key, value in variant.items() if key != 'rawParameters'}
                    for variant in definition['variants']
                ],
            }
            for identifier, definition in legends['definitions'].items()
        },
    }


def build(source, auxiliary=None, exports=None, verified_source=None, mechanism_rules=None, legend_mechanics=None, skin_source=None,
          *, title_source=None, race_expansions=None, formation_source=None, cultivation_source=None, cultivation_h5_source=None, cultivation_consumer_source=None,
          reference_consumers=None):
    auxiliary = auxiliary or source
    exports = exports or source / 'linked'

    def xml(name):
        files = list((source / 'xml').glob('*.' + name + '__*.xml'))
        if len(files) != 1:
            raise ValueError('expected exactly one ' + name)
        return ET.parse(files[0]).getroot()

    pet_provenance, formation_provenance, reused = verify_auxiliary(source, auxiliary, verified_source)
    buff_tables = extract_buff_tables(source)
    skill_tables = extract_skill_tables(source)
    entry_tables = extract_entry_tables(source)
    buffs, skills = buff_tables['buffs'], skill_tables['skills']
    pet_mechanisms = extract_pet_mechanisms(
        auxiliary, skills, mechanism_rules, legend_mechanics,
        source / 'xml/mmo.battleconfig.data.HuanConfigXML__xmlClass.xml', consumer_evidence_path=reference_consumers)
    identities = extract_pet_identities(pet_mechanisms['pets'], skin_source, verified_source)
    pets = read_json(auxiliary / 'pets.json')
    presentation = extract_presentation_config(source, auxiliary)
    pet_dragons = extract_pet_dragons(source, auxiliary)
    for race, description in presentation['descriptions'].items():
        if race in pets:
            pets[race]['desc'] = description
    for race, identity in identities['pets'].items():
        if race not in pets or pets[race]['name'] != pet_mechanisms['pets'][race]['name']:
            raise ValueError('Pet name snapshots disagree: ' + race)
        pets[race]['identity'] = identity
    supplements = extract_replay_supplements(source, auxiliary, title_source=title_source, formation_source=formation_source,
        skills=skills, buffs=buffs, pets=pets, race_expansions=read_json(race_expansions) if race_expansions else None)
    cultivation = extract_replay_cultivation(cultivation_source, cultivation_h5_source,
        consumer_source=cultivation_consumer_source) if cultivation_source and cultivation_h5_source else {}
    references = link_battle_tables(source, skills, buffs)
    entry_mentions, entry_by_buff, entry_by_skill = {}, {}, {}
    for definitions, destination, fields in ((buffs, entry_by_buff, ('nm', 'mess')),
                                             (skills, entry_by_skill, ('name', 'description'))):
        for identifier, definition in definitions.items():
            labels = []
            for field in fields:
                for mention in resolve_entry_mentions(definition.get(field, ''), entry_tables['entries']):
                    label = mention['displayLabel']
                    entry_mentions[label] = mention
                    if label not in labels: labels.append(label)
            if labels: destination[identifier] = labels
    entry_links = {'mentions': entry_mentions, 'byBuff': entry_by_buff, 'bySkill': entry_by_skill,
                   'scope': 'Literal bracketed terms in official text, not a Buff-ID-to-mechanism equivalence.'}
    provenance = read_json(source / 'provenance.json')
    provenance['h5'] = read_json(source / 'h5-provenance.json')
    h5_reader = source / 'h5-scripts' / 'battleconfig-provenance.json'
    if h5_reader.exists(): provenance['h5']['consumer'] = read_json(h5_reader)
    provenance['pets'] = pet_provenance
    provenance['formation'] = formation_provenance
    provenance['auxiliaryVersionChecks'] = reused
    if verified_source:
        proof = read_json(verified_source / 'flash-resolved' / 'provenance.json')
        provenance['versionChainRecheck'] = {key: proof[key] for key in ('retrievedUtc', 'manifestSha256', 'versiondataSha256')}
        provenance['versionChainRecheck']['packages'] = [
            {key: row[key] for key in ('resource', 'version', 'url', 'sha256', 'resolution')}
            for row in proof['files'] if not row.get('error') and not row['resource'].startswith('battlebuffer/')]
    evidence = {}
    for root in dict.fromkeys((auxiliary, source)):
        for folder in ('interfaces', 'battle', 'battleconfig', 'pet-interface', 'formation'):
            for path in sorted((root / folder / 'scripts').rglob('*.as')):
                evidence[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()

    report = {'format': 1, 'retrievedUtc': provenance['retrievedUtc'],
              'manifestSha256': provenance['manifestSha256'],
              'buffs': buff_tables['report'], 'skills': skill_tables['report'],
              'entries': entry_tables['report'],
              'petMechanisms': pet_mechanisms['report'],
              'petIdentities': identities['report'],
              'clientPresentation': presentation['report'],
              'replaySupplements': supplements['summary'],
              'replayCultivation': cultivation.get('summary', {}),
              'references': references['report'],
              'scope': 'Official table references only; not proof that a skill applied a Buff in a particular action.'}
    outputs = {}
    for filename, value in (
        ('buffs.json', buffs), ('skills.json', skills),
        ('skill-last-effects.json', skill_tables['lastSkills']),
        ('skill-embedded-effects.json', skill_tables['rawTables']['skillEmbedEffect']),
        ('buff-power-full-map.json', buff_tables['powerFull']),
        ('relations.json', references),
        ('official-entries.json', entry_tables['entries']),
        ('entry-mentions.json', entry_links),
        ('pet-mechanisms.json', pet_mechanisms),
        ('pet-identities.json', identities),
        ('client-presentation.json', presentation),
        ('pet-dragons.json', pet_dragons),
        ('replay-supplements.json', supplements),
        ('replay-cultivation.json', cultivation),
        ('raw-entry-tables.json', entry_tables['rawTables']),
        ('raw-buff-tables.json', buff_tables['rawTables']),
        ('raw-skill-tables.json', skill_tables['rawTables']),
        ('join-report.json', report),
    ):
        outputs[filename] = write_json(exports / filename, value)
    provenance['linkedTables'] = outputs
    summary = {'retrievedUtc': report['retrievedUtc'],
               'buffs': buff_tables['report']['counts'], 'skills': skill_tables['report']['counts'],
               'skillEmbeddedEffects': skill_tables['report']['skillEmbedEffect']['counts'],
               'entries': entry_tables['report'],
               'references': {key: value for key, value in references['report'].items()
                              if key in ('relationCount', 'byKind', 'indexedSkills', 'indexedBuffs',
                                         'kindSemantics', 'danglingCount', 'invalidCount', 'limitations')},
               'fullReport': outputs['join-report.json'], 'scope': report['scope']}
    catalog = {'format': 3, 'provenance': provenance, 'decompiledSourceSha256': evidence,
            'skills': runtime_skills(skills), 'buffs': buffs, 'skillLastEffects': skill_tables['lastSkills'],
            'buffPowerFullMap': buff_tables['powerFull'],
            'tableRelations': references['relations'],
            'relationsBySkill': references['bySkill'], 'relationsByBuff': references['byBuff'],
            'officialEntries': entry_tables['entries'], 'officialEntryMentions': entry_mentions,
            'entryMentionsByBuff': entry_by_buff, 'entryMentionsBySkill': entry_by_skill,
            'petMechanics': pet_mechanisms['pets'], 'mechanismRules': pet_mechanisms['rules'],
            'legendMechanics': runtime_legends(pet_mechanisms['legends']),
            'huanMechanics': pet_mechanisms['huan'],
            'petMechanicsSummary': pet_mechanisms['report'],
            'referenceConsumers': pet_mechanisms.get('consumerEvidence', {}),
            'petIdentitySummary': identities['report'],
            'clientPresentation': presentation['config'],
            'petDragons': pet_dragons,
            'replaySupplements': supplements,
            'replayCultivation': cultivation,
            'tableJoinSummary': summary,
            'pets': pets,
            'transformSkills': read_json(auxiliary / 'transform-skills.json'),
            'attributes': {row.get('id'): row.attrib for row in xml('RaceTypeConfigXML').iter('s')},
            'execution': {row.get('code', row.get('id', '')): row.attrib for row in xml('SkillExecuteXML').iter('s')},
            'bufferExecution': {row.get('bufferId'): row.attrib for row in xml('BufferExecuteXML').iter('s')},
            'combos': [row.attrib for row in xml('ComboBuffsConfigXML').iter('s')]}
    coverage = audit_reference_coverage(catalog)
    catalog['referenceCoverage'] = coverage
    report['referenceCoverage'] = coverage['summary']
    outputs['reference-coverage.json'] = write_json(exports / 'reference-coverage.json', coverage)
    outputs['join-report.json'] = write_json(exports / 'join-report.json', report)
    summary['fullReport'] = outputs['join-report.json']
    summary['supplements'] = supplements['summary']
    summary['cultivation'] = cultivation.get('summary', {})
    summary['referenceCoverage'] = coverage['summary']
    write_json(exports / 'index.json', {'format': 1, 'provenance': provenance,
                                       'decompiledSourceSha256': evidence, 'files': outputs})
    return catalog


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--auxiliary-source', type=Path)
    parser.add_argument('--exports', type=Path)
    parser.add_argument('--verified-source', type=Path)
    parser.add_argument('--mechanism-rules', type=Path)
    parser.add_argument('--legend-mechanics', type=Path)
    parser.add_argument('--skin-source', type=Path, required=True)
    parser.add_argument('--title-source', type=Path, required=True)
    parser.add_argument('--formation-source', type=Path, required=True)
    parser.add_argument('--race-expansions', type=Path, required=True)
    parser.add_argument('--cultivation-source', type=Path, required=True)
    parser.add_argument('--cultivation-h5-source', type=Path, required=True)
    parser.add_argument('--cultivation-consumer-source', type=Path)
    parser.add_argument('--reference-consumers', type=Path)
    args = parser.parse_args()
    result = build(args.source, args.auxiliary_source, args.exports, args.verified_source, args.mechanism_rules, args.legend_mechanics, args.skin_source,
        title_source=args.title_source, race_expansions=args.race_expansions, formation_source=args.formation_source, cultivation_source=args.cultivation_source,
        cultivation_h5_source=args.cultivation_h5_source, cultivation_consumer_source=args.cultivation_consumer_source,
        reference_consumers=args.reference_consumers)
    payload = json.dumps(result, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode('utf-8')
    temporary = args.output.with_suffix(args.output.suffix + '.tmp')
    temporary.write_bytes(payload)
    temporary.replace(args.output)
    print(json.dumps({'skills': len(result['skills']), 'buffs': len(result['buffs']),
                      'relations': len(result['tableRelations']), 'bytes': len(payload)}))
