"""Read-only numeric-reference inventory of saved local 22 text and tables.

This records lexical hits; it deliberately does not promote a nearby name to a
Buff definition. Binary/SWF auditing is performed separately. Source files are
never written. Output files live under the explicitly supplied audit directory.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


TEXT_SUFFIXES = {
    ".as", ".txt", ".xml", ".md", ".py", ".json", ".html", ".js", ".tsv",
    ".csv", ".log", ".ps1", ".sh", ".bat", ".properties", ".fnt", ".hex",
    ".policy", ".cfg", ".ja", ".jfc", ".src", ".access", ".template", ".security",
}


def decode(data):
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16"), "utf-16"
    if b"\x00" in data:
        return None, "binary-nul"
    for encoding in ("utf-8-sig", "gb18030"):
        try:
            return data.decode(encoding), encoding
        except UnicodeDecodeError:
            pass
    return None, "undecodable"


def audit(root: Path, coverage_path: Path, output: Path):
    coverage = json.loads(coverage_path.read_text(encoding="utf-8-sig"))
    entries = coverage["buffs"]["entries"]
    unnamed = {key for key, value in entries.items() if value["status"] == "officially_unnamed"}
    undescribed = {key for key, value in entries.items() if not value["hasDescription"]}
    targets = unnamed | undescribed
    pattern = re.compile(r"(?<![0-9])(?:" + "|".join(sorted(targets, key=lambda x: (-len(x), x))) + r")(?![0-9])")
    inventory, text_files, hit_files, xml_rows, errors = [], [], [], [], []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        stat = path.stat()
        item = {"path": str(path.resolve()), "relative": path.relative_to(root).as_posix(),
                "suffix": path.suffix.lower(), "bytes": stat.st_size}
        inventory.append(item)
        suffix = item["suffix"]
        if suffix not in TEXT_SUFFIXES and suffix not in ("", ".bin", ".dat"):
            continue
        try:
            data = path.read_bytes()
            text, encoding = decode(data)
            if text is None:
                item["textScan"] = encoding
                continue
            item["textScan"] = "decoded"
            meta = {**item, "encoding": encoding, "sha256": hashlib.sha256(data).hexdigest()}
            text_files.append(meta)
            counts, hits = Counter(), []
            offset = 0
            for line_number, line in enumerate(text.splitlines(keepends=True), 1):
                matches = list(pattern.finditer(line))
                if matches:
                    ids = list(dict.fromkeys(match[0] for match in matches))
                    counts.update(match[0] for match in matches)
                    hit = {"line": line_number, "characterOffset": offset, "ids": ids,
                           "occurrences": len(matches), "lineLength": len(line.rstrip("\r\n"))}
                    if len(line) <= 1800:
                        hit["text"] = line.rstrip("\r\n")
                    else:
                        hit["excerpts"] = []
                        for identifier in ids:
                            match = next(match for match in matches if match[0] == identifier)
                            begin, end = max(0, match.start() - 180), min(len(line), match.end() + 400)
                            hit["excerpts"].append({"id": identifier, "column": match.start() + 1,
                                                    "startColumn": begin + 1, "text": line[begin:end].rstrip("\r\n")})
                    hits.append(hit)
                offset += len(line)
            if hits:
                hit_files.append({**meta, "countsById": dict(counts), "hits": hits})
            if suffix in (".xml", ".bin", ".dat") or text.lstrip().startswith("<?xml"):
                try:
                    tree = ET.fromstring(text)
                except ET.ParseError:
                    continue
                for row_index, element in enumerate(tree.iter()):
                    id_matches = {key: value for key, value in element.attrib.items()
                                  if value in targets and key.lower() in ("id", "buffid", "bufferid", "bid", "skillid")}
                    if id_matches:
                        serialized = ET.tostring(element, encoding="unicode")
                        # A source-line match is recorded separately; this row is
                        # the complete exact parsed attribute set, not an alias.
                        xml_rows.append({"path": meta["path"], "sha256": meta["sha256"],
                                         "rootTag": tree.tag, "elementIndex": row_index,
                                         "tag": element.tag, "matchedAttributes": id_matches,
                                         "attributes": dict(element.attrib), "text": element.text,
                                         "serialized": serialized if len(serialized) <= 5000 else None})
        except (OSError, UnicodeError, ValueError) as error:
            errors.append({"path": str(path), "error": str(error)})
    summary = {
        "root": str(root.resolve()), "coveragePath": str(coverage_path.resolve()),
        "coverageSha256": hashlib.sha256(coverage_path.read_bytes()).hexdigest(),
        "targets": {"unnamed": sorted(unnamed, key=int), "undescribed": sorted(undescribed, key=int)},
        "inventoryFiles": len(inventory), "inventoryBytes": sum(item["bytes"] for item in inventory),
        "inventorySuffixCounts": dict(Counter(item["suffix"] or "<none>" for item in inventory)),
        "decodedTextFiles": len(text_files), "decodedTextBytes": sum(item["bytes"] for item in text_files),
        "decodedSuffixCounts": dict(Counter(item["suffix"] or "<none>" for item in text_files)),
        "hitFiles": len(hit_files), "hitLines": sum(len(item["hits"]) for item in hit_files),
        "hitSuffixCounts": dict(Counter(item["suffix"] or "<none>" for item in hit_files)),
        "hitIds": sorted({key for item in hit_files for key in item["countsById"]}, key=int),
        "xmlMatchingRows": len(xml_rows), "errors": errors,
        "boundary": "Lexical IDs and exact parsed XML attributes only; names require source verification. SWF, packet capture and other binary payloads audited separately.",
    }
    output.mkdir(parents=True, exist_ok=True)
    for name, value in (("inventory.json", inventory), ("decoded-files.json", text_files),
                        ("text-hits.json", hit_files), ("xml-rows.json", xml_rows), ("summary.json", summary)):
        (output / name).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("coverage", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = audit(args.root, args.coverage, args.output)
    print(json.dumps({key: value for key, value in result.items() if key not in ("targets", "hitIds")}, ensure_ascii=False))
