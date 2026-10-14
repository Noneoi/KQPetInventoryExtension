"""Join official Buff tables by Buff ID without replacing Flash state fields.

``extract_buff_tables(source)`` reads fetch_reference.py's extracted directory.
The flat ``buffs`` mapping remains suitable for the Flash replay state machine;
``rawTables`` retains every source row, H5 pipe string, and decoded H5 variant.
No client code is executed and no display resource ID is used as a text key.

Flash: BufferConfigXML.parseConfig joins BufferDescConfigXML with row.@id.
BufferDefine.parseDefine obtains render/state fields from config and text from
the descriptor. Config.nm is an explicitly labelled replay name fallback.
H5: battleconfig.js module 13 reads buffer_config.buff[id]; aqlib.js module 471
decodes the positional pipe string. Its pfmap is a separate full-power map.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import html
import json
from pathlib import Path
import re
from typing import Any
import xml.etree.ElementTree as ET


H5_COLORS = (
    "#FF0000", "#FFCC00", "#99CC00", "#0098FF", "#FF3300", "#00FFFF",
    "#D4D4D4", "#99CC01", "#99CC02", "#99CC03", "#99CC04", "#99CC05",
    "#99CC06", "#99CC07", "#99CC08",
)
H5_PIPE_FIELDS = (
    "nm", "mess", "did", "beffect", "bhs", "pos", "once", "df", "hd",
    "buffWaitType", "buffWaitEndFrame", "need2HidePlayerUi", "loc", "spineFlag",
)
_H5_NUMBERS = {
    "did": (2, "645"), "pos": (5, "0"), "once": (6, "0"),
    "df": (7, "0"), "hd": (8, "0"), "buffWaitType": (9, "0"),
    "buffWaitEndFrame": (10, "0"), "need2HidePlayerUi": (11, "0"),
    "loc": (12, "-1"), "spineFlag": (13, "0"),
}
_H5_BOOLEANS = {"once", "df", "hd", "need2HidePlayerUi", "spineFlag"}
_FLASH_XML_ROWS = {
    "BufferConfigXML": "./buff/s", "BufferDescConfigXML": "./buff/s",
    "BufferExecuteXML": "./s", "ComboBuffsConfigXML": "./combo/s",
}
_H5_FILES = (
    "buffer_config.json", "spine_buffer_config.json", "buffer_bg_config.json",
    "player_buffer_execute.json", "combo_buffs_config.json",
)


def _sort_ids(values):
    def order(value):
        try:
            return (0, int(value), value)
        except ValueError:
            return (1, 0, value)
    return sorted(values, key=order)


def _is_numeric_id(value: str) -> bool:
    return re.fullmatch(r"-?[0-9]+", value) is not None


def _metadata(path: Path, source: Path) -> dict:
    data = path.read_bytes()
    return {"file": path.relative_to(source).as_posix(), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def _last_rows(rows: list[dict], key: str = "id") -> tuple[dict, list[dict]]:
    """Replace complete rows; merging attributes can invent a nonexistent row."""
    result, positions = {}, defaultdict(list)
    for index, row in enumerate(rows):
        if key not in row:
            raise ValueError(f"row {index} is missing {key}: {row!r}")
        identity = row[key]
        positions[identity].append(index)
        result[identity] = dict(row)
    duplicates = []
    for identity in _sort_ids(k for k, indexes in positions.items() if len(indexes) > 1):
        indexes = positions[identity]
        duplicates.append({"id": identity, "rowIndexes": indexes,
                           "different": any(rows[i] != rows[indexes[0]] for i in indexes[1:]),
                           "selectedRowIndex": indexes[-1]})
    return result, duplicates


def _js_parse_int(value: str) -> int | None:
    """Relevant parseInt behavior; JSON null represents an official NaN result."""
    value = value.lstrip()
    sign = -1 if value.startswith("-") else 1
    if value.startswith(("+", "-")):
        value = value[1:]
    if value.startswith(("0x", "0X")):
        match = re.match(r"[0-9a-fA-F]+", value[2:])
        return sign * int(match[0], 16) if match else None
    match = re.match(r"[0-9]+", value)
    return sign * int(match[0]) if match else None


def _flash_int(value: str | None, default: int = 0) -> int:
    if value is None:
        return default
    try:
        return int(value)
    except ValueError:
        return 0


def _h5_html(message: str) -> str:
    for index, color in enumerate(H5_COLORS):
        # Official StringUtil.toLetter: 0 -> "0", positive 1 -> "A", 2 -> "B".
        token = "0" if index == 0 else chr(ord("A") + index - 1)
        message = message.replace("$c" + token, f"<font color='{color}'>")
    return message.replace("c$", "</font>")


def _plain(message: str) -> str:
    # Used only for comparison. Source strings and canonical mess are unchanged.
    return html.unescape(re.sub(r"<[^>]*>", "", message))


def _decode_h5(buff_id: str, raw: str, spine_ids: set[int]) -> tuple[dict, list[dict]]:
    if not isinstance(raw, str):
        raise ValueError(f"H5 Buff {buff_id} is not a pipe string")
    fields = (raw or "|").split("|")
    # Even absent fields are distinguished from present-but-empty fields.
    decoded: dict[str, Any] = {"id": buff_id, "fields": fields,
                               "nm": fields[0], "mess": fields[1] if len(fields) > 1 else "",
                               "beffect": fields[3] if len(fields) > 3 else "",
                               "bhs": fields[4] if len(fields) > 4 else ""}
    invalid = []
    for field, (index, default) in _H5_NUMBERS.items():
        source_value = fields[index] if len(fields) > index else default
        value = _js_parse_int(source_value)
        if value is None:
            invalid.append({"id": buff_id, "field": field, "pipeIndex": index,
                            "raw": source_value, "result": None, "error": "parseInt-NaN"})
        decoded[field] = value == 1 if field in _H5_BOOLEANS else value
    # StringUtil.replaceAll converts undefined/null/empty messages to "".
    decoded["messHtml"] = _h5_html(decoded["mess"])
    decoded["movieType"] = 2 if decoded["spineFlag"] or decoded["did"] in spine_ids else 1
    return decoded, invalid


def extract_buff_tables(source: Path) -> dict:
    """Return canonical Buffs, complete source rows, comparisons, and pfmap.

    Names prefer Flash descriptor.nm, then Flash config.nm, then H5.nm. Their
    source is explicit. The Flash descriptor alone supplies mess/beffect/bhs;
    a missing descriptor leaves those strings empty. H5 render/control fields
    stay in rawTables and never overwrite the flat Flash attributes.
    """
    source = Path(source)
    flash, roots = {}, {}
    for name, xpath in _FLASH_XML_ROWS.items():
        files = sorted((source / "xml").glob("*." + name + "__*.xml"))
        if len(files) != 1:
            raise ValueError(f"expected exactly one {name}, found {len(files)}")
        root = ET.parse(files[0]).getroot()
        roots[name] = root
        flash[name] = {**_metadata(files[0], source), "rowPath": xpath,
                       "rows": [dict(row.attrib) for row in root.findall(xpath)]}
    config, config_duplicates = _last_rows(flash["BufferConfigXML"]["rows"])
    descriptions, desc_duplicates = _last_rows(flash["BufferDescConfigXML"]["rows"])
    pf_root = roots["BufferConfigXML"]
    pf_rows = [dict(row.attrib) for row in pf_root.findall("./pfmap/m")]
    pf_defaults = [dict(row.attrib) for row in pf_root.findall("./pfmap/def")]
    flash["BufferConfigXML"]["powerFullRows"] = {"def": pf_defaults, "m": pf_rows}
    pf_index, pf_duplicates = _last_rows(pf_rows, "raceId")

    h5 = {}
    for filename in _H5_FILES:
        path = source / "h5-tables" / filename
        h5[filename] = {**_metadata(path, source),
                        "data": json.loads(path.read_text(encoding="utf-8-sig"))}
    h5_config = h5["buffer_config.json"]["data"]
    h5_raw = h5_config["buff"]
    spine_ids = set(h5["spine_buffer_config.json"]["data"]["id"])
    decoded, h5_invalid = {}, []
    for buff_id, raw in h5_raw.items():
        decoded[buff_id], invalid = _decode_h5(buff_id, raw, spine_ids)
        h5_invalid.extend(invalid)
    h5["buffer_config.json"]["pipeFields"] = list(H5_PIPE_FIELDS)
    h5["buffer_config.json"]["decoded"] = decoded

    buffs = {}
    all_ids = set(config) | set(descriptions) | set(decoded)
    for buff_id in _sort_ids(all_ids):
        cfg, desc, variant = config.get(buff_id, {}), descriptions.get(buff_id, {}), decoded.get(buff_id, {})
        buff = dict(cfg)
        buff["id"] = buff_id
        candidates = ((desc.get("nm", ""), "flash-desc"),
                      (cfg.get("nm", ""), "flash-config"),
                      (variant.get("nm", ""), "h5-buffer"))
        name, name_source = next(((name, origin) for name, origin in candidates if name), ("", "none"))
        buff.update(nm=name, nameSource=name_source, nameStatus="named" if name else "unnamed",
                    configName=cfg.get("nm", ""),
                    configSource="flash-config" if buff_id in config else "none",
                    descriptionSource="flash-desc" if buff_id in descriptions else "none")
        for field in ("mess", "beffect", "bhs"):
            buff[field] = desc.get(field, "")
        buffs[buff_id] = buff

    common = _sort_ids(set(config) & set(decoded))
    description_name_fills, canonical_name_fills, name_conflicts, h5_missing_names = [], [], [], []
    same_config_names, hidden_name_fills = [], []
    message_raw_differences, message_content_differences, message_fills = [], [], []
    descriptor_field_differences = {"beffect": [], "bhs": []}
    numeric_differences = {field: [] for field in _H5_NUMBERS}
    flash_invalid = []
    hidden_ids = {key for key, row in config.items() if _flash_int(row.get("hd")) == 1}
    for buff_id in common:
        cfg, desc, variant = config[buff_id], descriptions.get(buff_id, {}), decoded[buff_id]
        flash_name, h5_name = desc.get("nm", ""), variant["nm"]
        if not flash_name and h5_name:
            description_name_fills.append(buff_id)
            if h5_name == cfg.get("nm", ""):
                same_config_names.append(buff_id)
            if buff_id in hidden_ids:
                hidden_name_fills.append(buff_id)
        if not (flash_name or cfg.get("nm", "")) and h5_name:
            canonical_name_fills.append(buff_id)
        if (flash_name or cfg.get("nm", "")) and not h5_name:
            h5_missing_names.append(buff_id)
        if (flash_name or cfg.get("nm", "")) and h5_name and h5_name != (flash_name or cfg.get("nm", "")):
            name_conflicts.append({"id": buff_id, "flashDesc": flash_name,
                                   "flashConfig": cfg.get("nm", ""), "h5": h5_name})
        flash_message, h5_message = desc.get("mess", ""), variant["mess"]
        if flash_message != h5_message:
            message_raw_differences.append(buff_id)
            if _plain(flash_message) != _plain(variant["messHtml"] or ""):
                message_content_differences.append({"id": buff_id, "flash": flash_message,
                                                   "h5": h5_message})
        if not flash_message and h5_message:
            message_fills.append(buff_id)
        for field in descriptor_field_differences:
            if desc.get(field, "") != variant[field]:
                descriptor_field_differences[field].append({"id": buff_id,
                                                           "flash": desc.get(field, ""),
                                                           "h5": variant[field]})
        for field, (index, _) in _H5_NUMBERS.items():
            # h5_spine is a source override, not Flash's runtime spine flag.
            flash_field = "spine" if field == "spineFlag" else field
            raw = cfg.get(flash_field)
            try:
                flash_number = int(raw) if raw is not None else (-1 if field == "loc" else 0)
            except ValueError:
                flash_invalid.append({"id": buff_id, "field": flash_field, "raw": raw,
                                      "result": 0, "error": "not-an-integer"})
                continue
            h5_value = variant[field]
            if h5_value is None:
                continue
            flash_value = flash_number == 1 if field in _H5_BOOLEANS else flash_number
            if field == "need2HidePlayerUi":
                # Flash assigns int(@need2HidePlayerUi) to Boolean; H5 tests == 1.
                flash_value = bool(flash_number)
            if flash_value != h5_value:
                difference = {"id": buff_id, "flashRaw": raw, "flash": flash_value,
                              "h5Raw": variant["fields"][index] if len(variant["fields"]) > index else None,
                              "h5": h5_value}
                override = "h5_spine" if field == "spineFlag" else "h5_" + field
                if override in cfg:
                    difference["explicitH5Attribute"] = override
                    difference["explicitH5Value"] = cfg[override]
                numeric_differences[field].append(difference)

    flash_pf = {"defaultBuffId": pf_defaults[-1].get("buffId") if pf_defaults else None,
                "raceIdToBuffId": {key: row.get("buffId") for key, row in pf_index.items()}}
    h5_pf = h5_config["pfmap"]
    pf_differences = []
    for race_id in _sort_ids(set(flash_pf["raceIdToBuffId"]) | set(h5_pf["m"])):
        left, right = flash_pf["raceIdToBuffId"].get(race_id), h5_pf["m"].get(race_id)
        if (str(left) if left is not None else None) != (str(right) if right is not None else None):
            pf_differences.append({"raceId": race_id, "flash": left, "h5": right})
    power_full = {**flash_pf,
                  "lookupOrder": ["raceIdToBuffId", "petDictionary.fullPowerBuffId != -1", "defaultBuffId"],
                  "h5": {"defaultBuffId": h5_pf["def"], "raceIdToBuffId": h5_pf["m"]},
                  "differences": {"default": str(flash_pf["defaultBuffId"]) != str(h5_pf["def"]),
                                  "races": pf_differences}}
    counts = {
        "flashConfigRows": len(flash["BufferConfigXML"]["rows"]), "flashConfigIds": len(config),
        "flashDescriptionRows": len(flash["BufferDescConfigXML"]["rows"]), "flashDescriptionIds": len(descriptions),
        "h5BuffIds": len(decoded), "canonicalBuffIds": len(buffs),
        "flashConfigNumericIds": sum(_is_numeric_id(key) for key in config),
        "h5NumericIds": sum(_is_numeric_id(key) for key in decoded),
        "flashConfigDuplicateIds": len(config_duplicates),
        "flashConfigDifferentDuplicateIds": sum(item["different"] for item in config_duplicates),
        "flashDescriptionDuplicateIds": len(desc_duplicates),
        "flashConfigWithoutDescription": len(set(config) - set(descriptions)),
        "canonicalWithoutName": sum(not row["nm"] for row in buffs.values()),
        "flashHiddenIds": len(hidden_ids),
        "flashHiddenWithDescriptionName": sum(bool(descriptions.get(key, {}).get("nm")) for key in hidden_ids),
        "flashHiddenWithDescriptionMessage": sum(bool(descriptions.get(key, {}).get("mess")) for key in hidden_ids),
        "h5HiddenWithName": sum(row["hd"] and bool(row["nm"]) for row in decoded.values()),
        "h5HiddenWithMessage": sum(row["hd"] and bool(row["mess"]) for row in decoded.values()),
        "h5NamesAbsentFromFlashDescription": len(description_name_fills),
        "h5NamesMatchingFlashConfigFallback": len(same_config_names),
        "h5NamesAbsentFromBothFlashTables": len(canonical_name_fills),
        "h5MissingFlashNames": len(h5_missing_names),
        "nameConflicts": len(name_conflicts), "messageRawDifferences": len(message_raw_differences),
        "messageContentDifferences": len(message_content_differences), "h5MessageFills": len(message_fills),
        "flashInvalidNumericFields": len(flash_invalid), "h5InvalidNumericFields": len(h5_invalid),
        "flashBufferExecuteRows": len(flash["BufferExecuteXML"]["rows"]),
        "flashComboRows": len(flash["ComboBuffsConfigXML"]["rows"]),
        "flashPowerFullRaces": len(pf_index), "h5PowerFullRaces": len(h5_pf["m"]),
        "h5BackgroundBuffIds": len(h5["buffer_bg_config.json"]["data"]), "h5SpineResourceIds": len(spine_ids),
    }
    report = {
        "counts": counts, "nameSources": dict(Counter(row["nameSource"] for row in buffs.values())),
        "rules": {
            "joinKey": "BufferConfigXML.id = BufferDescConfigXML.id = H5 buffer_config.buff key",
            "displayId": "did identifies a visual resource and is never a description lookup key",
            "duplicatePolicy": "last complete row wins; raw rows and source order retained",
            "canonicalText": "nm: FlashDesc > FlashConfig > H5; mess/beffect/bhs: FlashDesc only",
            "canonicalState": "Flash config attributes preserved; H5 render/control fields never overwrite them",
            "missingText": "empty official text stays empty; no generated name or description",
            "h5NumericErrors": "parseInt NaN is represented by null/error with original field retained",
            "hidden": "hd controls client display; it does not imply missing name or description",
        },
        "duplicates": {"flashConfig": config_duplicates, "flashDescription": desc_duplicates,
                       "flashPowerFull": pf_duplicates},
        "invalidIds": {"flashConfig": [key for key in config if not _is_numeric_id(key)],
                       "flashDescription": [key for key in descriptions if not _is_numeric_id(key)],
                       "h5": [key for key in decoded if not _is_numeric_id(key)],
                       "policy": "retain original keys; never silently coerce a malformed key to Buff 0"},
        "missing": {
            "flashConfigWithoutDescriptionIds": _sort_ids(set(config) - set(descriptions)),
            "flashDescriptionWithoutConfigIds": _sort_ids(set(descriptions) - set(config)),
            "flashOnlyIds": _sort_ids(set(config) - set(decoded)), "h5OnlyIds": _sort_ids(set(decoded) - set(config)),
            "canonicalWithoutNameIds": _sort_ids(key for key, row in buffs.items() if not row["nm"]),
        },
        "textComparison": {
            "h5NamesAbsentFromFlashDescriptionIds": description_name_fills,
            "h5NamesMatchingFlashConfigFallbackIds": same_config_names,
            "h5NamesAbsentFromBothFlashTablesIds": canonical_name_fills,
            "h5MissingFlashNameIds": h5_missing_names,
            "hiddenH5NameFillIds": hidden_name_fills, "nameConflicts": name_conflicts,
            "messageRawDifferenceIds": message_raw_differences,
            "messageContentDifferences": message_content_differences, "h5MessageFillIds": message_fills,
            "descriptorFieldDifferences": descriptor_field_differences,
        },
        "renderComparison": {"differenceCounts": {key: len(value) for key, value in numeric_differences.items()},
                             "differences": numeric_differences,
                             "invalidNumericFields": {"flash": flash_invalid, "h5": h5_invalid}},
    }
    return {"buffs": buffs, "rawTables": {"flash": flash, "h5": h5},
            "report": report, "powerFull": power_full}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    extracted = extract_buff_tables(args.source)
    args.output.write_text(json.dumps(extracted, ensure_ascii=False, separators=(",", ":"), allow_nan=False),
                           encoding="utf-8")
    print(json.dumps(extracted["report"]["counts"], ensure_ascii=False))
