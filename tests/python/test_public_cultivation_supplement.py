"""Offline tests of existing display projections and transactional staging."""
from __future__ import annotations

import copy
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import public_cultivation_supplement as subject


def sha(value):
    return hashlib.sha256(value).hexdigest()


def swf(symbol, data):
    def tag(code, value=b""):
        return (struct.pack("<H", code << 6 | len(value)) if len(value) < 63 else
                struct.pack("<HI", code << 6 | 63, len(value))) + value
    body = (b"\0\0\0\1\0" + tag(87, struct.pack("<HI", 1, 0) + data) +
            tag(76, struct.pack("<HH", 1, 1) + symbol.encode() + b"\0") + tag(0))
    return b"CWS\x0d" + struct.pack("<I", len(body) + 8) + zlib.compress(body)


class CultivationSupplementTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="cultivation-supplement-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        blocked = mock.patch("urllib.request.urlopen", side_effect=AssertionError("No real network"))
        blocked.start()
        self.addCleanup(blocked.stop)
        self.versions = {resource: "202610110001" for _, resource, _, _ in subject.SUPPLEMENT_RESOURCES}
        self.packages, self.proof, self.scripts = {}, {}, {}
        self.updater = types.SimpleNamespace(scratch=self.root / "scratch", resource=mock.Mock(side_effect=self.resource),
                                             export=mock.Mock(side_effect=self.export))
        self.updater.scratch.mkdir()
        self.current = {"unknownFutureField": {"keep": [1, 2]}, "source": {"resources": {}},
                        "sourceBeasts": {}, "legendStones": {}, "proficiencies": {}}
        self.resources = {"existing": {"keep": True}}
        source = "\n".join('new E4PV2_EInfo(' + str(i) + ',"test beast ' + str(i) + '",0,"2,3");' for i in range(1, 21))
        legend = 'ITEM_DEFINES_SRC:Array = [{\n' + '\n},{\n'.join(
            '"id":' + str(i) + ',"name":"test stone ' + str(i) + '","levelDefines":[{"level":1,"desc":"test effect"}]'
            for i in range(1, 19)) + '\n}];'
        for _, resource, table, selected in subject.SUPPLEMENT_RESOURCES:
            package = self.root / (resource.replace('/', '_') + '.swf')
            if table == "proficiencies":
                payload = ('<config><proinfos>' + ''.join(
                    '<proinfo id="' + str(i) + '" name="test potential ' + str(i) + '" desc="test effect"></proinfo>'
                    for i in range(1, 21)) + '</proinfos></config>').encode()
                package.write_bytes(swf(subject.PROFICIENT_SYMBOL, payload))
            else:
                package.write_bytes(b'SYNTHETIC PACKAGE ' + resource.encode())
                script = self.root / table / (selected.rsplit('.', 1)[-1] + '.as')
                script.parent.mkdir()
                script.write_text(source if table == 'sourceBeasts' else legend, encoding='utf-8')
                self.scripts[selected] = script
            self.packages[resource] = package
            self.proof[resource] = {"version": self.versions[resource], "sha256": sha(package.read_bytes()),
                                    "url": "https://aoqi.100bt.com/play/" + resource + "~202610110001.swf"}

    def resource(self, key, versions):
        self.assertEqual(versions, self.versions)
        return self.packages[key], dict(self.proof[key])

    def export(self, package, destination, selected):
        self.assertTrue(destination.is_relative_to(self.updater.scratch))
        return self.scripts[selected]

    def refresh(self):
        subject.refresh_supplement(self.updater, self.current, self.versions, self.resources)

    def test_full_projection_retains_original_fields_and_exact_source_proofs(self):
        self.assertTrue(subject.needs_supplement(self.current, self.versions))
        self.refresh()
        self.assertEqual(self.current['sourceBeasts']['1'], {'name': 'test beast 1', 'attr': '2,3'})
        self.assertEqual(self.current['legendStones']['1'], {'name': 'test stone 1', 'levels': {'1': {'desc': 'test effect'}}})
        self.assertEqual(self.current['proficiencies']['1'], {'name': 'test potential 1', 'desc': 'test effect'})
        self.assertEqual(self.current['unknownFutureField'], {'keep': [1, 2]})
        self.assertEqual(self.resources['existing'], {'keep': True})
        for label, resource, table, selected in subject.SUPPLEMENT_RESOURCES:
            proof = self.resources[label]
            self.assertEqual((proof['resource'], proof['version'], proof['definitions']),
                             (resource, self.versions[resource], len(self.current[table])))
            self.assertEqual(proof['sha256'], sha(self.packages[resource].read_bytes()))
            if table != 'proficiencies':
                self.assertEqual(proof['decompiledSha256'], sha(self.scripts[selected].read_bytes()))
            else:
                self.assertEqual(proof['symbol'], subject.PROFICIENT_SYMBOL)
                self.assertEqual(len(proof['payloadSha256']), 64)
        self.current['source']['resources'] = self.resources
        self.assertFalse(subject.needs_supplement(self.current, self.versions))
        self.assertEqual(self.updater.resource.call_count, 3)
        self.assertEqual(self.updater.export.call_count, 2)

    def test_rule_upgrade_repairs_same_version_catalog_and_changed_resource_invalidates(self):
        self.refresh()
        self.current['source']['resources'] = self.resources
        self.current.pop(subject.RULE_FIELD)
        self.assertTrue(subject.needs_supplement(self.current, self.versions))
        self.current[subject.RULE_FIELD] = subject.RULE_VERSION
        for _, resource, table, _ in subject.SUPPLEMENT_RESOURCES:
            changed = dict(self.versions, **{resource: '202610110002'})
            self.assertTrue(subject.needs_supplement(self.current, changed))
            current = copy.deepcopy(self.current)
            current[table] = {}
            self.assertTrue(subject.needs_supplement(current, self.versions))

    def test_same_version_changed_package_hash_refreshes_the_complete_supplement(self):
        class Versions(dict):
            pass
        self.versions = Versions(self.versions)
        self.versions.source_hashes = {key: value["sha256"] for key, value in self.proof.items()}
        self.refresh()
        self.current["source"]["resources"] = self.resources
        self.assertFalse(subject.needs_supplement(self.current, self.versions))
        original_versions = dict(self.versions)
        resource = "equipment4petver2/equipment4petver2service"
        path = self.packages[resource]
        path.write_bytes(path.read_bytes() + b"\nSYNTHETIC SAME-VERSION REVISION")
        self.proof[resource]["sha256"] = sha(path.read_bytes())
        self.versions.source_hashes[resource] = self.proof[resource]["sha256"]
        selected = "mmo.equipment4petver2.config.E4PV2_EInfos"
        script = self.scripts[selected]
        script.write_text(script.read_text(encoding="utf-8").replace('"test beast 1"', '"revised beast 1"'), encoding="utf-8")
        self.assertTrue(subject.needs_supplement(self.current, self.versions))
        self.refresh()
        self.assertEqual(dict(self.versions), original_versions)
        self.assertEqual(self.current["sourceBeasts"]["1"]["name"], "revised beast 1")
        self.assertEqual(self.resources["sourceBeastCatalog"]["sha256"], self.proof[resource]["sha256"])
        self.assertEqual(self.current["unknownFutureField"], {"keep": [1, 2]})
        self.assertFalse(subject.needs_supplement(self.current, self.versions))

    def test_last_table_failure_does_not_partially_mutate_current_or_source_records(self):
        before = copy.deepcopy((self.current, self.resources))
        resource = 'proficient/proficient'
        self.packages[resource].write_bytes(swf('unrelated.Symbol', b'<config/>'))
        self.proof[resource]['sha256'] = sha(self.packages[resource].read_bytes())
        with self.assertRaisesRegex(ValueError, '指定内嵌数据'):
            self.refresh()
        self.assertEqual((self.current, self.resources), before)
        self.assertEqual(self.updater.export.call_count, 2)

    def test_large_definition_drop_is_blocked_before_publication(self):
        self.current['sourceBeasts'] = {str(i): {'name': 'previous'} for i in range(1, 25)}
        before = copy.deepcopy((self.current, self.resources))
        with self.assertRaisesRegex(ValueError, '不完整'):
            self.refresh()
        self.assertEqual((self.current, self.resources), before)

    def test_package_hash_mismatch_blocks_the_entire_supplement(self):
        self.proof['legend/legendservice']['sha256'] = '0' * 64
        before = copy.deepcopy((self.current, self.resources))
        with self.assertRaisesRegex(ValueError, '来源校验失败'):
            self.refresh()
        self.assertEqual((self.current, self.resources), before)

    def test_xml_with_duplicates_or_rows_outside_the_official_collection_is_rejected(self):
        resource = 'proficient/proficient'
        for extra in ('<proinfo id="1" name="duplicate"></proinfo>', '<other><proinfo id="99" name="outside"></proinfo></other>'):
            original = ('<config><proinfos>' + ''.join('<proinfo id="' + str(i) + '" name="entry"></proinfo>'
                        for i in range(1, 21)) + '</proinfos>' + extra + '</config>').encode()
            self.packages[resource].write_bytes(swf(subject.PROFICIENT_SYMBOL, original))
            self.proof[resource]['sha256'] = sha(self.packages[resource].read_bytes())
            before = copy.deepcopy((self.current, self.resources))
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                self.refresh()
            self.assertEqual((self.current, self.resources), before)

    def test_xml_entities_are_verified_without_changing_the_existing_projection(self):
        resource = 'proficient/proficient'
        payload = ('<config><proinfos>' + ''.join(
            '<proinfo id="' + str(i) + '" name="A &amp; B" desc="less &lt; more"></proinfo>'
            for i in range(1, 21)) + '</proinfos></config>').encode()
        self.packages[resource].write_bytes(swf(subject.PROFICIENT_SYMBOL, payload))
        self.proof[resource]['sha256'] = sha(self.packages[resource].read_bytes())
        self.refresh()
        self.assertEqual(self.current['proficiencies']['1'], {'name': 'A &amp; B', 'desc': 'less &lt; more'})


if __name__ == '__main__':
    unittest.main()
