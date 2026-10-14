"""Assemble source-linked, read-only audit of saved 22 Buff text.

Consumes audit_local22_text.py output. The optional p-code reader reconstructs
only literal data instructions; it neither executes AS nor emulates calls.
Every generated artifact is placed under the audit's local22 directory.
"""
from __future__ import annotations

from collections import Counter, defaultdict
from bisect import bisect_right
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parents[2]
AUDIT = REPO / "build-battle-reference/cross-resource-audit-20261009/local22"
LOCAL = Path("C:/Users/25982/Desktop/11/22")
LATEST = REPO / "build-battle-reference/latest-tables"


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(name, value):
    (AUDIT / name).write_text(json.dumps(value, ensure_ascii=False, indent=2,
                                        allow_nan=False), encoding="utf-8")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ref(path, number, raw=None):
    if raw is None:
        raw = path.read_text(encoding="utf-8-sig").splitlines()[number - 1]
    return {"path": str(path.resolve()), "line": number, "raw": raw}


def xml_rows(path, key):
    result = defaultdict(list)
    text = path.read_text(encoding="utf-8-sig")
    safe = re.sub(r"<!--.*?-->", lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), text, flags=re.S)
    newlines = [index for index, char in enumerate(text) if char == "\n"]
    found = 0
    for match in re.finditer(r"<s\b[^>]*?(?:/>|>\s*</s>)", safe, re.S):
        row = ET.fromstring(match[0])
        if key in row.attrib:
            found += 1
            number = bisect_right(newlines, match.start()) + 1
            raw = text[match.start():match.end()]
            result[row.attrib[key]].append({**ref(path, number, raw),
                                            "attributes": dict(row.attrib)})
    expected = sum(key in row.attrib for row in ET.fromstring(text).iter("s"))
    assert found == expected, (path, found, expected)
    return result


@dataclass(slots=True)
class Literal:
    value: object
    line: int


def read_literal_pcode(path, source_swf, target_ids, unnamed_ids, output_name):
    """Decode FFDec literal initializer only, rejecting any nonliteral opcode."""
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == "code") + 1
    stack = []
    counts = Counter()
    duplicates = []
    result = None
    for number in range(start, len(lines)):
        line = lines[number].strip()
        if not line:
            continue
        opcode, _, operand = line.partition(" ")
        counts[opcode] += 1
        if opcode in ("getlocal0", "pushscope", "findproperty"):
            # The initializer receiver/scope does not participate in literals.
            continue
        if opcode == "pushstring":
            value = json.loads(operand)
        elif opcode in ("pushint", "pushshort", "pushbyte"):
            value = int(operand)
        elif opcode == "pushdouble":
            value = float(operand)
            if not math.isfinite(value):
                raise ValueError(f"Non-finite literal at {path}:{number + 1}")
        elif opcode in ("pushtrue", "pushfalse", "pushnull"):
            value = {"pushtrue": True, "pushfalse": False, "pushnull": None}[opcode]
        elif opcode == "convert_s":
            assert isinstance(stack[-1].value, str), (path, number + 1)
            continue
        elif opcode in ("newarray", "newobject"):
            count = int(operand) * (2 if opcode == "newobject" else 1)
            assert len(stack) >= count, (path, number + 1, len(stack), count)
            children = stack[-count:] if count else []
            if count:
                del stack[-count:]
            if opcode == "newarray":
                value = children
            else:
                value = {}
                for key, item in zip(children[::2], children[1::2]):
                    assert isinstance(key.value, str), (path, number + 1)
                    if key.value in value:
                        duplicates.append({"line": number + 1, "key": key.value})
                    value[key.value] = item
        elif opcode == "initproperty":
            assert '"DATAS"' in operand and len(stack) == 1, (path, number + 1)
            result = stack.pop()
            break
        else:
            raise ValueError(f"Unsupported opcode {opcode} at {path}:{number + 1}")
        stack.append(Literal(value, number + 1))
    assert result is not None
    pattern = re.compile(r"(?<![0-9])(?:" + "|".join(target_ids) + r")(?![0-9])")
    references, cjk_keys, cjk_paths = [], Counter(), Counter()

    def visit(node, keypath):
        value = node.value
        if isinstance(value, dict):
            return {key: visit(child, keypath + [key]) for key, child in value.items()}
        if isinstance(value, list):
            return [visit(child, keypath + [str(index)]) for index, child in enumerate(value)]
        text = str(value)
        identifiers = sorted(set(pattern.findall(text)), key=int)
        if identifiers:
            references.append({"path": "/" + "/".join(keypath), "field": keypath[-1],
                               "ids": identifiers, "rawValue": value,
                               "pcodeLine": node.line})
        if isinstance(value, str) and re.search(r"[\u4e00-\u9fff]", value):
            cjk_keys[keypath[-1]] += 1
            cjk_paths["/" + "/".join("*" if part.isdigit() else part for part in keypath)] += 1
        return value

    data = visit(result, [])
    save(output_name, data)
    return {"sourceSwf": str(source_swf.resolve()), "sourceSha256": sha(source_swf),
            "pcodePath": str(path.resolve()), "pcodeSha256": sha(path),
            "pcodeLines": len(lines), "literalObjectEntries": len(data),
            "opcodes": dict(counts), "duplicateObjectKeys": duplicates,
            "searchedIds": target_ids, "allTargetReferences": references,
            "unnamedTargetReferences": [row for row in references if set(row["ids"]) & set(unnamed_ids)],
            "chineseTextFields": dict(cjk_keys), "chineseTextPathPatterns": dict(cjk_paths),
            "method": "Only literal push/convert_s/newarray/newobject parsed; unknown opcodes fail. No AS execution.",
            "dataArtifact": str((AUDIT / output_name).resolve())}


def classify_file(index, file):
    # Explicitly reviewed per-file classifications; all raw hits remain retained.
    groups = [
        ({27, 32}, "official-buff-table", "Same-ID official Buff rows; byte-equal to latest tables."),
        ({28}, "official-executor-table", "Explicit bufferId link; desc is an internal label, not a mechanics description."),
        ({29}, "official-buff-skill-relation", "buffs explicitly references 30823/30824; skillId and targetSkillIds are different namespace. No text field."),
        ({7, 26, 31, 34, 61}, "skill-id-collision", "Numeric ID belongs to SkillConfig/SkillDesc. Never use as Buff name by same-number join."),
        ({5, 9, 23, 24, 51, 52, 53, 54, 55, 56, 58}, "embedded-battle-record", "Buff occurrence in demo log; player/pet names describe log actors, not Buffs."),
        ({10, 11}, "formation-and-buff-references", "True .buff references separated from formation keys and pet race IDs. No Buff text definitions."),
        ({3}, "team-id-collision", "EB_SubLevelConfig constructor param2 is _teamId; adjacent description is a whole formation rule."),
        ({1, 2, 4, 6, 8, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 30, 33, 46, 48, 59, 60, 62, 70, 73, 74, 76},
         "other-config-namespace", "Pet race/skill, reward/material/survey/order namespaces; no evidence of a Buff text join."),
        ({47}, "activity-survey-id-collision", "surveyId attribute is an activity tracking ID, not a Buff ID."),
        ({13, 37, 38, 39, 40, 41, 42, 43, 44, 45, 49, 57, 71}, "incidental-numeric-hit", "Unicode/hexadecimal/geometry/runtime constants or symbols, not Buff text."),
        ({50}, "display-resource-version-reference", "battlebuffer asset names for 41851/41852/41853/42395 and version strings; no text definitions."),
        ({35}, "skill-movie-resource-reference", "200076 occurs inside battlemovie URL/class name; sf is 210076/220076. No Buff text or same-ID Buff link."),
        ({36, 68, 69, 72, 75, 90}, "captured-or-derived-battle-record", "Logged IDs / derived replay rows, not a distinct official text source."),
        ({0, 25, 63, 64, 65, 66, 67, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89},
         "human-analysis-or-derived-audit", "Source authority must be verified independently; includes valid table copies, author inferences and wrong-namespace historical lookups."),
    ]
    matches = [(kind, note) for indices, kind, note in groups if index in indices]
    assert len(matches) == 1, (index, file["relative"])
    kind, note = matches[0]
    return {"path": file["path"], "relative": file["relative"], "sha256": file["sha256"],
            "hitLines": len(file["hits"]), "ids": sorted(file["countsById"], key=int),
            "classification": kind, "finding": note}


def main():
    summary = read_json(AUDIT / "summary.json")
    hits = read_json(AUDIT / "text-hits.json")
    assert len(hits) == 91
    unnamed = summary["targets"]["unnamed"]
    undescribed = summary["targets"]["undescribed"]
    names = {"config": "BufferConfigXML", "description": "BufferDescConfigXML",
             "execute": "BufferExecuteXML", "combo": "ComboBuffsConfigXML"}
    tables, comparisons = {}, {}
    for key, name in names.items():
        local = next((LOCAL / "out/battleconfig_bin").glob(f"*data.{name}__*.bin"))
        latest = next((LATEST / "xml").glob(f"*data.{name}__*.xml"))
        comparisons[key] = {"localPath": str(local.resolve()), "latestPath": str(latest.resolve()),
                            "localSha256": sha(local), "latestSha256": sha(latest),
                            "bytesEqual": local.read_bytes() == latest.read_bytes()}
        if key != "combo":
            tables[key] = xml_rows(local, "bufferId" if key == "execute" else "id")
    rows = {}
    for identifier in undescribed:
        cfg = tables["config"].get(identifier, [])
        desc = tables["description"].get(identifier, [])
        executor = tables["execute"].get(identifier, [])
        current_cfg = cfg[-1]["attributes"] if cfg else {}
        current_desc = desc[-1]["attributes"] if desc else {}
        name = current_desc.get("nm") or current_cfg.get("nm") or ""
        assert not current_desc.get("mess"), identifier
        if identifier in unnamed:
            assert not name, identifier
        rows[identifier] = {
            "officialName": name, "nameSource": "flash-desc" if current_desc.get("nm") else "flash-config" if name else "none",
            "descriptionMess": current_desc.get("mess", ""),
            "effectTextBeffect": current_desc.get("beffect", ""), "bhs": current_desc.get("bhs", ""),
            "configRows": cfg, "descriptionRows": desc, "executeRows": executor,
            "newNameRecovered": False, "newDescriptionRecovered": False,
            "classification": "officially_unnamed" if identifier in unnamed else "official_name_without_mess",
        }
    per_file = [classify_file(index, file) for index, file in enumerate(hits)]
    save("file-classifications.json", per_file)
    save("target-buff-evidence.json", rows)

    formation = []
    for index in (10, 11):
        file = hits[index]
        fields = Counter()
        refs = []
        for hit in file["hits"]:
            line = hit["text"]
            match = re.search(r'^\s*"([^"\n]+)"\s*:', line)
            field = match[1] if match else "other"
            fields["numeric-formation-key" if field.isdigit() else field] += 1
            if field == "buff":
                value = re.search(r'"buff":"([^\"]*)"', line)[1]
                ids = [part.split(":")[0] for part in value.split("#")]
                ids = [identifier for identifier in ids if identifier in rows]
                if ids:
                    refs.append({"line": hit["line"], "buffIds": ids, "raw": line})
        formation.append({"path": file["path"], "hitFieldCounts": dict(fields), "actualBuffReferences": refs})
    save("formation-references.json", formation)

    pcode = []
    variants = [
        ("skin-demo-pcode", LOCAL / "swf/res/changepetskinv3_changepetskinservice.swf", "skin-demo-literal-data.json"),
        ("latest-skin-demo-pcode", REPO / "build-battle-reference/cross-resource-audit-20261009/flash-extra/changepetskinv3_changepetskinservice.swf", "latest-skin-demo-literal-data.json"),
    ]
    for directory, source, output in variants:
        path = next((AUDIT / directory).rglob("*.pcode"))
        pcode.append(read_literal_pcode(path, source, undescribed, unnamed, output))
    save("skin-demo-source-audit.json", pcode)

    valuable = []
    def finding(title, disposition, explanation, references):
        valuable.append({"title": title, "disposition": disposition,
                         "explanation": explanation, "references": references})
    descpath = Path(comparisons["description"]["localPath"])
    cfgpath = Path(comparisons["config"]["localPath"])
    finding("27 entries have no same-ID official name", "verified_official_absence_in_audited_tables",
            "All 27 have config entries, hd=1, did=645 and no nm; no matching named descriptor. did identifies visual resource, not description ID.",
            [record for identifier in unnamed for record in rows[identifier]["configRows"]])
    finding("beffect exists for 121 of the 253 missing-mess entries", "official_supplemental_field_only",
            "These are independent effect hints, often generic animation labels. Retain as beffect; do not rewrite them as full mechanics mess.",
            [record for identifier in ("14424", "100247") for record in rows[identifier]["descriptionRows"]])
    finding("Nine executor desc labels are already config names", "official_internal_label_only",
            "Explicit bufferId link is valid, but all nine desc values duplicate the corresponding config nm and provide no mechanics text.",
            [record for row in rows.values() for record in row["executeRows"]])
    finding("Same-number skill names are not Buff names", "reject_wrong_namespace_join",
            "The old report looked epbi up in skills. Buff 220483 is 灵初黑金究_通灵条, while skill 220483 is 暴龙泰山脚; the integer collision does not connect them.",
            [ref(Path(hits[65]["path"]), number) for number in (265, 359, 361, 366)]
            + rows["220483"]["configRows"] + rows["200047"]["configRows"])
    finding("Old statement that did=645 means intentionally nameless", "author_inference_not_official_semantics",
            "No nm is directly verified. The phrase 故意没名字 is the document author's interpretation; no missing name or individual Buff meaning follows from a shared display resource.",
            [ref(Path(hits[67]["path"]), 330)])
    finding("Descriptions next to EB_SubLevelConfig numeric matches describe a team", "reject_team_id_collision",
            "The constructor explicitly assigns param2 to _teamId. Matching 31331/31332/31333/31838 cannot be used as Buff definitions.",
            [ref(Path(hits[3]["path"]), number) for number in (53, 57, 59)])
    finding("ComboBuffs gives explicit skill-state relation without text", "official_relation_not_description",
            "30823/30824 are declared in buffs. The row maps base skillId to targetSkillIds and is useful relation evidence, not a name/description or proof of who applied a Buff in a particular log.",
            [ref(Path(hits[29]["path"]), 74)])
    finding("Newbie training embedded battle record", "official_occurrence_only",
            "200209/200076 are epbi and 200077/200076 occur in ad_bfs. These are actual Buff references, not same-number skill-name definitions.",
            [ref(Path(hits[5]["path"]), number) for number in (22, 116, 123)])
    finding("Pet descriptions cannot fill same-number Buff text", "reject_pet_race_id_collision",
            "7056 is raceId / PetInfoV2Define ID in this source. The adjacent name and mechanics summarize that pet, not Buff 7056.",
            [ref(Path(hits[20]["path"]), 875)])
    finding("Shared visual-resource join is not a descriptor join", "official_consumer_verified",
            "BufferConfig asks BufferDesc by row.@id. BufferDefine stores row.@did separately as displayId.",
            [ref(LOCAL / "out/battleconfig_as/scripts/mmo/battleconfig/data/BufferConfigXML.as", 50),
             ref(LOCAL / "out/interfaces_as/scripts/mmo/interfaces/battleconfig/define/BufferDefine.as", 46)])
    save("valuable-findings.json", valuable)
    overview = {
        "scope": summary,
        "sameSourceTables": comparisons,
        "targetCounts": {"unnamed": len(unnamed), "undescribedMess": len(undescribed),
                         "withDescriptorRow": sum(bool(row["descriptionRows"]) for row in rows.values()),
                         "withConfigName": sum(row["nameSource"] == "flash-config" for row in rows.values()),
                         "withEffectText": sum(bool(row["effectTextBeffect"]) for row in rows.values()),
                         "withExecutorDesc": sum(any(v["attributes"].get("desc") for v in row["executeRows"]) for row in rows.values()),
                         "newNamesRecovered": 0, "newMessRecovered": 0},
        "fileClassificationCounts": dict(Counter(row["classification"] for row in per_file)),
        "supplementalPcodeSources": [{k: v for k, v in row.items() if k not in ("allTargetReferences", "unnamedTargetReferences")} for row in pcode],
        "additionalDecodedText": read_json(AUDIT / "additional-text-hits.json"),
        "limits": [
            "The exhaustive lexical scan covers the inventoried decodable files, not every historical or future official resource.",
            "SWF/binary/package scanning is separately performed by summon_border_evidence; this report classifies saved text and selected literal demo initializers.",
            "Old source AS export of the skin Demo omitted its initializer due to FFDec timeout; p-code was successfully exported and literal data reconstructed without execution.",
            "No concrete alias for the 27 unnamed targets was found in reviewed local prose. Existing author paraphrases are not new official definitions.",
            "No arithmetic ID bridge for the proposed 300000/400000/500000 offsets was found in the scanned AS/JS outside literals/comments. No offset-based name inference was used.",
        ],
    }
    save("report.json", overview)
    report = [
        "22 目录 Buff 名称 / 说明复核（2026-10-09）", "",
        "结论：未从本次审核的 22 保存文本或补导出的官方演示数据中恢复这 27 项名称、253 项 mess 说明。",
        "缺 mess 不等于没有任何官方文字：其中 121 项有独立 beffect 提示，9 项有 Execute.desc 内部标签，均不可冒充完整机制说明。",
        "",
        f"范围：{summary['inventoryFiles']} 个文件，{summary['decodedTextFiles']} 份可解码文本，{summary['decodedTextBytes']} bytes；{summary['hitFiles']} 个命中文件，{summary['hitLines']} 行。",
        "所有匹配位置保存在 text-hits.json，所有命中文件已逐类审核，分类在 file-classifications.json。",
        "另复核 1 份因单个协议终止 NUL 被初筛跳过的 GB18030 文本 out/recheck_down5.txt（99,773 bytes），无目标 ID；总计 12,440 份本地文本已检查。",
        "每个目标 ID 的全部官方原行、路径、行号、各字段在 target-buff-evidence.json。",
        "", "可核验的关键发现：",
    ]
    for item in valuable:
        report += ["", item["title"], item["disposition"], item["explanation"]]
        for source in item["references"]:
            report += [f"{source['path']}:{source['line']}", source["raw"]]
    report += ["", "表字节一致性："]
    for name, item in comparisons.items():
        report += [f"{name}: bytesEqual={item['bytesEqual']}; sha256={item['localSha256']}", item["localPath"], item["latestPath"]]
    report += ["", "完整证据及边界见 report.json / valuable-findings.json / skin-demo-source-audit.json。"]
    (AUDIT / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(json.dumps(overview["targetCounts"], ensure_ascii=False))
    print("Artifacts:", AUDIT)


if __name__ == "__main__":
    main()
