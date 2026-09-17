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


def iter_call_args(source: str, marker: str):
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


def iter_create_args(source: str):
    yield from iter_call_args(source, "PetDictionaryDataItem.create(")


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
            breakthrough_costs = json.loads(args[62])
            if not isinstance(breakthrough_costs, str):
                raise ValueError("official astrolabe breakthrough costs are not a literal string")
            # PetDictionaryDataItem.create(param29:String) assigns _sign.
            # Preserve the official tags even when a skin hides the era in
            # its display name; do not derive an era from slot counts/costs.
            sign = json.loads(args[28])
            if not isinstance(sign, str):
                raise ValueError("official pet sign is not a literal string")
            pets[str(race_id)] = {
                "name": decode_as_string(args[1]),
                "attributes": decode_as_string(args[8]),
                "jobs": decode_as_string(args[9]),
                "groupRaceId": parse_int(args[42]),
                "sign": sign,
                "astrolabeBreakCosts": breakthrough_costs,
                # PetDictionaryDataItem.create(param31) → maxLevel.
                "maxLevel": parse_int(args[30], 0),
                # PetDictionaryDataItem.create(param64).  The official getter
                # falls back to level 6 when this value is not positive.
                "stargodSlotMaxLevel": max(6, parse_int(args[63], 6)),
            }
    return pets


def merge_pet_sources(roots: list[Path]) -> dict[str, dict]:
    """Use explicit base/update order; filesystem traversal order is not a version."""
    result: dict[str, dict] = {}
    for root in roots:
        result.update(parse_pets(root))
    return result


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
            number = strict_int(level.get("level", ""), "star level", 1)
            if str(number) in levels:
                raise ValueError("duplicate official star-god level")
            levels[str(number)] = strict_int(level.get("battlePower", ""), "star battle power")
        attrs["battlePower"] = levels
        result[attrs["defineId"]] = attrs
    return result


def strict_int(token: str, label: str, minimum: int = 0) -> int:
    if not re.fullmatch(r"\d+", token.strip()) or not minimum <= int(token) <= 2**31 - 1:
        raise ValueError(f"invalid official {label}: {token}")
    return int(token)

def strict_flag(token: str, label: str) -> bool:
    value = strict_int(token, label)
    if value not in (0, 1):
        raise ValueError(f"invalid official {label}: {token}")
    return bool(value)


def parse_pet_job_attack_types(source: str) -> dict[str, list[int]]:
    """Resolve current official enum aliases rather than maintaining a job list."""
    aliases = dict(re.findall(r"const\s+(\w+)\s*:\s*PetAttackType\s*=\s*PetAttackType\.(\w+)\s*;", source))
    result: dict[str, list[int]] = {}
    seen = set()
    declarations = re.findall(r"const\s+(\w+)\s*:\s*PetJob\s*=\s*enum__\((.*?)\)\s*;", source, re.S)
    if not declarations:
        raise ValueError("official PetJob enum definitions are missing")
    for name, arguments in declarations:
        args = split_as_args(arguments)
        if len(args) < 3:
            raise ValueError("official PetJob constructor changed")
        job = strict_int(args[0], "job id", 1)
        if job in seen:
            raise ValueError("duplicate official job id")
        seen.add(job)
        token = args[2].strip()
        if token == "null":
            continue
        attack = token.removeprefix("PetAttackType.") if token.startswith("PetAttackType.") else aliases.get(token)
        if not attack:
            raise ValueError("unresolved official job attack type")
        if name not in {"WORLD_BOSS", "GOD_BOSS"}:
            result.setdefault(attack, []).append(job)
    return {key: sorted(value) for key, value in result.items()}


def parse_stargod_rules(source: str, pet_jobs: str) -> dict[str, dict]:
    job_groups = parse_pet_job_attack_types(pet_jobs)
    if not all(token in source for token in ("PetJob.valuesOfAttackType", "PetJob.WORLD_BOSS", "PetJob.GOD_BOSS")):
        raise ValueError("official star-god job restriction helper changed")
    types: dict[int, list[int]] = {}
    for args in iter_call_args(source, "new StarGodType("):
        if len(args) != 4:
            raise ValueError("official StarGodType constructor changed")
        type_id = strict_int(args[0], "star type", 1)
        token = args[3].strip()
        if re.fullmatch(r"\[\s*\]", token):
            limits = []
        else:
            match = re.fullmatch(r"jobIds\((.*)\)", token, re.S)
            if not match:
                raise ValueError("unsupported official star-god job restriction")
            limits = []
            for group in split_as_args(match[1]):
                match_group = re.fullmatch(r"PetAttackType\.(\w+)", group)
                if not match_group or match_group[1] not in job_groups:
                    raise ValueError("unresolved official star-god job restriction")
                limits.extend(job_groups[match_group[1]])
        if type_id in types:
            raise ValueError("duplicate official star type")
        types[type_id] = sorted(set(limits))
    if not types:
        raise ValueError("official star types are missing")
    result = {}
    for args in iter_call_args(source, "new StarGodItem("):
        if len(args) < 13:
            raise ValueError("official StarGodItem constructor changed")
        identifier = str(strict_int(args[0], "star id", 1))
        type_id = strict_int(args[10], "star type", 1)
        quality = strict_int(args[11], "star quality")
        if type_id not in types or quality > 6 or args[12] not in {"true", "false"} or identifier in result:
            raise ValueError("invalid official star-god item rules")
        result[identifier] = {"type": type_id, "quality": quality, "limitJobs": types[type_id],
                              "limited": args[12] == "true", "changeable": type_id in {24, 25}}
    if not result:
        raise ValueError("official star-god item rules are missing")
    return result


def parse_stargod_catalog(swf: Path, material_script: Path, pet_job_script: Path) -> dict[str, dict]:
    stars = parse_stargods(swf)
    rules = parse_stargod_rules(material_script.read_text(encoding="utf-8-sig"),
                               pet_job_script.read_text(encoding="utf-8-sig"))
    if not stars or set(stars) != set(rules):
        raise ValueError("official star-god item and level tables do not agree")
    if any(not value["battlePower"] for key, value in stars.items() if rules[key]["quality"] >= 2):
        raise ValueError("official equipable star-god level table is missing")
    return {key: {"name": value["name"], "supplyExp": strict_int(value.get("supplyExp", "0"), "star experience"),
                  "battlePower": value["battlePower"], **rules[key]} for key, value in stars.items()}


def parse_astrolabe_catalog(swf: Path) -> dict[str, dict]:
    rows = parse_named_sheet(swf)
    if not rows:
        raise ValueError("official astrolabe table is missing")
    if any(value.get("exclusive") not in {"0", "1"} for value in rows.values()):
        raise ValueError("official astrolabe exclusivity rule is missing")
    return {key: {"name": value["name"], "exclusive": value.get("exclusive", "0") == "1",
                  "lightUpCost": value.get("lightUpCost", ""),
                  "lightUpMaterials": parse_material_cost(value.get("lightUpCost", "")),
                  "battlePower": strict_int(value.get("battlePower", ""), "astrolabe battle power"),
                  "locatedType": strict_int(value.get("locatedTypeId", ""), "astrolabe location"),
                  "isTBD": strict_flag(value.get("isTBD", "0"), "astrolabe unavailable flag")}
            for key, value in rows.items()}


def parse_material_cost(value: str) -> list[dict]:
    """Official material syntax is type:id:count or type:id:variant:count."""
    result = []
    for token in value.split("#"):
        if not token.strip():
            continue
        parts = token.split(":")
        if len(parts) not in (3, 4):
            raise ValueError("official cultivation material syntax changed")
        numbers = [strict_int(part, "cultivation material") for part in parts]
        if numbers[0] <= 0 or numbers[-1] <= 0:
            raise ValueError("official cultivation material type/count is invalid")
        item = {"type": numbers[0], "id": numbers[1], "count": numbers[-1]}
        if len(numbers) == 4:
            item["extra"] = numbers[2]
        result.append(item)
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
        identifier = str(strict_int(attrs["defineId"], "badge id", 1))
        badge_type = strict_int(attrs.get("type", ""), "badge type")
        if badge_type not in (0, 1) or identifier in result:
            raise ValueError("official badge definition changed")
        parsed_levels = {}
        for item in levels:
            level = str(strict_int(item.get("level", ""), "badge level", 1))
            if level in parsed_levels or "materialStrToUpgrade" not in item:
                raise ValueError("official badge level cost is missing")
            parsed_levels[level] = {
                "battlePower": strict_int(item.get("battlePower", ""), "badge battle power"),
                "cost": parse_material_cost(item["materialStrToUpgrade"]),
            }
        if badge_type == 0 and (not parsed_levels or sorted(map(int, parsed_levels)) != list(range(1, len(parsed_levels) + 1))):
            raise ValueError("official badge levels are incomplete")
        if badge_type == 1 and "materialStrToActivate" not in attrs:
            raise ValueError("official badge activation cost is missing")
        result[identifier] = {
            "name": attrs["name"],
            "type": badge_type,
            "maxLevel": len(parsed_levels) if badge_type == 0 else 1,
            "levels": parsed_levels,
            "activationCost": parse_material_cost(attrs.get("materialStrToActivate", "")),
        }
        if badge_type == 1:
            result[identifier]["battlePower"] = strict_int(attrs.get("battlePower", ""), "badge activation power")
    if not result:
        raise ValueError("official badge table is missing")
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
    items.update(parse_named_constructors(root, "ItemData*.as", "ItemForBadge"))
    return items


def parse_money(root: Path) -> dict[str, dict]:
    return parse_named_constructors(root, "MoneyData*.as", "Money")


def parse_source_beasts(root: Path) -> dict[str, dict]:
    path = find_one(root, "E4PV2_EInfos.as")
    result: dict[str, dict] = {}
    for args in iter_call_args(path.read_text(encoding="utf-8-sig"), "new E4PV2_EInfo("):
        if len(args) < 2:
            raise ValueError("official source-beast constructor changed")
        identifier = str(strict_int(args[0], "source beast id", 1))
        name = json.loads(args[1]) if args[1].startswith('"') else ""
        if not isinstance(name, str) or not name or identifier in result:
            raise ValueError("official source-beast name table is invalid")
        item: dict = {"name": name}
        if len(args) > 3 and args[3].startswith('"'):
            attr = json.loads(args[3])
            if isinstance(attr, str) and attr:
                item["attr"] = attr
        result[identifier] = item
    if len(result) < 20:
        raise ValueError("official source-beast table is missing")
    return result


def parse_legend_stones(root: Path) -> dict[str, dict]:
    path = find_one(root, "LgsConfig.as")
    source = path.read_text(encoding="utf-8-sig")
    result: dict[str, dict] = {}
    for match in re.finditer(
        r'\{\s*"id"\s*:\s*(\d+)\s*,\s*"name"\s*:\s*"([^"]+)"(?P<body>.*?)(?=\n\s*\},\{\s*"id"|\n\s*\}\];)',
        source,
        re.S,
    ):
        identifier = match.group(1)
        item: dict = {"name": match.group(2)}
        levels: dict[str, dict] = {}
        for level, desc in re.findall(r'"level"\s*:\s*(\d+)\s*,\s*"desc"\s*:\s*"([^"]*)"', match.group("body")):
            levels[level] = {"desc": desc}
        if levels:
            item["levels"] = levels
        result[identifier] = item
    if len(result) < 18:
        raise ValueError("official legend-stone table is missing")
    return result


def parse_proficiencies(root: Path) -> dict[str, dict]:
    matches = sorted(root.rglob("*xmlProficientInfoClass.bin"))
    if not matches:
        raise FileNotFoundError("cannot find proficient XML below unpack root")
    xml = matches[-1].read_text(encoding="utf-8", errors="replace")
    result: dict[str, dict] = {}
    for raw in re.finditer(r"<proinfo\b([^>]*)>", xml):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', raw.group(1)))
        identifier = attrs.get("id", "")
        name = attrs.get("name", "")
        if not identifier.isdigit() or not name or identifier in result:
            raise ValueError("official proficient table is invalid")
        item: dict = {"name": name}
        if attrs.get("desc"):
            item["desc"] = attrs["desc"]
        result[identifier] = item
    if len(result) < 20:
        raise ValueError("official proficient table is missing")
    return result


def parse_sacred_sources(source_path: Path) -> dict[str, str]:
    result = {}
    for args in iter_call_args(source_path.read_text(encoding="utf-8-sig"), "new Equipment4PetItem("):
        if len(args) < 3 or strict_int(args[1], "source equipment type") != 24:
            raise ValueError("official source equipment constructor changed")
        identifier = str(strict_int(args[0], "source equipment id", 1))
        if identifier in result or not args[2].startswith('"'):
            raise ValueError("official source equipment name is invalid")
        result[identifier] = json.loads(args[2])
    if not result:
        raise ValueError("official source equipment table is missing")
    return result


def parse_sacred(source_path: Path, source_items: Path | None = None) -> dict[str, dict]:
    source = source_path.read_text(encoding="utf-8")
    result: dict[str, dict] = {}
    pattern = re.compile(r'"(?P<key>\d+)"\s*:\s*(?P<body>\{[^{}]*\})', re.S)
    for match in pattern.finditer(source):
        value = json.loads(match.group("body"))
        identifier = str(strict_int(str(value.get("id", "")), "sacred id", 1))
        if identifier != match.group("key") or not isinstance(value.get("name"), str) or identifier in result:
            raise ValueError("official sacred equipment definition changed")
        result[identifier] = {"name": value["name"], "sourceId": strict_int(str(value.get("sourceId", "")), "sacred source id", 1)}
    if not result:
        raise ValueError("official sacred equipment table is missing")
    if source_items is not None:
        names = parse_sacred_sources(source_items)
        for item in result.values():
            # Official service files can publish a future sacred definition
            # before its consumable exists. Do not fabricate that item's name.
            item["sourceName"] = names.get(str(item["sourceId"]), "")
            item["sourceNameKnown"] = bool(item["sourceName"])
    return result


def parse_sacred_plans(source_path: Path, kind: str) -> dict[str, dict]:
    if kind not in ("Star", "Stage"):
        raise ValueError("invalid sacred plan kind")
    result = {}
    source = source_path.read_text(encoding="utf-8-sig")
    for args in iter_call_args(source, f"new SE_{kind}Define("):
        if len(args) != 3:
            raise ValueError("official sacred plan constructor changed")
        identifier = str(strict_int(args[0], "sacred plan id", 1))
        levels = {}
        for row in iter_call_args(args[2], f"new SE_{kind}LevelDefine("):
            if len(row) != (4 if kind == "Star" else 6):
                raise ValueError("official sacred level constructor changed")
            level = str(strict_int(row[0], "sacred plan level", 1))
            if level in levels:
                raise ValueError("duplicate official sacred level")
            power_index = 2 if kind == "Star" else 3
            cost_token = row[power_index + 1]
            if not cost_token.startswith('"') or not cost_token.endswith('"'):
                raise ValueError("official sacred cost string changed")
            levels[level] = {
                "battlePower": strict_int(row[power_index], "sacred level battle power"),
                "cost": parse_material_cost(json.loads(cost_token)),
                "equipmentCount": 0 if kind == "Star" else strict_int(row[5], "sacred equipment cost"),
            }
        if not levels or identifier in result or sorted(map(int, levels)) != list(range(1, len(levels) + 1)):
            raise ValueError("official sacred plan levels are incomplete")
        result[identifier] = {"maxLevel": len(levels), "levels": levels}
    if not result:
        raise ValueError("official sacred upgrade plans are missing")
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
    from public_names_updater import enum_names, fusion_jobs, NAME_RULE_VERSION
    parser = argparse.ArgumentParser()
    parser.add_argument("--unpack-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.unpack_root.resolve()

    badge_swf = find_one(root, "petbadgeservice~*.swf")
    sacred_as = find_one(root, "SE_SacredEquipmentConfig.as")
    astrolabe_swf = find_one(root, "astrolabeservice~*.swf")
    stargod_swf = find_one(root, "stargodservice~*.swf")

    stargods = parse_stargod_catalog(stargod_swf, find_one(root, "StarGodItemService.as"), find_one(root, "PetJob.as"))
    astrolabe = parse_astrolabe_catalog(astrolabe_swf)
    attr_source = find_one(root, "PetAttr.as").read_text(encoding="utf-8-sig")
    job_source = find_one(root, "PetJob.as").read_text(encoding="utf-8-sig")
    job_names = enum_names(job_source,"PetJob")

    catalog = {
        "schema": 1,
        "petDictionarySchema": 3,
        "powerRuleVersion": 1,
        "cultivationRuleVersion": 1,
        "source": {"kind": "aoqi-official-unpack", "powerRuleVersion": 1},
        "nameRuleVersion": NAME_RULE_VERSION,
        "attributes": {"0":"全部", **enum_names(attr_source,"PetAttr")},
        "jobs": job_names,
        "fusionJobs": fusion_jobs(job_source,job_names),
        "pets": parse_pets(root),
        "badges": parse_badges(badge_swf),
        "sacredEquipment": parse_sacred(sacred_as, find_one(root, "Equipment4PetItemService.as")),
        "sacredStarPlans": parse_sacred_plans(find_one(root, "SE_StarUpgradeConfig.as"), "Star"),
        "sacredStagePlans": parse_sacred_plans(find_one(root, "SE_StageUpgradeConfig.as"), "Stage"),
        "astrolabe": astrolabe,
        "stargods": stargods,
        "items": parse_items(root),
        "money": parse_money(root),
        "sourceBeasts": parse_source_beasts(root),
        "legendStones": parse_legend_stones(root),
        "proficiencies": parse_proficiencies(root),
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
        f"items={len(catalog['items'])}, money={len(catalog['money'])}, "
        f"sourceBeasts={len(catalog['sourceBeasts'])}, legendStones={len(catalog['legendStones'])}, "
        f"proficiencies={len(catalog['proficiencies'])}"
    )


if __name__ == "__main__":
    main()
