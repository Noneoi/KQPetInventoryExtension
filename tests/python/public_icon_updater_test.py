"""Isolated manual-update contracts; no account/game requests or live data roots."""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_icon_updater as icons


def png(color=0):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes([0, color, 20, 30, 255]))) + chunk(b"IEND", b""))


def archive(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as result:
        for name, value in files.items():
            # Real bytes, but a fixed ZIP timestamp so identical synthetic
            # input has a reproducible SHA across the test's separate checks.
            result.writestr(zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0)), value)
    return buffer.getvalue()


class ObservedVersions(dict):
    def __init__(self, values):
        super().__init__(values)
        self.source_hashes = {}


class FakeUpdater:
    def __init__(self, root):
        self.root, self.scratch, self.baseline = root, root / "scratch", root / "baseline"
        self.tools, self.runtimes = root / "data-tools", ("test-java", "test-ffdec")
        self.start_version = "20260910814679630"
        self.scratch.mkdir()
        self.calls = []
        self.exports = []
        self.loader_script = 'const DEFAULT_VERSION:String = "2000"; return _loc3_ + DEFAULT_VERSION;'
        self.definitions = {"1": {}, "92": {}}
        self.mapping = {1: "basic.png", 29: "newattribute.png"}
        self.resources = {name: png(key) for key, name in self.mapping.items()}
        self.fail = False
        bundled = self.baseline / "stargod-icons"
        bundled.mkdir(parents=True)
        records = []
        for key, version in ((1, "2026091000000000"), (92, "2000")):
            path = bundled / f"{key}.png"
            path.write_bytes(png(key))
            records.append({"id": key, "version": version, "png": icons.valid_png(path)})
        (bundled / "sources.json").write_text(json.dumps({"icons": records}))

    def current(self, name):
        return {"stargods": self.definitions}

    def resource_bytes(self, url):
        if "@res" in url:
            body = archive(self.resources)
        elif "attributeicon" in url:
            xml = '<packageDescription><resources>' + ''.join(
                f'<image name="attr_{key}" file="{filename}"/>' for key, filename in self.mapping.items()) + '</resources></packageDescription>'
            body = archive({"package.xml": xml})
        elif "/loader~" in url:
            body = b"loader-fixture\0" + self.loader_script.encode('utf-8')
        else:
            raise AssertionError("unexpected network request: " + url)
        return body

    def fetch(self, url, path):
        self.calls.append(url)
        if self.fail and "attributeicon" in url:
            raise OSError("offline")
        body = self.resource_bytes(url)
        path.write_bytes(body)
        return {"url": url, "sha256": hashlib.sha256(body).hexdigest()}

    def export(self, swf, target, selected):
        self.exports.append((swf, selected))
        target.mkdir(parents=True, exist_ok=True)
        path = target / "VersManager.as"
        path.write_text(swf.read_bytes().split(b'\0', 1)[1].decode('utf-8'), encoding='utf-8')
        return path


class Contracts(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.updater = FakeUpdater(self.root)
        self.versions = {icons.ATTRIBUTE_RESOURCE: "2025060596869460", icons.ATTRIBUTE_RESOURCE + "@res": "2025060596869460",
                         "stargodres/stargod1": "2026091000000000"}

    def tearDown(self):
        self.directory.cleanup()

    def loader_url(self):
        return icons.OFFICIAL + 'loader~' + self.updater.start_version + '.swf'

    def observe_attribute_hashes(self):
        if not isinstance(self.versions, ObservedVersions):
            self.versions = ObservedVersions(self.versions)
        for resource in (icons.ATTRIBUTE_RESOURCE, icons.ATTRIBUTE_RESOURCE + '@res'):
            url = icons.OFFICIAL + resource + '~' + self.versions[resource] + '.fui'
            self.versions.source_hashes[resource] = hashlib.sha256(self.updater.resource_bytes(url)).hexdigest()

    def test_dynamic_ids_and_unchanged_update_only_verifies_loader_without_downloading_icons(self):
        self.assertTrue(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.icon_failures, [])
        self.assertEqual((self.root / "images/attributes/29.png").read_bytes(), png(29))
        self.assertEqual((self.root / "images/stargods/92.png").read_bytes(), png(92))
        self.assertFalse(any("stargodres" in call for call in self.updater.calls))
        self.updater.calls.clear()
        exports = len(self.updater.exports)
        self.assertFalse(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.calls, [self.loader_url()])
        self.assertEqual(len(self.updater.exports), exports, 'Same loader bytes must not be decompiled again')

    def test_failed_update_keeps_previous_valid_icon(self):
        icons.update_icons(self.updater, self.versions)
        before = (self.root / "images/attributes/29.png").read_bytes()
        self.updater.fail = True
        self.versions[icons.ATTRIBUTE_RESOURCE] = "2026091000000001"
        self.assertFalse(icons.update_icons(self.updater, self.versions))
        self.assertTrue(self.updater.icon_failures)
        self.assertEqual(before, (self.root / "images/attributes/29.png").read_bytes())

    def test_attribute_package_is_validated_before_replacement(self):
        icons.update_icons(self.updater, self.versions)
        self.versions[icons.ATTRIBUTE_RESOURCE] = "2026091000000002"
        self.updater.resources["basic.png"] = png(100)
        self.updater.resources["newattribute.png"] = b"corrupt"
        icons.update_icons(self.updater, self.versions)
        self.assertTrue(self.updater.icon_failures)
        self.assertEqual((self.root / "images/attributes/1.png").read_bytes(), png(1))

    def test_new_stargod_is_downloaded_and_fixed_path_is_overwritten(self):
        icons.update_icons(self.updater, self.versions)
        self.versions["stargodres/stargod1"] = "2026091000000003"
        self.updater.definitions["93"] = {}
        def exported(updater, key, version, source):
            path = self.updater.scratch / f"result-{key}.png"
            path.write_bytes(png(200))
            return {"id": key, "version": version, "png": icons.valid_png(path)}, path
        with patch.object(icons, "export_star", side_effect=exported) as operation:
            self.assertTrue(icons.update_icons(self.updater, self.versions))
            self.assertEqual(operation.call_count, 2)
        self.assertEqual(self.updater.icon_failures, [])
        self.assertEqual((self.root / "images/stargods/1.png").read_bytes(), png(200))
        self.assertTrue((self.root / "images/stargods/93.png").exists())

    def test_corrupt_disk_icon_recovers_from_valid_bundled_copy(self):
        icons.update_icons(self.updater, self.versions)
        (self.root / "images/stargods/92.png").write_bytes(b"corrupt")
        self.updater.calls.clear()
        self.assertTrue(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.calls, [self.loader_url()])
        self.assertEqual((self.root / "images/stargods/92.png").read_bytes(), png(92))

    def test_same_version_loader_hash_change_reparses_actual_default_rule(self):
        first = icons.loader_default(self.updater, {})
        self.assertEqual(first['defaultVersion'], '2000')
        self.assertEqual(len(self.updater.exports), 1)
        current = {'source': {'loaderDefault': first}}
        self.assertEqual(icons.loader_default(self.updater, current), first)
        self.assertEqual(len(self.updater.exports), 1)
        self.updater.loader_script = self.updater.loader_script.replace('"2000"', '"2001"')
        changed = icons.loader_default(self.updater, current)
        self.assertEqual(changed['defaultVersion'], '2001')
        self.assertEqual(changed['version'], first['version'])
        self.assertNotEqual(changed['sha256'], first['sha256'])
        self.assertEqual(len(self.updater.exports), 2)
        self.assertEqual(self.updater.calls, [self.loader_url()] * 3)
        self.assertEqual(first['defaultVersion'], '2000', 'Reading a new rule must not mutate the old snapshot')

    def test_same_version_unknown_loader_rule_keeps_previous_index_and_icons(self):
        icons.update_icons(self.updater, self.versions)
        index = self.root / 'catalog' / icons.INDEX
        before_index = index.read_bytes()
        before_image = (self.root / 'images/stargods/92.png').read_bytes()
        self.updater.loader_script = 'const DEFAULT_VERSION:String = "2001"; return arbitraryFutureRule();'
        self.assertFalse(icons.update_icons(self.updater, self.versions))
        self.assertTrue(any('版本规则已变化' in failure for failure in self.updater.icon_failures))
        self.assertEqual(index.read_bytes(), before_index)
        self.assertEqual((self.root / 'images/stargods/92.png').read_bytes(), before_image)

    def test_same_version_attribute_image_hash_refreshes_png_without_fake_version(self):
        self.observe_attribute_hashes()
        versions = dict(self.versions)
        icons.update_icons(self.updater, self.versions)
        self.updater.calls.clear()
        old_hash = self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE + '@res']
        self.updater.resources['newattribute.png'] = png(77)
        self.observe_attribute_hashes()
        self.assertNotEqual(self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE + '@res'], old_hash)
        self.assertTrue(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.icon_failures, [])
        self.assertEqual((self.root / 'images/attributes/29.png').read_bytes(), png(77))
        current = icons.read_object(self.root / 'catalog' / icons.INDEX)
        self.assertEqual(current['source']['attributeImages']['sha256'],
                         self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE + '@res'])
        self.assertEqual(dict(self.versions), versions)
        self.assertEqual(sum('attributeicon' in url for url in self.updater.calls), 2)
        self.assertFalse(any('stargodres' in url for url in self.updater.calls))
        self.updater.calls.clear()
        self.assertFalse(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.calls, [self.loader_url()])

    def test_same_version_attribute_mapping_hash_refreshes_real_mapping(self):
        self.observe_attribute_hashes()
        icons.update_icons(self.updater, self.versions)
        image_hash = self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE + '@res']
        mapping_hash = self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE]
        self.updater.mapping[29] = 'basic.png'
        self.observe_attribute_hashes()
        self.assertEqual(self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE + '@res'], image_hash)
        self.assertNotEqual(self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE], mapping_hash)
        self.assertTrue(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.icon_failures, [])
        self.assertEqual((self.root / 'images/attributes/29.png').read_bytes(), png(1))
        current = icons.read_object(self.root / 'catalog' / icons.INDEX)
        self.assertEqual(current['source']['attributePackage']['sha256'],
                         self.versions.source_hashes[icons.ATTRIBUTE_RESOURCE])

    def test_same_version_stargod_source_hash_does_not_reuse_old_pixels(self):
        icons.update_icons(self.updater, self.versions)
        self.versions = ObservedVersions(self.versions)
        observed = 'a' * 64
        self.versions.source_hashes['stargodres/stargod1'] = observed
        def exported(updater, key, version, source):
            path = self.updater.scratch / f'fresh-{key}.png'
            path.write_bytes(png(88))
            return {'id': key, 'version': version, 'source': {'sha256': observed}, 'png': icons.valid_png(path)}, path
        with patch.object(icons, 'export_star', side_effect=exported) as operation:
            self.assertTrue(icons.update_icons(self.updater, self.versions))
            self.assertEqual(operation.call_count, 1)
        self.assertEqual((self.root / 'images/stargods/1.png').read_bytes(), png(88))
        with patch.object(icons, 'export_star', side_effect=AssertionError('exact verified bytes are reusable')):
            self.assertFalse(icons.update_icons(self.updater, self.versions))

    def test_same_version_corrupt_attribute_hotfix_keeps_all_old_files_and_retries(self):
        self.observe_attribute_hashes()
        icons.update_icons(self.updater, self.versions)
        index = self.root / 'catalog' / icons.INDEX
        before_index = index.read_bytes()
        before_images = {key: (self.root / f'images/attributes/{key}.png').read_bytes() for key in (1, 29)}
        self.updater.resources['basic.png'] = png(100)
        self.updater.resources['newattribute.png'] = b'corrupt same-version hotfix'
        self.observe_attribute_hashes()
        self.assertFalse(icons.update_icons(self.updater, self.versions))
        self.assertTrue(self.updater.icon_failures)
        self.assertEqual(index.read_bytes(), before_index)
        for key, before in before_images.items():
            self.assertEqual((self.root / f'images/attributes/{key}.png').read_bytes(), before)
        self.updater.resources['newattribute.png'] = png(101)
        self.observe_attribute_hashes()
        self.assertTrue(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.icon_failures, [])
        self.assertEqual((self.root / 'images/attributes/1.png').read_bytes(), png(100))
        self.assertEqual((self.root / 'images/attributes/29.png').read_bytes(), png(101))

    def test_bad_archive_mapping_is_rejected(self):
        with self.assertRaises(ValueError):
            icons.attribute_mapping({"package.xml": b'<packageDescription><resources><image name="attr_29" file="../oops.png"/></resources></packageDescription>'})


if __name__ == "__main__":
    unittest.main()
