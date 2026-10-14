"""Extract official battle terms and resolve literal bracket references.

The Flash EntryConfigXML/EntryArgLike and AQInteractiveBattleGTextWrap rules are
kept separate from Buff IDs. A term reference is additional explanatory text,
never a replacement for a Buff's own message or evidence of who applied it.
ActionScript and JavaScript are read as source data, never executed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


ENTRY_XML = "xml/mmo.battleconfig.data.EntryConfigXML__xmlClass.xml"
ENTRY_H5 = "h5-tables/entry_config.json"
RULE_FILES = (
    "battleconfig/scripts/mmo/battleconfig/data/EntryConfigXML.as",
    "battleconfig/scripts/mmo/battleconfig/data/EntryArgLike.as",
    "interfaces/scripts/mmo/interfaces/battleconfig/define/EntryDefine.as",
)


def _file_info(source: Path, relative: str) -> dict:
    data = (source / relative).read_bytes()
    return {"file": relative, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _decode_entities(text: str) -> str:
    # AQInteractiveBattleGTextWrap.decodeHtmlEntities performs these ordered
    # literal replacements, not a browser's complete HTML entity decoder.
    for old, new in (("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'),
                     ("&#39;", "'"), ("&apos;", "'"), ("&amp;", "&")):
        text = text.replace(old, new)
    return text


def _plain_lines(text: str) -> list[str]:
    text = re.sub(r"\r\n|\r", "\n", text)
    parts, pos = [], 0
    for match in re.finditer(r"<[^>]*>", text):
        parts.append(_decode_entities(text[pos:match.start()]))
        tag = re.search(r"<\s*/?\s*([a-z0-9]+)", match[0], re.I)
        if tag and tag[1].lower() == "br" and not re.match(r"<\s*/", match[0]):
            parts.append("\n")
        pos = match.end()
    parts.append(_decode_entities(text[pos:]))
    # textToLines runs after parseHtmlText, including its entity decoding.
    plain = re.sub(r"<br\s*/?>", "\n", "".join(parts), flags=re.I)
    return re.split(r"\r\n|\n|\r", plain)


def _render_template(template: str, arguments: list[str]) -> str:
    # These inputs can reinsert ARG in the official while/replace loop or use
    # ActionScript replacement-string tokens. Current official references have
    # none. Refuse such expansion rather than execute or invent its result.
    if any("ARG" in value or "$" in value for value in arguments):
        raise ValueError("Unsafe or unsupported ActionScript argument replacement")
    for index in reversed(range(len(arguments))):
        template = template.replace(f"ARG{index + 1}", arguments[index])
    if arguments:
        template = template.replace("ARG", arguments[0])
    return re.sub(r"ARG\d*", "指定数值", template)


def resolve_entry_mentions(text: str, entries: dict[str, dict]) -> list[dict]:
    """Resolve direct [term] mentions with exact official names and arguments.

    Missing references are retained with status='missing' for an honest audit.
    The official UI skips them. No prefix removal, punctuation conversion,
    numeric-ID matching, or recursive term expansion is performed here.
    """
    result, seen = [], set()
    for line in _plain_lines(str(text or "")):
        for match in re.finditer(r"\[([^\]]+)\]", line):
            label = match[1]
            parts = label.split("·")
            lookup_name, arguments = label, []
            if len(parts) > 1 and parts[0] in entries:
                lookup_name, arguments = parts[0], parts[1:]
            if label in seen:
                continue
            seen.add(label)
            entry = entries.get(lookup_name)
            item = {"displayLabel": label, "lookupName": lookup_name,
                    "description": "", "status": "missing", "arguments": arguments,
                    "sourcePath": "", "source": None}
            if entry is not None:
                item.update(sourcePath=entry.get("sourcePath", ""), source=entry.get("source"),
                            type=entry.get("type"), status="resolved")
                try:
                    item["description"] = _render_template(entry["template"], arguments)
                except ValueError as error:
                    item.update(status="unsupported_arguments", diagnostic=str(error))
            result.append(item)
    return result


def _as_parse_int(value) -> int | None:
    # These fields are decimal in both official tables; preserve raw values too.
    match = re.match(r"^[\s]*([+-]?\d+)", str(value))
    return int(match[1]) if match else None


def _index_rows(rows: list[dict], info: dict, table: str, duplicates: list) -> dict:
    result = {}
    for row_index, raw in enumerate(rows):
        name, template = str(raw.get("n", "")), str(raw.get("d", ""))
        path = f"/config/pet/s[{row_index + 1}]" if table == "EntryConfigXML" else f"/{row_index}"
        source = {"table": table, "file": info["file"], "sha256": info["sha256"],
                  "row": row_index, "path": path}
        item = {"label": name, "description": _render_template(template, []),
                "template": template, "type": _as_parse_int(raw.get("t", "")),
                "source": source, "sourcePath": path}
        if name in result:
            duplicates.append({"table": table, "name": name,
                               "keptRow": result[name]["source"]["row"],
                               "ignoredRow": row_index, "ignoredRaw": raw,
                               "differs": any(result[name][field] != item[field]
                                              for field in ("label", "template", "type"))})
        else:
            # Both official consumers test containsKey before insertion.
            result[name] = item
    return result


def extract_entry_tables(source: Path) -> dict:
    source = Path(source)
    flash_info = _file_info(source, ENTRY_XML)
    flash_rows = [dict(row.attrib) for row in ET.parse(source / ENTRY_XML).getroot().findall("./pet/s")]
    duplicates = []
    entries = _index_rows(flash_rows, flash_info, "EntryConfigXML", duplicates)
    raw_tables = {"flash": {"EntryConfigXML": {**flash_info, "rowPath": "./pet/s", "rows": flash_rows}},
                  "h5": {}}
    h5_entries, h5_rows = {}, []
    h5_present = (source / ENTRY_H5).is_file()
    if h5_present:
        h5_info = _file_info(source, ENTRY_H5)
        h5_rows = json.loads((source / ENTRY_H5).read_text(encoding="utf-8"))
        if not isinstance(h5_rows, list) or any(not isinstance(row, dict) for row in h5_rows):
            raise ValueError("Official entry_config must be a list of row objects")
        h5_entries = _index_rows(h5_rows, h5_info, "entry_config", duplicates)
        raw_tables["h5"]["entry_config"] = {**h5_info, "rows": h5_rows}
    differences = []
    for name in sorted(entries.keys() & h5_entries.keys()):
        differing_fields = [field for field in ("label", "template", "type")
                            if entries[name][field] != h5_entries[name][field]]
        if differing_fields:
            differences.append({"name": name, "fields": differing_fields,
                                "flash": {field: entries[name][field] for field in differing_fields},
                                "h5": {field: h5_entries[name][field] for field in differing_fields}})
    consumers = [_file_info(source, path) for path in RULE_FILES if (source / path).is_file()]
    return {"entries": entries, "rawTables": raw_tables, "report": {
        "flashRows": len(flash_rows), "flashEntries": len(entries),
        "h5Available": h5_present, "h5Rows": len(h5_rows), "h5Entries": len(h5_entries),
        "duplicates": duplicates, "duplicatePolicy": "first_row_wins",
        "flashH5Differences": differences,
        "missingInH5": sorted(entries.keys() - h5_entries.keys()) if h5_present else [],
        "missingInFlash": sorted(h5_entries.keys() - entries.keys()),
        "consumerSources": consumers,
        "rules": ["half-width square brackets only; exact names; first duplicate wins",
                  "plain HTML text split into lines; first occurrence of each display label wins",
                  "middle-dot parameters only if the prefix itself is an exact table name; otherwise exact full-name lookup",
                  "ARGn replaced from highest supplied argument down; bare ARG uses argument one; remaining ARG\\d* becomes 指定数值",
                  "unsupported replacement-token/self-referential arguments are reported without guessed expansion",
                  "H5 remains comparison evidence; it does not overwrite or fill Flash names",
                  "terms are textual references, not Buff ID aliases or battle causal attribution"],
    }}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = extract_entry_tables(args.source)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["report"], ensure_ascii=True))


if __name__ == "__main__":
    main()
