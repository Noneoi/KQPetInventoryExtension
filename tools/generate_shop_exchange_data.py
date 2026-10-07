#!/usr/bin/env python3
"""Extract designated-pet exchanges from the official web client's SEFConfig.

Online: --refresh-official --java <java.exe> --ffdec-jar <ffdec.jar>
Offline replay: --config <SEFConfig.as> --provenance <source.json>
No description whitelist: CommonEnhancePrize supplies the exact enhancement
and explicit positive race-id list. Ordinary material rewards are excluded.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, date, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFICIAL_ROOT = "https://aoqi.100bt.com/play/"
RESOURCE = "newactivityext/newact20260313/storeexchangeframework/storeexchangeframework"
CONFIG_CLASS = "mmo.activityext.newact20260313.storeexchangeframework.model.SEFConfig"
LIMIT_TYPES = [{"index": i, "key": key, "label": label}
               for i, (key, label) in enumerate(zip(
                   ("dl", "wl", "ml", "pl", "tl"), ("日", "周", "月", "期", "总")))]
PROTOCOL = {"extension": "TimelinessActExtension", "getInfoCommand": "1008_20260313_es_0",
            "getInfoParams": {}, "activityId": 1792, "itemKeyPrefix": "bi", "shopKeyPrefix": "si"}

# User-requested exclusions apply to these activity identities, including all
# their reward tables. The permanent eternal-battlefield exchange is separate.
EXCLUDED_ACTIVITIES = frozenset({'godfantasynuoyachallenge', 'eternalbattlefield',
                                'shenyundaqiaoevo', 'lingchushenandishitianchallenge'})


def excluded_activity(source_key: str) -> bool:
    return source_key.split('#', 1)[0].rsplit('/', 1)[-1] in EXCLUDED_ACTIVITIES


def field(block: str, name: str) -> str:
    match = re.search(rf'"{name}"\s*:\s*("(?:\\.|[^"\\])*"|-?\d+)', block)
    if not match:
        return ""
    value = match[1]
    return json.loads(value) if value.startswith('"') else value


def package_reward_ids(value) -> set[int]:
    if not isinstance(value, str):
        return set()
    result = set()
    for part in value.lstrip("!").split("|"):
        fields = part.lstrip("!").split(",")
        if len(fields) < 2 or fields[0] not in ("Material", "BatchMaterial"):
            continue
        for item in fields[1].split("#"):
            match = re.fullmatch(r"139:([1-9]\d*):[1-9]\d*", item)
            if match:
                result.add(int(match[1]))
    return result


def selectable_package_ids(value, packages=()) -> list[int]:
    return sorted(package_reward_ids(value) & {int(key) for key in packages})


def selectable_reward(value, packages=()) -> bool:
    """Use the official reward operator, never a promotional description."""
    return (isinstance(value, str) and bool(re.search(r"(?:^!?|\|!?)(?:Choice|SelectPrizes),[^\n]+", value)) or
            bool(selectable_package_ids(value, packages)))


def cultivation_alternative(row: dict) -> bool:
    """One explicit cultivation effect OR ordinary materials, not a choice box.

    Validate the raw target/effect against the normalized facts as well, so an
    old or malformed cached Choice cannot acquire an invented cultivation rule.
    """
    raw = row.get('rewardRaw', '')
    if not isinstance(raw, str) or not raw.removeprefix('!').startswith('Choice,|') or row.get('rewardOptions'):
        return False
    parts = raw.removeprefix('!').split('|')
    if len(parts) != 3:
        return False
    enhance = [p for p in parts[1:] if p.startswith('CommonEnhancePrize,')]
    material = [p for p in parts[1:] if re.fullmatch(
        r'(?:Material|BatchMaterial),[1-9]\d*:[1-9]\d*:[1-9]\d*(?:#[1-9]\d*:[1-9]\d*:[1-9]\d*)*(?:,[1-9]\d*)?', p)]
    if len(enhance) != 1 or len(material) != 1 or package_reward_ids(material[0]):
        return False
    fields = enhance[0].split(',')
    if len(fields) not in (5, 6) or (len(fields) == 6 and fields[5] not in ('VALID_ALL', 'GAIN_BATCH')):
        return False
    if not re.fullmatch(r'[1-9]\d*(?:\$[1-9]\d*)?(?:-[1-9]\d*(?:\$[1-9]\d*)?)*', fields[3]):
        return False
    if not re.fullmatch(r'[1-9]\d*(?:#[1-9]\d*)*(?::(?:false|0))?', fields[4]):
        return False
    races = list(dict.fromkeys(map(int, fields[4].split(':')[0].split('#'))))
    return fields[3] == row.get('enhanceType') and races == row.get('raceIds')


def excluded_choice(row: dict) -> bool:
    if cultivation_alternative(row):
        return False
    return (selectable_reward(row.get('rewardRaw'), row.get('selectablePackageIds', ())) or
            bool(row.get('rewardOptions')) or
            bool(re.search(r'自选|任选|[二三四五六七八九十0-9]+选[一二三四五六七八九十0-9]+', str(row.get('description', '')))))


def relevant_good(row: dict) -> bool:
    return not excluded_choice(row) and (bool(row.get("enhanceType") and row.get("raceIds")) or
                                        targeted_reward(row.get('rewardRaw')))


def targeted_reward(value) -> bool:
    # A known Strengthen effect with a runtime/exclusion filter is a manual
    # candidate; it must not become an unconstrained cultivation rule.
    return isinstance(value, str) and bool(re.match(r'^Strengthen,[1-9]\d*(?:\$[1-9]\d*)?(?:-[1-9]\d*(?:\$[1-9]\d*)?)*,', value))


def relevant_shops(shops: list) -> list:
    # Also migrate older broad scans and last-known-good fallback records.
    return [{**shop, "goods": goods} for shop in shops
            if not excluded_activity(shop.get('sourceKey', '')) and
            (goods := [{**good, 'manualSelectionRequired': True} if cultivation_alternative(good) else good
                       for good in shop.get("goods", []) if relevant_good(good)])]


def official_date(value: str) -> str:
    """DateUtil.parseDate uses fixed substrings and AS Date rollover.

    Official monthly goods say '2026929', which evaluates to 20330809.
    TOTAL_CONFIG separately closes that shop on 20260929; no zero is invented.
    """
    if not value:
        return ""
    if re.fullmatch(r'\d{4}-\d{2}-\d{2}(?: \d{2}:\d{2}:\d{2})?', value):
        value = value[:10].replace('-', '') + value[10:]
    timestamp = re.fullmatch(r"(\d{8}) (\d{2}):(\d{2}):(\d{2})", value)
    if timestamp:
        # Some newer activity tables use the game's full DateUtil timestamp.
        # Validate every component before reducing it to the catalog's
        # day-granularity availability field; malformed values must stay loud.
        datetime.strptime(value, "%Y%m%d %H:%M:%S")
        return timestamp[1]
    if not re.fullmatch(r"\d{7,8}", value):
        raise ValueError(f"unsupported official date: {value!r}")
    year, month, day = int(value[:4]), int(value[4:6]), int(value[6:8])
    if year < 100:
        raise ValueError(f"unsupported official year: {value!r}")
    year, month = divmod(year * 12 + month - 1, 12)
    return (date(year, month + 1, 1) + timedelta(days=day - 1)).strftime("%Y%m%d")


def parse_objects(body: str, shop: dict | None = None, *, include_manual: bool = False, selectable_packages=()) -> list[dict]:
    items = []
    for match in re.finditer(r"\{([^{}]+)\}", body):
        block = match[1]
        reward = field(block, "simpleParams")
        if excluded_choice({'rewardRaw': reward, 'description': field(block, 'basicDescription'),
                            'selectablePackageIds': selectable_package_ids(reward, selectable_packages)}):
            continue
        simple = reward.split(",")
        automatic = simple[0] == "CommonEnhancePrize"
        if not automatic and not (include_manual and selectable_reward(reward, selectable_packages) and field(block, "cost")):
            continue
        if automatic and (len(simple) != 5 or not re.fullmatch(r"[1-9]\d*(?:-[1-9]\d*)*", simple[3])
                or not re.fullmatch(r"[1-9]\d*(?:#[1-9]\d*)*", simple[4])):
            raise ValueError("invalid designated-pet CommonEnhancePrize payload")
        races = list(dict.fromkeys(int(x) for x in simple[4].split("#"))) if automatic else []
        limit = field(block, "limit")
        limit_index = limit_count = -1
        if re.fullmatch(r"[0-4]:\d+", limit):
            limit_index, limit_count = map(int, limit.split(":"))
        raw_start, raw_end = field(block, "shelfTime"), field(block, "removalTime")
        start, end = official_date(raw_start), official_date(raw_end)
        if not start:
            raise ValueError("missing official shelf date")
        if shop:
            shop_start = official_date(shop.get("startTime", ""))
            shop_end = official_date(shop.get("removalTime", ""))
            end = min(end, shop_end) if end and shop_end else end or shop_end
            # Keep an expired historical reward's old dates for quota/key
            # history; do not give it this season's later start date.
            if shop_start and (not end or end >= shop_start):
                start = max(start, shop_start)
        cost = field(block, "cost")
        cost_keys = [part.rsplit(":", 1)[0] for part in cost.split("#")]
        # Some ordinary SEF rewards have account-dependent prices separated
        # by '#'. Do not add up multiple alternatives for the same currency.
        known_cost = automatic or (bool(re.fullmatch(r"[1-9]\d*:[0-9]+:[1-9]\d*(?:#[1-9]\d*:[0-9]+:[1-9]\d*)*", cost))
                                   and len(set(cost_keys)) == len(cost_keys))
        items.append({
            "id": int(field(block, "id")), "itemServerId": int(field(block, "serverId")),
            "tab": int(field(block, "tab") or 0),
            "description": field(block, "basicDescription") or reward,
            "shelfTime": start, "removalTime": end,
            "officialShelfTime": raw_start, "officialRemovalTime": raw_end,
            "limit": limit, "limitIndex": limit_index, "limitCount": limit_count,
            "limitKey": LIMIT_TYPES[limit_index]["key"] if limit_index >= 0 else "",
            "limitLabel": LIMIT_TYPES[limit_index]["label"] if limit_index >= 0 else "",
            "cost": cost, "costKnown": known_cost,
            "costDescription": "" if known_cost else "价格条件待确认：" + cost,
            "enhanceType": simple[3] if automatic else "", "raceIds": races,
            "manualSelectionRequired": not automatic, "rewardRaw": reward,
            "selectablePackageIds": selectable_package_ids(reward, selectable_packages),
            "filterKey": field(block, "filterKey"), "unlock": field(block, "unlock"),
            "tag": field(block, "tag"),
        })
    return items


def parse_config(text: str, provenance: dict | None = None, *, include_manual: bool = False, selectable_packages=()) -> dict:
    total = re.search(r"TOTAL_CONFIG:Object\s*=\s*(.*?);", text, re.S)
    shop_metadata = {}
    if total:
        for match in re.finditer(r"\{([^{}]+)\}", total[1]):
            block = match[1]
            shop_id = field(block, "serverId")
            if shop_id:
                shop_metadata[int(shop_id)] = {
                    key: field(block, key) for key in ("name", "startTime", "removalTime", "isOnline")}
    shops = []
    markers = list(re.finditer(r"public static const SERVER_ID_(\d+)_REWARD_CONFIG:Array\s*=\s*", text))
    for match in markers:
        shop_id = int(match[1])
        end = text.find("public static const", match.end())
        body = text[match.end():end if end >= 0 else len(text)]
        meta = shop_metadata.get(shop_id, {})
        if meta.get("isOnline") == "FALSE":
            continue
        goods = parse_objects(body, meta, include_manual=include_manual, selectable_packages=selectable_packages)
        if not goods:
            continue
        shops.append({"shopId": shop_id, "name": meta.get("name") or f"商店 {shop_id}",
                      "siKey": f"si{shop_id}",
                      "navigationLink": f"btnNewAct_storeexchangeframework_showMainPanel_{shop_id}",
                      "officialShop": meta, "goods": goods})
    shops.sort(key=lambda shop: shop["shopId"])
    if not shops:
        raise ValueError("no designated-pet exchanges in official config")
    payload = {"schema": 2, "selection": "CommonEnhancePrize with explicit positive raceIds",
               "protocol": PROTOCOL, "limitTypes": LIMIT_TYPES, "shops": shops}
    if provenance:
        payload["source"] = provenance
    return payload


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fetch(url: str, path: Path) -> dict:
    request = urllib.request.Request(url, headers={"User-Agent": "KQPetCatalogGenerator/2.0"})
    with urllib.request.urlopen(request, timeout=45) as response:
        body = response.read(16 * 1024 * 1024 + 1)
        if len(body) > 16 * 1024 * 1024:
            raise ValueError("official resource exceeds 16 MiB")
        path.write_bytes(body)
        return {"url": response.url, "sha256": sha256(path), "bytes": len(body)}


def refresh_official(cache: Path, java: str, ffdec: Path) -> tuple[Path, dict]:
    cache.mkdir(parents=True, exist_ok=True)
    source = {"retrievedAt": datetime.now(timezone.utc).isoformat(timespec="seconds"), "resources": {}}
    source["resources"]["start"] = fetch(OFFICIAL_ROOT + "start.xml", cache / "start.xml")
    start = ET.parse(cache / "start.xml").getroot()
    version = start.findtext("lv") or start.findtext("v")
    if not version or not version.isdecimal():
        raise ValueError("invalid official client version")
    source["clientVersion"] = version
    version_file = cache / f"versiondata~{version}.swf"
    source["resources"]["versiondata"] = fetch(OFFICIAL_ROOT + version_file.name, version_file)
    def export(kind: str, output: Path, swf: Path, selected: str = "") -> None:
        command = [java, "-Djava.awt.headless=true", "-jar", str(ffdec.resolve())]
        if selected:
            command += ["-selectclass", selected]
        subprocess.run(command + ["-export", kind, str(output), str(swf)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    export("binaryData", cache / "versiondata-binary", version_file)
    manifests = list((cache / "versiondata-binary").glob("*DocClass_cls.bin"))
    if len(manifests) != 1:
        raise ValueError("official version manifest missing or ambiguous")
    resource = next((node for node in ET.parse(manifests[0]).getroot().iter("f")
                     if node.get("n") == RESOURCE), None)
    if resource is None or not resource.get("v", "").isdecimal():
        raise ValueError("official shop resource version missing")
    shop_version = resource.get("v")
    for override in start.iter("f"):
        if override.get("n") == RESOURCE:
            shop_version = override.get("v")
    if not shop_version or not shop_version.isdecimal():
        raise ValueError("invalid official shop hotfix version")
    source["shopVersion"] = shop_version
    swf = cache / f"storeexchangeframework~{shop_version}.swf"
    source["resources"]["shop"] = fetch(f"{OFFICIAL_ROOT}{RESOURCE}~{shop_version}.swf", swf)
    export("script", cache / "storeexchange-decomp", swf, CONFIG_CLASS)
    config = cache / "storeexchange-decomp" / "scripts" / Path(*CONFIG_CLASS.split(".")).with_suffix(".as")
    source["configSha256"] = sha256(config)
    source["extractor"] = "JPEXS Free Flash Decompiler 26.2.1; generate_shop_exchange_data.py"
    (cache / "source.json").write_text(json.dumps(source, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return config, source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--provenance", type=Path)
    parser.add_argument("--refresh-official", action="store_true")
    parser.add_argument("--java", default="java")
    parser.add_argument("--ffdec-jar", type=Path)
    parser.add_argument("--cache-directory", type=Path, default=ROOT / "build-official-shop")
    parser.add_argument("--output", type=Path, default=ROOT / "assets" / "shop-exchange-data.json")
    args = parser.parse_args()
    provenance = json.loads(args.provenance.read_text(encoding="utf-8")) if args.provenance else None
    config = args.config
    if args.refresh_official:
        if not args.ffdec_jar:
            parser.error("--refresh-official requires --ffdec-jar")
        config, provenance = refresh_official(args.cache_directory.resolve(), args.java, args.ffdec_jar)
    if not config:
        parser.error("provide --config, or --refresh-official with --ffdec-jar")
    if provenance and provenance.get("configSha256") != sha256(config):
        parser.error("config SHA256 differs from provenance")
    payload = parse_config(config.read_text(encoding="utf-8-sig"), provenance)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.output}")
    for shop in payload["shops"]:
        print(f"{shop['name']}: {len(shop['goods'])} designated-pet exchanges")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
