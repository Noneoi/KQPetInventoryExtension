"""Explicit activity selector tests over frozen synthetic official tables."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import activity_evolution_selector as selector


MANAGER = '''
public function getPetEvoLinkAllRaceIds(param1:Array,param2:Boolean=true):Array {
 if(param1==null){param1=[];}
 param1=this.resetRealPetRaceIds(param1);
 param1=this.checkAddAllMultiFormIds(param1);
 param1=this.findAllNextEvoRaceIds(param1);
 if(param2){param1=this.checkAddAllMultiFormIds(param1);param1=this.checkAddAllSkinIds(param1);}
 return param1;
}
private function findPetEvoDef(param1:int):PetEvoPanelDefine {
 for each(row in PetEvoPanelConfig.ObjDataByExcel){if(row.arrayPutPetRaceIds.indexOf(param1)>=0)return row;}
 for each(row in PetEvoPanelConfig.ObjData){if(row.arrayPutPetRaceIds.indexOf(param1)>=0)return row;}
 return null;
}
private function resetRealPetRaceIds(param1:Array):Array {return this.getSwitchSkinPetRaceIds(param1[0]);}
private function checkAddAllMultiFormIds(param1:Array):Array {return this.getSwitchPetRaceIds(param1.indexOf(1));}
private function findAllNextEvoRaceIds(param1:Array):Array {return findPetEvoDef(param1.indexOf(row.raceId));}
'''
DEFINE = '''public function tryInitDefine():void {this._arrayPutPetRaceIds=this._strPutPetRaceIds.split("#");}'''
CONFIG = '''public static const ObjDataByExcel:Object = {
 "200":new PetEvoPanelDefine(200,"evolved",2,"100",null),
 "300":new PetEvoPanelDefine(300,"later",2,"200",null),
 "201":new PetEvoPanelDefine(201,"other form evolves",2,"101",null)
}; public static const ObjData:Object = {"999":new PetEvoPanelDefine(999,"lower priority",2,"100")};'''
FORMS = '''private static const FORM_CONF:Array = [new MultipleFormPetFormDefine([100,101]),new MultipleFormPetFormDefine([200,202])];'''
SKINS = '''private static var SKIN_DEFINES_SRC:Array = [
 {"show":true,"fuse":null,"availablePets":[{"raceId":1000,"raceIdNeed":100}]},
 {"show":false,"fuse":null,"availablePets":[{"raceId":1001,"raceIdNeed":100}]},
 {"show":true,"fuse":null,"availablePets":[{"raceId":3000,"raceIdNeed":300}]},
 {"show":true,"fuse":[1,2],"availablePets":[{"raceId":9000,"raceIdNeed":0}]}
];'''
SUMMARY = '''public static function getRaceIds(param1:int,param2:Boolean,param3:Boolean):Array {
 var _loc4_=findOriginalRaceId(param1);
 if(!skin.isShow() && skin.findRaceIdNeed(param1)!=-1)return null;
 var _loc5_=[skin.findRaceId(_loc4_)];
 return [_loc4_].concat(_loc5_);
}'''
SKIN = '''public function isFusionPoster():Boolean {return this._fuse!=null;}'''


class FakeUpdater:
    def __init__(self, root):
        self.root, self.catalog, self.scratch = root, root / "catalog", root / "scratch"
        self.scratch.mkdir()
        self.calls = []
        self.scripts = {"PetEvoPanelManager": MANAGER, "PetEvoPanelConfig": CONFIG, "PetEvoPanelDefine": DEFINE,
                        "MultipleFormPetConfig": FORMS, "Cpsv3SkinConfig": SKINS, "Cpsv3SkinSummary": SUMMARY, "Cpsv3Skin": SKIN}

    def resource(self, key, versions):
        self.calls.append(key)
        path = self.scratch / (key.rsplit("/", 1)[-1] + ".swf")
        path.write_bytes(b"frozen fixture")
        return path, {"version": versions[key], "url": "https://aoqi.100bt.com/play/" + key + "~" + versions[key] + ".swf"}

    def export(self, swf, directory, selected):
        directory.mkdir(parents=True, exist_ok=True)
        name = selected.rsplit(".", 1)[-1]
        path = directory / (name + ".as")
        path.write_text(self.scripts[name], encoding="utf-8")
        return path


class ActivityEvolutionSelectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.updater = FakeUpdater(Path(self.temp.name))
        self.versions = {key: "2026091011614836" for _, key, _ in selector.RESOURCES}

    def expand(self, seeds):
        return selector.expand_selector(self.updater, self.versions, seeds)

    def test_skin_normalization_then_initial_forms_then_forward_evolution_only(self):
        self.assertEqual(self.expand([1000]), [100, 200, 300, 101, 201])
        # false means neither the downstream target's second form nor skin is
        # appended, and the legacy table must not override the Excel edge.
        self.assertNotIn(202, self.expand([1000]))
        self.assertNotIn(3000, self.expand([1000]))
        self.assertNotIn(999, self.expand([1000]))

    def test_hidden_and_fused_appearance_keep_the_official_reset_behavior(self):
        self.assertEqual(self.expand([1001]), [1001])
        self.assertEqual(self.expand([9000]), [9000])

    def test_cycle_terminates_without_duplicate_races(self):
        self.updater.scripts["PetEvoPanelConfig"] = CONFIG.replace('"300":new PetEvoPanelDefine(300,"later",2,"200",null)',
            '"100":new PetEvoPanelDefine(100,"cycle",2,"200",null)')
        self.assertEqual(self.expand([100, 100]), [100, 200, 101, 201])

    def test_ambiguous_object_enumeration_does_not_invent_all_branches(self):
        self.updater.scripts["PetEvoPanelConfig"] = CONFIG.replace('"201":new PetEvoPanelDefine(201,"other form evolves",2,"101",null)',
            '"201":new PetEvoPanelDefine(201,"ambiguous",2,"100",null)')
        with self.assertRaisesRegex(ValueError, "匹配多个目标"):
            self.expand([100])

    def test_same_versions_reuse_one_current_cache_without_download(self):
        self.expand([100])
        path = self.updater.catalog / selector.FILENAME
        before = path.read_bytes()
        self.updater.calls.clear()
        self.assertEqual(self.expand([100]), [100, 200, 300, 101, 201])
        self.assertEqual(self.updater.calls, [])
        self.assertEqual(path.read_bytes(), before)

    def test_changed_dependency_reexpands_existing_selector_and_only_fetches_that_module(self):
        before_revision = selector.dependency_revision(self.versions)
        self.expand([100])
        self.updater.calls.clear()
        self.versions["multipleformpet/multipleformpetservice"] = "2026091812345678"
        self.updater.scripts["MultipleFormPetConfig"] = FORMS.replace("[100,101]", "[100,101,102]")
        self.assertNotEqual(selector.dependency_revision(self.versions), before_revision)
        self.assertEqual(self.expand([100]), [100, 200, 300, 101, 201, 102])
        self.assertEqual(self.updater.calls, ["multipleformpet/multipleformpetservice"])

    def test_unrecognized_hotpatch_algorithm_preserves_previous_graph(self):
        self.expand([100])
        path = self.updater.catalog / selector.FILENAME
        before = path.read_bytes()
        self.versions["pet/petdataservice"] = "260911418760321"
        self.updater.scripts["PetEvoPanelManager"] = MANAGER.replace(
            "param1=this.resetRealPetRaceIds(param1);", "param1=this.getArbitraryNewMapping(param1);")
        with self.assertRaisesRegex(ValueError, "展开顺序已变化"):
            self.expand([100])
        self.assertEqual(path.read_bytes(), before)

    def test_invalid_or_empty_seeds_do_not_fetch_public_resources(self):
        self.assertEqual(self.expand([]), [])
        for values in ([0], [-1], [True], [1.5], ["7264@sb"]):
            with self.assertRaises(ValueError):
                self.expand(values)
        self.assertEqual(self.updater.calls, [])


if __name__ == "__main__":
    unittest.main()
