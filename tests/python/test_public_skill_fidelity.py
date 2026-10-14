#!/usr/bin/env python3
"""Synthetic corruption tests; fixtures are not purported official game data."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from public_skill_fidelity import audit_skill_fidelity, proven_skill_corrections


def pet_row(race, name):
    row = [""] * 67
    row[0], row[1], row[8], row[9] = race, name, "4", "1"
    for index in (14, 15, 16, 17, 31, 35, 36, 37, 46, 49, 59):
        row[index] = 0
    row[14], row[15] = 101, 202
    return row


def fixture():
    one, two = pet_row(1, "合成甲"), pet_row(2, "合成乙")
    two[28], two[36], two[65] = "六星神,寰力量", 700002, "entry#second"
    one[19], one[20] = "synthetic star", "real blank allowed"
    almighty = {"id": "2", "hn": "hero", "hd": "hero body", "sn": "summon", "sd": "summon body",
                "pn": "psychics", "pd": "psychics body", "psci": "1"}
    tables = {
        "petdictionarydata.json/petdictionarydata.json": {"1": one, "2": two},
        "battleconfig/skill_desc_config.json": {"101": "First|<b>first body</b>|AaB0E", "202": "Second|second body|BaB0E", "303": "||"},
        "battleconfig/skill_config.json": {"101": {"action": 1, "effect": 4, "hideself": 1}, "202": {"action": 2, "jump": 3}},
        "battleconfig/buffer_config.json": {"buff": {"10": "Named|body|645|effect", "11": "Empty name body|||", "12": "|official unnamed|11|animation"}},
        "battleconfig/combo_buffs_config.json": {"combo": [{"skillId": 101, "buffs": "10,11", "targetSkillIds": "202,303"}]},
        "battleconfig/entry_config.json": [{"n": "entry", "d": "description", "t": 2}, {"n": "blank", "d": ""}],
        "formation/transformskillconfig.json": {"505": [505, "transform", "desc", "auto desc", ["X", "Y"], ["Z"], 1]},
        "formation/heroskillconfig.json": {"1": [{"name": "first"}, {"name": "second"}]},
        "formation/psychicsskillconfig.json": {"2": [{"x": [1, 2], "flag": False}]},
        "formation/lingchuskillsulingconfig.json": {"3": {"x": "a"}},
        "formation/shenyunskillqiyunconfig.json": {"4": {"x": [1, 3]}},
        "battleconfig/almighty-skill-config.json": [almighty, dict(almighty, id="120002", hn="distinct skin hero")],
        "battleconfig/huan_skill_config.json": {"700002": ["a|1|0|0|text A", "a|1|5|1|text B"], "701": ["must not match race 1 by suffix"]},
    }
    pets = {}
    for race, name in (("1", "合成甲"), ("2", "合成乙")):
        pets[race] = {"name": name, "attributes": "4", "jobs": "1", "signs": "", "strategy": "", "position": "", "quality": "",
                      "slots": {"normal": 101, "ultimate": 202, "super": 0, "hero": 0, "large": 0, "dragonJob": 0, "yuanLi": 0, "psychics": 0, "fate": 0, "transform": 0},
                      "skillEntries": [], "qiYunType": 0, "traits": []}
    pets["1"]["starGodSkill"] = {"name": "synthetic star", "description": "real blank allowed"}
    pets["2"].update({"signs": "六星神,寰力量", "skillEntries": ["entry", "second"], "traits": ["寰力量"],
                      "huan": ["a|1|0|0|text A", "a|1|5|1|text B"],
                      "almighty": [{"id": 2, "heroName": "hero", "heroDescription": "hero body", "summonName": "summon", "summonDescription": "summon body", "psychicsName": "psychics", "psychicsDescription": "psychics body", "calledRaceId": 1}]})
    pets["2"]["slots"]["yuanLi"] = 700002
    catalog = {"schema": 1, "source": {"untrusted": "candidate claim"}, "pets": pets,
               "skills": {"101": {"name": "First", "description": "first body", "encoding": "AaB0E", "animation": {"action": 1, "effect": 4}},
                          "202": {"name": "Second", "description": "second body", "encoding": "BaB0E", "animation": {"action": 2, "jump": 3}}},
               "buffs": {"10": {"name": "Named", "description": "body", "effect": "effect"}, "11": {"name": "Empty name body"}},
               "entries": {"entry": "description"}, "combos": {"101": [{"buff": 10, "skill": 202}, {"buff": 11, "skill": 303}]},
               "transformSkills": {"505": {"name": "transform", "description": "desc", "teamDescription": "auto desc", "targets": ["X", "Y"], "effects": ["Z"], "type": 1}}}
    for output, file in (("heroSkills", "formation/heroskillconfig.json"), ("psychicsSkills", "formation/psychicsskillconfig.json"),
                         ("lingchuRules", "formation/lingchuskillsulingconfig.json"), ("shenyunRules", "formation/shenyunskillqiyunconfig.json"),
                         ("almightySkills", "battleconfig/almighty-skill-config.json")):
        catalog[output] = deepcopy(tables[file])
    return tables, catalog


class FidelityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.tables, self.catalog = fixture()
        self.write_sources()

    def write_sources(self):
        for name, value in self.tables.items():
            p = self.root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(json.dumps(value, ensure_ascii=False), encoding="utf8")

    def audit(self):
        return audit_skill_fidelity(self.root, self.catalog)

    def rejected_at(self, path):
        result = self.audit()
        self.assertFalse(result["passed"])
        self.assertTrue(any(f["path"].startswith(path) for f in result["findings"]), result["findings"])
        return result

    def test_exact_projection_passes_without_mutation(self):
        before = deepcopy(self.catalog)
        result = self.audit()
        self.assertTrue(result["passed"], result["findings"])
        self.assertEqual(before, self.catalog)
        self.assertEqual(13, result["summary"]["inputFiles"])
        self.assertEqual(1, result["summary"]["officialUnnamedBuffsOmitted"])

    def test_swapped_skill_descriptions_fail_even_with_equal_counts(self):
        a, b = self.catalog["skills"].values()
        a["description"], b["description"] = b["description"], a["description"]
        report = self.rejected_at("/skills/")
        self.assertEqual(2, report["summary"]["errors"])

    def test_fresh_official_text_and_matching_output_pass(self):
        self.tables["battleconfig/skill_desc_config.json"]["101"] = "Changed|new official body|AaB0E"
        self.catalog["skills"]["101"].update(name="Changed", description="new official body")
        self.write_sources()
        self.assertTrue(self.audit()["passed"])

    def test_official_empty_description_is_valid_but_erasing_text_is_not(self):
        self.tables["battleconfig/skill_desc_config.json"]["101"] = "First||AaB0E"
        self.catalog["skills"]["101"]["description"] = ""
        self.write_sources()
        self.assertTrue(self.audit()["passed"])
        self.catalog["skills"]["202"]["description"] = ""
        self.rejected_at("/skills/202/description")

    def test_named_description_without_name_is_kept_for_skills(self):
        self.tables["battleconfig/skill_desc_config.json"]["303"] = "|description only|"
        self.catalog["skills"]["303"] = {"name": "", "description": "description only"}
        self.write_sources()
        self.assertTrue(self.audit()["passed"])

    def test_skill_wrong_id_same_count_fails(self):
        self.catalog["skills"]["999"] = self.catalog["skills"].pop("101")
        result = self.rejected_at("/skills/101")
        self.assertTrue(any(x["path"] == "/skills/999" for x in result["findings"]))

    def test_missing_row_fails(self):
        del self.catalog["buffs"]["10"]
        self.rejected_at("/buffs/10")

    def test_unexpected_row_fails(self):
        self.catalog["buffs"]["12"] = {"name": "invented", "description": "official unnamed"}
        self.rejected_at("/buffs/12")

    def test_new_official_skill_and_matching_candidate_pass(self):
        self.tables["battleconfig/skill_desc_config.json"]["999"] = "Added|body|"
        self.catalog["skills"]["999"] = {"name": "Added", "description": "body"}
        self.write_sources()
        self.assertTrue(self.audit()["passed"])

    def test_animation_corruption_fails(self):
        self.catalog["skills"]["101"]["animation"]["effect"] = 3
        self.rejected_at("/skills/101/animation/effect")

    def test_packed_buff_effect_is_not_confused_with_display_id(self):
        self.catalog["buffs"]["10"]["effect"] = "645"
        self.rejected_at("/buffs/10/effect")

    def test_original_pet_name_and_slot_are_checked(self):
        self.catalog["pets"]["1"]["name"] = "wrong skin alias"
        self.catalog["pets"]["2"]["slots"]["normal"] = 202
        report = self.rejected_at("/pets/1/name")
        self.assertTrue(any(x["path"] == "/pets/2/slots/normal" for x in report["findings"]))

    def test_bool_is_not_valid_numeric_slot(self):
        self.catalog["pets"]["1"]["slots"]["super"] = False
        self.rejected_at("/pets/1/slots/super")

    def test_raw_source_dictionary_key_id_mismatch_fails(self):
        self.tables["petdictionarydata.json/petdictionarydata.json"]["1"][0] = 2
        self.write_sources()
        self.rejected_at("/inputs/dictionary/1")

    def test_raw_source_new_column_requires_review(self):
        self.tables["petdictionarydata.json/petdictionarydata.json"]["1"].append("new")
        self.write_sources()
        self.rejected_at("/inputs/dictionary/1")

    def test_source_text_type_change_cannot_be_hidden_by_string_coercion(self):
        self.tables["petdictionarydata.json/petdictionarydata.json"]["1"][1] = 12
        self.catalog["pets"]["1"]["name"] = "12"
        self.write_sources()
        self.rejected_at("/inputs/dictionary/1/1")

    def test_transform_new_text_type_cannot_be_silently_coerced(self):
        self.tables["formation/transformskillconfig.json"]["505"][2] = None
        self.catalog["transformSkills"]["505"]["description"] = ""
        self.write_sources()
        self.rejected_at("/inputs/transforms/505")

    def test_combo_order_is_semantic(self):
        self.catalog["combos"]["101"].reverse()
        self.rejected_at("/combos/101/0")

    def test_transform_array_order_is_preserved(self):
        self.catalog["transformSkills"]["505"]["targets"].reverse()
        self.rejected_at("/transformSkills/505/targets/0")

    def test_raw_rules_order_and_types_are_checked(self):
        for table, path, change in (
            ("heroSkills", "/heroSkills/1/0", lambda t: t["1"].reverse()),
            ("psychicsSkills", "/psychicsSkills/2/0/flag", lambda t: t["2"][0].update(flag=0)),
            ("lingchuRules", "/lingchuRules/3/x", lambda t: t["3"].update(x="b")),
            ("shenyunRules", "/shenyunRules/4/x/0", lambda t: t["4"]["x"].reverse()),
        ):
            with self.subTest(table=table):
                old = deepcopy(self.catalog[table])
                change(self.catalog[table])
                self.rejected_at(path)
                self.catalog[table] = old

    def test_huan_uses_exact_slot_and_exact_sign(self):
        self.catalog["pets"]["1"]["huan"] = ["must not match race 1 by suffix"]
        self.rejected_at("/pets/1/huan")
        del self.catalog["pets"]["1"]["huan"]
        self.tables["petdictionarydata.json/petdictionarydata.json"]["2"][28] = "不是寰力量"
        self.catalog["pets"]["2"]["signs"] = "不是寰力量"
        self.write_sources()
        self.rejected_at("/pets/2/huan")

    def test_almighty_never_modulos_or_inherits_from_form_group(self):
        self.catalog["pets"]["2"]["almighty"].append(dict(self.catalog["pets"]["2"]["almighty"][0], id=120002))
        self.rejected_at("/pets/2/almighty")

    def test_almighty_exact_row_is_independent_of_raw_table_pass(self):
        self.catalog["pets"]["2"]["almighty"][0]["heroName"] = "distinct skin hero"
        self.rejected_at("/pets/2/almighty/0/heroName")

    def test_candidate_source_claim_does_not_excuse_bad_value(self):
        self.catalog["source"] = {"verified": True, "officialProjectionPassed": True}
        self.catalog["skills"]["101"]["description"] = "fabricated"
        self.rejected_at("/skills/101/description")

    def test_input_hash_is_actual_bytes(self):
        report = self.audit()
        row = report["inputs"]["skillText"]
        self.assertEqual(hashlib.sha256((self.root / row["file"]).read_bytes()).hexdigest(), row["sha256"])

    def test_missing_source_file_fails_without_parent_fallback(self):
        (self.root / "battleconfig/skill_desc_config.json").unlink()
        self.rejected_at("/inputs/skillText")

    def test_duplicate_json_source_keys_fail(self):
        (self.root / "battleconfig/skill_desc_config.json").write_text('{"101":"a|b","101":"c|d"}', encoding="utf8")
        self.rejected_at("/inputs/skillText")

    def test_nonfinite_source_json_fails(self):
        (self.root / "battleconfig/skill_config.json").write_text('{"101":{"action":NaN}}', encoding="utf8")
        self.rejected_at("/inputs/animations")

    def test_ambiguous_dictionary_layout_fails(self):
        p = self.root / "pet/petdictionarydata.json"
        p.parent.mkdir()
        p.write_text("{}", encoding="utf8")
        self.rejected_at("/inputs/dictionary")

    def test_unchecked_fields_are_explicit_and_do_not_claim_semantics(self):
        self.catalog["pets"]["1"]["relations"] = [{"description": "not independently checked"}]
        report = self.audit()
        self.assertTrue(report["passed"])
        self.assertIn({"path": "/pets/*/relations", "rows": 1}, report["coverage"]["notChecked"])

    def test_no_generator_execution_or_shared_mapping(self):
        with patch.dict(sys.modules, {"generate_pet_skill_data": None}):
            self.assertTrue(self.audit()["passed"])

    def test_corrections_are_exactly_bound_to_old_and_proven_new_values(self):
        before = deepcopy(self.catalog)
        before["pets"]["1"]["huan"] = ["suffix collision"]
        before["pets"]["2"]["almighty"].append(dict(before["pets"]["2"]["almighty"][0], id=120002))
        before["pets"]["1"]["traits"] = ["寰力量"]
        result = audit_skill_fidelity(self.root, self.catalog, previous=before)
        self.assertTrue(result["passed"])
        corrections = {row["path"]: row for row in result["allowedCorrections"]}
        self.assertEqual({"/pets/1/huan", "/pets/1/traits", "/pets/2/almighty"}, set(corrections))
        huan = corrections["/pets/1/huan"]
        self.assertTrue(huan["previousPresent"])
        self.assertFalse(huan["candidatePresent"])
        self.assertIsNone(huan["candidateValueSha256"])
        expected = hashlib.sha256(json.dumps(["suffix collision"], ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf8")).hexdigest()
        self.assertEqual(expected, huan["previousValueSha256"])
        self.assertEqual(result["inputs"]["huan"]["sha256"], huan["source"]["sha256"])
        self.assertEqual(result["allowedCorrections"], proven_skill_corrections(self.root, self.catalog, before))

    def test_failed_audit_never_grants_correction_exceptions(self):
        before = deepcopy(self.catalog)
        before["pets"]["1"]["huan"] = ["old suffix collision"]
        self.catalog["skills"]["101"]["description"] = "wrong"
        result = audit_skill_fidelity(self.root, self.catalog, previous=before)
        self.assertFalse(result["passed"])
        self.assertEqual([], result["allowedCorrections"])

    def test_corrected_blank_is_not_general_permission_to_erase(self):
        before = deepcopy(self.catalog)
        before["pets"]["1"]["huan"] = ["old suffix collision"]
        del self.catalog["pets"]["2"]["huan"]
        result = audit_skill_fidelity(self.root, self.catalog, previous=before)
        self.assertFalse(result["passed"])
        self.assertEqual([], result["allowedCorrections"])

    def test_traits_require_exact_tokens(self):
        self.tables["petdictionarydata.json/petdictionarydata.json"]["1"][28] = "不是寰力量"
        self.catalog["pets"]["1"]["signs"] = "不是寰力量"
        self.catalog["pets"]["1"]["traits"] = ["寰力量"]
        self.write_sources()
        self.rejected_at("/pets/1/traits")


if __name__ == "__main__":
    unittest.main()
