"""Official relation semantics; deliberately unrelated synthetic race numbers."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_pet_relations import build_relation_rules, legacy_relation_views, SkinRelations


def skin(identifier, race, base, show=True, **extra):
    return {'id': identifier, 'show': show, 'fuse': None, 'xChangeableInfo': None,
            'availablePets': [{'raceId': race, 'raceIdNeed': base}], **extra}


class PublicPetRelationsTests(unittest.TestCase):
    def setUp(self):
        self.skins = {'defines': [skin(1, 101, 100), skin(2, 103, 102),
                                   skin(3, 201, 200), skin(4, 203, 202)]}
        self.forms = [[100, 102], [200, 202]]
        self.pets = {str(race): {'orgName': f'官方种族{race}', 'signs': '携同技' if race < 200 else ''}
                     for race in (100, 102, 200, 202, 300)}
        self.pets.update({str(race): {'orgName': f'皮肤{race}', 'signs': '皮肤'}
                          for race in (101, 103, 201, 203)})

    def build(self, summons=None, hidden=None, carries=None, **extra):
        return build_relation_rules(summons or [], hidden or [], carries or [], self.skins,
                                    self.forms, self.pets, lambda value: str(value or ''), **extra)

    def test_carry_resets_skin_expands_forms_then_skins_and_ignores_hash_tail(self):
        rules = self.build(carries=[{'c': '101#300', 'ced': '201#300', 'type': 4}])
        group = rules['carry']['bySourceRaceId']
        self.assertEqual(set(group), {'100', '101', '102', '103'})
        for rows in group.values():
            self.assertEqual([row['targetRaceId'] for row in rows], [200, 201, 202, 203])
            self.assertEqual([row['baseRaceId'] for row in rows], [200, 200, 202, 202])
            self.assertEqual([row['listedWithoutOwnership'] for row in rows], [True, False, True, False])

    def test_carry_duplicate_expanded_pairs_use_last_definition_and_role(self):
        rules = self.build(carries=[{'c': '100', 'ced': '200', 'type': 4, 'sdes': 'obsolete'},
                                   {'c': '102', 'ced': '202', 'type': 16, 'sdes': 'final'}], include_text=True)
        self.assertEqual({row['type'] for rows in rules['carry']['bySourceRaceId'].values() for row in rows}, {16})
        _, legacy = legacy_relation_views(rules)
        self.assertTrue(all(row['description'] == 'final' for rows in legacy.values() for row in rows))
        self.assertEqual(len(legacy['100']), 1)
        self.assertEqual(legacy['100'][0]['related'], [200, 201, 202, 203])

    def test_summon_main_then_hidden_override_keeps_exact_pairs_for_owned_matching(self):
        rules = self.build(summons=[{'s': '100#102', 'sed': '200#201', 'isShow': 1}],
                           hidden=[{'s': '100', 'sed': '200', 'isShow': 0}])
        rows = rules['summon']['bySourceRaceId']['100']
        self.assertEqual([row['targetRaceId'] for row in rows], [200, 201])
        self.assertFalse(rows[0]['isShow'])
        self.assertFalse(rows[0]['listedWithoutOwnership'])
        self.assertTrue(rows[1]['isShow'])
        self.assertFalse(rows[1]['listedWithoutOwnership'])
        self.assertTrue(rules['summon']['bySourceRaceId']['102'][0]['listedWithoutOwnership'])

    def test_summon_does_not_invent_skin_or_multiple_form_pairs(self):
        rules = self.build(summons=[{'s': '100', 'sed': '200', 'isShow': 1}])
        self.assertEqual(set(rules['summon']['bySourceRaceId']), {'100'})
        self.assertEqual([row['targetRaceId'] for row in rules['summon']['bySourceRaceId']['100']], [200])

    def test_missing_sheet_is_unknown_while_explicit_empty_sheets_are_known(self):
        empty = build_relation_rules([], [], [], {'defines': []}, [], {}, str)
        self.assertTrue(empty['summon']['available'])
        self.assertTrue(empty['carry']['available'])
        self.assertEqual(empty['carry']['bySourceRaceId'], {})
        for field in range(5):
            args = [[], [], [], {'defines': []}, []]
            args[field] = None
            result = build_relation_rules(*args, {}, str)
            group = 'summon' if field in (0, 1) else 'carry'
            self.assertFalse(result[group]['available'])

    def test_incomplete_or_malformed_existing_rows_fail_instead_of_disappearing(self):
        for carries in ([{'c': '100', 'ced': '200'}], [{'c': '', 'ced': '200', 'type': 4}],
                        [{'c': 'garbage', 'ced': '200', 'type': 4}]):
            with self.assertRaises(ValueError):
                self.build(carries=carries)
        with self.assertRaises(ValueError):
            self.build(summons=[{'s': '100', 'isShow': 1}])
        with self.assertRaises(ValueError):
            SkinRelations({'defines': [{'id': 1, 'availablePets': []}]})

    def test_hidden_skin_is_not_expanded_and_fusion_poster_is_not_an_alias(self):
        self.skins['defines'][0]['show'] = False
        self.skins['defines'].append(skin(5, 999, 0, fuse=[1, 2]))
        rules = self.build(carries=[{'c': '101', 'ced': '200', 'type': 4},
                                   {'c': '999', 'ced': '200', 'type': 8}])
        self.assertEqual(set(rules['carry']['bySourceRaceId']), {'101', '999'})

    def test_multiple_form_lookup_is_first_group_and_not_transitive(self):
        self.forms = [[100, 102], [102, 300]]
        rules = self.build(carries=[{'c': '100', 'ced': '200', 'type': 4}])
        self.assertNotIn('300', rules['carry']['bySourceRaceId'])

    def test_changeable_skin_mapping_and_names_come_from_public_data(self):
        self.skins['defines'].append(skin(5, 204, 200, xChangeableInfo='新形态@3'))
        self.pets['204'] = {'orgName': '另一套皮肤', 'signs': '皮肤'}
        rules = self.build(carries=[{'c': '100', 'ced': '200', 'type': 8}])
        rows = rules['carry']['bySourceRaceId']['100']
        item = next(row for row in rows if row['targetRaceId'] == 204)
        self.assertEqual(item['baseRaceId'], 200)
        self.assertEqual(item['originalName'], '官方种族200')

    def test_unverified_identity_keeps_exact_target_without_fuzzy_name_merging(self):
        self.pets['200'].pop('signs')
        rules = self.build(carries=[{'c': '100', 'ced': '200', 'type': 4}])
        row = next(item for item in rules['carry']['bySourceRaceId']['100'] if item['targetRaceId'] == 201)
        self.assertEqual((row['baseRaceId'], row['originalName']), (201, ''))

    def test_renamed_and_renumbered_input_uses_identical_general_rules(self):
        self.skins = {'defines': [skin(1, 9011, 9010), skin(2, 9021, 9020)]}
        self.forms = []
        self.pets = {'9010': {'orgName': '新携带者', 'signs': '吞噬技'},
                     '9011': {'orgName': '新皮肤甲', 'signs': '皮肤'},
                     '9020': {'orgName': '新书灵', 'signs': ''}, '9021': {'orgName': '新皮肤乙', 'signs': '皮肤'}}
        rules = self.build(carries=[{'c': '9011', 'ced': '9021', 'type': 16}])
        self.assertEqual(set(rules['carry']['bySourceRaceId']), {'9010', '9011'})
        self.assertEqual({row['originalName'] for row in rules['carry']['bySourceRaceId']['9010']}, {'新书灵'})


if __name__ == '__main__':
    unittest.main()
