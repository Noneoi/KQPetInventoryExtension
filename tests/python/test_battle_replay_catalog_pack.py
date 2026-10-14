"""Local package tests. Every fixture is synthetic and is NOT official game data."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "battle_replay"))
import catalog_pack as pack


def synthetic_catalog() -> dict:
    # Deliberately independent fixture shape. Source hashes/URLs below are
    # synthetic schema inputs, never fetched or presented as verified evidence.
    root = {key: {} for key in (
        "provenance", "decompiledSourceSha256", "skills", "buffs", "skillLastEffects",
        "buffPowerFullMap", "relationsBySkill", "relationsByBuff", "officialEntries",
        "officialEntryMentions", "entryMentionsByBuff", "entryMentionsBySkill", "petMechanics",
        "mechanismRules", "legendMechanics", "huanMechanics", "petMechanicsSummary",
        "referenceConsumers", "petIdentitySummary", "clientPresentation", "petDragons",
        "replaySupplements", "replayCultivation", "tableJoinSummary", "pets", "transformSkills",
        "attributes", "execution", "bufferExecution", "referenceCoverage",
    )}
    root.update(format=3, tableRelations=[], combos=[], testOnly="SYNTHETIC TEST CATALOG")
    root["clientPresentation"] = {"schemaVersion": 1}
    root["pets"] = {"1": {"name": "test pet", "identity": {"status": "native", "canonicalName": "test pet"},
                          "nativeAttributes": ["1"], "maxPower": 100}}
    root["skills"] = {"2": {"id": "2", "name": "", "description": "", "configResolution": "direct",
                            "textSource": {}, "playbackAvailable": False}}
    root["buffs"] = {"3": dict(id="3", did="0", nm="", df="0", once="0", pos="0", beffect="", bhs="",
                              mess="", nameStatus="unnamed", configSource="synthetic-only")}
    root["attributes"] = {"1": dict(id="1", nm="test attribute", pri="1", rcs="1")}
    root["execution"] = {"-1": dict(code="-1", nm="test execution", eid="0", type="1")}
    root["bufferExecution"] = {"4": dict(bufferId="4", actionResId="0", skillId="0", lightedSkillId="0", desc="")}
    root["tableJoinSummary"] = {"skills": {"skills": 1}, "buffs": {"canonicalBuffIds": 1,
                                   "flashBufferExecuteRows": 1, "flashComboRows": 0}}
    root["petMechanicsSummary"] = {"pets": 1}
    root["provenance"] = {
        "manifestSha256": "1" * 64,
        "versiondata": {"version": "synthetic-test-only"},
        "packages": [{"resource": key, "version": "synthetic-test-only", "sha256": "2" * 64,
                      "url": "https://aoqi.100bt.com/play/TEST-ONLY/" + key + ".swf"}
                     for key in ("library/interfaces", "battle/battle", "battleconfig/battleconfig")],
    }
    return root


def encoded(root: dict | None = None) -> bytes:
    return json.dumps(root if root is not None else synthetic_catalog(), ensure_ascii=False,
                      separators=(",", ":")).encode("utf-8")


def envelope(raw: bytes, body: bytes | None = None, *, json_size: int | None = None,
             compressed_size: int | None = None, checksum: bytes | None = None, magic: bytes = pack.MAGIC) -> bytes:
    body = zlib.compress(raw, 9) if body is None else body
    return struct.pack(">8sQQ32s", magic, len(raw) if json_size is None else json_size,
                       len(body) if compressed_size is None else compressed_size,
                       hashlib.sha256(raw).digest() if checksum is None else checksum) + body


class CatalogPackBytesTest(unittest.TestCase):
    def setUp(self):
        self.raw = encoded()
        self.packed = pack.pack_bytes(self.raw)

    def test_fixed_header_and_byte_preserving_deterministic_level9(self):
        root = synthetic_catalog()
        root["testOnly"] = "TEST \u751f\u547d\u62a4\u76fe\u6c14\u52bf \U0001f432"
        # BOM, CRLF, whitespace and non-ASCII text must survive unchanged.
        raw = b"\xef\xbb\xbf \r\n" + json.dumps(root, indent=2, ensure_ascii=False).replace("\n", "\r\n").encode("utf-8") + b"\r\n\t"
        first = pack.pack_bytes(raw)
        self.assertEqual(first, pack.pack_bytes(raw))
        self.assertEqual(pack.unpack_bytes(first), raw)
        self.assertEqual(pack.HEADER.size, 56)
        self.assertEqual(pack.MAX_JSON_BYTES, 128 * 1024 * 1024)
        self.assertEqual(pack.MAX_PACK_BYTES, 32 * 1024 * 1024)
        magic, size, compressed, sha = struct.unpack(">8sQQ32s", first[:56])
        self.assertEqual((magic, size, compressed, sha),
                         (b"KQBRPK1\n", len(raw), len(first) - 56, hashlib.sha256(raw).digest()))
        self.assertEqual(first[56:], zlib.compress(raw, 9))

    def test_corruption_and_truncated_header_or_body(self):
        bad_checksum = bytearray(self.packed)
        bad_checksum[-1] ^= 1
        candidates = [b"", self.packed[:7], self.packed[:55], self.packed[:56],
                      self.packed[:-1], self.packed + b"extra", bytes(bad_checksum),
                      envelope(self.raw, magic=b"KQBRPK2\n"), envelope(self.raw, checksum=b"\0" * 32),
                      envelope(self.raw, body=self.packed[56:-1])]
        for candidate in candidates:
            with self.subTest(size=len(candidate), head=candidate[:8]):
                with self.assertRaises(pack.CatalogPackError):
                    pack.unpack_bytes(candidate)

    def test_false_lengths_zero_lengths_and_global_upper_bound(self):
        for size in (0, 1, len(self.raw) - 1, len(self.raw) + 1, pack.MAX_JSON_BYTES + 1, 2**64 - 1):
            with self.subTest(json_size=size), self.assertRaises(pack.CatalogPackError):
                pack.unpack_bytes(envelope(self.raw, json_size=size))
        for size in (0, len(self.packed) - 57, len(self.packed) - 55, 2**64 - 1):
            with self.subTest(compressed_size=size), self.assertRaises(pack.CatalogPackError):
                pack.unpack_bytes(envelope(self.raw, compressed_size=size))
        with self.assertRaises(pack.CatalogPackError):
            pack.pack_bytes(b"")

    def test_rejects_trailing_content_multiple_streams_and_other_wrappers(self):
        body = self.packed[56:]
        bodies = [body + b"trailing", body + zlib.compress(b"second stream", 9)]
        for wbits in (-zlib.MAX_WBITS, zlib.MAX_WBITS + 16):
            compressor = zlib.compressobj(level=9, wbits=wbits)
            bodies.append(compressor.compress(self.raw) + compressor.flush())
        for candidate in bodies:
            with self.subTest(prefix=candidate[:4]), self.assertRaises(pack.CatalogPackError):
                pack.unpack_bytes(envelope(self.raw, body=candidate))

    def test_decompression_is_bounded_by_declared_size_plus_one(self):
        raw = b"x" * 1_000_000
        calls = []
        original = zlib.decompressobj

        class BoundedProbe:
            def __init__(self, *args, **kwargs):
                self.inner = original(*args, **kwargs)

            def decompress(self, data, max_length=0):
                calls.append(max_length)
                return self.inner.decompress(data, max_length)

            def __getattr__(self, key):
                return getattr(self.inner, key)

        with mock.patch.object(pack.zlib, "decompressobj", BoundedProbe):
            with self.assertRaises(pack.CatalogPackError):
                pack.unpack_bytes(envelope(raw, json_size=128))
        self.assertEqual(calls, [129])

    def test_exact_size_boundaries_without_large_test_allocations(self):
        with mock.patch.object(pack, "MAX_JSON_BYTES", len(self.raw)), \
             mock.patch.object(pack, "MAX_PACK_BYTES", len(self.packed)):
            self.assertEqual(pack.pack_bytes(self.raw), self.packed)
            self.assertEqual(pack.unpack_bytes(self.packed), self.raw)
        with mock.patch.object(pack, "MAX_JSON_BYTES", len(self.raw) - 1):
            with self.assertRaises(pack.CatalogPackError):
                pack.pack_bytes(self.raw)
            with self.assertRaises(pack.CatalogPackError):
                pack.unpack_bytes(self.packed)
        with mock.patch.object(pack, "MAX_PACK_BYTES", len(self.packed) - 1):
            with self.assertRaises(pack.CatalogPackError):
                pack.pack_bytes(self.raw)
            with self.assertRaises(pack.CatalogPackError):
                pack.unpack_bytes(self.packed)

    def test_invalid_json_and_every_required_container_are_rejected(self):
        invalid = [b"[]", b"{", b"\xff", b'{"format":NaN}', b'{"format":1e999}',
                   self.raw[:-1] + b',"format":3}']
        for raw in invalid:
            with self.subTest(raw=raw[:30]), self.assertRaises(pack.CatalogPackError):
                pack.pack_bytes(raw)
        original = synthetic_catalog()
        for key in original:
            if key == "testOnly":
                continue
            root = copy.deepcopy(original)
            del root[key]
            with self.subTest(missing=key), self.assertRaises(pack.CatalogPackError):
                pack.pack_bytes(encoded(root))

    def test_invalid_rows_summaries_and_provenance_are_rejected(self):
        cases = []
        root = synthetic_catalog(); root["format"] = "3"; cases.append(root)
        root = synthetic_catalog(); root["skills"]["2"] = {}; cases.append(root)
        root = synthetic_catalog(); root["skills"]["2"]["id"] = "9"; cases.append(root)
        root = synthetic_catalog(); root["buffs"] = {}; cases.append(root)
        root = synthetic_catalog(); root["petMechanicsSummary"]["pets"] = True; cases.append(root)
        root = synthetic_catalog(); root["tableJoinSummary"]["skills"]["skills"] = 2; cases.append(root)
        root = synthetic_catalog(); root["clientPresentation"]["schemaVersion"] = True; cases.append(root)
        root = synthetic_catalog(); root["provenance"]["manifestSha256"] = "wrong"; cases.append(root)
        root = synthetic_catalog(); root["provenance"]["packages"].pop(); cases.append(root)
        root = synthetic_catalog(); root["provenance"]["packages"][0]["url"] = "https://example.com/play/"; cases.append(root)
        root = synthetic_catalog(); root["provenance"]["packages"].append(root["provenance"]["packages"][0]); cases.append(root)
        for root in cases:
            with self.subTest(root=root), self.assertRaises(pack.CatalogPackError):
                pack.pack_bytes(encoded(root))

    def test_json_tree_budgets_include_non_bmp_utf16_units(self):
        root = synthetic_catalog()
        node = root
        for _ in range(70):
            node["nested"] = {}
            node = node["nested"]
        with self.assertRaises(pack.CatalogPackError):
            pack.pack_bytes(encoded(root))
        with mock.patch.object(pack, "MAX_JSON_NODES", 10):
            with self.assertRaises(pack.CatalogPackError):
                pack.pack_bytes(self.raw)
        with mock.patch.object(pack, "MAX_STRING_UNITS", 2):
            with self.assertRaises(pack.CatalogPackError):
                pack._check_tree({"": "\U0001f432x"})
            pack._check_tree({"": "\U0001f432"})


class CatalogPackFileTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kq-replay-pack-test-")
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "original.json"
        self.output = self.root / "battle-replay-catalog.brpack"
        self.raw = encoded()
        self.source.write_bytes(self.raw)
        self.old = b"TEST ONLY: previous output must remain byte-for-byte"
        self.output.write_bytes(self.old)

    def tearDown(self):
        self.temporary.cleanup()

    def assertPreserved(self):
        self.assertEqual(self.output.read_bytes(), self.old)
        self.assertEqual(self.source.read_bytes(), self.raw)
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_atomic_file_output_and_stats(self):
        real_replace = os.replace
        replacements = []

        def inspect_replace(source, target):
            self.assertEqual(Path(source).parent, self.output.parent)
            self.assertEqual(pack.unpack_bytes(Path(source).read_bytes()), self.raw)
            replacements.append((Path(source), Path(target)))
            real_replace(source, target)

        with mock.patch.object(pack.os, "replace", side_effect=inspect_replace):
            result = pack.pack_file(self.source, self.output)
        self.assertEqual(len(replacements), 1)
        self.assertEqual(pack.unpack_bytes(self.output.read_bytes()), self.raw)
        self.assertEqual(result, {"format": "KQBRPK1", "json_bytes": len(self.raw),
                                 "pack_bytes": self.output.stat().st_size, "sha256": hashlib.sha256(self.raw).hexdigest()})
        self.assertEqual(self.source.read_bytes(), self.raw)
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_fsync_replace_and_readback_failures_preserve_previous_output(self):
        for operation in ("fsync", "replace"):
            with self.subTest(operation=operation), mock.patch.object(pack.os, operation, side_effect=OSError("injected failure")):
                with self.assertRaises(OSError):
                    pack.pack_file(self.source, self.output)
            self.assertPreserved()
        with mock.patch.object(pack, "unpack_bytes", side_effect=[self.raw, pack.CatalogPackError("injected readback failure")]):
            with self.assertRaises(pack.CatalogPackError):
                pack.pack_file(self.source, self.output)
        self.assertPreserved()

    def test_invalid_and_oversized_input_preserve_previous_output(self):
        self.source.write_bytes(b'{"format":2}')
        with self.assertRaises(pack.CatalogPackError):
            pack.pack_file(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), self.old)
        self.assertEqual(self.source.read_bytes(), b'{"format":2}')
        self.source.write_bytes(self.raw)
        with mock.patch.object(pack, "MAX_JSON_BYTES", len(self.raw) - 1):
            with self.assertRaises(pack.CatalogPackError):
                pack.pack_file(self.source, self.output)
        self.assertPreserved()

    def test_same_input_path_or_hardlink_output_is_rejected(self):
        for target in (self.source, self.source.parent / "." / self.source.name):
            with self.assertRaises(pack.CatalogPackError):
                pack.pack_file(self.source, target)
        alias = self.root / "input-alias.brpack"
        os.link(self.source, alias)
        with self.assertRaises(pack.CatalogPackError):
            pack.pack_file(self.source, alias)
        self.assertEqual(alias.read_bytes(), self.raw)
        self.assertPreserved()

    def test_cli_requires_explicit_output_and_reports_success_as_json(self):
        command = [sys.executable, "-B", str(ROOT / "tools/battle_replay/catalog_pack.py"), "--input", str(self.source)]
        missing = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(missing.returncode, 2)
        self.assertPreserved()
        result = subprocess.run(command + ["--output", str(self.output)], capture_output=True,
                                text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        stats = json.loads(result.stdout)
        self.assertEqual(stats["json_bytes"], len(self.raw))
        self.assertEqual(stats["pack_bytes"], self.output.stat().st_size)
        self.assertEqual(stats["sha256"], hashlib.sha256(self.raw).hexdigest())
        self.assertEqual(pack.unpack_bytes(self.output.read_bytes()), self.raw)


if __name__ == "__main__":
    unittest.main()
