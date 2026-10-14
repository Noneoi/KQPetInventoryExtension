"""Static extraction of SkillEmbedEffectXML's original DefineBinaryData payload.

The shipped UTF-8 text is XML-shaped but is not well-formed XML 1.0: a comment
contains ``--`` and two attributes lack separating whitespace. A compatibility
view records every omitted comment and inserted separator. Original bytes are
always retained; no Flash code, embedded code class or resource is executed.
"""
from __future__ import annotations

import argparse
import base64
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

from fetch_reference import binary_payloads


SYMBOL_NAME = "mmo.battleconfig.data.SkillEmbedEffectXML__xmlClass"
PACKAGE_NAME = "battleconfig_battleconfig.swf"
_ATTRIBUTE = re.compile(r"[A-Za-z_][A-Za-z0-9_.:-]*\s*=")


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _location(text: str, index: int) -> dict:
    return {"characterOffset": index, "utf8ByteOffset": len(text[:index].encode("utf-8")),
            "line": text.count("\n", 0, index) + 1,
            "column": index - text.rfind("\n", 0, index)}


def compatibility_xml(text: str) -> tuple[str, list[dict]]:
    """Omit only comments and separate adjacent *quoted* attribute assignments.

    Quoted values and CDATA are copied verbatim. All other malformed XML remains
    an error for ElementTree; no generic recovery or missing-value guess is used.
    """
    output, edits = [], []
    index = 0
    while index < len(text):
        if text.startswith("<!--", index):
            end = text.find("-->", index + 4)
            if end < 0:
                raise ValueError(f"Unterminated comment at character {index}")
            end += 3
            original = text[index:end]
            edits.append({"operation": "omit-comment", **_location(text, index),
                          "endCharacterOffset": end, "original": original,
                          "containsXml10ForbiddenDoubleHyphen": "--" in original[4:-3]})
            # Keep all line positions available for the following XML parser.
            output.append("".join(char if char in "\r\n" else " " for char in original))
            index = end
        elif text.startswith("<![CDATA[", index):
            end = text.find("]]>", index + 9)
            if end < 0:
                raise ValueError(f"Unterminated CDATA at character {index}")
            output.append(text[index:end + 3])
            index = end + 3
        elif text.startswith("<!", index):
            # This table has no DTD. Do not expand entities or invent a recovery.
            raise ValueError(f"Unsupported XML declaration at character {index}")
        elif text[index] == "<":
            quote = None
            closed = False
            while index < len(text):
                char = text[index]
                output.append(char)
                index += 1
                if quote is not None:
                    if char == quote:
                        quote = None
                        if _ATTRIBUTE.match(text, index):
                            edits.append({"operation": "insert-attribute-separator",
                                          **_location(text, index), "inserted": " ",
                                          "context": text[max(0, index - 20):index + 30]})
                            output.append(" ")
                elif char in ("'", '"'):
                    quote = char
                elif char == ">":
                    closed = True
                    break
            if not closed:
                raise ValueError("Unterminated XML tag")
        else:
            output.append(text[index])
            index += 1
    return "".join(output), edits


def _tree(element: ET.Element) -> dict:
    result = {"tag": element.tag, "attributes": dict(element.attrib)}
    if element.text is not None:
        result["text"] = element.text
    if element.tail is not None:
        result["tail"] = element.tail
    if len(element):
        result["children"] = [_tree(child) for child in element]
    return result


def _integer(value, default=0) -> int:
    return default if value is None or value == "" else int(value)


def _integer_list(value: str | None) -> list[int]:
    return [] if value is None or value == "" else [_integer(item) for item in value.split(",")]


def parse_embed_payload(blob: bytes, known_skill_ids: set[str] | None = None) -> tuple[dict, dict]:
    raw = {"symbolName": SYMBOL_NAME, "bytes": len(blob), "sha256": _sha(blob),
           "binaryBase64": base64.b64encode(blob).decode("ascii")}
    report = {"status": "unparsed", "symbolName": SYMBOL_NAME, "bytes": len(blob),
              "sha256": raw["sha256"], "runtimeExecuted": False}
    try:
        text = blob.decode("utf-8")
    except UnicodeDecodeError as error:
        report["error"] = str(error)
        return raw, report
    raw["rawUtf8"] = text
    try:
        ET.fromstring(text)
        original_error = None
    except ET.ParseError as error:
        original_error = {"message": str(error), "line": error.position[0],
                          "columnZeroBased": error.position[1]}
    report["originalXmlWellFormed"] = original_error is None
    if original_error:
        report["originalXmlError"] = original_error
    try:
        normalized, edits = compatibility_xml(text)
        root = ET.fromstring(normalized)
    except (ET.ParseError, ValueError) as error:
        report["error"] = str(error)
        return raw, report
    raw["compatibilityView"] = {"xml": normalized, "sha256": _sha(normalized.encode("utf-8")),
                                 "edits": edits, "originalBytesChanged": False}
    raw["structure"] = _tree(root)
    if root.tag != "config":
        report["error"] = f"Unexpected root element {root.tag!r}"
        return raw, report

    command_rows, command_map, duplicates = [], {}, []
    for index, element in enumerate(root.findall("./cmdMap/m")):
        row = {"row": index, "path": f"/config/cmdMap/m[{index}]", "attributes": dict(element.attrib)}
        command_rows.append(row)
        nickname = element.get("nickName", "")
        if nickname in command_map:
            duplicates.append({"table": "cmdMap", "key": nickname,
                               "previousRow": command_map[nickname]["row"], "winningRow": index})
        command_map[nickname] = {"row": index, "className": element.get("clsName", "")}

    dialog_rows, dialogs = [], {}
    for index, element in enumerate(root.findall("./npcDialog/d")):
        dialog_id = _integer(element.get("id"))
        key = str(dialog_id)
        row = {"row": index, "path": f"/config/npcDialog/d[{index}]", "attributes": dict(element.attrib),
               "structure": _tree(element), "id": dialog_id, "npcId": _integer(element.get("npcId")),
               "iconId": _integer(element.get("iconId")), "showBtnOk": _integer(element.get("viewBtnOk")) == 1,
               "contexts": [_tree(child) for child in element.findall("./context")]}
        dialog_rows.append(row)
        if key in dialogs:
            duplicates.append({"table": "npcDialog", "key": key,
                               "previousRow": dialogs[key]["row"], "winningRow": index})
        dialogs[key] = row

    effects, effects_by_race, skill_refs, dialog_refs, timing_refs = [], {}, [], [], []
    unknown_fields = []
    supported = {"raceId", "timing", "type", "url", "params", "sf", "tf", "wf", "gv", "lhf", "hhf", "rc"}
    for index, element in enumerate(root.findall("./effect/e")):
        attributes = dict(element.attrib)
        row = {"row": index, "path": f"/config/effect/e[{index}]", "attributes": attributes,
               "raceId": _integer(element.get("raceId")), "type": _integer(element.get("type")),
               "timing": element.get("timing", "").split(","), "url": element.get("url", ""),
               "params": element.get("params", ""), "skillFilter": _integer_list(element.get("sf")),
               "taskFilter": _integer_list(element.get("tf")), "winnerTeamIdFilter": _integer_list(element.get("wf")),
               "globalVariableFilter": element.get("gv").split(",") if element.get("gv") else [],
               "lowerHpFilter": _integer(element.get("lhf"), 100),
               "higherHpFilter": _integer(element.get("hhf"), 0),
               "repeatCount": _integer(element.get("rc"), 2147483647)}
        effects.append(row)
        effects_by_race.setdefault(str(row["raceId"]), []).append(index)
        if attributes.keys() - supported:
            unknown_fields.append({"row": index, "fields": sorted(attributes.keys() - supported)})
        for nickname in row["timing"]:
            timing_refs.append({"effectRow": index, "nickname": nickname,
                                "className": command_map.get(nickname, {}).get("className"),
                                "commandRow": command_map.get(nickname, {}).get("row")})
        for skill_id in row["skillFilter"]:
            reference = {"effectRow": index, "raceId": row["raceId"], "field": "sf", "skillId": skill_id,
                         "meaning": "require current RoundResult.skillId in this filter before embedded presentation"}
            if known_skill_ids is not None:
                reference["skillEntryPresent"] = str(skill_id) in known_skill_ids
            skill_refs.append(reference)
        if row["type"] == 1:
            dialog_id = _integer(row["params"])
            dialog_refs.append({"effectRow": index, "field": "params", "dialogId": dialog_id,
                                "dialogRow": dialogs.get(str(dialog_id), {}).get("row")})

    raw["tables"] = {"effects": effects, "commands": command_rows, "dialogs": dialog_rows}
    raw["indexes"] = {"effectsByRaceId": effects_by_race, "commandMap": command_map,
                       "dialogRowsById": {key: value["row"] for key, value in dialogs.items()}}
    raw["references"] = {"skillFilters": skill_refs, "npcDialogs": dialog_refs,
                         "timingCommands": timing_refs,
                         "buffGrantRelations": [], "battleBuffApplierRelations": []}
    counts = {"effectRows": len(effects), "commandRows": len(command_rows), "dialogRows": len(dialog_rows),
              "effectTypes": dict(Counter(str(row["type"]) for row in effects)),
              "races": len(effects_by_race), "skillFilterReferences": len(skill_refs),
              "distinctSkillFilterIds": len({item["skillId"] for item in skill_refs}),
              "npcDialogReferences": len(dialog_refs), "commentOmissions": sum(edit["operation"] == "omit-comment" for edit in edits),
              "attributeSeparatorInsertions": sum(edit["operation"] == "insert-attribute-separator" for edit in edits)}
    report.update(status="parsed-compatibility-view" if original_error else "parsed",
                  counts=counts, duplicates=duplicates, unknownEffectFields=unknown_fields,
                  missingTimingCommands=[item for item in timing_refs if item["className"] is None],
                  missingDialogs=[item for item in dialog_refs if item["dialogRow"] is None],
                  missingSkillEntries=[item for item in skill_refs if item.get("skillEntryPresent") is False],
                  unknownEffectTypes=sorted({row["type"] for row in effects} - {0, 1, 2}),
                  attributeNames=sorted({key for element in root.iter() for key in element.attrib}),
                  referenceScope="sf is a presentation eligibility filter; no Buff grant or applier mapping is inferred",
                  filterCombination="all six filter categories must pass; taskFilter is any-of, globalVariableFilter is all-of",
                  compatibilityEdits=[{key: value for key, value in edit.items() if key != "original"} for edit in edits])
    return raw, report


def _consumer_evidence(source: Path) -> list[dict]:
    files = (
        ("battleconfig/scripts/mmo/battleconfig/data/SkillEmbedEffectXML.as", "battleconfig_battleconfig.swf"),
        ("interfaces/scripts/mmo/interfaces/battleconfig/define/EmbedEffectDefine.as", "library_interfaces.swf"),
        ("interfaces/scripts/mmo/interfaces/battleconfig/define/EmbedDialogDefine.as", "library_interfaces.swf"),
        ("interfaces/scripts/mmo/interfaces/battleconfig/consts/EmbedEffectTypes.as", "library_interfaces.swf"),
        ("battle/scripts/mmo/battle/command/embed/SetupEmbedEffectCommand.as", "battle_battle.swf"),
        ("battle/scripts/mmo/battle/command/embed/sub/EmbedCommandBase.as", "battle_battle.swf"),
        ("battle/scripts/mmo/battle/command/embed/sub/EmbedCommandNpcDialog.as", "battle_battle.swf"),
        ("battle/scripts/mmo/battle/command/embed/sub/EmbedCommandMovie.as", "battle_battle.swf"),
        ("battle/scripts/mmo/battle/command/embed/sub/EmbedCommandCode.as", "battle_battle.swf"),
    )
    evidence = []
    for relative, package in files:
        local = source / relative
        if local.is_file():
            evidence.append({"path": relative, "sha256": _sha(local.read_bytes())})
            continue
        # Previously decompiled scripts are applicable only to identical SWF bytes.
        previous = source.parent / relative
        current_swf, previous_swf = source / package, source.parent / package
        if previous.is_file() and current_swf.is_file() and previous_swf.is_file():
            package_hash = _sha(current_swf.read_bytes())
            if package_hash == _sha(previous_swf.read_bytes()):
                evidence.append({"path": "../" + relative, "sha256": _sha(previous.read_bytes()),
                                 "matchingPackageSha256": package_hash})
    return evidence


def extract_skill_embed_effect(source: Path, known_skill_ids: set[str] | None = None) -> tuple[dict, dict]:
    source = Path(source)
    package = source / PACKAGE_NAME
    if not package.is_file():
        return {}, {"status": "missing-package", "package": PACKAGE_NAME}
    package_bytes = package.read_bytes()
    payloads = binary_payloads(package_bytes)
    matches = [(name, blob) for name, blob in payloads if name == SYMBOL_NAME]
    if len(matches) != 1:
        return {}, {"status": "missing-or-ambiguous-symbol", "symbolName": SYMBOL_NAME, "matches": len(matches)}
    raw, report = parse_embed_payload(matches[0][1], known_skill_ids)
    raw["source"] = {"path": PACKAGE_NAME, "sha256": _sha(package_bytes),
                     "selection": "SWF SymbolClass name joined to DefineBinaryData character ID",
                     "binaryPayloadsInPackage": len(payloads)}
    raw["consumerEvidence"] = _consumer_evidence(source)
    h5_path = source / "h5-tables/skill_embed_effect.json"
    if h5_path.is_file():
        h5_bytes = h5_path.read_bytes()
        h5_text = h5_bytes.decode("utf-8-sig")
        h5_data = json.loads(h5_text)
        raw["h5"] = {"source": {"path": h5_path.relative_to(source).as_posix(), "sha256": _sha(h5_bytes)},
                     "rawJson": h5_text, "data": h5_data}
        report["h5SeparateNamespace"] = {"effectRows": len(h5_data.get("effect", [])),
                                          "dialogRows": len(h5_data.get("npcDialog", [])),
                                          "mergedIntoFlash": False}
    return raw, report


def export_skill_embed_effect(source: Path, output: Path) -> dict:
    raw, report = extract_skill_embed_effect(source)
    if not raw:
        raise ValueError(json.dumps(report, ensure_ascii=False))
    output.mkdir(parents=True, exist_ok=True)
    stem = SYMBOL_NAME
    (output / (stem + ".bin")).write_bytes(base64.b64decode(raw["binaryBase64"], validate=True))
    if "compatibilityView" in raw:
        (output / (stem + ".compatibility.xml")).write_text(raw["compatibilityView"]["xml"], encoding="utf-8")
    (output / (stem + ".json")).write_text(json.dumps({"raw": raw, "report": report}, ensure_ascii=False, indent=2), encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = export_skill_embed_effect(args.source, args.output)
    print(json.dumps({"status": report["status"], "bytes": report["bytes"],
                      "sha256": report["sha256"], "counts": report.get("counts")}, ensure_ascii=False))
