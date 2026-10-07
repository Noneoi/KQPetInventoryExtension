"""Static activity discovery and incremental refresh contracts."""
from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import public_activity_exchange_updater as activity


def uint(value):
    data = bytearray()
    while value >= 128:
        data.append((value & 127) | 128)
        value >>= 7
    return bytes(data + bytes([value]))


def swf(strings=(), xml=""):
    def tag(kind, body):
        return struct.pack("<H", (kind << 6) | 63) + struct.pack("<I", len(body)) + body
    body = b"\x00\x00\x00\x01\x00"
    if xml:
        body += tag(87, b"\x01\x00\x00\x00\x00\x00" + xml.encode())
    if strings:
        abc = b"\x10\x00\x2e\x00\x01\x01\x01" + uint(len(strings) + 1)
        for value in strings:
            text = value.encode()
            abc += uint(len(text)) + text
        body += tag(82, b"\x00\x00\x00\x00\x00" + abc)
    body += b"\x00\x00"
    return b"FWS\x12" + struct.pack("<I", len(body) + 8) + body


def script(rows, cls="FutureExchangeConfig", extra=""):
    return f'public class {cls} {{ public static const SHOP_ID:int=7; {extra} public static const EXCHANGES:Array=' + json.dumps(rows) + ";}"


class FakeUpdater:
    module = "newactivityext/newact20260911/futureexchange/futureexchange"
    empty = "newactivityext/newact20260911/newempty/newempty"
    old = "newactivityext/newact20200101/oldarchive/oldarchive"
    def __init__(self, root):
        self.root, self.scratch, self.baseline, self.tools = root, root / "scratch", root / "baseline", root / "tools"
        self.scratch.mkdir()
        self.start_version, self.runtimes = "20260911111111111", ("test", "test")
        self.calls, self.fail, self.new = [], False, False
        self.rows = [{"serverId": 0, "id": 1, "cost": "4:3266:500", "simpleParams": "CommonEnhancePrize,-1,123,92,8001#8002",
                      "basicDescription": "未来精灵培养", "limit": "4:1"}]
        self.registry = ('<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="./newactivityconfig.xsd">'
                         '<week version="20260911"><a name="futureexchange" desc="未来活动" file="' + self.module + '"/>'
                         '<a name="newempty" desc="没有指定兑换" file="' + self.empty + '"/></week>'
                         '<week version="20200101"><a name="oldarchive" file="' + self.old + '"/></week></config>')
    def resource(self, name, versions):
        target = self.scratch / "config.swf"
        target.write_bytes(swf(xml=self.registry))
        self.calls.append(name)
        return target, {"version": versions[name]}
    def export(self, path, target, cls):
        target.mkdir(exist_ok=True)
        script = target / "CommonHudConfig.as"
        script.write_text('public static const DATA:Object={"hud":[{"name":"未来活动","tryGetService":"NewActivityService#loadAndInitNormalActivity#futureexchange#showMainPanel"}]};', encoding="utf-8")
        return script
    def fetch(self, url, target):
        self.calls.append(url)
        if self.fail and "futureexchange" in url:
            raise OSError("offline")
        target.write_bytes(swf(strings=["CommonEnhancePrize"] if "futureexchange" in url else ["ordinary"] ))
        return {"url": url}
    def activity_exporter(self, path, target):
        return {"FutureExchangeConfig.as": script(self.rows)}


class Contracts(unittest.TestCase):
    def test_literal_choice_package_survives_dynamic_shop_wrapper_without_guessed_price(self):
        scripts = {"FutureShop.as": 'static const GOODS:Array=[new Row(1,buildIcon(),new Price(88),new Meta({'
                   '"name":"自选礼包","bonus":"139:11:1#4:55:10"}))];'}
        shops,pending,_ = activity.parse_module(scripts,"future/dynamic-shop",{},include_manual=True,
                                               selectable_packages={"11":{"selectable":True}})
        self.assertEqual(pending,[])
        good, = shops[0]["goods"]
        self.assertEqual(good["selectablePackageIds"],[11])
        self.assertTrue(good["manualSelectionRequired"] and good["displayIdentityOnly"])
        self.assertEqual(good["acquisitionKind"],"unknown")
        self.assertFalse(good["costKnown"])
        self.assertEqual(shops[0]["observation"]["requests"],[])

    def test_package_definition_change_invalidates_same_activity_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            updater.rows = [{"id":0,"simpleParams":"Material,139:11:1,1"}]
            original_fetch = updater.fetch
            def fetch(url,path):
                source = original_fetch(url,path)
                if "futureexchange" in url:
                    path.write_bytes(swf(strings=["simpleParams","Material,139:11:1,1"]))
                return source
            updater.fetch = fetch
            updater.selectable_package_source = {"versions":{"library/materialdataupdate":"2026100700000000"},"packages":{"11":{}}}
            versions = {activity.CONFIG_RESOURCE:"2026100700000000",updater.module:"2026100700000000",updater.empty:"2026100700000000"}
            with patch.dict(sys.modules,{"activity_evolution_selector":type("Selector",(),{"dependency_revision":staticmethod(lambda v:"same")})}):
                activity.update_activity_exchanges(updater,versions)
                path = updater.root / "catalog" / activity.FILENAME
                self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["shops"],[])
                updater.selectable_package_source = {"versions":{"library/materialdataupdate":"2026100700000001"},"packages":{}}
                activity.update_activity_exchanges(updater,versions)
                self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["shops"],[])

    def test_selectable_package_definition_not_name_controls_inclusion(self):
        definitions = activity.parse_selectable_packages('static const data:Object={'
            '"11":new OpenablePackage(11,"普通名字","24:51:1#24:54:1",999,""),'
            '"12":new OpenablePackage(12,"任选宣传","24:51:1%0.5#24:54:1%0.5",999,"")};')
        selected = {key:row for key,row in definitions.items() if row["selectable"]}
        rows = [{"id":0,"simpleParams":"Material,139:11:1#4:55:10,1"},
                {"id":1,"simpleParams":"Material,139:12:1,1"},
                {"id":2,"simpleParams":"Material,139:99:1,1"}]
        shops,pending,_ = activity.parse_module({"New.as":script(rows,"New")},"future/packages",{},
                                               include_manual=True,selectable_packages=selected)
        self.assertEqual(pending, [])
        good, = shops[0]["goods"]
        self.assertEqual(good["selectablePackageIds"],[11])
        self.assertTrue(good["manualSelectionRequired"])
        self.assertEqual(activity.relevant_shops(shops),[])
        with self.assertRaises(ValueError):
            activity.parse_selectable_packages('static const data:Object={"1":new OpenablePackage(1,"x",dynamic(),999,"")};')

    def test_package_cache_refreshes_changed_definitions_without_old_choice_leaking(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            versions = {key:"2026100700000000" for key,_ in activity.PACKAGE_RESOURCES}
            calls = []
            def resource(key, versions):
                calls.append(key)
                path = updater.scratch / (key.rsplit("/",1)[-1]+".as")
                content = "24:51:1#24:54:1"
                if key.endswith("update") and versions[key].endswith("1"):
                    content = "24:51:1%1#24:54:1%1"
                path.write_text('static const data:Object={"11":new OpenablePackage(11,"box","'+content+'",999,"")};',encoding="utf-8")
                return path,{"version":versions[key]}
            updater.resource = resource
            updater.export = lambda path,*args:path
            first = activity.load_selectable_packages(updater,versions)
            self.assertIn("11",first["packages"])
            path = updater.root / "catalog" / activity.FILENAME
            activity._atomic_json(path,{"source":{"selectablePackages":first}})
            self.assertEqual(activity.load_selectable_packages(updater,versions),first)
            self.assertEqual(len(calls),2)
            versions["library/materialdataupdate"] = "2026100700000001"
            self.assertEqual(activity.load_selectable_packages(updater,versions)["packages"],{})

    def test_future_pass_reward_lanes_keep_only_choices_and_no_shared_counter(self):
        rows = [{"index":1,"idN":1,"prizeNormal":"Material,4:55:10,1","idE":10,
                 "prizeExtreme":"!Choice,|Material,24:51:1,1|Material,24:54:1,1"},
                {"index":2,"idN":2,"prizeNormal":"CommonEnhancePrize,-1,1,92,8001","idE":11,
                 "prizeExtreme":"Material,4:55:10,1"}]
        shops,pending,_ = activity.parse_module({"NewPass.as":script(rows,"NewPass")},"future/pass",{},include_manual=True)
        self.assertEqual(pending,[])
        self.assertEqual(len(shops),2)
        self.assertEqual(len({s["sourceKey"] for s in shops}),2)
        for shop in shops:
            self.assertEqual(shop["observation"]["requests"],[])
            good, = shop["goods"]
            self.assertTrue(good["manualSelectionRequired"] and good["displayIdentityOnly"])
            self.assertFalse(good["costKnown"] or good["quotaKnown"])

    def test_future_tiered_signin_keeps_exact_rewards_and_scopes_duplicate_ids(self):
        rows = [{"index": 0, "lv": level, "day": 1, "desc": "任选精灵",
                 "simpleParams": f"Choice,|PetOpen,{8000+level}|PetOpen,9001"}
                for level in (1, 2, 3)]
        rows += [{"index": 1, "lv": level, "day": 2,
                  "simpleParams": "Material,4:55:2,1"} for level in (1, 2, 3)]
        text = script(rows, "FutureSigninConfig", 'public static const TabId:int=3; '
                      'public static const ReshowSelfActKey:String="btnNewAct_future_showMainPanel_"+TabId;')
        text += 'function getData():Array {for each(row in EXCHANGES) {if(row["lv"] == _lv) result.push(row);}}'
        parsed, pending, _ = activity.parse_module({"FutureSigninConfig.as": text}, "unseen/future/module", {}, include_manual=True)
        self.assertEqual(pending, [])
        self.assertEqual([s["rewardTier"] for s in parsed], [1, 2, 3])
        self.assertEqual(len({s["sourceKey"] for s in parsed}), 3)
        self.assertTrue(all(s["navigationLink"] == "btnNewAct_future_showMainPanel_3" for s in parsed))
        for s in parsed:
            self.assertEqual(s["observation"]["requests"], [])
            good, = s["goods"]
            self.assertEqual(good["itemServerId"], 0)
            self.assertTrue(good["manualSelectionRequired"] and good["displayIdentityOnly"])
            self.assertFalse(good["costKnown"] or good["quotaKnown"])
            self.assertEqual(good["conditionsRaw"]["lv"], s["rewardTier"])
        # A repeated ID without the official tier filter remains a conflict.
        _, pending, _ = activity.parse_module({"FutureSigninConfig.as": script(rows,"FutureSigninConfig")}, "module", {}, include_manual=True)
        self.assertEqual(len(pending), 2)

    def test_named_stargod_choice_preserves_target_parameter(self):
        rows = [{"index": 0, "simpleParams": "Choice,|CommonEnhancePrize,-1,31807,35$65,6501|Material,4:1336:10,31807"}]
        parsed, pending, _ = activity.parse_module({"NewConfig.as":script(rows,"NewConfig")},"new/module",{},include_manual=True)
        self.assertEqual(pending, [])
        good, = parsed[0]["goods"]
        self.assertEqual((good["enhanceType"],good["raceIds"]), ("35$65",[6501]))
        self.assertTrue(good["manualSelectionRequired"])

    def test_overview_and_unused_reward_wrapper_do_not_create_fake_candidates(self):
        scripts = {"Overview.as": 'static const PRIZE_DEFINES:Array=[{"index":0,"mStrForActIcon":"34",'
                   '"jumpStr":"btnNewAct_future_showMainPanel_3"}];',
                   "Icon.as": 'icon.initByStr("CommonEnhancePrize,,,"+code+",");'}
        parsed, pending, _ = activity.parse_module(scripts,"overview",{},include_manual=True)
        self.assertEqual((parsed,pending),([],[]))
        wrapper = 's=(s as String).replace("ChoiceCompensate,","Choice,|CompensateMaterial,");'
        self.assertEqual(activity.parse_module({"Wrapper.as":wrapper},"empty",{},include_manual=True)[:2], ([],[]))
        # A genuine runtime-only reward remains diagnosed, without a fake row.
        parsed,pending,_ = activity.parse_module({"Dynamic.as":'v="CommonEnhancePrize,,,"+compute()+","+race;'},"new/module",{},include_manual=True)
        self.assertEqual(parsed,[])
        self.assertTrue(pending)

    def test_last_known_good_preservation_filters_legacy_material_and_entry_rows(self):
        previous = {"shops":[{"sourceKey":"module#table","goods":[
            {"itemServerId":0,"rewardRaw":"ActivityEntry,module"},
            {"itemServerId":1,"rewardRaw":"Material,4:55:10,1"},
            {"itemServerId":2,"rewardRaw":"Choice,|PetOpen,8001|PetOpen,8002"}]}]}
        kept = activity.preserve_pending_rows(previous,[],[{"reason":"unknown new syntax"}])
        self.assertEqual(kept,[])

    def test_manual_scan_includes_exchange_and_claim_rewards(self):
        rows = [{"id": 0, "cost": "4:55:80", "simpleParams": "CommonEnhancePrize,-1,1,34,7529"},
                {"id": 1, "cost": "4:55:10", "simpleParams": "Material,4:55:1,1"},
                {"id": 2, "simpleParams": "Choice,|Material,4:55:2,1|PetOpen,8001"},
                {"id": 3, "simpleParams": "Material,4:55:2,1"}]
        shops, pending, _ = activity.parse_module({"FutureExchangeConfig.as": script(rows)}, "module", {}, include_manual=True)
        self.assertEqual([g["itemServerId"] for g in shops[0]["goods"]], [0, 2])
        self.assertFalse(shops[0]["goods"][0]["manualSelectionRequired"])
        self.assertTrue(shops[0]["goods"][1]["manualSelectionRequired"])
        self.assertEqual(pending, [])

    def test_signin_and_progress_without_invented_prices(self):
        rows = [{"day": 1, "simpleParams": "Choice,|PetOpen,8001|PetOpen,8002"},
                {"day": 2, "simpleParams": "CommonEnhancePrize,-1,1,34,8001"},
                {"day": 3, "simpleParams": "Material,4:55:2,1"}]
        scripts = {"SignInConfig.as": script(rows, "SignInConfig")}
        shops, pending, _ = activity.parse_module(scripts, "module", {}, include_manual=True)
        self.assertEqual(pending, [])
        self.assertEqual(len(shops[0]["goods"]), 2)
        self.assertTrue(all(g["manualSelectionRequired"] and g["displayIdentityOnly"] and not g["costKnown"]
                            and g["acquisitionKind"] == "signin" for g in shops[0]["goods"]))
        self.assertEqual(shops[0]["observation"]["requests"], [])
        again, _, _ = activity.parse_module({"SignInConfig.as": script(rows[::-1], "SignInConfig")}, "module", {}, include_manual=True)
        self.assertEqual({g["rewardRaw"]: g["itemServerId"] for g in shops[0]["goods"]},
                         {g["rewardRaw"]: g["itemServerId"] for g in again[0]["goods"]})
        progress = [{"id": 7,"daibi": 100,"simpleParams": "CommonEnhancePrize,-1,1,34,7529"}]
        parsed, _, _ = activity.parse_module({"ProgressConfig.as": script(progress,"ProgressConfig",'const COMMAND_PROGRESS:String="1008_999_1";')}, "module", {}, include_manual=True)
        good = parsed[0]["goods"][0]
        self.assertEqual(good["acquisitionKind"], "progress")
        self.assertFalse(good["costKnown"])
        self.assertTrue(good["manualSelectionRequired"])

    def test_static_typed_reward_and_named_reward_fields(self):
        scripts = {"DailyConfig.as": 'public static const PRIZES:Array=[new Gift(3,"Choice,|PetOpen,8001|PetOpen,8002")];',
                   "Gift.as": 'public function Gift(param1:int,param2:String) {this._day=param1;this._reward=param2;} '
                              'public function get day():int {return this._day;} public function get reward():String {return this._reward;}'}
        parsed, pending, _ = activity.parse_module(scripts,"module",{},include_manual=True)
        self.assertEqual(pending, [])
        self.assertEqual(parsed[0]["goods"][0]["rewardRaw"], "Choice,|PetOpen,8001|PetOpen,8002")
        scripts["Gift.as"] = scripts["Gift.as"].replace('this._reward=param2;', 'this._reward=compute(param2);')
        parsed, _, _ = activity.parse_module(scripts,"module",{},include_manual=True)
        self.assertEqual(parsed, [])

    def test_typed_constructor_reward_table_with_diamond_evidence(self):
        config = ('public static const RACE_ID:int=7605; public static const DEFINES:Array='
                  '[{"index":0,"param":"4:2143:10","cost":8,"limit":17,"serverIdForTimes":1},'
                  '{"index":1,"param":"11-62-31","cost":729,"limit":1,"serverIdForTimes":3,"serverIdForBuying":103}];')
        wrapper = ('this._index=p["index"]; this._param=p["param"]; this._cost=p["cost"]; '
                   'this._limit=p["limit"]; this._serverIdForTimes=p["serverIdForTimes"]; '
                   'reward.initByStr(this._param.indexOf(":") >= 0 ? "Material," + this._param : '
                   '"CommonEnhancePrize,,," + this._param + "," + NewConfig.RACE_ID); '
                   'for each(row in NewConfig.DEFINES) { values.push(new TypedReward(row)); }')
        scripts = {"NewConfig.as": config, "TypedReward.as": wrapper,
                   "NewDialog.as": 'var reward:TypedReward=null; enableConsume(AQGDialog.CT_DIAMOND,reward.cost);'}
        shops, pending, _ = activity.parse_module(scripts, "future/module", {}, include_manual=True)
        self.assertEqual(pending, [])
        self.assertEqual(len(shops), 1)
        auto, = shops[0]["goods"]
        self.assertFalse(auto["manualSelectionRequired"])
        self.assertEqual((auto["itemServerId"], auto["raceIds"], auto["cost"]), (103, [7605], "8:2:729"))
        scripts.pop("NewDialog.as")
        unknown, pending, _ = activity.parse_module(scripts, "future/module", {}, include_manual=True)
        self.assertEqual(unknown, [])  # No inferred diamond price from a plain number.
        self.assertTrue(pending)

    def test_material_only_module_is_scanned_and_negative_cache_is_invalidated(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            updater.rows = [{"id": 4, "cost": "4:55:10", "simpleParams": "Material,4:55:1,1"}]
            original_fetch = updater.fetch
            def fetch(url, target):
                source = original_fetch(url, target)
                if "futureexchange" in url:
                    target.write_bytes(swf(strings=["simpleParams", "cost", "Material,4:55:1,1"]))
                return source
            updater.fetch = fetch
            versions = {activity.CONFIG_RESOURCE: "20260911111111111", updater.module: "20260911111111111", updater.empty: "20260911111111111"}
            from activity_evolution_selector import RESOURCES
            versions.update({key: "20260911111111111" for _, key, _ in RESOURCES})
            activity.update_activity_exchanges(updater, versions)
            saved = json.loads((updater.root / "catalog" / activity.FILENAME).read_text(encoding="utf-8"))
            self.assertEqual(saved["shops"], [])
            self.assertEqual(saved["modules"][updater.module]["status"], "no-relevant-rewards")
            saved["parserVersion"] = activity.PARSER_VERSION - 1
            saved["shops"] = []
            saved["modules"][updater.module]["shops"] = []
            (updater.root / "catalog" / activity.FILENAME).write_text(json.dumps(saved), encoding="utf-8")
            updater.rows = [{"id": 4, "simpleParams": "CommonEnhancePrize,-1,1,92,8001"}]
            activity.update_activity_exchanges(updater, versions)
            saved = json.loads((updater.root / "catalog" / activity.FILENAME).read_text(encoding="utf-8"))
            self.assertEqual(len(saved["shops"]), 1)

    def test_literal_reward_choice_cost_and_zero_item_id(self):
        rows = [{"id": 0, "cost": "4:55:80", "simpleParams": "Choice,|CommonEnhancePrize,-1,1,34,7529#7545:false,GAIN_BATCH|Material,4:1:1,1"},
                {"id": 1, "cost": "4:55:10", "simpleParams": "Material,4:55:1,1"},
                {"id": 2, "simpleParams": "CommonEnhancePrize,-1,1,92,7529"}]
        shops, pending, evo = activity.parse_module({"FutureExchangeConfig.as": script(rows)}, "module", {"activityName": "未来活动"})
        self.assertEqual(len(shops), 1)
        self.assertEqual(len(shops[0]["goods"]), 1)
        good = shops[0]["goods"][0]
        self.assertEqual(good["itemServerId"], 0)
        self.assertEqual(good["raceIds"], [7529, 7545])
        self.assertTrue(good["costKnown"])
        self.assertFalse(good["quotaKnown"])
        self.assertFalse(good["availableKnown"])
        self.assertEqual(good["exchangeKind"], "activity")
        self.assertEqual(pending, [])

    def test_new_class_strengthen_template_and_unresolved_currency(self):
        text = ('public class FreshSaleConfig { public static const RACE_ID:int=8001; '
                'public static const LIMIT_RACEIDS:Array=[8001,8002]; public static const PRIZES:Array=['
                '{"index":0,"bi":0,"type":"Strengthen","params":"31-92","prices":[100,50],"filter":FreshSaleConfig.LIMIT_RACEIDS}];}')
        consumer = 'v="CommonEnhancePrize,-1,1,"+row["params"]+","+FreshSaleConfig.RACE_ID; selectPets(row["filter"]);'
        shops, pending, _ = activity.parse_module({"FreshSaleConfig.as": text, "UnrelatedView.as": consumer}, "module", {"activityName": "新活动"})
        self.assertEqual(shops[0]["goods"][0]["raceIds"], [8001, 8002])
        self.assertEqual(shops[0]["goods"][0]["costRaw"], {"prices": [100, 50]})
        self.assertFalse(shops[0]["goods"][0]["costKnown"])
        self.assertEqual(pending, [])

    def test_evolution_selector_is_expanded_and_unknown_rules_are_pending(self):
        rows = [{"id": 0, "cost": 140, "simpleParams": "CommonEnhancePrize,-1,1,92,7545@sb"},
                {"id": 1, "cost": 140, "simpleParams": "CommonEnhancePrize,-1,1,92,7545:true"}]
        seeds = []
        def expand(values):
            seeds.extend(values)
            return [7545, 7546]
        shops, pending, evo = activity.parse_module({"FutureExchangeConfig.as": script(rows)}, "module", {}, expand)
        self.assertEqual(shops[0]["goods"][0]["raceIds"], [7545, 7546])
        self.assertEqual(seeds, [7545])
        self.assertEqual(len(pending), 1)
        self.assertTrue(evo)

    def test_no_as_calls_or_arithmetic_are_executed(self):
        with self.assertRaises(ValueError):
            activity.LiteralReader('[dangerous(),1]').parse()
        with self.assertRaises(ValueError):
            activity.LiteralReader('[1+2]').parse()

    def test_navigation_requires_an_exact_official_route(self):
        self.assertEqual(activity.navigation_link("futureexchange", {}, {}), "")
        self.assertEqual(
            activity.navigation_link(
                "futureexchange",
                {"link": "btnNewAct_futureexchange_showMainPanel_2"},
                {},
            ),
            "btnNewAct_futureexchange_showMainPanel_2",
        )
        self.assertEqual(
            activity.navigation_link(
                "futureexchange",
                {},
                {"tryGetService": "NewActivityService#loadAndInitNormalActivity#futureexchange#showExchange#4"},
            ),
            "btnNewAct_futureexchange_showExchange_4",
        )

    def test_static_string_constant_concatenation_and_nested_quota_are_supported(self):
        config = ('public class FutureSaleConfig {'
                  'public static const RESHOW_SELF_ACT_KEY:String="btnNewAct_futuresale_showMainPanel_3";'
                  'private static const PETS:String="8001#8002";'
                  'private static const REWARDS:Array=['
                  '{"index":0,"basicDescription":"三颗红星",'
                  '"simpleParams":"CommonEnhancePrize,-1,1750,39$3,"+PETS,'
                  '"price":109,"limit":1,"buyId":3,"gainId":4,"addGainTimesNum":3}];}')
        model = ('class FutureSaleModel { function parseData(param1:Object):void {'
                 'var reward:Object; var state:Object; var used:int;'
                 'state=param1["b"+reward.buyId]; used=int(state["by"+reward.index]); }}')
        client = ('class FutureSaleClient extends ClientSA { function FutureSaleClient(){super(5755);}'
                  'function getInfo(callback:Function):void{request(ClientSA.CMD_GET_INFO,callback,null,true);}}')
        view = 'ActUtil.getMyDiamond();'
        shops, pending, _ = activity.parse_module(
            {"FutureSaleConfig.as": config, "FutureSaleModel.as": model,
             "FutureSaleClient.as": client, "FutureSaleView.as": view},
            "module", {"navigationLink": "btnNewAct_futuresale_showMainPanel"})
        self.assertEqual(pending, [])
        good = shops[0]["goods"][0]
        self.assertEqual((good["enhanceType"], good["raceIds"]), ("39$3", [8001, 8002]))
        self.assertEqual((good["cost"], good["exchangeKind"]), ("8:2:109", "diamond"))
        self.assertEqual((good["limitCount"], good["quotaObservation"]["path"]),
                         (1, ["b3", "by0"]))
        self.assertEqual(shops[0]["navigationLink"], "btnNewAct_futuresale_showMainPanel_3")
        self.assertEqual(shops[0]["source"]["navigationLinkSource"], "module")

    def test_parameterized_stars_preserve_parameters(self):
        self.assertEqual(activity.enhancement("CommonEnhancePrize,-1,1,33$8-39$1,7529")[0], "33$8-39$1")
        self.assertEqual(activity.enhancement("CommonEnhancePrize,-1,1,39$3,7529")[0], "39$3")
        self.assertEqual(activity.enhancement("CommonEnhancePrize,-1,1,89$1,7529")[0], "89$1")
        with self.assertRaises(ValueError):
            activity.enhancement("CommonEnhancePrize,-1,1,39$2,7529")

    def test_currency_threshold_reward_is_not_misclassified_as_exchange(self):
        rows = [{"index": 1, "lv": 1, "daibi": 1000, "simpleParams": "CommonEnhancePrize,-1,1,83,7529"}]
        source = script(rows) + 'private static const CMD_GAIN_PROGRESS_PRIZE:String="1039_1_3";'
        shops, pending, _ = activity.parse_module({"FutureExchangeConfig.as": source}, "module", {})
        self.assertEqual((shops, pending), ([], []))

    def test_hidden_conditional_exchange_is_kept_with_unknown_unlock(self):
        text = ('public class FreshSaleConfig { public static const RACE_ID:int=8001; '
                'public static const LIMIT_RACEIDS:Array=[8001,8002]; public static const PRIZES:Array=['
                '{"index":-1,"bi":5,"baseOnId":4,"type":"Strengthen","params":"95","prices":[79,29],"filter":FreshSaleConfig.LIMIT_RACEIDS}];}')
        consumer = 'v="CommonEnhancePrize,-1,1,"+row["params"]+","+FreshSaleConfig.RACE_ID; selectPets(row["filter"]);'
        shops, pending, _ = activity.parse_module({"FreshSaleConfig.as": text, "View.as": consumer}, "module", {})
        good = shops[0]["goods"][0]
        self.assertEqual(good["itemServerId"], 5)
        self.assertEqual(good["enhanceType"], "95")
        self.assertIn("4", good["unlock"])
        self.assertFalse(good["costKnown"])
        self.assertEqual(pending, [])

    def test_emergency_addition_and_official_temporary_block_are_applied(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            discovery = activity.build_discovery(ET.fromstring(updater.registry), 'const DATA:Object={"hud":[]};')
            def resource(name, versions):
                target = updater.scratch / ("extra-" + name.split('/')[0] + ".swf")
                xml = '<config><activity><a name="emergencynew" file="newactivityext/newact20260911/emergencynew/emergencynew"/></activity></config>'
                target.write_bytes(swf(xml=xml))
                return target, {"version": versions[name]}
            def export(path, target, cls):
                output = updater.scratch / "block.as"
                output.write_text('const DATA:Object={"futureexchange":{"isBlocked":true}};', encoding="utf-8")
                return output
            updater.resource, updater.export = resource, export
            activity.apply_discovery_overrides(updater, {activity.EMERGENCY_RESOURCE: "2026091100000000", activity.BLOCK_RESOURCE: "2026091100000000"}, discovery)
            self.assertIsNone(activity.resolve_alias("futureexchange", discovery))
            self.assertIsNotNone(activity.resolve_alias("emergencynew", discovery))
            self.assertIn("emergencynew", discovery["roots"])

    def test_official_registry_and_incremental_negative_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            versions = {activity.CONFIG_RESOURCE: "2026091100000000", updater.module: "2026091100000001", updater.empty: "2026091100000002"}
            with patch.dict(sys.modules, {"activity_evolution_selector": type("Selector", (), {"dependency_revision": staticmethod(lambda versions: "same")})}):
                self.assertTrue(activity.update_activity_exchanges(updater, versions))
                first = list(updater.calls)
                self.assertEqual(len(first), 3)
                self.assertFalse(any("oldarchive" in call for call in first))
                updater.calls.clear()
                self.assertFalse(activity.update_activity_exchanges(updater, versions))
                self.assertEqual(updater.calls, [])
                versions[updater.module] = "2026091100000003"
                updater.rows[0]["cost"] = "4:3266:600"
                self.assertTrue(activity.update_activity_exchanges(updater, versions))
                self.assertEqual(len(updater.calls), 1)
                updater.calls.clear()
                updater.fail = True
                versions[updater.module] = "2026091100000004"
                self.assertTrue(activity.update_activity_exchanges(updater, versions))
                data = json.loads((updater.root / "catalog" / activity.FILENAME).read_text(encoding="utf-8"))
                self.assertEqual(data["shops"][0]["goods"][0]["cost"], "4:3266:600")
                self.assertEqual(len(updater.activity_exchange_failures), 1)

    def test_new_official_activity_is_discovered_without_name_whitelist(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            versions = {activity.CONFIG_RESOURCE: "2026091100000000", updater.module: "2026091100000001", updater.empty: "2026091100000002"}
            with patch.dict(sys.modules, {"activity_evolution_selector": type("Selector", (), {"dependency_revision": staticmethod(lambda versions: "same")})}):
                activity.update_activity_exchanges(updater, versions)
                new_module = "newactivityext/newact20260918/futureexchange2/futureexchange2"
                updater.registry = updater.registry.replace('<week version="20260911">',
                    '<week version="20260918"><a name="futureexchange2" desc="下周新活动" file="' + new_module + '"/></week><week version="20260911">')
                versions[activity.CONFIG_RESOURCE] = "2026091800000000"
                versions[new_module] = "2026091800000001"
                updater.calls.clear()
                self.assertTrue(activity.update_activity_exchanges(updater, versions))
                data = json.loads((updater.root / "catalog" / activity.FILENAME).read_text(encoding="utf-8"))
                self.assertEqual(data["coverage"]["shops"], 2)
                self.assertEqual(len(updater.calls), 2)  # registry + just the new module
                new_shop = next(shop for shop in data["shops"] if shop["sourceKey"].startswith(new_module + "#"))
                self.assertEqual(new_shop["navigationLink"], "")

    def test_shop_retains_official_navigation_and_discovery_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            versions = {activity.CONFIG_RESOURCE: "2026091100000000", updater.module: "2026091100000001", updater.empty: "2026091100000002"}
            with patch.dict(sys.modules, {"activity_evolution_selector": type("Selector", (), {"dependency_revision": staticmethod(lambda versions: "same")})}):
                activity.update_activity_exchanges(updater, versions)
            data = json.loads((updater.root / "catalog" / activity.FILENAME).read_text(encoding="utf-8"))
            shop = next(shop for shop in data["shops"] if shop["sourceKey"].startswith(updater.module + "#"))
            self.assertEqual(shop["navigationLink"], "btnNewAct_futureexchange_showMainPanel")
            self.assertEqual(shop["activityEvidence"], "hud")
            self.assertEqual(shop["activityEvidenceDate"], "20260911")

    def test_unknown_new_rule_preserves_previous_row_as_stale(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            versions = {activity.CONFIG_RESOURCE: "2026091100000000", updater.module: "2026091100000001", updater.empty: "2026091100000002"}
            with patch.dict(sys.modules, {"activity_evolution_selector": type("Selector", (), {"dependency_revision": staticmethod(lambda versions: "same")})}):
                activity.update_activity_exchanges(updater, versions)
                versions[updater.module] = "2026091100000003"
                updater.rows[0]["simpleParams"] = "CommonEnhancePrize,-1,123,39$2,8001#8002"
                activity.update_activity_exchanges(updater, versions)
                data = json.loads((updater.root / "catalog" / activity.FILENAME).read_text(encoding="utf-8"))
                good = data["shops"][0]["goods"][0]
                self.assertEqual(good["enhanceType"], "92")
                self.assertTrue(good["catalogStale"])
                self.assertFalse(good["costKnown"])
                self.assertFalse(good["availableKnown"])
                self.assertTrue(good["unlock"])
                self.assertEqual(len(data["pending"]), 1)

    def test_matching_bundled_baseline_seeds_disk_without_module_downloads(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            versions = {activity.CONFIG_RESOURCE: "2026091100000000", updater.module: "2026091100000001", updater.empty: "2026091100000002"}
            with patch.dict(sys.modules, {"activity_evolution_selector": type("Selector", (), {"dependency_revision": staticmethod(lambda versions: "same")})}):
                activity.update_activity_exchanges(updater, versions)
                cache = updater.root / "catalog" / activity.FILENAME
                bundled_bytes = cache.read_bytes()
                updater.baseline.mkdir()
                (updater.baseline / activity.FILENAME).write_bytes(bundled_bytes)
                cache.unlink()  # Only this test's own temporary cache.
                updater.calls.clear()
                self.assertFalse(activity.update_activity_exchanges(updater, versions))
                self.assertEqual(updater.calls, [])
                self.assertEqual(cache.read_bytes(), bundled_bytes)

    def test_incompatible_baseline_is_not_treated_as_current(self):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            updater.baseline.mkdir()
            (updater.baseline / activity.FILENAME).write_text(json.dumps({"schema": 1, "parserVersion": activity.PARSER_VERSION - 1,
                "source": {}, "modules": {}, "shops": [], "pending": []}), encoding="utf-8")
            versions = {activity.CONFIG_RESOURCE: "2026091100000000", updater.module: "2026091100000001", updater.empty: "2026091100000002"}
            with patch.dict(sys.modules, {"activity_evolution_selector": type("Selector", (), {"dependency_revision": staticmethod(lambda versions: "same")})}):
                self.assertTrue(activity.update_activity_exchanges(updater, versions))
                self.assertEqual(len(updater.calls), 3)

    def test_sef_style_infers_read_shop_id_quota_bundle_and_missing_zero(self):
        rows = [{"serverId": 38, "cost": "4:3266:500", "limit": "4:1", "simpleParams": "CommonEnhancePrize,-1,1,92,8001"}]
        scripts = {"FutureExchangeConfig.as": script(rows), "FutureClient.as":
            'class FutureClient { static const CMD_INFO:String="1008_20260313_es_2";'
            'public function info(param1:int,param2:Function):void {ClientXT.sendXtMessage(CMD_INFO,param2,{"i":param1});}}',
            "FutureReward.as": 'createLimitedByBundle(param1["limit"]); if(found){frag.setData(value);}else{frag.setData(0);}'}
        shops, pending, _ = activity.parse_module(scripts, "new-module", {})
        query = shops[0]["observation"]["requests"][0]
        good = shops[0]["goods"][0]
        self.assertEqual(query["params"], {"i": 7})
        self.assertEqual((good["limitCount"], good["limitLabel"], good["limitKey"]), (1, "总", "tl"))
        self.assertEqual(good["quotaObservation"]["path"], ["bi38", "tl"])
        self.assertEqual(good["quotaObservation"]["missingValue"], 0)
        self.assertEqual(pending, [])

    def test_event_currency_and_counter_use_data_index_without_faking_item_id(self):
        rows = [{"serverId": 99, "dataIndex": 5, "daibi": 30, "limit": 1, "simpleParams": "CommonEnhancePrize,-1,1,92,8001"}]
        scripts = {"FutureExchangeConfig.as": script(rows, extra='static const DaibiName:String="V币";'),
            "FreshClient.as": 'class FreshClient {static const CMD_GET_INFO:String="1008_20251231_cvgv2_0";static const CMD_EXCHANGE:String="1008_20251231_cvgv2_1";'
            'public function getInfo(param1:Function):void {ClientXT.sendXtMessage(CMD_GET_INFO,param1,null);}}',
            "FreshModel.as": 'this._counts = param1["li"];this._coins=param1["c"];'}
        shops, pending, _ = activity.parse_module(scripts, "new-module", {})
        good = shops[0]["goods"][0]
        self.assertEqual(good["quotaObservation"]["path"], ["li", "5"])
        self.assertEqual(good["activityCosts"], [{"name": "V币", "count": 30, "requestKey": "state", "path": ["c"]}])
        self.assertEqual(good["cost"], "")
        self.assertTrue(good["costKnown"])
        self.assertIsNone(shops[0]["observation"]["requests"][0]["params"])
        self.assertEqual(good["limitCount"], 1)
        self.assertEqual(pending, [])

    def test_fixed_sale_price_and_hidden_branch_share_base_limit(self):
        rows = [
            {"index": 4, "bi": 4, "type": "Strengthen", "params": "92", "filter": [8001], "prices": [149, 103], "limit": 6},
            {"index": -1, "bi": 5, "baseOnId": 4, "type": "Strengthen", "params": "95", "filter": [8001], "prices": [79, 29], "limit": 0}]
        scripts = {"FutureExchangeConfig.as": script(rows, extra='static const RACE_ID:int=8001;'),
            "NewClient.as": 'class NewClient extends ClientSA {static const AI:int=6001; public function NewClient(){super(AI);}'
            'public function getInfo(param1:Function):void {request(ClientSA.CMD_GET_INFO,param1,null);}}',
            "NewModel.as": 'private function parseBought(param1:Object,param2:int):int {'
            'var x:Object=param1["b"+param2];if(x==null||x["b"+param2]==null){return 0;}return x["b"+param2];}',
            "NewView.as": 's="CommonEnhancePrize,-1,1,"+row["params"]+","+FutureExchangeConfig.RACE_ID;choose(row["filter"]);'
            'price = int(info["prices"][1]); if(ActUtil.getMyDiamond()<price){return;}'}
        shops, pending, _ = activity.parse_module(scripts, "new-module", {})
        ordinary, special = shops[0]["goods"]
        self.assertEqual(ordinary["cost"], "8:2:103")
        self.assertEqual(special["cost"], "8:2:29")
        self.assertTrue(special["costKnown"])
        self.assertEqual(special["limitCount"], 6)
        self.assertEqual(special["quotaObservation"]["path"], ["b4", "b4"])
        self.assertEqual(special["quotaSharedWith"], 4)
        self.assertEqual((ordinary["exchangeKind"], special["exchangeKind"]), ("diamond", "diamond"))
        self.assertNotIn("priceOptions", special)
        self.assertEqual(pending, [])

    def test_dynamic_price_uses_total_purchases_and_keeps_three_tiers(self):
        rows = [{"index": 1, "price": [47, 41, 38], "limit": 7, "simpleParams": "CommonEnhancePrize,-1,1,39$1,8001"}]
        scripts = {"FutureExchangeConfig.as": script(rows, extra='static const SimpleActId:int=6002;static const TotalPriceNum:int=3;') +
            'public function getPriceIndex(param1:int):int {if(param1 < TotalPriceNum){return param1;}return TotalPriceNum - 1;}',
            "NewClient.as": 'class NewClient extends ClientSA {public function NewClient(){super(FutureExchangeConfig.SimpleActId);}'
            'public function getInfo(param1:Function):void {request(ClientSA.CMD_GET_INFO,param1,null);}}',
            "NewModel.as": 'this._total = param1["bt"]; part = param1["b"+reward.index];'
            'public function getTotalBuyTimes():int {return this._total;}',
            "NewReward.as": 'this._times=int(param1["b"+index]);',
            "NewView.as": 'getPriceIndex(model.getTotalBuyTimes());ActUtil.getMyDiamond();'}
        shops, pending, _ = activity.parse_module(scripts, "new-module", {})
        good = shops[0]["goods"][0]
        self.assertEqual(good["quotaObservation"]["path"], ["b1", "b1"])
        self.assertNotIn("missingValue", good["quotaObservation"])
        self.assertEqual([tier["when"]["op"] for tier in good["priceOptions"]], ["eq", "eq", "gte"])
        self.assertEqual(good["priceOptions"][2]["when"]["path"], ["bt"])
        self.assertFalse(good["costKnown"])
        self.assertEqual(pending, [])

    def test_nested_pet_limit_and_standard_cost_do_not_emit_request_with_fake_ci(self):
        rows = [{"id": 0, "cost": 140, "costD": 0, "simpleParams": "CommonEnhancePrize,-1,1,34,8001"}]
        scripts = {"FreshExchangeConfig.as": script(rows, cls="FreshExchangeConfig"),
            "FreshConfig.as": 'static const UNIVERSAL_ITEM_ID:int=3254;'
            'static const PETS:Array=[{"id":0,"race":8001,"exchangeIds":"0:1"}];',
            "FreshClient.as": 'class FreshClient extends ClientSA {static const CMD_GET_INFO:String="1008_20260508_cl_0";'
            'public function getInfo(param1:Function):void {request(CMD_GET_INFO,param1,{"ci":ActUtil.getNewChangeSetId()});}}',
            "FreshReward.as": 'values=params["ep"]; fragment("total_limited");',
            "FreshView.as": 'setCoinBox(view,FreshConfig.UNIVERSAL_ITEM_ID,this.reward.cost);'}
        shops, pending, _ = activity.parse_module(scripts, "new-module", {})
        good = shops[0]["goods"][0]
        self.assertEqual(good["cost"], "4:3254:140")
        self.assertEqual((good["limitCount"], good["limitLabel"]), (1, "总"))
        self.assertEqual(shops[0]["observation"]["requests"], [])
        self.assertIn("ci", shops[0]["observation"]["pendingRequest"]["parameterGenerator"])
        self.assertEqual(good["quotaObservationPending"]["path"], ["pl", {"find": "id", "equals": 0}, "ep"])
        self.assertEqual(pending, [])

    def test_activity_coin_encoding_and_level_gate_do_not_assume_missing_quota_zero(self):
        rows = [{"index": 0, "daibi": "1:80", "limit": "4:4", "lvFlag": "NewFutureGroup$2",
                 "simpleParams": "CommonEnhancePrize,-1,1,92,8001"}]
        scripts = {"FutureExchangeConfig.as": script(rows) + 'key="NewFutureGroup$"+_lv;',
            "NamesConfig.as": 'static const ObjDaibiNames:Object={"1":"新活动养成币"};',
            "NewClient.as": 'class NewClient {static const CMD_GET_INFO:String="1039_3_0";'
            'static const CMD_GET_INFO_Exchange:String="1008_20260313_es_2";static const CMD_EXCHANGE:String="1008_20260313_es_1";'
            'public function getInfo(param1:Function):void {ClientXT.sendXtMessage(CMD_GET_INFO,param1,null);}'
            'public function getInfoExchange(param1:Function,param2:int):void {ClientXT.sendXtMessage(CMD_GET_INFO_Exchange,param1,{"i":param2});}}',
            "NewModel.as": 'value = param1["cn"];'}
        shops, pending, _ = activity.parse_module(scripts, "future-module", {})
        good = shops[0]["goods"][0]
        self.assertNotIn("missingValue", good["quotaObservation"])
        self.assertEqual(good["observationWhen"], {"requestKey":"eligibility", "path":["lv"], "op":"eq", "value":2})
        self.assertEqual(good["activityCosts"][0]["encoding"], "id-counts")
        self.assertEqual(good["activityCosts"][0]["name"], "新活动养成币")
        self.assertEqual(good["activityCosts"][0]["count"], 80)
        self.assertEqual(pending, [])


class RewardStructures(unittest.TestCase):
    def parse(self, scripts, **kwargs):
        return activity.parse_module(scripts, 'unseen/next-update/event', {'activityName': '未来活动'},
                                     include_manual=True, **kwargs)

    def test_display_policy_keeps_verified_cultivation_alternative_as_manual(self):
        fixed = {'itemServerId':1,'description':'指定精灵提升','enhanceType':'92','raceIds':[8101],
                 'rewardRaw':'CommonEnhancePrize,-1,1,92,8101'}
        cultivation = {**fixed,'rewardRaw':'Choice,|' + fixed['rewardRaw'] + '|Material,4:1359:38,1'}
        alternatives = [
            {**cultivation, 'raceIds':[8102]},
            {**cultivation, 'enhanceType':'62'},
            {**cultivation, 'rewardRaw':cultivation['rewardRaw'].replace('4:1359:38','139:11:1')},
            {**cultivation, 'rewardRaw':cultivation['rewardRaw'] + '|PetOpen,8101'},
            {**cultivation, 'rewardRaw':cultivation['rewardRaw'].replace('92,8101','92,8101:true')},
            {**fixed,'rewardRaw':'SelectPrizes,whatever'},
            {**fixed,'rewardRaw':'Material,139:11:1,1','selectablePackageIds':[11]},
            {**fixed,'description':'任选3颗红星'},
            {**fixed,'rewardOptions':[{'id':1},{'id':2}]}]
        shops = activity.relevant_shops([{'sourceKey':'future/module#rewards','goods':[fixed,cultivation,*alternatives]}])
        self.assertEqual(shops[0]['goods'],[fixed,{**cultivation,'manualSelectionRequired':True}])

    def test_generic_and_bundle_titles_show_each_real_effect_without_changing_ids(self):
        support = {'descriptions':{'11':'满级','62':'完美天赋','86':'满星迹','89':'星迹突破','94':'源兽神觉升至满阶'}}
        rows = [{'id':1,'desc':'通用养成','rewardType':'Strengthen','rewardParams':'86-89','filter':[8101]},
                {'id':2,'desc':'完美极品','rewardType':'Strengthen','rewardParams':'11-62','filter':[8101]},
                {'id':3,'desc':'指定养成','rewardType':'Strengthen','rewardParams':'94','filter':None}]
        shops, pending, _ = self.parse({'Future.as':script(rows,'Future')},reward_support=support)
        self.assertEqual(pending,[])
        self.assertEqual([g['description'] for g in shops[0]['goods']],[
            '指定精灵：满星迹 / 星迹突破','完美极品（满级 / 完美天赋）','指定精灵：源兽神觉升至满阶'])
        self.assertEqual([g['itemServerId'] for g in shops[0]['goods']],[1,2,3])
        self.assertEqual(shops[0]['goods'][2]['raceIds'],[])

    def test_pass_cultivation_names_decode_effect_and_keep_choice_identity(self):
        from activity_reward_structures import description_parameters
        definitions = {'GIFTED_PERFECT': {'id':62}, 'SACREDEQUIP_STAGE_GIVEN_MORE': {'id':92}}
        formatter = '''case StrengthenComboBasicType.SACREDEQUIP_STAR_GIVEN:
          case StrengthenComboBasicType.SACREDEQUIP_STAGE_GIVEN_MORE:
            return text.replace("#level#",item.getParamsInInt(kind,0,1));'''
        support = {'descriptions':{'62':'完美天赋','92':'源兽神觉提升#level#阶'},
                   'descriptionParameters':description_parameters(formatter, definitions)}
        rows = [{'index':30,'prizeNormal':'!Choice,|CommonEnhancePrize,-1,1,62,8101|Material,4:1248:1,1'},
                {'index':50,'prizeNormal':'!Choice,|CommonEnhancePrize,-1,1,92,8101|Material,4:1359:38,1'}]
        scripts = {'NextPass.as': script(rows, 'NextPass')}
        before, _, _ = self.parse(scripts)
        after, pending, _ = self.parse(scripts, reward_support=support)
        self.assertEqual(pending, [])
        goods = after[0]['goods']
        self.assertEqual([g['description'] for g in goods], [
            '指定精灵：完美天赋（与材料奖励二选一） · 第 30 项',
            '指定精灵：源兽神觉提升1阶（与材料奖励二选一） · 第 50 项'])
        self.assertEqual([(g['itemServerId'],g['rewardRaw']) for g in before[0]['goods']],
                         [(g['itemServerId'],g['rewardRaw']) for g in goods])

    def test_cultivation_description_uses_official_parameter_defaults(self):
        from activity_reward_structures import enhancement_description
        support = {'descriptions':{'11':'满级','62':'完美天赋','92':'源兽神觉提升#level#阶'},
                   'descriptionParameters':{'92':{'index':0,'default':1}}}
        self.assertEqual(enhancement_description('11-62-92$3',support),'满级 / 完美天赋 / 源兽神觉提升3阶')
        self.assertEqual(enhancement_description('92',support),'源兽神觉提升1阶')
        support['descriptionParameters'] = {}
        self.assertEqual(enhancement_description('92',support),'')

    def test_typed_future_rewards_use_scope_not_names_and_keep_lottery_manual(self):
        rows = [
            {'serverIndex': 1, 'rewardType': 'Strengthen', 'rewardParams': '92$1?4:55:8', 'filter': '8001#8002', 'cost': 120},
            {'serverIndex': 2, 'rewardType': 'Strengthen', 'rewardParams': '33', 'filter': ['8001'], 'probability': 0.02},
            {'serverIndex': 3, 'rewardType': 'NChoose1', 'rewardParams': '24:1:1;24:2:1', 'probability': 0.01},
            {'serverIndex': 4, 'rewardType': 'Material', 'rewardParams': '4:55:9', 'basicDescription': '任选精灵宣传'},
        ]
        shops, pending, _ = self.parse({'Next.as': script(rows, 'Next')})
        self.assertEqual(pending, [])
        goods = shops[0]['goods']
        self.assertEqual([g['itemServerId'] for g in goods], [1, 2, 3])
        self.assertEqual((goods[0]['enhanceType'], goods[0]['raceIds']), ('92$1', [8001,8002]))
        self.assertFalse(goods[0]['manualSelectionRequired'])
        self.assertTrue(goods[1]['manualSelectionRequired'] and goods[2]['manualSelectionRequired'])
        self.assertEqual(goods[1]['acquisitionKind'], 'lottery')
        self.assertFalse(goods[0]['costKnown'])  # 120 has no proven currency.
        self.assertEqual(shops[0]['observation']['requests'], [])

    def test_exclusion_or_dynamic_scope_is_visible_without_all_pet_rule(self):
        for scope in ('-8001', '8001:1', {'$ref':'runtimeFilter'}, None):
            with self.subTest(scope=scope):
                rows = [{'serverIndex': 1, 'cost': 20, 'rewardType':'Strengthen', 'rewardParams':'34', 'filter':scope}]
                shops, pending, _ = self.parse({'Future.as': script(rows, 'Future')})
                good, = shops[0]['goods']
                self.assertEqual((good['enhanceType'],good['raceIds']), ('',[]))
                self.assertTrue(good['manualSelectionRequired'])
                self.assertTrue(activity.targeted_reward(good['rewardRaw']))
                self.assertEqual(activity.relevant_shops(shops), shops)

    def test_wildcard_vector_and_indexed_copy_function_resolve_static_data(self):
        config = '''static const PETS:*=[[8101,8102]];
          static function pets(i:int):Array { return (PETS[i] as Array).concat(); }
          static const REWARDS:*= [{"serverIndex":0,"cost":40,"rewardType":"Strengthen",
            "rewardParams":"44","filter":pets(0)}];'''
        shops, pending, _ = self.parse({'UnknownConfig.as': config})
        self.assertEqual(pending, [])
        self.assertEqual(shops[0]['goods'][0]['raceIds'], [8101,8102])
        rows = 'static const PRIZES:Vector.<Entry> = Vector.<Entry>([new Entry(1,"92",[8201])]);'
        wrapper = '''class Entry { function Entry(param1:int,param2:String,param3:Array) {
          this.serverIndex=param1;this.rewardParams=param2;this.filter=param3;}
        }'''
        tables, _ = activity.parse_tables({'New.as':rows, 'Entry.as':wrapper})
        self.assertEqual(tables['New.PRIZES'][1][0]['filter'], [8201])

    def test_default_scope_follows_actual_consistent_config_binding(self):
        s = '''static const SCOPE:Array=[8101]; static const DECOY:Array=[9999];
          static const REWARDS:Array=[{"serverIndex":0,"rewardType":"Strengthen","rewardParams":"44","discount":60}];'''
        use = 'data.setConsistentConfig({"filter":Future.SCOPE}); data.init(Future.REWARDS,response,Reward);'
        shops, pending, _ = self.parse({'Future.as':s, 'Consumer.as':use})
        self.assertEqual(pending, [])
        self.assertEqual(shops[0]['goods'][0]['raceIds'], [8101])
        shops, _, _ = self.parse({'Future.as':s})
        self.assertEqual(shops[0]['goods'][0]['raceIds'], [])

    def test_partial_dynamic_rows_do_not_hide_valid_siblings_or_unknown_types(self):
        s = '''static const ROWS:Array=[
          {"serverIndex":0,"rewardType":"Strengthen","rewardParams":"44","filter":[8101]},
          {"serverIndex":1,"rewardType":"Strengthen","rewardParams":compute(),"filter":[8101]},
          {"serverIndex":2,"rewardType":"FutureChoiceVersion","rewardParams":"new"}];'''
        shops, pending, _ = self.parse({'Future.as':s})
        self.assertEqual(len(shops[0]['goods']), 1)
        self.assertTrue(any('强化奖励参数' in p['reason'] for p in pending))
        self.assertTrue(any('FutureChoiceVersion' in p['reason'] for p in pending))

    def test_pass_joins_only_live_placeholder_keys_and_preserves_selection_on_reprice(self):
        s = '''static const LEVELS:Array=[{"level":1,"idE":200,"prizeExtreme":"!","price":10}];
          static const EXTENDED_PRIZE_MAP:Object={
          "extreme_1":{"name":"红星任选","type":"starGodSelection","defines":[{"id":65},{"id":66}]},
          "extreme_999":{"name":"旧宣传","type":"starGodSelection","defines":[{"id":67}]}};'''
        consumer = 'function key():String {return (this.extreme ? "extreme_" : "normal_") + this.active.level;}'
        shops, pending, _ = self.parse({'Future.as':s,'Row.as':consumer})
        self.assertEqual(pending, [])
        goods = [g for shop in shops for g in shop['goods']]
        self.assertEqual(len(goods), 1)
        self.assertEqual(len(goods[0]['rewardOptions']), 2)
        changed, _, _ = self.parse({'Future.as':s.replace('"price":10','"price":20').replace('红星任选','新名称'),'Row.as':consumer})
        self.assertEqual(changed[0]['goods'][0]['itemServerId'], goods[0]['itemServerId'])
        self.assertEqual(changed[0]['goods'][0]['rewardRaw'], goods[0]['rewardRaw'])

    def test_progress_keeps_stage_identity_and_free_claims_have_no_invented_cost(self):
        rows = [{'campaignId':stage,'needToken':10,'rewardIds':'92#4:55:9','rewardTypes':'强化#道具',
                 'rewardNames':'源兽升阶#普通材料','putRaceIds':'8101','name':'奖励'} for stage in (1,2)]
        consumer = 'if(param1.indexOf("强化") != -1) { return "Strengthen"; }'
        shops, pending, _ = self.parse({'Future.as':script(rows,'Future'),'Consumer.as':consumer})
        self.assertEqual(pending, [])
        self.assertEqual(len(shops), 1)
        goods = shops[0]['goods']
        self.assertEqual(len({g['itemServerId'] for g in goods}), 2)
        self.assertTrue(all(g['acquisitionKind']=='progress' and not g['costKnown'] and g['manualSelectionRequired'] for g in goods))

    def test_shared_panel_uses_only_requested_pet_and_omits_testing_groups(self):
        support = {'hiddenDisplayId':999, 'pets':{'8101':{'testOnly':'0','priorContains':'1#2-999'}},
                   'prices':{'1':{'strengthenParams':'92'},'2':{'strengthenParams':'34'}},'descriptions':{'92':'源兽升阶'}}
        scripts = {'Next.as':'static const PET:Object={"raceId":8101}; function show():void {svc.showPanelCombo(Next.PET["raceId"].toString());}'}
        shops, pending, _ = self.parse(scripts,reward_support=support)
        self.assertEqual(pending, [])
        good, = shops[0]['goods']
        self.assertEqual((good['enhanceType'],good['raceIds']),('92',[8101]))
        self.assertTrue(good['manualSelectionRequired'])
        self.assertFalse(good['costKnown'])
        self.assertEqual(shops[0]['observation']['requests'], [])

    def test_future_activity_opening_gate_and_row_dates_survive_registry_dates(self):
        source = '''static const OPEN_TIME:String="20991009";
          static function isOpen():Boolean {return DateUtil.isAfterTimeWithDelayClose(OPEN_TIME);}
          static const ROWS:Array=[
            {"id":0,"rewardType":"Strengthen","rewardParams":"44","filter":[8101]},
            {"id":1,"rewardType":"Strengthen","rewardParams":"44","filter":[8101],"openTime":"20991010","endTime":"20991016"}];'''
        caller = 'if(!Future.isOpen()) {return;} showPanel();'
        shops, pending, _ = self.parse({'Future.as':source,'Entry.as':caller})
        self.assertEqual(pending, [])
        goods = shops[0]['goods']
        self.assertEqual([g['officialShelfTime'] for g in goods], ['20991009 02:00:00','20991010'])
        self.assertEqual(goods[0]['startsAt'], '2099-10-09T02:00:00+08:00')
        self.assertEqual([g['source']['shelfTimeSource'] for g in goods], ['module','row'])
        self.assertEqual(goods[1]['source']['removalTimeSource'], 'row')
        without_caller, _, _ = self.parse({'Future.as':source})
        self.assertEqual(without_caller[0]['goods'][0]['officialShelfTime'], '')

    def test_activity_deadline_requires_consumed_gate_and_caps_row_dates(self):
        from activity_reward_structures import module_deadline, apply_activity_period
        source = '''static const ActEndDate:String="2026-11-06";
          static const SALE_END_TIME:String="20260918";
          static function isActEnd():Boolean { return DateUtil.getServerTimeInMS() > DateUtil.parse(ActEndDate,true).getTime(); }'''
        constants = {'Future.ActEndDate':'2026-11-06', 'Future.SALE_END_TIME':'20260918'}
        self.assertEqual(module_deadline({'Future.as':source}, constants), '')
        scripts = {'Future.as':source, 'Panel.as':'setVisible("spOver", Future.isActEnd());'}
        end = module_deadline(scripts, constants)
        self.assertEqual(end, '2026-11-06 02:00:00')
        good = {'source':{'periodBounds': {'rowStart':'20261001', 'rowEnd':'20261201', 'moduleEnd':end}}}
        apply_activity_period(good, {'startTime':'20261009','endTime':'20261130'})
        self.assertEqual((good['startsAt'],good['endsAt']), ('2026-10-09T00:00:00+08:00','2026-11-06T02:00:00+08:00'))
        apply_activity_period(good, {'startTime':'20261009','endTime':'20261020 06:00:00'})
        self.assertEqual(good['endsAt'], '2026-10-20T06:00:00+08:00')
        apply_activity_period(good, {'startTime':'20261209','endTime':'20261020'})
        self.assertTrue(good['availabilityClosed'])

    def test_date_only_and_midnight_expiry_keep_correct_last_day(self):
        from activity_reward_structures import apply_activity_period
        good = {'source':{'periodBounds': {}}}
        apply_activity_period(good, {'endTime':'20261009'})
        self.assertEqual((good['removalTime'],good['endsAt']), ('20261009','2026-10-10T00:00:00+08:00'))
        apply_activity_period(good, {'endTime':'20261009 00:00:00'})
        self.assertEqual((good['removalTime'],good['endsAt']), ('20261008','2026-10-09T00:00:00+08:00'))

    @patch('activity_evolution_selector.dependency_revision', return_value='fixture')
    def test_reward_type_prefilter_and_partial_status_survive_incremental_refresh(self, _revision):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            updater.rows = [{'id':0,'rewardType':'Strengthen','rewardParams':'44','filter':[8101]},
                            {'id':1,'rewardType':'FutureChoice','rewardParams':'new'}]
            original = updater.fetch
            def fetch(url, path):
                source = original(url,path)
                if 'futureexchange' in url: path.write_bytes(swf(strings=['rewardType','Strengthen']))
                return source
            updater.fetch = fetch
            versions = {activity.CONFIG_RESOURCE:'2026100700000000',updater.module:'2026100700000000',updater.empty:'2026100700000000'}
            activity.update_activity_exchanges(updater,versions)
            result = json.loads((updater.root/'catalog'/activity.FILENAME).read_text(encoding='utf-8'))
            self.assertEqual(result['modules'][updater.module]['status'],'partial')
            self.assertEqual(result['coverage']['partialModules'],1)
            before = len(updater.calls)
            self.assertFalse(activity.update_activity_exchanges(updater,versions))
            self.assertEqual(len(updater.calls),before)

    @patch('activity_evolution_selector.dependency_revision', return_value='fixture')
    @patch('public_activity_exchange_updater.load_reward_support', return_value={})
    def test_shared_dependency_change_reparses_unchanged_activity(self, _support, _revision):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            updater.rows = [{'id':0,'rewardType':'Strengthen','rewardParams':'44','filter':[8101]}]
            versions = {activity.CONFIG_RESOURCE:'2026100700000000',updater.module:'2026100700000000',
                        updater.empty:'2026100700000000','library/interfaces':'2026100700000000',
                        'strengthencombo/strengthencomboservice':'2026100700000000'}
            activity.update_activity_exchanges(updater,versions)
            for dependency in ('library/interfaces','strengthencombo/strengthencomboservice'):
                before = len(updater.calls)
                versions[dependency] = '2026100800000000'
                self.assertTrue(activity.update_activity_exchanges(updater,versions))
                self.assertTrue(any('/futureexchange/' in call for call in updater.calls[before:]))
                before = len(updater.calls)
                self.assertFalse(activity.update_activity_exchanges(updater,versions))
                self.assertEqual(len(updater.calls),before)

    @patch('activity_evolution_selector.dependency_revision', return_value='fixture')
    def test_discovery_follows_deep_links_and_terminates_cycles(self, _revision):
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            modules = {f'chain{i}':f'newactivityext/newact20200101/chain{i}/chain{i}' for i in range(6)}
            updater.registry = ('<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="./newactivityconfig.xsd">'
                '<week version="20261007"><a name="chain0" file="'+modules['chain0']+'"/></week><week version="20200101">' +
                ''.join('<a name="'+name+'" file="'+module+'"/>' for name,module in list(modules.items())[1:]) + '</week></config>')
            def fetch(url,path):
                index = next(i for i in range(6) if f'/chain{i}/' in url)
                path.write_bytes(swf(strings=[f'chain{(index+1)%6}','rewardType','Strengthen']))
                return {'url':url}
            updater.fetch = fetch
            updater.rows = [{'id':0,'rewardType':'Strengthen','rewardParams':'44','filter':[8101]}]
            versions = {activity.CONFIG_RESOURCE:'2026100700000000',**{m:'2026100700000000' for m in modules.values()}}
            activity.update_activity_exchanges(updater,versions)
            result = json.loads((updater.root/'catalog'/activity.FILENAME).read_text(encoding='utf-8'))
            self.assertEqual(result['coverage']['activityModules'],6)
            self.assertEqual(result['coverage']['goods'],6)
            self.assertEqual(result['pending'],[])


class ActivityPeriods(unittest.TestCase):
    def test_container_period_and_overview_follow_only_the_bound_full_panel(self):
        from activity_periods import period_metadata, apply_graph_periods
        child = 'newactivityext/newact20261001/futurechild/futurechild'
        unrelated = 'newactivityext/newact20261001/unrelated/unrelated'
        scripts = {'Config.as': '''public static const TABS:Array = [
            {"id":7,"actName":"futurechild@20261001","actParams":"showMainPanel","isOnline":"20261002-20261101"},
            {"id":8,"actName":"unrelated","actParams":"showTab_2"}];
            public static const ORDER:Array = [7,8];
            public static const DATES:Array = [{"timeRange":"26.10.1-26.10.15","jumpStr":"btnNewAct_futureframe_showMainPanel_7"}];''',
            'Frame.as': 'new TabFrame().setTabArray(Config.TABS).setTabSeq(Config.ORDER).init(); Config.DATES.forEach(enum__);',
            'Prize.as': 'this._timeRange = param1["timeRange"];',
            'Util.as': '''public static const DAY_TIME:Number = 24 * 60 * 60 * 1000;
            public static function getEndTime(param1:String):Number {
                return Util.parseDateToMs(Util.splitTimeRange(param1)[1]) + Util.DAY_TIME;
            } DateUtil.parseDate(day);'''}
        meta = period_metadata(scripts, {'activityAlias':'futureframe'})
        self.assertEqual(len(meta['tabs']), 2)
        self.assertEqual(len(meta['schedules']), 1)
        good = {'source':{'periodBounds':{}},'rewardRaw':'unchanged'}
        import copy
        other = copy.deepcopy(good)
        modules = {'frame':{'periodMetadata':meta}, child:{'shops':[{'goods':[good]}]}, unrelated:{'shops':[{'goods':[other]}]}}
        discovery = {'entries':{'futurechild':{'file':child},'unrelated':{'file':unrelated}}}
        apply_graph_periods(modules, discovery)
        self.assertEqual(good['startsAt'],'2026-10-02T02:00:00+08:00')
        self.assertEqual(good['endsAt'],'2026-10-16T00:00:00+08:00')
        self.assertEqual(other['endsAt'],'')
        # Same tab reused for another campaign is ambiguous, not a union of
        # reward validity. A repeated cached refresh must remove old evidence.
        meta['schedules'].append({**meta['schedules'][0], 'period':{'startTime':'20261016','endTime':'20261105'}})
        apply_graph_periods(modules, discovery)
        self.assertEqual(good['endsAt'],'2026-11-01T02:00:00+08:00')
        self.assertEqual(good['rewardRaw'],'unchanged')
        modules['frame']['status'] = 'failed'
        apply_graph_periods(modules, discovery)
        self.assertEqual(good['endsAt'],'2026-11-01T02:00:00+08:00')
        self.assertFalse(good['availableKnown'])
        self.assertTrue(any(p.get('stale') for p in good['source']['activityPeriodEvidence']))
        scripts['Frame.as'] = scripts['Frame.as'].replace('Config.DATES.forEach(enum__);','')
        self.assertEqual(period_metadata(scripts, {'activityAlias':'futureframe'})['schedules'], [])

    def test_alias_wrapper_deadline_survives_target_resolution(self):
        discovery = {'entries':{'wrapper':{'link':'btnNewAct_target_showMainPanel','endTime':'20261015'},
                               'target':{'file':'newactivityext/newact20260101/target/target'}}}
        _, metadata = activity.resolve_alias('wrapper', discovery)
        from activity_reward_structures import apply_activity_period
        good = {'source':{'periodBounds':{}}}
        apply_activity_period(good, metadata)
        self.assertEqual(good['removalTime'],'20261015')

    def test_main_panel_label_requires_a_live_reference_and_release_year(self):
        from activity_periods import fui_period
        import zipfile
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'ui.fui'
            def write(component):
                with zipfile.ZipFile(path,'w') as archive:
                    archive.writestr('package.xml','''<packageDescription><resources>
                    <component id="main" name="MainPanel" exported="true"/>
                    <image id="date" name="活动时间：8.28-11.5"/>
                    <image id="unused" name="活动时间：8.28-9.5"/>
                    </resources></packageDescription>''')
                    archive.writestr('main.xml',component)
            write('<component><displayList><image src="date"/></displayList></component>')
            result = fui_period(path,'20260828')
            self.assertEqual((result['startTime'],result['endTime']),('20260828','20261105'))
            self.assertEqual(fui_period(path,'20260101'),{})
            write('<component><displayList><image src="date" visible="false"/></displayList></component>')
            self.assertEqual(fui_period(path,'20260828'),{})

    def test_panel_resource_change_invalidates_unchanged_reward_module(self):
        from activity_periods import panel_periods_reusable, PERIOD_PARSER_VERSION
        record = {'panelPeriods':[{'key':'future/ui','version':'2026100101','release':'20261001','parserVersion':PERIOD_PARSER_VERSION}]}
        self.assertTrue(panel_periods_reusable(record, {'future/ui':'2026100101'}, {'releaseVersion':'20261001'}))
        self.assertFalse(panel_periods_reusable(record, {'future/ui':'2026100801'}, {'releaseVersion':'20261001'}))
        self.assertFalse(panel_periods_reusable(record, {'future/ui':'2026100101'}, {'releaseVersion':'20271001'}))
        record['panelPeriods'][0].pop('parserVersion')
        self.assertFalse(panel_periods_reusable(record, {'future/ui':'2026100101'}, {'releaseVersion':'20261001'}))

    def test_failed_panel_update_keeps_verified_expiry_and_retries(self):
        from activity_periods import load_panel_periods, apply_graph_periods, panel_periods_reusable
        with tempfile.TemporaryDirectory() as temporary:
            updater = FakeUpdater(Path(temporary))
            updater.fetch = lambda *args: (_ for _ in ()).throw(OSError('unavailable'))
            old = {'key':'future/panel','version':'2026100100','release':'20261001',
                   'period':{'endTime':'20261005'}}
            versions = {'future/panel':'2026100700'}
            with patch('activity_periods.panel_resources',return_value=['future/panel']):
                rows = load_panel_periods(updater,versions,{},'future',{'releaseVersion':'20261001'},{'panelPeriods':[old]})
            self.assertEqual(rows[0]['period'],old['period'])
            self.assertEqual(rows[0]['version'],old['version'])
            self.assertFalse(panel_periods_reusable({'panelPeriods':rows},versions,{'releaseVersion':'20261001'}))
            good = {'source':{'periodBounds':{}}}
            apply_graph_periods({'future':{'panelPeriods':rows,'shops':[{'goods':[good]}]}},{'entries':{}})
            self.assertEqual(good['endsAt'],'2026-10-06T00:00:00+08:00')
            self.assertFalse(good['availableKnown'])

    def test_overview_direct_route_does_not_require_a_container(self):
        from activity_periods import apply_graph_periods
        module = 'newactivityext/newact20261001/future/future'
        good = {'source':{'periodBounds':{}}}
        modules = {'overview':{'periodMetadata':{'schedules':[{'route':'btnNewAct_future_showMainPanel',
                   'period':{'startTime':'20261001','endTime':'20261015'},'table':'Future.DATES'}]}},
                   module:{'shops':[{'goods':[good]}]}}
        apply_graph_periods(modules, {'entries':{'future':{'file':module}}})
        self.assertEqual(good['endsAt'],'2026-10-16T00:00:00+08:00')

    def test_entry_time_and_consumed_getter_use_server_closing_time(self):
        from activity_periods import entry_opening, tab_range
        from activity_reward_structures import module_deadline
        scripts = {'Future.as': '''override public function initActivity(param1:Object=null):void {
          if(DateUtil.getServerTimeInMS() < DateUtil.parseDate("20261009",true).getTime()) { showTips(); return; }
          showMainPanel(); }'''}
        self.assertEqual(entry_opening(scripts,{'cls':'test.Future'},{}),'20261009 02:00:00')
        self.assertEqual(tab_range('20261009-20261106')['endTime'],'20261106 02:00:00')
        scripts = {'Config.as': '''public static function get isActivityOnline():Boolean {
            return !DateUtil.isAfterTimeWithDelayClose("20261106"); }
            public static function get isMallOnline():Boolean {
            return !DateUtil.isAfterTimeWithDelayClose("20261016"); }'''}
        self.assertEqual(module_deadline(scripts,{}),'')
        scripts['Panel.as'] = 'if(!Config.isActivityOnline){return;} if(Config.isMallOnline){showDiscount();}'
        self.assertEqual(module_deadline(scripts,{}),'20261106 02:00:00')


if __name__ == "__main__":
    unittest.main()
