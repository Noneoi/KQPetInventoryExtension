#!/usr/bin/env python3
"""Build the compact, race-indexed pet skill catalog used by the Qt UI.

Inputs are the official AoQi H5 JSON sheets extracted from their versioned
``config/*.aqz`` bundles.  The builder deliberately keeps server-owned combat
math out of the catalog: only client-published descriptions, rules and links
are represented.
"""

from __future__ import annotations

import argparse
import html
import json
import re
from pathlib import Path


PET_FIELDS = (
    "raceId orgName height weight favour birthPlace desc dictionaryId attributes jobs rare canSeeInBook sendPlace "
    "evolutionInfo normalSkill ultimateSkill superSkill heroSkill evolutionTool starGodSkillName starGodSkillDesc "
    "suitGuardStone relearnPower recommendedStargods recommendedEquipment defaultEquipment recommendedProficient "
    "suitProficient signs rentType maxLevel largeSkill gridType extendedPointAttack recommendedGiftStars dragonJobSkill "
    "yuanLiSkill psychicsSkill strategy position evoableType evoableRaceId groupRaceId beforeGodAttributeRaceId sex "
    "skinFusion fateSkill recommendedLegendStoneGroups recommendedLegendStoneIds transformSkill packageBackground "
    "recommendedBadges psychicsPartners quality birthPlace2 sendPlace2 astrolabe recommendedAstrolabeTraces "
    "alterableName qiYunType sacredEquipment onlineDate astrolabeBreakCosts stargodSlotMaxLevel iconInfo skillEntries "
    "matchWords"
).split()

SKILL_SLOTS = {
    "normal": "normalSkill",
    "ultimate": "ultimateSkill",
    "super": "superSkill",
    "hero": "heroSkill",
    "large": "largeSkill",
    "dragonJob": "dragonJobSkill",
    "yuanLi": "yuanLiSkill",
    "psychics": "psychicsSkill",
    "fate": "fateSkill",
    "transform": "transformSkill",
}

def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def clean_text(value) -> str:
    text = html.unescape(str(value or "").replace("\\'", "'").replace('\\"', '"'))
    text = re.sub(r"\$c\w?", "", text).replace("c$", "")
    text = re.sub(r"<br\s*/?>|\\n|\r?\n", "\n", text, flags=re.I)
    text = re.sub(r"<[^>]+>", "", text).replace("#pet#", "")
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip(" ;；\n")


def split_skill(value: str) -> tuple[str, str, str]:
    parts = str(value or "").split("|", 2)
    parts += [""] * (3 - len(parts))
    return clean_text(parts[0]), clean_text(parts[1]), parts[2]


def pet_row(value) -> dict:
    if not isinstance(value, list) or len(value) < len(PET_FIELDS):
        return {}
    return dict(zip(PET_FIELDS, value))


def positive_int(value) -> int:
    try:
        number = int(value)
        return number if number > 0 else 0
    except (TypeError, ValueError):
        return 0


def ids(value, separator="#") -> list[int]:
    return [int(token) for token in str(value or "").split(separator) if token.isdecimal() and int(token) > 0]


def source_file(root: Path, *candidates: str) -> Path:
    for candidate in candidates:
        for base in (root, root.parent / "config"):
            path = base / candidate
            if path.is_file():
                return path
    raise FileNotFoundError("missing official skill sheet: " + " or ".join(candidates))


def optional_json(root: Path, *candidates: str, default=None):
    for candidate in candidates:
        for base in (root, root.parent / "config"):
            path = base / candidate
            if path.is_file():
                return load_json(path)
    return {} if default is None else default


def relation_index(config: dict) -> dict[str, list[dict]]:
    effects = {str(row[0]): row[2] for row in config.get("relationEffect", [])
               if isinstance(row, list) and len(row) > 2}
    points: dict[tuple[str, str], dict] = {}
    for row in config.get("relationPoint", []):
        if not isinstance(row, list) or len(row) < 9:
            continue
        team = str(row[1])
        for race in ids(row[3]):
            points[(team, str(race))] = {
                "ownedPoints": int(row[6]), "power": int(row[7]), "powerPoints": int(row[8])
            }
    result: dict[str, list[dict]] = {}
    for row in config.get("relationTeam", []):
        if not isinstance(row, list) or len(row) < 8:
            continue
        team_id, name = str(row[0]), clean_text(row[1])
        members = ids(row[2])
        phases = []
        phase_names = str(row[3]).split("|")
        thresholds = str(row[4]).split("|")
        effect_ids = str(row[6]).split("|")
        for phase, threshold, effect_id in zip(phase_names, thresholds, effect_ids):
            item = {"name": phase, "points": int(threshold) if threshold.isdecimal() else 0}
            if effect_id != "-1" and effects.get(effect_id):
                item["effect"] = clean_text(effects[effect_id])
            phases.append(item)
        for race in members:
            entry = {"id": int(team_id), "name": name, "members": members, "phases": phases,
                     "era": clean_text(row[7])}
            entry.update(points.get((team_id, str(race)), {}))
            result.setdefault(str(race), []).append(entry)
    return result


def build_catalog(root: Path, source: dict | None = None) -> dict:
    dictionary = load_json(source_file(root, "petdictionarydata.json/petdictionarydata.json",
                                       "pet/petdictionarydata.json"))
    skill_desc = load_json(source_file(root, "battleconfig/skill_desc_config.json"))
    animations = optional_json(root, "battleconfig/skill_config.json")
    combo_root = optional_json(root, "battleconfig/combo_buffs_config.json")
    entries_root = optional_json(root, "battleconfig/entry_config.json", default=[])
    buffer_root = optional_json(root, "battleconfig/buffer_config.json")
    transform = optional_json(root, "formation/transformskillconfig.json")
    hero = optional_json(root, "formation/heroskillconfig.json")
    psychics = optional_json(root, "formation/psychicsskillconfig.json")
    lingchu = optional_json(root, "formation/lingchuskillsulingconfig.json")
    shenyun = optional_json(root, "formation/shenyunskillqiyunconfig.json")
    relations = relation_index(optional_json(root, "relation/relationconfig.json"))
    pet_info = optional_json(root, "pet/petinfoconfig.json")
    evolution = optional_json(root, "pet/petevoconfig.json")
    tieba = optional_json(root, "pet/tiebaconfig.json", default=[])
    summons = optional_json(root, "battleconfig/summon_config.json", default=[])
    carries = optional_json(root, "battleconfig/carry_config.json", default=[])
    huan = optional_json(root, "battleconfig/huan_skill_config.json")
    almighty = optional_json(root, "battleconfig/almighty-skill-config.json", default=[])
    pet_data = optional_json(root, "petdata.json/petdata.json", "pet/petdata.json", default=[])
    skin_root = optional_json(root, "changepetskin/changepetskinconfig.json")

    skills = {}
    for skill_id, packed in skill_desc.items():
        name, description, encoding = split_skill(packed)
        if not name and not description:
            continue
        item = {"name": name, "description": description}
        if encoding:
            item["encoding"] = encoding
        animation = animations.get(str(skill_id))
        if isinstance(animation, dict):
            item["animation"] = {key: animation[key] for key in
                                 ("action", "effect", "jump", "shock", "black", "hideother", "movieType", "ef")
                                 if key in animation}
        skills[str(skill_id)] = item

    combos = {}
    for row in combo_root.get("combo", []):
        if not isinstance(row, dict):
            continue
        skill_id = positive_int(row.get("skillId"))
        buff_ids = ids(row.get("buffs"), ",")
        target_ids = ids(row.get("targetSkillIds"), ",")
        if skill_id and buff_ids and len(buff_ids) == len(target_ids):
            combos[str(skill_id)] = [{"buff": buff, "skill": target}
                                      for buff, target in zip(buff_ids, target_ids)]

    entries = {clean_text(row.get("n")): clean_text(row.get("d")) for row in entries_root
               if isinstance(row, dict) and clean_text(row.get("n")) and clean_text(row.get("d"))}
    buffs = {}
    for buff_id, packed in buffer_root.get("buff", {}).items():
        parts = str(packed).split("|")
        if not parts or not clean_text(parts[0]):
            continue
        item = {"name": clean_text(parts[0])}
        if len(parts) > 1 and clean_text(parts[1]):
            item["description"] = clean_text(parts[1])
        if len(parts) > 3 and clean_text(parts[3]):
            item["effect"] = clean_text(parts[3])
        buffs[str(buff_id)] = item

    summon_by_race: dict[str, list[dict]] = {}
    for row in summons if isinstance(summons, list) else []:
        if not isinstance(row, dict):
            continue
        summoners, summoned = ids(row.get("s")), ids(row.get("sed"))
        common = {"description": clean_text(row.get("sdes")), "effect": clean_text(row.get("des"))}
        for race in summoners:
            summon_by_race.setdefault(str(race), []).append(
                {**common, "role": "summoner", "related": summoned})
        for race in summoned:
            summon_by_race.setdefault(str(race), []).append(
                {**common, "role": "summoned", "related": summoners})

    carry_by_race: dict[str, list[dict]] = {}
    for row in carries if isinstance(carries, list) else []:
        if not isinstance(row, dict):
            continue
        carriers, carried = ids(row.get("c")), ids(row.get("ced"))
        description = clean_text(row.get("sdes"))
        for race in carriers:
            carry_by_race.setdefault(str(race), []).append(
                {"role": "carrier", "related": carried, "description": description})
        for race in carried:
            carry_by_race.setdefault(str(race), []).append(
                {"role": "carried", "related": carriers, "description": description})

    skins_by_race: dict[str, list[dict]] = {}
    definitions = skin_root.get("defines", []) if isinstance(skin_root, dict) else []
    for skin in definitions:
        if not isinstance(skin, dict):
            continue
        for available in skin.get("availablePets") or []:
            if not isinstance(available, dict):
                continue
            base, race = positive_int(available.get("raceIdNeed")), positive_int(available.get("raceId"))
            if base and race:
                skins_by_race.setdefault(str(base), []).append({"name": clean_text(skin.get("name")), "raceId": race})

    base_stats = {str(row.get("RaceId")): {"values": str(row.get("BaseStatuses", "")),
                                           "battlePower": int(row.get("BasicZhanDouLi", 0))}
                  for row in pet_data if isinstance(row, dict) and positive_int(row.get("RaceId"))}
    groups = {}
    if isinstance(tieba, list):
        for row in tieba:
            if not isinstance(row, dict):
                continue
            for race in row.get("raceIds", []):
                groups[str(race)] = row.get("raceIds", [])

    almighty_by_race: dict[str, list[dict]] = {}
    for row in almighty if isinstance(almighty, list) else []:
        if not isinstance(row, dict):
            continue
        define_id = positive_int(row.get("id"))
        race = define_id % 10000
        if not race:
            continue
        almighty_by_race.setdefault(str(race), []).append({
            "id": define_id, "heroName": clean_text(row.get("hn")), "heroDescription": clean_text(row.get("hd")),
            "summonName": clean_text(row.get("sn")), "summonDescription": clean_text(row.get("sd")),
            "psychicsName": clean_text(row.get("pn")), "psychicsDescription": clean_text(row.get("pd")),
            "calledRaceId": positive_int(row.get("psci")),
        })

    pets = {}
    for race_id, raw in dictionary.items():
        row = pet_row(raw)
        if not row:
            continue
        race = str(positive_int(row["raceId"]) or race_id)
        slots = {name: positive_int(row[field]) for name, field in SKILL_SLOTS.items()}
        pet = {
            "name": clean_text(row["orgName"]), "attributes": str(row["attributes"] or ""),
            "jobs": str(row["jobs"] or ""), "signs": clean_text(row["signs"]),
            "strategy": clean_text(row["strategy"]), "position": clean_text(row["position"]),
            "quality": clean_text(row["quality"]), "slots": slots,
            "skillEntries": [value for value in str(row["skillEntries"] or "").split("#") if value],
            "qiYunType": positive_int(row["qiYunType"]),
        }
        pet["traits"] = [tag for tag in ("天觉者", "吞噬技", "携同技", "寰力量", "异形族", "双英雄技")
                         if tag in pet["signs"]]
        if row["starGodSkillName"]:
            pet["starGodSkill"] = {"name": clean_text(row["starGodSkillName"]),
                                   "description": clean_text(row["starGodSkillDesc"])}
        if race in base_stats:
            pet["baseStats"] = base_stats[race]
        if race in relations:
            pet["relations"] = relations[race]
        if race in summon_by_race:
            pet["summons"] = summon_by_race[race]
        if race in carry_by_race:
            pet["carry"] = carry_by_race[race]
        if race in skins_by_race:
            pet["skins"] = skins_by_race[race]
        if race in groups:
            pet["forms"] = groups[race]
        almighty_rows = list(almighty_by_race.get(race, []))
        for grouped_race in groups.get(race, []):
            for value in almighty_by_race.get(str(grouped_race), []):
                if value not in almighty_rows:
                    almighty_rows.append(value)
        if almighty_rows:
            pet["almighty"] = almighty_rows
        info = pet_info.get(race)
        if isinstance(info, list) and len(info) > 6:
            pet["officialRole"] = clean_text(info[4] or info[3])
            pet["hexagon"] = str(info[6])
        evo = evolution.get(race)
        if isinstance(evo, list) and len(evo) > 3 and positive_int(evo[3]) != int(race):
            pet["evolutionFrom"] = positive_int(evo[3])
        huan_rows = [value for key, value in huan.items() if str(key).endswith(race)]
        if huan_rows:
            pet["huan"] = [clean_text(value) for value in huan_rows[0] if clean_text(value)]
        pets[race] = pet

    compact_transform = {}
    for skill_id, value in transform.items():
        if isinstance(value, list) and len(value) >= 7:
            compact_transform[str(skill_id)] = {
                "name": clean_text(value[1]), "description": clean_text(value[2]),
                "teamDescription": clean_text(value[3]), "targets": value[4], "effects": value[5],
                "type": value[6],
            }

    return {
        "schema": 1,
        "source": source or {"kind": "aoqi-official-h5-skill-data"},
        "pets": pets,
        "skills": skills,
        "entries": entries,
        "buffs": buffs,
        "combos": combos,
        "transformSkills": compact_transform,
        "heroSkills": hero,
        "psychicsSkills": psychics,
        "lingchuRules": lingchu,
        "shenyunRules": shenyun,
        "almightySkills": almighty,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-version", default="")
    args = parser.parse_args()
    source = {"kind": "aoqi-official-h5-skill-data", "version": args.source_version}
    catalog = build_catalog(args.source_root, source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
