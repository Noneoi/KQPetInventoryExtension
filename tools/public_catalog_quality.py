"""Read-only, namespace-aware catalog audit and conservative update regression gate.

This audits generated outputs. Source coverage is a separate audit: observing a
field here does not prove that all official input fields have been understood.
Existing official gaps are retained, not filled or reclassified as parser bugs.
New losses require review; valid new definitions and legitimate empty text are
reported without synthesizing names, relationships, equipment or battle effects.
"""
from __future__ import annotations

from collections import Counter, defaultdict
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

SCHEMA_VERSION = 1
_ABSENT = object()
_METADATA = frozenset({"source", "sources", "textSource", "configSource", "nameSource",
    "descriptionSource", "sourcePath", "sourceFile", "sourceRow", "sourceFields",
    "importEvidence", "evidence", "consumerEvidence", "referenceConsumption"})
_TEXT = frozenset({"name", "nm", "description", "desc", "mess", "canonicalName"})
_REPLAY_TABLES = (
    "pets", "skills", "buffs", "attributes", "execution", "bufferExecution", "combos",
    "skillLastEffects", "buffPowerFullMap", "transformSkills", "tableRelations",
    "relationsBySkill", "relationsByBuff", "officialEntries", "officialEntryMentions",
    "entryMentionsByBuff", "entryMentionsBySkill", "petMechanics", "mechanismRules", "legendMechanics/definitions",
    "huanMechanics", "petDragons/definitions", "petDragons/qualities",
    "petDragons/geneRule", "clientPresentation",
    *("clientPresentation/" + key for key in ("attributeIndex", "formations", "jobs", "pets")),
    "replayCultivation/stargods/definitions", "replayCultivation/badges/definitions",
    "replayCultivation/astrolabe/definitions", "replayCultivation/propertyTypes",
    "replayCultivation/astrolabeLocationTypes", "replayCultivation/links",
    *("replaySupplements/" + key + "/rows" for key in
      ("summon", "carry", "almighty", "hero", "psychics", "formationTeams", "powerFull", "titles")),
    # These are the explicit indexes built by extract_replay_supplements, not
    # inferred Buff/skill relationships. Keep source order and duplicate history.
    *("replaySupplements/" + group + "/" + field for group, fields in (
        ("summon", ("byPair", "historyByPair", "bySourceRaceId", "hiddenSourceRaceIds", "loadOrder")),
        ("carry", ("byPair", "historyByPair", "bySourceRaceId", "byDeclaredSourceRaceId")),
        ("almighty", ("byRaceId", "historyByRaceId")),
        ("hero", ("bySkillId",)), ("psychics", ("bySkillId",)),
        ("formationTeams", ("byId", "historyById")),
        ("powerFull", ("byRaceId", "raceIdOverrides", "rawMappingRows")),
        ("titles", ("byId", "historyById", "recordSchema")),
    ) for field in fields),
    "replaySupplements/powerFull", "replaySupplements/titles", "replaySupplements/relations",
    *("replaySupplements/resources/" + group + "/" + field for group, fields in (
        ("avatar", ("rows", "byId", "historyById")),
        ("changeAvatar", ("rows", "byId", "historyById")),
        ("actionMovie", ("rows", "byId", "historyById")),
        ("preDownload", ("rows", "byId", "historyById", "byMode")),
        ("background", ("rows", "byId", "historyById", "byScene")),
        ("loading", ("rows", "byId", "versionRules", "downloadFailureRule")),
    ) for field in fields),
)
_PROFILES = {
    "battleReplay": _REPLAY_TABLES,
    "skills": ("pets", "skills", "buffs", "entries", "combos", "transformSkills", "heroSkills",
               "psychicsSkills", "lingchuRules", "shenyunRules", "almightySkills"),
    "pets": ("pets", "stargods", "badges", "astrolabe", "sacredEquipment", "items", "attributes",
             "jobs", "money", "sourceBeasts", "legendStones", "proficiencies",
             "sacredStarPlans", "sacredStagePlans", "fusionJobs"),
}


def _pointer(*parts):
    return "/" + "/".join(str(part).replace("~", "~0").replace("/", "~1") for part in parts)


def _get(root, path):
    value = root
    for key in path.split("/"):
        if not isinstance(value, dict) or key not in value:
            return _ABSENT
        value = value[key]
    return value


def _type(value):
    if value is None:
        return "null"
    if type(value) is bool:
        return "boolean"
    if type(value) in (int, float):
        return "number"
    return {dict: "object", list: "array", str: "string"}.get(type(value), "invalid")


def _semantic(value):
    if isinstance(value, dict):
        return {key: _semantic(child) for key, child in value.items() if key not in _METADATA}
    if isinstance(value, list):
        return [_semantic(child) for child in value]
    return value


def _digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _same(before, after):
    # Python equates False with 0; the JSON consumer does not.
    if _type(before) != _type(after):
        return False
    if isinstance(before, dict):
        return before.keys() == after.keys() and all(_same(value, after[key]) for key, value in before.items())
    if isinstance(before, list):
        return len(before) == len(after) and all(_same(a, b) for a, b in zip(before, after))
    return before == after


def _rows(value):
    if isinstance(value, dict):
        return value
    if isinstance(value, list):
        return {str(index): row for index, row in enumerate(value)}
    return {}


def _table_rows(value, table):
    """Expose output rows, keeping evidence/counters out of new mixed records.

    The excluded names below are explicit extractor packaging fields. Nested
    data maps are audited separately so a pet/formation ID remains a row ID,
    instead of being expanded into thousands of apparent schema field names.
    This inventory does not assert that an index has been re-derived correctly.
    """
    excluded = {
        "clientPresentation": {"schemaVersion", "scope", "sourceEvidence", "attributeIndex", "formations", "jobs", "pets"},
        "petDragons/geneRule": {"source"},
        "replaySupplements/powerFull": {
            "rows", "byRaceId", "raceIdOverrides", "rawMappingRows", "overrideSources", "overrideHistory",
            "scope", "defaultSource", "consumerEvidence", "lookupScope"},
        "replaySupplements/titles": {
            "rows", "byId", "historyById", "recordSchema", "status", "namespace", "definitionStatus", "reason", "scope",
            "rawSource", "resourceVerificationSource", "resourceVerificationScope", "resourceVerificationIssues",
            "unmatchedResourceVerificationIds", "verifiedResourceCount"},
        "replaySupplements/resources/loading/versionRules": {"status", "source", "consumerEvidence"},
        "replaySupplements/resources/loading/downloadFailureRule": {"source", "consumerEvidence"},
    }.get(table, ())
    return {key: row for key, row in _rows(value).items() if key not in excluded}


def _field_inventory(rows):
    fields = defaultdict(Counter)
    def visit(value, parts):
        fields[_pointer(*parts)][_type(value)] += 1
        if isinstance(value, dict):
            for key, child in value.items():
                visit(child, (*parts, key))
        elif isinstance(value, list):
            for child in value:
                visit(child, (*parts, "*"))
    for row in rows.values():
        visit(row, ("*",))
    return [{"path": key, "types": dict(sorted(value.items()))} for key, value in sorted(fields.items())]


def _issue(kind, path, message, severity="review", **extra):
    return dict(kind=kind, path=path, message=message, severity=severity, **extra)


def _positive(value):
    if type(value) not in (int, str) or not re.fullmatch(r"[+-]?[0-9]+", str(value).strip()):
        return None
    return str(int(value)) if int(value) > 0 else None


def _replay_references(root):
    """Recompute endpoints and reverse indexes; ignore cached resolved flags."""
    findings = []
    skills, buffs, pets = (root.get(key, {}) for key in ("skills", "buffs", "pets"))
    if any(not isinstance(value, dict) for value in (skills, buffs, pets)):
        return [_issue("invalid-table", "/", "核心资料表类型不正确", "error")]
    from battle_replay.catalog_pack import _valid_row
    for table in ("pets", "skills", "buffs", "attributes", "execution", "bufferExecution"):
        for key, row in _rows(root.get(table)).items():
            if not isinstance(row, dict) or not _valid_row(table, key, row):
                findings.append(_issue("invalid-row-schema", _pointer(table, key), "条目缺少必要字段或类型不正确", "error"))
    for table, field in (("skills", "id"), ("buffs", "id"), ("attributes", "id"),
                         ("execution", "code"), ("bufferExecution", "bufferId")):
        for key, row in _rows(root.get(table)).items():
            if not isinstance(row, dict) or row.get(field) != key:
                findings.append(_issue("row-id-mismatch", _pointer(table, key, field), "条目身份与所在表不一致", "error"))

    expected_skill, expected_buff = defaultdict(list), defaultdict(list)
    identifiers = set()
    relations = root.get("tableRelations", [])
    if not isinstance(relations, list):
        return findings + [_issue("invalid-relations", "/tableRelations", "关联表类型不正确", "error")]
    for index, row in enumerate(relations):
        path = _pointer("tableRelations", index)
        if not isinstance(row, dict) or not isinstance(row.get("id"), str) or not row["id"]:
            findings.append(_issue("invalid-relation", path, "关联记录缺少身份", "error")); continue
        identifier = row["id"]
        if identifier in identifiers:
            findings.append(_issue("duplicate-relation", path + "/id", "关联身份重复", "error"))
        identifiers.add(identifier)
        kind = row.get("kind")
        if kind not in ("combo_buff_playback", "buffer_execute_skill", "buffer_execute_lighted_skill"):
            findings.append(_issue("unclassified-relation", path + "/kind", "新增关联类型尚未核对", "error")); continue
        fields = ("skillId", "targetSkillId") if kind == "combo_buff_playback" else ("skillId",)
        for field in (*fields, "buffId"):
            value = row.get(field)
            namespace = "buffs" if field == "buffId" else "skills"
            if not isinstance(value, str) or not value:
                findings.append(_issue("invalid-reference", path + "/" + field, "关联端点缺失或类型不正确", "error"))
            elif value not in root[namespace]:
                findings.append(_issue("missing-reference", path + "/" + field, "明确引用的定义未找到", targetNamespace=namespace, targetId=value))
        for value in dict.fromkeys(row.get(field) for field in fields if isinstance(row.get(field), str)):
            expected_skill[value].append(identifier)
        if isinstance(row.get("buffId"), str):
            expected_buff[row["buffId"]].append(identifier)
        source = row.get("sourceFields")
        if "sourceFields" in row and not isinstance(source, dict):
            findings.append(_issue("invalid-source-fields", path + "/sourceFields", "原始关联字段不是对象", "error"))
        if "priorityIndex" in row and (type(row["priorityIndex"]) is not int or row["priorityIndex"] < 0):
            findings.append(_issue("invalid-reference", path + "/priorityIndex", "组合优先顺序类型不正确", "error"))
        if isinstance(source, dict):
            expected = {}
            if kind == "combo_buff_playback":
                position = row.get("priorityIndex")
                if type(position) is int and position >= 0:
                    left, right = str(source.get("buffs", "")).split(","), str(source.get("targetSkillIds", "")).split(",")
                    if position < len(left) and position < len(right):
                        expected = {"skillId": source.get("skillId"), "buffId": left[position], "targetSkillId": right[position]}
            else:
                field = "lightedSkillId" if kind == "buffer_execute_lighted_skill" else "skillId"
                expected = {"buffId": source.get("bufferId"), "skillId": source.get(field)}
            if not expected or any(str(value) != row.get(key) for key, value in expected.items()):
                findings.append(_issue("source-reference-mismatch", path, "生成的关联与所保存的官方原字段不一致", "error"))
    for name, expected in (("relationsBySkill", expected_skill), ("relationsByBuff", expected_buff)):
        actual = root.get(name, {})
        if not isinstance(actual, dict):
            findings.append(_issue("invalid-index", _pointer(name), "反向索引类型不正确", "error")); continue
        for key in sorted(set(actual) | set(expected)):
            value = actual.get(key, [])
            if (not isinstance(value, list) or any(not isinstance(item, str) for item in value)
                    or Counter(value) != Counter(expected.get(key, []))):
                findings.append(_issue("reverse-index-mismatch", _pointer(name, key), "反向索引漏项、重复或对应到了其他对象", "error"))
    expected_edges, actual_edges = Counter(), Counter()
    combos = root.get("combos", [])
    if not isinstance(combos, list):
        findings.append(_issue("invalid-combos", "/combos", "组合技原表类型不正确", "error")); combos = []
    for index, row in enumerate(combos):
        if not isinstance(row, dict):
            findings.append(_issue("invalid-combo", _pointer("combos", index), "组合技原字段结构不正确", "error")); continue
        left, right = str(row.get("buffs", "")).split(","), str(row.get("targetSkillIds", "")).split(",")
        if len(left) != len(right):
            findings.append(_issue("invalid-combo", _pointer("combos", index), "组合技条件与目标数量不一致", "error"))
        for priority, (buff, target) in enumerate(zip(left, right)):
            skill, buff, target = _positive(row.get("skillId")), _positive(buff), _positive(target)
            if skill and buff and target:
                expected_edges[("combo_buff_playback", skill, buff, target, priority)] += 1
    for key, row in _rows(root.get("bufferExecution")).items():
        if not isinstance(row, dict):
            continue
        for field, kind in (("skillId", "buffer_execute_skill"), ("lightedSkillId", "buffer_execute_lighted_skill")):
            skill, buff = _positive(row.get(field)), _positive(row.get("bufferId"))
            if skill and buff:
                expected_edges[(kind, skill, buff, "", -1)] += 1
    for row in relations:
        if (isinstance(row, dict) and all(isinstance(row.get(key, ""), str) for key in ("kind", "skillId", "buffId", "targetSkillId"))
                and type(row.get("priorityIndex", -1)) is int):
            actual_edges[(row.get("kind"), row.get("skillId"), row.get("buffId"), row.get("targetSkillId", ""), row.get("priorityIndex", -1))] += 1
    if expected_edges != actual_edges:
        findings.append(_issue("source-graph-mismatch", "/tableRelations", "关联图未完整对应当前组合技与队伍执行原字段", "error",
            missingEdges=[{"edge": list(edge), "count": count} for edge, count in (expected_edges - actual_edges).items()],
            extraEdges=[{"edge": list(edge), "count": count} for edge, count in (actual_edges - expected_edges).items()]))
    # Existing audited gaps are recomputed in their exact namespace, with each
    # referring owner retained so a new owner cannot borrow an old exception.
    from battle_replay.audit_reference_coverage import audit_reference_coverage
    try:
        coverage = audit_reference_coverage(root)
    except (ValueError, TypeError, KeyError) as error:
        findings.append(_issue("invalid-reference-shape", "/petMechanics", "明确引用结构不合法", "error", detail=str(error)))
        return findings
    for item in coverage["items"]:
        status = item["status"]
        if status == "official-text-empty":
            for field in item["evidence"]["emptyFields"]:
                namespace = "buffs" if item["category"] == "buff-text" else "skills"
                findings.append(_issue(status, _pointer(namespace, item["id"], field), "官方名称或独立说明为空；不补造内容", "info"))
        elif status == "identity-unresolved":
            findings.append(_issue(status, _pointer("pets", item["id"], "identity"), "原名对应尚未证实", "info"))
        else:
            for reference in item.get("references", []) or [{}]:
                namespace = reference.get("namespace", "officialEntryMentions")
                owner = reference.get("id", item["id"])
                field = reference.get("field", "")
                if item["category"] == "pet-skill-reference":
                    path = _pointer(namespace, owner, "skillLinks", reference["linkIndex"], "skillId")
                elif item["category"] == "mechanism-plan-reference":
                    path = _pointer(namespace, owner, "qiYunType")
                else:
                    path = _pointer(namespace, owner, *field.split("."))
                findings.append(_issue(status, path, item["reasonLabel"],
                    "review" if status == "missing-reference" else "info",
                    targetNamespace=item["category"], targetId=item["id"]))
    return findings


def _workbench_references(root, component):
    # Only fields whose generator explicitly declares the target namespace.
    # No substring matching of descriptions or same-number cross-table joins.
    findings = []
    def invalid(path, message, namespace, identifier=""):
        findings.append(_issue("invalid-reference-shape", path, message, "error",
                               targetNamespace=namespace, targetId=identifier))

    def definitions(name):
        value = root.get(name, {})
        if not isinstance(value, dict):
            invalid(_pointer(name), "引用目标表必须是按 ID 索引的对象", name)
            return None
        return value

    pets = definitions("pets")
    def reference(value, path, namespace, targets, *, allow_zero=False, message=None):
        # Generated workbench endpoints are integers. Do not coerce strings,
        # floats or booleans into an ID and thereby hide malformed output.
        if type(value) is not int or value < (0 if allow_zero else 1):
            invalid(path, "引用 ID 必须是非负整数" if allow_zero else "引用 ID 必须是正整数",
                    namespace, str(value) if type(value) in (int, str) else "")
            return None
        if value == 0:
            return None
        target = str(value)
        if targets is not None and target not in targets:
            findings.append(_issue("missing-reference", path,
                message or "明确引用的定义未在对应资料表找到",
                targetNamespace=namespace, targetId=target))
        return target

    def array(value, path, namespace):
        if not isinstance(value, list):
            invalid(path, "引用集合必须是数组", namespace)
            return []
        return value

    if component == "pets":
        parents = {}
        for key, row in _rows(pets).items():
            if not isinstance(row, dict):
                invalid(_pointer("pets", key), "精灵记录必须是对象", "pets", key)
                continue
            if "groupRaceId" not in row:
                continue
            target = reference(row["groupRaceId"], _pointer("pets", key, "groupRaceId"),
                "pets", pets, allow_zero=True, message="明确的精灵归组引用缺失")
            if target and target != key:
                parents[key] = target
        for key in parents:
            visited, cursor = set(), key
            while cursor in parents and cursor not in visited:
                visited.add(cursor); cursor = parents[cursor]
            if cursor in visited:
                findings.append(_issue("identity-cycle", _pointer("pets", key, "groupRaceId"),
                    "精灵归组存在非自指循环", "error", targetNamespace="pets", targetId=parents[key]))
        return findings
    skills, buffs = definitions("skills"), definitions("buffs")
    combos = root.get("combos", {})
    if not isinstance(combos, dict):
        invalid(_pointer("combos"), "组合技必须按来源技能 ID 索引", "skills")
        combos = {}
    for key, rules in combos.items():
        path = _pointer("combos", key)
        if not isinstance(key, str) or not re.fullmatch(r"[1-9][0-9]*", key):
            invalid(path, "组合技来源必须是正整数技能 ID 键", "skills", str(key))
        elif skills is not None and key not in skills:
            findings.append(_issue("missing-reference", path, "组合技来源技能未在技能表找到",
                                   targetNamespace="skills", targetId=key))
        for index, rule in enumerate(array(rules, path, "skills")):
            parts = ("combos", key, index)
            if not isinstance(rule, dict):
                invalid(_pointer(*parts), "组合技条件与目标记录必须是对象", "skills")
                continue
            reference(rule.get("buff"), _pointer(*parts, "buff"), "buffs", buffs,
                message="组合技条件 Buff 未在工作台说明表收录；不据此认定官方 Buff 定义缺失")
            reference(rule.get("skill"), _pointer(*parts, "skill"), "skills", skills,
                message="组合技目标技能未在技能表找到")

    for key, pet in _rows(pets).items():
        if not isinstance(pet, dict):
            invalid(_pointer("pets", key), "精灵记录必须是对象", "pets", key)
            continue
        if "slots" in pet:
            if not isinstance(pet["slots"], dict):
                invalid(_pointer("pets", key, "slots"), "技能槽必须是按槽名索引的对象", "skills")
            else:
                for slot, identifier in pet["slots"].items():
                    reference(identifier, _pointer("pets", key, "slots", slot), "skills", skills,
                        allow_zero=True, message="精灵明确引用的技能未在技能表找到")
        if "evolutionFrom" in pet:
            reference(pet["evolutionFrom"], _pointer("pets", key, "evolutionFrom"),
                      "pets", pets, allow_zero=True)
        if "forms" in pet:
            for index, identifier in enumerate(array(pet["forms"], _pointer("pets", key, "forms"), "pets")):
                reference(identifier, _pointer("pets", key, "forms", index), "pets", pets)
        # These fields are constructed with explicit race-ID endpoints by
        # generate_pet_skill_data, unlike descriptive text and rule codes.
        for field, endpoint, many in (("skins", "raceId", False),
                                      ("summons", "related", True),
                                      ("carry", "related", True),
                                      ("relations", "members", True),
                                      ("almighty", "calledRaceId", False)):
            if field not in pet:
                continue
            for index, row in enumerate(array(pet[field], _pointer("pets", key, field), "pets")):
                parts = ("pets", key, field, index)
                if not isinstance(row, dict):
                    invalid(_pointer(*parts), "精灵关联记录必须是对象", "pets")
                    continue
                path = _pointer(*parts, endpoint)
                if many:
                    for related_index, identifier in enumerate(array(row.get(endpoint), path, "pets")):
                        reference(identifier, _pointer(*parts, endpoint, related_index), "pets", pets)
                else:
                    reference(row.get(endpoint), path, "pets", pets, allow_zero=field == "almighty")
    return findings


def audit_catalog_quality(candidate, previous=None, *, component="battleReplay"):
    if component not in _PROFILES:
        raise ValueError("Unsupported catalog quality profile: " + component)
    if not isinstance(candidate, dict) or (previous is not None and not isinstance(previous, dict)):
        raise ValueError("Catalog quality inputs must be objects")
    old = previous or {}
    findings, changes, tables = [], [], []
    def change(kind, path, before=_ABSENT, after=_ABSENT, severity="info"):
        row = dict(kind=kind, path=path)
        for label, value in (("before", before), ("after", after)):
            if value is not _ABSENT:
                row[label + "Type"] = _type(value)
                row[label + "Sha256"] = _digest(value)
        changes.append(row)
        if severity != "info":
            findings.append(_issue(kind, path, "原有资料发生退化，需核对官方原文与解析结果", severity, baselineStatus="new"))
    def compare(before, after, parts):
        path = _pointer(*parts)
        if _type(before) != _type(after):
            change("field-type-changed", path, before, after, severity="review"); return
        if isinstance(before, dict):
            for key in sorted(set(before) | set(after)):
                if key in _METADATA:
                    continue
                if key not in after:
                    change("field-removed", _pointer(*parts, key), before[key], severity="review")
                elif key not in before:
                    change("field-added", _pointer(*parts, key), after=after[key])
                else:
                    compare(before[key], after[key], (*parts, key))
        elif isinstance(before, list):
            if len(after) < len(before):
                change("array-shortened", path, before, after, severity="review")
            for index in range(min(len(before), len(after))):
                compare(before[index], after[index], (*parts, index))
        elif isinstance(before, str) and parts[-1] in _TEXT and before.strip() and not after.strip():
            change("text-cleared", path, before, after, severity="review")
        elif parts[-1] == "status" and parts[-2:-1] == ("identity",) and before in ("native", "skin") and after not in ("native", "skin"):
            change("identity-regressed", path, before, after, severity="review")
        elif parts[-1] == "playbackAvailable" and before is True and after is False:
            change("playback-unavailable", path, before, after, severity="review")
    mandatory = {"pets", "skills", "buffs"} if component == "battleReplay" else {"pets", "skills"} if component == "skills" else {"pets"}
    for table in _PROFILES[component]:
        value, prior = _get(candidate, table), _get(old, table)
        path = _pointer(*table.split("/"))
        present = value is not _ABSENT
        rows, earlier = _table_rows(value, table), _table_rows(prior, table)
        tables.append({"path": path, "present": present, "rows": len(rows), "fields": _field_inventory(rows),
                       "coverage": "observed-output-fields" if present else "not-provided"})
        if not present:
            if table in mandatory:
                findings.append(_issue("missing-table", path, "必要输出表未提供", "error", baselineStatus="new"))
            elif prior is not _ABSENT:
                change("table-removed", path, prior, severity="review")
            continue
        if not isinstance(value, (dict, list)):
            findings.append(_issue("invalid-table", path, "资料表类型不正确", "error", baselineStatus="new")); continue
        if prior is not _ABSENT and _type(prior) != _type(value):
            change("field-type-changed", path, prior, value, severity="review")
        for key in sorted(set(rows) | set(earlier)):
            parts = (*table.split("/"), key)
            if key not in rows:
                change("row-removed", _pointer(*parts), earlier[key], severity="review")
            elif key not in earlier:
                change("row-added", _pointer(*parts), after=rows[key])
            elif not _same(_semantic(earlier[key]), _semantic(rows[key])):
                change("row-changed", _pointer(*parts), _semantic(earlier[key]), _semantic(rows[key]))
                compare(earlier[key], rows[key], parts)
    references = _replay_references if component == "battleReplay" else lambda root: _workbench_references(root, component)
    if component == "battleReplay":
        old_relations = {row["id"]: row for row in old.get("tableRelations", []) if isinstance(row, dict) and isinstance(row.get("id"), str)}
        for index, row in enumerate(candidate.get("tableRelations", []) if isinstance(candidate.get("tableRelations"), list) else []):
            if not isinstance(row, dict) or not isinstance(row.get("id"), str):
                continue
            prior = old_relations.get(row["id"], {})
            if "sourceFields" in prior and "sourceFields" not in row:
                change("field-removed", _pointer("tableRelations", index, "sourceFields"), prior["sourceFields"], severity="review")
    current_refs = references(candidate)
    previous_refs = references(old) if previous is not None else []
    def signature(row):
        return tuple(row.get(key, "") for key in ("kind", "path", "targetNamespace", "targetId"))
    old_refs = {signature(row): row for row in previous_refs}
    current_keys = {signature(row) for row in current_refs}
    for row in current_refs:
        item = dict(row, baselineStatus="existing" if signature(row) in old_refs else "new")
        if item["baselineStatus"] == "existing" and item["severity"] == "review":
            item["severity"] = "info"
        findings.append(item)
    for key in sorted(set(old_refs) - current_keys):
        findings.append(dict(old_refs[key], severity="info", baselineStatus="resolved"))
    counts = Counter(row["severity"] for row in findings)
    return {"schemaVersion": SCHEMA_VERSION, "component": component, "passed": not (counts["error"] or counts["review"]),
        "summary": {"errors": counts["error"], "reviewRequired": counts["review"], "information": counts["info"],
            "existingFindings": sum(row["baselineStatus"] == "existing" for row in findings),
            "resolvedFindings": sum(row["baselineStatus"] == "resolved" for row in findings),
            "tablesProvided": sum(row["present"] for row in tables), "tablesTracked": len(tables),
            "changesByKind": dict(Counter(row["kind"] for row in changes))},
        "tables": tables, "changes": changes, "findings": findings,
        "scope": "已声明输出表、字段退化与明确命名空间引用；输入覆盖由独立来源审计负责。不是服务器计算或全部游戏资源覆盖证明。",
        "policy": "既有缺项保留；新增合法空说明不补造；新增断链和既有资料丢失需核对；结构与索引错误禁止发布。"}


class CatalogQualityError(ValueError):
    def __init__(self, metadata):
        self.metadata = metadata
        summary = metadata["summary"]
        super().__init__(f"解析质量检查未通过：{summary['errors']} 项错误、{summary['reviewRequired']} 项变化待核对；现有资料保留")


def _merge_fidelity(report, candidate, previous, fidelity):
    """Accept only an independent audit bound to these exact output values.

    The updater calls the source reader itself. No receipts are read from
    candidate metadata. A correction can remove a proven legacy bad join, but
    cannot pardon a malformed record, unrelated field loss, or failed audit.
    """
    report['sourceFidelity'] = deepcopy(fidelity)
    valid = isinstance(fidelity, dict) and fidelity.get('schemaVersion') == 1 \
        and fidelity.get('candidateContentSha256') == _digest(candidate) \
        and isinstance(fidelity.get('inputs'), (list, dict)) and bool(fidelity['inputs']) \
        and isinstance(fidelity.get('findings'), list) and type(fidelity.get('passed')) is bool
    if valid:
        valid = all(isinstance(row, dict) and row.get('severity') in ('error', 'review', 'info')
                    and all(isinstance(row.get(key), str) for key in ('kind', 'path', 'message'))
                    for row in fidelity['findings'])
    if not valid:
        report['findings'].append(_issue('source-fidelity-invalid', '/',
            '官方来源核对报告缺失、格式无效或与本次候选资料不匹配', 'error', baselineStatus='new'))
        return
    report['findings'].extend(dict(row, baselineStatus='source-audit') for row in fidelity['findings'])
    if not fidelity['passed'] or any(row['severity'] != 'info' for row in fidelity['findings']):
        if not any(row['severity'] != 'info' for row in fidelity['findings']):
            report['findings'].append(_issue('source-fidelity-failed', '/',
                '候选资料与官方原始记录的独立核对未通过', 'error', baselineStatus='new'))
        return
    if report['component'] not in ('skills', 'battleReplay'):
        return
    def value_at(root, path):
        value = root
        for part in path.lstrip('/').split('/'):
            if isinstance(value, dict):
                value = value.get(part, _ABSENT)
            elif isinstance(value, list) and part.isdecimal() and int(part) < len(value):
                value = value[int(part)]
            else:
                return _ABSENT
        return value
    is_skill = report['component'] == 'skills'
    allowed_path = (r'/pets/[0-9]+/(huan|almighty|traits)' if is_skill else
                    r'/replaySupplements/hero/rows/[0-9]+/(effectiveRaceIds|readings/[0-9]+/value)')
    for receipt in fidelity.get('allowedCorrections', []):
        if not isinstance(receipt, dict) or not re.fullmatch(allowed_path, receipt.get('path', '')):
            continue
        path = receipt['path']
        before, after = value_at(previous or {}, path), value_at(candidate, path)
        if not receipt.get('source') or not receipt.get('rule'):
            continue
        if any(receipt.get(label + 'Present') is not (value is not _ABSENT)
               or receipt.get(label + 'ValueSha256') != (_digest(value) if value is not _ABSENT else None)
               for label, value in (('previous', before), ('candidate', after))):
            continue
        for row in report['findings']:
            kinds = ('field-removed', 'array-shortened', 'text-cleared', 'field-type-changed') if is_skill else ('array-shortened',)
            if row['severity'] == 'review' and row['kind'] in kinds \
                    and (row['path'] == path or (is_skill and row['path'].startswith(path + '/'))):
                row.update(severity='info', resolution=('legacy-association-corrected-from-official-consumer' if is_skill else
                                                      'derived-display-race-set-verified-from-current-official-input'),
                           officialInputEvidence=deepcopy(receipt))


def enforce_catalog_quality(component, candidate, previous, report_directory, *, source_coverage=None,
                            reference_evidence=None, source_fidelity=None, replay_regression=None):
    """Persist the last validation attempt BEFORE the caller may replace data.

    A passing report means validated, not installed. The caller still performs
    its normal version check and atomic write. Personal/account files are not
    read. Keep one last-attempt report per profile, not unbounded run history.
    """
    report = audit_catalog_quality(candidate, previous, component=component)
    report["candidateContentSha256"] = _digest(candidate)
    report["previousContentSha256"] = _digest(previous) if previous is not None else None
    # Receipts are independently read from this update's verified official
    # input files by the updater, never taken from candidate metadata. An
    # official dangling reference is retained; this cannot pardon a malformed
    # endpoint, a broken reverse index or loss of previously available data.
    receipts = {(row.get("kind"), row.get("path"), row.get("targetNamespace"), row.get("targetId")): row
                for row in (reference_evidence or []) if isinstance(row, dict) and row.get("evidence")}
    for row in report["findings"]:
        key = tuple(row.get(name) for name in ("kind", "path", "targetNamespace", "targetId"))
        if row["kind"] == "missing-reference" and row["severity"] == "review" and key in receipts:
            row["severity"] = "info"
            row["officialInputEvidence"] = deepcopy(receipts[key]["evidence"])
            row["resolution"] = "reference-present-and-definition-absent-in-official-input"
    if source_coverage is not None:
        report["sourceCoverage"] = source_coverage
        for row in source_coverage.get("releaseFindings", []):
            report["findings"].append(dict(row))
    if source_fidelity is not None:
        _merge_fidelity(report, candidate, previous, source_fidelity)
    if replay_regression is not None:
        report['replayRegression'] = deepcopy(replay_regression)
        if not isinstance(replay_regression, dict) or replay_regression.get('passed') is not True \
                or replay_regression.get('candidateContentSha256') != _digest(candidate):
            report['findings'].append(_issue('native-replay-regression-failed', '/',
                '真实战报回归未通过或报告与本次候选资料不匹配', 'error', baselineStatus='new'))
    counts = Counter(row["severity"] for row in report["findings"])
    report["summary"].update(errors=counts["error"], reviewRequired=counts["review"], information=counts["info"],
        existingFindings=sum(row.get("baselineStatus") == "existing" for row in report["findings"]),
        resolvedFindings=sum(row.get("baselineStatus") == "resolved" for row in report["findings"]))
    report["passed"] = not (counts["error"] or counts["review"])
    directory = Path(report_directory)
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / (component + "-latest-attempt.json")
    report["decision"] = "validated-not-yet-applied" if report["passed"] else "rejected-kept-existing"
    data = json.dumps(report, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
    handle, temporary = tempfile.mkstemp(prefix=".quality-", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    metadata = {"schemaVersion": SCHEMA_VERSION, "passed": report["passed"], "summary": report["summary"],
        "reportFile": str(destination), "reportSha256": hashlib.sha256(data).hexdigest(),
        "candidateContentSha256": report["candidateContentSha256"], "decision": report["decision"]}
    if not report["passed"]:
        raise CatalogQualityError(metadata)
    return metadata
