"""One-run public-resource contracts using synthetic SWF/JSON and no network."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import public_resource_session as subject
from public_resource_plan import build_plan
from test_public_battle_replay_updater import swf_payloads


def encoded(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def sha(value):
    return hashlib.sha256(value).hexdigest()


def dependency(kind, resource, *components, version_only=False):
    return {"kind": kind, "resource": resource, "components": list(components),
            "reason": "synthetic declared dependency", "versionOnly": version_only}


class PublicResourceSessionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shared-public-session-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        blocker = mock.patch("urllib.request.urlopen", side_effect=AssertionError("Real network is forbidden"))
        blocker.start()
        self.addCleanup(blocker.stop)
        self.flash_version = "202610110001"
        self.base_version = "202610090002"
        self.override_version = "202610110003"
        self.flash_start = (f'<start><v>{self.flash_version}</v><lv>199901010000</lv>'
                            f'<u><f n="battle/battle" v="{self.override_version}"/></u>'
                            '<unrelated>original</unrelated></start>').encode()
        self.flash_versions = {"dummy/" + str(index): self.base_version for index in range(105)}
        self.flash_versions.update({"battle/battle": self.base_version, "pet/petdictionarydata": self.base_version})
        self.h5_version = "202610110099"
        self.h5_start = {"version": self.h5_version, "files": {
            "config/battleconfig.aqz": "202610110008", "config/formation.mix": "202610110009"},
            "unrelated": "original"}
        self.h5_versions = {"config/battleconfig.aqz": self.base_version,
                            "config/formation.aqz": self.base_version,
                            "config/formation.mix": "202610090003",
                            "config/pet/petinfoconfig.json": "202610100010"}
        self.main_bytes = b"// synthetic reviewed H5 loader; never executed"
        approved = mock.patch.object(subject, "H5_MAIN_SHA256", sha(self.main_bytes))
        approved.start()
        self.addCleanup(approved.stop)
        self.calls = []
        self.call_guard = threading.Lock()
        self.payloads = {}
        self.block_path = None
        self.entered = threading.Event()
        self.release = threading.Event()
        self.session = subject.PublicResourceSession(self.root, self.transport)

    def transport(self, url, output, *, runtime=False):
        path = urlsplit(url).path
        with self.call_guard:
            self.calls.append((url, runtime))
        if path == self.block_path:
            self.entered.set()
            if not self.release.wait(5):
                raise TimeoutError("Synthetic concurrent transport was never released")
        if runtime:
            data = b"synthetic pinned runtime"
        elif path == "/play/start.xml":
            data = self.flash_start
        elif path.startswith("/play/versiondata~"):
            body = '<vconfig>' + ''.join(f'<f n="{key}" v="{value}"/>'
                                        for key, value in self.flash_versions.items()) + '</vconfig>'
            data = swf_payloads({"mmo.versiondata.DocClass_cls": body.encode()})
        elif re.fullmatch(r"/h5/start~[0-9]+\.json", path):
            data = encoded(self.h5_start)
        elif path.startswith("/h5/version~"):
            data = encoded(self.h5_versions)
        elif path.startswith("/h5/js/main~"):
            data = self.main_bytes
        elif url in self.payloads:
            data = self.payloads[url]
        else:
            raise OSError("Unregistered synthetic resource: " + url)
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        return {"url": url, "sha256": sha(data), "bytes": len(data)}

    def count_path(self, path):
        return sum(urlsplit(url).path == path for url, _ in self.calls)

    def test_flash_uses_v_then_u_f_and_official_missing_resource_default(self):
        versions, start, proof = self.session.flash_chain()
        self.assertEqual(start, self.flash_start)
        self.assertEqual(versions["battle/battle"], self.override_version)
        self.assertEqual(versions["pet/petdictionarydata"], self.base_version)
        self.assertEqual(versions.get("missing/officialresource", "2000"), "2000")
        self.assertEqual(proof["versiondata"]["version"], self.flash_version)
        self.assertEqual(proof["versiondata"]["manifestSha256"], sha(self.flash_start))
        urls = [url for url, _ in self.calls]
        self.assertIn(subject.FLASH + "versiondata~" + self.flash_version + ".swf", urls)
        self.assertFalse(any("199901010000" in url for url in urls))

    def test_workbench_and_replay_share_one_flash_snapshot_and_download(self):
        self.session.component = "pets"
        workbench = self.session.flash_chain()
        self.session.component = "battleReplay"
        replay = self.session.flash_chain(self.root / "relocated-flash")
        self.assertEqual(replay, workbench)
        self.assertEqual(self.count_path("/play/start.xml"), 1)
        self.assertEqual(self.count_path("/play/versiondata~" + self.flash_version + ".swf"), 1)
        self.assertEqual((self.root / "relocated-flash/start.xml").read_bytes(), self.flash_start)
        self.assertTrue((self.root / "relocated-flash/versiondata.swf").is_file())
        replay[0]["battle/battle"] = "caller-only-change"
        self.assertEqual(self.session.flash_chain()[0]["battle/battle"], self.override_version)

    def test_h5_files_override_applies_to_archives_and_mix_precedes_direct_key(self):
        battle = self.session.resolve_h5("config/battleconfig.aqz")
        formation = self.session.resolve_h5("config/formation.aqz")
        petinfo = self.session.resolve_h5("config/pet/petinfoconfig.json")
        missing = self.session.resolve_h5("config/pet/missing.json")
        self.assertEqual((battle["version"], battle["resolution"]), ("202610110008", "start-files"))
        self.assertEqual((formation["version"], formation["resolution"]), ("202610110009", "start-files"))
        self.assertEqual(formation["url"], subject.H5 + "config/formation~202610110009.aqz")
        self.assertEqual((petinfo["version"], petinfo["resolution"]), ("202610100010", "version-manifest"))
        self.assertEqual((missing["version"], missing["resolution"]), ("20211126", "default-20211126"))
        chain = self.session.h5_chain(self.root / "relocated-h5")
        self.assertEqual(chain["versions"]["config/battleconfig.aqz"], "202610110008")
        self.assertEqual(json.loads((self.root / "relocated-h5/h5-start.json").read_bytes()), self.h5_start)
        self.assertEqual((self.root / "relocated-h5/h5-main.js").read_bytes(), self.main_bytes)

    def test_h5_manifest_mix_alias_precedes_direct_start_files_entry(self):
        self.h5_start["files"].pop("config/formation.mix")
        self.h5_start["files"]["config/formation.aqz"] = "202610110020"
        result = self.session.resolve_h5("config/formation.aqz")
        self.assertEqual((result["version"], result["resolution"]), ("202610090003", "version-manifest"))

    def test_h5_double_extension_archive_uses_only_final_extension_for_versioned_url(self):
        resource = "config/petdictionarydata.json.aqz"
        self.h5_versions[resource] = "202610110021"
        self.h5_versions["config/petdictionarydata.mix"] = "202610110022"
        # The earlier dot is part of the stem, so a stem-shortening alias must
        # not accidentally select the revision of a different resource.
        resolved = self.session.resolve_h5(resource)
        self.assertEqual(resolved["version"], "202610110021")
        self.assertEqual(resolved["url"], subject.H5 + "config/petdictionarydata.json~202610110021.aqz")
        self.payloads[resolved["url"]] = b"synthetic double-extension archive"
        path, record = self.session.h5_resource(resource, self.root / "dictionary.aqz")
        self.assertEqual(path.read_bytes(), self.payloads[resolved["url"]])
        self.assertEqual(record["resource"], resource)
        self.assertEqual(record["sha256"], sha(self.payloads[resolved["url"]]))

    def test_h5_double_extension_archive_prefers_json_mix_and_files_override(self):
        resource = "config/petdictionarydata.json.aqz"
        alias = "config/petdictionarydata.json.mix"
        self.h5_versions[resource] = "202610110021"
        self.h5_versions[alias] = "202610110022"
        self.h5_start["files"][resource] = "202610110023"
        manifest_alias = self.session.resolve_h5(resource)
        self.assertEqual((manifest_alias["version"], manifest_alias["resolution"]),
                         ("202610110022", "version-manifest"))
        self.assertEqual(manifest_alias["url"], subject.H5 + "config/petdictionarydata.json~202610110022.aqz")
        self.h5_start["files"][alias] = "202610110024"
        self.session.reset()
        override = self.session.resolve_h5(resource)
        self.assertEqual((override["version"], override["resolution"]), ("202610110024", "start-files"))
        self.assertEqual(override["url"], subject.H5 + "config/petdictionarydata.json~202610110024.aqz")

    def test_h5_double_extension_support_does_not_allow_traversal_or_absolute_paths(self):
        for resource in ("../config/petdictionarydata.json.aqz", "config/../petdictionarydata.json.aqz",
                         "config/pet..json.aqz", "/config/petdictionarydata.json.aqz",
                         "config\\petdictionarydata.json.aqz", "C:/config/petdictionarydata.json.aqz"):
            with self.subTest(resource=resource), self.assertRaisesRegex(ValueError, "无效"):
                self.session.resolve_h5(resource)
        self.assertEqual(self.calls, [], "Invalid resource paths must fail before any transport")

    def test_h5_unreviewed_main_fails_and_is_not_automatically_approved(self):
        self.main_bytes = b"changed executable loader"
        with self.assertRaisesRegex(ValueError, "加载代码变化"):
            self.session.h5_chain()
        calls = len(self.calls)
        with self.assertRaisesRegex(ValueError, "加载代码变化"):
            self.session.h5_chain()
        self.assertEqual(len(self.calls), calls, "A failed snapshot remains failed until the next run")

    def test_h5_signature_is_order_independent_and_tracks_resolution_and_loader_rules(self):
        keys = ["config/battleconfig.aqz", "config/formation.aqz"]
        signature = self.session.h5_signature(keys)
        self.assertEqual(signature, self.session.h5_signature(keys[::-1] + keys[:1]))
        with mock.patch.object(subject, "VERSION_RULE", subject.VERSION_RULE + 1):
            self.assertNotEqual(signature, self.session.h5_signature(keys))
        self.h5_start["files"][keys[0]] = "202610120001"
        self.session.reset()
        self.assertNotEqual(signature, self.session.h5_signature(keys))

    def test_same_h5_asset_is_downloaded_once_across_workbench_and_replay(self):
        resource = "config/battleconfig.aqz"
        url = subject.H5 + "config/battleconfig~202610110008.aqz"
        self.payloads[url] = b"synthetic shared archive"
        self.session.component = "skills"
        first, workbench = self.session.h5_resource(resource, self.root / "workbench.aqz")
        self.session.component = "battleReplay"
        second, replay = self.session.h5_resource(resource, self.root / "replay.aqz")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(workbench, replay)
        self.assertEqual(self.count_path(urlsplit(url).path), 1)
        rows = [row for row in self.session.audit()["resources"] if row["url"] == url]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["components"], ["battleReplay", "skills"])
        self.assertEqual((rows[0]["downloads"], rows[0]["reuses"]), (1, 1))

    def test_concurrent_same_url_fetches_once_and_returns_same_bytes(self):
        url = subject.FLASH + "battle/battle~202610110003.swf"
        self.payloads[url] = b"synthetic same-url resource"
        self.block_path = urlsplit(url).path
        start = threading.Barrier(6)

        def worker(index):
            start.wait(timeout=5)
            target = self.root / ("worker-" + str(index))
            return self.session.fetch(url, target), target

        with ThreadPoolExecutor(max_workers=6) as pool:
            jobs = [pool.submit(worker, index) for index in range(6)]
            self.assertTrue(self.entered.wait(5))
            self.release.set()
            results = [job.result(timeout=5) for job in jobs]
        self.assertEqual(self.count_path(self.block_path), 1)
        self.assertTrue(all(target.read_bytes() == self.payloads[url] for _, target in results))
        self.assertTrue(all(row == results[0][0] for row, _ in results))
        audit = self.session.audit()
        self.assertEqual(audit["reusedDownloads"], 5)
        self.assertEqual(audit["uniqueDownloadedBytes"], len(self.payloads[url]))

    def test_tampered_cached_bytes_fail_before_copy_or_silent_redownload(self):
        url = subject.FLASH + "battle/battle~202610110003.swf"
        self.payloads[url] = b"verified original"
        self.session.fetch(url, self.root / "first")
        cache_file = self.session._cache[url][0]
        cache_file.write_bytes(b"tampered data")
        with self.assertRaisesRegex(ValueError, "缓存校验失败"):
            self.session.fetch(url, self.root / "second")
        self.assertFalse((self.root / "second").exists())
        self.assertEqual(self.count_path(urlsplit(url).path), 1)

    def test_flash_stability_ignores_unrelated_entry_content(self):
        self.session.flash_chain()
        self.flash_start = self.flash_start.replace(b"199901010000", b"199901020000") \
            .replace(b"<unrelated>original</unrelated>", b"<unrelated>other</unrelated><another value='x'/>")
        self.session.assert_stable(("flash",))
        self.assertEqual(self.count_path("/play/start.xml"), 2)

    def test_flash_stability_rejects_v_or_effective_override_change(self):
        for changed in (self.flash_start.replace(self.flash_version.encode(), b"202610120001"),
                        self.flash_start.replace(self.override_version.encode(), b"202610120002")):
            with self.subTest(changed=changed):
                self.session.reset()
                original = self.flash_start
                self.session.flash_chain()
                self.flash_start = changed
                try:
                    with self.assertRaisesRegex(ValueError, "切换版本"):
                        self.session.assert_stable(("flash",))
                finally:
                    self.flash_start = original

    def test_h5_stability_rejects_release_and_files_changes(self):
        for field in ("version", "files"):
            with self.subTest(field=field):
                self.session.reset()
                original = copy.deepcopy(self.h5_start)
                self.session.h5_chain()
                if field == "version":
                    self.h5_start["version"] = "202610120001"
                else:
                    self.h5_start["files"]["config/battleconfig.aqz"] = "202610120002"
                try:
                    with self.assertRaisesRegex(ValueError, "切换版本"):
                        self.session.assert_stable(("h5",))
                finally:
                    self.h5_start = original

    def test_h5_stability_ignores_unrelated_start_fields(self):
        self.session.h5_chain()
        self.h5_start["unrelated"] = {"extra": "unconsumed content"}
        self.session.assert_stable(("h5",))

    def test_runtime_download_is_not_official_input_or_audit_data(self):
        url = "https://runtime.example.invalid/pinned-runtime.zip"
        record = self.session.fetch(url, self.root / "runtime.zip", runtime=True)
        self.assertEqual(record["sha256"], sha(b"synthetic pinned runtime"))
        self.assertEqual(self.calls, [(url, True)])
        self.assertEqual(self.session.audit()["resources"], [])
        self.assertEqual(self.session.audit()["uniqueDownloadedBytes"], 0)

    def test_nonofficial_nonruntime_download_is_rejected_without_transport(self):
        for url in ("http://aoqi.100bt.com/play/x", "https://aoqi.100bt.com.evil.invalid/play/x",
                    "https://example.invalid/h5/x", "https://aoqi.100bt.com/private/x"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                self.session.fetch(url, self.root / "rejected")
        self.assertEqual(self.calls, [])

    def test_reset_fetches_new_run_entry_and_payload_instead_of_reusing_old_cache(self):
        resource = "config/battleconfig.aqz"
        url = subject.H5 + "config/battleconfig~202610110008.aqz"
        self.payloads[url] = b"first run bytes"
        self.session.h5_resource(resource, self.root / "first.aqz")
        self.session.plan = [{"old": "plan"}]
        self.payloads[url] = b"new run bytes under same version"
        self.session.reset()
        self.assertEqual(self.session.audit()["resources"], [])
        self.assertEqual(self.session.audit()["initialPlan"], [])
        path, row = self.session.h5_resource(resource, self.root / "second.aqz")
        self.assertEqual(path.read_bytes(), self.payloads[url])
        self.assertEqual(row["sha256"], sha(self.payloads[url]))
        self.assertEqual(self.count_path(urlsplit(url).path), 2)
        self.assertEqual(self.count_path("/h5/version~" + self.h5_version + ".json"), 2)
        self.assertEqual(sum(bool(re.fullmatch(r"/h5/start~[0-9]+\.json", urlsplit(call[0]).path))
                             for call in self.calls), 2)

    def test_parallel_first_flash_chain_keeps_one_snapshot(self):
        self.block_path = "/play/start.xml"
        second_entered = threading.Event()

        def second():
            second_entered.set()
            return self.session.flash_chain()

        with ThreadPoolExecutor(max_workers=2) as pool:
            first = pool.submit(self.session.flash_chain)
            self.assertTrue(self.entered.wait(5))
            later = pool.submit(second)
            self.assertTrue(second_entered.wait(5))
            # Give the second call a chance to contend on snapshot creation
            # while the first is deliberately blocked in official transport.
            time.sleep(0.03)
            self.release.set()
            first_value, second_value = first.result(timeout=5), later.result(timeout=5)
        self.assertEqual(first_value, second_value)
        self.assertEqual(self.count_path("/play/start.xml"), 1)
        self.assertEqual(self.count_path("/play/versiondata~" + self.flash_version + ".swf"), 1)

    def test_h5_provenance_returned_to_consumers_cannot_mutate_shared_snapshot(self):
        first = self.session.h5_chain()
        original = {key: copy.deepcopy(first[key]) for key in ("entry", "manifest", "main")}
        for key in original:
            first[key]["sha256"] = "caller-overwrite"
        second = self.session.h5_chain()
        for key in original:
            self.assertEqual(second[key], original[key])

    def test_flash_duplicate_override_order_change_is_detected_when_last_value_wins(self):
        needle = f'<f n="battle/battle" v="{self.override_version}"/>'.encode()
        earlier = f'<f n="battle/battle" v="{self.base_version}"/>'.encode()
        self.flash_start = self.flash_start.replace(needle, earlier + needle)
        self.assertEqual(self.session.flash_chain()[0]["battle/battle"], self.override_version)
        self.flash_start = self.flash_start.replace(earlier + needle, needle + earlier)
        with self.assertRaisesRegex(ValueError, "切换版本"):
            self.session.assert_stable(("flash",))

    def test_prefetch_union_downloads_each_input_once_then_all_consumers_reuse_it(self):
        completed = []
        self.session.progress_callback = lambda done, total: completed.append((done, total))
        flash = subject.FLASH + "pet/petdictionarydata~" + self.base_version + ".swf"
        h5 = subject.H5 + "config/battleconfig~202610110008.aqz"
        self.payloads.update({flash: b"shared dictionary", h5: b"shared battle archive"})
        self.session.plan = build_plan(
            [dependency("flash", "pet/petdictionarydata", "pets"),
             dependency("h5", "config/battleconfig.aqz", "skills")],
            [dependency("flash", "pet/petdictionarydata", "battleReplay"),
             dependency("h5", "config/battleconfig.aqz", "battleReplay")])
        self.assertEqual(self.session.prefetch(["pets", "skills", "battleReplay"]), {})
        self.assertEqual(completed, [(0, 2), (1, 2), (2, 2)])
        self.assertEqual(len(self.session.plan), 2)
        versions, _, _ = self.session.flash_chain()
        self.assertIsInstance(versions, subject.ResourceVersions)
        self.assertEqual(versions.source_hashes["pet/petdictionarydata"], sha(self.payloads[flash]))
        for component in ("pets", "skills", "battleReplay"):
            self.session.component = component
            url = h5 if component == "skills" else flash
            self.session.fetch(url, self.root / (component + ".input"))
        self.session.h5_resource("config/battleconfig.aqz", self.root / "replay.aqz")
        for url in (flash, h5):
            self.assertEqual(self.count_path(urlsplit(url).path), 1)
        self.assertEqual(self.count_path("/play/start.xml"), 1)
        self.assertEqual(self.count_path("/play/versiondata~" + self.flash_version + ".swf"), 1)
        self.assertEqual(self.count_path("/h5/version~" + self.h5_version + ".json"), 1)

    def test_prefetch_only_resolves_selected_consumers(self):
        h5 = subject.H5 + "config/battleconfig~202610110008.aqz"
        self.payloads[h5] = b"selected H5 input"
        self.session.plan = build_plan(
            [dependency("flash", "pet/petdictionarydata", "pets"),
             dependency("h5", "config/battleconfig.aqz", "skills")],
            [dependency("flash", "battle/battle", "battleReplay")])
        self.assertEqual(self.session.prefetch(["skills"]), {})
        self.assertTrue(self.calls)
        self.assertTrue(all(url.startswith(subject.H5) for url, _ in self.calls))
        self.assertEqual(self.count_path(urlsplit(h5).path), 1)

    def test_prefetch_historical_and_version_only_rows_never_start_downloads(self):
        self.session.plan = build_plan(
            [dependency("flash", "library/util", "pets", version_only=True)], (),
            [dependency("flash", "old/activity", "pets"),
             dependency("h5", "config/historical.json", "skills")])
        self.assertEqual(self.session.prefetch(["pets", "skills"]), {})
        self.assertEqual(self.calls, [])
        self.assertEqual(self.session.audit()["resources"], [])

    def test_prefetch_declared_use_survives_merged_historical_and_version_only_uses(self):
        resource = "pet/petdictionarydata"
        url = subject.FLASH + resource + "~" + self.base_version + ".swf"
        self.payloads[url] = b"one complete declared resource"
        self.session.plan = build_plan(
            [dependency("flash", resource, "pets", version_only=True)],
            [dependency("flash", resource, "battleReplay")],
            [dependency("flash", resource, "pets")])
        self.assertFalse(self.session.plan[0]["versionOnly"])
        self.assertEqual(self.session.prefetch(["battleReplay"]), {})
        self.assertEqual(self.count_path(urlsplit(url).path), 1)

    def test_prefetch_resource_failure_only_blocks_its_selected_owners(self):
        bad = "pet/petdictionarydata"
        good = subject.H5 + "config/battleconfig~202610110008.aqz"
        self.payloads[good] = b"independent skill input"
        self.session.plan = build_plan(
            [dependency("flash", bad, "pets"), dependency("h5", "config/battleconfig.aqz", "skills")],
            [dependency("flash", bad, "battleReplay")])
        failures = self.session.prefetch(["pets", "skills"])
        self.assertEqual(set(failures), {"pets"})
        self.assertIn(bad, failures["pets"])
        self.assertNotIn("battleReplay", failures)
        self.assertEqual(self.count_path(urlsplit(good).path), 1)
        # A second consumer of the same failed resource must get the failure,
        # while independent inputs remain cache hits rather than redownloads.
        failures = self.session.prefetch(["pets", "skills", "battleReplay"])
        self.assertEqual(set(failures), {"pets", "battleReplay"})
        self.assertEqual(self.count_path(urlsplit(good).path), 1)

    def test_prefetch_flash_snapshot_failure_keeps_h5_independent_and_is_shared(self):
        self.flash_versions = {"only/one": self.base_version}
        h5 = subject.H5 + "config/battleconfig~202610110008.aqz"
        self.payloads[h5] = b"independent H5 payload"
        self.session.plan = build_plan(
            [dependency("flash", "pet/petdictionarydata", "pets"),
             dependency("h5", "config/battleconfig.aqz", "skills")],
            [dependency("flash", "battle/battle", "battleReplay")])
        failures = self.session.prefetch(["pets", "skills", "battleReplay"])
        self.assertEqual(set(failures), {"pets", "battleReplay"})
        self.assertEqual(self.count_path("/play/start.xml"), 1)
        self.assertEqual(self.count_path("/play/versiondata~" + self.flash_version + ".swf"), 1)
        self.assertEqual(self.count_path(urlsplit(h5).path), 1)

    def test_prefetch_main_uses_start_version_even_with_other_manifest_and_mix_versions(self):
        self.h5_versions.update({"js/main.js": self.base_version, "js/main.mix": self.override_version})
        self.h5_start["files"]["js/main.js"] = "202610110055"
        self.session.plan = build_plan([], [dependency("h5", "js/main.js", "battleReplay")])
        self.assertEqual(self.session.prefetch(["battleReplay"]), {})
        main_urls = [url for url, _ in self.calls if "/js/main~" in url]
        self.assertEqual(main_urls, [subject.H5 + "js/main~" + self.h5_version + ".js"])
        self.assertEqual(self.session.h5_chain()["main"]["version"], self.h5_version)

    def test_prefetch_retains_both_bootstrap_and_version_map_loader_inputs(self):
        self.flash_versions["loader"] = self.base_version
        bootstrap = subject.FLASH + "loader~" + self.flash_version + ".swf"
        mapped = subject.FLASH + "loader~" + self.base_version + ".swf"
        self.payloads.update({bootstrap: b"bootstrap loader evidence", mapped: b"version map loader evidence"})
        self.session.plan = build_plan([dependency("flash", "loader", "icons", "images", "shop")],
                                       [dependency("flash", "loader", "battleReplay")])
        self.assertEqual(self.session.prefetch(["icons", "battleReplay"]), {})
        self.assertEqual(self.count_path(urlsplit(bootstrap).path), 1)
        self.assertEqual(self.count_path(urlsplit(mapped).path), 1)
        versions, _, _ = self.session.flash_chain()
        self.assertEqual(versions["loader"], self.base_version)
        self.assertEqual(versions.source_hashes["loader"], sha(self.payloads[mapped]))
        self.assertNotEqual(versions.source_hashes["loader"], sha(self.payloads[bootstrap]))

    def test_prefetch_loader_consumers_do_not_require_the_other_loader_revision(self):
        self.flash_versions["loader"] = self.base_version
        bootstrap = subject.FLASH + "loader~" + self.flash_version + ".swf"
        mapped = subject.FLASH + "loader~" + self.base_version + ".swf"
        plan = build_plan([dependency("flash", "loader", "icons", "images", "shop")],
                          [dependency("flash", "loader", "battleReplay")])
        for selected, required, unrelated in ((["battleReplay"], mapped, bootstrap),
                                               (["icons", "shop"], bootstrap, mapped)):
            with self.subTest(selected=selected):
                self.session.reset()
                self.calls.clear()
                self.payloads = {required: b"only selected loader revision is available"}
                self.session.plan = plan
                self.assertEqual(self.session.prefetch(selected), {})
                self.assertEqual(self.count_path(urlsplit(required).path), 1)
                self.assertEqual(self.count_path(urlsplit(unrelated).path), 0)

    def test_prefetch_loader_revision_failure_only_blocks_its_actual_consumers(self):
        self.flash_versions["loader"] = self.base_version
        bootstrap = subject.FLASH + "loader~" + self.flash_version + ".swf"
        mapped = subject.FLASH + "loader~" + self.base_version + ".swf"
        plan = build_plan([dependency("flash", "loader", "icons", "images", "shop")],
                          [dependency("flash", "loader", "battleReplay")])
        for available, blocked in ((mapped, {"icons", "shop"}), (bootstrap, {"battleReplay"})):
            with self.subTest(available=available):
                self.session.reset()
                self.calls.clear()
                self.payloads = {available: b"one loader revision is available"}
                self.session.plan = plan
                self.assertEqual(set(self.session.prefetch(["icons", "shop", "battleReplay"])), blocked)
                self.assertEqual(self.count_path(urlsplit(available).path), 1)

    def test_resource_versions_keep_content_hashes_separate_and_copied(self):
        original = {"pet/petdictionarydata": self.base_version}
        hashes = {"pet/petdictionarydata": sha(b"verified bytes")}
        versions = subject.ResourceVersions(original, hashes)
        self.assertEqual(dict(versions), original)
        self.assertEqual(json.loads(encoded(versions)), original)
        versions["pet/petdictionarydata"] = self.override_version
        versions.source_hashes["pet/petdictionarydata"] = sha(b"other")
        self.assertEqual(original["pet/petdictionarydata"], self.base_version)
        self.assertEqual(hashes["pet/petdictionarydata"], sha(b"verified bytes"))

    def test_resource_matches_checks_version_and_fresh_hash_but_supports_plain_maps(self):
        key = "pet/petdictionarydata"
        versions = subject.ResourceVersions({key: self.base_version}, {key: sha(b"fresh")})
        good = {"version": self.base_version, "sha256": sha(b"fresh")}
        self.assertTrue(subject.resource_matches(good, key, versions))
        for old in ({"version": self.base_version, "sha256": sha(b"stale")},
                    {"version": self.base_version}, {"version": self.override_version, "sha256": good["sha256"]},
                    {}, None):
            with self.subTest(old=old):
                self.assertFalse(subject.resource_matches(old, key, versions))
        self.assertTrue(subject.resource_matches({"version": self.base_version}, key, dict(versions)))
        self.assertTrue(subject.resource_matches({"version": "2000"}, "missing/resource", {}))
        for bad_version in (123, "../bad", "123", None):
            self.assertFalse(subject.resource_matches({"version": bad_version}, key, {key: bad_version}))

    def test_same_version_flash_hotfix_invalidates_previous_content_after_new_run(self):
        key = "pet/petdictionarydata"
        url = subject.FLASH + key + "~" + self.base_version + ".swf"
        plan = build_plan([dependency("flash", key, "pets")], [])
        self.payloads[url] = b"first published dictionary"
        self.session.plan = plan
        self.assertEqual(self.session.prefetch(["pets"]), {})
        before, _, _ = self.session.flash_chain()
        previous = {"version": self.base_version, "sha256": before.source_hashes[key]}
        self.assertTrue(subject.resource_matches(previous, key, before))
        self.payloads[url] = b"same version official hotfix"
        self.session.reset()
        self.session.plan = plan
        self.assertEqual(self.session.prefetch(["pets"]), {})
        after, _, _ = self.session.flash_chain()
        self.assertEqual(dict(before), dict(after))
        self.assertNotEqual(before.source_hashes[key], after.source_hashes[key])
        self.assertFalse(subject.resource_matches(previous, key, after))
        self.assertEqual(self.count_path(urlsplit(url).path), 2)
        after.source_hashes.clear()
        self.assertEqual(self.session.flash_chain()[0].source_hashes[key], sha(self.payloads[url]))

    def test_prefetch_in_parallel_still_downloads_shared_payload_and_snapshot_once(self):
        key = "pet/petdictionarydata"
        url = subject.FLASH + key + "~" + self.base_version + ".swf"
        self.payloads[url] = b"concurrently requested union input"
        self.block_path = urlsplit(url).path
        self.session.plan = build_plan([dependency("flash", key, "pets")],
                                       [dependency("flash", key, "battleReplay")])
        with ThreadPoolExecutor(max_workers=2) as pool:
            first = pool.submit(self.session.prefetch, ["pets"])
            self.assertTrue(self.entered.wait(5))
            second = pool.submit(self.session.prefetch, ["battleReplay"])
            self.release.set()
            self.assertEqual(first.result(timeout=5), {})
            self.assertEqual(second.result(timeout=5), {})
        self.assertEqual(self.count_path(self.block_path), 1)
        self.assertEqual(self.count_path("/play/start.xml"), 1)
        self.assertEqual(self.count_path("/play/versiondata~" + self.flash_version + ".swf"), 1)

    def test_prefetch_independent_resources_run_concurrently(self):
        rows = [dependency("flash", key, "pets") for key in ("pet/petdictionarydata", "battle/battle")]
        self.session.plan = build_plan(rows, [])
        urls = [subject.FLASH + "pet/petdictionarydata~" + self.base_version + ".swf",
                subject.FLASH + "battle/battle~" + self.override_version + ".swf"]
        self.payloads.update({url: b"independent resource" for url in urls})
        original = self.session.transport
        gate = threading.Barrier(2)
        def parallel_transport(url, output, *, runtime=False):
            if url in urls:
                gate.wait(timeout=5)
            return original(url, output, runtime=runtime)
        self.session.transport = parallel_transport
        self.assertEqual(self.session.prefetch(["pets"]), {})
        self.assertTrue(all(self.count_path(urlsplit(url).path) == 1 for url in urls))


if __name__ == "__main__":
    unittest.main()
