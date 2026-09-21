"""Updater contract tests use isolated files and fake official transport only."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_data_updater as updater
from generate_pet_detail_data import parse_pets


class PublicDataUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.baseline = self.root / "baseline"
        self.baseline.mkdir()
        self.scratch = self.root / "scratch"
        self.scratch.mkdir()
        self.calls = []
        self.version = "2026091011614836"
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
                            "petDictionarySchema":3,
                            "nameRuleVersion": updater.NAME_RULE_VERSION, "attributes": {"1":"普"}, "jobs":{"1":"利爪"},
                            "fusionJobs":[], "money":{"1":{"name":"金币"}},
                            "powerRuleVersion": updater.POWER_RULE_VERSION, "stargods": self.stars, "astrolabe": self.astrolabe,
                            **self.cultivation})
        updater.atomic_json(self.baseline / "pet-skill-data.json",
                            {"schema": 1, "source": {"version": self.version},
                             "pets": {"1": {"name": "pet", "slots": {"normal": 100001}}},
                             "skills": {"100001": {"name": "skill", "description": "desc"}},
                             "entries": {"term": "desc"}, "buffs": {"1": {"name": "buff"}}})
        updater.atomic_json(self.baseline / "shop-exchange-data.json", {"shops": [{"goods": [{"id": 1}]}], "source": {"shopVersion": self.version}})
        self.subject = updater.Updater(self.root, self.baseline, self.scratch, fetcher=self.fetch)
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
        if url.endswith("start.xml"):
            path.write_text('<r><v>2026091012345678</v><u/></r>', encoding="utf-8")
        elif "versiondata~" in url:
            # swf_text only consumes this fixture in tests, while downloaded
            # production files retain the bounded official parser.
            body = ''.join(f'<f n="{k}" v="{v}"/>' for k, v in self.versions.items())
            path.write_bytes(b"FWS" + body.encode())
        else:
            raise OSError("synthetic resource unavailable")
        return {"url": url, "sha256": updater.digest(path)}

    def test_repeat_manual_check_only_fetches_start_and_no_component_download(self):
        with patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            first = self.subject.run()
            self.assertTrue(all(v["status"] != "failed" for v in first.values()))
            self.assertEqual(len(self.calls), 2)
            saved = (self.root / "catalog/pet-detail-data.json").read_bytes()
            self.calls.clear()
            second = self.subject.run()
        self.assertEqual(self.calls, [updater.OFFICIAL + "start.xml"])
        self.assertTrue(all(v["status"] == "unchanged" for v in second.values()))
        self.assertEqual(saved, (self.root / "catalog/pet-detail-data.json").read_bytes())
        self.assertEqual(len(list((self.root / "catalog").glob("*.json"))), 6)
        self.assertEqual(set(second), {"pets","skills","shop","images","icons","routines"})
        self.assertTrue(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])

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
        self.assertEqual(set(status["components"]), {"pets", "skills", "shop", "images", "icons", "routines"})
        self.assertTrue(status["complete"])
        with self.assertRaises(ValueError):
            self.subject.run(set())

    def test_partial_first_run_is_not_reported_complete(self):
        with patch.object(updater, "runtime_entry", return_value=Path("synthetic-runtime")):
            self.subject.run({"pets"})
        self.assertFalse(updater.read_object(self.root / "catalog/public-update-status.json")["complete"])

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

    def test_unknown_shop_rules_are_reported_instead_of_claiming_full_coverage(self):
        path = self.baseline / "shop-exchange-data.json"
        catalog = updater.read_object(path)
        catalog["shops"][0]["goods"] = [{"enhanceType":"11-31-34-33$5-39$1-39$3-89$1-95"}]
        updater.atomic_json(path,catalog)
        with patch.object(updater,"runtime_entry",return_value=Path("synthetic-runtime")):
            result = self.subject.run()
        self.assertNotIn("error",result["shop"])
        target = self.root / "catalog/shop-exchange-data.json"
        catalog["shops"][0]["goods"].append({"enhanceType":"777"})
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

    def test_dictionary_schema_three_adds_official_sign_from_same_version_sources(self):
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
        self.assertEqual(current["petDictionarySchema"], 3)
        self.assertEqual(current["pets"]["1"]["sign"], "神运,灵初,皮肤")
        self.assertEqual(current["untouchedSheet"], old["untouchedSheet"])
        self.calls.clear()
        self.assertFalse(self.subject.pets(self.versions))
        self.assertEqual(self.calls, [])

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
        args[28] = json.dumps("神职,神运,灵初,皮肤", ensure_ascii=False)
        args[62], args[63] = '"8:51:100"', "8"
        fixture.write_text("PetDictionaryDataItem.create(" + ",".join(args) + ");", encoding="utf-8")
        pet = parse_pets(self.scratch)["9001"]
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
        target = self.root / "images/pets/7529_7529.png"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"existing image bytes")
        with patch.object(updater, "fetch", side_effect=AssertionError("network")), patch.object(updater, "installed_runtime", side_effect=AssertionError("runtime")):
            self.assertEqual(updater.extract_image(self.root, "7529_7529")["status"], "cached")
        self.assertEqual(target.read_bytes(), b"existing image bytes")

    def test_updated_image_failure_retries_next_manual_check_and_retains_old(self):
        target = self.root / "images/pets/7529_7529.png"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"old")
        faces = updater.image_entries(self.versions)
        same_index = {"faces": faces}
        with patch.object(updater, "extract_image", side_effect=OSError("synthetic timeout")) as fetch_image:
            updater.refresh_cached_images(self.root, same_index, faces)
            updater.refresh_cached_images(self.root, same_index, faces)
        self.assertEqual(fetch_image.call_count, 2)
        self.assertEqual(target.read_bytes(), b"old")

    def test_unchanged_index_skips_hashing_but_still_retries_stale_revisions(self):
        target = self.root / "images/pets/7529_7529.png"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"old")
        faces = updater.image_entries(self.versions)
        entry = updater.picture_entry({"faces": faces}, "7529_7529")
        # Matching revision but a different hash: only a content check notices.
        updater.atomic_json(target.with_suffix(".json"), {"revision": entry["revision"], "pngSha256": "0" * 64})
        with patch.object(updater, "extract_image") as fetch_image:
            updater.refresh_cached_images(self.root, {"faces": faces}, faces, verify_content=False)
            self.assertEqual(fetch_image.call_count, 0)
            updater.refresh_cached_images(self.root, {"faces": faces}, faces)
            self.assertEqual(fetch_image.call_count, 1)
        updater.atomic_json(target.with_suffix(".json"), {"revision": "1", "pngSha256": "0" * 64})
        with patch.object(updater, "extract_image") as fetch_image:
            updater.refresh_cached_images(self.root, {"faces": faces}, faces, verify_content=False)
            self.assertEqual(fetch_image.call_count, 1)

    def test_face_lookup_uses_exact_resource_before_documented_display_suffix(self):
        index = {"faces": {"7529": {"symbol": "base"}, "17529": {"symbol": "exact"}}}
        self.assertEqual(updater.picture_entry(index, "7529_17529")["symbol"], "exact")
        del index["faces"]["17529"]
        self.assertEqual(updater.picture_entry(index, "7529_17529")["symbol"], "base")
        with self.assertRaises(ValueError):
            updater.picture_entry(index, "../../outside")

    def test_official_default_and_high_face_exceptions(self):
        index = updater.image_entries({}, ("7529", "17529", "10366"), (10366,))
        self.assertTrue(index["7529"]["swfUrl"].endswith("peticon7529~2000.swf"))
        self.assertEqual(index["17529"], index["7529"])
        self.assertTrue(index["10366"]["swfUrl"].endswith("peticon10366~2000.swf"))


if __name__ == "__main__":
    unittest.main()
