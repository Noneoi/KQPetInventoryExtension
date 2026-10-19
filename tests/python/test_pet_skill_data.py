import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from generate_pet_skill_supplement import TERM, official_texts  # noqa: E402
from generate_pet_skill_data import PET_FIELDS, build_catalog, huan_descriptions  # noqa: E402


class PetSkillReferenceTests(unittest.TestCase):
    def catalog(self, pets, huan, almighty=None, groups=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "petdictionarydata.json").mkdir()
            (root / "battleconfig").mkdir()
            dictionary = {}
            for race, sign, skill in pets:
                values = {"raceId": race, "orgName": "pet", "signs": sign, "yuanLiSkill": skill}
                dictionary[str(race)] = [values.get(field, "") for field in PET_FIELDS]
            (root / "petdictionarydata.json/petdictionarydata.json").write_text(json.dumps(dictionary), encoding="utf-8")
            (root / "battleconfig/skill_desc_config.json").write_text('{}', encoding="utf-8")
            (root / "battleconfig/huan_skill_config.json").write_text(json.dumps(huan), encoding="utf-8")
            (root / "battleconfig/almighty-skill-config.json").write_text(json.dumps(almighty or []), encoding="utf-8")
            (root / "pet").mkdir()
            (root / "pet/tiebaconfig.json").write_text(json.dumps(groups or []), encoding="utf-8")
            return build_catalog(root)

    def test_shared_huan_reference_keeps_skins_and_rejects_race_suffix_collisions(self):
        result = self.catalog([(3, "", 0), (3873, "启元,寰力量", 703873),
                               (3876, "启元,寰力量", 703873), (91234, "异形族", 703873)],
                              {"703873": ["寰力量|1|16000|1|战斗力≥16000时提升命中"]})
        self.assertNotIn("huan", result["pets"]["3"])
        for race in ("3873", "3876", "91234"):
            self.assertEqual(result["pets"][race]["huan"], ["寰力量：战斗力≥16000时提升命中"])
            self.assertEqual(result["pets"][race]["slots"]["yuanLi"], 0)

    def test_non_huan_yuanli_skill_keeps_the_original_slot(self):
        result = self.catalog([(3873, "启元", 703873)], {"703873": ["寰力量|1|0|0|effect"]})
        self.assertEqual(result["pets"]["3873"]["slots"]["yuanLi"], 703873)
        self.assertNotIn("huan", result["pets"]["3873"])

    def test_missing_huan_reference_does_not_invent_another_races_definition(self):
        result = self.catalog([(3873, "寰力量", 0), (3876, "寰力量", 800001)],
                              {"703873": ["寰力量|1|0|0|unrelated"]})
        self.assertNotIn("huan", result["pets"]["3873"])
        self.assertNotIn("huan", result["pets"]["3876"])

    def test_huan_packed_fields_are_decoded_and_thresholds_ordered(self):
        self.assertEqual(huan_descriptions({"99": ["寰力量|1|20000|3|later",
                                                   "寰力量|1|0|0|first|literal"]}, 99),
                         ["寰力量：first|literal", "寰力量：later"])

    def test_changed_huan_schema_fails_instead_of_showing_encoded_garbage(self):
        for rows in ({"99": "invalid"}, {"99": ["missing|fields"]},
                     {"99": ["name|1|unknown|0|description"]}):
            with self.assertRaises(ValueError):
                huan_descriptions(rows, 99)

    def test_almighty_uses_complete_race_id_without_merging_related_forms(self):
        result = self.catalog([(4006, "天觉者", 0), (4009, "天觉者", 0), (124006, "天觉者", 0)], {},
                              almighty=[{"id": "4006", "hd": "base", "psci": "4007"},
                                        {"id": "4009", "hd": "skin", "psci": "4010"},
                                        {"id": "124006", "hd": "long-form", "psci": "124007"}],
                              groups=[{"raceIds": [4006, 4009]}])
        for race, description, target in (("4006", "base", 4007), ("4009", "skin", 4010),
                                           ("124006", "long-form", 124007)):
            rows = result["pets"][race]["almighty"]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["id"], int(race))
            self.assertEqual(rows[0]["heroDescription"], description)
            self.assertEqual(rows[0]["calledRaceId"], target)
        self.assertEqual(result["pets"]["4006"]["forms"], [4006, 4009])

    def test_almighty_requires_the_official_trait_and_its_own_definition(self):
        result = self.catalog([(4006, "", 0), (4009, "天觉者", 0)], {},
                              almighty=[{"id": "4006", "hd": "unrelated"}],
                              groups=[{"raceIds": [4006, 4009]}])
        self.assertNotIn("almighty", result["pets"]["4006"])
        self.assertNotIn("almighty", result["pets"]["4009"])


class PetSkillDataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = json.loads((ROOT / "assets/pet-skill-data.json").read_text(encoding="utf-8"))
        cls.supplement = json.loads((ROOT / "assets/pet-skill-supplement.json").read_text(encoding="utf-8"))
        cls.corpus = official_texts(cls.catalog)

    def test_official_catalog_and_manual_supplement_are_separate(self):
        self.assertEqual(self.catalog["schema"], 1)
        self.assertGreaterEqual(len(self.catalog["pets"]), 9000)
        self.assertGreaterEqual(len(self.catalog["skills"]), 20000)
        self.assertGreaterEqual(len(self.catalog["entries"]), 100)
        self.assertNotIn("mechanisms", self.catalog)
        self.assertNotIn("mechanismCoverage", self.catalog)
        self.assertNotIn("mechanismRuleVersion", self.catalog)

        self.assertEqual(self.supplement["schema"], 1)
        self.assertEqual(self.supplement["kind"], "pet-skill-manual-supplement")
        self.assertEqual(self.supplement["updatePolicy"], "manual-static")
        self.assertEqual(self.supplement["sourceVersion"], self.catalog["source"]["version"])

    def test_lingchu_examples_keep_all_skill_slots_and_terms(self):
        extreme = self.catalog["pets"]["7396"]
        thunder = self.catalog["pets"]["6984"]
        self.assertEqual(extreme["slots"]["normal"], 107396)
        self.assertEqual(extreme["slots"]["ultimate"], 207396)
        self.assertEqual(extreme["slots"]["fate"], 927396)
        self.assertEqual(extreme["slots"]["transform"], 507396)
        self.assertEqual(thunder["slots"]["normal"], 106984)
        self.assertEqual(thunder["slots"]["ultimate"], 206984)
        self.assertEqual(thunder["slots"]["fate"], 926984)
        self.assertIn("灵力枷锁", self.catalog["entries"])
        self.assertIn("战神领域", self.catalog["entries"])
        self.assertIn("507396", self.catalog["transformSkills"])

    def test_every_current_term_is_official_or_has_traceable_supplement(self):
        entries = self.catalog["entries"]
        mechanisms = self.supplement["mechanisms"]
        references = {match.group(1).strip() for text in self.corpus for match in TERM.finditer(text)
                      if match.group(1).strip()}
        self.assertEqual(references - set(entries), set(mechanisms))
        self.assertEqual(self.supplement["mechanismCoverage"], {
            "referenced": len(references),
            "officialDefinitions": len(references & set(entries)),
            "supplemented": len(mechanisms),
            "uncovered": 0,
        })
        self.assertIn("龙尊权能", mechanisms)
        self.assertIn("龙尊觉醒", mechanisms["龙尊权能"]["text"])
        self.assertIn("崩甲", mechanisms)
        self.assertIn("降低超物防、超魔防10%", mechanisms["崩甲"]["text"])
        self.assertIn("受伤降低·30%", mechanisms)
        self.assertIn("30%", mechanisms["受伤降低·30%"]["text"])

        allowed = {"parameterized-official-entry", "official-buff", "related-official-entry",
                   "official-skill-definition", "official-skill-evidence"}
        for term, value in mechanisms.items():
            self.assertIn(value.get("source"), allowed, term)
            self.assertTrue(value.get("text", "").strip(), term)
            self.assertTrue(value.get("evidence"), term)
            for evidence in value["evidence"]:
                self.assertTrue(any(evidence in official for official in self.corpus),
                                f"{term}: evidence is not in official text: {evidence}")

    def test_evaluations_cover_only_current_lingchu_pets_and_cite_their_skills(self):
        expected = {race_id for race_id, pet in self.catalog["pets"].items()
                    if "灵初" in pet.get("signs", "")}
        evaluations = self.supplement["evaluations"]
        self.assertGreater(len(expected), 0)
        self.assertEqual(set(evaluations), expected)
        for race_id, evaluation in evaluations.items():
            pet = self.catalog["pets"][race_id]
            skill_ids = sorted({value for value in pet["slots"].values()
                                if isinstance(value, int) and value > 0})
            self.assertEqual(evaluation["skillIds"], skill_ids, race_id)
            self.assertEqual(evaluation["petName"], pet["name"], race_id)
            self.assertTrue(evaluation["text"].startswith("技能文字显示"), race_id)
            self.assertIn("以本页技能原文为准", evaluation["text"], race_id)
            self.assertLessEqual(len(evaluation["text"]), 90, race_id)


if __name__ == "__main__":
    unittest.main()
