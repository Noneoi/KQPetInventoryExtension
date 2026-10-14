"""Independently check core replay output against staged official input bytes.

This is a second *projection*, not a second call to the catalog generator. The
only reused parser is the bounded AS data-literal lexer. XML selection, key
joins, text routing, playback inheritance and field comparisons are defined
here from the inspected Flash consumers. No AS/JS code or network request runs.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

_MODULES = Path(__file__).resolve().parent / "battle_replay"
if str(_MODULES) not in sys.path:
    sys.path.insert(0, str(_MODULES))
from extract_skill_tables import LiteralObject, LiteralParser
from extract_replay_supplements import declaration


SCHEMA_VERSION = 1
MAX_INPUT_BYTES = 64 * 1024 * 1024
_ABSENT = object()
_TEXT_TABLES = ("SkillConfigSimpleAC", "SkillConfigSimple3000", "SkillConfigSimple6000", "SkillConfigSimple9999")
_DIRECT = "battleconfig/scripts/mmo/battleconfig/data/direct/"
# These are consumer-code approvals, never accepted from a candidate's own
# provenance. They belong to the already inspected library/interfaces package
# 2a7b34e2...ad6872 and the retained battleconfig consumer scripts. New consumer
# code must be reviewed; updating its data MAP does not update these approvals.
_CONSUMER_HASHES = {
    "interfaces/scripts/mmo/interfaces/battleconfig/define/SkillDefine.as": "4570131d8eb9263a5293ebcdfadc6ea48d9472c9b97911e98bc17707a33f8317",
    "interfaces/scripts/mmo/interfaces/battleconfig/define/SkillDescDefine.as": "5827145ef190c2435e9849267220a8d104b3e849eb50d397453f900a21cbdb5a",
    "interfaces/scripts/mmo/interfaces/battleconfig/define/BufferDefine.as": "85e1597a25f41eb9e96ac71dd143eed674b495ef514cf0869171488d4bb95b66",
    "interfaces/scripts/mmo/interfaces/battleconfig/define/BufferDescDefine.as": "eea806138df2592210302bec909498da2b2cf56bb35baf7305bbc8b8dd4f4ca0",
    "interfaces/scripts/mmo/interfaces/battleconfig/consts/SkillTypes.as": "0b95a8a97de487f18b6ea220d104f0b1b9809667faa1a8ca9bd2b314fd063a1e",
    "battleconfig/scripts/mmo/battleconfig/BattleConfig.as": "4a3a9f2b458226e6ddf1002b9fe31745ccfef943bb90a756b378af1d1a20dc8f",
    "battleconfig/scripts/mmo/battleconfig/data/SkillConfigXML.as": "b5beb86c5dc61ef3cb30726a54bec746ed24a619f122cd7cb71a06df1e2e0505",
    "battleconfig/scripts/mmo/battleconfig/data/SkillDescConfigXML.as": "5dc8d3ceb1bd4633143a38a8f6681c38bd000415f1ed6a9d21aafc4fd13fdab7",
    "battleconfig/scripts/mmo/battleconfig/data/BufferConfigXML.as": "70ae46702a2a9db4635d4cb53136b8c34b32feb070eb56e7d5c0b2627ebd7e35",
    "battleconfig/scripts/mmo/battleconfig/data/BufferDescConfigXML.as": "4ea2d9801c49b084cbb103565feea8dd0dbf08c5b2515e25d8fc8c149804be2f",
    "battleconfig/scripts/mmo/battleconfig/data/RaceTypeConfigXML.as": "3b5f734f05f3f29671ec6cd4de39475d9c94b1f488c5a09fc7d6be91655ca403",
    "battleconfig/scripts/mmo/battleconfig/data/SkillExecuteXML.as": "1939a848abf7d60908db71d4cf4e945abdd45e65d2a5162013df29dc1d8dec64",
    "battleconfig/scripts/mmo/battleconfig/data/BufferExecuteXML.as": "87d3bf4bdc36fa1fed0e98e532a3227949146ed1cc1a92074814f6376f91674d",
    "battleconfig/scripts/mmo/battleconfig/data/ComboBuffsConfigXML.as": "2aaddbaec15049a561f3756a80a770b402699e0bea9ff76a1f3a56712936da16",
    _DIRECT + "SkillConfigAdapter.as": "354d036cf4435204364d139100f3bcc80713a02f9b37ea97f6d1d02510e07e5c",
}
_PLAYBACK_ATTRIBUTES = ("action", "pos", "jump", "effect", "shock", "black", "hideother", "last",
                        "central", "playOrderType", "ef", "spine", "swf")
_INHERITED_DESC = ("attackType", "skillBaseType", "skillType", "bossSkill")
_HERO_CONSUMERS = {
    "hero_definition": "51da67ab25d0293aaa2fdf610ba539a8927a31cf2228ea4c5cae11972b7554c3",
    "skin_service": "90808cb703375027bd0c59a362c9d28a21a857887b9516ad3360830ebab4b3ba",
    "skin_definition": "4ab37fd9843abe014462e0e463b4764fc2b3527d2dc894bdba5ac84ba8b14d63",
    "skin_summary": "b935afa746369c43172e81698afd4fc335e3e99258bdc2398ef81969ada6f1bf",
    "form_config": "6998d6cf306f520c9d038ef75d185e17c35405024dd6a432092041f7a47e8761",
    "form_controller": "67b43975e26889445c1712be8e9c3552744adb6490d174a958f2a71fdf005e45",
    "race_util": "6c8a940592c46e48093edccf2701ef9b8e9a8cae30b2d3c9fa9328d72617138c",
}
_HERO_MEMBERSHIP_LABEL = "官方皮肤及形态兼容种族集合"


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _content_sha(value):
    return _sha(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
                           allow_nan=False).encode("utf-8"))


def _pointer(*parts):
    return "/" + "/".join(str(part).replace("~", "~0").replace("/", "~1") for part in parts)


def _same(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(_same(value, right[key]) for key, value in left.items())
    if isinstance(left, list):
        return len(left) == len(right) and all(_same(a, b) for a, b in zip(left, right))
    return left == right


def _integer(value):
    # Only the decimal integer spellings actually supported by the inspected
    # data projection are accepted. New AS numeric forms require adaptation.
    if value is None or value == "":
        return 0
    if not isinstance(value, str) or not re.fullmatch(r"[+-]?\d+", value.strip()):
        raise ValueError("Unreviewed numeric form in official playback attributes")
    result = int(value)
    if not -(2**31) <= result < 2**31:
        raise ValueError("Official playback integer is outside the reviewed int range")
    return result


def _plain_literal(value):
    if isinstance(value, LiteralObject):
        return {key: _plain_literal(child) for key, child, _ in value.pairs}
    if isinstance(value, list):
        return [_plain_literal(child) for child in value]
    return value


def _read_map(text):
    matches = list(re.finditer(r"\bpublic\s+static\s+const\s+MAP\s*:\s*Object\s*=", text))
    if len(matches) != 1:
        raise ValueError("Missing or ambiguous official skill MAP")
    parser = LiteralParser(text, matches[0].end())
    literal = parser.value(); parser.space()
    if not isinstance(literal, LiteralObject) or text[parser.pos:parser.pos + 1] != ";":
        raise ValueError("Official skill MAP is not a complete data literal")
    result = {}
    for key, value, _ in literal.pairs:
        if not isinstance(value, LiteralObject):
            raise ValueError("Official skill MAP record is not an object")
        fields = _plain_literal(value)
        if any(field in fields and not isinstance(fields[field], str) for field in ("name", "desc")):
            raise ValueError("Official skill text is not a string")
        # Object literal assignment replaces the complete previous object;
        # absent fields must not be inherited from a shadowed duplicate.
        result[key] = fields
    return result


def _description_table(identifier, maps):
    # SkillConfigAdapter.getSkillConfigObj: the AC map has priority. The
    # shortened number chooses a bucket only; the lookup uses the ORIGINAL ID.
    if identifier in maps["SkillConfigSimpleAC"]:
        return "SkillConfigSimpleAC"
    number = int(identifier)
    if number > 100000000:
        number //= 1000
    elif number > 10000000:
        number //= 100
    elif number > 1000000:
        number //= 10
    bucket = number % 10000
    return "SkillConfigSimple3000" if bucket <= 3000 else "SkillConfigSimple6000" if bucket <= 6000 else "SkillConfigSimple9999"


def _origin(identifier):
    # SkillDefine.checkIsAutoChessSkillId/getOrigSkillId; no generic prefix alias.
    number = int(identifier)
    if len(identifier) == 7 and identifier[0] in "12" and identifier[1] in "123":
        return str((number // 1000000) * 100000 + number % 100000)
    return identifier


def _valid_playback(attributes, description):
    # SkillConfigXML.parseConfig catches each rejected SkillDefine row without
    # inserting it. A rejected later duplicate does not erase an earlier one.
    jump = _integer(attributes.get("jump")) if description.get("attackType", "") == "近身" else -1
    action, position, effect = (_integer(attributes.get(key)) for key in ("action", "pos", "effect"))
    spine = _integer(attributes.get("spine")) == 1
    return action != 0 and 1 <= position <= 10 and (effect != 0 or spine) and effect != jump


def _available(attributes):
    if attributes is None:
        return False
    return (_integer(attributes.get("action")) > 0 and _integer(attributes.get("pos")) > 0
            and (_integer(attributes.get("effect")) > 0 or _integer(attributes.get("spine")) == 1))


class _Audit:
    def __init__(self, candidate):
        self.candidate = candidate
        self.findings, self.inputs, self.coverage = [], [], []
        self.allowed_corrections = []
        self.compared_fields = 0
        try:
            self.candidate_sha = _content_sha(candidate)
        except (TypeError, ValueError, UnicodeError):
            self.candidate_sha = None
            self.issue("invalid-candidate-json", "/", "回放候选资料不是有效 JSON")

    def issue(self, kind, path, message, **extra):
        self.findings.append(dict(severity="error", kind=kind, path=path, message=message, **extra))

    def read(self, path, role):
        path = Path(path)
        row = {"role": role, "path": str(path.resolve()), "status": "missing"}
        self.inputs.append(row)
        if not path.is_file():
            raise ValueError("Required official input missing: " + role)
        if path.stat().st_size > MAX_INPUT_BYTES:
            row["status"] = "oversized"
            raise ValueError("Official input exceeds audit size limit: " + role)
        data = path.read_bytes()
        row.update(status="read", bytes=len(data), sha256=_sha(data))
        return data

    def xml(self, source, name):
        paths = sorted((source / "xml").glob("*." + name + "__*.xml"))
        if len(paths) != 1:
            self.inputs.append({"role": name, "path": str(source / "xml"),
                                "status": "missing-or-ambiguous", "matchingFiles": len(paths)})
            raise ValueError("Expected exactly one official XML: " + name)
        return ET.fromstring(self.read(paths[0], name))

    def json(self, path, role):
        value = json.loads(self.read(path, role).decode("utf-8-sig"))
        if not isinstance(value, dict):
            raise ValueError("Official input must be a JSON object: " + role)
        return value

    def check(self, expected, actual, parts):
        self.compared_fields += 1
        path = _pointer(*parts)
        if isinstance(expected, dict) and isinstance(actual, dict):
            for key in sorted(set(expected) | set(actual)):
                self.check(expected.get(key, _ABSENT), actual.get(key, _ABSENT), (*parts, key))
        elif isinstance(expected, list) and isinstance(actual, list) and len(expected) == len(actual):
            for index, (left, right) in enumerate(zip(expected, actual)):
                self.check(left, right, (*parts, index))
        elif not _same(expected, actual):
            detail = {"expectedMissing": expected is _ABSENT, "actualMissing": actual is _ABSENT}
            if expected is not _ABSENT:
                detail["expected"] = expected
            if actual is not _ABSENT:
                detail["actual"] = actual
            self.issue("official-value-mismatch", path, "解析结果与已核对的官方原字段不一致", **detail)

    def table(self, name, expected, actual=_ABSENT, *, scope):
        start, fields = len(self.findings), self.compared_fields
        if actual is _ABSENT:
            actual = self.candidate.get(name, _ABSENT)
        self.check(expected, actual, (name,))
        self.coverage.append({"path": _pointer(name), "rows": len(expected), "scope": scope,
            "status": "matched" if start == len(self.findings) else "mismatch",
            "comparisons": self.compared_fields - fields})

    def report(self):
        counts = Counter(row["severity"] for row in self.findings)
        return {"schemaVersion": SCHEMA_VERSION, "candidateContentSha256": self.candidate_sha,
            "passed": not (counts["error"] or counts["review"]),
            "summary": {"errors": counts["error"], "reviewRequired": counts["review"], "information": counts["info"],
                "tablesChecked": len(self.coverage), "rowsChecked": sum(row["rows"] for row in self.coverage),
                "comparisons": self.compared_fields, "inputFiles": len(self.inputs)},
            "findings": self.findings, "coverage": self.coverage, "inputs": self.inputs,
            "allowedCorrections": self.allowed_corrections if not (counts["error"] or counts["review"]) else [],
            "boundaries": [
                "仅证明已列核心输出与本次传入的官方暂存文件相符；网络版本、包哈希和原子替换由更新器另行验证。",
                "技能文字按 Flash Adapter 原编号路由，动作只按直接配置或已证实的自动棋继承；H5 技能说明只用于已有记录范围，不替代 Flash 文字。",
                "Buff 的 nm 回退是本项目明确标记来源的展示规则（Flash 说明、Flash 配置、H5 名称）；mess/beffect/bhs 只取 Flash 说明。",
                "不认证养成效果触发、Buff 施加者、服务端数值、皮肤原名、术语索引或其他派生关系；不把空说明填成猜测内容。",
                "skillLastEffects 只核对显式 XML last 的核心字段，不重建运行时惰性缓存、runtimeDefine 或 H5 对照报告。",
                "共享的 AS 字面量词法读取器只负责读取数据；本审核不调用目录生成器或技能/Buff 业务提取函数。",
                "英雄 RaceEffected 兼容集合仅按本次皮肤可见配置及已核验形态消费者重算；不是实际战斗作用对象。仅精确值哈希绑定的派生集合缩短可取得收据。",
            ]}


def _indexed(elements, field, *, numeric=False):
    result = {}
    for element in elements:
        if field not in element.attrib:
            raise ValueError("Official XML record lacks explicit identity: " + field)
        key = str(_integer(element.attrib[field])) if numeric else element.attrib[field]
        result[key] = dict(element.attrib)
    return result


def _core_skill(identifier, maps, descriptions, playback, *, explicit=None, last=False):
    table = _description_table(identifier, maps)
    text = maps[table].get(identifier, {})
    own = descriptions.get(identifier, {})
    source_id = identifier
    configuration = explicit if last else playback.get(identifier)
    resolution = "direct-last" if last else "direct"
    if configuration is None:
        source_id = _origin(identifier)
        configuration = playback.get(source_id) if source_id != identifier else None
        resolution = "copyDefine" if configuration is not None else "description-only"
    expected = dict(own)
    if configuration is not None:
        if resolution == "copyDefine":
            inherited = descriptions.get(source_id, {})
            expected.update({field: inherited.get(field, "") for field in _INHERITED_DESC})
            expected.update({field: configuration[field] for field in _PLAYBACK_ATTRIBUTES if field in configuration})
            expected["swf"] = str(_integer(configuration.get("swf", source_id)))
        else:
            expected.update(configuration)
        expected["configSourceId"] = source_id
    expected.update(id=identifier, name=text.get("name", ""), description=text.get("desc", ""),
                    configResolution=resolution, playbackAvailable=_available(configuration))
    # runtime_skills deliberately retains own XML metadata only when its
    # projection differs from inherited/default fields. Compare it exactly.
    if last or any(expected.get(key, _ABSENT) != value for key, value in own.items()):
        expected["descriptionAttributes"] = dict(own)
    return expected


def _hero_membership_audit(audit, arguments, previous):
    supplements = audit.candidate.get("replaySupplements", {})
    if not isinstance(supplements, dict):
        raise ValueError("Replay supplement table is not an object")
    if "hero" not in supplements:
        return
    group = supplements["hero"]
    if not isinstance(group, dict) or not isinstance(group.get("rows"), list):
        raise ValueError("Hero table lacks ordered rows")
    if not all(isinstance(arguments.get(key), (str, Path)) for key in ("auxiliary", "skin_source", "formation_source")):
        raise ValueError("Hero fidelity requires the explicit auxiliary, skin and formation input roots")
    auxiliary, skin = Path(arguments["auxiliary"]), Path(arguments["skin_source"])
    roots = [Path(arguments["source"]), auxiliary, Path(arguments["formation_source"])]
    def selected(relative):
        for root in roots:
            if (root / relative).is_file():
                return root / relative
        raise ValueError("Missing official hero input: " + relative)
    inputs = {
        "hero_definition": selected("interfaces/scripts/mmo/interfaces/formation/HeroSkillDefine.as"),
        "hero_config": selected("formation/scripts/mmo/formation/config/HeroSkillConfig.as"),
        "skin_config": skin / "scripts/mmo/changepetskinv3/other/Cpsv3SkinConfig.as",
        "skin_definition": skin / "scripts/mmo/changepetskinv3/other/Cpsv3Skin.as",
        "skin_summary": skin / "scripts/mmo/changepetskinv3/other/Cpsv3SkinSummary.as",
        "skin_service": auxiliary / "pet-identity-20261009/skin/scripts/mmo/changepetskinv3/ChangePetSkinService.as",
        "form_config": auxiliary / "completion-20261010/multipleform/scripts/mmo/multipleformpet/config/MultipleFormPetConfig.as",
        "form_controller": auxiliary / "completion-20261010/multipleform/scripts/mmo/multipleformpet/control/MultipleFormPetController.as",
        "race_util": auxiliary / "origin-link-audit-20261009/fortune/common/scripts/mmo/common/utils/petrace/PetRaceUtil.as",
    }
    start_inputs, start_findings, start_comparisons = len(audit.inputs), len(audit.findings), audit.compared_fields
    texts = {}
    for role, path in inputs.items():
        data = audit.read(path, "hero-membership:" + role)
        if role in _HERO_CONSUMERS and _sha(data) != _HERO_CONSUMERS[role]:
            raise ValueError("Unreviewed official hero/skin/form consumer: " + role)
        texts[role] = data.decode("utf-8-sig")
    hero_definitions, _ = declaration(texts["hero_config"], "defines", ["HeroSkillDefine"])
    forms, _ = declaration(texts["form_config"], "FORM_CONF", ["MultipleFormPetFormDefine"])
    markers = list(re.finditer(r"\bSKIN_DEFINES_SRC\s*:\s*Array\s*=\s*", texts["skin_config"]))
    if len(markers) != 1:
        raise ValueError("Missing or ambiguous official skin data")
    text = texts["skin_config"][markers[0].end():]
    skins, end = json.JSONDecoder().raw_decode(text)
    if not isinstance(skins, list) or not text[end:].lstrip().startswith(";"):
        raise ValueError("Official skin definitions must be a literal array")
    for row in skins:
        if (not isinstance(row, dict) or type(row.get("show")) is not bool
                or not isinstance(row.get("availablePets"), list)
                or any(not isinstance(pet, dict) or type(pet.get("raceId")) is not int or pet["raceId"] <= 0
                       or type(pet.get("raceIdNeed")) is not int or pet["raceIdNeed"] < 0 for pet in row["availablePets"])):
            raise ValueError("Unreviewed official skin membership fields")
    if not isinstance(forms, list):
        raise ValueError("Official form definitions must be a constructor array")
    form_groups = []
    for row in forms:
        if (not isinstance(row, dict) or row.get("constructor") != "MultipleFormPetFormDefine"
                or len(row.get("arguments", [])) != 1 or not isinstance(row["arguments"][0], list)
                or any(type(value) is not int or value <= 0 for value in row["arguments"][0])):
            raise ValueError("Unreviewed official form membership fields")
        form_groups.append(row["arguments"][0])
    # Independently follow the public getter. In the approved Cpsv3Skin code,
    # x-changeable dependents are visible and filtered by the same raceIdNeed;
    # the parent is first and skipped. They add no new membership beyond this
    # complete visible-row scan (their duplicates/order are not reproduced).
    skin_for_race = {}
    for row in skins:
        for pet in row["availablePets"]:
            skin_for_race[pet["raceId"]] = row
    def first(row, field, identifier):
        return next((pet for pet in row["availablePets"] if pet[field] == identifier), None)
    def visible_switch(identifier):
        owner = skin_for_race.get(identifier)
        if owner is None:
            if not any(row.get("fuse") is None and first(row, "raceIdNeed", identifier) for row in skins):
                return []
            original = identifier
        else:
            if owner.get("fuse") is not None:
                return []
            original = first(owner, "raceId", identifier)["raceIdNeed"]
        if any(not row["show"] and first(row, "raceId", identifier) for row in skins):
            return []
        visible = [pet["raceId"] for row in skins if row["show"]
                   and (pet := first(row, "raceIdNeed", original)) is not None]
        return [original, *visible] if visible else []
    def membership(identifier):
        reset = visible_switch(identifier)
        original = reset[0] if reset else identifier
        forms = next((row for row in form_groups if original in row), [])
        expanded = set([original, *forms])
        result = set(expanded)
        for form in expanded:
            result.update(visible_switch(form))
        return [str(value) for value in sorted(result)]
    if not isinstance(hero_definitions, list) or len(hero_definitions) != len(group["rows"]):
        raise ValueError("Hero row count/order does not match the current official constructor array")
    before_group = previous.get("replaySupplements", {}) if isinstance(previous, dict) else {}
    before_group = before_group.get("hero", {}) if isinstance(before_group, dict) else {}
    before_rows = before_group.get("rows", []) if isinstance(before_group, dict) else []
    verified_rows = 0
    for index, (raw, row) in enumerate(zip(hero_definitions, group["rows"])):
        if (not isinstance(raw, dict) or raw.get("constructor") != "HeroSkillDefine"
                or not 7 <= len(raw.get("arguments", [])) <= 11 or not isinstance(row, dict)):
            raise ValueError("Hero row has an unreviewed constructor/schema")
        args = raw["arguments"]
        if type(args[0]) is not int or args[0] <= 0 or not isinstance(args[4], str) or not isinstance(args[5], str):
            raise ValueError("Hero source identity/condition is not a reviewed literal")
        parts = ("replaySupplements", "hero", "rows", index)
        raw_fields = {"rowId": index, "skillId": str(args[0]), "cls": args[4], "clsParams": args[5], "rawParameters": args}
        for field, expected in raw_fields.items():
            audit.check(expected, row.get(field, _ABSENT), (*parts, field))
        if args[4] != "RaceEffected":
            audit.check(_ABSENT, row.get("effectiveRaceIds", _ABSENT), (*parts, "effectiveRaceIds"))
            continue
        seed = _integer(args[5].split(",")[0])
        if seed <= 0:
            raise ValueError("Hero race seed is not a positive explicit identity")
        expected = membership(seed)
        audit.check(str(seed), row.get("effectRaceSeed", _ABSENT), (*parts, "effectRaceSeed"))
        audit.check("resolved", row.get("effectiveClsParamsStatus", _ABSENT), (*parts, "effectiveClsParamsStatus"))
        audit.check(expected, row.get("effectiveRaceIds", _ABSENT), (*parts, "effectiveRaceIds"))
        readings = row.get("readings")
        if not isinstance(readings, list) or len(readings) <= 5 or not isinstance(readings[5], dict):
            raise ValueError("Hero membership display reading is missing")
        audit.check(_HERO_MEMBERSHIP_LABEL, readings[5].get("label", _ABSENT), (*parts, "readings", 5, "label"))
        audit.check(expected, readings[5].get("value", _ABSENT), (*parts, "readings", 5, "value"))
        verified_rows += 1
        old = before_rows[index] if isinstance(before_rows, list) and index < len(before_rows) else None
        if not isinstance(old, dict) or any(old.get(key) != raw_fields[key] for key in ("skillId", "cls", "clsParams")):
            continue
        old_values = old.get("effectiveRaceIds")
        old_readings = old.get("readings")
        if (not isinstance(old_values, list) or any(not isinstance(value, str) for value in old_values)
                or len(expected) >= len(old_values) or not isinstance(old_readings, list) or len(old_readings) <= 5
                or not isinstance(old_readings[5], dict) or old_readings[5].get("label") != _HERO_MEMBERSHIP_LABEL
                or not _same(old_readings[5].get("value"), old_values)):
            continue
        proof = {"resolution": "derived-display-race-set-verified-from-current-official-input",
                 "skillId": str(args[0]), "seedRaceId": str(seed), "rawHeroFields": raw_fields,
                 "expectedMembership": expected, "inputs": [dict(value) for value in audit.inputs[start_inputs:]]}
        for suffix in (("effectiveRaceIds",), ("readings", 5, "value")):
            audit.allowed_corrections.append({"path": _pointer(*parts, *suffix),
                "rule": "hero-raceeffected-visible-skin-membership-v1", "previousPresent": True, "candidatePresent": True,
                "previousValueSha256": _content_sha(old_values), "candidateValueSha256": _content_sha(expected), "source": proof})
    audit.coverage.append({"path": "/replaySupplements/hero/rows", "rows": len(hero_definitions),
        "raceEffectedRows": verified_rows, "scope": "raw hero identity/condition rows and current visible-skin/first-form-group membership only",
        "status": "matched" if len(audit.findings) == start_findings else "mismatch", "comparisons": audit.compared_fields - start_comparisons})


def audit_replay_fidelity(candidate, *, build_arguments, previous=None):
    """Read-only core fidelity audit; missing/changed consumers or inputs fail.

    ``build_arguments['source']`` is the same staged root passed to build_catalog.
    A hero table additionally requires auxiliary/skin_source/formation_source;
    generated race_expansions is never an oracle. ``previous`` binds narrowly
    scoped correction receipts, never the current expected value. The caller
    must finalize candidate metadata before invoking this function.
    """
    audit = _Audit(candidate)
    if not isinstance(candidate, dict):
        audit.issue("invalid-candidate", "/", "回放候选资料必须是对象")
        audit.inputs.append({"role": "source", "status": "not-read-invalid-candidate"})
        return audit.report()
    try:
        if not isinstance(build_arguments, dict) or not isinstance(build_arguments.get("source"), (str, Path)):
            audit.inputs.append({"role": "source", "status": "missing"})
            raise ValueError("Official source root was not supplied")
        source = Path(build_arguments["source"])
        for relative, expected_hash in _CONSUMER_HASHES.items():
            data = audit.read(source / relative, "consumer:" + relative)
            if _sha(data) != expected_hash:
                audit.issue("unreviewed-consumer", "/inputs/" + relative, "官方消费脚本变化，需先核对解析规则",
                            expectedSha256=expected_hash, actualSha256=_sha(data))
        if audit.findings:
            return audit.report()
        maps = {name: _read_map(audit.read(source / _DIRECT / (name + ".as"), name).decode("utf-8-sig"))
                for name in _TEXT_TABLES}
        desc_xml = audit.xml(source, "SkillDescConfigXML")
        skill_xml = audit.xml(source, "SkillConfigXML")
        descriptions = _indexed(desc_xml.findall("./pet/s"), "id", numeric=True)
        normal, last_rows = {}, {}
        known = set(descriptions)
        rejected_rows = 0
        for node_name, output in (("s", normal), ("last", last_rows)):
            for element in skill_xml.findall("./" + node_name):
                if "id" not in element.attrib:
                    raise ValueError("Official skill XML lacks ID")
                identifier = str(_integer(element.attrib["id"]))
                known.add(identifier)
                if _valid_playback(element.attrib, descriptions.get(identifier, {})):
                    output[identifier] = dict(element.attrib)
                else:
                    rejected_rows += 1
        h5_descriptions = audit.json(source / "h5-tables/skill_desc_config.json", "h5-skill-id-union")
        known.update(h5_descriptions)
        for values in maps.values():
            known.update(values)
        if any(not re.fullmatch(r"0|[1-9]\d*", identifier) or int(identifier) > 2147483647 for identifier in known):
            raise ValueError("Official skill identity needs an unreviewed normalization")
        expected_skills = {key: _core_skill(key, maps, descriptions, normal) for key in known}
        def projected(rows, *, last=False):
            if not isinstance(rows, dict):
                return rows
            ignored = {"textSource", "configSource"}
            if last:
                ignored |= {"runtimeDefine", "h5", "flashText"}
            return {key: {field: value for field, value in row.items() if field not in ignored}
                    if isinstance(row, dict) else row for key, row in rows.items()}
        audit.table("skills", expected_skills, projected(candidate.get("skills", _ABSENT)),
                    scope="Flash routed text, full direct XML fields, explicit auto-chess playback inheritance, exact ID union")
        audit.coverage[-1]["rejectedOfficialPlaybackRows"] = rejected_rows
        expected_lasts = {key: _core_skill(key, maps, descriptions, normal, explicit=row, last=True)
                          for key, row in last_rows.items()}
        audit.table("skillLastEffects", expected_lasts, projected(candidate.get("skillLastEffects", _ABSENT), last=True),
                    scope="explicit valid XML last rows; core fields only")

        configs = _indexed(audit.xml(source, "BufferConfigXML").findall("./buff/s"), "id")
        descriptors = _indexed(audit.xml(source, "BufferDescConfigXML").findall("./buff/s"), "id")
        h5 = audit.json(source / "h5-tables/buffer_config.json", "h5-buffer-name-fallback").get("buff")
        if not isinstance(h5, dict) or any(not isinstance(value, str) for value in h5.values()):
            raise ValueError("Official H5 Buff table is not a packed string map")
        expected_buffs = {}
        for key in set(configs) | set(descriptors) | set(h5):
            config, description = configs.get(key, {}), descriptors.get(key, {})
            name = description.get("nm", "")
            name_source = "flash-desc"
            if not name:
                name = config.get("nm", ""); name_source = "flash-config"
            if not name:
                # Inspected H5 aqlib module 471: t||(t="|"); r=t.split("|");
                # _sName=r[0]. This named fallback is our declared projection,
                # not a claim that Flash reads H5 rendering/control fields.
                name = (h5.get(key, "") or "|").split("|", 1)[0]; name_source = "h5-buffer"
            if not name:
                name_source = "none"
            row = dict(config)
            row.update(id=key, nm=name, nameSource=name_source, nameStatus="named" if name else "unnamed",
                       configName=config.get("nm", ""), configSource="flash-config" if key in configs else "none",
                       descriptionSource="flash-desc" if key in descriptors else "none")
            row.update({field: description.get(field, "") for field in ("mess", "beffect", "bhs")})
            expected_buffs[key] = row
        audit.table("buffs", expected_buffs, scope="exact Flash config + descriptor ID join; explicitly labelled name fallbacks only")

        for name, xml_name, selector, identity in (
            ("execution", "SkillExecuteXML", "./s", "code"),
            ("bufferExecution", "BufferExecuteXML", "./s", "bufferId"),
            ("attributes", "RaceTypeConfigXML", "./race_type/s", "id"),
        ):
            root = audit.xml(source, xml_name)
            audit.table(name, _indexed(root.findall(selector), identity), scope=f"all original {xml_name}{selector} attributes keyed by {identity}; last complete row wins")
        combos = [dict(element.attrib) for element in audit.xml(source, "ComboBuffsConfigXML").findall("./combo/s")]
        audit.table("combos", combos, scope="all explicit ComboBuffsConfigXML.combo.s rows, attributes and order")
        _hero_membership_audit(audit, build_arguments, previous)
    except (OSError, ValueError, TypeError, ET.ParseError, UnicodeError) as error:
        audit.issue("official-input-unavailable", "/inputs", "官方原资料缺失、结构变化或无法按已确认规则读取", detail=str(error))
    return audit.report()
