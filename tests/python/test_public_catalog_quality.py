"""Quality-gate regressions using synthetic catalogs, never live game evidence."""
from __future__ import annotations

from collections import Counter
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from public_catalog_quality import CatalogQualityError, audit_catalog_quality, enforce_catalog_quality
from test_battle_replay_catalog_pack import synthetic_catalog as package_catalog


def update_counts(catalog):
    """Keep unrelated package counters valid when an individual case changes rows."""
    catalog["tableJoinSummary"]["skills"]["skills"] = len(catalog["skills"])
    catalog["tableJoinSummary"]["buffs"]["canonicalBuffIds"] = len(catalog["buffs"])
    catalog["tableJoinSummary"]["buffs"]["flashBufferExecuteRows"] = len(catalog["bufferExecution"])
    catalog["tableJoinSummary"]["buffs"]["flashComboRows"] = len(catalog["combos"])
    catalog["petMechanicsSummary"]["pets"] = len(catalog["pets"])
    return catalog


def add_skill(catalog, identifier, name="合成技能", description="合成技能说明"):
    row = deepcopy(catalog["skills"]["2"])
    row.update(id=identifier, name=name, description=description)
    catalog["skills"][identifier] = row


def add_buff(catalog, identifier, name="合成效果", description="合成效果说明"):
    row = deepcopy(catalog["buffs"]["3"])
    row.update(id=identifier, did="645", nm=name, mess=description,
               nameStatus="named" if name else "unnamed")
    catalog["buffs"][identifier] = row


def add_relation(catalog, relation_id, skill_id, buff_id):
    # The relationships and reverse indexes use the real catalog schema. Their
    # IDs/text/source paths here are deliberately synthetic fixtures.
    fields = dict(bufferId=buff_id, actionResId="0", skillId=skill_id,
                  lightedSkillId="0", desc="合成执行引用")
    catalog["bufferExecution"][buff_id] = deepcopy(fields)
    catalog["tableRelations"].append({
        "id": relation_id, "kind": "buffer_execute_skill", "sourceTable": "BufferExecuteXML",
        "sourceFile": "SYNTHETIC/BufferExecuteXML.xml", "sourceRow": len(catalog["tableRelations"]),
        "sourcePath": "/synthetic/s[1]", "sourceFields": fields,
        "skillId": skill_id, "buffId": buff_id, "skillField": "skillId",
    })
    catalog["relationsBySkill"].setdefault(skill_id, []).append(relation_id)
    catalog["relationsByBuff"].setdefault(buff_id, []).append(relation_id)


def battle_catalog():
    catalog = package_catalog()
    add_skill(catalog, "10", "合成技能甲", "合成说明甲")
    add_skill(catalog, "11", "合成技能乙", "合成说明乙")
    add_buff(catalog, "20", "合成效果甲", "合成效果说明甲")
    add_buff(catalog, "21", "合成效果乙", "合成效果说明乙")
    add_relation(catalog, "synthetic-relation-a", "10", "20")
    catalog["petMechanics"] = {
        "1": {"raceId": "1", "name": "test pet", "skillLinks": [
            {"field": "norSkillNo", "label": "普通技能", "skillId": "10", "resolved": True}],
            "otherReferences": {}, "legendReferenceNamespace": "none",
            "mechanismFamily": "", "qiYunType": "0"},
    }
    catalog["legendMechanics"] = {"definitions": {}}
    catalog["mechanismRules"] = {"qiYun": {"plans": {}}, "suLing": {"plans": {}}}
    return update_counts(catalog)


class PublicCatalogQualityBattleReplayTests(unittest.TestCase):
    def audit(self, candidate, previous=None, *, component="battleReplay"):
        original_candidate, original_previous = deepcopy(candidate), deepcopy(previous)
        result = audit_catalog_quality(candidate, previous, component=component)
        self.assertEqual(candidate, original_candidate, "audit must not repair/mutate the candidate")
        self.assertEqual(previous, original_previous, "audit must not mutate the trusted baseline")
        self.assertTrue({"schemaVersion", "component", "summary", "passed", "tables", "changes", "findings"} <= result.keys())
        self.assertEqual(result["component"], component)
        self.assertIsInstance(result["passed"], bool)
        self.assertIsInstance(result["tables"], list)
        self.assertIsInstance(result["changes"], list)
        for table in result["tables"]:
            self.assertTrue({"path", "present", "rows", "fields"} <= table.keys())
            self.assertTrue(table["path"].startswith("/"))
            self.assertIsInstance(table["present"], bool)
        for change in result["changes"]:
            self.assertTrue({"kind", "path"} <= change.keys())
            self.assertTrue(change["path"].startswith("/"))
        self.assertIsInstance(result["findings"], list)
        for finding in result["findings"]:
            self.assertTrue({"kind", "path", "severity", "baselineStatus", "message"} <= finding.keys())
            self.assertIn(finding["severity"], {"error", "review", "info"})
            self.assertIn(finding["baselineStatus"], {"existing", "new", "resolved"})
            self.assertIsInstance(finding["message"], str)
        self.assertEqual(result["passed"], not any(
            finding["severity"] in {"error", "review"} for finding in result["findings"]))
        return result

    def assertFinding(self, result, path, *, severities=("review", "error"), status="new"):
        matches = [finding for finding in result["findings"]
                   if finding["path"].startswith(path) and finding["severity"] in severities
                   and (status is None or finding["baselineStatus"] == status)]
        self.assertTrue(matches, f"missing finding at {path}: {result['findings']!r}")
        return matches[0]

    def assertChange(self, result, kind, path):
        self.assertTrue(any(item["kind"] == kind and item["path"].startswith(path)
                            for item in result["changes"]),
                        f"missing {kind} at {path}: {result['changes']!r}")

    def test_valid_first_snapshot_and_identical_baseline_allow_official_empty_text(self):
        candidate = battle_catalog()
        # The underlying fixture has a name/description-empty skill and a
        # name/mess-empty Buff. Their presence is legal official-table shape.
        self.assertTrue(self.audit(candidate)["passed"])
        self.assertTrue(self.audit(candidate, deepcopy(candidate))["passed"])

    def test_existing_unresolved_identity_is_not_a_global_failure(self):
        previous = battle_catalog()
        previous["pets"]["1"]["identity"].update(status="unresolved", canonicalName="",
                                                   reason="missingExactSkinMapping")
        result = self.audit(deepcopy(previous), previous)
        self.assertTrue(result["passed"])
        existing = self.assertFinding(result, "/pets/1", severities=("info",), status="existing")
        self.assertNotEqual(existing["kind"], "missing-reference")

    def test_linked_addition_and_legal_empty_new_rows_pass_and_record_changes(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        add_skill(candidate, "30", "", "")
        add_buff(candidate, "40", "", "")
        add_relation(candidate, "synthetic-relation-b", "30", "40")
        candidate["petMechanics"]["1"]["skillLinks"].append({"field": "heroSkillNo", "skillId": "30", "resolved": True})
        update_counts(candidate)
        result = self.audit(candidate, previous)
        self.assertTrue(result["passed"])
        self.assertChange(result, "row-added", "/skills/30")
        self.assertChange(result, "row-added", "/buffs/40")

    def test_nonempty_text_update_passes_but_is_not_silent(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["skills"]["10"]["description"] = "合成说明的第二版"
        result = self.audit(candidate, previous)
        self.assertTrue(result["passed"])
        self.assertChange(result, "row-changed", "/skills/10")

    def test_clearing_existing_names_or_explanations_requires_review(self):
        for table, identifier, field in (("skills", "10", "name"), ("skills", "10", "description"),
                                         ("buffs", "20", "nm"), ("buffs", "20", "mess")):
            for value in ("", " \t\n"):
                with self.subTest(table=table, field=field, value=value):
                    previous = battle_catalog(); candidate = deepcopy(previous)
                    candidate[table][identifier][field] = value
                    result = self.audit(candidate, previous)
                    self.assertFalse(result["passed"])
                    self.assertFinding(result, f"/{table}/{identifier}/{field}")
                    self.assertChange(result, "text-cleared", f"/{table}/{identifier}/{field}")

    def test_confirmed_native_or_skin_identity_cannot_silently_become_unresolved(self):
        for status in ("native", "skin"):
            with self.subTest(status=status):
                previous = battle_catalog()
                previous["pets"]["1"]["identity"]["status"] = status
                candidate = deepcopy(previous)
                candidate["pets"]["1"]["identity"].update(status="unresolved", canonicalName="")
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertFinding(result, "/pets/1/identity")
                self.assertChange(result, "identity-regressed", "/pets/1")

    def test_loss_of_existing_fields_and_type_changes_require_review(self):
        mutations = (
            ("/buffs/20/once", lambda c: c["buffs"]["20"].pop("once")),
            ("/skills/10/playbackAvailable", lambda c: c["skills"]["10"].__setitem__("playbackAvailable", "false")),
            ("/skills/10/playbackAvailable", lambda c: c["skills"]["10"].__setitem__("playbackAvailable", 0)),
            ("/pets/1/maxPower", lambda c: c["pets"]["1"].__setitem__("maxPower", "100")),
            ("/petMechanics/1/skillLinks", lambda c: c["petMechanics"]["1"].__setitem__("skillLinks", {})),
        )
        for path, mutate in mutations:
            with self.subTest(path=path):
                previous = battle_catalog(); candidate = deepcopy(previous); mutate(candidate)
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertFinding(result, path)

    def test_deleted_unreferenced_row_is_not_ignored(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        del candidate["skills"]["11"]; update_counts(candidate)
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/skills/11")
        self.assertChange(result, "row-removed", "/skills/11")

    def test_same_row_count_does_not_hide_an_identifier_replacement(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        replacement = candidate["buffs"].pop("21"); replacement["id"] = "22"
        candidate["buffs"]["22"] = replacement
        self.assertEqual(len(candidate["buffs"]), len(previous["buffs"]))
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/buffs/21")
        self.assertChange(result, "row-added", "/buffs/22")
        self.assertChange(result, "row-removed", "/buffs/21")

    def test_relation_requires_buff_even_when_same_number_exists_as_skill(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["tableRelations"][0]["buffId"] = "11"
        candidate["tableRelations"][0]["sourceFields"]["bufferId"] = "11"
        candidate["relationsByBuff"] = {"11": ["synthetic-relation-a"]}
        # Skill 11 exists; Buff 11 intentionally does not.
        self.assertIn("11", candidate["skills"]); self.assertNotIn("11", candidate["buffs"])
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/tableRelations/", severities=("error", "review"))

    def test_same_number_in_distinct_present_namespaces_is_legal(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        add_buff(candidate, "10")
        add_relation(candidate, "synthetic-same-number", "10", "10")
        update_counts(candidate)
        self.assertTrue(self.audit(candidate, previous)["passed"])

    def test_reverse_indexes_cannot_have_wrong_owner_ghost_or_missing_relation(self):
        cases = (
            ("/relationsBySkill", lambda c: c["relationsBySkill"].__setitem__("11", ["synthetic-relation-a"])),
            ("/relationsByBuff", lambda c: c["relationsByBuff"].__setitem__("21", ["synthetic-relation-a"])),
            ("/relationsBySkill", lambda c: c["relationsBySkill"]["10"].append("synthetic-missing-relation")),
            ("/relationsByBuff", lambda c: c["relationsByBuff"]["20"].clear()),
            ("/relationsBySkill", lambda c: c["relationsBySkill"]["10"].append("synthetic-relation-a")),
        )
        for path, mutate in cases:
            with self.subTest(path=path, mutate=repr(mutate)):
                previous = battle_catalog(); candidate = deepcopy(previous); mutate(candidate)
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertFinding(result, path, severities=("error",))

    def test_duplicate_relation_ids_and_row_id_key_mismatches_are_errors(self):
        cases = (
            ("/tableRelations", lambda c: c["tableRelations"].append(deepcopy(c["tableRelations"][0]))),
            ("/skills/10", lambda c: c["skills"]["10"].__setitem__("id", "11")),
            ("/buffs/20", lambda c: c["buffs"]["20"].__setitem__("id", "10")),
        )
        for path, mutate in cases:
            with self.subTest(path=path):
                previous = battle_catalog(); candidate = deepcopy(previous); mutate(candidate)
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertFinding(result, path, severities=("error",))

    def test_forged_coverage_and_resolved_cache_cannot_hide_actual_pet_skill_break(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["petMechanics"]["1"]["skillLinks"][0].update(skillId="999", resolved=True)
        candidate["referenceCoverage"] = {"schemaVersion": 1, "summary": {"byStatus": {"missing-reference": 0}}, "items": []}
        add_buff(candidate, "999", "同号合成 Buff", "不能代替技能")
        update_counts(candidate)
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/petMechanics/1/skillLinks/0", severities=("review", "error"))

    def test_existing_missing_reference_is_visible_but_does_not_block_unchanged_snapshot(self):
        previous = battle_catalog()
        previous["petMechanics"]["1"]["skillLinks"][0].update(skillId="999", resolved=True)
        result = self.audit(deepcopy(previous), previous)
        self.assertTrue(result["passed"])
        finding = self.assertFinding(result, "/petMechanics/1/skillLinks/0", severities=("info",), status="existing")
        self.assertEqual(finding["kind"], "missing-reference")

    def test_present_source_fields_must_agree_with_the_relationship(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["tableRelations"][0]["sourceFields"]["skillId"] = "11"
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/tableRelations/", severities=("error",))

    def test_present_source_fields_cannot_bypass_validation_with_an_invalid_type(self):
        for value in ([], None, "synthetic-invalid-source"):
            with self.subTest(value=value):
                previous = battle_catalog(); candidate = deepcopy(previous)
                candidate["tableRelations"][0]["sourceFields"] = value
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertFinding(result, "/tableRelations/0", severities=("error",))

    def test_invalid_relation_fields_return_findings_instead_of_crashing(self):
        # The report itself must remain usable when a candidate is corrupt;
        # invalid unhashable values cannot reach reverse-index or edge counters.
        for field in ("skillId", "buffId", "kind", "priorityIndex"):
            for value in ([], {}):
                with self.subTest(field=field, value=value):
                    previous = battle_catalog(); candidate = deepcopy(previous)
                    candidate["tableRelations"][0][field] = value
                    result = self.audit(candidate, previous)
                    self.assertFalse(result["passed"])
                    self.assertFinding(result, "/tableRelations/0", severities=("error",))

    def test_unverified_relation_kind_is_not_accepted_as_a_numeric_join(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["tableRelations"][0]["kind"] = "synthetic-unverified-namespace"
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/tableRelations/", severities=("error",))

    def test_deleting_generated_graph_and_both_indexes_cannot_hide_source_references(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["tableRelations"] = []
        candidate["relationsBySkill"] = {}
        candidate["relationsByBuff"] = {}
        self.assertEqual(candidate["bufferExecution"]["20"]["skillId"], "10")
        self.assertEqual(candidate["bufferExecution"]["20"]["bufferId"], "20")
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"], "the official source edge still exists even if all generated indexes disappear")
        self.assertTrue(any(finding["severity"] in {"review", "error"} and
                            finding["path"].startswith(("/tableRelations", "/bufferExecution/20"))
                            for finding in result["findings"]), result["findings"])

    def test_fixed_reference_is_recorded_as_resolved_without_blocking(self):
        previous = battle_catalog()
        previous["petMechanics"]["1"]["skillLinks"][0].update(skillId="999", resolved=True)
        candidate = deepcopy(previous)
        add_skill(candidate, "999", "补齐的合成技能", "合成说明")
        update_counts(candidate)
        result = self.audit(candidate, previous)
        self.assertTrue(result["passed"])
        self.assertFinding(result, "/petMechanics/1/skillLinks/0", severities=("info",), status="resolved")

    def test_explicit_legend_huan_and_plan_namespaces_are_independent(self):
        for field, target in (("legendSkillNo", "900001"), ("huanSkillNo", "704832")):
            with self.subTest(field=field):
                previous = battle_catalog(); candidate = deepcopy(previous)
                pet = candidate["petMechanics"]["1"]
                pet["otherReferences"][field] = target
                if field == "legendSkillNo": pet["legendReferenceNamespace"] = "legacyLegend"
                add_skill(candidate, target); add_buff(candidate, target); update_counts(candidate)
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertFinding(result, f"/petMechanics/1/otherReferences/{field}")
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["mechanismRules"]["qiYun"]["plans"]["1"] = {"id": 1, "name": "合成气运方案"}
        candidate["petMechanics"]["1"].update(mechanismFamily="suLing", qiYunType="1")
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/petMechanics/1/qiYunType")

    def test_workbench_skill_slots_use_the_declared_skill_namespace(self):
        previous = {
            "schema": 1,
            "pets": {"1": {"name": "合成精灵", "slots": {"normal": 10, "super": 11}}},
            "skills": {"10": {"name": "合成普通技", "description": "合成说明"},
                       "11": {"name": "合成超杀技", "description": "合成说明"}},
            "buffs": {"999": {"name": "同号合成效果"}},
        }
        self.assertTrue(self.audit(deepcopy(previous), previous, component="skills")["passed"])
        candidate = deepcopy(previous)
        candidate["pets"]["1"]["slots"]["normal"] = 999
        result = self.audit(candidate, previous, component="skills")
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/pets/1/slots/normal", severities=("review",))
        # A previously retained official gap must remain visible but must not
        # be reintroduced as a new failure just because the report was rebuilt.
        result = self.audit(deepcopy(candidate), candidate, component="skills")
        self.assertTrue(result["passed"])
        self.assertFinding(result, "/pets/1/slots/normal", severities=("info",), status="existing")
        corrected = deepcopy(candidate)
        corrected["skills"]["999"] = {"name": "", "description": ""}
        result = self.audit(corrected, candidate, component="skills")
        self.assertTrue(result["passed"])
        self.assertFinding(result, "/pets/1/slots/normal", severities=("info",), status="resolved")

    def test_workbench_skill_slots_still_detect_a_new_owner_of_an_old_gap(self):
        previous = {"pets": {"1": {"name": "合成旧精灵", "slots": {"normal": 999}}}, "skills": {}}
        candidate = deepcopy(previous)
        candidate["pets"]["2"] = {"name": "合成新精灵", "slots": {"normal": 999}}
        result = self.audit(candidate, previous, component="skills")
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/pets/1/slots/normal", severities=("info",), status="existing")
        self.assertFinding(result, "/pets/2/slots/normal", severities=("review",), status="new")

    def test_new_workbench_slot_shape_cannot_be_silently_treated_as_empty(self):
        previous = {"pets": {"1": {"name": "合成旧精灵", "slots": {"normal": 10}}},
                    "skills": {"10": {"name": "合成技能"}}}
        for value in ("10", None, False, ["10"]):
            with self.subTest(value=value):
                candidate = deepcopy(previous)
                candidate["pets"]["2"] = {"name": "合成新精灵", "slots": value}
                result = self.audit(candidate, previous, component="skills")
                self.assertFalse(result["passed"])
                self.assertFinding(result, "/pets/2/slots", severities=("error",))

    def test_workbench_pets_accept_zero_self_and_terminating_group_chains(self):
        # Older schemas may omit unrelated sheets and quality metadata.
        previous = {"pets": {
            "1": {"name": "合成无分组", "groupRaceId": 0},
            "2": {"name": "合成分组本体", "groupRaceId": 2},
            "3": {"name": "合成分组形态", "groupRaceId": 1},
        }}
        self.assertTrue(self.audit(deepcopy(previous), previous, component="pets")["passed"])
        candidate = deepcopy(previous)
        candidate["pets"]["1"]["groupRaceId"] = 2
        self.assertTrue(self.audit(candidate, previous, component="pets")["passed"])
        self.assertTrue(self.audit({"pets": {"1": {"name": "旧版最小条目"}}}, component="pets")["passed"])

    def test_workbench_pet_missing_group_is_new_review_then_existing_information(self):
        previous = {"pets": {"1": {"name": "合成精灵", "groupRaceId": 0}}}
        candidate = deepcopy(previous); candidate["pets"]["1"]["groupRaceId"] = 999
        result = self.audit(candidate, previous, component="pets")
        self.assertFalse(result["passed"])
        self.assertFinding(result, "/pets/1/groupRaceId", severities=("review",))
        result = self.audit(deepcopy(candidate), candidate, component="pets")
        self.assertTrue(result["passed"])
        self.assertFinding(result, "/pets/1/groupRaceId", severities=("info",), status="existing")

    def test_new_workbench_group_reference_must_be_a_valid_identifier(self):
        previous = {"pets": {"1": {"name": "合成旧精灵", "groupRaceId": 0}}}
        for value in ([], {}, True, "synthetic-invalid-group"):
            with self.subTest(value=value):
                candidate = deepcopy(previous)
                candidate["pets"]["2"] = {"name": "合成新精灵", "groupRaceId": value}
                result = self.audit(candidate, previous, component="pets")
                self.assertFalse(result["passed"])
                self.assertFinding(result, "/pets/2/groupRaceId", severities=("error",))

    def test_workbench_pet_group_cycles_cannot_pass_as_resolved_references(self):
        previous = {"pets": {
            "1": {"name": "合成精灵甲", "groupRaceId": 0},
            "2": {"name": "合成精灵乙", "groupRaceId": 2},
            "3": {"name": "合成精灵丙", "groupRaceId": 1},
        }}
        candidate = deepcopy(previous)
        candidate["pets"]["1"]["groupRaceId"] = 3
        result = self.audit(candidate, previous, component="pets")
        self.assertFalse(result["passed"])
        self.assertTrue(any(finding["path"] in ("/pets/1/groupRaceId", "/pets/3/groupRaceId") and
                            finding["severity"] in {"review", "error"} for finding in result["findings"]),
                        result["findings"])

    @staticmethod
    def cultivation_catalog():
        # Same output structure as parse_sacred_plans, with deliberately
        # synthetic values. Empty cost and zero equipmentCount are legal.
        plan = {"maxLevel": 2, "levels": {
            "1": {"battlePower": 10, "cost": [{"type": 4, "id": 700, "count": 2}], "equipmentCount": 1},
            "2": {"battlePower": 20, "cost": [], "equipmentCount": 0},
        }}
        return {"pets": {"1": {"name": "合成精灵"}},
                "sacredStarPlans": {"10": deepcopy(plan)},
                "sacredStagePlans": {"20": deepcopy(plan)},
                "fusionJobs": [{"name": "合成组合职业", "jobs": [[1, 2], [3, 4]]}]}

    def test_sacred_plan_same_count_identifier_replacement_requires_review(self):
        for table, identifier in (("sacredStarPlans", "10"), ("sacredStagePlans", "20")):
            with self.subTest(table=table):
                previous = self.cultivation_catalog(); candidate = deepcopy(previous)
                candidate[table]["99"] = candidate[table].pop(identifier)
                self.assertEqual(len(candidate[table]), len(previous[table]))
                result = self.audit(candidate, previous, component="pets")
                self.assertFalse(result["passed"])
                self.assertChange(result, "row-removed", f"/{table}/{identifier}")
                self.assertChange(result, "row-added", f"/{table}/99")

    def test_sacred_plan_cost_level_and_fields_cannot_silently_disappear(self):
        for table, identifier in (("sacredStarPlans", "10"), ("sacredStagePlans", "20")):
            for field in ("maxLevel", "levels", "battlePower", "cost", "equipmentCount", "cost.count", "level"):
                with self.subTest(table=table, field=field):
                    previous = self.cultivation_catalog(); candidate = deepcopy(previous)
                    plan = candidate[table][identifier]
                    path = f"/{table}/{identifier}"
                    if field in ("maxLevel", "levels"):
                        del plan[field]; path += "/" + field
                    elif field == "level":
                        del plan["levels"]["1"]; path += "/levels/1"
                    elif field == "cost.count":
                        del plan["levels"]["1"]["cost"][0]["count"]; path += "/levels/1/cost/0/count"
                    else:
                        del plan["levels"]["1"][field]; path += "/levels/1/" + field
                    result = self.audit(candidate, previous, component="pets")
                    self.assertFalse(result["passed"])
                    self.assertFinding(result, path)

    def test_new_sacred_plans_and_existing_legal_empty_costs_pass(self):
        previous = self.cultivation_catalog()
        self.assertTrue(self.audit(deepcopy(previous), previous, component="pets")["passed"])
        candidate = deepcopy(previous)
        for table, identifier in (("sacredStarPlans", "10"), ("sacredStagePlans", "20")):
            candidate[table]["99"] = deepcopy(candidate[table][identifier])
        result = self.audit(candidate, previous, component="pets")
        self.assertTrue(result["passed"])
        for table in ("sacredStarPlans", "sacredStagePlans"):
            self.assertChange(result, "row-added", f"/{table}/99")
        # Output coverage does not fabricate tables absent in an older schema;
        # the updater retains its independent required-rule validators.
        self.assertTrue(self.audit({"pets": {}}, {"pets": {}}, component="pets")["passed"])

    def test_fusion_job_rule_fields_and_alternatives_are_tracked(self):
        mutations = (
            ("/fusionJobs/0/name", lambda c: c["fusionJobs"][0].pop("name")),
            ("/fusionJobs/0/jobs", lambda c: c["fusionJobs"][0]["jobs"].pop()),
            ("/fusionJobs/0/jobs/0", lambda c: c["fusionJobs"][0]["jobs"][0].pop()),
            ("/fusionJobs/0", lambda c: c["fusionJobs"].clear()),
        )
        for path, mutate in mutations:
            with self.subTest(path=path):
                previous = self.cultivation_catalog(); candidate = deepcopy(previous); mutate(candidate)
                result = self.audit(candidate, previous, component="pets")
                self.assertFalse(result["passed"]); self.assertFinding(result, path)

    def test_additional_replay_tables_report_lost_fields_and_index_rows(self):
        paths = (
            "entryMentionsByBuff", "entryMentionsBySkill", "clientPresentation/pets",
            "clientPresentation/attributeIndex", "clientPresentation/formations", "clientPresentation/jobs",
            "replayCultivation/links", "replaySupplements/relations",
            "replaySupplements/summon/byPair", "replaySupplements/carry/byDeclaredSourceRaceId",
            "replaySupplements/almighty/byRaceId", "replaySupplements/hero/bySkillId",
            "replaySupplements/psychics/bySkillId", "replaySupplements/formationTeams/historyById",
            "replaySupplements/powerFull/raceIdOverrides", "replaySupplements/powerFull/rawMappingRows",
            "replaySupplements/titles/recordSchema", "replaySupplements/resources/avatar/rows",
            "replaySupplements/resources/changeAvatar/byId", "replaySupplements/resources/actionMovie/historyById",
            "replaySupplements/resources/preDownload/byMode", "replaySupplements/resources/background/byScene",
            "replaySupplements/resources/loading/byId",
        )
        for path in paths:
            with self.subTest(path=path):
                previous = battle_catalog(); parent = previous
                parts = path.split("/")
                for part in parts[:-1]: parent = parent.setdefault(part, {})
                # These tests exercise table preservation, not new game
                # mechanics or the detailed producer-specific row validators.
                parent[parts[-1]] = {"synthetic": {"value": "retained"}}
                candidate = deepcopy(previous); parent = candidate
                for part in parts: parent = parent[part]
                del parent["synthetic"]
                result = self.audit(candidate, previous)
                self.assertFalse(result["passed"])
                self.assertChange(result, "row-removed", "/" + path + "/synthetic")

    def test_new_mixed_configuration_records_exclude_source_wrappers_and_counters(self):
        previous = battle_catalog()
        previous["clientPresentation"].update(weakThreshold=0.8, strongThreshold=1.2,
            sourceEvidence={"SYNTHETIC/input": {"sha256": "old", "file": "old.as"}})
        previous["petDragons"]["geneRule"] = {"glitteryPrefix": "aa", "caseSensitive": True,
                                                 "source": {"file": "SYNTHETIC/gene.as"}}
        previous["replaySupplements"]["powerFull"] = {"defaultBuffId": "10000", "superJobBuffId": "10005",
            "lookupOrder": ["raceIdOverrides", "pet-dictionary", "defaultBuffId"],
            "defaultSource": {"file": "SYNTHETIC/full.as"}, "overrideHistory": {"1": [{"file": "old.as"}]}}
        previous["replaySupplements"]["titles"] = {"resourceTemplate": "SYNTHETIC/title{id}",
            "verifiedResourceCount": 1, "rawSource": {"file": "SYNTHETIC/title.as"}}
        candidate = deepcopy(previous)
        del candidate["clientPresentation"]["sourceEvidence"]
        del candidate["petDragons"]["geneRule"]["source"]
        del candidate["replaySupplements"]["powerFull"]["overrideHistory"]
        del candidate["replaySupplements"]["titles"]["verifiedResourceCount"]
        result = self.audit(candidate, previous)
        self.assertTrue(result["passed"])
        self.assertFalse(result["changes"], "provenance packaging must not be counted as lost game data")
        candidate = deepcopy(previous)
        del candidate["clientPresentation"]["weakThreshold"]
        del candidate["petDragons"]["geneRule"]["glitteryPrefix"]
        del candidate["replaySupplements"]["powerFull"]["defaultBuffId"]
        del candidate["replaySupplements"]["titles"]["resourceTemplate"]
        result = self.audit(candidate, previous)
        self.assertFalse(result["passed"])
        for path in ("/clientPresentation/weakThreshold", "/petDragons/geneRule/glitteryPrefix",
                     "/replaySupplements/powerFull/defaultBuffId", "/replaySupplements/titles/resourceTemplate"):
            self.assertFinding(result, path)


class PublicCatalogQualityGateTests(unittest.TestCase):
    @staticmethod
    def content_sha256(value):
        # The gate binds the complete input, including source evidence omitted
        # from semantic comparisons. Do not use the production hash helper here.
        data = json.dumps(value, ensure_ascii=False, sort_keys=True,
                          separators=(",", ":"), allow_nan=False).encode("utf-8")
        return hashlib.sha256(data).hexdigest()

    def enforce(self, candidate, previous, directory, *, source_coverage=None,
                reference_evidence=None, component="battleReplay"):
        before = deepcopy((candidate, previous, source_coverage, reference_evidence))
        try:
            return enforce_catalog_quality(component, candidate, previous, directory,
                                           source_coverage=source_coverage,
                                           reference_evidence=reference_evidence)
        finally:
            self.assertEqual((candidate, previous, source_coverage, reference_evidence), before,
                             "successful and rejected gates must leave every input unchanged")

    @staticmethod
    def synthetic_receipt(finding):
        return {**{key: finding.get(key) for key in
                   ("kind", "path", "targetNamespace", "targetId")},
                "evidence": {"synthetic": True,
                             "note": "合成审核测试证明，不代表任何游戏事实",
                             "source": "SYNTHETIC/verified-input.json"}}

    def assertPersistedReport(self, metadata, candidate, previous, directory, *,
                              component="battleReplay"):
        path = Path(metadata["reportFile"])
        self.assertEqual(path, Path(directory) / f"{component}-latest-attempt.json")
        raw = path.read_bytes()
        report = json.loads(raw.decode("utf-8"))
        self.assertEqual(metadata["reportSha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(report["candidateContentSha256"], self.content_sha256(candidate))
        self.assertEqual(metadata["candidateContentSha256"], report["candidateContentSha256"])
        self.assertEqual(report["previousContentSha256"],
                         self.content_sha256(previous) if previous is not None else None)
        self.assertEqual(report["component"], component)
        for field in ("schemaVersion", "passed", "summary", "decision"):
            self.assertEqual(metadata[field], report[field])
        counts = Counter(item["severity"] for item in report["findings"])
        self.assertEqual(metadata["summary"]["errors"], counts["error"])
        self.assertEqual(metadata["summary"]["reviewRequired"], counts["review"])
        self.assertEqual(metadata["summary"]["information"], counts["info"])
        self.assertEqual(metadata["summary"]["existingFindings"],
                         sum(item["baselineStatus"] == "existing" for item in report["findings"]))
        self.assertEqual(metadata["summary"]["resolvedFindings"],
                         sum(item["baselineStatus"] == "resolved" for item in report["findings"]))
        self.assertEqual(report["passed"], not (counts["error"] or counts["review"]))
        return report

    def test_passing_gate_binds_complete_candidate_and_baseline_before_publication(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["skills"]["10"]["description"] = "合成候选说明"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            active = root / "active.json"
            old_bytes = json.dumps(previous, ensure_ascii=False, indent=2).encode("utf-8")
            active.write_bytes(old_bytes)
            directory = root / "reports"
            metadata = self.enforce(candidate, previous, directory)
            report = self.assertPersistedReport(metadata, candidate, previous, directory)
            self.assertTrue(metadata["passed"])
            self.assertEqual(report["decision"], "validated-not-yet-applied")
            self.assertEqual(active.read_bytes(), old_bytes,
                             "validation must not claim to have installed the candidate")
            self.assertNotEqual(metadata["candidateContentSha256"], report["previousContentSha256"])

    def test_content_hash_ignores_object_order_but_covers_source_only_changes(self):
        candidate = battle_catalog()
        reordered = dict(reversed(list(candidate.items())))
        source_changed = deepcopy(candidate)
        source_changed["skills"]["10"]["sourceFile"] = "SYNTHETIC/new-source.xml"
        with tempfile.TemporaryDirectory() as temporary:
            first = self.enforce(candidate, None, temporary)
            self.assertPersistedReport(first, candidate, None, temporary)
            ordered = self.enforce(reordered, None, temporary)
            self.assertPersistedReport(ordered, reordered, None, temporary)
            self.assertEqual(first["candidateContentSha256"], ordered["candidateContentSha256"])
            changed = self.enforce(source_changed, candidate, temporary)
            self.assertPersistedReport(changed, source_changed, candidate, temporary)
            self.assertTrue(changed["passed"])
            self.assertNotEqual(first["candidateContentSha256"], changed["candidateContentSha256"])

    def test_rejected_gate_persists_evidence_and_prevents_the_callers_active_write(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["skills"]["10"]["name"] = ""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); directory = root / "reports"
            active = root / "active.json"
            old_bytes = json.dumps(previous, ensure_ascii=False, indent=2).encode("utf-8")
            active.write_bytes(old_bytes)

            def publish_after_validation():
                metadata = self.enforce(candidate, previous, directory)
                active.write_text(json.dumps(candidate, ensure_ascii=False), encoding="utf-8")
                return metadata

            with self.assertRaises(CatalogQualityError) as caught:
                publish_after_validation()
            metadata = caught.exception.metadata
            report = self.assertPersistedReport(metadata, candidate, previous, directory)
            self.assertFalse(metadata["passed"])
            self.assertGreater(metadata["summary"]["reviewRequired"], 0)
            self.assertEqual(report["decision"], "rejected-kept-existing")
            self.assertEqual(active.read_bytes(), old_bytes)
            self.assertTrue(any(item["kind"] == "text-cleared" and
                                item["path"] == "/skills/10/name" for item in report["findings"]))

    def test_source_coverage_review_or_error_blocks_publication_with_bound_metadata(self):
        for severity in ("review", "error"):
            with self.subTest(severity=severity), tempfile.TemporaryDirectory() as temporary:
                previous = battle_catalog(); candidate = deepcopy(previous)
                finding = {"kind": "synthetic-source-gap", "path": "/sources/synthetic",
                           "severity": severity, "baselineStatus": "new",
                           "message": "合成来源核验待处理", "sourceSha256": "a" * 64}
                coverage = {"schemaVersion": 1, "releaseFindings": [finding],
                            "summary": {"syntheticRows": 1}}
                active = Path(temporary) / "active.json"
                original = json.dumps(previous, ensure_ascii=False).encode("utf-8")
                active.write_bytes(original)
                directory = Path(temporary) / "reports"
                with self.assertRaises(CatalogQualityError) as caught:
                    self.enforce(candidate, previous, directory, source_coverage=coverage)
                    active.write_bytes(b"must never publish a rejected source")
                metadata = caught.exception.metadata
                report = self.assertPersistedReport(metadata, candidate, previous, directory)
                self.assertFalse(metadata["passed"])
                self.assertEqual(report["sourceCoverage"], coverage)
                self.assertIn(finding, report["findings"])
                self.assertEqual(report["decision"], "rejected-kept-existing")
                self.assertEqual(active.read_bytes(), original)

    def test_existing_or_resolved_source_information_is_retained_without_blocking(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        rows = [{"kind": "synthetic-source-gap", "path": f"/sources/{status}",
                 "severity": "info", "baselineStatus": status,
                 "message": "合成来源信息仍保留"} for status in ("existing", "resolved")]
        coverage = {"schemaVersion": 1, "releaseFindings": rows}
        with tempfile.TemporaryDirectory() as temporary:
            metadata = self.enforce(candidate, previous, temporary, source_coverage=coverage)
            report = self.assertPersistedReport(metadata, candidate, previous, temporary)
            self.assertTrue(metadata["passed"])
            self.assertEqual(report["sourceCoverage"], coverage)
            for row in rows:
                self.assertIn(row, report["findings"])

    def test_exact_reference_receipt_preserves_the_gap_as_information_with_its_evidence(self):
        previous = {"pets": {"1": {"name": "合成精灵", "slots": {"normal": 10}}},
                    "skills": {"10": {"name": "合成技能"}}}
        candidate = deepcopy(previous); candidate["pets"]["1"]["slots"]["normal"] = 999
        identity = {"kind": "missing-reference", "path": "/pets/1/slots/normal",
                    "targetNamespace": "skills", "targetId": "999"}
        receipt = self.synthetic_receipt(identity)
        # Candidate metadata is not an independently supplied receipt.
        candidate["referenceEvidence"] = [deepcopy(receipt)]
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(CatalogQualityError):
                self.enforce(candidate, previous, temporary, component="skills")
            metadata = self.enforce(candidate, previous, temporary, component="skills",
                                    reference_evidence=[receipt])
            report = self.assertPersistedReport(metadata, candidate, previous, temporary,
                                                component="skills")
            self.assertTrue(metadata["passed"])
            rows = [row for row in report["findings"] if row["path"] == identity["path"]]
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertEqual({key: row[key] for key in identity}, identity)
            self.assertEqual(row["severity"], "info")
            self.assertEqual(row["baselineStatus"], "new")
            self.assertEqual(row["officialInputEvidence"], receipt["evidence"])
            self.assertEqual(row["resolution"], "reference-present-and-definition-absent-in-official-input")
            self.assertNotIn("999", candidate["skills"], "a receipt must not synthesize a missing definition")

    def test_reference_receipts_cannot_match_another_owner_namespace_target_or_an_empty_proof(self):
        previous = {"pets": {"1": {"name": "合成精灵", "slots": {"normal": 10}}},
                    "skills": {"10": {"name": "合成技能"}}}
        candidate = deepcopy(previous); candidate["pets"]["1"]["slots"]["normal"] = 999
        receipt = self.synthetic_receipt({"kind": "missing-reference", "path": "/pets/1/slots/normal",
                                          "targetNamespace": "skills", "targetId": "999"})
        cases = (("kind", "official-text-empty"), ("path", "/pets/2/slots/normal"),
                 ("targetNamespace", "buffs"), ("targetId", "998"), ("targetId", 999),
                 ("evidence", {}), ("evidence", None))
        with tempfile.TemporaryDirectory() as temporary:
            for field, value in cases:
                with self.subTest(field=field, value=value):
                    wrong = deepcopy(receipt); wrong[field] = value
                    with self.assertRaises(CatalogQualityError) as caught:
                        self.enforce(candidate, previous, temporary, component="skills",
                                     reference_evidence=[wrong])
                    report = self.assertPersistedReport(caught.exception.metadata, candidate, previous,
                                                        temporary, component="skills")
                    row = next(item for item in report["findings"]
                               if item["path"] == "/pets/1/slots/normal")
                    self.assertEqual(row["severity"], "review")
                    self.assertEqual(row["baselineStatus"], "new")
                    self.assertNotIn("officialInputEvidence", row)

    def test_reference_receipts_cannot_pardon_field_loss_structure_or_reverse_index_errors(self):
        cases = (
            ("field-removed", "/buffs/20/once", lambda c: c["buffs"]["20"].pop("once")),
            ("invalid-reference", "/tableRelations/0/skillId",
             lambda c: c["tableRelations"][0].__setitem__("skillId", [])),
            ("reverse-index-mismatch", "/relationsBySkill/11",
             lambda c: c["relationsBySkill"].__setitem__("11", ["synthetic-relation-a"])),
        )
        for kind, path, mutate in cases:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                previous = battle_catalog(); candidate = deepcopy(previous)
                candidate["petMechanics"]["1"]["skillLinks"][0]["skillId"] = "999"
                mutate(candidate)
                initial = audit_catalog_quality(candidate, previous)
                blocking = next(row for row in initial["findings"]
                                if row["kind"] == kind and row["path"] == path)
                # Even a matching receipt for every finding is only allowed to
                # annotate a proper missing-reference review. Other failures
                # must survive unchanged, while the independently proven gap
                # is retained as information in the same rejected report.
                receipts = [self.synthetic_receipt(row) for row in initial["findings"]]
                with self.assertRaises(CatalogQualityError) as caught:
                    self.enforce(candidate, previous, temporary, reference_evidence=receipts)
                report = self.assertPersistedReport(caught.exception.metadata, candidate, previous,
                                                    temporary)
                remaining = next(row for row in report["findings"]
                                 if row["kind"] == kind and row["path"] == path)
                self.assertEqual(remaining["severity"], blocking["severity"])
                self.assertIn(remaining["severity"], ("review", "error"))
                self.assertNotIn("officialInputEvidence", remaining)
                gap = next(row for row in report["findings"] if row["kind"] == "missing-reference" and
                           row["path"] == "/petMechanics/1/skillLinks/0/skillId")
                self.assertEqual(gap["severity"], "info")
                self.assertTrue(gap["officialInputEvidence"]["synthetic"])

    def test_every_attempt_atomically_replaces_only_the_latest_report_for_its_profile(self):
        previous = battle_catalog()
        rejected = deepcopy(previous); rejected["buffs"]["20"]["mess"] = ""
        corrected = deepcopy(previous); corrected["buffs"]["20"]["mess"] = "合成已核新说明"
        original_replace = os.replace
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            destination = directory / "battleReplay-latest-attempt.json"
            prior_bytes = [None]
            replacements = []

            def inspect_replace(source, target):
                source, target = Path(source), Path(target)
                self.assertEqual(target, destination)
                self.assertEqual(source.parent, directory)
                self.assertNotEqual(source, target)
                data = source.read_bytes()
                json.loads(data.decode("utf-8"))  # The staged report is complete.
                if prior_bytes[0] is None:
                    self.assertFalse(target.exists())
                else:
                    self.assertEqual(target.read_bytes(), prior_bytes[0],
                                     "the prior complete report must survive until replacement")
                original_replace(source, target)
                prior_bytes[0] = data
                replacements.append(data)

            with patch("public_catalog_quality.os.replace", side_effect=inspect_replace):
                initial = self.enforce(previous, None, directory)
                self.assertPersistedReport(initial, previous, None, directory)
                with self.assertRaises(CatalogQualityError) as caught:
                    self.enforce(rejected, previous, directory)
                self.assertPersistedReport(caught.exception.metadata, rejected, previous, directory)
                final = self.enforce(corrected, previous, directory)
                self.assertPersistedReport(final, corrected, previous, directory)
            self.assertEqual(len(replacements), 3)
            self.assertEqual(destination.read_bytes(), replacements[-1])
            self.assertEqual({path.name for path in directory.iterdir()}, {destination.name})
            # A second declared profile gets its own last attempt; it cannot
            # overwrite or prune the battle replay report.
            pets = {"pets": {"1": {"name": "合成精灵", "groupRaceId": 0}}}
            other = self.enforce(pets, None, directory, component="pets")
            self.assertPersistedReport(other, pets, None, directory, component="pets")
            self.assertEqual(destination.read_bytes(), replacements[-1])
            self.assertEqual({path.name for path in directory.iterdir()},
                             {destination.name, "pets-latest-attempt.json"})

    def test_failed_atomic_replace_preserves_the_old_report_and_cleans_temporary_files(self):
        previous = battle_catalog(); candidate = deepcopy(previous)
        candidate["skills"]["10"]["description"] = "合成下一版说明"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            initial = self.enforce(previous, None, directory)
            destination = Path(initial["reportFile"])
            original = destination.read_bytes()
            with patch("public_catalog_quality.os.replace", side_effect=PermissionError("synthetic locked report")):
                with self.assertRaises(PermissionError):
                    self.enforce(candidate, previous, directory)
            self.assertEqual(destination.read_bytes(), original)
            self.assertEqual({path.name for path in directory.iterdir()}, {destination.name})
            self.assertPersistedReport(initial, previous, None, directory)


if __name__ == "__main__":
    unittest.main()
