"""Small offline regressions for designated-pet catalog extraction."""
import importlib.util
import json
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("shop_generator", ROOT / "tools/generate_shop_exchange_data.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


def reward(description, simple, removal="20260929"):
    return json.dumps({"id": 1, "serverId": 1, "tab": 1, "basicDescription": description,
                       "simpleParams": simple, "shelfTime": "20260828", "removalTime": removal,
                       "cost": "4:3237:1", "limit": "3:1"}, ensure_ascii=False)


class DesignatedPetCatalog(unittest.TestCase):
    def test_removed_activities_stay_removed_without_hiding_permanent_shop(self):
        good = {'enhanceType':'92', 'raceIds':[9001], 'rewardRaw':'CommonEnhancePrize,-1,1,92,9001'}
        shops = [{'sourceKey':'', 'name':'永恒战场商店', 'goods':[good]}]
        for alias in generator.EXCLUDED_ACTIVITIES:
            shops.append({'sourceKey':f'newactivityext/newact20260911/{alias}/{alias}#Config.REWARDS', 'goods':[good]})
        self.assertEqual(generator.relevant_shops(shops), shops[:1])

    def test_material_packages_require_official_selectable_definition(self):
        text = ','.join((reward("自选礼包","Material,139:11:1#4:55:10,1"),
                         reward("随机礼包","Material,139:12:1,1")))
        goods = generator.parse_objects(text,include_manual=True,selectable_packages={"11":{}})
        self.assertEqual(goods,[])

    def test_manual_scan_excludes_choices_and_locks_automatic_cultivation_rows(self):
        auto = reward("培养", "CommonEnhancePrize,-1,1792,31,9001")
        choice = reward("任选", "Choice,|PetOpen,7115|PetOpen,6964")
        text = ("public static const SERVER_ID_1_REWARD_CONFIG:Array=[" + auto + "];"
                "public static const SERVER_ID_9_REWARD_CONFIG:Array=[" + choice + "];"
                "public static const SERVER_ID_10_REWARD_CONFIG:Array=[" + reward("任选宣传", "BatchMaterial,4:1336:5,1792") + "];")
        default = generator.parse_config(text)
        scanned = generator.parse_config(text, include_manual=True)
        self.assertEqual([shop["shopId"] for shop in default["shops"]], [1])
        self.assertEqual([shop["shopId"] for shop in scanned["shops"]], [1])
        self.assertFalse(scanned["shops"][0]["goods"][0]["manualSelectionRequired"])

    def test_payload_decides_inclusion_not_description(self):
        body = ",".join((reward("全新培养项目名称", "CommonEnhancePrize,-1,1792,31,9001#9002"),
                         reward("指定精灵奖励礼盒", "BatchMaterial,4:1336:5,1792")))
        goods = generator.parse_objects(body)
        self.assertEqual(len(goods), 1)
        self.assertEqual(goods[0]["raceIds"], [9001, 9002])
        self.assertEqual(goods[0]["enhanceType"], "31")

    def test_invalid_race_list_does_not_silently_broaden_or_truncate(self):
        for races in ("9001#bad", "0", "*", "9001#", "9001##9002"):
            with self.assertRaises(ValueError):
                generator.parse_objects(reward("指定精灵", f"CommonEnhancePrize,-1,1792,31,{races}"))

    def test_choice_reward_with_dynamic_prices_is_also_excluded(self):
        row = json.loads(reward("任选兑换", "SelectPrizes,4:55:1#4:56:1,1"))
        row["cost"] = "8:1:40000#8:1:35000#8:1:30000"
        self.assertEqual(generator.parse_objects(json.dumps(row), include_manual=True), [])

    def test_new_shop_and_official_date_rollover(self):
        text = ('public static const TOTAL_CONFIG:Object = {"month":{'
                '"serverId":9,"name":"新商店","isOnline":"TRUE",'
                '"startTime":"20260828","removalTime":"20260929"}};'
                'public static const SERVER_ID_9_REWARD_CONFIG:Array = [' +
                reward("指定精灵元魂觉醒", "CommonEnhancePrize,-1,1792,44,9001", "2026929") + '];')
        shop = generator.parse_config(text)["shops"][0]
        self.assertEqual(shop["shopId"], 9)
        self.assertEqual(generator.official_date("2026929"), "20330809")
        self.assertEqual(shop["goods"][0]["removalTime"], "20260929")
        self.assertEqual(shop["goods"][0]["officialRemovalTime"], "2026929")

    def test_full_official_timestamp_is_validated_and_reduced_to_business_date(self):
        self.assertEqual(generator.official_date("20261009 02:00:00"), "20261009")
        for invalid in ("20261009 24:00:00", "20260230 02:00:00", "20261009T02:00:00"):
            with self.assertRaises(ValueError):
                generator.official_date(invalid)

    def test_current_official_catalog_and_enhancement_coverage(self):
        data = json.loads((ROOT / "assets/shop-exchange-data.json").read_text(encoding="utf-8"))
        fixed_shops = [s for s in data["shops"] if not s.get("sourceKey")]
        goods = [g for s in fixed_shops for g in s["goods"]]
        self.assertEqual(len(goods), 26)
        self.assertEqual(sum(g["shelfTime"] <= "20260913" and
                             (not g["removalTime"] or g["removalTime"] >= "20260913") for g in goods), 18)
        self.assertEqual({s["shopId"] for s in fixed_shops}, {1, 2, 3, 4, 5, 6})
        self.assertTrue(all(g["raceIds"] for g in goods))
        self.assertEqual({g["enhanceType"] for g in goods if "金星" in g["description"]}, {"31"})
        self.assertEqual(data["source"]["shopVersion"], "2026091776736973")


if __name__ == "__main__":
    unittest.main()
