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
        self.rows = [{"serverId": 0, "id": 1, "cost": "4:3266:500", "simpleParams": "Choice,|CommonEnhancePrize,-1,123,92,8001#8002|Material,4:1:1,123",
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

    def test_parameterized_stars_preserve_parameters(self):
        self.assertEqual(activity.enhancement("CommonEnhancePrize,-1,1,33$8-39$1,7529")[0], "33$8-39$1")
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
                self.assertTrue(any(shop["sourceKey"].startswith(new_module + "#") for shop in data["shops"]))

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


if __name__ == "__main__":
    unittest.main()
