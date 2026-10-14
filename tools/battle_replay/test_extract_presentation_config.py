"""Check client display extraction against the pinned official source snapshot."""
from pathlib import Path
import unittest
from unittest.mock import patch

import extract_presentation_config as extraction
from extract_presentation_config import extract_presentation_config, parse_formation_names, parse_string_constant_array


class PresentationConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2] / 'build-battle-reference'
        cls.result = extract_presentation_config(cls.root / 'latest-tables', cls.root)
        cls.config = cls.result['config']

    def test_current_thresholds_override_as_defaults(self):
        self.assertEqual(self.config['weakThreshold'], 0.8)
        self.assertEqual(self.config['strongThreshold'], 1.2)

    def test_enhanced_attribute_uses_explicit_column(self):
        self.assertEqual(self.config['attributeIndex']['10021'], 29)
        self.assertEqual(self.config['attributeIndex']['10028'], 36)
        self.assertEqual(self.config['godPriorities'], [2, 3])
        self.assertEqual(self.config['representerGodPriorities'], [2])

    def test_job_is_attack_type_not_numeric_range(self):
        self.assertFalse(self.config['jobs']['1']['god'])
        self.assertTrue(self.config['jobs']['16']['god'])
        self.assertFalse(self.config['jobs']['30']['god'])
        self.assertTrue(self.config['jobs']['43']['god'])

    def test_explicit_ignore_list_and_dictionary_coverage(self):
        self.assertEqual(self.config['ignoreRestraintBuffIds'], [4467, 5257, 5655])
        self.assertEqual(set(self.config['pets']), set(self.result['descriptions']))
        self.assertGreater(self.result['report']['tianQiPets'], 0)
        self.assertTrue(all(isinstance(pet['isTianQi'], bool) for pet in self.config['pets'].values()))

    def test_evidence_pins_extracted_sources_and_packages(self):
        evidence = self.config['sourceEvidence']
        for key in ('attributeTable', 'jobConsumer', 'petConsumer', 'attackConsumer', 'counterNumbers',
                    'buffHarmSources', 'formationNames', 'formationConsumer'):
            self.assertEqual(len(evidence[key]['sha256']), 64)
            self.assertEqual(len(evidence[key]['packageSha256']), 64)
        self.assertIn('-selectclass mmo.interfaces.pet.data.PetJob', evidence['jobConsumer']['rebuild'])
        self.assertIn('-selectclass mmo.interfaces.formation.FormationImageConfig', evidence['formationNames']['rebuild'])
        self.assertIn('-selectclass mmo.battle.consts.BufferConsts', evidence['buffHarmSources']['rebuild'])

    def test_harm_whitelist_is_literal_official_list(self):
        sources = self.config['buffHarmSources']
        self.assertEqual(len(sources), 43)
        self.assertEqual(sources[:5], ['爆炸', '烧伤', '冻伤', '电伤', '中毒'])
        self.assertEqual(sources[-1], '修罗冥狱')
        self.assertIn('血量上限', sources)
        self.assertNotIn('血量不显示', sources)
        self.assertNotIn('物攻', sources)
        self.assertEqual(self.config['sourceEvidence']['buffHarmSources']['sha256'],
                         self.config['sourceEvidence']['ignoreBuffs']['sha256'])

    def test_formations_include_every_declared_id_without_inventing_modifiers(self):
        formations = self.config['formations']
        self.assertEqual(set(formations), {str(number) for number in range(1, 22)})
        self.assertEqual(formations['1'], {'name': '格挡阵'})
        self.assertEqual(formations['2'], {'name': '物攻阵'})
        self.assertEqual(formations['9'], {'name': '超级阵'})
        self.assertEqual(formations['11'], {'name': '奇迹阵'})
        for number in range(12, 22):
            self.assertEqual(formations[str(number)], {'name': '百变阵'})
        self.assertTrue(all(set(value) == {'name'} for value in formations.values()))

    def test_unknown_constructor_and_id_mismatch_are_rejected(self):
        scripts = self.root / 'presentation-interfaces/scripts/mmo/interfaces/formation'
        config = (scripts / 'FormationImageConfig.as').read_text(encoding='utf-8-sig')
        define = (scripts / 'FormationDefine.as').read_text(encoding='utf-8-sig')
        with self.assertRaisesRegex(ValueError, 'Inconsistent or duplicate'):
            parse_formation_names(config.replace('"21":new FormationDefine(21,', '"22":new FormationDefine(21,'), define)
        with self.assertRaisesRegex(ValueError, 'Unsupported official formation'):
            parse_formation_names(config.replace('new FormationDefine(21,', 'new UnknownDefine(21,'), define)
        with self.assertRaisesRegex(ValueError, 'Unverified FormationDefine'):
            parse_formation_names(config, define.replace('this.name = Translator.translate(param2);', 'this.name = param4;'))

    def test_harm_array_does_not_accept_expressions_or_other_types(self):
        for expression in ('["烧伤", arbitraryName]', '["烧伤", 123]', '["烧伤", "烧伤"]', '[]'):
            with self.subTest(expression=expression), self.assertRaises(ValueError):
                parse_string_constant_array('public static const BUFF_HARM_SOURCE:Array = ' + expression + ';', 'BUFF_HARM_SOURCE')

    def test_an_unpinned_extraction_is_rejected(self):
        real_read_json = extraction.read_json
        for folder in ('presentation-interfaces', 'presentation-battle'):
            def mismatched_proof(path):
                result = real_read_json(path)
                if path.parent.name == folder and path.name == 'extraction.json':
                    result['packageSha256'] = '0' * 64
                return result
            with self.subTest(folder=folder), patch.object(extraction, 'read_json', side_effect=mismatched_proof):
                with self.assertRaisesRegex(ValueError, 'pinned .* export'):
                    extract_presentation_config(self.root / 'latest-tables', self.root)


if __name__ == '__main__':
    unittest.main()
