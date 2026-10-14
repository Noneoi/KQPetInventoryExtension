"""Offline updater checks; synthetic fixtures are never official game evidence.

The transaction tests use the real source-bundle relocation, official URL-chain
selection, SWF/XML extraction, package validation and atomic pack writer. Only
the expensive evidence rebuild/decompiler and network are replaced with small
deterministic fixtures. No test writes to the user's installed data directory.
"""
from __future__ import annotations

from contextlib import ExitStack
import copy
import hashlib
import io
import json
import lzma
from pathlib import Path
import re
import stat
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock
from urllib.parse import urlsplit
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/battle_replay"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import public_battle_replay_updater as subject
import public_data_updater as public
import public_resource_session as public_session
import public_catalog_quality as quality
import catalog_pack as pack
import fetch_reference as flash
import update_source_bundle as bundle
from test_battle_replay_catalog_pack import synthetic_catalog


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def tag(code, payload=b""):
    if len(payload) < 63:
        return struct.pack("<H", (code << 6) | len(payload)) + payload
    return struct.pack("<HI", (code << 6) | 63, len(payload)) + payload


def swf_from_body(body, signature=b"CWS", *, declared=None):
    header = signature + b"\x0d" + struct.pack("<I", len(body) + 8 if declared is None else declared)
    if signature == b"CWS":
        return header + zlib.compress(body)
    if signature == b"ZWS":
        compressed = lzma.compress(body, format=lzma.FORMAT_ALONE)
        payload = compressed[:5] + compressed[13:]
        return header + struct.pack("<I", len(payload)) + payload
    return header + body


def swf_payloads(named, signature=b"CWS"):
    # RECT with Nbits=0, frame rate=0, frame count=1; the fixture has no code.
    body = b"\0\0\0\1\0"
    symbols = struct.pack("<H", len(named))
    for index, (name, payload) in enumerate(named.items(), 1):
        body += tag(87, struct.pack("<HI", index, 0) + payload)
        symbols += struct.pack("<H", index) + name.encode("utf-8") + b"\0"
    body += tag(76, symbols) + tag(0)
    return swf_from_body(body, signature)


def aqz_bytes(members):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in members:
            archive.writestr(name, value)
    return stream.getvalue()


class OfflineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="replay-public-update-test-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        blocker = mock.patch("urllib.request.urlopen", side_effect=AssertionError("Real network is forbidden in this test"))
        blocker.start()
        self.addCleanup(blocker.stop)

    def write(self, name, content):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path


class BoundedSwfTests(OfflineTest):
    def test_fws_cws_zws_preserve_all_named_binary_data(self):
        expected = {"mmo.Test.xml": b"<data><x value='3'/></data>", "mmo.Test.opaque": b"\xff\0opaque\x01"}
        for signature in (b"FWS", b"CWS", b"ZWS"):
            with self.subTest(signature=signature):
                self.assertEqual(dict(flash.binary_payloads(swf_payloads(expected, signature))), expected)
        output = self.work / "extracted"
        evidence = {}
        flash.save_binary_payloads(swf_payloads(expected), output, "test/package", evidence)
        self.assertEqual(set(evidence["binary"]), set(expected))
        self.assertEqual(set(evidence["xml"]), {"mmo.Test.xml"})
        for name, raw in expected.items():
            record = evidence["binary"][name]
            self.assertEqual((output / record["file"]).read_bytes(), raw)
            self.assertEqual((record["bytes"], record["sha256"]), (len(raw), sha(raw)))

    def test_declared_size_and_compressed_stream_limits_are_enforced(self):
        valid = swf_payloads({"mmo.Test.data": b"x" * 4096})
        declared = struct.unpack_from("<I", valid, 4)[0]
        candidates = [b"", valid[:8], valid[:-1], valid + b"trailing",
                      valid + zlib.compress(b"another stream"),
                      valid[:4] + struct.pack("<I", 8) + valid[8:],
                      valid[:4] + struct.pack("<I", declared - 1) + valid[8:],
                      valid[:4] + struct.pack("<I", declared + 1) + valid[8:],
                      valid[:4] + struct.pack("<I", flash.MAX_SWF_BYTES + 1) + valid[8:]]
        for value in candidates:
            with self.subTest(length=len(value)), self.assertRaises(ValueError):
                flash.binary_payloads(value)
        calls = []
        original = zlib.decompressobj

        class Probe:
            def __init__(self, *args, **kwargs):
                self.inner = original(*args, **kwargs)

            def decompress(self, data, max_length=0):
                calls.append(max_length)
                return self.inner.decompress(data, max_length)

            def __getattr__(self, key):
                return getattr(self.inner, key)

        undersized = valid[:4] + struct.pack("<I", 40) + valid[8:]
        with mock.patch.object(flash.zlib, "decompressobj", Probe), self.assertRaises(ValueError):
            flash.binary_payloads(undersized)
        self.assertEqual(calls, [33], "Expansion must stop at the declared body length plus one")

    def test_truncated_tags_and_unsafe_symbol_cannot_be_silently_extracted(self):
        prefix = b"\0\0\0\1\0"
        bodies = [prefix + struct.pack("<H", (87 << 6) | 63) + b"\x02",
                  prefix + struct.pack("<H", (87 << 6) | 10) + b"short",
                  prefix + tag(87, b"\0" * 5), prefix + tag(76, b"\0"),
                  prefix + tag(76, struct.pack("<H", 1) + b"\x01")]
        for body in bodies:
            with self.subTest(body=body), self.assertRaises(ValueError):
                flash.binary_payloads(swf_from_body(body))
        with self.assertRaisesRegex(ValueError, "unsafe SWF symbol"):
            flash.save_binary_payloads(swf_payloads({"../outside": b"payload"}), self.work / "out", "test", {})
        self.assertFalse((self.work / "outside.bin").exists())

    def test_versiondata_then_start_overrides_not_lv_or_a_global_resource_default(self):
        manifest = b'<start><v>202610100001</v><lv>199901010000</lv><u><f n="battle/battle" v="202610100003"/></u></start>'
        base = b'<vconfig><f n="battle/battle" v="202610100002"/><f n="library/interfaces" v="202610090001"/></vconfig>'
        result = flash.flash_versions(manifest, swf_payloads({"mmo.versiondata.DocClass_cls": base}))
        self.assertEqual(result, {"battle/battle": "202610100003", "library/interfaces": "202610090001"})
        self.assertEqual(result.get("unknown/resource", "2000"), "2000")


class LiteralCompatibilityTests(OfflineTest):
    DICTIONARY = '''package test { public class PetDictionaryDataContents {
        public static const timestamp:Number = 202610100000;
        public static const datas:Object = {
          "1":PetDictionaryDataItem.create(1,"test pet",0,NO_SEND_PLACE,PetEvoableType_NULL)
        };
        public static function lookup(id:int):Object { return datas[id]; }
    }}'''
    MAP = '''package test { public class SkillConfigSimpleAC {
        public static const MAP:Object = {"12":{"name":"test name","desc":"test description"}};
        public static function lookup(id:int):Object { return MAP[id]; }
    }}'''

    def test_dictionary_literal_changes_allowed_but_expression_arity_and_consumer_changes_blocked(self):
        before, arity = subject._dictionary_skeleton(self.DICTIONARY)
        changed = self.DICTIONARY.replace('202610100000', '202610100001').replace('"test pet"', '"a new name"')
        self.assertEqual(subject._dictionary_skeleton(changed, arity)[0], before)
        for invalid in (self.DICTIONARY.replace('"test pet"', 'loadRemote()'),
                        self.DICTIONARY.replace('PetEvoableType_NULL)', 'PetEvoableType_NULL,0)'),
                        self.DICTIONARY.replace('"1":Pet', '"2":Pet')):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                subject._dictionary_skeleton(invalid, arity)
        changed_code = self.DICTIONARY.replace('return datas[id]', 'return datas[id+1]')
        self.assertNotEqual(subject._dictionary_skeleton(changed_code, arity)[0], before)
        duplicate = self.DICTIONARY.replace('PetEvoableType_NULL)\n', 'PetEvoableType_NULL),"1":PetDictionaryDataItem.create(1,"duplicate",0,NO_SEND_PLACE,PetEvoableType_NULL)\n')
        with self.assertRaisesRegex(ValueError, "重复编号"):
            subject._dictionary_skeleton(duplicate, arity)

    def test_map_literal_changes_allowed_but_executable_expression_and_code_changes_blocked(self):
        before = subject._literal_map_skeleton(self.MAP)
        self.assertEqual(subject._literal_map_skeleton(self.MAP.replace('test description', 'new description')), before)
        with self.assertRaises(ValueError):
            subject._literal_map_skeleton(self.MAP.replace('"test name"', 'getName()'))
        self.assertNotEqual(subject._literal_map_skeleton(self.MAP.replace('return MAP[id]', 'return MAP[id+1]')), before)

    def test_existing_official_small_exports_are_compatible_without_reading_full_catalog(self):
        dictionary = ROOT / "build-battle-reference/petdictionarydataupdate/scripts/mmo/pet/petdictionarydata/PetDictionaryDataContentsUpdate.as"
        skill_map = ROOT / "build-battle-reference/battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigSimpleAC.as"
        if not dictionary.is_file() or not skill_map.is_file():
            self.skipTest("Optional local official decompiler exports are not in this checkout")
        text = dictionary.read_text(encoding="utf-8-sig")
        skeleton, arity = subject._dictionary_skeleton(text)
        self.assertEqual(arity, 67)
        changed = re.sub(r'(const timestamp:Number = )\d+', r'\g<1>202610100001', text, count=1)
        self.assertEqual(subject._dictionary_skeleton(changed, arity)[0], skeleton)
        map_text = skill_map.read_text(encoding="utf-8-sig")
        self.assertTrue(subject._literal_map_skeleton(map_text))


class ArchiveAndDependencyTests(OfflineTest):
    def test_archive_keeps_every_table_and_exact_binary_content(self):
        files = [("skill_config.json", b'{"raw": [1, 2]}\r\n'),
                 ("nested/skill_desc_config.json", b'{}'), ("additional/unknown.bin", b"\0\xff\x01")]
        source = self.write("tables.aqz", aqz_bytes(files))
        target = self.work / "out"
        result = subject._safe_aqz(source, target)
        self.assertEqual({row["member"] for row in result}, {name for name, _ in files})
        for row, (name, data) in zip(result, files):
            self.assertEqual((target / name).read_bytes(), data)
            self.assertEqual((row["bytes"], row["sha256"]), (len(data), sha(data)))

    def test_archive_rejects_escaping_windows_aliases_and_symlinks(self):
        symlink = zipfile.ZipInfo("link")
        symlink.create_system = 3
        symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
        # ZipInfo normalizes the platform separator in its constructor. Assign
        # afterwards so this fixture contains the original invalid bytes even
        # on Windows; the reader exposes them as orig_filename.
        backslash = zipfile.ZipInfo("x/escape.json")
        backslash.filename = "x\\escape.json"
        for index, name in enumerate(("../escape.json", "/escape.json", "C:/escape.json", backslash, "dir./x", symlink)):
            with self.subTest(name=name), self.assertRaises(ValueError):
                subject._safe_aqz(self.write(f"bad-{index}.aqz", aqz_bytes([(name, b"x")])), self.work / f"out-{index}")
        self.assertFalse((self.work / "escape.json").exists())

    def test_archive_rejects_two_spellings_of_one_destination(self):
        # PurePosixPath normalizes '.' and empty path components. Checking only
        # raw archive names lets a later member overwrite an earlier table.
        for index, alias in enumerate(("nested//table.json", "nested/./table.json", "NESTED/table.json")):
            source = self.write(f"alias-{index}.aqz", aqz_bytes([("nested/table.json", b"first"), (alias, b"second")]))
            with self.subTest(alias=alias), self.assertRaises(ValueError):
                subject._safe_aqz(source, self.work / f"alias-out-{index}")

    def test_download_and_cache_boundaries_fail_before_using_invalid_bytes(self):
        fetched = []
        def fetch(url, path):
            fetched.append(url)
            path.write_bytes(b"download")
        updater = types.SimpleNamespace(fetch=fetch, resource_cache={})
        for resource in ("../escape", "/absolute", "x/../../y", "https://other.invalid/x", "x\\y"):
            with self.subTest(resource=resource), self.assertRaises(ValueError):
                subject._package(updater, resource, {}, self.work / "packages")
        with self.assertRaises(ValueError):
            subject._download(updater, "https://other.invalid/file", self.work / "outside")
        self.assertEqual(fetched, [])
        cached = self.write("cached.swf", b"tampered")
        updater.resource_cache[("library/interfaces", "2000")] = (cached, {"sha256": sha(b"original")})
        with self.assertRaisesRegex(ValueError, "缓存校验失败"):
            subject._package(updater, "library/interfaces", {}, self.work / "packages")
        self.assertEqual(fetched, [])
        with mock.patch.object(subject, "MAX_DOWNLOAD", 3), self.assertRaises(ValueError):
            subject._download(updater, subject.FLASH + "sample~2000.swf", self.work / "too-big")

    def test_dependency_conflict_cannot_select_an_arbitrary_historical_hash(self):
        records = [{"url": subject.FLASH + name + "~202610100001.swf", "sha256": "a" * 64}
                   for name in sorted(subject.DATA_PACKAGES | {"library/interfaces", "battle/battle", "loader"})]
        path = self.write("proof.json", encoded({"nested": {"packages": records}}))
        approved, h5 = subject.dependency_records([path])
        self.assertEqual(set(approved), subject.DATA_PACKAGES | {"library/interfaces", "battle/battle", "loader"})
        self.assertEqual(h5, {})
        records.append({"url": subject.FLASH + "battle/battle~202610090001.swf", "sha256": "b" * 64})
        path.write_bytes(encoded(records))
        with self.assertRaises(ValueError):
            subject.dependency_records([path])


class LiteralRefreshTests(OfflineTest):
    """Exercise genuine table refresh, rather than the transaction's mock."""

    @staticmethod
    def dictionary(rows, timestamp):
        values = []
        for race, name, attributes, ultimate in rows:
            fields = [0] * 67
            fields[0], fields[1], fields[8], fields[16] = race, name, attributes, ultimate
            values.append('"' + str(race) + '":PetDictionaryDataItem.create(' +
                          ','.join(json.dumps(value) for value in fields) + ')')
        return ('package test { public class PetDictionaryDataContents { '
                'public static const timestamp:Number = ' + str(timestamp) + '; '
                'public static const datas:Object = {' + ','.join(values) + '}; '
                'public static function lookup(id:int):Object { return datas[id]; } }}')

    def setUp(self):
        super().setUp()
        self.source, self.auxiliary = self.work / 'source', self.work / 'auxiliary'
        self.source.mkdir()
        self.auxiliary.mkdir()
        self.scratch = self.work / 'scratch'
        self.scratch.mkdir()
        self.arguments = {'source': self.source, 'auxiliary': self.auxiliary}
        self.base = self.write('auxiliary/base.as', self.dictionary(
            ((race, 'synthetic ' + str(race), '1,2', 0) for race in range(1, 9001)), 100).encode())
        self.delta = self.write('auxiliary/update.as', self.dictionary([(1, 'old delta', '3', 0)], 101).encode())
        self.write('auxiliary/pets.json', encoded({str(race): {} for race in range(1, 9001)}))
        self.approved, self.packages, records = {}, {}, []
        for resource, path in (('battleconfig/battleconfig', None), ('pet/petdictionarydata', self.base),
                               ('pet/petdictionarydataupdate', self.delta)):
            payload = self.write('packages/' + resource.replace('/', '_') + '.swf', b'SYNTHETIC ' + resource.encode())
            row = {'resource': resource, 'version': '202610100001', 'url': subject.FLASH + resource + '~202610100001.swf',
                   'sha256': sha(payload.read_bytes())}
            self.approved[resource] = dict(row)
            self.packages[resource] = (payload, row)
            if path:
                records.append(dict(row, decompiledFile=path.name, decompiledSha256=sha(path.read_bytes())))
        self.write('auxiliary/pet-provenance.json', encoded({'packages': records}))

    def changed_delta(self, text):
        fresh = self.write('decompiler/current-update.as', text.encode())
        resource = 'pet/petdictionarydataupdate'
        package, row = self.packages[resource]
        package.write_bytes(package.read_bytes() + b' changed')
        row['sha256'] = sha(package.read_bytes())
        return types.SimpleNamespace(scratch=self.scratch, export=mock.Mock(return_value=fresh))

    def refresh(self, updater):
        self.validated_changes = subject.refresh_data_packages(updater, self.arguments, [], self.packages, self.approved)
        return json.loads((self.auxiliary / 'pets.json').read_bytes())

    def test_new_dictionary_rows_use_fresh_values_and_refresh_the_source_hash(self):
        before_hash = sha(self.delta.read_bytes())
        updater = self.changed_delta(self.dictionary([(1, 'new delta', '7,8', 901), (9001, 'added pet', '9', 0)], 102))
        pets = self.refresh(updater)
        self.assertEqual(len(pets), 9001)
        self.assertEqual((pets['1']['name'], pets['1']['nativeAttributes'], pets['1']['maxPower']),
                         ('new delta', ['7', '8'], 150))
        self.assertEqual(pets['9001']['name'], 'added pet')
        self.assertEqual(pets['2']['name'], 'synthetic 2')
        proof = json.loads((self.auxiliary / 'pet-provenance.json').read_bytes())['packages'][1]
        self.assertEqual(proof['decompiledSha256'], sha(self.delta.read_bytes()))
        self.assertEqual(proof['sha256'], self.packages['pet/petdictionarydataupdate'][1]['sha256'])
        self.assertEqual(self.validated_changes, [{'sourcePath': str(self.delta.resolve()), 'beforeSha256': before_hash,
            'afterSha256': sha(self.delta.read_bytes()), 'resource': 'pet/petdictionarydataupdate',
            'producerSha256': self.packages['pet/petdictionarydataupdate'][1]['sha256'],
            'validation': 'dictionary-literal-skeleton-identical', 'constructorFieldCount': 67}])
        updater.export.assert_called_once()
        self.assertEqual(updater.export.call_args.args[1], self.scratch / 'fresh-petdictionarydataupdate')

    def test_equal_or_older_delta_timestamp_does_not_override_base_dictionary(self):
        updater = self.changed_delta(self.dictionary([(1, 'ignored delta', '9', 901)], 100))
        pets = self.refresh(updater)
        self.assertEqual((pets['1']['name'], pets['1']['maxPower']), ('synthetic 1', 100))
        self.assertEqual(len(pets), 9000)

    def test_empty_delta_is_a_valid_no_op_after_rows_move_into_the_base(self):
        updater = self.changed_delta(self.dictionary([], 102))
        pets = self.refresh(updater)
        self.assertEqual(len(pets), 9000)
        self.assertEqual(pets['1']['name'], 'synthetic 1')
        proof = json.loads((self.auxiliary / 'pet-provenance.json').read_bytes())['packages'][1]
        self.assertEqual(proof['entries'], 0)
        self.assertEqual(proof['decompiledSha256'], sha(self.delta.read_bytes()))

    def test_new_skill_text_replaces_all_used_exports_without_approving_changed_code(self):
        old_text = LiteralCompatibilityTests.MAP
        old = self.write('source/battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigSimpleAC.as', old_text.encode())
        duplicate = self.write('auxiliary/battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigSimpleAC.as', old_text.encode())
        package, row = self.packages['battleconfig/battleconfig']
        package.write_bytes(package.read_bytes() + b' changed')
        row['sha256'] = sha(package.read_bytes())
        fresh_path = self.scratch / 'fresh-battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigSimpleAC.as'
        fresh_path.parent.mkdir(parents=True)
        fresh_path.write_text(old_text.replace('test description', 'new official data'), encoding='utf-8')
        updater = types.SimpleNamespace(scratch=self.scratch, export=mock.Mock(side_effect=AssertionError('Dictionary is unchanged')))
        with mock.patch.object(subject, '_export_battleconfig') as exported:
            changes = subject.refresh_data_packages(updater, self.arguments, [old, duplicate], self.packages, self.approved)
        exported.assert_called_once_with(updater, package, self.scratch / 'fresh-battleconfig')
        self.assertEqual(old.read_bytes(), fresh_path.read_bytes())
        self.assertEqual(duplicate.read_bytes(), fresh_path.read_bytes())
        self.assertEqual({row['sourcePath'] for row in changes}, {str(old.resolve()), str(duplicate.resolve())})
        for receipt in changes:
            self.assertEqual((receipt['beforeSha256'], receipt['afterSha256'], receipt['validation']),
                             (sha(old_text.encode()), sha(fresh_path.read_bytes()), 'skill-literal-skeleton-identical'))
        before = old.read_bytes()
        fresh_path.write_text(old_text.replace('return MAP[id]', 'return MAP[id+1]'), encoding='utf-8')
        with mock.patch.object(subject, '_export_battleconfig'), self.assertRaisesRegex(ValueError, '解析代码变化'):
            subject.refresh_data_packages(updater, self.arguments, [old, duplicate], self.packages, self.approved)
        self.assertEqual(old.read_bytes(), before)
        self.assertEqual(duplicate.read_bytes(), before)


class SkinPackageRefreshTests(OfflineTest):
    @staticmethod
    def row(identifier=1):
        return {'id': identifier, 'name': 'synthetic skin', 'sale': False, 'price': 488, 'show': True,
            'new': False, 'voice': 0, 'poster': False, 'fuse': None, 'fuseFix': False, 'fuseAtLeast': -1,
            'xPosterRes': False, 'xInteractivePosterRes': False, 'interactive': False, 'tryingCardId': -1,
            'xChangeableInfo': None, 'exchangingMaterialStr': None, 'exchangingWeight': 0,
            'changeableRaceIds': None, 'changeableNames': None, 'changeableVoiceDiv': None,
            'changeableWithMovie': False, 'confTag': '',
            'availablePets': [{'no': 0, 'raceId': identifier + 100, 'raceIdNeed': 0}]}

    @staticmethod
    def config(rows, groups):
        return ('package mmo.changepetskinv3.other { public class Cpsv3SkinConfig {\n'
                'private static var SKIN_DEFINES_SRC:Array = ' + encoded(rows).decode() + ';\n'
                'private static var SKIN_GROUPS_SRC:Array = ' + encoded(groups).decode() + ';\n'
                'public function read():Array { return SKIN_DEFINES_SRC; } }}').encode()

    def setUp(self):
        super().setUp()
        self.source = self.work / 'source'
        self.source.mkdir()
        self.skin_source = self.work / 'skin-source'
        self.arguments = {'source': self.source, 'skin_source': self.skin_source}
        self.old_config = self.config([self.row()], [[1]])
        self.new_config = self.config([self.row(), self.row(2)], [[1, 2]])
        self.paths, self.fresh_scripts = [], {}
        for name in sorted(subject.SKIN_CONSUMER_CLASSES | {subject.SKIN_CONFIG_CLASS}):
            raw = self.old_config if name == subject.SKIN_CONFIG_CLASS else ('// exact synthetic reader\n' + name).encode()
            path = self.write('skin-source/scripts/' + name, raw)
            self.paths.append(path)
            self.fresh_scripts[name] = self.new_config if name == subject.SKIN_CONFIG_CLASS else raw
        self.config_path = self.skin_source / 'scripts' / subject.SKIN_CONFIG_CLASS
        self.package = self.write('package.swf', b'Synthetic changed package; never executed')
        self.packages = {subject.SKIN_RESOURCE: (self.package, {'resource': subject.SKIN_RESOURCE,
            'sha256': sha(self.package.read_bytes())})}
        self.approved = {subject.SKIN_RESOURCE: {'sha256': sha(b'Synthetic approved prior package')}}
        self.updater = types.SimpleNamespace(scratch=self.work / 'scratch')
        self.updater.scratch.mkdir()
        self.export = mock.patch.object(subject, '_export_skin_package', side_effect=self.export_scripts)
        self.mock_export = self.export.start()
        self.addCleanup(self.export.stop)

    def export_scripts(self, updater, package, target):
        self.assertEqual(package, self.package)
        self.assertEqual(target, updater.scratch / 'fresh-skin')
        self.assertFalse(target.resolve().is_relative_to(self.skin_source.resolve()))
        for name, raw in self.fresh_scripts.items():
            path = target / 'scripts' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)

    def refresh(self):
        return subject.refresh_skin_package(self.updater, self.arguments, self.paths, self.packages, self.approved)

    def test_both_literal_arrays_refresh_all_saved_copies_and_keep_precise_consumer_receipts(self):
        duplicate = self.write('race-input/scripts/' + subject.SKIN_CONFIG_CLASS, self.old_config)
        self.paths.append(duplicate)
        before = subject._audit_inputs(self.arguments)
        receipts = self.refresh()
        self.assertEqual(len(receipts), 2)
        self.assertEqual(duplicate.read_bytes(), self.new_config)
        self.assertEqual(self.config_path.read_bytes(), self.new_config)
        for receipt in receipts:
            self.assertEqual((receipt['beforeSha256'], receipt['afterSha256']), (sha(self.old_config), sha(self.new_config)))
            self.assertEqual(receipt['validation'], 'skin-config-literal-skeleton-identical')
            self.assertEqual((receipt['beforeCounts']['skins'], receipt['afterCounts']['skins']), (1, 2))
            self.assertEqual(set(row['class'] for row in receipt['validatedConsumers']), subject.SKIN_CONSUMER_CLASSES)
            self.assertTrue(all(row['beforeSha256'] == row['afterSha256'] for row in receipt['validatedConsumers']))
        after = subject._audit_inputs(self.arguments, before)
        self.assertFalse(any('fresh-skin' in row['sourcePath'] for row in after['artifacts']))
        findings = subject._source_release_findings(after, before, approved={}, records=[], data_changes=receipts, controls={})
        self.assertEqual([(row['kind'], row['severity']) for row in findings], [('consumer-script-changed', 'info')])
        self.assertEqual(findings[0]['dataOnlyValidation']['resource'], subject.SKIN_RESOURCE)

    def test_each_consumed_class_change_rejects_before_any_input_is_written(self):
        for name in sorted(subject.SKIN_CONSUMER_CLASSES):
            with self.subTest(class_name=name):
                prior = self.fresh_scripts[name]
                self.fresh_scripts[name] = prior + b'\n// changed reader'
                with self.assertRaisesRegex(ValueError, '皮肤读取规则变化') as error:
                    self.refresh()
                self.assertNotIn('.as', str(error.exception))
                self.assertEqual(self.config_path.read_bytes(), self.old_config)
                self.fresh_scripts[name] = prior

    def test_non_data_code_or_whitespace_changes_reject_before_write(self):
        for changed in (self.new_config.replace(b'return SKIN_DEFINES_SRC', b'return SKIN_GROUPS_SRC'),
                        self.new_config.replace(b'public function', b'public  function'),
                        self.new_config.replace(b'package ', b'\xef\xbb\xbfpackage ', 1)):
            with self.subTest(changed=changed[-80:]):
                self.fresh_scripts[subject.SKIN_CONFIG_CLASS] = changed
                with self.assertRaisesRegex(ValueError, '非数据代码变化'):
                    self.refresh()
                self.assertEqual(self.config_path.read_bytes(), self.old_config)

    def test_unknown_keys_types_duplicate_ids_and_dynamic_expressions_are_rejected(self):
        cases = []
        for field, value in (('unknown', 1), ('show', 1), ('id', True), ('id', 0),
                             ('changeableNames', [1]), ('fuse', [True]), ('availablePets', [{'no': 0, 'raceId': 2, 'raceIdNeed': 1, 'extra': 0}])):
            row = self.row()
            row[field] = value
            cases.append(self.config([row], [[1]]))
        missing = self.row()
        del missing['xChangeableInfo']
        cases.extend([self.config([missing], [[1]]), self.config([self.row(), self.row()], [[1]]),
                      self.config([self.row()], [[True]]), self.config([self.row()], [[999]]),
                      self.old_config.replace(b'"sale":false', b'"sale":false,"sale":true'),
                      self.old_config.replace(b'"price":488', b'"price":NaN'),
                      self.old_config.replace(b'"price":488', b'"price":loadPrice()'),
                      self.old_config.replace(b'[[1]];', b'[[1]]+loadGroups();')])
        for data in cases:
            with self.subTest(data=data[:120]), self.assertRaises(ValueError):
                subject._skin_config_skeleton(data)
        # Official nullable alternatives and raceIdNeed zero are preserved.
        row = self.row()
        row.update(changeableNames=[None, 'display'], changeableVoiceDiv=['1'], changeableRaceIds=[100], fuse=[1])
        subject._skin_config_skeleton(self.config([row], [[1]]))

    def test_missing_saved_reader_or_wrong_package_hash_cannot_skip_validation(self):
        prior = self.paths.pop()
        with self.assertRaisesRegex(ValueError, '完整已核验皮肤消费者'):
            self.refresh()
        self.paths.append(prior)
        self.package.write_bytes(b'tampered bytes')
        with self.assertRaisesRegex(ValueError, '皮肤包字节'):
            self.refresh()
        self.mock_export.assert_not_called()
        self.assertEqual(self.config_path.read_bytes(), self.old_config)

    def test_same_package_reuses_sources_but_changed_ui_only_package_retains_consumer_proof(self):
        self.fresh_scripts[subject.SKIN_CONFIG_CLASS] = self.old_config
        receipts = self.refresh()
        self.assertEqual(len(receipts), 1)
        self.assertFalse(receipts[0]['configurationChanged'])
        self.assertEqual(len(receipts[0]['validatedConsumers']), 5)
        self.mock_export.reset_mock()
        self.approved[subject.SKIN_RESOURCE] = dict(self.packages[subject.SKIN_RESOURCE][1])
        self.assertEqual(self.refresh(), [])
        self.mock_export.assert_not_called()

    def test_reviewed_official_config_pair_matches_the_exact_two_array_contract(self):
        folder = ROOT / 'qa/skin-review'
        old = folder / 'old/decompiled/scripts' / subject.SKIN_CONFIG_CLASS
        new = folder / 'new/decompiled/scripts' / subject.SKIN_CONFIG_CLASS
        if not old.is_file() or not new.is_file():
            self.skipTest('Optional hash-verified local review exports are absent')
        before, old_counts = subject._skin_config_skeleton(old.read_bytes())
        after, new_counts = subject._skin_config_skeleton(new.read_bytes())
        self.assertEqual(before, after)
        self.assertEqual((old_counts['skins'], new_counts['skins']), (693, 697))


class SourceCoverageReleaseTests(OfflineTest):
    def release(self, current, before, *, data_changes=(), approved=None, records=()):
        return subject._source_release_findings(current, before, approved=approved or {}, records=records,
                                                data_changes=data_changes, controls={})

    def test_historical_missing_exports_match_stable_locations_across_scratch_roots(self):
        def report(name):
            source = self.work / name / 'source'
            source.mkdir(parents=True)
            missing = {'binary': {'versiondata': {'file': 'version-data/binary/versiondata.bin', 'sha256': 'a' * 64}},
                       'xml': {'versiondata': {'file': 'version-data/xml/versiondata.xml', 'sha256': 'b' * 64}}}
            for filename in ('provenance.json', 'versiondata-provenance.json'):
                (source / filename).write_bytes(encoded(missing))
            return subject._audit_inputs({'source': source, 'auxiliary': source, 'exports': source / 'ignored'})
        before, current = report('old-scratch'), report('new-scratch')
        self.assertEqual(len(before['issues']), 4)
        self.assertEqual(len(current['issues']), 4)
        findings = self.release(current, before)
        self.assertEqual({(row['severity'], row['baselineStatus']) for row in findings}, {('info', 'existing')})
        self.assertFalse(any('scratch' in row['path'] for row in findings))
        current['reviewRequired'][0]['declaredSha256'] = 'c' * 64
        self.assertEqual(self.release(current, before)[0]['severity'], 'review', 'A changed missing declaration needs review')
        # An existing checksum failure is still an error, not a grandfathered gap.
        item = {'kind': 'declared-hash-mismatch', 'key': 'source:xml/test.xml', 'severity': 'error'}
        before['issues'].append(item)
        current['issues'].append(item)
        current['reviewRequired'].append(item)
        mismatch = next(row for row in self.release(current, before) if row['kind'] == item['kind'])
        self.assertEqual((mismatch['severity'], mismatch['baselineStatus']), ('error', 'existing'))

    def test_data_only_receipt_requires_exact_old_new_script_and_does_not_approve_new_fields(self):
        relative = 'battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigSimpleAC.as'
        path = self.write('source/' + relative, LiteralCompatibilityTests.MAP.encode())
        arguments = {'source': self.work / 'source', 'auxiliary': self.work / 'source', 'exports': self.work / 'exports'}
        before = subject._audit_inputs(arguments)
        original_hash = sha(path.read_bytes())
        path.write_text(LiteralCompatibilityTests.MAP.replace('test description', 'new description'), encoding='utf-8')
        current = subject._audit_inputs(arguments, before)
        receipt = {'sourcePath': str(path.resolve()), 'beforeSha256': original_hash,
                   'afterSha256': sha(path.read_bytes()), 'validation': 'skill-literal-skeleton-identical'}
        findings = self.release(current, before, data_changes=[receipt])
        self.assertEqual(len(findings), 1)
        self.assertEqual((findings[0]['kind'], findings[0]['severity']), ('consumer-script-changed', 'info'))
        self.assertEqual(findings[0]['dataOnlyValidation'], receipt)
        self.assertEqual(self.release(current, before)[0]['severity'], 'review')
        for wrong in ({'beforeSha256': '0' * 64}, {'afterSha256': '0' * 64}, {'sourcePath': str(self.work / 'elsewhere.as')}):
            with self.subTest(wrong=wrong):
                self.assertEqual(self.release(current, before, data_changes=[dict(receipt, **wrong)])[0]['severity'], 'review')
        text = path.read_text(encoding='utf-8').replace('"desc":', '"unknownField":5,"desc":')
        path.write_text(text, encoding='utf-8')
        receipt['afterSha256'] = sha(path.read_bytes())
        current = subject._audit_inputs(arguments, before)
        findings = self.release(current, before, data_changes=[receipt])
        self.assertTrue(any(row['kind'] == 'consumer-script-changed' and row['severity'] == 'info' for row in findings))
        self.assertTrue(any(row['kind'] == 'new-unrecognized-field' and row['severity'] == 'review' for row in findings))

    def test_conflicting_or_nonofficial_h5_history_cannot_prove_same_producer(self):
        records = [{'url': subject.H5 + 'config/battleconfig~202610100001.aqz', 'sha256': 'a' * 64},
                   {'url': 'https://other.invalid/config/test~202610100001.json', 'sha256': 'a' * 64}]
        path = self.write('proof.json', encoded(records))
        approved = subject._h5_data_approvals([path])
        self.assertEqual(set(approved), {'config/battleconfig.aqz'})
        records.append({'url': subject.H5 + 'config/battleconfig~202610110001.aqz', 'sha256': 'b' * 64})
        path.write_bytes(encoded(records))
        self.assertEqual(subject._h5_data_approvals([path]), {})

    def test_quality_code_is_part_of_the_updater_fingerprint(self):
        archive = self.write('bundle.zip', b'synthetic bundle bytes')
        before = subject._code_fingerprint(archive)
        read = subject._read
        def changed_quality(path, *args, **kwargs):
            raw = read(path, *args, **kwargs)
            return raw + b'\n# synthetic parser version change\n' if Path(path).name == 'public_catalog_quality.py' else raw
        with mock.patch.object(subject, '_read', side_effect=changed_quality):
            self.assertNotEqual(before, subject._code_fingerprint(archive))

    def test_existing_alias_promotion_keeps_the_exact_path_and_byte_evidence(self):
        source = self.work / 'project/latest'
        source.mkdir(parents=True)
        old_file = self.write('project/identity-reference/race-expansions.json', encoded({'format': 1}))
        arguments = {'source': source, 'title_source': source.parent, 'race_expansions': old_file}
        before = subject._audit_inputs(arguments)
        new_file = self.write('generated/race-expansions.json', encoded({'format': 1}))
        after = subject._audit_inputs(dict(arguments, race_expansions=new_file), before)
        findings = self.release(after, before)
        self.assertEqual(len(findings), 1)
        self.assertEqual((findings[0]['kind'], findings[0]['severity'], findings[0]['baselineStatus']),
                         ('existing-input-role-reassigned', 'info', 'existing'))
        self.assertEqual(findings[0]['byteProof']['sourcePath'], str(old_file.resolve()))
        self.assertEqual(findings[0]['byteProof']['sha256'], sha(old_file.read_bytes()))


class ReplayUpdateTransactionTests(OfflineTest):
    def setUp(self):
        super().setUp()
        self.research = self.work / "original research"
        self.research.mkdir()
        self.rows = []
        self.calls = []
        self.requested_paths = []
        self.attempt = 0
        self.flash_start_count = self.h5_start_count = 0
        self.switch_flash = self.switch_h5 = False
        self.version = "202610100001"
        self.old_catalog = synthetic_catalog()
        self.catalog_result = copy.deepcopy(self.old_catalog)
        # A full rebuild must pass through every builder field, including fields
        # the update component does not itself understand or display.
        self.catalog_result["replayCultivation"] = {"opaqueNewOfficialField": {"raw": [1, "中文", None]}}
        self.catalog_result["referenceCoverage"] = {"unresolved": ["preserve without guessing"]}
        self.catalog_result["unknownFutureTopLevel"] = {"payload": [0, False, {"x": "y"}]}
        self.old_raw = encoded(self.old_catalog)
        self.old_pack = pack.pack_bytes(self.old_raw)
        self.baseline = self.work / "baseline"
        self.baseline.mkdir()
        (self.baseline / "battle-replay-catalog.brpack").write_bytes(self.old_pack)
        self.data_root = self.work / "installed test data"
        self.catalog = self.data_root / "catalog"
        self.catalog.mkdir(parents=True)
        self.active = self.catalog / "battle-replay-catalog.brpack"
        self.active.write_bytes(self.old_pack)
        resources = sorted(subject.DATA_PACKAGES | {"library/interfaces", "battle/battle", "loader", subject.SKIN_RESOURCE})
        self.packages = {name: swf_payloads({"test." + name.replace("/", "_"): b"<synthetic/>"}) for name in resources}
        self.aqz = aqz_bytes([("skill_config.json", b"{}"), ("skill_desc_config.json", b"{}"), ("extra-table.json", b'{"keep":true}')])
        self.package_proof = [{"resource": name, "version": self.version, "url": subject.FLASH + name + "~" + self.version + ".swf",
                               "sha256": sha(data)} for name, data in self.packages.items()]
        self.add_source("source/provenance.json", {"packages": [row for row in self.package_proof if row["resource"] in {"library/interfaces", "battle/battle", "battleconfig/battleconfig"}]})
        self.h5_consumer = b"// synthetic pinned consumer; never executed"
        self.add_source("verified/proof.json", {"flash": self.package_proof,
            "h5": {"url": subject.H5 + "js/aqlib~" + self.version + ".js", "sha256": sha(self.h5_consumer)}})
        # The historical bundle deliberately omitted members, but retained the
        # explicit producer's approved bytes. Discovery is not official novelty.
        self.add_source("source/h5-provenance.json", {"files": [{"resource": "config/battleconfig.aqz",
            "url": subject.H5 + "config/battleconfig~" + self.version + ".aqz", "sha256": sha(self.aqz),
            "file": "h5-battleconfig.aqz"}]})
        self.add_source("auxiliary/pets.json", {})
        self.add_source("cultivation/provenance.json", {"files": [{"resource": "config/test.json",
            "url": subject.H5 + "config/test~20211126.json", "sha256": sha(b'{"synthetic":true}'),
            "file": "raw/config/test.json", "matchesPreviouslyVerifiedBytes": True}]})
        self.add_source("source/scripts/mmo/test/Consumer.as", b"package test {}")
        self.add_source('evidence/reference-consumers.json', {'schemaVersion': 1})
        self.add_source('evidence/race-expansions.json', {'format': 1})
        self.skin_config = SkinPackageRefreshTests.config([SkinPackageRefreshTests.row()], [[1]])
        for name in sorted(subject.SKIN_CONSUMER_CLASSES | {subject.SKIN_CONFIG_CLASS}):
            raw = self.skin_config if name == subject.SKIN_CONFIG_CLASS else ('// exact synthetic reader\n' + name).encode()
            self.add_source('skin-source/scripts/' + name, raw)
        self.archive = self.work / "update-sources.zip"
        bundle.create_bundle(self.rows, {"source": "source", "verified_source": "verified", "auxiliary": "auxiliary",
                                           "cultivation_h5_source": "cultivation", "skin_source": "skin-source",
                                           "reference_consumers": "evidence/reference-consumers.json",
                                           "race_expansions": "evidence/race-expansions.json"}, self.archive)
        self.source_bytes = {row["projectRelative"]: Path(row["path"]).read_bytes() for row in self.rows}
        # Removing the original path proves the live update does not fall back
        # to the developer's research checkout or mutate any historical input.
        self.relocated_research = self.work / "offline research snapshot"
        self.research.rename(self.relocated_research)
        mapping = {"dummy/" + str(i): self.version for i in range(100)}
        mapping.update({name: self.version for name in resources})
        mapping["battle/battle"] = "202610090001"
        xml = "<vconfig>" + "".join(f'<f n="{name}" v="{version}"/>' for name, version in mapping.items()) + "</vconfig>"
        self.version_swf = swf_payloads({"mmo.versiondata.DocClass_cls": xml.encode()})
        self.flash_start = ('<start><v>' + self.version + '</v><lv>199901010000</lv><u><f n="battle/battle" v="' + self.version + '"/></u></start>').encode()
        self.main_js = b"// synthetic pinned H5 resource loader; never executed"
        self.h5_start = encoded({"version": self.version, "files": {"js/aqlib.mix": "202610100003"}})
        self.h5_versions = encoded({"js/aqlib.mix": "202610100002", "js/aqlib.js": "202610100099",
                                   "config/battleconfig.aqz": self.version})
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(public_session, "H5_MAIN_SHA256", sha(self.main_js)))
        self.stack.enter_context(mock.patch.object(public, "emit"))
        self.stack.enter_context(mock.patch.object(subject, "_code_fingerprint", return_value="c" * 64))
        self.refresh = self.stack.enter_context(mock.patch.object(subject, "refresh_data_packages", return_value=[]))
        self.prepare = self.stack.enter_context(mock.patch("update_generated_evidence.prepare_reference_and_race", side_effect=lambda arguments, **kwargs: arguments))
        self.build = self.stack.enter_context(mock.patch("build_catalog.build", side_effect=self.build_catalog))
        # This transaction fixture deliberately has no real official tables or
        # real battle data. The independent source reader and native process
        # each have full raw-input tests; here verify both are mandatory gates.
        self.fidelity = self.stack.enter_context(mock.patch('public_replay_fidelity.audit_replay_fidelity',
            side_effect=lambda candidate, **kwargs: {'schemaVersion': 1, 'passed': True,
                'candidateContentSha256': quality._digest(candidate), 'inputs': [{'file': 'synthetic'}], 'findings': []}))
        self.regression = self.stack.enter_context(mock.patch('public_replay_regression.audit_replay_regression',
            side_effect=lambda path, candidate, **kwargs: {'schemaVersion': 1, 'passed': True,
                'candidateContentSha256': quality._digest(candidate)}))

    def add_source(self, relative, value):
        raw = value if isinstance(value, bytes) else encoded(value)
        path = self.research / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
        self.rows.append({"path": str(path), "projectRelative": relative, "bytes": len(raw), "sha256": sha(raw)})

    def fetch(self, url, output):
        self.calls.append(url)
        self.requested_paths.append(output)
        path = urlsplit(url).path
        if path == "/play/start.xml":
            self.flash_start_count += 1
            raw = self.flash_start
            if self.switch_flash and self.flash_start_count % 2 == 0:
                raw = raw.replace(self.version.encode(), b'202610110001')
        elif path == "/play/versiondata~" + self.version + ".swf":
            raw = self.version_swf
        elif path.startswith("/play/") and path.endswith(".swf"):
            resource, version = path[len("/play/"):-4].split("~", 1)
            self.assertEqual(version, self.version, "start.u override must beat versiondata")
            raw = self.packages[resource]
        elif path.startswith("/h5/start~"):
            self.h5_start_count += 1
            raw = self.h5_start
            if self.switch_h5 and self.h5_start_count % 2 == 0:
                value = json.loads(raw)
                value['version'] = '202610110001'
                raw = encoded(value)
        elif path == "/h5/version~" + self.version + ".json":
            raw = self.h5_versions
        elif path == "/h5/js/main~" + self.version + ".js":
            raw = self.main_js
        elif path == "/h5/js/aqlib~202610100003.js":
            raw = self.h5_consumer
        elif path == "/h5/config/battleconfig~" + self.version + ".aqz":
            raw = self.aqz
        elif path == "/h5/config/test~20211126.json":
            raw = b'{"synthetic":true}'
        else:
            raise AssertionError("Unexpected download URL: " + url)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(raw)
        return {"url": url, "sha256": sha(raw), "bytes": len(raw)}

    def build_catalog(self, **arguments):
        for key, path in arguments.items():
            self.assertTrue(path.resolve().is_relative_to(self.last_updater.scratch.resolve()), (key, path))
        self.assertEqual((arguments["source"] / "h5-tables/extra-table.json").read_bytes(), b'{"keep":true}')
        proof = json.loads((arguments["cultivation_h5_source"] / "provenance.json").read_bytes())
        self.assertNotIn("matchesPreviouslyVerifiedBytes", proof["files"][0])
        return copy.deepcopy(self.catalog_result)

    def update(self):
        self.attempt += 1
        scratch = self.work / f"scratch-{self.attempt}"
        scratch.mkdir()
        self.last_updater = public.Updater(self.data_root, self.baseline, scratch, fetcher=self.fetch, isolated_workspace=True,
                                           exporter=lambda *args: self.fail("Unexpected real decompiler request"))
        return subject.update_battle_replay(self.last_updater, bundle_path=self.archive)

    def assert_old_unchanged(self):
        self.assertEqual(self.active.read_bytes(), self.old_pack)
        self.assertEqual((self.baseline / "battle-replay-catalog.brpack").read_bytes(), self.old_pack)
        self.assertEqual(sorted(path.name for path in self.catalog.iterdir()), ["battle-replay-catalog.brpack"])

    def quality_report(self):
        return json.loads((self.data_root / 'data-tools/quality/battleReplay-latest-attempt.json').read_bytes())

    def test_full_transaction_retains_all_fields_follows_both_official_chains_and_stays_in_scratch(self):
        changed, metadata = self.update()
        self.assertTrue(changed)
        raw = pack.unpack_bytes(self.active.read_bytes())
        pack.validate_catalog_bytes(raw)
        result = json.loads(raw)
        for key, value in self.catalog_result.items():
            self.assertEqual(result[key], value, key)
        self.assertEqual(metadata["counts"], {"pets": 1, "skills": 1, "buffs": 1})
        self.assertEqual(metadata["jsonBytes"], len(raw))
        self.assertEqual(metadata["packBytes"], self.active.stat().st_size)
        self.assertIn(subject.FLASH + "versiondata~" + self.version + ".swf", self.calls)
        self.assertFalse(any("199901010000" in url for url in self.calls))
        sources = {row["resource"]: row for row in result["publicUpdate"]["h5Sources"]}
        self.assertEqual(sources["js/aqlib.js"]["resolution"], "start-files")
        self.assertEqual(sources["config/test.json"]["resolution"], "default-20211126")
        self.assertTrue(all(path.resolve().is_relative_to(self.last_updater.scratch.resolve()) for path in self.requested_paths))
        for relative, before in self.source_bytes.items():
            self.assertEqual((self.relocated_research / relative).read_bytes(), before)
        self.assertFalse(self.research.exists())
        report = self.quality_report()
        self.assertTrue(report['passed'])
        self.assertEqual(report['candidateContentSha256'], quality._digest(result))
        self.assertEqual(report['previousContentSha256'], quality._digest(self.old_catalog))
        self.assertEqual(self.last_updater.quality_results['battleReplay']['candidateContentSha256'], quality._digest(result))
        coverage = report['sourceCoverage']
        extra = next(row for row in coverage['releaseFindings'] if row['path'] == 'source:h5-tables/extra-table.json')
        self.assertEqual((extra['severity'], extra['baselineStatus']), ('info', 'new'))
        self.assertEqual(extra['producerProof']['approvedProducerSha256'], sha(self.aqz))
        artifact = next(row for row in coverage['artifacts'] if row['key'] == extra['path'])
        self.assertEqual(artifact['integration'], 'not-integrated', 'Same bytes prove age, never semantic coverage')
        self.assertEqual(artifact['fields']['/keep']['coverage'], 'unknown-field')
        before = json.loads((self.last_updater.scratch / 'replay-update/source-coverage-before.json').read_bytes())
        self.assertEqual(before['materialStage'], 'historical-bundle-input-subset')
        self.assertFalse(before['officialFreshnessVerified'])
        self.assertEqual(coverage['materialStage'], 'refreshed-local-inputs-after-current-resource-byte-verification')

    def test_unchanged_package_is_kept_byte_for_byte_without_rebuilding(self):
        self.assertTrue(self.update()[0])
        before, before_time = self.active.read_bytes(), self.active.stat().st_mtime_ns
        self.build.reset_mock()
        self.build.side_effect = AssertionError("An unchanged catalog must not be rebuilt")
        changed, metadata = self.update()
        self.assertFalse(changed)
        self.assertEqual((self.active.read_bytes(), self.active.stat().st_mtime_ns), (before, before_time))
        self.assertEqual(metadata["counts"]["skills"], 1)
        self.build.assert_not_called()
        report = self.quality_report()
        self.assertTrue(report['passed'])
        self.assertEqual(report['candidateContentSha256'], quality._digest(subject._validate_pack(self.active)))
        self.assertEqual(report['previousContentSha256'], report['candidateContentSha256'])
        self.assertTrue((self.last_updater.scratch / 'replay-update/source-coverage-before.json').is_file())
        self.assertTrue((self.last_updater.scratch / 'replay-update/source-coverage-after.json').is_file())
        self.assertIn('battleReplay', self.last_updater.quality_results)
        self.assertEqual(self.fidelity.call_count, 2)
        self.assertEqual(self.regression.call_count, 2)
        self.assertTrue(report['sourceFidelity']['passed'])
        self.assertTrue(report['replayRegression']['passed'])

    def test_source_value_mismatch_preserves_old_pack(self):
        self.fidelity.side_effect = lambda candidate, **kwargs: {'schemaVersion': 1, 'passed': False,
            'candidateContentSha256': quality._digest(candidate), 'inputs': [{'file': 'synthetic'}],
            'findings': [{'severity': 'error', 'kind': 'value-mismatch', 'path': '/skills/1/description', 'message': 'synthetic mismatch'}]}
        with self.assertRaises(quality.CatalogQualityError): self.update()
        self.assert_old_unchanged()
        self.assertFalse(self.quality_report()['sourceFidelity']['passed'])

    def test_native_failure_preserves_changed_and_unchanged_pack(self):
        self.regression.side_effect = lambda path, candidate, **kwargs: {'schemaVersion': 1, 'passed': False,
            'candidateContentSha256': quality._digest(candidate), 'errors': ['synthetic native failure']}
        with self.assertRaises(quality.CatalogQualityError): self.update()
        self.assert_old_unchanged()
        self.regression.side_effect = lambda path, candidate, **kwargs: {'schemaVersion': 1, 'passed': True,
            'candidateContentSha256': quality._digest(candidate)}
        self.assertTrue(self.update()[0])
        before = self.active.read_bytes(), self.active.stat().st_mtime_ns
        self.regression.side_effect = lambda path, candidate, **kwargs: {'schemaVersion': 1, 'passed': False,
            'candidateContentSha256': quality._digest(candidate)}
        with self.assertRaises(quality.CatalogQualityError): self.update()
        self.assertEqual((self.active.read_bytes(), self.active.stat().st_mtime_ns), before)

    def test_unproved_new_table_rejects_before_version_commit_and_retains_failure_report(self):
        self.aqz = aqz_bytes([('skill_config.json', b'{}'), ('skill_desc_config.json', b'{}'),
                              ('extra-table.json', b'{"keep":true}'), ('new-table.json', b'{"unexplained":12}')])
        with self.assertRaises(quality.CatalogQualityError) as error:
            self.update()
        self.assert_old_unchanged()
        report = self.quality_report()
        self.assertFalse(report['passed'])
        self.assertEqual(report['decision'], 'rejected-kept-existing')
        finding = next(row for row in report['sourceCoverage']['releaseFindings'] if row['path'] == 'source:h5-tables/new-table.json')
        self.assertEqual((finding['kind'], finding['severity'], finding['baselineStatus']), ('new-unmapped-table', 'review', 'new'))
        self.assertNotIn('producerProof', finding)
        self.assertEqual(self.last_updater.quality_results['battleReplay'], error.exception.metadata)
        self.assertEqual((self.flash_start_count, self.h5_start_count), (1, 1), 'Quality refusal must precede stable_versions')

    def test_overlapping_title_root_and_generated_provenance_are_audited_without_false_release_refusal(self):
        from update_generated_evidence import REFERENCE_CONSUMERS
        def generate(arguments, **kwargs):
            arguments = dict(arguments, title_source=arguments['source'].parent)
            arguments['reference_consumers'].write_bytes(encoded({'schemaVersion': 1,
                'consumerApprovals': {name: {'file': str(arguments['source'] / (name + '.as')), 'sha256': 'a' * 64}
                                      for name in REFERENCE_CONSUMERS}}))
            arguments['race_expansions'].write_bytes(encoded({'format': 1, 'inputTables': [
                {'file': str(arguments['auxiliary'] / 'pets.json'), 'sha256': sha(b'{}')}]}))
            return arguments
        self.prepare.side_effect = generate
        self.assertTrue(self.update()[0])
        report = self.quality_report()
        self.assertTrue(report['passed'])
        coverage = report['sourceCoverage']
        extra = next(row for row in coverage['artifacts'] if row['key'] == 'source:h5-tables/extra-table.json')
        self.assertIn('title_source', extra['roles'])
        self.assertTrue(any(alias['aliasOf'] == extra['key'] and alias['role'] == 'title_source' for alias in coverage['aliases']))
        self.assertFalse(any(row['severity'] == 'review' for row in coverage['releaseFindings']))
        self.assertEqual(coverage['coverage']['fieldCounts']['local-generated-provenance'], 56)
        self.assertEqual(report['candidateContentSha256'], quality._digest(subject._validate_pack(self.active)))

    def test_unrecognized_generated_provenance_sibling_still_rejects_the_transaction(self):
        def generate(arguments, **kwargs):
            arguments['reference_consumers'].write_bytes(encoded({'schemaVersion': 1,
                'consumerApprovals': {'skill_icon': {'file': 'test.as', 'sha256': 'a' * 64, 'unreviewedMeaning': True}}}))
            return arguments
        self.prepare.side_effect = generate
        with self.assertRaises(quality.CatalogQualityError):
            self.update()
        self.assert_old_unchanged()
        finding = next(row for row in self.quality_report()['sourceCoverage']['releaseFindings']
                       if row['path'].endswith('#/consumerApprovals/skill_icon/unreviewedMeaning'))
        self.assertEqual((finding['kind'], finding['severity']), ('new-unrecognized-field', 'review'))

    def test_unchanged_fingerprint_still_rejects_inconsistent_refreshed_input(self):
        self.assertTrue(self.update()[0])
        before, modified = self.active.read_bytes(), self.active.stat().st_mtime_ns
        refresh_h5 = subject.refresh_h5
        def corrupt_extracted_table(updater, arguments, *args):
            result = refresh_h5(updater, arguments, *args)
            (arguments['source'] / 'h5-tables/extra-table.json').write_bytes(b'{"keep":false}')
            return result
        self.build.reset_mock()
        self.build.side_effect = AssertionError('Unchanged resources must not rebuild')
        with mock.patch.object(subject, 'refresh_h5', side_effect=corrupt_extracted_table), self.assertRaises(quality.CatalogQualityError):
            self.update()
        self.assertEqual((self.active.read_bytes(), self.active.stat().st_mtime_ns), (before, modified))
        self.build.assert_not_called()
        report = self.quality_report()
        self.assertFalse(report['passed'])
        self.assertTrue(any(row['kind'] == 'declared-hash-mismatch' and row['severity'] == 'error'
                            for row in report['sourceCoverage']['releaseFindings']))
        self.assertEqual(report['candidateContentSha256'], quality._digest(subject._validate_pack(self.active)))

    def test_missing_or_invalid_active_catalog_uses_validated_baseline_for_quality_comparison(self):
        for invalid in (None, b'corrupt external data'):
            with self.subTest(invalid=invalid):
                if invalid is None:
                    self.active.unlink()
                else:
                    self.active.write_bytes(invalid)
                self.assertTrue(self.update()[0])
                report = self.quality_report()
                self.assertEqual(report['previousContentSha256'], quality._digest(self.old_catalog))
                self.assertEqual(report['candidateContentSha256'], quality._digest(subject._validate_pack(self.active)))

    def test_unreviewed_consumer_change_fails_before_rebuild_and_preserves_active(self):
        self.packages["battle/battle"] = swf_payloads({"test.changed": b"<changed/>"})
        with self.assertRaisesRegex(ValueError, "官方依赖已变化"):
            self.update()
        self.build.assert_not_called()
        self.assert_old_unchanged()

    def test_changed_skin_package_passes_only_after_all_six_exact_inputs_are_checked(self):
        self.packages[subject.SKIN_RESOURCE] = swf_payloads({'test.changed_skin_data': b'<synthetic changed="true"/>'})
        config = SkinPackageRefreshTests.config([SkinPackageRefreshTests.row(), SkinPackageRefreshTests.row(2)], [[1, 2]])
        def export(updater, package, target):
            skin = updater._prepared_replay['arguments']['skin_source']
            for name in subject.SKIN_CONSUMER_CLASSES | {subject.SKIN_CONFIG_CLASS}:
                path = target / 'scripts' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(config if name == subject.SKIN_CONFIG_CLASS else (skin / 'scripts' / name).read_bytes())
        with mock.patch.object(subject, '_export_skin_package', side_effect=export) as exported:
            self.assertTrue(self.update()[0])
        exported.assert_called_once()
        self.build.assert_called_once()
        report = self.quality_report()
        self.assertTrue(report['passed'])
        receipts = report['sourceCoverage']['validatedDataOnlyScriptChanges']
        self.assertEqual(len(receipts), 1)
        self.assertEqual((receipts[0]['beforeSha256'], receipts[0]['afterSha256']), (sha(self.skin_config), sha(config)))
        self.assertEqual(receipts[0]['producerSha256'], sha(self.packages[subject.SKIN_RESOURCE]))
        self.assertEqual(receipts[0]['approvedProducerSha256'], next(r['sha256'] for r in self.package_proof if r['resource'] == subject.SKIN_RESOURCE))
        self.assertEqual(report['candidateContentSha256'], quality._digest(subject._validate_pack(self.active)))
        finding = next(row for row in report['sourceCoverage']['releaseFindings'] if row['kind'] == 'consumer-script-changed')
        self.assertEqual(finding['severity'], 'info')
        self.assertEqual(finding['dataOnlyValidation']['validation'], 'skin-config-literal-skeleton-identical')

    def test_changed_skin_consumer_keeps_old_catalog_and_never_builds(self):
        self.packages[subject.SKIN_RESOURCE] = swf_payloads({'test.changed_skin_code': b'<synthetic changed="true"/>'})
        def export(updater, package, target):
            skin = updater._prepared_replay['arguments']['skin_source']
            for name in subject.SKIN_CONSUMER_CLASSES | {subject.SKIN_CONFIG_CLASS}:
                path = target / 'scripts' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                raw = (skin / 'scripts' / name).read_bytes()
                path.write_bytes(raw + b'\n// changed consumer' if name.endswith('Cpsv3SkinSummary.as') else raw)
        with mock.patch.object(subject, '_export_skin_package', side_effect=export), self.assertRaisesRegex(ValueError, '皮肤读取规则变化'):
            self.update()
        self.build.assert_not_called()
        self.assert_old_unchanged()

    def test_bad_builder_output_and_missing_collection_do_not_replace_old_pack(self):
        self.catalog_result.pop("replayCultivation")
        with self.assertRaisesRegex(ValueError, "missing catalog object"):
            self.update()
        self.assert_old_unchanged()

    def test_large_core_table_drop_is_rejected_even_when_schema_is_valid(self):
        self.old_catalog["skills"]["4"] = dict(self.old_catalog["skills"]["2"], id="4")
        self.old_catalog["tableJoinSummary"]["skills"]["skills"] = 2
        baseline = pack.pack_bytes(encoded(self.old_catalog))
        (self.baseline / "battle-replay-catalog.brpack").write_bytes(baseline)
        with self.assertRaisesRegex(ValueError, "明显不完整"):
            self.update()
        self.assertEqual(self.active.read_bytes(), self.old_pack)

    def test_completeness_also_compares_with_the_newer_active_catalog(self):
        newer = copy.deepcopy(self.old_catalog)
        newer['skills']['4'] = dict(newer['skills']['2'], id='4')
        newer['tableJoinSummary']['skills']['skills'] = 2
        active_before = pack.pack_bytes(encoded(newer))
        self.active.write_bytes(active_before)
        with self.assertRaisesRegex(ValueError, '明显不完整'):
            self.update()
        self.assertEqual(self.active.read_bytes(), active_before)
        self.assertEqual((self.baseline / 'battle-replay-catalog.brpack').read_bytes(), self.old_pack)

    def test_mid_update_flash_or_h5_manifest_switch_keeps_old_package(self):
        for mode in ("flash", "h5"):
            self.switch_flash, self.switch_h5 = mode == "flash", mode == "h5"
            self.flash_start_count = self.h5_start_count = 0
            with self.subTest(chain=mode), self.assertRaisesRegex(ValueError, "切换版本"):
                self.update()
            self.assert_old_unchanged()

    def test_atomic_replace_failure_preserves_previous_package_and_cleans_partial_output(self):
        original_replace = pack.os.replace
        def fail_active(source, destination):
            if Path(destination).resolve() == self.active.resolve():
                raise OSError("synthetic final replacement failure")
            return original_replace(source, destination)
        with mock.patch.object(pack.os, "replace", side_effect=fail_active), self.assertRaisesRegex(OSError, "synthetic final replacement"):
            self.update()
        self.assert_old_unchanged()


if __name__ == "__main__":
    unittest.main()
