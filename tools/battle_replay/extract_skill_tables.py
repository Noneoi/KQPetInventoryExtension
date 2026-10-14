"""Read official skill tables without executing ActionScript or inventing relations.

``skills`` is the Flash lookup used by the replay. H5 values remain a separately
identified comparison source. ``lastSkills`` contains only explicit XML ``last``
rows, not the lazy copyDefine cache populated by the running Flash client.

The runtime projection follows SkillDefine.parseDefine/copyDefine/wrapDescOnly in
library/interfaces 20260930468207480. In particular, automatic-chess IDs may copy
an existing animation, but ordinary numeric prefixes do not establish aliases.
These tables describe client presentation; they do not identify Buff appliers.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from typing import Any
import xml.etree.ElementTree as ET

from extract_skill_embed_effect import extract_skill_embed_effect


SKILL_TYPES = ("近身", "远程", "物攻", "魔攻", "超杀技", "伤害", "恢复", "召唤技",
               "超必杀技", "超物攻", "超魔攻")
# H5 aqlib module 467 additionally names the functionalType values 11..13.
H5_SKILL_TYPES = SKILL_TYPES + ("召唤", "治疗", "BuffOnly")
H5_COLORS = ("#00FFFF", "#FF0000", "#FF0001", "#FF0002", "#FF0003", "#FF0004",
             "#FF0005", "#FF0006", "#FF0007", "#FF0008", "#FFCC00")
TEXT_TABLES = ("SkillConfigSimpleAC", "SkillConfigSimple3000", "SkillConfigSimple6000",
               "SkillConfigSimple9999")
INTERFACE_PACKAGE_SHA256 = "2a7b34e20b998b40e38fea17050fde581c10ae22b6f1750632709a1dc8ad6872"
DIRECT_DIR = Path("battleconfig/scripts/mmo/battleconfig/data/direct")
COPY_RUNTIME_FIELDS = ("fullEffect", "swfId", "actionFrame", "pos", "type", "jumpFrame",
                       "type2", "effectFrame", "shockScreen", "blackScreen", "isBossSkill",
                       "isHideOtherAvatar", "hasLastEffect", "isCentral", "playOrderType",
                       "isSpine")
PLAYBACK_FIELDS = ("action", "pos", "jump", "effect", "shock", "black", "hideother",
                   "last", "central", "playOrderType", "ef", "spine", "swf")


@dataclass
class LiteralObject:
    # Pairs, rather than a dict, preserve duplicate keys and their source offsets.
    pairs: list[tuple[str, Any, int]]


class LiteralParser:
    """A deliberately small AS/JS data-literal parser: never eval/import code."""

    def __init__(self, text: str, pos: int = 0):
        self.text, self.pos = text, pos

    def error(self, message: str):
        raise ValueError(f"{message} at character {self.pos}")

    def space(self):
        while self.pos < len(self.text):
            if self.text[self.pos].isspace():
                self.pos += 1
            elif self.text.startswith("//", self.pos):
                end = self.text.find("\n", self.pos + 2)
                self.pos = len(self.text) if end < 0 else end + 1
            elif self.text.startswith("/*", self.pos):
                end = self.text.find("*/", self.pos + 2)
                if end < 0:
                    self.error("Unterminated comment")
                self.pos = end + 2
            else:
                break

    def string(self) -> str:
        quote = self.text[self.pos]
        self.pos += 1
        result = []
        escapes = {"b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t",
                   "v": "\v", "0": "\0", "\\": "\\", "'": "'", '"': '"', "/": "/"}
        while self.pos < len(self.text):
            char = self.text[self.pos]
            self.pos += 1
            if char == quote:
                # AS strings use UTF-16; join escaped surrogate pairs without
                # losing standalone code units present in the original literal.
                return "".join(result).encode("utf-16", "surrogatepass").decode("utf-16", "surrogatepass")
            if char != "\\":
                if char in "\r\n":
                    self.error("Unescaped newline in string")
                result.append(char)
                continue
            if self.pos >= len(self.text):
                self.error("Unterminated string escape")
            escape = self.text[self.pos]
            self.pos += 1
            if escape in escapes:
                if escape == "0" and self.pos < len(self.text) and self.text[self.pos].isdigit():
                    self.error("Octal escape is unsupported")
                result.append(escapes[escape])
            elif escape in ("u", "x"):
                length = 4 if escape == "u" else 2
                digits = self.text[self.pos:self.pos + length]
                if len(digits) != length or not re.fullmatch(r"[0-9a-fA-F]+", digits):
                    self.error("Invalid hexadecimal string escape")
                result.append(chr(int(digits, 16)))
                self.pos += length
            else:
                self.error(f"Unsupported string escape {escape!r}")
        self.error("Unterminated string")

    def value(self):
        self.space()
        if self.pos >= len(self.text):
            self.error("Expected literal")
        char = self.text[self.pos]
        if char in "\"'":
            return self.string()
        if char == "{":
            self.pos += 1
            pairs = []
            self.space()
            while self.pos < len(self.text) and self.text[self.pos] != "}":
                offset = self.pos
                if self.text[self.pos] not in "\"'":
                    self.error("Object keys must be quoted data strings")
                key = self.string()
                self.space()
                if self.text[self.pos:self.pos + 1] != ":":
                    self.error("Expected colon")
                self.pos += 1
                pairs.append((key, self.value(), offset))
                self.space()
                if self.text[self.pos:self.pos + 1] != ",":
                    break
                self.pos += 1
                self.space()
            if self.text[self.pos:self.pos + 1] != "}":
                self.error("Expected closing object brace")
            self.pos += 1
            return LiteralObject(pairs)
        if char == "[":
            self.pos += 1
            values = []
            self.space()
            while self.pos < len(self.text) and self.text[self.pos] != "]":
                values.append(self.value())
                self.space()
                if self.text[self.pos:self.pos + 1] != ",":
                    break
                self.pos += 1
                self.space()
            if self.text[self.pos:self.pos + 1] != "]":
                self.error("Expected closing array bracket")
            self.pos += 1
            return values
        match = re.match(r"(?:-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null)(?![\w.$])",
                         self.text[self.pos:])
        if not match:
            self.error("Expected a data literal; executable expressions are unsupported")
        self.pos += len(match[0])
        return json.loads(match[0])


def _plain(value):
    if isinstance(value, LiteralObject):
        return {key: _plain(item) for key, item, _ in value.pairs}
    if isinstance(value, list):
        return [_plain(item) for item in value]
    return value


def parse_map(text: str) -> list[dict]:
    match = re.search(r"\bpublic\s+static\s+const\s+MAP\s*:\s*Object\s*=", text)
    if not match:
        raise ValueError("Missing public static MAP:Object literal")
    parser = LiteralParser(text, match.end())
    parsed = parser.value()
    parser.space()
    if not isinstance(parsed, LiteralObject) or text[parser.pos:parser.pos + 1] != ";":
        raise ValueError("MAP must be a standalone object literal followed by a semicolon")
    result = []
    for index, (key, value, offset) in enumerate(parsed.pairs):
        if not isinstance(value, LiteralObject):
            raise ValueError(f"MAP[{key!r}] must be a data object")
        result.append({"id": key, "value": _plain(value), "row": index,
                       "line": text.count("\n", 0, offset) + 1,
                       "fieldPairs": [[field, _plain(item)] for field, item, _ in value.pairs]})
    return result


def parse_adapter_rules(text: str) -> dict:
    # Refuse a changed routing algorithm instead of silently applying ID heuristics.
    compact = re.sub(r"\s+", "", text)
    start = compact.find("var_loc2_:String=param1.toString();")
    end = compact.find("if(ConfigReader.instance.debug)", start)
    expected = (
        "var_loc2_:String=param1.toString();"
        "if(SkillConfigSimpleAC.MAP.hasOwnProperty(_loc2_)){returnSkillConfigSimpleAC.MAP[_loc2_];}"
        "var_loc3_:int=param1;"
        "if(_loc3_>100000000){_loc3_=Math.floor(_loc3_/1000);}"
        "elseif(_loc3_>10000000){_loc3_=Math.floor(_loc3_/100);}"
        "elseif(_loc3_>1000000){_loc3_=Math.floor(_loc3_/10);}"
        "_loc3_%=10000;"
        "if(_loc3_<=3000){returnSkillConfigSimple3000.MAP[_loc2_];}"
        "if(_loc3_<=6000){returnSkillConfigSimple6000.MAP[_loc2_];}"
        "if(_loc3_<=9999){returnSkillConfigSimple9999.MAP[_loc2_];}"
    )
    if start < 0 or end < 0 or compact[start:end] != expected:
        raise ValueError("Unreviewed SkillConfigAdapter routing; retain/review the new official source first")
    return {"priorityTable": TEXT_TABLES[0], "key": "original decimal skill ID",
            "bucketDivisions": [[100000000, 1000], [10000000, 100], [1000000, 10]],
            "bucketModulo": 10000, "bucketUpperBounds": [[3000, TEXT_TABLES[1]],
            [6000, TEXT_TABLES[2]], [9999, TEXT_TABLES[3]]]}


def text_table_for(skill_id: str, tables: dict) -> str:
    if skill_id in tables[TEXT_TABLES[0]]:
        return TEXT_TABLES[0]
    number = int(skill_id)
    for threshold, divisor in ((100000000, 1000), (10000000, 100), (1000000, 10)):
        if number > threshold:
            number //= divisor
            break
    number %= 10000
    return TEXT_TABLES[1] if number <= 3000 else TEXT_TABLES[2] if number <= 6000 else TEXT_TABLES[3]


def original_skill_id(skill_id: str) -> str:
    if len(skill_id) != 7 or skill_id[0] not in "12" or skill_id[1] not in "123":
        return skill_id
    number = int(skill_id)
    return str(number // 1000000 * 100000 + number % 100000)


def decode_h5(packed: str) -> dict:
    parts = packed.split("|")
    result = {"raw": packed, "fields": parts}
    if len(parts) < 3:
        return result
    result.update(name=parts[0], description=parts[1], encoding=parts[2])
    # Official StringUtil.parseLetters: A..Z => 1..26, a..z => -1..-26,
    # every other character => 0. The consumer requires exactly five values.
    decoded = [ord(c) - 64 if "A" <= c <= "Z" else -(ord(c) - 96) if "a" <= c <= "z" else 0
               for c in parts[2]]
    result["decoded"] = decoded
    description = parts[1]
    for index, color in enumerate(H5_COLORS):
        marker = "0" if index == 0 else chr(64 + index)
        description = description.replace("$c" + marker, "<font color='" + color + "'>")
    result["renderedDescription"] = description.replace("c$", "</font>")
    return result


def _int(value) -> int:
    return int(value) if value not in (None, "") else 0


def _type(value) -> int:
    return SKILL_TYPES.index(value) if value in SKILL_TYPES else -1


def _h5_type(value) -> int:
    return H5_SKILL_TYPES.index(value) if value in H5_SKILL_TYPES else -1


def _visible_text(value: str) -> str:
    class TextReader(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.parts = []

        def handle_data(self, data):
            self.parts.append(data)

        def handle_starttag(self, tag, attrs):
            if tag == "br":
                self.parts.append("\n")

    reader = TextReader()
    reader.feed(value)
    reader.close()
    return "".join(reader.parts)


def runtime_define(skill_id: str, row: dict | None, desc: dict, name: str, description: str) -> dict:
    # Defaults are the declared fields of SkillDefine. wrapDescOnly sets only
    # id/name/description/isBossSkill, not action/type/functionality.
    result = {"id": int(skill_id), "name": name, "description": description,
              "swfId": 0, "actionFrame": 0, "pos": 5, "type": 0, "type2": 0,
              "jumpFrame": -1, "effectFrame": -1, "shockScreen": False,
              "blackScreen": False, "isBossSkill": _int(desc.get("bossSkill")) == 1,
              "isHideOtherAvatar": False, "hasLastEffect": False, "isCentral": False,
              "fullEffect": False, "playOrderType": 0, "functionalType": None, "isSpine": False}
    if row is None:
        return result
    attack_type = _type(desc.get("attackType", ""))
    base_type = _type(desc.get("skillBaseType", ""))
    result.update(swfId=_int(row.get("swf", skill_id)), actionFrame=_int(row.get("action")),
                  pos=_int(row.get("pos")), type=attack_type,
                  jumpFrame=_int(row.get("jump")) if attack_type == 0 else -1,
                  type2=base_type if base_type in (4, 7, 8) else _type(desc.get("skillType", "")),
                  effectFrame=_int(row.get("effect")), shockScreen=_int(row.get("shock")) == 1,
                  blackScreen=_int(row.get("black")) == 1,
                  isHideOtherAvatar=_int(row.get("hideother")) == 1,
                  hasLastEffect=_int(row.get("last")) == 1, isCentral=_int(row.get("central")) == 1,
                  fullEffect=_int(row.get("ef")) == 1, playOrderType=_int(row.get("playOrderType")),
                  functionalType=desc.get("ft", ""), isSpine=_int(row.get("spine")) == 1)
    return result


def _runtime_errors(runtime: dict) -> list[str]:
    errors = []
    if runtime["actionFrame"] == 0:
        errors.append("actionFrame == 0")
    if not 1 <= runtime["pos"] <= 10:
        errors.append("pos outside [1, 10]")
    if runtime["effectFrame"] == 0 and not runtime["isSpine"]:
        errors.append("effectFrame == 0 without spine")
    if runtime["effectFrame"] == runtime["jumpFrame"]:
        errors.append("effectFrame == jumpFrame")
    return errors


def _available(runtime: dict) -> bool:
    return runtime["actionFrame"] > 0 and runtime["pos"] > 0 and (runtime["effectFrame"] > 0 or runtime["isSpine"])


def _file_info(path: Path, source: Path) -> dict:
    return {"path": path.relative_to(source).as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def _xml_table(source: Path, name: str) -> tuple[ET.Element, dict]:
    paths = list((source / "xml").glob("*." + name + "__*.xml"))
    if len(paths) != 1:
        raise ValueError(f"Expected exactly one {name} XML, found {len(paths)}")
    path = paths[0]
    raw = path.read_text(encoding="utf-8-sig")
    return ET.fromstring(raw), {"source": _file_info(path, source), "rawXml": raw}


def _indexed_rows(rows: list[dict], table: str, duplicates: list[dict]) -> dict:
    result = {}
    for row in rows:
        key = row["id"]
        if key in result:
            duplicates.append({"table": table, "id": key, "previousRow": result[key]["row"],
                               "winningRow": row["row"]})
        result[key] = row
    return result


def _xml_rows(elements) -> list[dict]:
    result = []
    for index, element in enumerate(elements):
        key = str(_int(element.get("id")))
        result.append({"id": key, "row": index, "attributes": dict(element.attrib),
                       "xml": ET.tostring(element, encoding="unicode")})
    return result


def extract_skill_tables(source: Path) -> dict:
    source = Path(source)
    # The copied runtime fields below are tied to this inspected official class.
    interface_package = source / "library_interfaces.swf"
    if not interface_package.is_file() or hashlib.sha256(interface_package.read_bytes()).hexdigest() != INTERFACE_PACKAGE_SHA256:
        raise ValueError("Unreviewed/missing library_interfaces.swf: recheck SkillDefine runtime semantics first")
    raw_tables, report = {}, {"duplicates": [], "missing": {}, "differences": {},
                              "invalidPlaybackRows": [], "normalizedXmlIds": []}
    adapter_path = source / DIRECT_DIR / "SkillConfigAdapter.as"
    adapter_text = adapter_path.read_text(encoding="utf-8-sig")
    rules = parse_adapter_rules(adapter_text)
    raw_tables["skillConfigAdapter"] = {"source": _file_info(adapter_path, source),
                                         "rawActionScript": adapter_text, "rules": rules}
    text_tables = {}
    raw_tables["flashTextMaps"] = {}
    for table in TEXT_TABLES:
        path = source / DIRECT_DIR / (table + ".as")
        raw = path.read_text(encoding="utf-8-sig")
        rows = parse_map(raw)
        raw_tables["flashTextMaps"][table] = {"source": _file_info(path, source),
                                             "rawActionScript": raw, "rows": rows}
        text_tables[table] = _indexed_rows(rows, table, report["duplicates"])
        for row in rows:
            counts = Counter(key for key, _ in row["fieldPairs"])
            for key, count in counts.items():
                if count > 1:
                    report["duplicates"].append({"table": table, "id": row["id"],
                                                  "row": row["row"], "field": key, "count": count})

    desc_root, desc_raw = _xml_table(source, "SkillDescConfigXML")
    desc_raw["rows"] = _xml_rows(desc_root.findall("./pet/s"))
    raw_tables["flashSkillDesc"] = desc_raw
    descriptions = _indexed_rows(desc_raw["rows"], "SkillDescConfigXML.pet.s", report["duplicates"])
    config_root, config_raw = _xml_table(source, "SkillConfigXML")
    config_raw["s"] = _xml_rows(config_root.findall("./s"))
    config_raw["last"] = _xml_rows(config_root.findall("./last"))
    raw_tables["flashSkillConfig"] = config_raw
    for table, rows in (("SkillDescConfigXML.pet.s", desc_raw["rows"]),
                        ("SkillConfigXML.s", config_raw["s"]), ("SkillConfigXML.last", config_raw["last"])):
        for row in rows:
            if row["attributes"].get("id") != row["id"]:
                report["normalizedXmlIds"].append({"table": table, "row": row["row"],
                                                   "rawId": row["attributes"].get("id"), "id": row["id"]})

    h5_values = {}
    for name, output_key in (("skill_desc_config.json", "h5SkillDesc"), ("skill_config.json", "h5SkillConfig")):
        path = source / "h5-tables" / name
        if not path.is_file():
            path = source / name
        if not path.is_file():
            report["missing"][name] = True
            continue
        # Keep the full raw JSON as well, so even repeated JSON keys are retained.
        raw = path.read_text(encoding="utf-8-sig")
        pairs = json.loads(raw, object_pairs_hook=lambda entries: LiteralObject(
            [(key, item, index) for index, (key, item) in enumerate(entries)]))
        value = _plain(pairs)
        if not isinstance(value, dict):
            raise ValueError(f"Expected a JSON object in {name}")
        seen = {}
        for key, _, row in pairs.pairs:
            if key in seen:
                report["duplicates"].append({"table": name, "id": key, "previousRow": seen[key],
                                              "winningRow": row})
            seen[key] = row
        raw_tables[output_key] = {"source": _file_info(path, source), "rawJson": raw, "data": value}
        if output_key == "h5SkillDesc":
            h5_values = value

    known_ids = set(descriptions) | set(h5_values)
    for table in text_tables.values():
        known_ids.update(table)
    for rows in (config_raw["s"], config_raw["last"]):
        known_ids.update(row["id"] for row in rows)
    if any(not re.fullmatch(r"0|[1-9]\d*", key) or int(key) > 2147483647 for key in known_ids):
        raise ValueError("Noncanonical/out-of-range skill ID: review official int ID semantics before flattening")

    def text_for(key):
        table = text_table_for(key, text_tables)
        row = text_tables[table].get(key)
        return table, row

    def names_for(key):
        _, row = text_for(key)
        fields = row["value"] if row else {}
        return fields.get("name", ""), fields.get("desc", "")

    def desc_for(key):
        row = descriptions.get(key)
        return row["attributes"] if row else {}

    configs = {}
    lasts = {}
    for tag, destination in (("s", configs), ("last", lasts)):
        _indexed_rows(config_raw[tag], "SkillConfigXML." + tag, report["duplicates"])
        for row in config_raw[tag]:
            key = row["id"]
            runtime = runtime_define(key, row["attributes"], desc_for(key), *names_for(key))
            errors = _runtime_errors(runtime)
            if errors:
                report["invalidPlaybackRows"].append({"table": tag, "id": key, "row": row["row"], "errors": errors})
                continue  # The official per-row try/catch does not insert invalid definitions.
            destination[key] = {**row, "runtime": runtime}

    def canonical(key: str, explicit: dict | None = None, last: bool = False) -> dict:
        table, text_row = text_for(key)
        own_desc = desc_for(key)
        name, description = names_for(key)
        item = dict(own_desc)
        item.update(id=key, name=name, description=description, descriptionAttributes=dict(own_desc))
        if text_row:
            item["flashText"] = text_row["value"]
            item["textSource"] = {"table": table, "row": text_row["row"], "line": text_row["line"],
                                  **raw_tables["flashTextMaps"][table]["source"]}
        else:
            item["textSource"] = {"table": table, "missing": True}
        row = explicit if last else configs.get(key)
        resolution = "direct-last" if last else "direct"
        if row is None:
            original = original_skill_id(key)
            row = configs.get(original) if original != key else None
            resolution = "copyDefine" if row else "description-only"
        if row:
            if resolution == "copyDefine":
                origin_desc = desc_for(row["id"])
                # copyDefine inherits type/type2/boss from the origin; ft and text
                # belong to the requested ID. Retain own metadata separately.
                for field in ("attackType", "skillBaseType", "skillType", "bossSkill"):
                    item[field] = origin_desc.get(field, "")
                for field in PLAYBACK_FIELDS:
                    if field in row["attributes"]:
                        item[field] = row["attributes"][field]
                runtime = runtime_define(key, None, own_desc, name, description)
                runtime.update({field: row["runtime"][field] for field in COPY_RUNTIME_FIELDS})
                runtime["functionalType"] = own_desc.get("ft", "")
                # swfId defaults to the origin ID, not the requested derived ID.
                item["swf"] = str(runtime["swfId"])
            else:
                item.update(row["attributes"])
                runtime = row["runtime"].copy()
            # XML ID fields are parsed as int by the client; raw attributes stay
            # in rawTables. Text getters always route through the Flash MAP.
            item.update(id=key, name=name, description=description)
            item["configSourceId"] = row["id"]
            item["configSource"] = {"table": "SkillConfigXML.last" if last else "SkillConfigXML.s",
                                    "row": row["row"], **config_raw["source"]}
        else:
            runtime = runtime_define(key, None, own_desc, name, description)
        item["configResolution"] = resolution
        item["runtimeDefine"] = runtime
        item["playbackAvailable"] = _available(runtime)
        if key in h5_values and isinstance(h5_values[key], str):
            item["h5"] = decode_h5(h5_values[key])
        return item

    skills = {key: canonical(key) for key in sorted(known_ids, key=int)}
    last_skills = {key: canonical(key, row, True) for key, row in lasts.items()}
    report["missing"].update(
        flashText=[key for key, value in skills.items() if value["textSource"].get("missing")],
        flashDescriptionMetadata=[key for key in skills if key not in descriptions],
        h5Text=[key for key in skills if key not in h5_values],
        normalPlayback=[key for key, value in skills.items() if value["configResolution"] == "description-only"],
        normalLastFlagWithoutExplicitLast=[key for key, value in configs.items()
                                          if value["runtime"]["hasLastEffect"] and key not in lasts])
    report["unreachableFlashTextRows"] = [
        {"table": table, "id": key, "selectedTable": text_table_for(key, text_tables)}
        for table, rows in text_tables.items() for key in rows if text_table_for(key, text_tables) != table]
    raw_diff, rendered_diff, visible_diff, metadata_diff, invalid_h5 = [], [], [], [], []
    for key, item in skills.items():
        h5 = item.get("h5")
        if not h5:
            continue
        if len(h5["fields"]) != 3 or len(h5.get("decoded", [])) != 5:
            invalid_h5.append(key)
            continue
        if item["textSource"].get("missing"):
            continue
        if (item["name"], item["description"]) != (h5["name"], h5["description"]):
            raw_diff.append(key)
        if (item["name"], item["description"]) != (h5["name"], h5["renderedDescription"]):
            rendered_diff.append(key)
        if (item["name"], _visible_text(item["description"])) != (h5["name"], _visible_text(h5["renderedDescription"])):
            visible_diff.append(key)
        desc = item["descriptionAttributes"]
        if desc:
            values = [_h5_type(desc.get("attackType")), _h5_type(desc.get("skillBaseType")),
                      _h5_type(desc.get("skillType")), _int(desc.get("bossSkill")), _h5_type(desc.get("ft"))]
            if values != h5["decoded"]:
                metadata_diff.append({"id": key, "flash": values, "h5": h5["decoded"]})
    report["differences"] = {"rawTextIds": raw_diff, "renderedTextIds": rendered_diff,
                              "visibleTextIds": visible_diff, "metadata": metadata_diff,
                              "invalidH5PackedIds": invalid_h5}
    report["counts"] = {"skills": len(skills), "lastSkills": len(last_skills),
                         "flashTextRows": sum(len(value["rows"]) for value in raw_tables["flashTextMaps"].values()),
                         "flashDescriptionRows": len(desc_raw["rows"]), "normalRows": len(config_raw["s"]),
                         "lastRows": len(config_raw["last"]), "h5DescriptionRows": len(h5_values),
                         "configResolutions": dict(Counter(value["configResolution"] for value in skills.values())),
                         "duplicateEntries": len(report["duplicates"]), "rawTextDifferences": len(raw_diff),
                         "renderedTextDifferences": len(rendered_diff), "visibleTextDifferences": len(visible_diff),
                         "metadataDifferences": len(metadata_diff)}
    report["runtimeRules"] = {"interfacePackageSha256": INTERFACE_PACKAGE_SHA256,
                              "source": "mmo/interfaces/battleconfig/define/SkillDefine.as",
                              "copyFields": list(COPY_RUNTIME_FIELDS),
                              "lastSkillsScope": "explicit XML last rows; excludes runtime lazy copyDefine cache",
                              "otherSkillNamespacesMerged": False,
                              "buffApplierOrGrantRelationsInferred": False}
    embed_raw, embed_report = extract_skill_embed_effect(source, set(skills))
    raw_tables["skillEmbedEffect"] = embed_raw
    report["skillEmbedEffect"] = embed_report
    return {"skills": skills, "lastSkills": last_skills, "rawTables": raw_tables, "report": report}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()
    result = extract_skill_tables(args.source)
    if args.output:
        args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(json.dumps(result["report"]["counts"], ensure_ascii=False))
