"""Independent fixtures for official table reference extraction, without Qt."""
import copy
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from link_battle_tables import link_battle_tables


class LinkBattleTablesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name)
        (self.source / 'xml').mkdir()
        self.skills = {str(value): {} for value in (10, 11, 12, 21, 22)}
        self.buffs = {str(value): {} for value in (101, 102, 201)}

    def write(self, combos=(), executions=()):
        for name, rows in [('ComboBuffsConfigXML', combos), ('BufferExecuteXML', executions)]:
            root = ET.Element('config')
            parent = ET.SubElement(root, 'combo') if name == 'ComboBuffsConfigXML' else root
            for row in rows:
                ET.SubElement(parent, 's', row)
            ET.ElementTree(root).write(self.source / 'xml' / f'mmo.battleconfig.data.{name}__xmlClass.xml',
                                       encoding='utf-8', xml_declaration=True)

    def test_reverse_priority_pairs_and_bidirectional_indices(self):
        combo = {'skillId': '10', 'buffs': '101,102', 'targetSkillIds': '11,12'}
        execution = {'bufferId': '201', 'skillId': '21', 'lightedSkillId': '22',
                     'maxValue': '12', 'desc': 'official description'}
        self.write([combo], [execution])
        original = copy.deepcopy((self.skills, self.buffs))
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual((self.skills, self.buffs), original)
        first, second, normal, lighted = result['relations']
        self.assertEqual((first['buffId'], first['targetSkillId'], first['priorityIndex']), ('101', '11', 0))
        self.assertEqual((second['buffId'], second['targetSkillId'], second['priorityIndex']), ('102', '12', 1))
        self.assertEqual(second['priorityRule'], 'highest_matching_index')
        self.assertEqual(second['sourceFields'], combo)
        self.assertEqual(second['sourceRow'], 0)
        self.assertEqual(second['sourcePath'], '/config/combo/s[1]')
        self.assertEqual(result['bySkill']['10'], [first['id'], second['id']])
        self.assertEqual(result['bySkill']['12'], [second['id']])
        self.assertEqual(result['byBuff']['102'], [second['id']])
        self.assertEqual(result['byBuff']['201'], [normal['id'], lighted['id']])
        self.assertEqual(lighted['kind'], 'buffer_execute_lighted_skill')
        self.assertEqual(lighted['skillField'], 'lightedSkillId')
        self.assertEqual(lighted['skillId'], '22')
        self.assertEqual(result['report']['danglingCount'], 0)

    def test_zero_is_not_a_missing_skill_or_a_created_record(self):
        self.write([{'skillId': '10', 'buffs': '101', 'targetSkillIds': '0'}],
                   [{'bufferId': '201', 'skillId': '0', 'lightedSkillId': '0'}])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(result['relations'], [])
        self.assertEqual(result['bySkill'], {})
        self.assertEqual(result['report']['danglingCount'], 0)
        self.assertEqual(result['report']['skippedNonPositiveCount'], 3)
        self.assertNotIn('0', self.skills)

    def test_dangling_references_are_preserved_without_inventing_definitions(self):
        self.write([{'skillId': '10', 'buffs': '999', 'targetSkillIds': '998'}])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(len(result['relations']), 1)
        self.assertEqual(result['report']['danglingUnique'], {'skill': ['998'], 'buff': ['999']})
        self.assertEqual({item['field'] for item in result['report']['dangling']}, {'targetSkillId', 'buffId'})
        self.assertIn('998', result['bySkill'])
        self.assertIn('999', result['byBuff'])
        self.assertNotIn('998', self.skills)
        self.assertNotIn('999', self.buffs)

    def test_mismatched_arrays_keep_indices_and_report_unpaired_values(self):
        self.write([{'skillId': '10', 'buffs': '101,102', 'targetSkillIds': '11'}])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(len(result['relations']), 1)
        self.assertEqual(result['relations'][0]['priorityIndex'], 0)
        self.assertEqual(result['report']['invalidCount'], 2)
        self.assertEqual(result['report']['invalid'][1]['priorityIndex'], 1)

    def test_repeated_prerequisite_retains_higher_priority_alternative(self):
        self.write([{'skillId': '10', 'buffs': '101,101', 'targetSkillIds': '11,12'}])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(len(result['byBuff']['101']), 2)
        self.assertEqual([r['priorityIndex'] for r in result['relations']], [0, 1])

    def test_stable_ids_ignore_unrelated_row_and_attribute_order(self):
        combo = {'skillId': '10', 'buffs': '101', 'targetSkillIds': '11'}
        self.write([combo])
        original = link_battle_tables(self.source, self.skills, self.buffs)['relations'][0]
        reordered = dict(reversed(list(combo.items())))
        self.write([{'skillId': '12', 'buffs': '102', 'targetSkillIds': '12'}, reordered])
        new = link_battle_tables(self.source, self.skills, self.buffs)['relations'][1]
        self.assertEqual(original['id'], new['id'])
        self.assertEqual(new['sourceRow'], 1)

    def test_duplicate_rows_are_preserved_and_reported(self):
        combo = {'skillId': '10', 'buffs': '101', 'targetSkillIds': '11'}
        self.write([combo, combo])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(len({r['id'] for r in result['relations']}), 2)
        self.assertEqual(result['report']['duplicateSourceKeys'][0]['sourceRows'], [0, 1])

    def test_same_skill_in_both_roles_is_not_duplicated_in_index(self):
        self.write([{'skillId': '10', 'buffs': '101', 'targetSkillIds': '10'}])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(len(result['bySkill']['10']), 1)

    def test_missing_and_out_of_range_fields_are_reported(self):
        self.write(executions=[{'bufferId': '201', 'skillId': '2147483648'}])
        result = link_battle_tables(self.source, self.skills, self.buffs)
        self.assertEqual(result['relations'], [])
        self.assertEqual(result['report']['invalidCount'], 2)


if __name__ == '__main__':
    unittest.main()
