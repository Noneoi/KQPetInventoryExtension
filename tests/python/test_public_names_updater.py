import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from public_names_updater import (enum_names, fusion_jobs, equipment_source_names, update_names,
                                  needs_names_update, NAME_RESOURCES)

ATTR = 'public static const NORMAL:PetAttr = enum__(1,"普"); public static const NEW:PetAttr = enum__(99,"新属性",true);'
JOBS = '''public static const ONE:PetJob = enum__(1,"新物理职业",TYPE,"新职业");
public static const TWO:PetJob = enum__(88,"第二职业",TYPE);
public static const EMPTY:PetJob = enum__(89,"保留职业",null,"");
public static const FUSION_JOBS:Array = [{"jobs":[[1],[88]],"name":"新组合"}];'''


class NamesTests(unittest.TestCase):
    def setUp(self):
        blocked = mock.patch("urllib.request.urlopen", side_effect=AssertionError("No real network"))
        blocked.start()
        self.addCleanup(blocked.stop)

    def test_new_non_contiguous_ids_and_official_short_names(self):
        self.assertEqual(enum_names(ATTR, "PetAttr"), {"1":"普","99":"新属性"})
        jobs = enum_names(JOBS, "PetJob")
        self.assertEqual(jobs["1"], "新职业")
        self.assertEqual(jobs["89"], "保留职业")
        self.assertEqual(fusion_jobs(JOBS,jobs), [{"jobs":[[1],[88]],"name":"新组合"}])

    def test_invalid_names_and_unresolved_fusion_are_rejected(self):
        with self.assertRaises(ValueError):
            enum_names(ATTR + 'public const OTHER:PetAttr = enum__(1,"重复");', "PetAttr")
        with self.assertRaises(ValueError):
            enum_names('public const BAD:PetAttr = enum__(1,callCode());', "PetAttr")
        with self.assertRaises(ValueError):
            fusion_jobs(JOBS, {"1":"新职业"})

    def test_source_equipment_mapping_is_generic_and_preserves_exact_name(self):
        self.assertEqual(equipment_source_names('x = new E4PV2_EInfo(114,"神·薇斯佩拉源兽",1);\n'
                                                'y = new E4PV2_EInfo(900,"新源兽",2);'),
                         {"114":"神·薇斯佩拉源兽","900":"新源兽"})
        with self.assertRaises(ValueError):
            equipment_source_names('new E4PV2_EInfo(1,unknownName,2)')

    def test_update_names_stages_all_sheets_and_same_version_does_no_work(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            class Fake:
                scratch = root
                calls = []
                def resource(self, key, versions):
                    self.calls.append(key)
                    path = root / (key.replace('/', '_')+'.swf')
                    path.write_bytes(b'FWSnot-a-money-update')
                    return path, {"version": versions[key]}
                def export(self, swf, target, selected):
                    target.mkdir(parents=True, exist_ok=True)
                    path = target / (selected.rsplit('.',1)[-1]+'.as')
                    source = ATTR if selected.endswith('PetAttr') else JOBS if selected.endswith('PetJob') else '"1":new Money(1,"金币",1),"777":new Money(777,"新货币",1)'
                    path.write_text(source,encoding='utf-8')
                    return path
            updater = Fake()
            current = {"source":{"resources":{}},"attributes":{},"jobs":{},"money":{}}
            versions = {key:"2026091012345678" for _,key in NAME_RESOURCES}
            resources = {}
            self.assertTrue(update_names(updater,current,versions,resources))
            self.assertEqual(current["money"]["777"]["name"],"新货币")
            self.assertEqual(current["fusionJobs"][0]["name"],"新组合")
            current["source"]["resources"] = resources
            updater.calls.clear()
            self.assertFalse(needs_names_update(current,versions))
            self.assertFalse(update_names(updater,current,versions,resources))
            self.assertEqual(updater.calls,[])

    def test_same_version_changed_package_hash_rebuilds_only_affected_name_sheets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            class Versions(dict):
                pass
            class Fake:
                scratch = root
                def __init__(self):
                    self.calls = []
                    self.texts = {"PetAttr": ATTR, "PetJob": JOBS,
                                  "MoneyData": '"1":new Money(1,"金币",1),"777":new Money(777,"基础货币",1)',
                                  "MoneyData_Update": '"888":new Money(888,"增量货币",1)'}
                    self.sheets = {NAME_RESOURCES[0][1]: ("PetAttr", "PetJob"),
                                   NAME_RESOURCES[1][1]: ("MoneyData",), NAME_RESOURCES[2][1]: ("MoneyData_Update",)}
                def payload(self, key):
                    return b"FWS" + '\n'.join(name + '\n' + self.texts[name] for name in self.sheets[key]).encode()
                def resource(self, key, versions):
                    self.calls.append(key)
                    path = root / (key.replace('/', '_') + '.swf')
                    path.write_bytes(self.payload(key))
                    return path, {"version": versions[key], "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                def export(self, swf, target, selected):
                    target.mkdir(parents=True, exist_ok=True)
                    name = selected.rsplit('.', 1)[-1]
                    path = target / (name + '.as')
                    path.write_text(self.texts[name], encoding='utf-8')
                    return path
            updater = Fake()
            versions = Versions({key: "2026091012345678" for _, key in NAME_RESOURCES})
            versions.source_hashes = {key: hashlib.sha256(updater.payload(key)).hexdigest() for key in versions}
            current = {"source": {"resources": {}}, "attributes": {}, "jobs": {}, "money": {}}
            resources = {}
            self.assertTrue(update_names(updater, current, versions, resources))
            current["source"]["resources"] = resources
            self.assertFalse(needs_names_update(current, versions))
            cases = [("PetAttr", "新属性", "修订属性", NAME_RESOURCES[0][1], "attributes", "99", [NAME_RESOURCES[0][1]]),
                     ("MoneyData", "基础货币", "修订基础货币", NAME_RESOURCES[1][1], "money", "777", [key for _, key in NAME_RESOURCES[1:]]),
                     ("MoneyData_Update", "增量货币", "修订增量货币", NAME_RESOURCES[2][1], "money", "888", [key for _, key in NAME_RESOURCES[1:]])]
            for sheet, old, new, key, table, identifier, expected_calls in cases:
                with self.subTest(sheet=sheet):
                    updater.calls.clear()
                    updater.texts[sheet] = updater.texts[sheet].replace(old, new)
                    versions.source_hashes[key] = hashlib.sha256(updater.payload(key)).hexdigest()
                    self.assertTrue(needs_names_update(current, versions))
                    self.assertTrue(update_names(updater, current, versions, resources))
                    self.assertEqual(updater.calls, expected_calls)
                    value = current[table][identifier]
                    self.assertEqual(value if isinstance(value, str) else value["name"], new)
                    self.assertFalse(needs_names_update(current, versions))
                    updater.calls.clear()
                    self.assertFalse(update_names(updater, current, versions, resources))
                    self.assertEqual(updater.calls, [])


if __name__ == "__main__":
    unittest.main()
