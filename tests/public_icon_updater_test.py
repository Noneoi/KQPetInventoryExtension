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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
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
            result.writestr(name, value)
    return buffer.getvalue()


class FakeUpdater:
    def __init__(self, root):
        self.root, self.scratch, self.baseline = root, root / "scratch", root / "baseline"
        self.tools, self.runtimes = root / "data-tools", ("test-java", "test-ffdec")
        self.start_version = "20260910814679630"
        self.scratch.mkdir()
        self.calls = []
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

    def fetch(self, url, path):
        self.calls.append(url)
        if self.fail and "attributeicon" in url:
            raise OSError("offline")
        if "@res" in url:
            body = archive(self.resources)
        elif "attributeicon" in url:
            xml = '<packageDescription><resources>' + ''.join(
                f'<image name="attr_{key}" file="{filename}"/>' for key, filename in self.mapping.items()) + '</resources></packageDescription>'
            body = archive({"package.xml": xml})
        elif "/loader~" in url:
            body = b"loader-fixture"
        else:
            raise AssertionError("unexpected network request: " + url)
        path.write_bytes(body)
        return {"url": url, "sha256": hashlib.sha256(body).hexdigest()}

    def export(self, swf, target, selected):
        target.mkdir(parents=True, exist_ok=True)
        path = target / "VersManager.as"
        path.write_text('const DEFAULT_VERSION:String = "2000"; return _loc3_ + DEFAULT_VERSION;')
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

    def test_dynamic_ids_and_unchanged_update_does_not_download(self):
        self.assertTrue(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.icon_failures, [])
        self.assertEqual((self.root / "images/attributes/29.png").read_bytes(), png(29))
        self.assertEqual((self.root / "images/stargods/92.png").read_bytes(), png(92))
        self.assertFalse(any("stargodres" in call for call in self.updater.calls))
        self.updater.calls.clear()
        self.assertFalse(icons.update_icons(self.updater, self.versions))
        self.assertEqual(self.updater.calls, [])

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
        self.assertEqual(self.updater.calls, [])
        self.assertEqual((self.root / "images/stargods/92.png").read_bytes(), png(92))

    def test_bad_archive_mapping_is_rejected(self):
        with self.assertRaises(ValueError):
            icons.attribute_mapping({"package.xml": b'<packageDescription><resources><image name="attr_29" file="../oops.png"/></resources></packageDescription>'})


if __name__ == "__main__":
    unittest.main()
