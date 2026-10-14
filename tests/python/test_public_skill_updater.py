"""Offline skill-update contracts; fixtures are synthetic, not game evidence.

The shared-session tests own start.files/mix/default URL resolution and download
deduplication. These tests verify that all workbench inputs use that session,
that byte changes cannot hide behind an unchanged release, and that activation
retains the old catalog on any incomplete/unsafe input or changing entry.
"""
from __future__ import annotations

import copy
import hashlib
import io
import json
from pathlib import Path
import stat
import sys
import tempfile
import types
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import public_skill_updater as subject
from generate_pet_skill_data import PET_FIELDS, SKILL_SLOTS


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def sha(value):
    return hashlib.sha256(value).hexdigest()


def archive_bytes(members):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in members:
            archive.writestr(name, value)
    return output.getvalue()


def input_tables(*, pets=1, skills=1, entries=1):
    """Complete synthetic generator inputs; these are not official evidence."""
    dictionary = {}
    for race in range(1, pets + 1):
        row = [""] * len(PET_FIELDS)
        row[0], row[1] = race, "synthetic pet"
        for field in SKILL_SLOTS.values():
            row[PET_FIELDS.index(field)] = 0
        row[PET_FIELDS.index("normalSkill")] = 1
        row[PET_FIELDS.index("skillEntries")] = "entry0"
        row[PET_FIELDS.index("qiYunType")] = 0
        if race == 1:
            row[PET_FIELDS.index("signs")] = "寰力量"
            row[PET_FIELDS.index("yuanLiSkill")] = 1
        dictionary[str(race)] = row
    return {
        "petdictionarydata.json/petdictionarydata.json": dictionary,
        "battleconfig/skill_desc_config.json": {str(n): "synthetic skill|description " + str(n) for n in range(1, skills + 1)},
        "battleconfig/skill_config.json": {"1": {"action": "test"}},
        "battleconfig/combo_buffs_config.json": {"combo": [{"skillId": 1, "buffs": "1", "targetSkillIds": "2"}]},
        "battleconfig/entry_config.json": [{"n": "entry" + str(n), "d": "description"} for n in range(entries)],
        "battleconfig/buffer_config.json": {"buff": {"1": "synthetic buff|description||effect"}},
        "formation/transformskillconfig.json": {"1": [1, "transform", "description", "team", [1], [1], 1]},
        "formation/heroskillconfig.json": {"1": [1, "hero", {"unknownFutureField": [1, 2]}]},
        "formation/psychicsskillconfig.json": {"1": [1, "psychics"]},
        "formation/lingchuskillsulingconfig.json": {"1": {"fixture": "value"}},
        "formation/shenyunskillqiyunconfig.json": {"1": {"fixture": "value"}},
        "relation/relationconfig.json": {"relationTeam": [[1, "team", "1", "phase", "1", 0, "1", "era"]],
                                          "relationPoint": [[0, 1, 0, "1", 0, 0, 1, 1, 1]],
                                          "relationEffect": [[1, 0, "effect"]]},
        "pet/petinfoconfig.json": {"1": [0, 0, 0, "role", "official role", 0, "hexagon"]},
        "pet/petevoconfig.json": {"1": [0, 0, 0, 2]},
        "pet/tiebaconfig.json": [{"raceIds": [1, 2]}],
        "battleconfig/summon_config.json": [{"s": "1", "sed": "2", "sdes": "summoner", "des": "summon"}],
        "battleconfig/carry_config.json": [{"c": "1", "ced": "2", "sdes": "carry"}],
        "battleconfig/huan_skill_config.json": {"1": ["huan"]},
        "battleconfig/almighty-skill-config.json": [{"id": 1, "hn": "hero", "hd": "description", "psci": 2}],
        "petdata.json/petdata.json": [{"RaceId": 1, "BaseStatuses": "1#2", "BasicZhanDouLi": 1}],
        "changepetskin/changepetskinconfig.json": {"defines": [{"name": "skin", "availablePets": [{"raceIdNeed": 1, "raceId": 2}]}]},
    }


def change_archive(payload, *, changes=None, removed=()):
    with zipfile.ZipFile(io.BytesIO(payload)) as package:
        values = {item.filename: package.read(item) for item in package.infolist() if item.filename not in removed}
    values.update(changes or {})
    return archive_bytes(values.items())


def raw_member(name, *, mode=stat.S_IFREG | 0o644):
    # ZipInfo(name) normalizes '\\' on Windows. Assign afterward so the ZIP
    # really contains the malicious raw spelling, including embedded NUL.
    info = zipfile.ZipInfo("placeholder")
    info.filename = name
    info.orig_filename = name
    info.create_system = 3
    info.external_attr = mode << 16
    return info


class OfflineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="skill-public-update-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        network = mock.patch("urllib.request.urlopen", side_effect=AssertionError("Real network is forbidden"))
        network.start()
        self.addCleanup(network.stop)


class SafeArchiveTests(OfflineTest):
    def extract(self, members, name="extracted"):
        output = self.root / name
        subject.safe_extract(archive_bytes(members), output)
        return output

    def test_keeps_nested_json_unknown_fields_and_opaque_members(self):
        expected = {"nested/all.json": encoded({"unusedByUi": {"x": [1, 2, 3]}}),
                    "opaque.bin": b"\x00\xfforiginal", "empty.txt": b""}
        output = self.extract([("nested/", b""), *expected.items()])
        self.assertEqual({str(path.relative_to(output)).replace("\\", "/"): path.read_bytes()
                          for path in output.rglob("*") if path.is_file()}, expected)

    def test_rejects_windows_and_normalization_aliases_before_any_write(self):
        names = ("../outside", "/absolute", "C:/absolute", "C:relative", "a//b", "a/./b",
                 "a/../b", "a\\b", "good\0hidden", "a.", "a ", "nul.json", "COM1/x",
                 "x:stream", "question?", "star*", "quote\"", "angle<", "pipe|", "bad\x1f")
        for index, name in enumerate(names):
            output = self.root / str(index)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsafe"):
                subject.safe_extract(archive_bytes([("valid.json", b"{}"), (raw_member(name), b"x")]), output)
            self.assertFalse(output.exists(), "Unsafe member lists must fail before extracting valid entries")

    def test_rejects_duplicate_case_alias_and_file_directory_conflict(self):
        pairs = ([('x.json', b'1'), ('X.json', b'2')],
                 [('folder', b'1'), ('folder/', b'')],
                 [('folder/data.json', b'{}'), ('folder', b'file')])
        for index, members in enumerate(pairs):
            with self.subTest(members=members), self.assertRaises(ValueError):
                self.extract(members, str(index))
            self.assertFalse((self.root / str(index)).exists())

    def test_rejects_symbolic_links_and_other_special_members(self):
        for mode in (stat.S_IFLNK | 0o777, stat.S_IFIFO | 0o600, stat.S_IFCHR | 0o600):
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, "member type"):
                self.extract([(raw_member("link", mode=mode), b"../outside")], str(mode))

    def test_rejects_existing_output_without_overwriting_it(self):
        target = self.root / "extracted" / "kept.json"
        target.parent.mkdir()
        target.write_bytes(b"kept")
        with self.assertRaisesRegex(ValueError, "overwrite"):
            self.extract([("kept.json", b"replacement")])
        self.assertEqual(target.read_bytes(), b"kept")

    def test_archive_count_download_member_and_total_limits(self):
        payload = archive_bytes([("first", b"1234"), ("second", b"5678")])
        cases = (("MAX_DOWNLOAD_BYTES", len(payload) - 1, "download"),
                 ("MAX_ARCHIVE_FILES", 1, "too many"),
                 ("MAX_MEMBER_BYTES", 3, "member"),
                 ("MAX_ARCHIVE_BYTES", 7, "expansion"))
        for key, limit, error in cases:
            with self.subTest(key=key), mock.patch.object(subject, key, limit), \
                    self.assertRaisesRegex(ValueError, error):
                subject.safe_extract(payload, self.root / key)
            self.assertFalse((self.root / key).exists())


class FakeSession:
    def __init__(self):
        self.version = "202610110001"
        self.requests = []
        self.stability_checks = []
        self.fail_resource = None
        self.corrupt_resource = None
        self.unstable = False
        self.set_tables(input_tables())
        self.versions = {path: self.version for path in self.payloads}

    def set_tables(self, tables):
        self.payloads = {}
        for resource in subject.ARCHIVES:
            folder = Path(resource).name.removesuffix(".aqz") + "/"
            members = [(name.removeprefix(folder), encoded(value)) for name, value in tables.items()
                       if name.startswith(folder)]
            members.append(("complete.json", encoded({"source": resource, "unknown": [1, 2]})))
            self.payloads[resource] = archive_bytes(members)
        self.payloads.update({path: encoded(tables[path.removeprefix("config/")]) for path in subject.DIRECT})

    def h5_chain(self):
        start = encoded({"version": self.version, "files": {}})
        return {"version": self.version, "versions": dict(self.versions), "overrides": {}, "start": start,
                "entry": {"url": subject.H5 + "start~123.json", "sha256": sha(start), "bytes": len(start)},
                "manifest": {"url": subject.H5 + "version~" + self.version + ".json",
                             "sha256": sha(encoded(self.versions)), "bytes": len(encoded(self.versions))},
                "main": {"url": subject.H5 + "js/main~" + self.version + ".js",
                         "sha256": sha(b"synthetic reviewed loader"), "bytes": 25}}

    def h5_resource(self, resource, destination):
        self.requests.append(resource)
        if resource == self.fail_resource:
            raise OSError("synthetic download failure")
        raw = self.payloads[resource]
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        record = {"url": subject.H5 + subject.versioned(resource, self.versions[resource]),
                  "resource": resource, "version": self.versions[resource],
                  "sha256": sha(raw), "bytes": len(raw), "resolution": "session-resolved",
                  "file": str(destination), "retrievedUtc": "deliberately volatile"}
        if resource == self.corrupt_resource:
            record["sha256"] = "0" * 64
        return destination, record

    def h5_signature(self, keys):
        return sha(encoded({key: self.versions[key] for key in keys}))

    def assert_stable(self, kinds=("h5",)):
        self.stability_checks.append(kinds)
        if self.unstable:
            raise ValueError("synthetic official entry changed")


class SharedSkillUpdaterTests(OfflineTest):
    """Transaction/resource plumbing with an intentionally synthetic builder.

    The source fidelity reader is outside this class's boundary: this fake
    builder's 9000/20000 rows deliberately do not correspond to its tiny input
    fixture. RealBuilderTests below runs the real reader and generator together.
    """
    def setUp(self):
        super().setUp()
        self.session = FakeSession()
        self.catalog = self.root / "catalog"
        self.catalog.mkdir()
        self.target = self.catalog / "pet-skill-data.json"
        self.updater = types.SimpleNamespace(public_resources=self.session, catalog=self.catalog,
                                             current=lambda key: json.loads(self.target.read_bytes())
                                             if self.target.is_file() else {})
        self.writes = []
        self.builds = []
        self.iteration = 0
        self.builder = mock.patch.object(subject, "build_catalog", side_effect=self.build_catalog)
        self.builder.start()
        self.addCleanup(self.builder.stop)
        fidelity = mock.patch("public_skill_fidelity.audit_skill_fidelity", side_effect=lambda root, catalog, **kw: {
            "schemaVersion": 1, "passed": True, "candidateContentSha256": subject._digest(catalog),
            "inputs": {"synthetic-transaction-boundary": {"file": "fixture", "sha256": "0" * 64}},
            "summary": {"errors": 0}, "findings": [], "allowedCorrections": [],
            "boundaries": ["Mock used only to isolate session/transaction plumbing from source fidelity"]})
        fidelity.start()
        self.addCleanup(fidelity.stop)

    def build_catalog(self, root, source):
        self.builds.append((root, copy.deepcopy(source)))
        for resource in subject.ARCHIVES:
            folder = Path(resource).name.removesuffix(".aqz")
            self.assertEqual((root / folder / "complete.json").read_bytes(),
                             zipfile.ZipFile(io.BytesIO(self.session.payloads[resource])).read("complete.json"))
        for resource in subject.DIRECT:
            self.assertEqual((root / resource.removeprefix("config/")).read_bytes(), self.session.payloads[resource])
        return {"schema": 1, "source": source,
                "pets": {str(n): {"name": "synthetic pet"} for n in range(9000)},
                "skills": {str(n): {"name": "synthetic skill"} for n in range(20000)},
                "entries": {str(n): "synthetic entry" for n in range(100)},
                "unknownFutureTable": {"complete": [1, 2, {"untouched": True}]},
                "heroSkills": {"1": ["original", "fields"]}, "buffs": {"1": {"name": "fixture"}},
                "combos": {"1": [{"buff": 1, "skill": 2}]}, "transformSkills": {"1": {"name": "fixture"}},
                "psychicsSkills": {"1": [1]}, "lingchuRules": {"1": {"fixture": "value"}},
                "shenyunRules": {"1": {"fixture": "value"}}, "almightySkills": [{"id": 1}]}

    def writer(self, path, value):
        self.writes.append(path)
        path.write_bytes(encoded(value))

    def run_update(self):
        self.iteration += 1
        self.updater.scratch = self.root / ("scratch-" + str(self.iteration))
        self.updater.scratch.mkdir()
        return subject.update_skills(self.updater, self.writer)

    def test_all_ten_resources_come_from_session_and_all_builder_tables_survive(self):
        self.assertTrue(self.run_update())
        self.assertEqual(self.session.requests, list(subject.ARCHIVES + subject.DIRECT))
        self.assertEqual(self.session.stability_checks, [("h5",)])
        saved = json.loads(self.target.read_bytes())
        self.assertEqual(saved["unknownFutureTable"], {"complete": [1, 2, {"untouched": True}]})
        self.assertEqual(saved["heroSkills"], {"1": ["original", "fields"]})
        resources = saved["source"]["resources"]
        self.assertEqual(set(resources), set(subject.ARCHIVES + subject.DIRECT))
        for row in resources.values():
            self.assertEqual(row["resolution"], "session-resolved")
            self.assertTrue(row["url"].startswith(subject.H5))
            self.assertNotIn("file", row)
            self.assertNotIn("retrievedUtc", row)
        extracted = saved["source"]["extractedFiles"]
        self.assertEqual(set(extracted), set(subject.ARCHIVES + subject.DIRECT))
        for resource in subject.ARCHIVES:
            with zipfile.ZipFile(io.BytesIO(self.session.payloads[resource])) as package:
                expected = [{"member": item.filename, "bytes": item.file_size, "sha256": sha(package.read(item))}
                            for item in package.infolist()]
            self.assertEqual(extracted[resource], expected)
        for resource in subject.DIRECT:
            self.assertEqual(extracted[resource][0]["sha256"], sha(self.session.payloads[resource]))

    def test_identical_bytes_and_metadata_skip_build_and_write_after_all_download_checks(self):
        self.run_update()
        original = self.target.read_bytes()
        timestamp = self.target.stat().st_mtime_ns
        self.session.requests.clear()
        self.assertFalse(self.run_update())
        self.assertEqual(len(self.builds), 1)
        self.assertEqual(len(self.writes), 1)
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(self.target.stat().st_mtime_ns, timestamp)
        self.assertEqual(self.session.requests, list(subject.ARCHIVES + subject.DIRECT))
        self.assertEqual(self.session.stability_checks, [("h5",), ("h5",)])

    def test_same_release_and_resource_version_with_changed_bytes_rebuilds(self):
        self.run_update()
        resource = subject.ARCHIVES[0]
        self.session.payloads[resource] = change_archive(self.session.payloads[resource],
                                                       changes={"complete.json": encoded({"sameVersionNewData": True})})
        self.assertTrue(self.run_update())
        self.assertEqual(len(self.builds), 2)
        saved = json.loads(self.target.read_bytes())
        self.assertEqual(saved["source"]["version"], self.session.version)
        self.assertEqual(saved["source"]["resources"][resource]["sha256"], sha(self.session.payloads[resource]))

    def test_new_release_same_bytes_updates_provenance_without_rebuilding(self):
        self.run_update()
        self.session.version = "202610110002"
        self.session.versions[subject.ARCHIVES[0]] = "202610110099"
        self.assertTrue(self.run_update())
        self.assertEqual(len(self.builds), 1)
        saved = json.loads(self.target.read_bytes())
        self.assertEqual(saved["source"]["version"], "202610110002")
        self.assertEqual(saved["source"]["resources"][subject.ARCHIVES[0]]["version"], "202610110099")

    def test_parser_change_rebuilds_same_official_bytes(self):
        self.run_update()
        with mock.patch.object(subject, "_parser_fingerprint", return_value="new-reviewed-parser"):
            self.assertTrue(self.run_update())
        self.assertEqual(len(self.builds), 2)

    def test_old_release_only_cache_is_rebuilt_to_complete_resource_fingerprint(self):
        self.target.write_bytes(encoded({"schema": 1, "source": {"version": self.session.version},
                                        "pets": {str(n): {} for n in range(9000)},
                                        "skills": {str(n): {} for n in range(20000)}}))
        self.assertTrue(self.run_update())
        self.assertEqual(len(self.builds), 1)
        self.assertIn("contentFingerprint", json.loads(self.target.read_bytes())["source"])

    def test_stable_entry_recheck_is_required_even_for_unchanged_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        self.session.unstable = True
        with self.assertRaisesRegex(ValueError, "entry changed"):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)

    def test_download_failure_and_corrupt_shared_cache_preserve_old_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        self.session.fail_resource = subject.DIRECT[-1]
        with self.assertRaises(OSError):
            self.run_update()
        self.session.fail_resource = None
        self.session.corrupt_resource = subject.ARCHIVES[0]
        with self.assertRaisesRegex(ValueError, "cache failed validation"):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)

    def test_unsafe_archive_preserves_old_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        self.session.payloads[subject.ARCHIVES[-1]] = archive_bytes([(raw_member("../outside"), b"x")])
        with self.assertRaisesRegex(ValueError, "unsafe"):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)

    def test_invalid_direct_table_preserves_old_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        self.session.payloads[subject.DIRECT[-1]] = b"not-json"
        with self.assertRaises(json.JSONDecodeError):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)

    def test_incomplete_new_catalog_preserves_old_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        self.session.payloads[subject.DIRECT[0]] = encoded({"new": ["data"]})
        with mock.patch.object(subject, "build_catalog", return_value={"schema": 1, "pets": {}, "skills": {}}), \
                self.assertRaisesRegex(ValueError, "incomplete|解析质量检查"):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)


class RealBuilderTests(OfflineTest):
    def setUp(self):
        super().setUp()
        self.session = FakeSession()
        self.session.set_tables(input_tables(pets=9000, skills=20000, entries=100))
        self.catalog = self.root / "catalog"
        self.catalog.mkdir()
        self.target = self.catalog / "pet-skill-data.json"
        self.updater = types.SimpleNamespace(public_resources=self.session, catalog=self.catalog,
                                             current=lambda key: json.loads(self.target.read_bytes())
                                             if self.target.is_file() else {})
        self.writes, self.iteration = [], 0

    def run_update(self):
        self.iteration += 1
        self.updater.scratch = self.root / ("scratch-" + str(self.iteration))
        self.updater.scratch.mkdir()
        def writer(path, value):
            self.writes.append(path)
            path.write_bytes(encoded(value))
        return subject.update_skills(self.updater, writer)

    def test_real_complete_builder_keeps_all_tables_and_unknown_raw_fields(self):
        self.assertTrue(self.run_update())
        saved = json.loads(self.target.read_bytes())
        self.assertEqual(set(saved["source"]["validatedInputs"]),
                         {names[0] for names, _, _ in subject.REQUIRED_INPUTS})
        self.assertTrue(all(saved[key] for key in subject._OUTPUT_TYPES))
        self.assertEqual(saved["heroSkills"]["1"][2], {"unknownFutureField": [1, 2]})
        self.assertTrue(all(saved["pets"]["1"][field] for field in
                            ("baseStats", "relations", "summons", "carry", "skins", "forms", "huan", "almighty")))
        self.assertFalse(self.run_update())
        self.assertEqual(len(self.writes), 1)

    def test_real_builder_missing_buff_sheet_keeps_previous_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        resource = "config/battleconfig.aqz"
        self.session.payloads[resource] = change_archive(self.session.payloads[resource],
                                                       removed=("buffer_config.json",))
        with self.assertRaisesRegex(ValueError, "required official skill sheet.*buffer_config"):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)

    def test_real_builder_empty_association_sheet_keeps_previous_catalog(self):
        self.run_update()
        original = self.target.read_bytes()
        resource = "config/battleconfig.aqz"
        self.session.payloads[resource] = change_archive(self.session.payloads[resource],
                                                       changes={"summon_config.json": b"[]"})
        with self.assertRaises(ValueError):
            self.run_update()
        self.assertEqual(self.target.read_bytes(), original)
        self.assertEqual(len(self.writes), 1)
        report = json.loads((self.root / "data-tools/quality/skills-latest-attempt.json").read_bytes())
        self.assertFalse(report["passed"])
        self.assertTrue(any("summons" in row["path"] for row in report["findings"] if row["severity"] != "info"))

    def test_unchanged_fingerprint_cache_still_checks_official_skill_text(self):
        self.run_update()
        current = json.loads(self.target.read_bytes())
        current["skills"]["1"]["description"], current["skills"]["2"]["description"] = \
            current["skills"]["2"]["description"], current["skills"]["1"]["description"]
        self.target.write_bytes(encoded(current))
        damaged = self.target.read_bytes()
        with mock.patch.object(subject, "build_catalog", side_effect=AssertionError("Same fingerprint must use the cached candidate")), \
                self.assertRaisesRegex(ValueError, "解析质量检查"):
            self.run_update()
        self.assertEqual(damaged, self.target.read_bytes())
        self.assertEqual(len(self.writes), 1)
        report = json.loads((self.root / "data-tools/quality/skills-latest-attempt.json").read_bytes())
        self.assertFalse(report["sourceFidelity"]["passed"])
        self.assertEqual({"/skills/1/description", "/skills/2/description"},
                         {row["path"] for row in report["sourceFidelity"]["findings"]})

    def test_real_builder_equal_counts_and_valid_ids_cannot_swap_descriptions(self):
        self.run_update()
        original = self.target.read_bytes()
        build = subject.build_catalog
        def swapped(root, source):
            result = build(root, source)
            a, b = result["skills"]["1"], result["skills"]["2"]
            a["description"], b["description"] = b["description"], a["description"]
            return result
        with mock.patch.object(subject, "_parser_fingerprint", return_value="synthetic-parser-update"), \
                mock.patch.object(subject, "build_catalog", side_effect=swapped), \
                self.assertRaisesRegex(ValueError, "解析质量检查"):
            self.run_update()
        self.assertEqual(original, self.target.read_bytes())
        self.assertEqual(len(self.writes), 1)
        report = json.loads((self.root / "data-tools/quality/skills-latest-attempt.json").read_bytes())
        self.assertFalse(report["sourceFidelity"]["passed"])
        self.assertEqual(2, report["sourceFidelity"]["summary"]["errors"])

    def legacy_wrong_associations(self):
        self.run_update()
        old = json.loads(self.target.read_bytes())
        pet = old["pets"]["2"]
        pet["huan"] = ["wrong suffix association"]
        pet["almighty"] = [dict(old["pets"]["1"]["almighty"][0], id=10001)]
        pet["traits"] = ["寰力量"]
        self.target.write_bytes(encoded(old))
        return self.target.read_bytes()

    def test_official_exact_association_corrections_can_replace_legacy_wrong_joins(self):
        self.legacy_wrong_associations()
        with mock.patch.object(subject, "_parser_fingerprint", return_value="official-exact-join-fix"):
            self.assertTrue(self.run_update())
        saved = json.loads(self.target.read_bytes())
        self.assertNotIn("huan", saved["pets"]["2"])
        self.assertNotIn("almighty", saved["pets"]["2"])
        self.assertEqual([], saved["pets"]["2"]["traits"])
        report = json.loads((self.root / "data-tools/quality/skills-latest-attempt.json").read_bytes())
        self.assertTrue(report["sourceFidelity"]["passed"])
        self.assertTrue(report["passed"])
        self.assertEqual({"/pets/2/huan", "/pets/2/almighty", "/pets/2/traits"},
                         {row["path"] for row in report["sourceFidelity"]["allowedCorrections"]})
        self.assertTrue(any(row.get("resolution") == "legacy-association-corrected-from-official-consumer"
                            for row in report["findings"]))

    def test_proven_association_correction_does_not_excuse_other_field_loss(self):
        old = self.legacy_wrong_associations()
        build = subject.build_catalog
        def loses_unrelated_field(root, source):
            result = build(root, source)
            del result["pets"]["1"]["baseStats"]
            return result
        with mock.patch.object(subject, "_parser_fingerprint", return_value="official-exact-join-fix"), \
                mock.patch.object(subject, "build_catalog", side_effect=loses_unrelated_field), \
                self.assertRaisesRegex(ValueError, "解析质量检查"):
            self.run_update()
        self.assertEqual(old, self.target.read_bytes())
        report = json.loads((self.root / "data-tools/quality/skills-latest-attempt.json").read_bytes())
        self.assertTrue(report["sourceFidelity"]["passed"])
        self.assertFalse(report["passed"])
        self.assertTrue(any(row["path"] == "/pets/1/baseStats" and row["severity"] == "review"
                            for row in report["findings"]))

    def test_all_required_input_types_and_missing_files_are_checked(self):
        root = self.root / "input-types"
        for name, value in input_tables().items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(encoded(value))
        self.assertEqual(len(subject._validate_inputs(root)), 21)
        for alternatives, kind, _ in subject.REQUIRED_INPUTS:
            path = root / alternatives[0]
            original = path.read_bytes()
            with self.subTest(sheet=alternatives[0]):
                path.unlink()
                with self.assertRaisesRegex(ValueError, "required official skill sheet"):
                    subject._validate_inputs(root)
                path.write_bytes(b"[]" if kind is dict else b"{}")
                with self.assertRaisesRegex(ValueError, "unsupported type"):
                    subject._validate_inputs(root)
                path.write_bytes(original)
        path = root / "battleconfig/buffer_config.json"
        path.write_bytes(encoded({"buff": []}))
        with self.assertRaisesRegex(ValueError, "unsupported type.*buffer_config.json/buff"):
            subject._validate_inputs(root)

    def test_saved_full_official_archives_pass_real_builder_and_baseline_guards(self):
        official = ROOT / "build-battle-reference/cross-resource-audit-20261009/h5-all-config/raw"
        paths = {resource: official / resource for resource in subject.ARCHIVES + subject.DIRECT}
        if not all(path.is_file() for path in paths.values()):
            self.skipTest("Saved official research archives are not distributed with the unit tests")
        # Read-only recorded evidence: no network and no installed user data.
        self.session.payloads = {resource: path.read_bytes() for resource, path in paths.items()}
        self.updater.baseline = ROOT / "assets"
        self.assertTrue(self.run_update())
        saved = json.loads(self.target.read_bytes())
        self.assertEqual(len(saved["source"]["validatedInputs"]), 21)
        self.assertGreaterEqual(len(saved["pets"]), 9671)
        self.assertGreaterEqual(len(saved["buffs"]), 31234)

    def test_dictionary_ids_and_column_contract_cannot_be_silently_coerced(self):
        root = self.root / "strict-inputs"
        tables = input_tables()
        for name, value in tables.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(encoded(value))
        path = root / "petdictionarydata.json/petdictionarydata.json"
        original = tables["petdictionarydata.json/petdictionarydata.json"]
        for value in (True, "1", 1.5, -1, None):
            with self.subTest(skill=value):
                table = copy.deepcopy(original)
                table["1"][PET_FIELDS.index("normalSkill")] = value
                path.write_bytes(encoded(table))
                with self.assertRaisesRegex(ValueError, "invalid identity or skill reference"):
                    subject._validate_inputs(root)
        for kind in ("identity", "key", "new-column"):
            with self.subTest(kind=kind):
                table = copy.deepcopy(original)
                if kind == "identity":
                    table["1"][0] = 2
                elif kind == "key":
                    table["01"] = table.pop("1")
                else:
                    table["1"].append("not yet interpreted")
                path.write_bytes(encoded(table))
                with self.assertRaisesRegex(ValueError, "inconsistent identity|new columns"):
                    subject._validate_inputs(root)


class PreservationGuardTests(OfflineTest):
    def setUp(self):
        super().setUp()
        self.catalog = json.loads((ROOT / "assets/pet-skill-data.json").read_bytes())

    def test_each_output_table_cannot_silently_shrink(self):
        for key in subject._OUTPUT_TYPES:
            candidate = copy.deepcopy(self.catalog)
            old = candidate[key]
            keep = max(0, int(len(old) * .8))
            candidate[key] = dict(list(old.items())[:keep]) if isinstance(old, dict) else old[:keep]
            with self.subTest(table=key), self.assertRaisesRegex(ValueError, "incomplete|lost required data"):
                subject._validate_preservation(candidate, self.catalog, {})

    def test_each_existing_pet_association_and_skill_animation_cannot_disappear(self):
        for table, fields in (("pets", subject._PET_ASSOCIATIONS), ("skills", {"animation": dict})):
            for field in fields:
                if not any(bool(row.get(field)) for row in self.catalog[table].values()):
                    continue
                candidate = copy.deepcopy(self.catalog)
                for row in candidate[table].values():
                    row.pop(field, None)
                with self.subTest(table=table, field=field), self.assertRaisesRegex(ValueError, "lost required data.*" + field):
                    subject._validate_preservation(candidate, self.catalog, {})

    def test_each_reference_can_supply_the_larger_minimum(self):
        larger = copy.deepcopy(self.catalog)
        larger["buffs"].update({"new-fixture-" + str(n): {"name": "fixture"}
                                for n in range(len(larger["buffs"]))})
        for current, baseline in ((larger, self.catalog), (self.catalog, larger)):
            with self.subTest(larger_is_current=current is larger), self.assertRaisesRegex(ValueError, "lost required data.*buffs"):
                subject._validate_preservation(self.catalog, current, baseline)


if __name__ == "__main__":
    unittest.main()
