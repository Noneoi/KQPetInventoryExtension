import json
from copy import deepcopy
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from generate_pet_skill_supplement import (TERM, official_texts, parameterized_entry,
                                          parameter_text, build_supplement)  # noqa: E402


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
        self.assertEqual(len(expected), 1096)
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


class ExactSupplementTests(unittest.TestCase):
    def catalog(self):
        return {"source": {"version": "synthetic-version"},
                "entries": {"护盾": "ARG1护盾，ARG2回合", "官方空": ""},
                "skills": {"1": {"description": "己阵获得[护盾·30·2]与[护盾Ⅱ]。"}},
                "pets": {"1": {"name": "synthetic", "signs": "灵初", "slots": {"normal": 1}},
                         "2": {"name": "synthetic other", "signs": "不是灵初", "slots": {"normal": 1}}}}

    def test_only_official_middle_dot_parameters_are_aliases(self):
        entries = {"护盾": "值ARG", "双参数": "ARG1和ARG2", "护盾·30": ""}
        for text in ("护盾Ⅱ", "护盾II", "护盾-Ⅱ", "护盾-30", "旧护盾", "护盾加强"):
            with self.subTest(text=text):
                self.assertIsNone(parameterized_entry(text, entries))
        self.assertIsNone(parameterized_entry("护盾·30", entries), "Exact empty row must win")
        base, arguments, description = parameterized_entry("双参数·甲·乙", entries)
        self.assertEqual((base, arguments), ("双参数", ["甲", "乙"]))
        self.assertEqual(parameter_text(description, arguments), "甲和乙")
        self.assertEqual(parameter_text("值ARG", []), "值指定数值")
        self.assertEqual(parameter_text("ARG1 / ARG2 / ARG", ["甲"]), "甲 / 指定数值 / 甲")
        self.assertEqual(parameter_text("ARG1 / ARG10", ["甲"]), "甲 / 指定数值")
        self.assertEqual(parameter_text("ARG1 / ARG10", list("甲乙丙丁戊己庚辛壬癸")), "甲 / 癸")
        self.assertEqual(parameter_text("ARG1 / ARG2 / ARG", ["ARG2", "乙"]), "ARG2 / 乙 / ARG2")

    def test_local_evaluations_are_archival_and_tags_are_exact(self):
        result = build_supplement(self.catalog())
        self.assertEqual(result["defaultDisplayPolicy"], "current-official-catalog-only")
        self.assertEqual(set(result["evaluations"]), {"1"})
        self.assertEqual(result["evaluations"]["1"]["source"], "local-keyword-summary")
        self.assertFalse(result["evaluations"]["1"]["defaultVisible"])
        self.assertEqual(len(result["sourceCatalogSha256"]), 64)
        self.assertEqual(result["mechanisms"]["护盾·30·2"]["text"], "30护盾，2回合")
        self.assertEqual(result["mechanisms"]["护盾Ⅱ"]["source"], "official-skill-evidence")
        self.assertIn("[护盾Ⅱ]", result["mechanisms"]["护盾Ⅱ"]["evidence"][0])
        self.assertEqual(result["mechanisms"]["护盾Ⅱ"]["usage"], "historical-reference-only")

    def test_all_historical_records_survive_without_becoming_default_text(self):
        previous = {"schema": 1, "kind": "pet-skill-manual-supplement", "updatePolicy": "manual-static",
                    "sourceVersion": "old", "mechanisms": {"护盾Ⅱ": {"text": "legacy note"}},
                    "evaluations": {"1": {"text": "legacy evaluation"}}, "opaqueHistoricalField": {"keep": 1}}
        original = deepcopy(previous)
        first = build_supplement(self.catalog(), previous=previous)
        self.assertEqual(previous, original)
        self.assertEqual(first["historicalSnapshots"], [original])
        again = build_supplement(self.catalog(), previous=first)
        self.assertEqual(again, first, "Same source rebuild must not duplicate history")
        current = self.catalog()
        current["source"]["version"] = "new"
        changed = build_supplement(current, previous=first)
        self.assertEqual(len(changed["historicalSnapshots"]), 2)
        self.assertEqual(changed["historicalSnapshots"][0], original)
        self.assertEqual(changed["sourceVersion"], "new")
        self.assertNotEqual(changed["sourceCatalogSha256"], first["sourceCatalogSha256"])

    def test_unknown_existing_output_is_not_silently_overwritten(self):
        with self.assertRaises(ValueError):
            build_supplement(self.catalog(), previous={"unrelated": "file"})


if __name__ == "__main__":
    unittest.main()
