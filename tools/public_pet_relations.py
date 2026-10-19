"""Project official final summon/carry pairs for the pet-detail workbench.

This has no account or replay dependencies. Carry expansion follows the client
PetRaceUtil: reset skin -> multiple forms -> shown skins, once in that order.
"""
from __future__ import annotations

import re


def integer(value, minimum=1):
    if isinstance(value, bool) or not re.fullmatch(r"-?\d+", str(value)):
        raise ValueError("invalid official relation integer")
    result = int(value)
    if not minimum <= result <= 2147483647:
        raise ValueError("official relation integer out of range")
    return result


def race_list(value):
    if not isinstance(value, str):
        raise ValueError("official relation race list is missing")
    return list(dict.fromkeys(integer(token) for token in value.split("#") if token))


class SkinRelations:
    def __init__(self, config):
        if not isinstance(config, dict) or not isinstance(config.get("defines"), list):
            raise ValueError("official skin relation definitions are missing")
        self.rows, self.by_race, self.by_id, self.xgroups = [], {}, {}, {}
        for raw in config["defines"]:
            if not isinstance(raw, dict) or not isinstance(raw.get("show"), bool) or "fuse" not in raw or not isinstance(raw.get("availablePets"), list):
                raise ValueError("incomplete official skin relation definition")
            identifier = integer(raw.get("id"))
            if identifier in self.by_id:
                raise ValueError("duplicate official skin definition id")
            pairs = [(integer(row.get("raceId")), integer(row.get("raceIdNeed"), 0 if raw["fuse"] is not None else 1))
                     for row in raw["availablePets"] if isinstance(row, dict)]
            if len(pairs) != len(raw["availablePets"]):
                raise ValueError("invalid official skin race mapping")
            row = {"id": identifier, "show": raw["show"], "fusion": raw["fuse"] is not None,
                   "pairs": pairs, "dependent": None}
            dependent = raw.get("xChangeableInfo")
            if dependent is not None:
                if not isinstance(dependent, str) or len(dependent.split("@")) != 2:
                    raise ValueError("invalid official changeable skin relation")
                row["dependent"] = integer(dependent.split("@")[1])
            self.rows.append(row)
            self.by_id[identifier] = row
            for race, _ in pairs:
                self.by_race[race] = row  # official valueOfRaceId: later mapping wins
        for row in self.rows:
            parent = row["dependent"]
            if parent is None or not row["show"]:
                continue
            if parent not in self.by_id:
                raise ValueError("official changeable skin references an unknown parent")
            group = self.xgroups.setdefault(parent, [self.by_id[parent]])
            if row not in group:
                group.append(row)
                self.xgroups[row["id"]] = group

    @staticmethod
    def find_race(row, base):
        return next((race for race, original in row["pairs"] if original == base), None)

    def original(self, race):
        row = self.by_race.get(race)
        if row is not None:
            if row["fusion"]:
                return None
            return next((base for item, base in row["pairs"] if item == race), None)
        if any(not row["fusion"] and self.find_race(row, race) is not None for row in self.rows):
            return race
        return None

    def switch_races(self, race):
        base = self.original(race)
        if base is None:
            return []
        result = []
        for row in self.rows:
            if row["show"]:
                target = self.find_race(row, base)
                if target is not None:
                    result.append(target)
                # Official getXChangeable filters first, then skips index zero.
                forms = [self.find_race(item, base) for item in self.xgroups.get(row["id"], [])]
                result.extend([item for item in forms if item is not None][1:])
            elif any(item == race for item, _ in row["pairs"]):
                return []
        return [base, *result] if result else []


def _group(available=False):
    return {"available": available, "bySourceRaceId": {}}


def build_relation_rules(summons, hidden_summons, carries, skin_config, multiple_forms, pets, clean_text, *, include_text=False):
    """None means unavailable; a verified empty list means a known empty set."""
    result = {"schema": 1, "summon": _group(), "carry": _group()}
    skins = SkinRelations(skin_config) if skin_config is not None else None
    if not isinstance(pets, dict):
        raise ValueError("official relation pet dictionary is missing")

    def target_identity(target):
        own = pets.get(str(target), {})
        base = skins.original(target) if skins is not None else None
        base = base or target
        original = pets.get(str(base), {})
        if not original or not isinstance(original.get("signs"), str) or "皮肤" in original["signs"].split(","):
            return target, ""
        # No mapping evidence must not call a skin's display title its original name.
        if base == target and "皮肤" in str(own.get("signs", "")).split(","):
            return target, ""
        return base, clean_text(original.get("orgName"))

    def publish(pairs, group):
        output = result[group]["bySourceRaceId"]
        for (source, target), data in sorted(pairs.items()):
            base, name = target_identity(target)
            signs = pets.get(str(target), {}).get("signs")
            listed = isinstance(signs, str) and "皮肤" not in signs.split(",")
            listed = listed and (group == "carry" or data["isShow"])
            fields = data if include_text else {"type": data["type"], "isShow": data["isShow"]}
            output.setdefault(str(source), []).append({"targetRaceId": target, "baseRaceId": base,
                "originalName": name, "listedWithoutOwnership": listed, **fields})
        result[group]["available"] = True

    if summons is not None and hidden_summons is not None and skins is not None:
        if not isinstance(summons, list) or not isinstance(hidden_summons, list):
            raise ValueError("official summon relation sheets are invalid")
        pairs = {}
        for raw in [*summons, *hidden_summons]:
            if not isinstance(raw, dict):
                raise ValueError("invalid official summon relation row")
            sources, targets = race_list(raw.get("s")), race_list(raw.get("sed"))
            if not sources or not targets:
                raise ValueError("empty official summon relation row")
            show = integer(raw.get("isShow", 0), 0) == 1
            data = {"type": 0, "isShow": show, "description": clean_text(raw.get("sdes")),
                    "effect": clean_text(raw.get("des"))}
            for source in sources:
                for target in targets:
                    pairs[(source, target)] = data
        publish(pairs, "summon")

    if carries is not None and skins is not None and multiple_forms is not None:
        if not isinstance(carries, list) or not isinstance(multiple_forms, list):
            raise ValueError("official carry expansion sheets are invalid")
        by_form = {}
        for raw in multiple_forms:
            if not isinstance(raw, list) or not raw:
                raise ValueError("invalid official multiple-form group")
            members = [integer(value) for value in raw]
            for member in members:
                by_form.setdefault(member, members)  # official first containing group

        def expand(seed):
            choices = skins.switch_races(seed)
            base = choices[0] if choices else seed
            members = list(dict.fromkeys([base, *by_form.get(base, [])]))
            expanded = list(members)
            for member in members:
                for target in skins.switch_races(member):
                    if target not in expanded:
                        expanded.append(target)
            return expanded

        pairs = {}
        for raw in carries:
            if not isinstance(raw, dict):
                raise ValueError("invalid official carry relation row")
            for field in ("c", "ced"):
                if not isinstance(raw.get(field), str) or not raw[field].split("#")[0]:
                    raise ValueError("official carry seed is missing")
            sources = expand(integer(raw["c"].split("#")[0]))
            targets = expand(integer(raw["ced"].split("#")[0]))
            # Type is indispensable to distinguish gods, summoned beasts and books.
            kind = integer(raw.get("type"), -1)
            data = {"type": kind, "isShow": True, "description": clean_text(raw.get("sdes")), "effect": ""}
            for source in sources:
                for target in targets:
                    pairs[(source, target)] = data
        publish(pairs, "carry")
    return result


def legacy_relation_views(rules):
    """Keep existing skill text cards, but derive them from final exact pairs."""
    output = {"summon": {}, "carry": {}}
    for group, roles in (("summon", ("summoner", "summoned")), ("carry", ("carrier", "carried"))):
        for source, rows in rules[group]["bySourceRaceId"].items():
            for row in rows:
                target = str(row["targetRaceId"])
                common = {"description": row["description"]}
                if group == "summon":
                    common["effect"] = row["effect"]
                for race, role, related in ((source, roles[0], int(target)), (target, roles[1], int(source))):
                    entries = output[group].setdefault(race, [])
                    entry = next((item for item in entries if item["role"] == role and
                                  all(item.get(key) == value for key, value in common.items())), None)
                    if entry is None:
                        entry = {**common, "role": role, "related": []}
                        entries.append(entry)
                    if related not in entry["related"]:
                        entry["related"].append(related)
    return output["summon"], output["carry"]
