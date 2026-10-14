"""Official power-rule parsing contracts; fixtures stay in temporary folders."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import generate_pet_detail_data as catalog


JOBS = '''
private static const AT_PHYSICS:PetAttackType = PetAttackType.PHYSICS;
private static const AT_MAGIC:PetAttackType = PetAttackType.MAGIC;
public static const CLAW:PetJob = enum__(1,"physical",AT_PHYSICS);
public static const MAGIC:PetJob = enum__(2,"magic",AT_MAGIC);
public static const WORLD_BOSS:PetJob = enum__(13,"boss",AT_PHYSICS);
public static const GOD_BOSS:PetJob = enum__(16,"god boss",AT_PHYSICS);
public static const NEW_JOB:PetJob = enum__(44,"future job",PetAttackType.MAGIC);
'''


def item(identifier, star_type, quality=6, limited="false"):
    return f'new StarGodItem({identifier},138,"star",0,0,false,999999,false,999999,"description, with punctuation",{star_type},{quality},{limited},"S")'


def service():
    return '''
new StarGodType(1,"physical",1,jobIds(PetAttackType.MAGIC));
new StarGodType(17,"magic",4,jobIds(PetAttackType.PHYSICS));
new StarGodType(25,"changeable",99,[]);
PetJob.valuesOfAttackType(PetAttackType.PHYSICS);
if(job != PetJob.WORLD_BOSS && job != PetJob.GOD_BOSS) {}
''' + ";".join((item(71, 1), item(65, 17, limited="true"), item(80, 25)))


class PowerMetadataTests(unittest.TestCase):
    def test_current_job_enums_drive_star_restrictions_and_exclude_bosses(self):
        rules = catalog.parse_stargod_rules(service(), JOBS)
        self.assertEqual(rules["71"]["limitJobs"], [2, 44])
        self.assertEqual(rules["65"]["limitJobs"], [1])
        self.assertTrue(rules["65"]["limited"])
        self.assertTrue(rules["80"]["changeable"])
        self.assertFalse(rules["71"]["changeable"])
        self.assertEqual(rules["71"]["quality"], 6)

    def test_unresolved_new_job_group_fails_instead_of_allowing_wrong_star_power(self):
        with self.assertRaises(ValueError):
            catalog.parse_stargod_rules(service().replace("jobIds(PetAttackType.MAGIC)", "jobIds(PetAttackType.FUTURE)"), JOBS)

    def test_constructor_field_changes_fail_instead_of_using_guessed_quality(self):
        with self.assertRaises(ValueError):
            catalog.parse_stargod_rules(service().replace(',17,6,true,"S")', ',17,99,true,"S")'), JOBS)

    def test_star_sheet_must_match_material_and_preserves_official_level_power(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            swf, material, jobs = root / "stars.swf", root / "Material.as", root / "PetJob.as"
            material.write_text(service(), encoding="utf-8")
            jobs.write_text(JOBS, encoding="utf-8")
            swf.write_bytes(b'FWS' + ''.join(f'<s defineId="{id}" name="star" supplyExp="1080"><l level="8" battlePower="650"/></s>'
                                           for id in (65, 71, 80)).encode())
            result = catalog.parse_stargod_catalog(swf, material, jobs)
            self.assertEqual(result["71"]["battlePower"], {"8": 650})
            self.assertEqual(result["71"]["type"], 1)
            swf.write_bytes(swf.read_bytes().replace(b'defineId="71"', b'defineId="99"'))
            with self.assertRaises(ValueError):
                catalog.parse_stargod_catalog(swf, material, jobs)

    def test_astrolabe_rule_values_are_typed_and_missing_values_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            swf = Path(temporary) / "astrolabe.swf"
            swf.write_bytes(b'FWS<s defineId="1" name="node" locatedTypeId="7" exclusive="1" battlePower="600" lightUpCost="8:1:20"/>')
            self.assertEqual(catalog.parse_astrolabe_catalog(swf)["1"], {
                "name": "node", "locatedType": 7, "battlePower": 600, "exclusive": True, "lightUpCost": "8:1:20",
        "lightUpMaterials": [{"type": 8, "id": 1, "count": 20}], "isTBD": False})
            swf.write_bytes(swf.read_bytes().replace(b' battlePower="600"', b''))
            with self.assertRaises(ValueError):
                catalog.parse_astrolabe_catalog(swf)

    def test_badge_awakening_keeps_actual_item_id_and_target_level_costs(self):
        with tempfile.TemporaryDirectory() as temporary:
            swf = Path(temporary) / "badge.swf"
            swf.write_bytes(b'FWS<s defineId="1" name="job" type="0"><l level="1" battlePower="120" materialStrToUpgrade=""/>'
                            b'<l level="2" battlePower="240" materialStrToUpgrade="8:1:3000#4:1384:20"/></s>'
                            b'<s defineId="647" name="exclusive" type="1" battlePower="200" materialStrToActivate="4:1450:1"></s>')
            result = catalog.parse_badges(swf)
            self.assertEqual(result["1"]["levels"]["1"]["cost"], [])
            self.assertEqual(result["1"]["levels"]["2"]["cost"][-1], {"type": 4, "id": 1384, "count": 20})
            self.assertEqual(result["647"]["activationCost"], [{"type": 4, "id": 1450, "count": 1}])
            self.assertEqual(result["647"]["maxLevel"], 1)
            swf.write_bytes(swf.read_bytes().replace(b'materialStrToActivate="4:1450:1"', b''))
            with self.assertRaises(ValueError):
                catalog.parse_badges(swf)

    def test_sacred_plan_retains_two_copy_and_zero_copy_transitions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = root / "Equipment.as"
            definition.write_text('"1046":{"id":1046,"name":"source","sourceId":"46","isOnline":1,"desc":"x"}', encoding="utf-8")
            self.assertEqual(catalog.parse_sacred(definition)["1046"]["sourceId"], 46)
            sources = root / "Equipment4PetItemService.as"
            sources.write_text('new Equipment4PetItem(46,24,"actual consumable",0)', encoding="utf-8")
            self.assertEqual(catalog.parse_sacred(definition, sources)["1046"]["sourceName"], "actual consumable")
            plan = root / "Stage.as"
            plan.write_text('new SE_StageDefine(13,"plan",['
                            'new SE_StageLevelDefine(1,[1,2],[0,0],300,"",2),'
                            'new SE_StageLevelDefine(2,[2,4],[0,0],600,"4:3092:100",0),'
                            'new SE_StageLevelDefine(3,[3,6],[0,0],900,"",0)])', encoding="utf-8")
            result = catalog.parse_sacred_plans(plan, "Stage")["13"]
            self.assertEqual(result["maxLevel"], 3)
            self.assertEqual([row["equipmentCount"] for row in result["levels"].values()], [2, 0, 0])
            self.assertEqual(result["levels"]["2"]["cost"], [{"type": 4, "id": 3092, "count": 100}])
            plan.write_text(plan.read_text().replace('Define(2,', 'Define(4,'), encoding="utf-8")
            with self.assertRaises(ValueError):
                catalog.parse_sacred_plans(plan, "Stage")

    def test_special_badge_items_and_essence_names_are_not_lost(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "ItemData_Update.as").write_text('"1451":new ItemForBadge(1451,"薇斯佩拉元魂",18,9999),'
                                                    '"2078":new ItemForEssence(2078,"聚星精华",18,9999)', encoding="utf-8")
            self.assertEqual(catalog.parse_items(root), {"1451": {"name": "薇斯佩拉元魂"}, "2078": {"name": "聚星精华"}})

    def test_material_four_elements_preserves_variant_and_rejects_negative_cost(self):
        self.assertEqual(catalog.parse_material_cost("24:46:1:2#"), [{"type": 24, "id": 46, "extra": 1, "count": 2}])
        with self.assertRaises(ValueError):
            catalog.parse_material_cost("4:1:-20")


class PetDictionaryMergeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.base_root, self.update_root = self.root / "z-base", self.root / "a-update"
        self.base_root.mkdir()
        self.update_root.mkdir()
        self.base = self.base_root / "PetDictionaryDataContents.as"
        self.update = self.update_root / "PetDictionaryDataContentsUpdate.as"

    @staticmethod
    def row(identifier, name):
        args = ["0"] * 67
        args[0], args[1] = str(identifier), json.dumps(name)
        args[8], args[9], args[28] = '"26,28"', '"22"', '"神运,灵初,皮肤"'
        args[30], args[42], args[53], args[62], args[63] = "120", "8000", '"SP"', '"8:51:100"', "8"
        return json.dumps(str(identifier)) + ":PetDictionaryDataItem.create(" + ",".join(args) + ")"

    @staticmethod
    def write_table(path, timestamp, rows):
        path.write_text(f"public static const timestamp:Number = {timestamp};\n"
                        "public static const datas:Object = {" + ",".join(rows) + "};", encoding="utf-8")

    def test_newer_update_overrides_adds_and_preserves_every_workbench_pet_field(self):
        self.write_table(self.base, 100, [self.row(9001, "base"), self.row(9002, "retained")])
        self.write_table(self.update, 101, [self.row(9001, "updated"), self.row(9003, "added")])
        merged = catalog.merge_pet_sources([self.update_root, self.base_root])
        self.assertEqual(set(merged), {"9001", "9002", "9003"})
        self.assertEqual(merged["9001"], {
            "name": "updated", "attributes": "26,28", "jobs": "22", "groupRaceId": 8000,
            "sign": "神运,灵初,皮肤", "quality": "SP", "astrolabeBreakCosts": "8:51:100",
            "maxLevel": 120, "stargodSlotMaxLevel": 8,
        })
        self.assertEqual(merged["9002"]["name"], "retained")
        self.assertEqual(merged["9003"]["name"], "added")
        self.assertEqual(catalog.merge_pet_sources([self.root]), merged)

    def test_equal_and_older_timestamps_ignore_override_and_update_only_ids(self):
        self.write_table(self.base, 100, [self.row(9001, "base")])
        for timestamp in (100, 99):
            with self.subTest(timestamp=timestamp):
                self.write_table(self.update, timestamp, [self.row(9001, "unused"), self.row(9002, "unused new")])
                merged = catalog.merge_pet_sources([self.base_root, self.update_root])
                self.assertEqual(set(merged), {"9001"})
                self.assertEqual(merged["9001"]["name"], "base")

    def test_newer_legal_empty_update_keeps_base(self):
        self.write_table(self.base, 100, [self.row(9001, "base")])
        self.write_table(self.update, 101, [])
        self.assertEqual(catalog.merge_pet_sources([self.root]), catalog.parse_pets(self.base_root))

    def test_missing_ambiguous_or_nonliteral_timestamp_is_not_guessed(self):
        self.write_table(self.base, 100, [self.row(9001, "base")])
        for declaration in ("", "const timestamp:Number = NEXT;",
                            "const timestamp:Number = 101; const timestamp:Number = 102;"):
            with self.subTest(declaration=declaration):
                self.update.write_text(declaration + "const datas:Object = {};", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "timestamp"):
                    catalog.merge_pet_sources([self.root])

    def test_conflicting_dictionary_copies_do_not_depend_on_directory_order(self):
        self.write_table(self.base, 100, [self.row(9001, "base")])
        self.write_table(self.update, 101, [])
        conflict = self.update_root / self.base.name
        self.write_table(conflict, 100, [self.row(9001, "different")])
        with self.assertRaisesRegex(ValueError, "ambiguous official pet dictionary"):
            catalog.merge_pet_sources([self.root])

    def test_missing_pair_empty_base_or_unreadable_delta_fails(self):
        self.write_table(self.base, 100, [self.row(9001, "base")])
        with self.assertRaisesRegex(ValueError, "incomplete"):
            catalog.merge_pet_sources([self.root])
        self.write_table(self.update, 101, ['"9001":PetDictionaryDataItem.create(9001)'])
        with self.assertRaisesRegex(ValueError, "not a readable table"):
            catalog.merge_pet_sources([self.root])
        self.write_table(self.base, 100, [])
        self.write_table(self.update, 101, [])
        with self.assertRaisesRegex(ValueError, "base pet dictionary is empty"):
            catalog.merge_pet_sources([self.root])


if __name__ == "__main__":
    unittest.main()
