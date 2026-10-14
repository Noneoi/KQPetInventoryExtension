"""Focused static-parser/version-cache tests; no account or network access."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_routine_updater as routines
import public_routine_rules as rules

RULE_FIXTURES = Path(__file__).resolve().parents[1] / "fixtures/routine-official-rules"
RULE_MANIFEST = json.loads((RULE_FIXTURES / "manifest.json").read_text(encoding="utf-8"))


def official_rule_texts():
    return {name: (RULE_FIXTURES / record["file"]).read_text(encoding="utf-8-sig")
            for name, record in RULE_MANIFEST["files"].items()}


TASKS = r'''public static const TASK_CONFIG:Array = [
 new DiamondTaskDefine(1,"活动, (一)",1,15,15,3,15,"ignored",7,"res"),
 new DiamondTaskDefine(2,"新任务",1,15,13,Number\n .MAX_VALUE,15)];
 public static const DAY_PRIZE_PROGRESS1:Array = [10,20,30,60,100];
 public static const WEEK_PRIZE_PROGRESS:Array = [150,300,600,900,1200];'''.replace(r"\n", "\n")
HUD = '''public static const DATA:Object = {"hud":{"group":[
 {"key":"old-activity","name":"活动 A","startTime":"20260911","redPointId":100,"tryGetService":"NewActivityService#action"},
 {"key":"no-red","name":"活动 B","tryGetService":"NewActivityService#action"},
 {"key":"decoration","name":"不属于活动的静态图标"}]}};'''
RED = '''public static const A:* = new RedPointConfigNode(100,[101]);
 public static const B:* = new RedPointConfigNode(101,[100,102]);
 public static const C:* = new RedPointConfigNode(102);'''


class FakeUpdater:
    def __init__(self, root):
        self.root = root
        self.catalog, self.baseline, self.scratch = (root / name for name in ("catalog", "baseline", "scratch"))
        self.baseline.mkdir()
        self.scratch.mkdir()
        self.start_version = "2026091112345678"
        self.calls, self.texts = [], {"tasks": TASKS, "hud": HUD, "redPoints": RED}
        self.rule_texts = official_rule_texts()

    def package_bytes(self, label):
        payload = self.texts[label] if label not in rules.SPECS else json.dumps(
            {cls: self.rule_texts[cls] for cls in rules.SPECS[label][2]}, sort_keys=True)
        return b"SYNTHETIC PACKAGE\n" + payload.encode()

    def resource(self, key, versions):
        label = next(label for label, resource, _ in routines.RESOURCES if resource == key)
        self.calls.append(label)
        path = self.scratch / (label + ".swf")
        path.write_bytes(self.package_bytes(label))
        return path, {"version": versions[key], "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                      "url": "https://aoqi.100bt.com/play/" + key + "~" + versions[key] + ".swf"}

    def export(self, swf, directory, selected):
        directory.mkdir(parents=True, exist_ok=True)
        result = directory / (selected.rsplit(".", 1)[-1] + ".as")
        result.write_text(self.rule_texts[selected] if swf.stem in rules.SPECS else self.texts[swf.stem], encoding="utf-8")
        return result


class PublicRoutineUpdaterTests(unittest.TestCase):
    def setUp(self):
        blocked = mock.patch("urllib.request.urlopen", side_effect=AssertionError("No real network"))
        blocked.start()
        self.addCleanup(blocked.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.updater = FakeUpdater(Path(self.temp.name))
        self.versions = {key: "2026091112345678" for _, key, _ in routines.RESOURCES}
        self.path = self.updater.catalog / routines.FILENAME
        self.stdout = contextlib.redirect_stdout(io.StringIO())
        self.stdout.__enter__()
        self.addCleanup(self.stdout.__exit__, None, None, None)

    def update(self):
        return routines.update_routines(self.updater, self.versions)

    def test_static_task_values_and_red_graph_keep_official_meaning(self):
        self.assertTrue(self.update())
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertTrue(routines.catalog_valid(catalog))
        self.assertEqual(catalog["tasks"][0]["name"], "活动, (一)")
        self.assertEqual(catalog["tasks"][1]["weekDailyMax"], 2**31 - 1)
        self.assertEqual(len(catalog["activities"]), 2)
        first = catalog["activities"][0]
        self.assertEqual(first["redPointIds"], [100, 101, 102])
        self.assertEqual(catalog["source"]["counts"], {"tasks": 2, "activities": 2, "redPointNodes": 3, "unsupportedRules": 0})

    def test_unchanged_resource_versions_do_not_download_or_rewrite(self):
        self.update()
        original = self.path.read_bytes()
        self.updater.calls.clear()
        self.assertFalse(self.update())
        self.assertEqual(self.updater.calls, [])
        self.assertEqual(self.path.read_bytes(), original)

    def versions_with_current_hashes(self):
        class Versions(dict):
            pass
        versions = Versions(self.versions)
        versions.source_hashes = {key: hashlib.sha256(self.updater.package_bytes(label)).hexdigest()
                                  for label, key, _ in routines.RESOURCES}
        return versions

    def test_same_version_changed_hud_hash_refreshes_content_then_reuses_new_cache(self):
        self.versions = self.versions_with_current_hashes()
        self.update()
        original_versions = dict(self.versions)
        self.updater.calls.clear()
        self.assertFalse(self.update())
        self.assertEqual(self.updater.calls, [])
        self.updater.texts["hud"] = HUD.replace("old-activity", "same-version-revised-activity")
        self.versions = self.versions_with_current_hashes()
        self.assertTrue(self.update())
        self.assertEqual(dict(self.versions), original_versions)
        self.assertEqual(self.updater.calls, ["hud"])
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(catalog["activities"][0]["key"], "same-version-revised-activity")
        self.assertEqual(catalog["source"]["resources"]["hud"]["sha256"], self.versions.source_hashes["config/config"])
        saved = self.path.read_bytes()
        self.updater.calls.clear()
        self.assertFalse(self.update())
        self.assertEqual(self.updater.calls, [])
        self.assertEqual(self.path.read_bytes(), saved)

    def test_same_version_changed_hash_with_invalid_rules_preserves_published_catalog(self):
        self.versions = self.versions_with_current_hashes()
        self.update()
        original = self.path.read_bytes()
        self.updater.texts["tasks"] = TASKS.replace("1,15,15,3,15", "1,15,getWeekTarget(),3,15")
        self.versions = self.versions_with_current_hashes()
        self.updater.calls.clear()
        with self.assertRaisesRegex(ValueError, "尚不支持"):
            self.update()
        self.assertEqual(self.updater.calls, ["tasks"])
        self.assertEqual(self.path.read_bytes(), original)

    def test_only_changed_hud_is_downloaded_and_new_activities_are_discovered(self):
        self.update()
        self.updater.calls.clear()
        self.versions["config/config"] = "2026091812345678"
        self.updater.texts["hud"] = HUD.replace("old-activity", "new-activity").replace("20260911", "20260918")
        self.assertTrue(self.update())
        self.assertEqual(self.updater.calls, ["hud"])
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(catalog["activities"][0]["key"], "new-activity")
        self.assertEqual(catalog["activities"][0]["redPointIds"], [100, 101, 102])
        self.assertEqual(catalog["source"]["resources"]["hud"]["version"], self.versions["config/config"])

    def test_only_changed_red_graph_recomputes_links_for_saved_hud(self):
        self.update()
        self.updater.calls.clear()
        self.versions["library/gameconst"] = "2026091812345678"
        self.updater.texts["redPoints"] = RED.replace("[100,102]", "[100,103]")
        self.assertTrue(self.update())
        self.assertEqual(self.updater.calls, ["redPoints"])
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(catalog["activities"][0]["redPointIds"], [100, 101, 103])

    def test_unrecognized_task_formula_preserves_old_published_catalog(self):
        self.update()
        original = self.path.read_bytes()
        self.versions["dailytask/dailytaskservice"] = "2026091812345678"
        self.updater.texts["tasks"] = TASKS.replace("1,15,15,3,15", "1,15,getWeekTarget(),3,15")
        with self.assertRaisesRegex(ValueError, "尚不支持"):
            self.update()
        self.assertEqual(self.path.read_bytes(), original)

    def test_unrecognized_activity_value_is_reported_without_executing_it(self):
        self.updater.texts["hud"] = HUD.replace('"redPointId":100', '"redPointId":"runDynamicCode()"')
        self.assertTrue(self.update())
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(len(catalog["activities"]), 1)
        self.assertEqual(catalog["unsupportedRules"][0]["key"], "old-activity")
        self.assertEqual(catalog["source"]["counts"]["unsupportedRules"], 1)

    def test_bad_resource_or_duplicate_identifier_never_replaces_previous_catalog(self):
        self.update()
        original = self.path.read_bytes()
        self.versions["library/gameconst"] = "2026091812345678"
        self.updater.texts["redPoints"] += "new RedPointConfigNode(100);"
        with self.assertRaisesRegex(ValueError, "重复"):
            self.update()
        self.assertEqual(self.path.read_bytes(), original)
        self.versions.pop("config/config")
        with self.assertRaisesRegex(ValueError, "清单缺少"):
            self.update()
        self.assertEqual(self.path.read_bytes(), original)

    def test_corrupt_local_metadata_is_rebuilt_without_trusting_bad_source_types(self):
        self.updater.catalog.mkdir()
        self.path.write_text('{"schema":1,"source":[]}', encoding="utf-8")
        self.assertTrue(self.update())
        self.assertEqual(self.updater.calls, [label for label, _, _ in routines.RESOURCES])
        self.assertTrue(routines.catalog_valid(json.loads(self.path.read_text(encoding="utf-8"))))

    def test_real_rule_fixtures_are_byte_bound_to_official_exports(self):
        for selected, record in RULE_MANIFEST["files"].items():
            raw = (RULE_FIXTURES / record["file"]).read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), record["sha256"], selected)
        self.update()
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(catalog["opportunityRules"]["starAdventure"], {"dailyDeclaredLimit": 3, "weeklyLimit": 6})
        self.assertEqual(catalog["opportunityRules"]["nationalCompetition"], {"dailyBase": 40, "weeklyDeclaredLimit": 40})
        self.assertEqual(catalog["opportunityRules"]["arena"]["zao1"], {"fieldId": 8, "dailyBase": 8})
        self.assertEqual([(r["dailyRefresh"], r["refreshMax"]) for r in catalog["opportunityRules"]["farm"]["inviteBonus"]],
                         [(4, 16), (4, 16), (4, 16), (4, 20), (5, 20)])
        self.assertEqual(catalog["source"]["rulesStatus"], rules.STATUSES)

    def test_synthetic_changed_numeric_declaration_updates_actual_rule_with_same_version(self):
        self.versions = self.versions_with_current_hashes()
        self.update()
        for selected, before, after in (
                (rules.STAR + "config.SWA_Config", "WEEKLY_GAME_TIMES:int = 6", "WEEKLY_GAME_TIMES:int = 9"),
                (rules.COMPETITION + "config.NC_Config", "DAILY_MAX_TICKET:int = 40", "DAILY_MAX_TICKET:int = 43"),
                (rules.FARM + "model.FV2Config", '"refreshMax":16', '"refreshMax":18'),
                (rules.ARENA + "config.ArenaV3BattleFieldConfig", "return 8;", "return 10;")):
            self.assertIn(before, self.updater.rule_texts[selected])
            self.updater.rule_texts[selected] = self.updater.rule_texts[selected].replace(before, after)
        self.versions = self.versions_with_current_hashes()
        self.updater.calls.clear()
        self.assertTrue(self.update())
        self.assertEqual(self.updater.calls, list(rules.SPECS))
        values = json.loads(self.path.read_text(encoding="utf-8"))["opportunityRules"]
        self.assertEqual(values["starAdventure"]["weeklyLimit"], 9)
        self.assertEqual(values["nationalCompetition"]["dailyBase"], 43)
        self.assertEqual(values["farm"]["inviteBonus"][0]["refreshMax"], 18)
        self.assertEqual(values["arena"]["zao2"]["dailyBase"], 10)

    def test_synthetic_consumer_change_or_dynamic_number_preserves_whole_catalog(self):
        self.versions = self.versions_with_current_hashes()
        self.update()
        original = self.path.read_bytes()
        changes = [
            (rules.STAR + "model.SWA_Model", 'param1["wgt"]', 'param1["ti"]'),
            (rules.COMPETITION + "panel.NC_MainPanel", '+ model.buyDailyTicketTimes - model.dailyTicketNum', '- model.buyDailyTicketTimes - model.dailyTicketNum'),
            (rules.FARM + "model.FV2Client", 'param1["rt"] = param1["rft"]', 'param1["rt"] = param1["pt"]'),
            (rules.FARM + "model.FV2Config", 'Math.min(param1,INVITE_BONUS.length - 1)', 'Math.max(param1,INVITE_BONUS.length - 1)'),
            (rules.ARENA + "config.ArenaV3BattleFieldConfig", 'return 8;', 'return isWeekendDay ? 9 : 8;'),
            (rules.ARENA + "config.ArenaV3BattleFieldConfig", 'JING_YING_BATTLE_FIELDS[1] = LUAN_DOU__BATTLE_FIELD;', 'JING_YING_BATTLE_FIELDS[1] = JING_DIAN_BATTLE_FIELD;'),
        ]
        for selected, before, after in changes:
            with self.subTest(selected=selected, before=before):
                self.updater.rule_texts = official_rule_texts()
                self.assertIn(before, self.updater.rule_texts[selected])
                self.updater.rule_texts[selected] = self.updater.rule_texts[selected].replace(before, after)
                self.versions = self.versions_with_current_hashes()
                with self.assertRaises(ValueError):
                    self.update()
                self.assertEqual(self.path.read_bytes(), original)

    def test_cached_rules_cannot_be_edited_without_invalidating_source_binding(self):
        self.update()
        catalog = json.loads(self.path.read_text(encoding="utf-8"))
        catalog["opportunityRules"]["nationalCompetition"]["dailyBase"] += 1
        self.assertFalse(routines.catalog_valid(catalog))
        catalog["source"]["resources"]["competitionRules"]["rulesSha256"] = rules.digest(catalog["opportunityRules"]["nationalCompetition"])
        catalog["source"]["resources"]["competitionRules"]["contracts"].clear()
        self.assertFalse(routines.catalog_valid(catalog))

    def test_missing_rule_package_keeps_legacy_catalog_bytes_for_offline_readers(self):
        self.updater.catalog.mkdir()
        legacy = b'{"schema":1,"tasks":[{"id":1}],"note":"legacy local catalog"}'
        self.path.write_bytes(legacy)
        self.versions.pop(rules.SPECS["farmRules"][0])
        with self.assertRaisesRegex(ValueError, "清单缺少"):
            self.update()
        self.assertEqual(self.path.read_bytes(), legacy)


if __name__ == "__main__":
    unittest.main()
