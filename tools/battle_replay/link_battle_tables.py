"""Link only explicit references in the official battle configuration tables.

`bySkill` and `byBuff` contain relation IDs, not positions in `relations`.
`sourceRow` and `priorityIndex` are zero-based. `sourceFields` preserves the
entire XML attribute map; sourcePath is a one-based XPath to the original row.

ComboBuffsConfigXML.getTargetSkillIds scans buffs from the last index to the
first. BattleFighter.attack replaces the playback skill only for a result > 0.
This is conditional client playback selection, not a skill applying a Buff.
BufferExecuteDefine.parseDefine reads bufferId/skillId/lightedSkillId directly.
BattleRepresenter.getChangedSkillId selects lightedSkillId for a glittery
dragon, otherwise skillId; skillId is also used for the psychic progress UI.

References missing from the supplied catalogs remain traceable in the graph
and are reported as dangling. No catalog records, including ID 0, are created.
Only Flash embedded XML is linked here; H5 differences are not silently merged.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


_KINDS = {
    'combo_buff_playback': {
        'label': '持有 Buff 时选择播放技能',
        'meaning': 'For skillId, possession of buffId selects targetSkillId for client playback. '
                   'The highest matching priorityIndex within the source row wins.',
        'evidence': ['mmo.battleconfig.data.ComboBuffsConfigXML.getTargetSkillIds',
                     'mmo.battle.fighter.BattleFighter.attack'],
    },
    'buffer_execute_skill': {
        'label': '队伍 Buff 对应技能',
        'meaning': 'bufferId explicitly references skillId for the ordinary team execution '
                   'or psychic progress display. The table does not establish application of the Buff.',
        'evidence': ['mmo.interfaces.battleconfig.define.BufferExecuteDefine.parseDefine',
                     'mmo.battle.fighter.BattleRepresenter.getChangedSkillId',
                     'mmo.battle.util.BattleNumUnitUtil.afterPlayBufferStateChangedMovie'],
    },
    'buffer_execute_lighted_skill': {
        'label': '队伍 Buff 对应闪光技能',
        'meaning': 'bufferId explicitly references lightedSkillId; the client selects it '
                   'when the executing dragon is glittery.',
        'evidence': ['mmo.interfaces.battleconfig.define.BufferExecuteDefine.parseDefine',
                     'mmo.battle.fighter.BattleRepresenter.getChangedSkillId'],
    },
}


def _table(source: Path, name: str, selector: str) -> tuple[Path, list[dict]]:
    paths = sorted((source / 'xml').glob(f'*.{name}__*.xml'))
    if len(paths) != 1:
        raise ValueError(f'expected exactly one {name} XML under {source / "xml"}; found {len(paths)}')
    return paths[0], [dict(row.attrib) for row in ET.parse(paths[0]).getroot().findall(selector)]


def _row_fingerprint(fields: dict) -> str:
    # Independent of XML attribute order and unrelated row insertions.
    raw = json.dumps(fields, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return hashlib.sha256(raw).hexdigest()[:20]


def link_battle_tables(source: Path, skills: dict, buffs: dict) -> dict:
    """Return traceable relations, reverse indices and unresolved-reference reports.

    IDs use source-table row contents plus the original pair/field index. Exact
    duplicate rows receive an occurrence suffix; no duplicate row is lost.
    Missing positive references are retained, but non-positive/invalid IDs do
    not create edges. A malformed ComboBuffs array pair is never guessed.
    """
    source = Path(source)
    relations, dangling, skipped, invalid, duplicate_keys = [], [], [], [], []
    by_skill, by_buff = defaultdict(list), defaultdict(list)
    known_skills, known_buffs = {str(k) for k in skills}, {str(k) for k in buffs}
    table_report = {}
    relation_ids = set()

    def read_table(name: str, selector: str, key: str):
        path, rows = _table(source, name, selector)
        file = path.relative_to(source).as_posix()
        table_report[name] = {'file': file, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                              'rowCount': len(rows)}
        keys = defaultdict(list)
        for row_index, fields in enumerate(rows):
            keys[fields.get(key)].append(row_index)
        duplicate_keys.extend({'sourceTable': name, 'field': key, 'value': value, 'sourceRows': indices}
                              for value, indices in keys.items() if len(indices) > 1)
        occurrences = Counter()
        for row_index, fields in enumerate(rows):
            fingerprint = _row_fingerprint(fields)
            occurrences[fingerprint] += 1
            duplicate = f':duplicate-{occurrences[fingerprint]}' if occurrences[fingerprint] > 1 else ''
            xpath = '/config/combo/s' if name == 'ComboBuffsConfigXML' else '/config/s'
            yield {'sourceTable': name, 'sourceFile': file, 'sourceRow': row_index,
                   'sourcePath': f'{xpath}[{row_index + 1}]', 'sourceFields': fields}, fingerprint + duplicate

    def positive_id(raw, field: str, location: dict, pair_index=None):
        reference = {**location, 'field': field, 'raw': raw}
        if pair_index is not None:
            reference['priorityIndex'] = pair_index
        if raw is None or not re.fullmatch(r'[+-]?[0-9]+', str(raw).strip()):
            invalid.append({**reference, 'reason': 'missing_or_non_integer_reference'})
            return None
        number = int(str(raw).strip())
        if number <= 0:
            skipped.append({**reference, 'reason': 'non_positive_reference'})
            return None
        if number > 2147483647:
            invalid.append({**reference, 'reason': 'outside_official_signed_int_range'})
            return None
        return str(number)

    def add_relation(relation: dict):
        identifier = relation['id']
        if identifier in relation_ids:
            raise ValueError(f'duplicate generated relation ID: {identifier}')
        relation_ids.add(identifier)
        relations.append(relation)
        # A playback skill can equal its source. Its reverse index still has one
        # entry per relation, while each field is checked separately below.
        for skill_id in dict.fromkeys(relation[field] for field in ('skillId', 'targetSkillId') if field in relation):
            by_skill[skill_id].append(identifier)
        by_buff[relation['buffId']].append(identifier)
        for field, known, entity in [('skillId', known_skills, 'skill'),
                                     ('targetSkillId', known_skills, 'skill'),
                                     ('buffId', known_buffs, 'buff')]:
            if field in relation and relation[field] not in known:
                dangling.append({'relationId': identifier, 'entity': entity, 'field': field,
                                 'id': relation[field], 'sourceTable': relation['sourceTable'],
                                 'sourceRow': relation['sourceRow'], 'sourcePath': relation['sourcePath']})

    for location, fingerprint in read_table('ComboBuffsConfigXML', './combo/s', 'skillId'):
        fields = location['sourceFields']
        skill_id = positive_id(fields.get('skillId'), 'skillId', location)
        buff_ids = fields['buffs'].split(',') if 'buffs' in fields else []
        targets = fields['targetSkillIds'].split(',') if 'targetSkillIds' in fields else []
        if len(buff_ids) != len(targets) or not buff_ids:
            invalid.append({**location, 'reason': 'combo_pair_count_mismatch_or_empty',
                            'buffCount': len(buff_ids), 'targetSkillCount': len(targets)})
        # Keep aligned pairs only, preserving their official indices. A target
        # without a same-index prerequisite, or the reverse, is not invented.
        for priority_index in range(max(len(buff_ids), len(targets))):
            buff_id = positive_id(buff_ids[priority_index] if priority_index < len(buff_ids) else None,
                                  'buffs', location, priority_index)
            target_id = positive_id(targets[priority_index] if priority_index < len(targets) else None,
                                    'targetSkillIds', location, priority_index)
            if skill_id is not None and buff_id is not None and target_id is not None:
                add_relation({'id': f'combo:{fingerprint}:{priority_index}', 'kind': 'combo_buff_playback',
                              **location, 'skillId': skill_id, 'buffId': buff_id,
                              'targetSkillId': target_id, 'priorityIndex': priority_index,
                              'priorityRule': 'highest_matching_index'})

    for location, fingerprint in read_table('BufferExecuteXML', './s', 'bufferId'):
        fields = location['sourceFields']
        buff_id = positive_id(fields.get('bufferId'), 'bufferId', location)
        for field, kind in [('skillId', 'buffer_execute_skill'),
                            ('lightedSkillId', 'buffer_execute_lighted_skill')]:
            skill_id = positive_id(fields.get(field), field, location)
            if buff_id is not None and skill_id is not None:
                add_relation({'id': f'buffer-execute:{fingerprint}:{field}', 'kind': kind,
                              **location, 'skillId': skill_id, 'buffId': buff_id, 'skillField': field})

    dangling_entities = defaultdict(set)
    for item in dangling:
        dangling_entities[item['entity']].add(item['id'])
    report = {
        'format': 1, 'sourceFormat': 'Flash embedded XML',
        'indexValues': 'relation IDs; bySkill includes source and playback target skills',
        'sourceRowBase': 0, 'priorityIndexBase': 0,
        'relationCount': len(relations), 'byKind': dict(Counter(r['kind'] for r in relations)),
        'indexedSkills': len(by_skill), 'indexedBuffs': len(by_buff),
        'sourceTables': table_report, 'kindSemantics': _KINDS,
        'danglingCount': len(dangling),
        'danglingUnique': {kind: sorted(ids, key=int) for kind, ids in dangling_entities.items()},
        'dangling': dangling,
        'skippedNonPositiveCount': len(skipped), 'skippedNonPositive': skipped,
        'invalidCount': len(invalid), 'invalid': invalid,
        'duplicateSourceKeyCount': len(duplicate_keys), 'duplicateSourceKeys': duplicate_keys,
        'limitations': [
            'These are explicit client configuration references, not server Buff application rules.',
            'Combo alternatives retain priority but do not assert which one occurred in a battle.',
            'Dangling references are retained; no missing skill/Buff definitions are synthesized.',
            'Same names and similar numeric IDs are never used to infer a relation.',
        ],
    }
    return {'relations': relations, 'bySkill': dict(by_skill), 'byBuff': dict(by_buff), 'report': report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='official extraction directory containing xml/')
    parser.add_argument('catalog', type=Path, help='JSON containing skills and buffs tables')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding='utf-8-sig'))
    result = link_battle_tables(args.source, catalog['skills'], catalog['buffs'])
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    report = result['report']
    print(json.dumps({key: report[key] for key in ('relationCount', 'byKind', 'indexedSkills', 'indexedBuffs',
                                                   'danglingCount', 'danglingUnique', 'skippedNonPositiveCount',
                                                   'invalidCount', 'duplicateSourceKeyCount')}, ensure_ascii=False))


if __name__ == '__main__':
    main()
