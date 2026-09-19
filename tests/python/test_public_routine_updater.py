"""Focused static-parser/version-cache tests; no account or network access."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_routine_updater as routines


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

    def resource(self, key, versions):
        label = next(label for label, resource, _ in routines.RESOURCES if resource == key)
        self.calls.append(label)
        path = self.scratch / (label + ".swf")
        path.write_bytes(b"static fixture")
        return path, {"version": versions[key], "url": "https://aoqi.100bt.com/play/" + key + "~" + versions[key] + ".swf"}

    def export(self, swf, directory, selected):
        directory.mkdir(parents=True, exist_ok=True)
        result = directory / (selected.rsplit(".", 1)[-1] + ".as")
        result.write_text(self.texts[swf.stem], encoding="utf-8")
        return result


class PublicRoutineUpdaterTests(unittest.TestCase):
    def setUp(self):
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
        self.assertEqual(self.updater.calls, ["tasks", "hud", "redPoints"])
        self.assertTrue(routines.catalog_valid(json.loads(self.path.read_text(encoding="utf-8"))))


if __name__ == "__main__":
    unittest.main()
