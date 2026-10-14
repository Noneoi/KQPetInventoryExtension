"""Independent, synthetic source-to-output regressions; no fabricated evidence."""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import public_replay_fidelity as subject


class ReplayFidelityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name)
        approved = {}
        # Production approvals must never be learned from the candidate. For
        # tests alone we substitute identifiable synthetic trusted input bytes.
        for relative in subject._CONSUMER_HASHES:
            data = ("SYNTHETIC APPROVED CONSUMER FIXTURE: " + relative).encode()
            self.write(relative, data)
            approved[relative] = hashlib.sha256(data).hexdigest()
        guard = patch.object(subject, "_CONSUMER_HASHES", approved)
        guard.start(); self.addCleanup(guard.stop)
        self.maps = {
            "SkillConfigSimpleAC": {"1103016": {"name": "派生技能", "desc": "派生独立说明"}},
            "SkillConfigSimple3000": {"500001": {"name": "只有说明", "desc": ""}},
            "SkillConfigSimple6000": {
                "103016": {"name": "技能甲", "desc": "<font color='#00FFFF'>甲说明</font>\n下一行"},
                "103017": {"name": "技能乙", "desc": "乙说明"}},
            "SkillConfigSimple9999": {},
        }
        self.write_maps()
        self.desc = {
            "103016": dict(id="103016", attackType="近身", skillBaseType="普攻技", skillType="物攻", bossSkill="1", ft="伤害"),
            "103017": dict(id="103017", attackType="远程", skillBaseType="普攻技", skillType="魔攻", bossSkill="0", ft="伤害"),
            "1103016": dict(id="1103016", attackType="远程", skillBaseType="召唤技", skillType="魔攻", bossSkill="0", ft="BuffOnly"),
            "500001": dict(id="500001", attackType="远程", skillBaseType="普攻技", skillType="魔攻", bossSkill="0", ft="伤害"),
        }
        self.xml("SkillDescConfigXML", "<config><pet>" + self.elements(self.desc.values()) + "</pet></config>")
        self.playback = {
            "103016": dict(id="103016", action="4", pos="5", jump="5", effect="9", swf="610", extraRef="77"),
            "103017": dict(id="103017", action="6", pos="10", jump="", effect="13"),
        }
        self.last = dict(id="103016", action="7", pos="5", jump="5", effect="14")
        old = dict(id="103016 ", action="2", pos="5", jump="5", effect="8", obsolete="must-not-survive")
        invalid_later = dict(id="103016", action="0", pos="5", jump="5", effect="9")
        skill_xml = ("<config>" + self.elements([old, *self.playback.values(), invalid_later])
                     + self.elements([self.last], tag="last")
                     + '<transform><s id="103016" action="999"/></transform></config>')
        self.xml("SkillConfigXML", skill_xml)
        self.write("h5-tables/skill_desc_config.json", json.dumps({
            "103016": "不可代替 Flash 名称|不可代替 Flash 说明|AAAAA",
            "103018": "H5 独有|只扩大资料范围|AAAAA"}, ensure_ascii=False))
        self.configs = {
            "7": dict(id="7", did="8", pos="5", once="0", df="0", nm="配置甲"),
            "8": dict(id="8", did="7", pos="0", once="1", df="1", nm="配置乙"),
            "9": dict(id="9", did="645", pos="0", once="0", df="0"),
        }
        self.buff_desc = {
            "7": dict(id="7", nm="说明甲名称", mess="说明甲内容", beffect="标签甲", bhs="来源标签甲"),
            "8": dict(id="8", nm="", mess="", beffect="", bhs=""),
        }
        self.xml("BufferConfigXML", '<config><buff><s id="7" did="999" obsolete="not-merged"/>'
                 + self.elements(self.configs.values()) + "</buff></config>")
        self.xml("BufferDescConfigXML", "<config><buff>" + self.elements(self.buff_desc.values()) + "</buff></config>")
        self.write("h5-tables/buffer_config.json", json.dumps({"buff": {
            "7": "H5甲|不替代甲说明|70", "8": "H5乙|不补空说明|80",
            "9": "H5丙|不补无描述记录|90", "raw_legacy_key": "||645"}}, ensure_ascii=False))
        self.execution = {"1": dict(code="1", nm="执行", eid="-1", type="1"),
                          "-1": dict(code="-1", nm="不能执行", eid="1", type="2")}
        self.buffer_execution = {"200001": dict(bufferId="200001", actionResId="70", skillId="103016", lightedSkillId="103017", desc="队伍执行原文")}
        self.attributes = {"1": dict(id="1", nm="属性甲", pri="1", rcs="1,0.7"),
                           "2": dict(id="2", nm="属性乙", pri="2", rcs="1.3,1")}
        self.combos = [dict(skillId="103016", buffs="7,8", targetSkillIds="103017,1103016"),
                       dict(skillId="103017", buffs="9", targetSkillIds="103016")]
        self.xml("SkillExecuteXML", "<config>" + self.elements(self.execution.values()) + "</config>")
        self.xml("BufferExecuteXML", "<config>" + self.elements(self.buffer_execution.values()) + "</config>")
        self.xml("RaceTypeConfigXML", "<config><race_type>" + self.elements(self.attributes.values()) + "</race_type></config>")
        self.xml("ComboBuffsConfigXML", "<config><combo>" + self.elements(self.combos) + "</combo></config>")
        # Hand-built expected output: the producer/production auditor is not
        # used to generate any assertion fixture.
        skills = {}
        for key, name, description in (
            ("103016", "技能甲", "<font color='#00FFFF'>甲说明</font>\n下一行"),
            ("103017", "技能乙", "乙说明"),
        ):
            skills[key] = {**self.desc[key], **self.playback[key], "id": key, "name": name, "description": description,
                           "configSourceId": key, "configResolution": "direct", "playbackAvailable": True}
        skills["500001"] = {**self.desc["500001"], "name": "只有说明", "description": "",
                            "configResolution": "description-only", "playbackAvailable": False}
        skills["103018"] = {"id": "103018", "name": "", "description": "",
                            "configResolution": "description-only", "playbackAvailable": False}
        skills["1103016"] = {**self.desc["1103016"], "name": "派生技能", "description": "派生独立说明",
            "attackType": "近身", "skillBaseType": "普攻技", "skillType": "物攻", "bossSkill": "1",
            "action": "4", "pos": "5", "jump": "5", "effect": "9", "swf": "610",
            "descriptionAttributes": deepcopy(self.desc["1103016"]), "configSourceId": "103016",
            "configResolution": "copyDefine", "playbackAvailable": True}
        buffs = {
            "7": {**self.configs["7"], "nm": "说明甲名称", "nameSource": "flash-desc", "nameStatus": "named",
                  "configName": "配置甲", "configSource": "flash-config", "descriptionSource": "flash-desc",
                  "mess": "说明甲内容", "beffect": "标签甲", "bhs": "来源标签甲"},
            "8": {**self.configs["8"], "nameSource": "flash-config", "nameStatus": "named", "configName": "配置乙",
                  "configSource": "flash-config", "descriptionSource": "flash-desc", "mess": "", "beffect": "", "bhs": ""},
            "9": {**self.configs["9"], "nm": "H5丙", "nameSource": "h5-buffer", "nameStatus": "named", "configName": "",
                  "configSource": "flash-config", "descriptionSource": "none", "mess": "", "beffect": "", "bhs": ""},
            "raw_legacy_key": {"id": "raw_legacy_key", "nm": "", "nameSource": "none", "nameStatus": "unnamed",
                  "configName": "", "configSource": "none", "descriptionSource": "none", "mess": "", "beffect": "", "bhs": ""},
        }
        self.candidate = {"skills": skills, "buffs": buffs, "execution": deepcopy(self.execution),
            "bufferExecution": deepcopy(self.buffer_execution), "attributes": deepcopy(self.attributes),
            "combos": deepcopy(self.combos), "skillLastEffects": {"103016": {**self.desc["103016"], **self.last,
                "name": "技能甲", "description": "<font color='#00FFFF'>甲说明</font>\n下一行",
                "configSourceId": "103016", "configResolution": "direct-last", "playbackAvailable": True,
                "descriptionAttributes": deepcopy(self.desc["103016"])}}}

    def write(self, relative, data):
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
        return path

    def write_maps(self):
        for name, rows in self.maps.items():
            self.write(subject._DIRECT + name + ".as", "public static const MAP:Object = "
                       + json.dumps(rows, ensure_ascii=False) + ";")

    def xml(self, name, value):
        return self.write("xml/mmo.battleconfig.data." + name + "__xmlClass.xml", value)

    @staticmethod
    def elements(rows, *, tag="s"):
        return "".join(ET.tostring(ET.Element(tag, row), encoding="unicode") for row in rows)

    def audit(self, candidate=None, **kwargs):
        candidate = self.candidate if candidate is None else candidate
        before = deepcopy(candidate)
        result = subject.audit_replay_fidelity(candidate, build_arguments=kwargs.get("build_arguments", {"source": self.source}),
                                               previous=kwargs.get("previous"))
        self.assertEqual(candidate, before)
        self.assertEqual(result["schemaVersion"], 1)
        self.assertTrue(result["inputs"])
        self.assertEqual(result["candidateContentSha256"], hashlib.sha256(json.dumps(candidate,
            ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest())
        self.assertEqual(result["passed"], not any(row["severity"] in ("error", "review") for row in result["findings"]))
        for row in result["findings"]:
            self.assertTrue({"severity", "kind", "path", "message"} <= row.keys())
        return result

    def assertMismatch(self, result, path):
        self.assertFalse(result["passed"])
        self.assertTrue(any(row["kind"] == "official-value-mismatch" and row["path"] == path
                            for row in result["findings"]), result["findings"])

    def test_hand_built_catalog_matches_independent_source_projection(self):
        with patch("extract_skill_tables.extract_skill_tables", side_effect=AssertionError("must not call generator")), \
             patch("extract_buff_tables.extract_buff_tables", side_effect=AssertionError("must not call generator")):
            result = self.audit()
        self.assertTrue(result["passed"], result["findings"])
        self.assertEqual(result["summary"]["tablesChecked"], 7)
        self.assertEqual(result["summary"]["inputFiles"], 29)
        self.assertEqual(result["coverage"][0]["rejectedOfficialPlaybackRows"], 1)
        self.assertGreater(result["summary"]["comparisons"], 100)

    def test_skill_description_and_animation_pointer_swaps_fail_without_count_change(self):
        for field in ("name", "description", "action", "effect"):
            with self.subTest(field=field):
                candidate = deepcopy(self.candidate)
                a, b = candidate["skills"]["103016"], candidate["skills"]["103017"]
                a[field], b[field] = b[field], a[field]
                result = self.audit(candidate)
                self.assertMismatch(result, "/skills/103016/" + field)
                self.assertMismatch(result, "/skills/103017/" + field)

    def test_exact_original_id_text_routing_and_ac_priority(self):
        # An unreachable duplicate from the wrong bucket must not override AC.
        self.maps["SkillConfigSimple6000"]["1103016"] = {"name": "错误桶", "desc": "错误说明"}
        self.write_maps()
        self.assertTrue(self.audit()["passed"])
        candidate = deepcopy(self.candidate)
        candidate["skills"]["1103016"]["name"] = "技能甲"
        self.assertMismatch(self.audit(candidate), "/skills/1103016/name")
        candidate = deepcopy(self.candidate)
        candidate["skills"]["103018"]["description"] = "只扩大资料范围"
        self.assertMismatch(self.audit(candidate), "/skills/103018/description")

    def test_data_map_last_complete_duplicate_wins_without_merging_text(self):
        self.write(subject._DIRECT + "SkillConfigSimpleAC.as", '''public static const MAP:Object = {
            "1103016":{"name":"过期名","desc":"不得继承"}, "1103016":{"name":"新名"}};''')
        candidate = deepcopy(self.candidate)
        candidate["skills"]["1103016"].update(name="新名", description="")
        self.assertTrue(self.audit(candidate)["passed"])
        candidate["skills"]["1103016"]["description"] = "不得继承"
        self.assertMismatch(self.audit(candidate), "/skills/1103016/description")

    def test_copy_define_fields_do_not_copy_unknown_fields_or_replace_own_text(self):
        for field, value in (("swf", "1103016"), ("configSourceId", "103017"), ("ft", "伤害"),
                             ("attackType", "远程"), ("extraRef", "77")):
            with self.subTest(field=field):
                candidate = deepcopy(self.candidate); candidate["skills"]["1103016"][field] = value
                self.assertMismatch(self.audit(candidate), "/skills/1103016/" + field)
        candidate = deepcopy(self.candidate)
        candidate["skills"]["1103016"]["descriptionAttributes"]["skillType"] = "物攻"
        self.assertMismatch(self.audit(candidate), "/skills/1103016/descriptionAttributes/skillType")

    def test_invalid_last_duplicate_and_nested_skill_do_not_replace_valid_normal_row(self):
        self.assertTrue(self.audit()["passed"])
        for value in ("0", "999"):
            candidate = deepcopy(self.candidate); candidate["skills"]["103016"]["action"] = value
            self.assertMismatch(self.audit(candidate), "/skills/103016/action")
        candidate = deepcopy(self.candidate); candidate["skills"]["103016"]["obsolete"] = "must-not-survive"
        self.assertMismatch(self.audit(candidate), "/skills/103016/obsolete")
        candidate = deepcopy(self.candidate); candidate["skillLastEffects"]["103016"]["action"] = "4"
        self.assertMismatch(self.audit(candidate), "/skillLastEffects/103016/action")

    def test_non_auto_chess_long_identifier_does_not_gain_inherited_playback(self):
        self.maps["SkillConfigSimpleAC"]["1403016"] = {"name": "独立长编号", "desc": ""}
        self.write_maps(); candidate = deepcopy(self.candidate)
        candidate["skills"]["1403016"] = {"id": "1403016", "name": "独立长编号", "description": "",
            "configResolution": "description-only", "playbackAvailable": False}
        self.assertTrue(self.audit(candidate)["passed"])
        candidate["skills"]["1403016"]["action"] = "4"
        self.assertMismatch(self.audit(candidate), "/skills/1403016/action")

    def test_buff_descriptor_uses_buff_id_never_display_resource_id(self):
        candidate = deepcopy(self.candidate)
        candidate["buffs"]["7"]["mess"], candidate["buffs"]["8"]["mess"] = "", "说明甲内容"
        result = self.audit(candidate)
        self.assertMismatch(result, "/buffs/7/mess")
        self.assertMismatch(result, "/buffs/8/mess")
        candidate = deepcopy(self.candidate)
        candidate["buffs"]["7"]["did"], candidate["buffs"]["8"]["did"] = "7", "8"
        self.assertMismatch(self.audit(candidate), "/buffs/7/did")

    def test_named_fallbacks_and_official_blank_text_do_not_fill_other_buff_fields(self):
        self.assertTrue(self.audit()["passed"])
        for identifier, field, value in (("8", "nm", "H5乙"), ("8", "mess", "不补空说明"),
                                         ("9", "mess", "不补无描述记录"), ("9", "did", "90"),
                                         ("raw_legacy_key", "id", "0")):
            with self.subTest(identifier=identifier, field=field):
                candidate = deepcopy(self.candidate); candidate["buffs"][identifier][field] = value
                self.assertMismatch(self.audit(candidate), f"/buffs/{identifier}/{field}")

    def test_raw_xml_codes_targets_coefficients_and_combo_order_are_exact(self):
        cases = (
            ("/execution/1/eid", lambda c: c["execution"]["1"].__setitem__("eid", "1")),
            ("/bufferExecution/200001/skillId", lambda c: c["bufferExecution"]["200001"].__setitem__("skillId", "103017")),
            ("/attributes/1/rcs", lambda c: c["attributes"]["1"].__setitem__("rcs", "1.3,1")),
            ("/combos/0/targetSkillIds", lambda c: c["combos"][0].__setitem__("targetSkillIds", "1103016,103017")),
            ("/combos/0/skillId", lambda c: c["combos"].reverse()),
        )
        for path, mutate in cases:
            with self.subTest(path=path):
                candidate = deepcopy(self.candidate); mutate(candidate)
                self.assertMismatch(self.audit(candidate), path)

    def test_same_count_id_replacement_and_omitted_source_fields_fail(self):
        candidate = deepcopy(self.candidate)
        candidate["skills"]["103019"] = candidate["skills"].pop("103018")
        self.assertMismatch(self.audit(candidate), "/skills/103018")
        candidate = deepcopy(self.candidate); del candidate["buffs"]["7"]["once"]
        self.assertMismatch(self.audit(candidate), "/buffs/7/once")
        candidate = deepcopy(self.candidate); candidate["skills"]["103016"]["playbackAvailable"] = 1
        self.assertMismatch(self.audit(candidate), "/skills/103016/playbackAvailable")

    def test_new_official_text_requires_matching_candidate_but_valid_update_passes(self):
        self.maps["SkillConfigSimple6000"]["103017"]["desc"] = "官方输入更新后的合成说明"
        self.write_maps()
        self.assertMismatch(self.audit(), "/skills/103017/description")
        candidate = deepcopy(self.candidate); candidate["skills"]["103017"]["description"] = "官方输入更新后的合成说明"
        self.assertTrue(self.audit(candidate)["passed"])

    def test_missing_ambiguous_or_changed_consumer_input_fails_closed(self):
        relative = next(iter(subject._CONSUMER_HASHES))
        path = self.source / relative
        original = path.read_bytes(); path.write_bytes(original + b"CHANGED")
        result = self.audit()
        self.assertFalse(result["passed"])
        self.assertTrue(any(row["kind"] == "unreviewed-consumer" for row in result["findings"]))
        path.write_bytes(original)
        xml_path = next((self.source / "xml").glob("*.SkillExecuteXML__*.xml"))
        duplicate = self.write("xml/duplicate.SkillExecuteXML__xmlClass.xml", xml_path.read_bytes())
        self.assertFalse(self.audit()["passed"])
        duplicate.unlink()
        xml_path.unlink()
        result = self.audit(); self.assertFalse(result["passed"])
        self.assertTrue(any(row["kind"] == "official-input-unavailable" for row in result["findings"]))
        self.assertFalse(self.audit(build_arguments={})["passed"])

    def test_executable_map_and_unknown_numeric_forms_are_rejected_not_evaluated(self):
        self.write(subject._DIRECT + "SkillConfigSimpleAC.as", 'public static const MAP:Object = {"1": unknown()};')
        result = self.audit(); self.assertFalse(result["passed"])
        self.write_maps()
        self.xml("SkillConfigXML", '<config><s id="103016" action="not-a-frame" pos="5" effect="8"/></config>')
        result = self.audit(); self.assertFalse(result["passed"])
        self.assertTrue(any(row["kind"] == "official-input-unavailable" for row in result["findings"]))

    def test_candidate_source_claims_are_not_used_as_fidelity_oracle(self):
        candidate = deepcopy(self.candidate)
        candidate["publicUpdate"] = {"sourceFingerprint": "fabricated-claim"}
        candidate["skills"]["103016"]["textSource"] = {"table": "fabricated-claim", "sha256": "fabricated-claim"}
        self.assertTrue(self.audit(candidate)["passed"], "metadata is outside source value verification")
        candidate["skills"]["103016"]["description"] = "与官方输入不符"
        self.assertMismatch(self.audit(candidate), "/skills/103016/description")

    def hero_fixture(self, *, seed=100, expected=None):
        self.hero_arguments = {"source": self.source, "auxiliary": self.source / "aux",
            "skin_source": self.source / "skins", "formation_source": self.source / "formation-data"}
        locations = {
            "hero_definition": "formation-data/interfaces/scripts/mmo/interfaces/formation/HeroSkillDefine.as",
            "hero_config": "formation-data/formation/scripts/mmo/formation/config/HeroSkillConfig.as",
            "skin_config": "skins/scripts/mmo/changepetskinv3/other/Cpsv3SkinConfig.as",
            "skin_definition": "skins/scripts/mmo/changepetskinv3/other/Cpsv3Skin.as",
            "skin_summary": "skins/scripts/mmo/changepetskinv3/other/Cpsv3SkinSummary.as",
            "skin_service": "aux/pet-identity-20261009/skin/scripts/mmo/changepetskinv3/ChangePetSkinService.as",
            "form_config": "aux/completion-20261010/multipleform/scripts/mmo/multipleformpet/config/MultipleFormPetConfig.as",
            "form_controller": "aux/completion-20261010/multipleform/scripts/mmo/multipleformpet/control/MultipleFormPetController.as",
            "race_util": "aux/origin-link-audit-20261009/fortune/common/scripts/mmo/common/utils/petrace/PetRaceUtil.as",
        }
        approvals = {}
        for role in subject._HERO_CONSUMERS:
            data = ("SYNTHETIC APPROVED HERO CONSUMER: " + role).encode()
            if role == "form_config":
                # First group only, not transitive closure through [200,300].
                data = b'private static const FORM_CONF:Array = [new MultipleFormPetFormDefine([100,200]),new MultipleFormPetFormDefine([200,300])];'
            self.write(locations[role], data)
            approvals[role] = hashlib.sha256(data).hexdigest()
        guard = patch.object(subject, "_HERO_CONSUMERS", approvals)
        guard.start(); self.addCleanup(guard.stop)
        skins = [
            {"id": 11, "show": True, "fuse": None, "availablePets": [{"raceId": 101, "raceIdNeed": 100}]},
            {"id": 12, "show": False, "fuse": None, "availablePets": [{"raceId": 102, "raceIdNeed": 100}]},
            {"id": 13, "show": True, "fuse": None, "availablePets": [{"raceId": 201, "raceIdNeed": 200}]},
            {"id": 14, "show": True, "fuse": [11, 12], "availablePets": [{"raceId": 301, "raceIdNeed": 0}]},
        ]
        self.hero_skin_path = self.write(locations["skin_config"], "private static const SKIN_DEFINES_SRC:Array = " + json.dumps(skins) + ";")
        params = [300100, 0, 0, 1, "RaceEffected", str(seed), 0, "AllCreature", "", "1", 0]
        self.write(locations["hero_config"], "private static const defines:Array = [new HeroSkillDefine("
            + ",".join(json.dumps(value) for value in params) + ")];")
        values = expected if expected is not None else ["100", "101", "200", "201"]
        row = {"rowId": 0, "skillId": "300100", "cls": "RaceEffected", "clsParams": str(seed), "rawParameters": params,
            "effectRaceSeed": str(seed), "effectiveClsParamsStatus": "resolved", "effectiveRaceIds": list(values),
            "readings": [{"label": "synthetic field " + str(i), "value": ""} for i in range(5)]
                         + [{"label": "官方皮肤及形态兼容种族集合", "value": list(values)}]}
        self.candidate["replaySupplements"] = {"hero": {"rows": [row]}}
        previous = deepcopy(self.candidate)
        old = previous["replaySupplements"]["hero"]["rows"][0]
        old_values = ["100", "101", "102", "200", "201"]
        old["effectiveRaceIds"] = list(old_values); old["readings"][5]["value"] = list(old_values)
        return previous

    def test_hero_hidden_skin_change_emits_only_two_exact_value_bound_receipts(self):
        previous = self.hero_fixture()
        previous_copy = deepcopy(previous)
        report = self.audit(build_arguments=self.hero_arguments, previous=previous)
        self.assertTrue(report["passed"], report["findings"])
        self.assertEqual(previous, previous_copy)
        receipts = report["allowedCorrections"]
        self.assertEqual({row["path"] for row in receipts}, {
            "/replaySupplements/hero/rows/0/effectiveRaceIds", "/replaySupplements/hero/rows/0/readings/5/value"})
        for receipt in receipts:
            self.assertEqual(receipt["rule"], "hero-raceeffected-visible-skin-membership-v1")
            self.assertTrue(receipt["previousPresent"] and receipt["candidatePresent"])
            for prefix, value in (("previous", ["100", "101", "102", "200", "201"]),
                                  ("candidate", ["100", "101", "200", "201"])):
                self.assertEqual(receipt[prefix + "ValueSha256"], hashlib.sha256(json.dumps(value,
                    ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest())
            proof = receipt["source"]
            self.assertEqual(proof["resolution"], "derived-display-race-set-verified-from-current-official-input")
            record = next(value for value in proof["inputs"] if value["role"] == "hero-membership:skin_config")
            self.assertEqual(record["sha256"], hashlib.sha256(self.hero_skin_path.read_bytes()).hexdigest())

    def test_hero_wrong_membership_or_other_fidelity_error_never_gets_receipts(self):
        previous = self.hero_fixture()
        for field in ("effectiveRaceIds", "reading", "skillId", "core-skill"):
            with self.subTest(field=field):
                candidate = deepcopy(self.candidate); row = candidate["replaySupplements"]["hero"]["rows"][0]
                if field == "effectiveRaceIds": row[field] = ["100", "101", "300"]
                elif field == "reading": row["readings"][5]["value"] = ["100", "102"]
                elif field == "skillId": row[field] = "300999"
                else: candidate["skills"]["103016"]["description"] = "错误说明"
                report = self.audit(candidate, build_arguments=self.hero_arguments, previous=previous)
                self.assertFalse(report["passed"]); self.assertEqual(report["allowedCorrections"], [])

    def test_hero_missing_old_value_or_changed_owner_never_gets_receipts(self):
        previous = self.hero_fixture()
        for field in ("effectiveRaceIds", "skillId", "reading"):
            with self.subTest(field=field):
                old = deepcopy(previous); row = old["replaySupplements"]["hero"]["rows"][0]
                if field == "effectiveRaceIds": del row[field]
                elif field == "skillId": row[field] = "300999"
                else: row["readings"][5]["value"] = ["100"]
                report = self.audit(build_arguments=self.hero_arguments, previous=old)
                self.assertTrue(report["passed"]); self.assertEqual(report["allowedCorrections"], [])

    def test_hero_missing_raw_inputs_or_changed_consumer_rejects(self):
        previous = self.hero_fixture()
        report = self.audit(previous=previous)
        self.assertFalse(report["passed"]); self.assertEqual(report["allowedCorrections"], [])
        consumer = self.source / "skins/scripts/mmo/changepetskinv3/other/Cpsv3SkinSummary.as"
        consumer.write_bytes(consumer.read_bytes() + b"UNKNOWN NEW RULE")
        report = self.audit(build_arguments=self.hero_arguments, previous=previous)
        self.assertFalse(report["passed"]); self.assertEqual(report["allowedCorrections"], [])

    def test_hero_public_skin_reset_hidden_exact_skin_and_fusion_branches(self):
        # Independent expected results: skin 101 normalizes to 100 then gets
        # first form group; hidden exact skin 102 and fusion poster 301 do not.
        self.hero_fixture(seed=101, expected=["100", "101", "200", "201"])
        self.assertTrue(self.audit(build_arguments=self.hero_arguments)["passed"])
        for seed in (102, 301):
            params = [300100, 0, 0, 1, "RaceEffected", str(seed), 0, "AllCreature", "", "1", 0]
            self.write("formation-data/formation/scripts/mmo/formation/config/HeroSkillConfig.as",
                "private static const defines:Array = [new HeroSkillDefine(" + ",".join(json.dumps(v) for v in params) + ")];")
            row = self.candidate["replaySupplements"]["hero"]["rows"][0]
            row.update(clsParams=str(seed), rawParameters=params, effectRaceSeed=str(seed), effectiveRaceIds=[str(seed)])
            row["readings"][5]["value"] = [str(seed)]
            report = self.audit(build_arguments=self.hero_arguments)
            self.assertTrue(report["passed"], report["findings"])
            self.assertEqual(report["allowedCorrections"], [])


if __name__ == "__main__":
    unittest.main()
