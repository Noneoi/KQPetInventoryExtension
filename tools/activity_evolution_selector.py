"""Static implementation of PetDataService.getPetEvoLinkAllRaceIds(ids, false).

This is used only for explicit public-data checks of activity @sb selectors.
No ActionScript is executed; names, eras and dictionary groupRaceId are never
used to invent a relationship. The current normalized tables overwrite one
local cache and resource versions include start.xml hot-patch overrides.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

from public_routine_updater import _arguments, _atomic_json, _balanced, _declaration, _read

PARSER_VERSION = 1
FILENAME = "activity-evolution-data.json"
RESOURCES = (
    ("evolution", "pet/petdataservice", (
        "mmo.pet.dataservice.petevopanel.PetEvoPanelManager",
        "mmo.pet.dataservice.petevopanel.config.PetEvoPanelConfig",
        "mmo.pet.dataservice.petevopanel.config.PetEvoPanelDefine")),
    ("forms", "multipleformpet/multipleformpetservice", (
        "mmo.multipleformpet.config.MultipleFormPetConfig",)),
    ("skins", "changepetskinv3/changepetskinservice", (
        "mmo.changepetskinv3.other.Cpsv3SkinConfig",
        "mmo.changepetskinv3.other.Cpsv3SkinSummary",
        "mmo.changepetskinv3.other.Cpsv3Skin")),
)


def _id(value) -> bool:
    return type(value) is int and 0 < value <= 2**31 - 1


def dependency_revision(versions: dict) -> str:
    values = {"parserVersion": PARSER_VERSION}
    for _, key, _ in RESOURCES:
        version = versions.get(key, "")
        if not re.fullmatch(r"[0-9]{8,20}", version):
            raise ValueError("进化适用范围缺少官方资源版本：" + key)
        values[key] = version
    return hashlib.sha256(json.dumps(values, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _method(text: str, name: str) -> str:
    match = re.search(r"\bfunction\s+" + re.escape(name) + r"\s*\([^)]*\)\s*:\s*[\w.<>]+\s*", text)
    if not match:
        raise ValueError("官方进化适用规则缺少方法：" + name)
    return _balanced(text, match.end())[0]


def verify_evolution_algorithm(manager: str, define: str) -> None:
    body = re.sub(r"\s+", "", _method(manager, "getPetEvoLinkAllRaceIds"))
    expected = ("{if(param1==null){param1=[];}param1=this.resetRealPetRaceIds(param1);"
                "param1=this.checkAddAllMultiFormIds(param1);param1=this.findAllNextEvoRaceIds(param1);"
                "if(param2){param1=this.checkAddAllMultiFormIds(param1);param1=this.checkAddAllSkinIds(param1);}returnparam1;}")
    if body != expected:
        raise ValueError("官方 @sb 展开顺序已变化，保留活动原适用范围")
    for method, required in (
        ("findPetEvoDef", ("PetEvoPanelConfig.ObjDataByExcel", "PetEvoPanelConfig.ObjData", "arrayPutPetRaceIds.indexOf(param1)")),
        ("resetRealPetRaceIds", ("getSwitchSkinPetRaceIds", "[0]")),
        ("checkAddAllMultiFormIds", ("getSwitchPetRaceIds", "indexOf")),
        ("findAllNextEvoRaceIds", ("findPetEvoDef", ".raceId", "indexOf")),
    ):
        method_body = re.sub(r"\s+", "", _method(manager, method))
        if any(token not in method_body for token in required):
            raise ValueError("官方 @sb " + method + " 规则已变化，保留活动原适用范围")
        if method == "findPetEvoDef" and method_body.index(required[0]) > method_body.index(required[1]):
            raise ValueError("官方进化表优先级已变化，保留活动原适用范围")
    if 'this._strPutPetRaceIds.split("#")' not in re.sub(r"\s+", "", _method(define, "tryInitDefine")):
        raise ValueError("官方进化前置字段已变化，保留活动原适用范围")


def parse_evolution_config(text: str) -> dict:
    result = {}
    for section, label in (("ObjDataByExcel", "excel"), ("ObjData", "legacy")):
        table = _declaration(text, section, "{")
        keyed = {}
        for entry in _arguments(table[1:-1]):
            if not entry and table == "{}":
                continue
            match = re.match(r'"([0-9]+)"\s*:\s*new\s+PetEvoPanelDefine\s*', entry)
            if not match:
                raise ValueError("官方进化表出现尚不支持的定义，保留活动原适用范围")
            encoded, end = _balanced(entry, match.end())
            if entry[end:].strip():
                raise ValueError("官方进化表出现动态定义，保留活动原适用范围")
            arguments = _arguments(encoded[1:-1])
            if len(arguments) < 3 or not re.fullmatch(r"[1-9][0-9]*", arguments[0]):
                raise ValueError("官方进化目标 ID 无效")
            target = int(arguments[0])
            if not _id(target) or int(match[1]) != target:
                raise ValueError("官方进化配置键与目标 ID 不一致")
            inputs = json.loads(arguments[3]) if len(arguments) > 3 else ""
            if not isinstance(inputs, str) or inputs and not re.fullmatch(r"[1-9][0-9]*(?:#[1-9][0-9]*)*", inputs):
                raise ValueError("官方进化前置不是静态精灵 ID 列表")
            values = [int(value) for value in inputs.split("#")] if inputs else []
            if any(not _id(value) for value in values):
                raise ValueError("官方进化前置 ID 越界")
            keyed[target] = list(dict.fromkeys(values))
        edges = {}
        for target, inputs in keyed.items():
            for source in inputs:
                edges.setdefault(str(source), []).append(target)
        result[label] = edges
    if not result["excel"]:
        raise ValueError("官方主进化表为空，保留活动原适用范围")
    return result


def parse_form_config(text: str) -> list[list[int]]:
    table = _declaration(text, "FORM_CONF", "[")
    groups = []
    for entry in _arguments(table[1:-1]):
        match = re.match(r"new\s+MultipleFormPetFormDefine\s*", entry)
        if not match:
            raise ValueError("官方多形态表出现尚不支持的定义")
        encoded, end = _balanced(entry, match.end())
        if entry[end:].strip():
            raise ValueError("官方多形态表出现动态定义")
        values = json.loads(encoded[1:-1])
        if not isinstance(values, list) or not values or any(not _id(value) for value in values):
            raise ValueError("官方多形态组不完整")
        groups.append(list(dict.fromkeys(values)))
    if not groups or len(groups) > 10000:
        raise ValueError("官方多形态表不完整")
    return groups


def parse_skin_config(text: str, summary: str, skin: str) -> list[dict]:
    summary_body = re.sub(r"\s+", "", _method(summary, "getRaceIds"))
    if not all(token in summary_body for token in ("findOriginalRaceId(param1)", ".isShow()", ".findRaceIdNeed(param1)", "returnnull;", ".findRaceId(_loc4_)", "return[_loc4_].concat(_loc5_);")):
        raise ValueError("官方皮肤原名映射规则已变化")
    skin_body = re.sub(r"\s+", "", _method(skin, "isFusionPoster"))
    if "returnthis._fuse!=null;" not in skin_body:
        raise ValueError("官方融合皮肤规则已变化")
    table = json.loads(_declaration(text, "SKIN_DEFINES_SRC", "["))
    if not isinstance(table, list) or not table or len(table) > 10000:
        raise ValueError("官方皮肤映射表不完整")
    result = []
    for entry in table:
        if not isinstance(entry, dict) or not isinstance(entry.get("availablePets"), list) or type(entry.get("show")) is not bool or "fuse" not in entry:
            raise ValueError("官方皮肤可用精灵规则不完整")
        available = []
        for value in entry["availablePets"]:
            if not isinstance(value, dict) or not _id(value.get("raceId")) or not (
                    _id(value.get("raceIdNeed")) or value.get("raceIdNeed") == 0 and entry["fuse"] is not None):
                raise ValueError("官方皮肤对应精灵 ID 无效")
            available.append([value["raceId"], value["raceIdNeed"]])
        result.append({"show": entry["show"], "fusion": entry["fuse"] is not None, "available": available})
    return result


def _cache_valid(value: dict) -> bool:
    if value.get("schema") != 1 or value.get("parserVersion") != PARSER_VERSION or not isinstance(value.get("resources"), dict):
        return False
    if any(not isinstance(value["resources"].get(label), dict) for label, _, _ in RESOURCES):
        return False
    edges, forms, skins = value.get("evolution"), value.get("forms"), value.get("skins")
    if not isinstance(edges, dict) or not isinstance(forms, list) or not forms or not isinstance(skins, list) or not skins:
        return False
    for group in ("excel", "legacy"):
        if not isinstance(edges.get(group), dict):
            return False
        for key, targets in edges[group].items():
            if not key.isdecimal() or not _id(int(key)) or not isinstance(targets, list) or not targets or any(not _id(target) for target in targets):
                return False
    for group in forms:
        if not isinstance(group, list) or not group or any(not _id(value) for value in group):
            return False
    for row in skins:
        if not isinstance(row, dict) or type(row.get("show")) is not bool or type(row.get("fusion")) is not bool or not isinstance(row.get("available"), list):
            return False
        if any(not isinstance(pair, list) or len(pair) != 2 or not _id(pair[0]) or not (
                _id(pair[1]) or type(pair[1]) is int and pair[1] == 0 and row["fusion"]) for pair in row["available"]):
            return False
    return True


def _load_tables(updater, versions: dict) -> dict:
    revision = dependency_revision(versions)
    path = updater.catalog / FILENAME
    current = _read(path)
    if not _cache_valid(current):
        current = {}
    previous, scripts = current.get("resources", {}), {}
    resources = dict(previous)
    changed = []
    for label, key, classes in RESOURCES:
        if current and previous[label].get("version") == versions[key]:
            continue
        changed.append(label)
        swf, source = updater.resource(key, versions)
        scripts[label] = {}
        for selected in classes:
            exported = updater.export(swf, updater.scratch / ("activity-evolution-" + label), selected)
            scripts[label][selected.rsplit(".", 1)[-1]] = exported.read_text(encoding="utf-8-sig")
        resources[label] = {**source, "key": key}
    if not changed:
        return current
    result = {**current, "schema": 1, "parserVersion": PARSER_VERSION, "dependencyRevision": revision, "resources": resources}
    if "evolution" in changed:
        source = scripts["evolution"]
        verify_evolution_algorithm(source["PetEvoPanelManager"], source["PetEvoPanelDefine"])
        result["evolution"] = parse_evolution_config(source["PetEvoPanelConfig"])
    if "forms" in changed:
        result["forms"] = parse_form_config(scripts["forms"]["MultipleFormPetConfig"])
    if "skins" in changed:
        source = scripts["skins"]
        result["skins"] = parse_skin_config(source["Cpsv3SkinConfig"], source["Cpsv3SkinSummary"], source["Cpsv3Skin"])
    if not _cache_valid(result):
        raise ValueError("官方进化适用目录不完整，保留旧目录")
    _atomic_json(path, result)
    return result


def _original(seed: int, skins: list[dict]) -> int:
    # valueOfRaceId uses a map populated in configuration order; a later skin
    # owns a duplicate appearance. findRaceIdNeed still selects its first pair.
    selected = None
    for row in skins:
        if any(race == seed for race, _ in row["available"]):
            selected = row
    if selected:
        if selected["fusion"]:
            return seed
        original = next(original for race, original in selected["available"] if race == seed)
    elif any(not row["fusion"] and any(original == seed for _, original in row["available"]) for row in skins):
        original = seed
    else:
        return seed
    # getRaceIds(seed,true,true) returns null for a hidden source appearance.
    # resetRealPetRaceIds then keeps seed; otherwise only element 0 is used.
    if any(not row["show"] and any(race == seed for race, _ in row["available"]) for row in skins):
        return seed
    if any(row["show"] and any(need == original for _, need in row["available"]) for row in skins):
        return original
    return seed


def expand_from_tables(tables: dict, seeds: list[int]) -> list[int]:
    if not isinstance(seeds, (list, tuple)) or len(seeds) > 10000 or any(not _id(seed) for seed in seeds):
        raise ValueError("@sb 种子必须是正整数精灵 ID")
    normalized = list(dict.fromkeys(_original(seed, tables["skins"]) for seed in seeds))
    initial = []
    for seed in normalized:
        if seed in initial:
            continue
        initial.append(seed)
        group = next((group for group in tables["forms"] if seed in group), [])
        initial.extend(value for value in group if value not in initial)
    result = []
    for seed in initial:
        if seed in result:
            continue
        result.append(seed)
        current = seed
        while True:
            targets = tables["evolution"]["excel"].get(str(current))
            if targets is None:
                targets = tables["evolution"]["legacy"].get(str(current), [])
            targets = list(dict.fromkeys(targets))
            if not targets:
                break
            if len(targets) != 1:
                # AVM2 Object for-each ordering is not an ordered graph rule.
                # Never invent all branches or claim one arbitrary branch is
                # the exact official result when several definitions match.
                raise ValueError("精灵 %d 的官方进化前置匹配多个目标，适用范围待支持" % current)
            target = targets[0]
            if target in result:
                break
            result.append(target)
            if len(result) > 10000:
                raise ValueError("官方进化链超过可识别范围")
            current = target
    # false deliberately prevents a second multi-form/skin expansion here.
    return result


def expand_selector(updater, versions: dict, seeds: list[int]) -> list[int]:
    if not isinstance(seeds, (list, tuple)) or any(not _id(seed) for seed in seeds):
        raise ValueError("@sb 种子必须是正整数精灵 ID")
    if not seeds:
        return []
    return expand_from_tables(_load_tables(updater, versions), seeds)
