"""Small offline regressions for designated-pet catalog extraction."""
import importlib.util
import json
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("shop_generator", ROOT / "tools/generate_shop_exchange_data.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


def reward(description, simple, removal="20260929"):
    return json.dumps({"id": 1, "serverId": 1, "tab": 1, "basicDescription": description,
                       "simpleParams": simple, "shelfTime": "20260828", "removalTime": removal,
                       "cost": "4:3237:1", "limit": "3:1"}, ensure_ascii=False)


class DesignatedPetCatalog(unittest.TestCase):
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

    def test_current_official_catalog_and_enhancement_coverage(self):
        data = json.loads((ROOT / "assets/shop-exchange-data.json").read_text(encoding="utf-8"))
        goods = [g for s in data["shops"] for g in s["goods"]]
        self.assertEqual(len(goods), 26)
        self.assertEqual(sum(g["shelfTime"] <= "20260913" and
                             (not g["removalTime"] or g["removalTime"] >= "20260913") for g in goods), 25)
        self.assertEqual({s["shopId"] for s in data["shops"]}, {1, 2, 3, 4, 5, 6})
        self.assertTrue(all(g["raceIds"] for g in goods))
        self.assertEqual({g["enhanceType"] for g in goods if "金星" in g["description"]}, {"31"})
        self.assertEqual(data["source"]["shopVersion"], "2026091011614836")


if __name__ == "__main__":
    unittest.main()
