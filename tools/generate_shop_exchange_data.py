#!/usr/bin/env python3
"""Generate shop-exchange-data.json from official SEFConfig.as."""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEF_CONFIG = Path(
    r"D:\奥奇工程\奥奇传说解包\奥奇传说解包\商店兑换框架\解包"
    r"\storeexchangeframework~2026081364897007_decomp\scripts"
    r"\mmo\activityext\newact20260313\storeexchangeframework\model\SEFConfig.as"
)
OUTPUT = ROOT / "assets" / "shop-exchange-data.json"

WANTED = [
    "指定精灵满1条星迹",
    "指定精灵源兽神觉升1阶",
    "指定精灵神运极品",
    "指定通灵师装备1颗红星",
    "指定精灵源兽升1阶",
    "指定精灵源兽满星级",
    "指定精灵满级",
    "指定精灵元魂满级",
    "指定精灵装备1个1级红色星神",
    "指定精灵装备1个专属元魂",
    "指定精灵完美天赋",
    "指定精灵满星源兽",
    "指定精灵满级元魂",
    "指定精灵源兽觉醒1阶",
]
WANTED_SET = set(WANTED)

SHOP_NAMES = {
    1: "永恒战场商店",
    2: "奥奇之星商店",
    3: "排位赛商店",
    4: "竞技场商店",
    5: "月福利中心商店",
    6: "联盟商店",
}

LIMIT_TYPES = [
    {"index": 0, "key": "dl", "label": "日"},
    {"index": 1, "key": "wl", "label": "周"},
    {"index": 2, "key": "ml", "label": "月"},
    {"index": 3, "key": "pl", "label": "期"},
    {"index": 4, "key": "tl", "label": "总"},
]


def field(block: str, name: str) -> str:
    match = re.search(rf'"{name}":(?:"((?:\\.|[^"\\])*)"|(-?\d+))', block)
    if not match:
        return ""
    return match.group(1) if match.group(1) is not None else match.group(2)


def parse_objects(body: str) -> list[dict]:
    items = []
    for match in re.finditer(r"\{([^{}]+)\}", body):
        block = match.group(1)
        desc = field(block, "basicDescription")
        if desc not in WANTED_SET:
            continue
        simple = field(block, "simpleParams")
        enhance_type = ""
        races: list[int] = []
        prize = re.search(
            r"CommonEnhancePrize,[^,]+,[^,]+,([^,]+),([^,\"]+)", simple
        )
        if prize:
            enhance_type = prize.group(1)
            races = [int(x) for x in prize.group(2).split("#") if x.isdigit()]
        limit = field(block, "limit")
        limit_index = -1
        limit_count = 0
        limit_key = ""
        limit_label = ""
        if ":" in limit:
            left, right = limit.split(":", 1)
            if left.isdigit() and right.isdigit():
                limit_index = int(left)
                limit_count = int(right)
                if 0 <= limit_index < len(LIMIT_TYPES):
                    limit_key = LIMIT_TYPES[limit_index]["key"]
                    limit_label = LIMIT_TYPES[limit_index]["label"]
        items.append(
            {
                "id": int(field(block, "id") or 0),
                "itemServerId": int(field(block, "serverId") or 0),
                "tab": int(field(block, "tab") or 0),
                "description": desc,
                "shelfTime": field(block, "shelfTime"),
                "removalTime": field(block, "removalTime"),
                "limit": limit,
                "limitIndex": limit_index,
                "limitCount": limit_count,
                "limitKey": limit_key,
                "limitLabel": limit_label,
                "cost": field(block, "cost"),
                "enhanceType": enhance_type,
                "raceIds": races,
                "filterKey": field(block, "filterKey"),
                "unlock": field(block, "unlock"),
                "tag": field(block, "tag"),
            }
        )
    return items


def main() -> None:
    text = SEF_CONFIG.read_text(encoding="utf-8")
    shops = []
    for shop_id, shop_name in SHOP_NAMES.items():
        marker = f"public static const SERVER_ID_{shop_id}_REWARD_CONFIG:Array = "
        start = text.find(marker)
        if start < 0:
            raise SystemExit(f"missing shop config {shop_id}")
        rest = text[start + len(marker) :]
        end = rest.find("public static const")
        body = rest if end < 0 else rest[:end]
        goods = parse_objects(body)
        shops.append(
            {
                "shopId": shop_id,
                "name": shop_name,
                "siKey": f"si{shop_id}",
                "goods": goods,
            }
        )

    payload = {
        "protocol": {
            "extension": "TimelinessActExtension",
            "getInfoCommand": "1008_20260313_es_0",
            "getInfoParams": {},
            "activityId": 1792,
            "itemKeyPrefix": "bi",
            "shopKeyPrefix": "si",
        },
        "wanted": WANTED,
        "limitTypes": LIMIT_TYPES,
        "shops": shops,
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"wrote {OUTPUT}")
    for shop in shops:
        print(f"=== {shop['name']} ({len(shop['goods'])}) ===")
        for good in shop["goods"]:
            print(
                f"  {good['description']} bi={good['itemServerId']} "
                f"limit={good['limit']}({good['limitLabel']}) "
                f"races={len(good['raceIds'])}"
            )


if __name__ == "__main__":
    main()
