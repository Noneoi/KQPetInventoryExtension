import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/battle_replay'))
from extract_pet_identity import (build_race_expansions, parse_multiple_form_groups,
                                  resolve_identities)


class PetIdentityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads((ROOT / 'tests/fixtures/battle_replay_pet_identity.json').read_text(encoding='utf-8'))

    def test_same_skin_name_keeps_different_official_versions(self):
        pets = self.fixture['pets']
        self.assertEqual(pets['6528']['name'], pets['7405']['name'])
        names = resolve_identities(pets, self.fixture['skins'])
        self.assertEqual(names['6528']['baseRaceId'], '5990')
        self.assertEqual(names['6528']['canonicalName'], '[神运]虚构之神·奈非利塔')
        self.assertEqual(names['7405']['baseRaceId'], '7403')
        self.assertEqual(names['7405']['canonicalName'], '[灵初]虚构神权·奈非利塔')
        self.assertEqual(names['7403']['status'], 'native')

    def test_chaser_group_does_not_prove_its_original_form(self):
        names = resolve_identities(self.fixture['pets'], self.fixture['skins'])
        self.assertEqual(names['7383']['baseRaceId'], '7355')
        self.assertEqual(names['7384']['groupRaceId'], '7355')
        self.assertEqual(names['7384']['status'], 'unresolved')
        self.assertEqual(names['7384']['canonicalName'], '')
        # The non-cosmetic chaser has its own real dictionary name.
        self.assertEqual(names['7356']['canonicalName'], '灵初秩序追击者')

    def test_exact_skin_consumer_precedes_different_group_reference(self):
        names = resolve_identities(self.fixture['pets'], self.fixture['skins'])
        self.assertEqual(names['2488']['groupRaceId'], '2280')
        self.assertEqual(names['2488']['baseRaceId'], '2487')

    def test_missing_ambiguous_cyclic_and_fusion_records_stay_unresolved(self):
        pets = {'1': {'name': 'test skin', 'sign': '皮肤', 'groupRaceId': '2'},
                '2': {'name': 'test base', 'sign': '', 'groupRaceId': '0'}}
        def row(target, fuse=None):
            return {'id': 1, 'fuse': fuse, 'availablePets': [{'raceId': 1, 'raceIdNeed': target}]}
        for rows, reason in [([], 'missingExactSkinMapping'),
                             ([row(1)], 'cycle'), ([row(3)], 'missingDictionaryEntry'),
                             ([row(2), row(3)], 'ambiguousSkinMapping'),
                             ([row(2, [])], 'missingExactSkinMapping')]:
            with self.subTest(reason=reason):
                identity = resolve_identities(pets, rows)['1']
                self.assertEqual(identity['reason'], reason)
                self.assertEqual(identity['canonicalName'], '')

    def test_resolution_preserves_source_rows(self):
        before = copy.deepcopy(self.fixture)
        resolve_identities(self.fixture['pets'], self.fixture['skins'])
        self.assertEqual(self.fixture, before)

    def test_display_form_uses_its_explicit_first_form_not_first_skin_pet(self):
        pets = {str(i): {'name': str(i), 'sign': '皮肤' if i >= 10 else '',
                         'groupRaceId': '99'} for i in (1, 2, 10, 20, 100)}
        skins = [{'id': 7, 'availablePets': [
            {'raceId': 20, 'raceIdNeed': 2}, {'raceId': 10, 'raceIdNeed': 1}],
            'changeableRaceIds': [10, 100]}]
        # The extra consumer must be explicitly verified/enabled.
        self.assertEqual(resolve_identities(pets, skins)['100']['status'], 'unresolved')
        result = resolve_identities(pets, skins, include_display_forms=True)['100']
        self.assertEqual(result['baseRaceId'], '1')
        self.assertEqual(result['canonicalName'], '1')
        self.assertEqual(result['mappingType'], 'skinDisplayForm')
        self.assertEqual(result['chain'][0]['skinRows'][0]['baseDisplayRaceId'], '10')
        self.assertEqual(result['chain'][0]['basePaths'][0]['chain'][0]['originalRaceId'], '1')

    def test_shared_display_form_requires_all_exact_paths_to_agree(self):
        pets = {str(i): {'name': str(i), 'sign': '皮肤' if i >= 10 else '',
                         'groupRaceId': '0'} for i in (1, 2, 10, 20, 100)}
        skins = [{'id': 7, 'availablePets': [{'raceId': 10, 'raceIdNeed': 1}],
                  'changeableRaceIds': [10, 100]},
                 {'id': 8, 'availablePets': [{'raceId': 20, 'raceIdNeed': 1}],
                  'changeableRaceIds': [20, 100]}]
        result = resolve_identities(pets, skins, include_display_forms=True)['100']
        self.assertEqual(result['baseRaceId'], '1')
        self.assertEqual(len(result['chain'][0]['basePaths']), 2)
        skins[1]['availablePets'][0]['raceIdNeed'] = 2
        result = resolve_identities(pets, skins, include_display_forms=True)['100']
        self.assertEqual(result['reason'], 'ambiguousDisplayFormMapping')
        self.assertEqual(result['canonicalName'], '')

    def test_display_form_does_not_override_direct_skin_or_fusion_rules(self):
        pets = {str(i): {'name': str(i), 'sign': '皮肤' if i >= 10 else '',
                         'groupRaceId': '0'} for i in (1, 2, 10, 100)}
        skins = [{'id': 7, 'availablePets': [{'raceId': 10, 'raceIdNeed': 1}],
                  'changeableRaceIds': [10, 100]},
                 {'id': 8, 'availablePets': [{'raceId': 100, 'raceIdNeed': 2}]}]
        result = resolve_identities(pets, skins, include_display_forms=True)['100']
        self.assertEqual(result['baseRaceId'], '2')
        self.assertNotIn('mappingType', result)
        skins.pop()
        skins[0]['fuse'] = []
        self.assertEqual(resolve_identities(pets, skins, include_display_forms=True)['100']['status'],
                         'unresolved')

    def test_display_form_missing_or_cyclic_base_stays_unknown(self):
        pets = {'100': {'name': 'display', 'sign': '皮肤', 'groupRaceId': '1'}}
        skins = [{'id': 1, 'availablePets': [], 'changeableRaceIds': [10, 100]}]
        self.assertEqual(resolve_identities(pets, skins, include_display_forms=True)['100']['reason'],
                         'missingDictionaryEntry')
        pets['10'] = {'name': 'base', 'sign': '皮肤', 'groupRaceId': '1'}
        skins.append({'id': 2, 'availablePets': [], 'changeableRaceIds': [100, 10]})
        self.assertEqual(resolve_identities(pets, skins, include_display_forms=True)['100']['reason'],
                         'cycle')

    def test_official_required_race_branch_is_not_a_group_fallback(self):
        pets = {'1': {'name': 'official requirement', 'sign': '皮肤', 'groupRaceId': '999'},
                '2': {'name': 'skin', 'sign': '皮肤', 'groupRaceId': '999'}}
        skins = [{'id': 1, 'availablePets': [{'raceId': 2, 'raceIdNeed': 1}]}]
        self.assertEqual(resolve_identities(pets, skins)['1']['baseRaceId'], '1')
        self.assertEqual(resolve_identities(pets, skins)['2']['baseRaceId'], '1')
        # The same ID being a fusion record is explicitly refused by the consumer.
        skins.append({'id': 2, 'fuse': [], 'availablePets': [{'raceId': 1, 'raceIdNeed': 0}]})
        self.assertEqual(resolve_identities(pets, skins)['1']['status'], 'unresolved')

    def test_compatibility_expansion_uses_public_skin_getter_then_one_form_group(self):
        skins = [{'id': 1, 'show': True, 'availablePets': [{'raceId': 10, 'raceIdNeed': 1}]},
                 {'id': 2, 'show': True, 'availablePets': [{'raceId': 20, 'raceIdNeed': 2}]},
                 {'id': 3, 'show': True, 'availablePets': [{'raceId': 30, 'raceIdNeed': 3}]}]
        expanded = build_race_expansions([10, 2], skins, [[1, 2], [2, 3]])
        self.assertEqual(expanded['10'], ['1', '2', '10', '20'])
        self.assertEqual(expanded['2'], ['1', '2', '10', '20'])
        self.assertNotIn('3', expanded['10'])  # No recursive group expansion.

    def test_hidden_and_fusion_skin_compatibility_do_not_reset_to_native(self):
        skins = [{'id': 1, 'show': False, 'availablePets': [{'raceId': 10, 'raceIdNeed': 1}]},
                 {'id': 2, 'show': True, 'availablePets': [{'raceId': 20, 'raceIdNeed': 1}]},
                 {'id': 3, 'show': True, 'fuse': [],
                  'availablePets': [{'raceId': 30, 'raceIdNeed': 1}]}]
        result = build_race_expansions([10, 30, 99], skins, [])
        self.assertEqual(result, {'10': ['10'], '30': ['30'], '99': ['99']})

    def test_form_group_parser_preserves_overlaps_and_rejects_expressions(self):
        text = 'const FORM_CONF:Array = [new MultipleFormPetFormDefine([1,2]),new MultipleFormPetFormDefine([2,3])];'
        self.assertEqual(parse_multiple_form_groups(text), [[1, 2], [2, 3]])
        with self.assertRaises(ValueError):
            parse_multiple_form_groups(text.replace('[2,3]', 'getForms()'))


if __name__ == '__main__':
    unittest.main()
