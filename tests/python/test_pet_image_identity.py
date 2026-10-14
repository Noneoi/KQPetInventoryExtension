"""Exact official image identity and immutable-version cache contracts."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_data_updater as updater
import public_icon_updater as icons


class PetImageIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.entry = updater.image_entries({}, ("123",), default_version="2000")["123"]
        updater.atomic_json(self.root / "catalog/pet-image-index.json", {"schemaVersion": 1, "faces": {"123": self.entry}})
        network = patch("urllib.request.urlopen", side_effect=AssertionError("no real network"))
        network.start()
        self.addCleanup(network.stop)

    def cached(self, path, entry=None):
        entry = entry or self.entry
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"test-only encoded contents; decoding has separate C++ coverage")
        updater.atomic_json(path.with_suffix(".json"), {"url": entry["swfUrl"], "symbol": entry["symbol"],
                            "revision": entry["revision"], "pngSha256": updater.digest(path)})
        return path

    def test_no_implicit_default_and_future_verified_default(self):
        with self.assertRaisesRegex(ValueError, "默认版本"):
            updater.image_entries({}, ("123",))
        rows = updater.image_entries({}, ("123", "10123", "10366"), (10366,), default_version="3001")
        self.assertEqual(rows["123"], rows["10123"])
        self.assertTrue(rows["10366"]["swfUrl"].endswith("peticon10366~3001.swf"))
        expected = hashlib.sha256(json.dumps([self.entry["swfUrl"],self.entry["symbol"],"2000"],
                                            separators=(",", ":")).encode()).hexdigest()
        self.assertEqual(self.entry["sourceIdentity"], expected)

    def test_resource_symbol_and_version_must_agree(self):
        for change in ({"symbol":"mmo.pet.peticon124_L"}, {"revision":"3001"}, {"sourceIdentity":"0"*64}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                updater.picture_source_identity({**self.entry, **change})

    def test_explicit_high_face_exception_never_falls_back(self):
        index = {"faces": {"366": self.entry}, "faceIdExceptions": [10366]}
        self.assertEqual(updater.picture_entry(index, "123_10366"), {})

    def test_proven_legacy_cache_migrates_and_preserves_original(self):
        legacy = self.cached(self.root / "images/pets/123_0.png")
        old = legacy.read_bytes()
        with patch.object(updater,"fetch",side_effect=AssertionError("must stay offline")):
            result = updater.extract_image(self.root,"123_0")
        output = updater.picture_cache_path(self.root,"123_0",self.entry)
        self.assertEqual(Path(result["path"]),output)
        self.assertEqual(output.read_bytes(),old)
        self.assertEqual(legacy.read_bytes(),old)
        self.assertEqual(updater.read_object(output.with_suffix(".json"))["sourceIdentity"], self.entry["sourceIdentity"])

    def test_unknown_legacy_contents_are_retained_not_treated_as_cache_hit(self):
        legacy = self.root / "images/pets/123_0.png"
        legacy.parent.mkdir(parents=True)
        legacy.write_bytes(b"unproven historical file")
        with patch.object(updater,"installed_runtime",side_effect=ValueError("tool missing")):
            with self.assertRaisesRegex(ValueError,"tool missing"):
                updater.extract_image(self.root,"123_0")
        self.assertEqual(legacy.read_bytes(),b"unproven historical file")
        self.assertFalse(updater.picture_cache_path(self.root,"123_0",self.entry).exists())

    def test_version_change_keeps_previous_snapshot_path(self):
        old = self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        new = updater.image_entries({}, ("123",), default_version="3001")["123"]
        self.assertNotEqual(old,updater.picture_cache_path(self.root,"123_0",new))
        updater.atomic_json(self.root / "catalog/pet-image-index.json", {"faces":{"123":new}})
        with patch.object(updater,"fetch",side_effect=AssertionError("no newer-source fetch")):
            with self.assertRaisesRegex(ValueError,"来源快照"):
                updater.extract_image(self.root,"123_0",expected_source_identity=self.entry["sourceIdentity"])
        self.assertTrue(old.exists())

    def test_other_race_cannot_supply_an_exact_key(self):
        updater.atomic_json(self.root / "catalog/pet-image-index.json", {"images":{"124_0":self.entry}})
        with self.assertRaisesRegex(ValueError,"外观编号"):
            updater.extract_image(self.root,"124_0")

    def test_corrupted_proven_cache_is_not_a_hit(self):
        path = self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        path.write_bytes(b"changed after metadata")
        with patch.object(updater,"installed_runtime",side_effect=ValueError("tool missing")):
            with self.assertRaisesRegex(ValueError,"tool missing"):
                updater.extract_image(self.root,"123_0")

    def test_images_only_calls_existing_loader_guard_and_saves_its_proof(self):
        baseline = self.root / "baseline"; baseline.mkdir()
        scratch = self.root / "scratch"; scratch.mkdir()
        subject = updater.Updater(self.root, baseline, scratch, isolated_workspace=True)
        subject.start_version = "20261014123456"
        subject.runtimes = (Path("synthetic-java"),Path("synthetic-ffdec"))
        subject.current = lambda part: {"pets":{"123":{}}} if part=="pets" else {}
        subject.icon_exceptions = lambda versions,current: ([],{"version":"test-only"})
        proof = {"version":subject.start_version,"defaultVersion":"3001","sha256":"f"*64}
        with patch.object(icons,"loader_default",return_value=proof) as guard:
            self.assertTrue(subject.images({}))
        guard.assert_called_once_with(subject,{})
        saved = updater.read_object(self.root / "catalog/pet-image-index.json")
        self.assertEqual(saved["source"]["loaderDefault"],proof)
        self.assertEqual(saved["faces"]["123"]["revision"],"3001")

    def test_changed_loader_rule_stops_images_before_replacing_index(self):
        baseline = self.root / "baseline"; baseline.mkdir()
        scratch = self.root / "scratch"; scratch.mkdir()
        subject = updater.Updater(self.root,baseline,scratch,isolated_workspace=True)
        subject.start_version = "20261014123456"
        def fetch(url,path):
            path.write_bytes(b"synthetic loader")
            return {"url":url,"sha256":updater.digest(path)}
        subject.fetch = fetch
        source = scratch / "VersManager.as"
        source.write_text('const DEFAULT_VERSION:String = "3001"; return changedVersionRule;',encoding="utf-8")
        subject.export = Mock(return_value=source)
        original = (self.root / "catalog/pet-image-index.json").read_bytes()
        with self.assertRaisesRegex(ValueError,"版本规则已变化"):
            subject.images({})
        self.assertEqual((self.root / "catalog/pet-image-index.json").read_bytes(),original)

    def test_failed_cached_refresh_preserves_active_source_index(self):
        baseline = self.root / "baseline"; baseline.mkdir()
        scratch = self.root / "scratch"; scratch.mkdir()
        subject = updater.Updater(self.root,baseline,scratch,isolated_workspace=True)
        subject.start_version = "20261014123456"
        subject.runtimes = (Path("synthetic-java"),Path("synthetic-ffdec"))
        previous = updater.read_object(self.root / "catalog/pet-image-index.json")
        subject.current = lambda part: {"pets":{"123":{}}} if part=="pets" else previous
        subject.icon_exceptions = lambda versions,current: ([],{})
        with patch.object(icons,"loader_default",return_value={"defaultVersion":"3001"}), \
                patch.object(updater,"refresh_cached_images",return_value=1):
            self.assertFalse(subject.images({}))
        self.assertEqual(updater.read_object(self.root / "catalog/pet-image-index.json"),previous)
        self.assertEqual(subject.image_failures,1)

    def test_replace_does_not_redownload_an_intact_same_source(self):
        path=self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        with patch.object(updater,"fetch",side_effect=AssertionError("same source must stay cached")):
            result=updater.extract_image(self.root,"123_0",replace=True)
        self.assertEqual(result["contentSha256"],updater.digest(path))

    def candidate(self, contents):
        path=self.root / "candidate.png"; path.write_bytes(contents)
        metadata={"url":self.entry["swfUrl"],"symbol":self.entry["symbol"],"revision":self.entry["revision"]}
        return path,metadata

    def test_same_source_cannot_commit_a_different_png(self):
        output=self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        old=output.read_bytes(); proof=output.with_suffix(".json").read_bytes()
        candidate,metadata=self.candidate(b"different official bytes at identical URL/version")
        with self.assertRaisesRegex(ValueError,"内容已变化"):
            updater.commit_picture(candidate,output,self.entry,metadata)
        self.assertEqual(output.read_bytes(),old)
        self.assertEqual(output.with_suffix(".json").read_bytes(),proof)

    def test_missing_metadata_does_not_allow_replacing_existing_png(self):
        output=self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        old=output.read_bytes(); output.with_suffix(".json").unlink()
        candidate,metadata=self.candidate(b"different new pixels")
        with self.assertRaisesRegex(ValueError,"已有不同内容"):
            updater.commit_picture(candidate,output,self.entry,metadata)
        self.assertEqual(output.read_bytes(),old)
        self.assertFalse(output.with_suffix(".json").exists())

    def test_corruption_can_only_restore_the_recorded_fingerprint(self):
        output=self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        original=output.read_bytes(); output.write_bytes(b"corruption")
        candidate,metadata=self.candidate(b"different new official pixels")
        with self.assertRaisesRegex(ValueError,"内容已变化"):
            updater.commit_picture(candidate,output,self.entry,metadata)
        self.assertEqual(output.read_bytes(),b"corruption")
        candidate.write_bytes(original)
        updater.commit_picture(candidate,output,self.entry,metadata)
        self.assertEqual(output.read_bytes(),original)

    def test_concurrent_first_publication_never_overwrites_the_winner(self):
        output=updater.picture_cache_path(self.root,"123_0",self.entry)
        candidate,metadata=self.candidate(b"new pixels")
        def winner(_temporary,target):
            target.write_bytes(b"other publisher won")
            raise FileExistsError("synthetic race")
        with patch.object(updater.os,"link",side_effect=winner), self.assertRaisesRegex(ValueError,"并发写入"):
            updater.commit_picture(candidate,output,self.entry,metadata)
        self.assertEqual(output.read_bytes(),b"other publisher won")
        self.assertFalse(output.with_suffix(".json").exists())

    def test_pinned_content_blocks_changed_png_even_after_cache_deletion(self):
        old=self.cached(updater.picture_cache_path(self.root,"123_0",self.entry))
        fingerprint=updater.digest(old); old.unlink(); old.with_suffix(".json").unlink()
        (self.root / "data-tools").mkdir()
        def fetch(url,path):
            path.write_bytes(b"synthetic changed SWF at same URL")
            return {"url":url,"sha256":updater.digest(path)}
        def export(command,**_kwargs):
            folder=Path(command[-2]); folder.mkdir(); (folder / "1.png").write_bytes(b"changed PNG")
            return Mock(returncode=0)
        with patch.object(updater,"installed_runtime",return_value=Path("synthetic runtime")), \
                patch.object(updater,"fetch",side_effect=fetch),patch.object(updater,"symbol_character_id",return_value=1), \
                patch.object(updater.subprocess,"run",side_effect=export), \
                patch.object(updater,"png_details",side_effect=lambda path: {"width":1,"height":1,"bytes":path.stat().st_size,"sha256":updater.digest(path)}):
            with self.assertRaisesRegex(ValueError,"同版本图片内容"):
                updater.extract_image(self.root,"123_0",replace=True,expected_source_identity=self.entry["sourceIdentity"],
                                      expected_content_sha256=fingerprint)
        self.assertFalse(old.exists())

    def test_migrated_object_is_not_a_hard_link_to_the_legacy_file(self):
        legacy=self.cached(self.root / "images/pets/123_0.png")
        result=updater.extract_image(self.root,"123_0")
        committed=Path(result["path"]); original=committed.read_bytes()
        legacy.write_bytes(b"legacy writer changes its file")
        self.assertEqual(committed.read_bytes(),original)


if __name__ == "__main__":
    unittest.main()
