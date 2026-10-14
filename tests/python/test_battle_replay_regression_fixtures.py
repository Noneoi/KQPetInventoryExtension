from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("fixture_builder", ROOT / "tools/battle_replay/build_regression_fixtures.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)
FIXTURES = ROOT / "assets/battle-replay-regression"


class RegressionFixtures(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((FIXTURES / "manifest.json").read_bytes())

    def test_reviewed_real_inputs_are_bound_and_expectations_are_frozen(self):
        self.assertEqual(self.manifest["projectionSchema"], "qtcore-domain-v1")
        self.assertEqual(len(self.manifest["cases"]), 3)
        for case in self.manifest["cases"]:
            data = (FIXTURES / case["file"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), case["sha256"])
            self.assertTrue(case["source"]["cleaningProjectionEquivalent"])
            self.assertTrue(case["source"]["name"].endswith(".txt"))
            self.assertEqual(case["expected"]["effectsSha256"], case["source"]["verifiedSanitizedEffectsSha256"])
            self.assertGreater(case["expected"]["actions"], 0)

    def test_published_reports_exclude_account_names_ids_and_custom_nicknames(self):
        self.assertEqual(self.manifest["privacySchema"], 2)
        self.assertFalse(self.manifest.get("privacyRebaselinePending", False))
        for case in self.manifest["cases"]:
            doc = json.loads((FIXTURES / case["file"]).read_bytes())
            self.assertEqual(set(doc), {"br"})
            self.assertNotIn("bi", doc["br"])
            self.assertNotIn("ir", doc["br"])
            for team in doc["br"]["or"]["team"]:
                for index, player in enumerate(team["players"]):
                    self.assertEqual(player["name"], f"side{team['id']}-player{index}")
                    self.assertTrue({"id", "nn", "items"}.isdisjoint(player))
                    for fighter in player["cts"]:
                        self.assertNotIn("customName", fighter)
                        # Only PLAYER ids are removed. Combat instance IDs stay.
                        self.assertIn("id", fighter)
                        self.assertIn("raceId", fighter)

            def check_references(value):
                if isinstance(value, dict):
                    self.assertNotIn("customName", value)
                    for number_key, name_key in BUILDER.NUMERIC_PLAYER_REFERENCE_FIELDS.items():
                        if number_key not in value or name_key not in value:
                            continue
                        number = BUILDER.player_number(value[number_key], "/test/" + number_key)
                        if number > 0:
                            self.assertEqual(number, BUILDER.anonymous_player_number(value[name_key]))
                    for item in value.values():
                        check_references(item)
                elif isinstance(value, list):
                    for item in value:
                        check_references(item)
            check_references(doc)
            migration = case.get("privacyMigration", {})
            if migration:
                self.assertFalse(migration["nativeRefreezePending"])
                self.assertTrue(migration["unitIdentitiesPreserved"])
                self.assertTrue(migration["nonIdentityValuesAndOrderPreserved"])
                self.assertEqual(migration["fixtureSha256"], case["sha256"])
                self.assertTrue(all(set(change) == {"path", "operation"} for change in migration["changes"]))

    def test_sanitizer_is_narrow_and_does_not_mutate_original(self):
        fixture = json.loads((FIXTURES / self.manifest["cases"][0]["file"]).read_bytes())
        # Reapplying cannot silently invent or coerce combat values.
        original = copy.deepcopy(fixture)
        clean, edits, _aliases = BUILDER.sanitize(fixture)
        self.assertEqual(fixture, original)
        self.assertEqual(clean, original)
        self.assertTrue(all(edit["operation"] in ("pseudonymize-player-reference",
            "pseudonymize-numeric-player-reference") for edit in edits))

    def test_numeric_reference_anonymity_is_contextual_and_join_preserving(self):
        # Deliberately synthetic identities. The identical value in damage,
        # unit id and unpaired pi must remain untouched.
        original = {"br": {"or": {"team": [{"id": 0, "players": [{"id": 987654321,
            "name": "synthetic-player", "nn": "synthetic-nickname",
            "cts": [{"id": 987654321, "raceId": 77, "customName": "synthetic-pet-name"}]}]}]},
            "fr": [{"cpi": 987654321, "cpn": "synthetic-player", "damage": 987654321,
                "trl": [{"tpi": "987654321", "tpn": "synthetic-player",
                    "tn": {"pi": 987654321, "rl": [{"pi": 987654321, "pn": "synthetic-player"}]}}]}]}}
        before = copy.deepcopy(original)
        clean, _edits, _aliases = BUILDER.sanitize(original)
        self.assertEqual(original, before)
        action = clean["br"]["fr"][0]
        self.assertEqual((action["cpn"], action["cpi"]), ("side0-player0", 1000))
        self.assertEqual(action["trl"][0]["tpi"], "1000")
        self.assertEqual(action["trl"][0]["tn"]["rl"][0]["pi"], 1000)
        self.assertEqual(action["damage"], 987654321)
        self.assertEqual(action["trl"][0]["tn"]["pi"], 987654321)
        self.assertEqual(clean["br"]["or"]["team"][0]["players"][0]["cts"][0]["id"], 987654321)
        self.assertEqual(BUILDER.sanitize(clean)[0], clean)
        wrong = copy.deepcopy(original); wrong["br"]["fr"][0]["cpi"] += 1
        with self.assertRaisesRegex(ValueError, "name/number reference disagrees"):
            BUILDER.sanitize(wrong)
        wrong = copy.deepcopy(original); wrong["br"]["fr"][0].pop("cpn")
        with self.assertRaisesRegex(ValueError, "Unrecognized numeric player reference"):
            BUILDER.sanitize(wrong)

    def test_numeric_identity_equivalence_rejects_any_battle_value_change(self):
        before = {"row": {"cpn": "side0-player0", "cpi": 987654321, "hp": 987654321,
                          "id": 987654321, "pn": "side1-player0", "pi": -1}}
        after = copy.deepcopy(before); after["row"]["cpi"] = 1000
        self.assertEqual(BUILDER.numeric_identity_changes(before, after), [
            {"path": "/row/cpi", "operation": "pseudonymize-numeric-player-reference"}])
        for field in ("hp", "id", "pi"):
            changed = copy.deepcopy(after); changed["row"][field] += 1
            with self.assertRaises(ValueError):
                BUILDER.numeric_identity_changes(before, changed)
        changed = copy.deepcopy(after); changed["row"]["cpi"] = "1000"
        with self.assertRaises(ValueError):
            BUILDER.numeric_identity_changes(before, changed)

    def test_privacy_refreeze_cannot_hide_changed_battle_counts(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            manifest = copy.deepcopy(self.manifest)
            manifest["cases"] = manifest["cases"][:1]
            case = manifest["cases"][0]
            data = (FIXTURES / case["file"]).read_bytes()
            (directory / case["file"]).write_bytes(data)
            case["privacyMigration"] = {"nativeRefreezePending": True,
                "fixtureSha256": BUILDER.sha(data),
                "normalizedStructureSha256": BUILDER.sha(BUILDER.encode(json.loads(data))),
                "priorExpectedSha256": BUILDER.sha(BUILDER.encode(case["expected"])),
                "normalizedLegacyEffectsSha256": case["expected"]["effectsSha256"]}
            manifest_bytes = BUILDER.encode(manifest)
            (directory / "manifest.json").write_bytes(manifest_bytes)
            observed = copy.deepcopy(case["expected"]); observed["actions"] += 1
            report = {"mode": "baseline-observation", "passed": False, "observationComplete": True,
                "errors": [], "catalogSha256": manifest["baseline"]["catalogJsonSha256"],
                "fixtureSetSha256": BUILDER.sha(manifest_bytes), "cases": [{"id": case["id"],
                    "passed": True, "fixtureSha256": case["sha256"], "observed": observed}]}
            report_path = directory / "observation.json"; report_path.write_bytes(BUILDER.encode(report))
            with self.assertRaisesRegex(ValueError, "non-identity battle state"):
                BUILDER.freeze(SimpleNamespace(output=directory, freeze_observation=report_path))
            self.assertEqual((directory / "manifest.json").read_bytes(), manifest_bytes)

    def test_coverage_claims_are_actual_nonzero_native_observations(self):
        combined = {}
        for case in self.manifest["cases"]:
            for kind, count in case["expected"]["coverage"].items():
                combined[kind] = combined.get(kind, 0) + count
        for key in ("normalActions", "multiTargetActions", "execution200195", "restoreDisplay30098", "appearances",
                    "transforms", "hpDecreases", "hpIncreases", "shieldDecreases", "shieldIncreases",
                    "powerDecreases", "powerIncreases", "zeroHpEvents", "hpRestoredEvents"):
            self.assertGreater(combined.get(key, 0), 0, key)

    def test_candidate_cannot_be_used_to_freeze_its_own_expectations(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "manifest.json").write_bytes(BUILDER.encode(self.manifest))
            observation = {"mode": "baseline-observation", "passed": False, "observationComplete": True,
                           "errors": [], "catalogSha256": "0" * 64}
            output = root / "observation.json"
            output.write_bytes(BUILDER.encode(observation))
            before = (root / "manifest.json").read_bytes()
            with self.assertRaisesRegex(ValueError, "candidate may never redefine"):
                BUILDER.freeze(SimpleNamespace(output=root, freeze_observation=output))
            self.assertEqual((root / "manifest.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
