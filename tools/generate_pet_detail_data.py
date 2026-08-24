#!/usr/bin/env python3
"""Generate the compact pet-detail catalog embedded by the Qt extension.

The source of truth is the official AoQi unpack directory.  The game keeps
several XML sheets as DefineBinaryData inside compressed SWFs, so this script
also extracts those sheets directly from the SWF payload.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import zlib
from pathlib import Path


ATTRIBUTE_NAMES = [
    "全部", "普", "草", "水", "火", "风", "电", "土", "暗", "龙", "冰",
    "光", "飞行", "机械", "武", "美食", "超能", "幻化", "恶魔", "萌",
    "战神", "神草", "神水", "神火", "神暗", "神光", "神灵", "神无极", "神幻",
]

JOB_NAMES = {
    1: "利爪", 2: "魔法", 3: "射击", 4: "治疗", 5: "平衡", 6: "肉盾",
    7: "英雄", 8: "英雄", 9: "召唤师", 10: "召唤师", 11: "巨人",
    12: "巨人", 13: "世界BOSS", 14: "龙骑", 15: "龙骑", 16: "神职BOSS",
    17: "赋能师", 18: "赋能师", 19: "元素师", 20: "元素师", 21: "神速",
    22: "神攻", 23: "神平衡", 24: "神盾", 25: "神英雄", 26: "神召唤师",
    27: "神巨人", 28: "通灵师", 29: "超级英雄", 30: "备用7", 31: "神速",
    32: "神攻", 33: "神平衡", 34: "神盾", 35: "神英雄", 36: "神召唤师",
    37: "神巨人", 38: "通灵师", 39: "超级英雄", 40: "天觉者",
    41: "天觉者", 42: "幻元师", 43: "幻元师",
}


def find_one(root: Path, pattern: str) -> Path:
    matches = sorted(root.rglob(pattern))
    if not matches:
        raise FileNotFoundError(f"cannot find {pattern} below {root}")
    return matches[-1]


def decode_as_string(token: str) -> str:
    token = token.strip()
    if token.startswith('"') and token.endswith('"'):
        try:
            return json.loads(token)
        except json.JSONDecodeError:
            return token[1:-1].replace(r'\"', '"').replace(r"\\", "\\")
    return token


def split_as_args(text: str) -> list[str]:
    result: list[str] = []
    start = 0
    depth = 0
    quoted = False
    escaped = False
    for index, char in enumerate(text):
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char in "([{":
            depth += 1
        elif char in ")]}" and depth:
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:index].strip())
            start = index + 1
    result.append(text[start:].strip())
    return result


def iter_create_args(source: str):
    marker = "PetDictionaryDataItem.create("
    offset = 0
    while True:
        begin = source.find(marker, offset)
        if begin < 0:
            return
        cursor = begin + len(marker)
        quoted = False
        escaped = False
        depth = 1
        while cursor < len(source) and depth:
            char = source[cursor]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            else:
                if char == '"':
                    quoted = True
                elif char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
            cursor += 1
        if depth == 0:
            yield split_as_args(source[begin + len(marker): cursor - 1])
        offset = cursor


def parse_int(token: str, default: int = 0) -> int:
    match = re.search(r"-?\d+", token.strip())
    return int(match.group()) if match else default


def parse_pets(root: Path) -> dict[str, dict]:
    files = list(root.rglob("PetDictionaryDataContents.as"))
    files += list(root.rglob("PetDictionaryDataContentsUpdate.as"))
    pets: dict[str, dict] = {}
    for path in sorted(files, key=lambda item: "Update" in item.name):
        source = path.read_text(encoding="utf-8")
        for args in iter_create_args(source):
            if len(args) < 64:
                continue
            race_id = parse_int(args[0], -1)
            if race_id < 0:
                continue
            pets[str(race_id)] = {
                "name": decode_as_string(args[1]),
                "attributes": decode_as_string(args[8]),
                "jobs": decode_as_string(args[9]),
                "groupRaceId": parse_int(args[42]),
                # PetDictionaryDataItem.create(param64).  The official getter
                # falls back to level 6 when this value is not positive.
                "stargodSlotMaxLevel": max(6, parse_int(args[63], 6)),
            }
    return pets


def swf_text(path: Path) -> str:
    data = path.read_bytes()
    if data[:3] == b"CWS":
        data = data[:8] + zlib.decompress(data[8:])
    elif data[:3] != b"FWS":
        raise ValueError(f"unsupported SWF signature in {path}")
    return data.decode("utf-8", errors="ignore")


def xml_attributes(fragment: str) -> dict[str, str]:
    return {
        key: html.unescape(value)
        for key, value in re.findall(r'(\w+)="([^"]*)"', fragment)
    }


def parse_named_sheet(path: Path) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for opening in re.findall(r"<s\s+[^>]*>", swf_text(path)):
        attrs = xml_attributes(opening)
        if "defineId" in attrs and "name" in attrs:
            result[attrs["defineId"]] = attrs
    return result


def parse_stargods(path: Path) -> dict[str, dict]:
    """Read star-god metadata and the official per-level battle-power table."""
    result: dict[str, dict] = {}
    for match in re.finditer(r"<s\s+([^>]*)>(.*?)</s>", swf_text(path), re.S):
        attrs = xml_attributes(match.group(1))
        if "defineId" not in attrs or "name" not in attrs:
            continue
        levels: dict[str, int] = {}
        for fragment in re.findall(r"<l\s+([^>]*)/?>", match.group(2)):
            level = xml_attributes(fragment)
            number = parse_int(level.get("level", "0"))
            if number > 0:
                levels[str(number)] = parse_int(level.get("battlePower", "0"))
        attrs["battlePower"] = levels
        result[attrs["defineId"]] = attrs
    return result


def parse_badges(path: Path) -> dict[str, dict]:
    text = swf_text(path)
    result: dict[str, dict] = {}
    for match in re.finditer(r"<s\s+([^>]*)>(.*?)</s>", text, re.S):
        attrs = xml_attributes(match.group(1))
        if "defineId" not in attrs or "name" not in attrs:
            continue
        levels = [
            xml_attributes(fragment)
            for fragment in re.findall(r"<l\s+([^>]*)/?>", match.group(2))
        ]
        result[attrs["defineId"]] = {
            "name": attrs["name"],
            "type": parse_int(attrs.get("type", "0")),
            "maxLevel": max((parse_int(item.get("level", "0")) for item in levels), default=0),
        }
    return result


def parse_named_constructors(root: Path, filename_glob: str, ctor: str) -> dict[str, dict]:
    pattern = re.compile(
        rf'"(?P<id>\d+)"\s*:\s*new {re.escape(ctor)}\(\s*(?P<id2>\d+)\s*,\s*"(?P<name>[^"]*)"',
        re.S,
    )
    result: dict[str, dict] = {}
    for path in sorted(root.rglob(filename_glob)):
        source = path.read_text(encoding="utf-8", errors="replace")
        for match in pattern.finditer(source):
            result[match.group("id")] = {"name": match.group("name")}
    return result


def parse_items(root: Path) -> dict[str, dict]:
    items = parse_named_constructors(root, "ItemData*.as", "Item")
    items.update(parse_named_constructors(root, "ItemData*.as", "ItemForEssence"))
    return items


def parse_money(root: Path) -> dict[str, dict]:
    return parse_named_constructors(root, "MoneyData*.as", "Money")


def parse_sacred(source_path: Path) -> dict[str, dict]:
    source = source_path.read_text(encoding="utf-8")
    result: dict[str, dict] = {}
    pattern = re.compile(
        r'"(?P<key>\d+)"\s*:\s*\{\s*"id"\s*:\s*(?P<id>\d+)\s*,\s*'
        r'"name"\s*:\s*"(?P<name>[^"]+)"', re.S
    )
    for match in pattern.finditer(source):
        result[match.group("id")] = {"name": match.group("name")}
    return result


def star_quality(attrs: dict[str, str]) -> int:
    define_id = parse_int(attrs.get("defineId", "0"))
    name = attrs.get("name", "")
    supply_exp = parse_int(attrs.get("supplyExp", "0"))
    if define_id == 79 or "万变金星" in name:
        return 5
    if define_id == 80 or "万变红星" in name:
        return 6
    # Official StarGodItem quality values are green=2, blue=3, purple=4,
    # gold=5 and red=6.  The service sheet groups those tiers by supply EXP.
    if supply_exp >= 1080:
        return 6
    if supply_exp >= 720:
        return 5
    if supply_exp >= 120:
        return 4
    if supply_exp >= 60:
        return 3
    if supply_exp > 0:
        return 2
    return 0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unpack-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.unpack_root.resolve()

    badge_swf = find_one(root, "petbadgeservice~*.swf")
    sacred_as = find_one(root, "SE_SacredEquipmentConfig.as")
    astrolabe_swf = find_one(root, "astrolabeservice~*.swf")
    stargod_swf = find_one(root, "stargodservice~*.swf")

    stargods = parse_stargods(stargod_swf)
    stargods = {
        key: {
            "name": value["name"],
            "quality": star_quality(value),
            "supplyExp": parse_int(value.get("supplyExp", "0")),
            "changeable": key in {"79", "80"} or "万变" in value["name"],
            "battlePower": value["battlePower"],
        }
        for key, value in stargods.items()
    }
    astrolabe = {
        key: {
            "name": value["name"],
            "exclusive": value.get("exclusive", "0") == "1",
            "lightUpCost": value.get("lightUpCost", ""),
        }
        for key, value in parse_named_sheet(astrolabe_swf).items()
    }

    catalog = {
        "schema": 1,
        "source": "奥奇传说官方解包",
        "attributes": {str(index): name for index, name in enumerate(ATTRIBUTE_NAMES)},
        "jobs": {str(key): value for key, value in JOB_NAMES.items()},
        "pets": parse_pets(root),
        "badges": parse_badges(badge_swf),
        "sacredEquipment": parse_sacred(sacred_as),
        "astrolabe": astrolabe,
        "stargods": stargods,
        "items": parse_items(root),
        "money": parse_money(root),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(catalog, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    print(
        f"generated {args.output}: pets={len(catalog['pets'])}, "
        f"badges={len(catalog['badges'])}, sacred={len(catalog['sacredEquipment'])}, "
        f"astrolabe={len(catalog['astrolabe'])}, stargods={len(catalog['stargods'])}, "
        f"items={len(catalog['items'])}, money={len(catalog['money'])}"
    )


if __name__ == "__main__":
    main()
