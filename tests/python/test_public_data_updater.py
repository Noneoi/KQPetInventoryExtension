"""Updater contract tests use isolated files and fake official transport only."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_data_updater as updater
import public_resource_session as resources
from generate_pet_detail_data import parse_pets


def version_swf(versions):
    """Minimal valid SWF with the same named binary XML as official versiondata."""
    def tag(code, payload):
        if len(payload) < 63:
            return struct.pack("<H", (code << 6) | len(payload)) + payload
        return struct.pack("<HI", (code << 6) | 63, len(payload)) + payload
    xml = ('<r>' + ''.join(f'<f n="{key}" v="{value}"/>' for key, value in versions.items()) + '</r>').encode()
    symbols = struct.pack("<HH", 1, 1) + b"mmo.versiondata.DocClass_cls\0"
    binary = struct.pack("<HI", 1, 0) + xml
    body = b"\0" * 5 + tag(76, symbols) + tag(87, binary) + b"\0\0"
    return b"FWS\x0a" + struct.pack("<I", len(body) + 8) + body


class PublicDataUpdateTests(unittest.TestCase):
    def setUp(self):
        blocked = patch("urllib.request.urlopen", side_effect=AssertionError("No real network in updater contract tests"))
        blocked.start()
        self.addCleanup(blocked.stop)
        # Supplement extraction and completeness have a separate fixture suite;
        # these pre-existing dispatch fixtures do not stand for those tables.
        supplement = patch.object(updater, "needs_supplement", return_value=False)
        supplement.start()
        self.addCleanup(supplement.stop)
        loader = patch("public_icon_updater.loader_default", return_value={"defaultVersion": "2000", "version": "2026091012345678"})
        loader.start()
        self.addCleanup(loader.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.baseline = self.root / "baseline"
        self.baseline.mkdir()
        self.scratch = self.root / "scratch"
        self.scratch.mkdir()
        self.calls = []
        self.version = "2026091011614836"
        self.entry_version = "2026091012345678"
        self.start_overrides = {}
        self.versions = {"dummy/" + str(n): self.version for n in range(120)}
        self.versions.update({"pet/petdictionarydata": self.version, "pet/petdictionarydataupdate": self.version,
                              updater.RESOURCE: self.version, "peticon/peticon7529": self.version})
        self.versions.update({key: self.version for _, key in updater.POWER_RESOURCES})
        self.versions.update({key: self.version for _, key in updater.CULTIVATION_RESOURCES})
        self.versions.update({key: self.version for _, key in updater.NAME_RESOURCES})
        self.versions[updater.SACRED_SOURCE_RESOURCE[1]] = self.version
        pets = {str(n): {"name": "test", "astrolabeBreakCosts":"", "sign":""} for n in range(9000)}
        source = {"resources": {"petDictionary": {"version": self.version}, "petDictionaryUpdate": {"version": self.version}}}
        source["resources"].update({label: {"version": self.version} for label, _ in updater.POWER_RESOURCES})
        source["resources"].update({label: {"version": self.version} for label, _ in updater.CULTIVATION_RESOURCES})
        source["resources"].update({label: {"version": self.version} for label, _ in updater.NAME_RESOURCES})
        source["resources"][updater.SACRED_SOURCE_RESOURCE[0]] = {"version": self.version}
        self.stars = {str(n): {"name": "star", "type": n, "quality": 6, "limitJobs": [], "limited": False,
                                "changeable": n in (24, 25), "battlePower": {"1": 140}} for n in range(1, 31)}
        self.astrolabe = {str(n): {"name": "node", "locatedType": n % 7, "battlePower": 210, "lightUpMaterials": [],"isTBD":False} for n in range(30)}
        self.badges = {str(n): {"name": "badge", "type": 1, "maxLevel": 1, "levels": {},
                                 "activationCost": [{"type": 4, "id": 1450, "count": 1}]} for n in range(1, 31)}
        self.sacred = {str(n): {"name": "sacred", "sourceId": n, "sourceName": "source"} for n in range(1, 31)}
        self.plans = {"1": {"maxLevel": 1, "levels": {"1": {"battlePower": 300, "cost": [], "equipmentCount": 0}}}}
        self.cultivation = {"cultivationRuleVersion": updater.CULTIVATION_RULE_VERSION, "badges": self.badges,
                            "sacredEquipment": self.sacred, "sacredStarPlans": self.plans, "sacredStagePlans": self.plans,
                            "items": {"1450": {"name": "解神元魂"}}}
        updater.atomic_json(self.baseline / "pet-detail-data.json", {"pets": pets, "source": source, "untouchedSheet": {"x": 1},
                            "petDictionarySchema":5,
                            "nameRuleVersion": updater.NAME_RULE_VERSION, "attributes": {"1":"普"}, "jobs":{"1":"利爪"},
                            "fusionJobs":[], "money":{"1":{"name":"金币"}},
                            "powerRuleVersion": updater.POWER_RULE_VERSION, "stargods": self.stars, "astrolabe": self.astrolabe,
                            **self.cultivation})
        updater.atomic_json(self.baseline / "pet-skill-data.json",
                            {"schema": 1, "source": {"version": self.version},
                             "pets": {"1": {"name": "pet", "slots": {"normal": 100001}}},
                             "skills": {"100001": {"name": "skill", "description": "desc"}},
                             "entries": {"term": "desc"}, "buffs": {"1": {"name": "buff"}}})
        package_versions = {key: self.version for key in ("library/materialdata", "library/materialdataupdate")}
        packages = patch("public_activity_exchange_updater.load_selectable_packages",
                         return_value={"versions": package_versions, "packages": {}})
        packages.start()
        self.addCleanup(packages.stop)
        updater.atomic_json(self.baseline / "shop-exchange-data.json", {"shops": [{"goods": [{"id": 1,"enhanceType":"92","raceIds":[1]}]}], "source": {
            "shopVersion": self.version, "shopParserVersion": 5, "resources": {"shop": {"version": self.version}},
            "selectablePackageVersions": package_versions}})
        self.subject = updater.Updater(self.root, self.baseline, self.scratch, fetcher=self.fetch, isolated_workspace=True)
        # The actual initial union, portable replay preparation and dependency
        # coverage are exercised by resource-plan and end-to-end tests. This
        # suite controls component implementations and tests their dispatch.
        self.subject.prepare_resource_plan = Mock(return_value={})
        self.replay_calls = 0
        # This suite tests the public dispatch contract. The replay builder and
        # package format have their own tests; treat its installed file as opaque.
        self.replay_bytes = b"SYNTHETIC TEST-ONLY replay package sentinel"
        self.subject.battle_replay_result = {"testOnly": True, "counts": {"skills": 1, "buffs": 1}}
        def battle_replay(_versions):
            self.replay_calls += 1
            target = self.root / "catalog" / updater.COMPONENTS["battleReplay"]
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                target.write_bytes(self.replay_bytes)
            return False
        self.subject.battleReplay = battle_replay
        self.subject.icon_exceptions = lambda versions, current: ([], {"version": self.version})
        self.subject.icons = lambda versions: False
        self.subject.routines = lambda versions: False
        def skills(_versions):
            current = self.subject.current("skills")
            target = self.root / "catalog/pet-skill-data.json"
            if updater.read_object(target) != current:
                updater.atomic_json(target, current)
            return False
        self.subject.skills = skills
        self.subject.activity_exchanges = lambda versions: False
        self.quiet = contextlib.redirect_stdout(io.StringIO())
        self.quiet.__enter__()
        self.addCleanup(self.quiet.__exit__, None, None, None)

    def fetch(self, url, path):
        self.calls.append(url)
        if url.split("?", 1)[0].endswith("start.xml"):
            overrides = ''.join(f'<f n="{key}" v="{value}"/>' for key, value in self.start_overrides.items())
            path.write_text(f'<r><v>{self.entry_version}</v><lv>200001010000</lv><u>{overrides}</u></r>', encoding="utf-8")
        elif "versiondata~" in url:
            path.write_bytes(version_swf(self.versions))
        else:
            raise OSError("synthetic resource unavailable")
        return {"url": url, "sha256": updater.digest(path)}

    def test_each_manual_check_refreshes_start_v_manifest_without_component_download(self):
        with patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            first = self.subject.run()
            self.assertTrue(all(v["status"] != "failed" for v in first.values()))
            self.assertEqual(len(self.calls), 2)
            saved = (self.root / "catalog/pet-detail-data.json").read_bytes()
            self.calls.clear()
            second = self.subject.run()
        self.assertEqual(self.calls, [updater.OFFICIAL + "start.xml",
                                     updater.OFFICIAL + f"versiondata~{self.entry_version}.swf"])
        self.assertTrue(all(v["status"] == "unchanged" for v in second.values()))
        self.assertEqual(saved, (self.root / "catalog/pet-detail-data.json").read_bytes())
        # Replay stays binary; the extra JSON is the resource union audit.
        self.assertEqual(len(list((self.root / "catalog").glob("*.json"))), 7)
        self.assertEqual(set(second), {"pets","skills","shop","images","icons","routines","battleReplay"})
        self.assertEqual(self.replay_calls, 2)
        self.assertEqual((self.root / "catalog/battle-replay-catalog.brpack").read_bytes(), self.replay_bytes)
        self.assertEqual(second["battleReplay"]["counts"], {"skills": 1, "buffs": 1})
        self.assertTrue(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])
        versions = updater.read_object(self.root / "catalog/official-versions.json")
        self.assertEqual(versions["startVersion"], self.entry_version)
        self.assertEqual(versions["versionRule"], updater.VERSION_RULE)
        audit = updater.read_object(self.root / "catalog/public-resource-manifest.json")
        self.assertEqual(audit["state"], "finished")
        self.assertEqual(audit["components"], second)
        self.assertEqual(audit["preparationErrors"], {})

    def test_shop_parser_upgrade_rechecks_unchanged_official_version(self):
        baseline = self.baseline / "shop-exchange-data.json"
        previous = updater.read_object(baseline)
        previous["source"].pop("shopParserVersion")
        updater.atomic_json(baseline, previous)
        source = self.scratch / "SEFConfig.as"
        source.write_text('public static const SERVER_ID_9_REWARD_CONFIG:Array=[{"id":1,"serverId":1,'
            '"simpleParams":"Choice,|PetOpen,7115|PetOpen,6964","cost":"4:77:1","shelfTime":"20260101"},'
            '{"id":2,"serverId":2,"simpleParams":"CommonEnhancePrize,-1,1,92,7115","cost":"4:77:1","shelfTime":"20260101"}];', encoding="utf-8")
        selection = self.root / "catalog/shop-selection.json"
        updater.atomic_json(selection, {"schema": 1, "selected": ["saved-choice"]})
        before = selection.read_bytes()
        with patch.object(self.subject, "resource", return_value=(source, {"version": self.version})) as download, \
                patch.object(self.subject, "export", return_value=source):
            self.assertTrue(self.subject.shop(self.versions))
            self.assertFalse(self.subject.shop(self.versions))
            self.assertEqual(download.call_count, 1)
        current = self.subject.current("shop")
        self.assertEqual(len(current["shops"][0]["goods"]),1)
        self.assertFalse(current["shops"][0]["goods"][0]["manualSelectionRequired"])
        self.assertEqual(selection.read_bytes(), before)

    def test_selected_components_run_alone_and_keep_other_status(self):
        with patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            self.subject.run()
            self.calls.clear()
            original_images = self.subject.images
            self.subject.images = lambda versions: self.fail("images ran although only shop was selected")
            result = self.subject.run({"shop"})
            self.subject.images = original_images
        self.assertEqual(set(result), {"shop"})
        status = updater.read_object(self.root / "catalog/public-update-status.json")
        self.assertEqual(status["checked"], ["shop"])
        self.assertEqual(set(status["components"]), {"pets", "skills", "shop", "images", "icons", "routines", "battleReplay"})
        self.assertEqual(self.replay_calls, 1)
        self.assertTrue(status["complete"])
        with self.assertRaises(ValueError):
            self.subject.run(set())

    def test_partial_first_run_is_not_reported_complete(self):
        with patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            self.subject.run({"pets"})
        self.assertFalse(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])

    def test_structured_phases_and_counts_follow_actual_dispatch(self):
        stages = []
        self.subject.progress_callback = lambda phase, component, completed, total: stages.append((phase, component, completed, total))
        with patch.object(updater, 'runtime_entry', return_value=Path('synthetic-runtime')):
            result = self.subject.run({'pets', 'routines'})
        self.assertEqual(set(result), {'pets', 'routines'})
        self.assertEqual(stages[0], ('fetchingResources', '', -1, -1))
        self.assertIn(('parsing', 'pets', -1, -1), stages)
        self.assertIn(('validating', 'pets', -1, -1), stages)
        self.assertIn(('parsing', '', 1, 2), stages)
        self.assertEqual(stages[-1], ('parsing', '', 2, 2))

    def test_failed_component_preserves_old_file_other_components_finish(self):
        target = self.root / "catalog/shop-exchange-data.json"
        target.parent.mkdir()
        original = (self.baseline / target.name).read_bytes()
        target.write_bytes(original)
        self.versions[updater.RESOURCE] = "2026091212345678"
        with patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            result = self.subject.run()
        self.assertEqual(result["shop"]["status"], "failed")
        self.assertEqual(target.read_bytes(), original)
        self.assertNotEqual(result["pets"]["status"], "failed")
        self.assertNotEqual(result["images"]["status"], "failed")
        self.assertEqual(result["battleReplay"]["status"], "unchanged")
        self.assertEqual(self.replay_calls, 1)

    def test_replay_only_uses_its_own_updater_without_generic_versions(self):
        # Restore the real dispatch method, but supply an isolated optional
        # module so this test cannot enter the official download/build pipeline.
        del self.subject.battleReplay
        metadata = {"testOnly": True, "counts": {"pets": 2, "skills": 3, "buffs": 4}}
        update_replay = Mock(side_effect=[(True, metadata), (False, metadata)])
        module = types.ModuleType("public_battle_replay_updater")
        module.update_battle_replay = update_replay
        with patch.dict(sys.modules, {"public_battle_replay_updater": module}), \
                patch.object(self.subject, "versions", side_effect=AssertionError("generic versions must not run")) as versions, \
                patch.object(updater, "runtime_entry", side_effect=AssertionError("runtime preparation must not run")):
            changed = self.subject.run({"battleReplay"})
            unchanged = self.subject.run({"battleReplay"})
        versions.assert_not_called()
        self.assertEqual(self.calls, [])
        self.assertEqual(update_replay.call_count, 2)
        self.assertTrue(all(call.args == (self.subject,) for call in update_replay.call_args_list))
        self.assertEqual(changed, {"battleReplay": {"status": "updated", **metadata}})
        self.assertEqual(unchanged, {"battleReplay": {"status": "unchanged", **metadata}})
        status = updater.read_object(self.root / "catalog/public-update-status.json")
        self.assertEqual(status["checked"], ["battleReplay"])
        self.assertEqual(status["components"], unchanged)
        self.assertFalse(status["complete"])

    def test_flash_version_failure_does_not_block_independent_components(self):
        with patch.object(self.subject, "versions", side_effect=ValueError("synthetic generic manifest failure")):
            result = self.subject.run({"pets", "skills", "battleReplay"})
        self.assertEqual(result["pets"]["status"], "failed")
        self.assertEqual(result["skills"]["status"], "unchanged")
        self.assertEqual(result["battleReplay"]["status"], "unchanged")
        self.assertEqual(self.replay_calls, 1)
        self.assertFalse(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])

    def test_replay_preparation_failure_keeps_package_and_allows_workbench_dispatch(self):
        target = self.root / "catalog/battle-replay-catalog.brpack"
        target.parent.mkdir()
        target.write_bytes(self.replay_bytes)
        self.subject.prepare_resource_plan.return_value = {"battleReplay": "synthetic missing replay source"}
        with patch.object(self.subject, "battleReplay", side_effect=AssertionError("unprepared replay ran")) as replay:
            result = self.subject.run({"pets", "battleReplay"})
        replay.assert_not_called()
        self.assertEqual(result["pets"]["status"], "unchanged")
        self.assertEqual(result["battleReplay"], {"status": "failed", "error": "synthetic missing replay source", "retryable": True})
        self.assertEqual(target.read_bytes(), self.replay_bytes)
        audit = updater.read_object(self.root / "catalog/public-resource-manifest.json")
        self.assertEqual(audit["components"], result)
        self.assertEqual(audit["preparationErrors"], {"battleReplay": "synthetic missing replay source"})

    def test_component_parse_failure_does_not_poison_shared_official_snapshot_or_download(self):
        key = "library/interfaces"
        override = "2026091212345678"
        self.start_overrides[key] = override
        url = updater.OFFICIAL + f"{key}~{override}.swf"
        payload = b"SYNTHETIC TEST-ONLY shared official resource"
        target = self.root / "catalog/pet-detail-data.json"
        target.parent.mkdir()
        original = (self.baseline / target.name).read_bytes()
        target.write_bytes(original)
        snapshots = []
        def transport(request, output):
            if request != url:
                return self.fetch(request, output)
            self.calls.append(request)
            output.write_bytes(payload)
            return {"url": request, "sha256": updater.digest(output)}
        self.subject.public_resources.transport = transport
        def pets(versions):
            snapshots.append(dict(versions))
            path, _ = self.subject.resource(key, versions)
            self.assertEqual(path.read_bytes(), payload)
            raise ValueError("synthetic pet parser failure after verified download")
        def replay(_versions):
            versions, _, _ = self.subject.public_resources.flash_chain()
            snapshots.append(versions)
            path = self.scratch / "replay-consumer.swf"
            self.subject.public_resources.fetch(url, path)
            self.assertEqual(path.read_bytes(), payload)
            return False
        self.subject.pets, self.subject.battleReplay = pets, replay
        for run in range(2):
            result = self.subject.run({"pets", "battleReplay"})
            self.assertEqual(result["pets"]["status"], "failed")
            self.assertEqual(result["battleReplay"]["status"], "unchanged")
            self.assertEqual(target.read_bytes(), original)
            self.assertEqual(snapshots[-2], snapshots[-1])
            self.assertEqual(snapshots[-1][key], override)
            audit = updater.read_object(self.root / "catalog/public-resource-manifest.json")
            shared = next(row for row in audit["resources"] if row["url"] == url)
            self.assertEqual(shared["components"], ["battleReplay", "pets"])
            self.assertEqual((shared["downloads"], shared["reuses"]), (1, 1))
            self.assertEqual(self.calls.count(updater.OFFICIAL + "start.xml"), run + 1)
            self.assertEqual(self.calls.count(updater.OFFICIAL + f"versiondata~{self.entry_version}.swf"), run + 1)
            self.assertEqual(self.calls.count(url), run + 1)

    def test_tampered_shared_cache_fails_its_consumer_without_erasing_other_results(self):
        key = "library/interfaces"
        url = updater.OFFICIAL + f"{key}~{self.version}.swf"
        target = self.root / "catalog/battle-replay-catalog.brpack"
        target.parent.mkdir()
        target.write_bytes(self.replay_bytes)
        def transport(request, output):
            if request != url:
                return self.fetch(request, output)
            self.calls.append(request)
            output.write_bytes(b"SYNTHETIC verified resource")
            return {"url": request, "sha256": updater.digest(output)}
        self.subject.public_resources.transport = transport
        def pets(versions):
            path, _ = self.subject.resource(key, versions)
            path.write_bytes(b"SYNTHETIC later corruption")
            return False
        def replay(versions):
            self.subject.resource(key, versions)
            self.fail("corrupted shared cache accepted")
        self.subject.pets, self.subject.battleReplay = pets, replay
        result = self.subject.run({"pets", "skills", "battleReplay"})
        self.assertEqual(result["pets"]["status"], "unchanged")
        self.assertEqual(result["skills"]["status"], "unchanged")
        self.assertEqual(result["battleReplay"]["status"], "failed")
        self.assertIn("共享资源缓存校验失败", result["battleReplay"]["error"])
        self.assertEqual(target.read_bytes(), self.replay_bytes)
        saved = updater.read_object(self.root / "catalog/public-update-status.json")
        self.assertEqual(saved["components"], result)
        self.assertFalse(saved["complete"])

    def test_h5_only_skills_use_h5_snapshot_without_any_flash_request(self):
        del self.subject.skills
        version = "20260930123456"
        main = b"SYNTHETIC TEST-ONLY approved loader"
        resource = "config/battleconfig.aqz"
        payload = b"SYNTHETIC TEST-ONLY H5 skills payload"
        def transport(url, output):
            self.calls.append(url)
            self.assertTrue(url.startswith(resources.H5), "H5-only skills requested Flash")
            if "/start~" in url:
                output.write_text(json.dumps({"version": version, "files": {}}), encoding="utf-8")
            elif "/version~" in url:
                output.write_text(json.dumps({resource: version}), encoding="utf-8")
            elif "/js/main~" in url:
                output.write_bytes(main)
            elif url == resources.H5 + f"config/battleconfig~{version}.aqz":
                output.write_bytes(payload)
            else:
                self.fail("unexpected H5 request: " + url)
            return {"url": url, "sha256": updater.digest(output)}
        self.subject.public_resources.transport = transport
        def update_skills(subject, atomic_json, versions):
            self.assertIs(subject, self.subject)
            self.assertEqual(versions, {})
            path, proof = subject.public_resources.h5_resource(resource, self.scratch / "skills.aqz")
            self.assertEqual(path.read_bytes(), payload)
            self.assertEqual(proof["version"], version)
            return False
        module = types.ModuleType("public_skill_updater")
        module.update_skills = Mock(side_effect=update_skills)
        with patch.dict(sys.modules, {"public_skill_updater": module}), \
                patch.object(resources, "H5_MAIN_SHA256", hashlib.sha256(main).hexdigest()), \
                patch.object(self.subject, "versions", side_effect=AssertionError("Flash chain must not run")) as flash:
            result = self.subject.run({"skills"})
        flash.assert_not_called()
        module.update_skills.assert_called_once()
        self.assertEqual(result["skills"]["status"], "unchanged")
        self.assertEqual(len(self.calls), 4)
        audit = updater.read_object(self.root / "catalog/public-resource-manifest.json")
        self.assertEqual(audit["versions"], {"h5": version})
        self.assertEqual(audit["versionErrors"], {})

    def test_failed_replay_keeps_previous_package_and_other_components_finish(self):
        target = self.root / "catalog/battle-replay-catalog.brpack"
        target.parent.mkdir()
        target.write_bytes(self.replay_bytes)
        with patch.object(self.subject, "battleReplay", side_effect=ValueError("synthetic replay rebuild failure")), \
                patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            result = self.subject.run()
        self.assertEqual(result["battleReplay"], {"status": "failed", "error": "synthetic replay rebuild failure", "retryable": True})
        self.assertEqual(set(result), {"pets", "skills", "shop", "images", "icons", "routines", "battleReplay"})
        self.assertTrue(all(value["status"] != "failed" for key, value in result.items() if key != "battleReplay"))
        self.assertEqual(target.read_bytes(), self.replay_bytes)
        self.assertFalse(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])

    def test_non_replay_update_does_not_import_unavailable_replay_module(self):
        # Reload the public entry under a separate module identity as well as
        # running a non-replay component. This catches an eager top-level import
        # even if another test has already imported the normal public entry.
        spec = importlib.util.spec_from_file_location("public_updater_without_replay_fixture", updater.__file__)
        isolated = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {"public_battle_replay_updater": None}), \
                patch.object(sys, "path", list(sys.path)):
            spec.loader.exec_module(isolated)
            subject = isolated.Updater(self.root, self.baseline, self.scratch, fetcher=self.fetch, isolated_workspace=True)
            with patch.object(subject, "versions", return_value=self.versions) as versions, \
                    patch.object(subject, "prepare_resource_plan", return_value={}), \
                    patch.object(isolated, "needs_supplement", return_value=False), \
                    patch.object(isolated, "runtime_entry", side_effect=AssertionError("runtime preparation must not run")):
                result = subject.run({"pets"})
        versions.assert_called_once_with()
        self.assertEqual(set(result), {"pets"})
        self.assertEqual(result["pets"]["status"], "unchanged")
        self.assertEqual(self.calls, [])
        self.assertFalse((self.root / "catalog/battle-replay-catalog.brpack").exists())

    def test_unknown_shop_rules_are_reported_instead_of_claiming_full_coverage(self):
        path = self.baseline / "shop-exchange-data.json"
        catalog = updater.read_object(path)
        catalog["shops"][0]["goods"] = [{"enhanceType":"11-31-34-33$5-39$1-39$3-89$1-95","raceIds":[1]}]
        updater.atomic_json(path,catalog)
        with patch.object(updater,"runtime_entry",return_value=Path("synthetic-runtime")):
            result = self.subject.run()
        self.assertNotIn("error",result["shop"])
        target = self.root / "catalog/shop-exchange-data.json"
        catalog["shops"][0]["goods"].append({"enhanceType":"777","raceIds":[1]})
        updater.atomic_json(target,catalog)
        with patch.object(updater,"runtime_entry",return_value=Path("synthetic-runtime")):
            result = self.subject.run()
        self.assertEqual(result["shop"]["unsupportedTypes"],["777"])
        self.assertFalse(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])

    def test_activity_merge_keeps_local_id_zero_and_only_replaces_activity_scope(self):
        target = self.root / "catalog/activity-exchange-data.json"
        activity = {"schema":1,"source":{"kind":"official", "discovery":{"large":"scanner-only"}},
                    "coverage":{"goods":1}, "shops":[{"sourceKey":"module#Config.TABLE","shopId":1,"name":"活动",
                    "goods":[{"itemServerId":0,"enhanceType":"33$1","raceIds":[7545,7546]}]}]}
        updater.atomic_json(target,activity)
        self.assertTrue(self.subject.shop(self.versions))
        merged = updater.read_object(self.root / "catalog/shop-exchange-data.json")
        self.assertEqual(len(merged["shops"]),2)
        self.assertEqual(merged["shops"][1]["goods"][0]["itemServerId"],0)
        self.assertNotIn("discovery",merged["activitySource"])
        self.assertFalse(self.subject.shop(self.versions))
        activity["shops"] = []
        updater.atomic_json(target,activity)
        self.assertTrue(self.subject.shop(self.versions))
        self.assertEqual(len(updater.read_object(self.root / "catalog/shop-exchange-data.json")["shops"]),1)

    def test_broken_current_file_can_recover_from_matching_valid_baseline(self):
        target = self.root / "catalog/pet-detail-data.json"
        target.parent.mkdir()
        target.write_text('{"pets":null}', encoding="utf-8")
        self.assertFalse(self.subject.pets(self.versions))
        self.assertEqual(len(updater.read_object(target)["pets"]), 9000)

    def resource_fetch(self, url, path):
        self.calls.append(url)
        path.write_bytes(b"FWSsynthetic")
        return {"url": url, "sha256": updater.digest(path)}

    def export_fixture(self, swf, target, selected):
        target.mkdir(parents=True, exist_ok=True)
        script = target / (selected.rsplit(".", 1)[-1] + ".as")
        script.write_text("synthetic parsed by controlled test fixture", encoding="utf-8")
        return script

    def test_rule_schema_upgrade_does_not_redownload_unchanged_pet_dictionaries(self):
        target = self.root / "catalog/pet-detail-data.json"
        old = updater.read_object(self.baseline / target.name)
        del old["powerRuleVersion"]
        updater.atomic_json(target, old)
        self.subject.fetch, self.subject.exporter = self.resource_fetch, self.export_fixture
        with patch.object(updater, "parse_stargod_catalog", return_value=self.stars), \
                patch.object(updater, "parse_astrolabe_catalog", return_value=self.astrolabe), \
                patch.object(updater, "merge_pet_sources", side_effect=AssertionError("unchanged dictionary parsed")):
            self.assertTrue(self.subject.pets(self.versions))
        self.assertEqual(len(self.calls), 4)
        self.assertTrue(all("petdictionary" not in url for url in self.calls))
        current = updater.read_object(target)
        self.assertEqual(current["pets"], old["pets"])
        self.assertEqual(current["untouchedSheet"], {"x": 1})
        self.assertTrue(updater.power_rules_valid(current))
        self.calls.clear()
        self.assertFalse(self.subject.pets(self.versions))
        self.assertEqual(self.calls, [])

    def test_dictionary_schema_upgrade_restores_official_sign_from_same_version_sources(self):
        target = self.root / "catalog/pet-detail-data.json"
        old = updater.read_object(self.baseline / target.name)
        latest_pets = {key: {**value, "sign": "神运,灵初,皮肤"} for key, value in old["pets"].items()}
        old["petDictionarySchema"] = 2
        for pet in old["pets"].values():
            pet.pop("sign")
        updater.atomic_json(target, old)
        self.subject.fetch, self.subject.exporter = self.resource_fetch, self.export_fixture
        with patch.object(updater, "merge_pet_sources", return_value=latest_pets):
            self.assertTrue(self.subject.pets(self.versions))
        self.assertEqual(len(self.calls), 2)
        self.assertTrue(all("petdictionary" in url for url in self.calls))
        current = updater.read_object(target)
        self.assertEqual(current["petDictionarySchema"], 5)
        self.assertEqual(current["pets"]["1"]["sign"], "神运,灵初,皮肤")
        self.assertEqual(current["untouchedSheet"], old["untouchedSheet"])
        self.calls.clear()
        self.assertFalse(self.subject.pets(self.versions))
        self.assertEqual(self.calls, [])

    def test_schema_four_rebuilds_unchanged_dictionary_for_strict_timestamp_merging(self):
        target = self.root / "catalog/pet-detail-data.json"
        old = updater.read_object(self.baseline / target.name)
        old["petDictionarySchema"] = 4
        updater.atomic_json(target, old)
        corrected = {key: {**value, "name": "timestamp-corrected"} for key, value in old["pets"].items()}
        self.subject.fetch, self.subject.exporter = self.resource_fetch, self.export_fixture
        with patch.object(updater, "merge_pet_sources", return_value=corrected) as merge:
            self.assertTrue(self.subject.pets(self.versions))
            self.assertFalse(self.subject.pets(self.versions))
        merge.assert_called_once()
        self.assertEqual(len(self.calls), 2)
        self.assertTrue(all("petdictionary" in url for url in self.calls))
        current = updater.read_object(target)
        self.assertEqual(current["petDictionarySchema"], 5)
        self.assertEqual(current["pets"], corrected)
        self.assertEqual(current["untouchedSheet"], old["untouchedSheet"])

    def test_missing_sign_rechecks_dictionary_but_failed_parse_keeps_disk(self):
        target = self.root / "catalog/pet-detail-data.json"
        old = updater.read_object(self.baseline / target.name)
        old["pets"]["1"].pop("sign")
        updater.atomic_json(target, old)
        before = target.read_bytes()
        self.subject.fetch, self.subject.exporter = self.resource_fetch, self.export_fixture
        with patch.object(updater, "merge_pet_sources", side_effect=ValueError("sign is not literal")):
            with self.assertRaises(ValueError):
                self.subject.pets(self.versions)
        self.assertEqual(target.read_bytes(), before)

    def test_official_constructor_sign_uses_parameter_29_and_requires_string(self):
        fixture = self.scratch / "PetDictionaryDataContents.as"
        args = ["0"] * 67
        args[0], args[1] = "9001", json.dumps("没有时代前缀的皮肤", ensure_ascii=False)
        args[8], args[9] = '"26"', '"22"'
        args[53] = '"SP"'
        args[28] = json.dumps("神职,神运,灵初,皮肤", ensure_ascii=False)
        args[62], args[63] = '"8:51:100"', "8"
        fixture.write_text("PetDictionaryDataItem.create(" + ",".join(args) + ");", encoding="utf-8")
        pet = parse_pets(self.scratch)["9001"]
        self.assertEqual(pet["quality"], "SP")
        self.assertEqual(pet["sign"], "神职,神运,灵初,皮肤")
        self.assertEqual(pet["astrolabeBreakCosts"], "8:51:100")
        for invalid in ("null", "123", "SomeDynamicConstant"):
            args[28] = invalid
            fixture.write_text("PetDictionaryDataItem.create(" + ",".join(args) + ");", encoding="utf-8")
            with self.assertRaises(ValueError):
                parse_pets(self.scratch)

    def test_astrolabe_version_change_only_updates_its_rule_sheet(self):
        self.subject.fetch = self.resource_fetch
        self.versions["astrolabe/astrolabeservice"] = "2026091212345678"
        changed = {**self.astrolabe, "30": {"name": "new node", "locatedType": 7, "battlePower": 750, "lightUpMaterials": [],"isTBD":False}}
        with patch.object(updater, "parse_astrolabe_catalog", return_value=changed), \
                patch.object(updater, "merge_pet_sources", side_effect=AssertionError("unchanged dictionary parsed")), \
                patch.object(updater, "parse_stargod_catalog", side_effect=AssertionError("unchanged stars parsed")):
            self.assertTrue(self.subject.pets(self.versions))
        self.assertEqual(self.calls, [updater.OFFICIAL + "astrolabe/astrolabeservice~2026091212345678.swf"])
        self.assertEqual(updater.read_object(self.root / "catalog/pet-detail-data.json")["astrolabe"], changed)

    def test_failed_rules_preserve_whole_pet_component_even_if_dictionary_parse_succeeded(self):
        target = self.root / "catalog/pet-detail-data.json"
        original = (self.baseline / target.name).read_bytes()
        target.parent.mkdir()
        target.write_bytes(original)
        self.subject.fetch, self.subject.exporter = self.resource_fetch, self.export_fixture
        self.versions["pet/petdictionarydataupdate"] = "2026091212345678"
        self.versions["material/materialservice"] = "2026091212345678"
        new_pets = {str(n): {"name": "new name"} for n in range(9000)}
        with patch.object(updater, "merge_pet_sources", return_value=new_pets), \
                patch.object(updater, "parse_stargod_catalog", side_effect=ValueError("official constructor changed")):
            with self.assertRaises(ValueError):
                self.subject.pets(self.versions)
        self.assertEqual(target.read_bytes(), original)

    def test_cultivation_upgrade_updates_costs_without_reparsing_dictionary_or_stars(self):
        target = self.root / "catalog/pet-detail-data.json"
        old = updater.read_object(self.baseline / target.name)
        old.pop("cultivationRuleVersion")
        updater.atomic_json(target, old)
        self.subject.fetch, self.subject.exporter = self.resource_fetch, self.export_fixture
        with patch.object(updater, "parse_badges", return_value=self.badges), \
                patch.object(updater, "parse_sacred", return_value=self.sacred), \
                patch.object(updater, "equipment_source_names", return_value={"1":"source"}), \
                patch.object(updater, "parse_sacred_plans", return_value=self.plans), \
                patch.object(updater, "parse_items", return_value=self.cultivation["items"]), \
                patch.object(updater, "parse_astrolabe_catalog", return_value=self.astrolabe), \
                patch.object(updater, "merge_pet_sources", side_effect=AssertionError("unchanged dictionary parsed")), \
                patch.object(updater, "parse_stargod_catalog", side_effect=AssertionError("unchanged stars parsed")):
            self.assertTrue(self.subject.pets(self.versions))
        self.assertEqual(len(self.calls), 7)
        self.assertTrue(updater.cultivation_rules_valid(updater.read_object(target)))
        self.calls.clear()
        self.assertFalse(self.subject.pets(self.versions))
        self.assertEqual(self.calls, [])

    def test_failed_cost_parse_preserves_previous_whole_pet_file(self):
        target = self.root / "catalog/pet-detail-data.json"
        original = (self.baseline / target.name).read_bytes()
        target.parent.mkdir()
        target.write_bytes(original)
        self.subject.fetch = self.resource_fetch
        self.versions["petbadge/petbadgeservice"] = "2026091212345678"
        with patch.object(updater, "parse_badges", side_effect=ValueError("invalid official cost")):
            with self.assertRaises(ValueError):
                self.subject.pets(self.versions)
        self.assertEqual(target.read_bytes(), original)

    def test_zip_traversal_does_not_escape_owned_directory(self):
        archive = self.root / "bad.zip"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("../escaped.txt", "bad")
        with self.assertRaises(ValueError):
            updater.extract_zip(archive, self.root / "extract")
        self.assertFalse((self.root / "escaped.txt").exists())

    def test_local_picture_hit_never_downloads_or_bootstraps(self):
        entry = updater.image_entries(self.versions)["7529"]
        updater.atomic_json(self.root / "catalog/pet-image-index.json", {"faces": {"7529": entry}})
        target = updater.picture_cache_path(self.root, "7529_7529", entry)
        target.parent.mkdir(parents=True)
        target.write_bytes(b"existing image bytes")
        updater.atomic_json(target.with_suffix(".json"), {"url": entry["swfUrl"], "symbol": entry["symbol"],
                            "revision": entry["revision"], "pngSha256": updater.digest(target)})
        with patch.object(updater, "fetch", side_effect=AssertionError("network")), patch.object(updater, "installed_runtime", side_effect=AssertionError("runtime")):
            self.assertEqual(updater.extract_image(self.root, "7529_7529")["status"], "cached")
        self.assertEqual(target.read_bytes(), b"existing image bytes")

    def test_updated_image_failure_retries_next_manual_check_and_retains_old(self):
        target = self.root / "images/pets/7529_7529.png"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"old")
        faces = updater.image_entries(self.versions)
        same_index = {"faces": faces}
        entry = faces["7529"]
        updater.atomic_json(target.with_suffix(".json"), {"url": entry["swfUrl"], "symbol": entry["symbol"],
                            "revision": entry["revision"], "pngSha256": updater.digest(target)})
        with patch.object(updater, "extract_image", side_effect=OSError("synthetic timeout")) as fetch_image:
            updater.refresh_cached_images(self.root, same_index, faces)
            updater.refresh_cached_images(self.root, same_index, faces)
        self.assertEqual(fetch_image.call_count, 2)
        self.assertEqual(target.read_bytes(), b"old")

    def test_unchanged_index_skips_hashing_but_still_retries_stale_revisions(self):
        faces = updater.image_entries(self.versions)
        entry = updater.picture_entry({"faces": faces}, "7529_7529")
        target = updater.picture_cache_path(self.root, "7529_7529", entry)
        target.parent.mkdir(parents=True)
        target.write_bytes(b"old")
        # Matching revision but a different hash: only a content check notices.
        updater.atomic_json(target.with_suffix(".json"), {"url": entry["swfUrl"], "symbol": entry["symbol"],
                            "revision": entry["revision"], "pngSha256": "0" * 64})
        with patch.object(updater, "extract_image") as fetch_image:
            updater.refresh_cached_images(self.root, {"faces": faces}, faces, verify_content=False)
            self.assertEqual(fetch_image.call_count, 0)
            updater.refresh_cached_images(self.root, {"faces": faces}, faces)
            self.assertEqual(fetch_image.call_count, 1)
        changed = updater.image_entries({"peticon/peticon7529": "20261015123456"})
        with patch.object(updater, "extract_image") as fetch_image:
            updater.refresh_cached_images(self.root, {"faces": faces}, changed, verify_content=False)
            self.assertEqual(fetch_image.call_count, 1)

    def test_face_lookup_uses_exact_resource_before_documented_display_suffix(self):
        index = {"faces": {"7529": {"symbol": "base"}, "17529": {"symbol": "exact"}}}
        self.assertEqual(updater.picture_entry(index, "7529_17529")["symbol"], "exact")
        del index["faces"]["17529"]
        self.assertEqual(updater.picture_entry(index, "7529_17529")["symbol"], "base")
        with self.assertRaises(ValueError):
            updater.picture_entry(index, "../../outside")

    def test_official_default_and_high_face_exceptions(self):
        index = updater.image_entries({}, ("7529", "17529", "10366"), (10366,), default_version="2000")
        self.assertTrue(index["7529"]["swfUrl"].endswith("peticon7529~2000.swf"))
        self.assertEqual(index["17529"], index["7529"])
        self.assertTrue(index["10366"]["swfUrl"].endswith("peticon10366~2000.swf"))


if __name__ == "__main__":
    unittest.main()
