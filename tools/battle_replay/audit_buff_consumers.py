"""Read-only audit of official Buff text consumers and exact bracketed entries.

This deliberately does not update the catalog, infer Buff names from display IDs,
or turn a configuration reference into an in-battle source/target attribution.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def file_record(path: Path) -> dict:
    data = path.read_bytes()
    return {"file": str(path.resolve()), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def plain_text(text: str) -> str:
    """The textual portion of AQInteractiveBattleGTextWrap.parseHtmlText.

    Decode exactly its six named/numbered entities, after removing markup, and
    preserve BR/newline boundaries. We do not normalize punctuation or names.
    """
    text = re.sub(r"\r\n|\r", "\n", text)
    parts, pos = [], 0
    for match in re.finditer(r"<[^>]*>", text):
        parts.append(decode_entities(text[pos:match.start()]))
        tag = re.search(r"<\s*/?\s*([a-z0-9]+)", match[0], re.I)
        if tag and tag[1].lower() == "br" and not re.match(r"<\s*/", match[0]):
            parts.append("\n")
        pos = match.end()
    parts.append(decode_entities(text[pos:]))
    return "".join(parts)


def decode_entities(text: str) -> str:
    for before, after in (("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'),
                          ("&#39;", "'"), ("&apos;", "'"), ("&amp;", "&")):
        text = text.replace(before, after)
    return text


def build_entry_index(path: Path) -> tuple[dict, list]:
    index, duplicates = {}, []
    for row_index, row in enumerate(ET.parse(path).getroot().findall("./pet/s")):
        attrs = dict(row.attrib)
        name = attrs.get("n", "")
        record = {"name": name, "description": attrs.get("d", ""),
                  "type": attrs.get("t", ""), "raw": attrs,
                  "sourceRow": row_index, "sourcePath": f"/config/pet/s[{row_index + 1}]"}
        # EntryConfigXML keeps the FIRST row for a name, unlike BufferConfigXML.
        if name in index:
            duplicates.append({"name": name, "keptRow": index[name]["sourceRow"],
                               "ignoredRow": row_index, "ignored": attrs})
        else:
            index[name] = record
    return index, duplicates


def resolve_entry(content: str, index: dict) -> dict:
    parts = content.split("·")
    base, args = content, []
    if len(parts) > 1 and parts[0] in index:
        base, args = parts[0], parts[1:]
    row = index.get(base)
    if row is None:
        return {"literal": content, "resolved": False}
    description = row["description"]
    # EntryArgLike substitutes numbered arguments backwards, then bare ARG.
    # No current target argument contains ARG or AS replacement tokens; reject
    # such input rather than pretend Python replacement exactly emulates AS.
    if any("ARG" in arg or "$" in arg for arg in args):
        raise ValueError(f"Unsupported ActionScript replacement argument: {content!r}")
    for i in reversed(range(len(args))):
        description = description.replace(f"ARG{i + 1}", args[i])
    if args:
        description = description.replace("ARG", args[0])
    description = re.sub(r"ARG\d*", "指定数值", description)
    return {"literal": content, "resolved": True, "entryName": base,
            "arguments": args, "renderedName": base + ("·" + "·".join(args) if args else ""),
            "renderedDescription": description, "entry": row}


def find_entries(text: str, index: dict) -> list:
    links, seen = [], set()
    # Official matching operates on individual plain-text lines; a bracket may
    # not silently span a BR or newline that the official consumer splits.
    for line in re.split(r"\r\n|\n|\r", plain_text(text)):
        for match in re.finditer(r"\[([^\]]+)\]", line):
            link = resolve_entry(match[1], index)
            key = link.get("renderedName", link["literal"])
            if key not in seen:
                seen.add(key)
                links.append(link)
    return links


def make_entry_report(reference: Path, legacy: Path) -> dict:
    latest = reference / "latest-tables"
    linked = latest / "linked"
    files = {name: linked / name for name in
             ("buffs.json", "skills.json", "relations.json", "log-coverage.json")}
    buffs, skills, relations, coverage = (read_json(files[name]) for name in files)
    entry_path = latest / "xml/mmo.battleconfig.data.EntryConfigXML__xmlClass.xml"
    index, duplicates = build_entry_index(entry_path)
    targets = sorted((key for key, row in coverage["buffs"]["entries"].items()
                      if not row["hasDescription"]), key=int)
    own, related = [], []
    for buff_id in targets:
        buff = buffs[buff_id]
        fields = []
        for field in ("nm", "mess"):
            value = buff.get(field, "")
            mentions = find_entries(value, index)
            if mentions:
                fields.append({"field": field, "value": value, "links": mentions})
        if fields:
            own.append({"buffId": buff_id, "nameSource": buff.get("nameSource"), "fields": fields})
    for relation in relations["relations"]:
        if (relation["buffId"] not in targets or relation["kind"] not in
                ("buffer_execute_skill", "buffer_execute_lighted_skill")):
            continue
        skill = skills.get(relation["skillId"])
        fields = []
        if skill:
            for field in ("name", "description"):
                value = skill.get(field, "")
                mentions = find_entries(value, index)
                if mentions:
                    fields.append({"field": field, "value": value, "links": mentions})
        related.append({"buffId": relation["buffId"], "skillId": relation["skillId"],
                        "skillName": skill.get("name", "") if skill else "",
                        "skillDescription": skill.get("description", "") if skill else "",
                        "skillTextSource": skill.get("textSource") if skill else None,
                        "relation": relation, "fields": fields})
    own_links = [link for row in own for field in row["fields"] for link in field["links"]]
    related_links = [link for row in related for field in row["fields"] for link in field["links"]]
    current_text_consumer = (reference / "cross-resource-audit-20261009/consumer/current-common/scripts/"
                             "mmo/common/aoqi/component/text/AQInteractiveBattleGTextWrap.as")
    consumers = [
        current_text_consumer if current_text_consumer.is_file() else
        legacy / "out/as_library_common/scripts/mmo/common/aoqi/component/text/AQInteractiveBattleGTextWrap.as",
        latest / "battleconfig/scripts/mmo/battleconfig/data/EntryConfigXML.as",
        latest / "battleconfig/scripts/mmo/battleconfig/data/EntryArgLike.as",
        reference / "battle/scripts/mmo/battle/helper/BattleBufferHelper.as",
        reference / "battle/scripts/mmo/battle/util/BattleNumUnitUtil.as",
    ]
    return {
        "generatedUtc": datetime.now(timezone.utc).isoformat(),
        "scope": "Only sampled Buff IDs whose own canonical mess field is empty; direct bracket references only.",
        "interpretation": [
            "A resolved bracket is a literal textual reference to an official term, not proof the Buff ID equals that term.",
            "Own-name links are found in official metadata; this does not assert the Flash Buff tooltip renders that metadata with AQInteractiveBattleGTextWrap.",
            "Associated-skill links require an explicit BufferExecuteXML skillId/lightedSkillId relation; they are not the Buff's own mess field.",
            "No displayId/name similarity/prefix stripping, guessed causes, game-code execution, or catalog changes are used.",
            "EntryConfigXML exact names keep the first duplicate; arguments use the official middle-dot rule. This report excludes caller-supplied dynamic extraEntries and recursive focused side-panels.",
        ],
        "sources": {**{name: file_record(path) for name, path in files.items()},
                    "EntryConfigXML": file_record(entry_path),
                    "consumers": [file_record(path) for path in consumers]},
        "textConsumerStatus": "current_version_decompiled" if current_text_consumer.is_file() else "cached_consumer_only",
        "statistics": {
            "targetBuffsMissingOwnMess": len(targets), "entryNames": len(index),
            "duplicateEntryRows": len(duplicates), "buffsWithOwnBracketText": len(own),
            "ownBracketLinks": len(own_links), "ownResolvedLinks": sum(x["resolved"] for x in own_links),
            "ownDistinctResolvedEntries": sorted({x["entryName"] for x in own_links if x["resolved"]}),
            "relatedExecutionBuffs": len({x["buffId"] for x in related}),
            "relatedExecutionSkills": len({x["skillId"] for x in related}),
            "relatedBracketLinks": len(related_links),
            "relatedResolvedLinks": sum(x["resolved"] for x in related_links),
            "relatedDistinctResolvedEntries": sorted({x["entryName"] for x in related_links if x["resolved"]}),
            "ownMissingMessStillMissingAfterAudit": len(targets),
        },
        "targetBuffIds": targets, "ownBuffFields": own,
        "associatedExecutionSkillFields": related, "duplicateEntries": duplicates,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=Path("build-battle-reference"))
    parser.add_argument("--legacy", type=Path, default=Path("../22"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or args.reference / "cross-resource-audit-20261009/consumer/entry-links.json"
    report = make_entry_report(args.reference, args.legacy)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "statistics": report["statistics"]}, ensure_ascii=True))


if __name__ == "__main__":
    main()
