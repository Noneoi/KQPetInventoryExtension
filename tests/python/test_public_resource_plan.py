"""Offline resource-union contracts; no URL in a fixture is downloaded."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import public_resource_plan as subject


class ResourcePlanTests(unittest.TestCase):
    def setUp(self):
        blocked = mock.patch("urllib.request.urlopen", side_effect=AssertionError("No real network in resource planning"))
        blocked.start()
        self.addCleanup(blocked.stop)

    @staticmethod
    def row(kind, resource, component, reason, **extra):
        return dict(kind=kind, resource=resource, components=[component], reason=reason, **extra)

    @staticmethod
    def keyed(rows):
        return {(row["kind"], row["resource"]): row for row in rows}

    def test_fixed_workbench_plan_keeps_all_existing_components_and_special_formats(self):
        plan = subject.workbench_dependencies()
        rows = self.keyed(plan)
        self.assertEqual(len(rows), len(plan))
        for resource in ("dailytask/dailytaskservice", "equipment4petver2/equipment4petver2service",
                         "library/gameconst", "library/materialdata", "library/materialdataupdate",
                         "material/materialservice", "sacredequipment/sacredequipmentservice",
                         "strengthencombo/strengthencomboservice", "multipleformpet/multipleformpetservice",
                         "changepetskinv3/changepetskinservice", "legend/legendservice", "proficient/proficient"):
            self.assertIn(("flash", resource), rows)
        self.assertTrue(rows["flash", "library/util"]["versionOnly"])
        self.assertFalse(rows["flash", "library/interfaces"]["versionOnly"])
        self.assertEqual(rows["flash", "common/icon/attributeicon@res"]["formats"], ["fui"])
        self.assertEqual(len([row for row in plan if row["kind"] == "h5"]), 10)
        self.assertEqual(set(rows["flash", "pet/petdataservice"]["components"]), {"images", "shop"})
        self.assertEqual(set(rows["flash", "config/config"]["components"]), {"routines", "shop"})

    def test_union_retains_unique_dependencies_and_all_consumers(self):
        workbench = [self.row("flash", "library/interfaces", "pets", "pet fields", versionOnly=True),
                     self.row("h5", "config/equipment4pet.aqz", "skills", "source beasts")]
        replay = [self.row("flash", "library/interfaces", "battleReplay", "battle fields"),
                  self.row("flash", "battle/battle", "battleReplay", "playback")]
        before = copy.deepcopy((workbench, replay))
        plan = subject.build_plan(workbench, replay)
        self.assertEqual(len(plan), 3)
        shared = self.keyed(plan)["flash", "library/interfaces"]
        self.assertEqual(shared["components"], ["battleReplay", "pets"])
        self.assertEqual(shared["reasons"], ["battle fields", "pet fields"])
        self.assertFalse(shared["versionOnly"])
        self.assertEqual((workbench, replay), before)
        self.assertEqual(subject.build_plan(replay, workbench), plan)
        self.assertEqual(subject.build_plan(plan, plan), plan)

    def test_historical_records_never_become_current_approvals(self):
        old = self.row("flash", "battle/battle", "battleReplay", "recorded source",
                       sourceStatus="verified", evidence=[{"sha256": "a" * 64, "status": "current", "version": "2000"}])
        before = copy.deepcopy(old)
        plan = subject.build_plan([], [], [old])
        self.assertEqual(plan[0]["sourceStatuses"], ["historical-evidence"])
        self.assertEqual(plan[0]["evidence"][0]["status"], "historical-evidence")
        self.assertEqual(plan[0]["evidence"][0]["sha256"], "a" * 64)
        self.assertEqual(old, before)

    def test_recorded_urls_and_explicit_dynamic_keys_survive_without_guessing(self):
        value = {"source": {"resources": {
            "petDictionary": {"url": "https://aoqi.100bt.com/play/pet/petdictionarydata~202609300001.swf", "sha256": "b" * 64},
            "config/petdictionarydata.json.aqz": {"version": "202609300002"}}},
            "modules": {"newactivityext/newact20260930/test/test": {"version": "202609300003", "panelPeriods": [
                {"key": "newactivityext/newact20260930/test/test_main", "version": "202609300004"}]}},
            "picture": {"swfUrl": "https://aoqi.100bt.com/play/peticon/peticon123~2000.swf"},
            "js": {"url": "https://aoqi.100bt.com/h5/js/services/battleconfig~202609300001.js"},
            "unrelated": {"url": "https://other.invalid/play/fake/pkg~2000.swf", "file": "C:/local/snapshot.as"},
            "description": "https://aoqi.100bt.com/play/not/a/dependency~2000.swf"}
        rows = self.keyed(subject.recorded_dependencies(value, "shop"))
        self.assertEqual(len(rows), 6)
        self.assertIn(("flash", "newactivityext/newact20260930/test/test"), rows)
        self.assertIn(("flash", "peticon/peticon123"), rows)
        self.assertIn(("h5", "config/petdictionarydata.json.aqz"), rows)
        self.assertEqual(rows["flash", "newactivityext/newact20260930/test/test_main"]["formats"], ["fui"])
        self.assertTrue(all(row["sourceStatuses"] == ["recorded-not-revalidated"] for row in rows.values()))
        proof = rows["flash", "pet/petdictionarydata"]["evidence"][0]
        self.assertEqual(proof["version"], "202609300001")
        self.assertEqual(proof["sha256"], "b" * 64)

    def test_recorded_urls_preserve_fui_and_do_not_confuse_flash_h5_namespaces(self):
        rows = subject.recorded_dependencies({"files": [
            {"url": "https://aoqi.100bt.com/play/common/icon/attributeicon@res~202609300001.fui"},
            {"url": "https://aoqi.100bt.com/h5/config/battleconfig~202609300001.aqz"},
            {"url": "https://aoqi.100bt.com/play/battleconfig/battleconfig~202609300001.swf"}]}, "all")
        self.assertEqual(len(rows), 3)
        self.assertEqual(self.keyed(rows)["flash", "common/icon/attributeicon@res"]["formats"], ["fui"])

    def test_bad_identities_do_not_enter_the_plan(self):
        for kind, resource in (("flash", "../escape"), ("flash", "C:/local"), ("flash", "x\\y"),
                               ("flash", "pkg/file.swf"), ("h5", "config/../file.json"), ("unknown", "a/b")):
            with self.subTest(resource=resource), self.assertRaises(ValueError):
                subject.build_plan([self.row(kind, resource, "test", "invalid")], [])
        for url in ("http://aoqi.100bt.com/play/a/b~2000.swf", "https://evil@aoqi.100bt.com/play/a/b~2000.swf",
                    "https://aoqi.100bt.com.evil/play/a/b~2000.swf", "https://aoqi.100bt.com/play/../b~2000.swf"):
            self.assertEqual(subject.recorded_dependencies({"url": url}, "test"), [])

    def test_current_bundle_and_workbench_union_cannot_drop_workbench_only_sources(self):
        archive = ROOT / "assets/battle-replay-update-sources.zip"
        if not archive.is_file():
            self.skipTest("Optional full reviewed replay bundle is absent")
        replay = []
        with zipfile.ZipFile(archive) as source:
            for name in source.namelist():
                if name.endswith(".json"):
                    replay.extend(subject.recorded_dependencies(json.loads(source.read(name)), "battleReplay"))
        plan = self.keyed(subject.build_plan(subject.workbench_dependencies(), [], replay))
        self.assertIn(("flash", "battle/battle"), plan)
        self.assertIn(("flash", "dailytask/dailytaskservice"), plan)
        self.assertIn(("flash", "sacredequipment/sacredequipmentservice"), plan)
        self.assertIn(("flash", "proficient/proficient"), plan)
        self.assertIn(("h5", "config/equipment4pet.aqz"), plan)
        self.assertIn(("h5", "config/stargod/stargodconfig.json"), plan)
        self.assertIn("historical-evidence", plan["flash", "battle/battle"]["sourceStatuses"])
        self.assertIn("pets", plan["flash", "library/interfaces"]["components"])
        self.assertIn("battleReplay", plan["flash", "library/interfaces"]["components"])


if __name__ == "__main__":
    unittest.main()
