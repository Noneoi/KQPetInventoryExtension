"""Build the read-only task/activity directory from current public game modules.

Called only by the explicit public-data updater. ActionScript is treated as
text: bounded literal tables and constructor arguments are parsed, never run.
Only the latest normalized component is retained on disk.
"""
from __future__ import annotations

import contextlib
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

PARSER_VERSION = 1
FILENAME = "routine-overview.json"
RESOURCES = (
    ("tasks", "dailytask/dailytaskservice", "mmo.h5version.DiamondTaskConfig"),
    ("hud", "config/config", "mmo.config.CommonHudConfig"),
    ("redPoints", "library/gameconst", "mmo.gameconst.RedPointConfig"),
)
LIMIT = 2**31 - 1


def _emit(message: str) -> None:
    # Pure ASCII progress line: the consumer decodes the same text back, and the
    # bytes no longer depend on the console code page of a standalone run.
    print(json.dumps({"event": "progress", "message": message}, ensure_ascii=True), flush=True)


def _read(path: Path) -> dict:
    try:
        if path.stat().st_size > 16 * 1024 * 1024:
            return {}
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def _atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def _integer(value, minimum: int = 0) -> bool:
    return type(value) is int and minimum <= value <= LIMIT


def _number(text: str) -> int:
    compact = re.sub(r"\s+", "", text)
    if compact == "Number.MAX_VALUE":
        return LIMIT
    if not re.fullmatch(r"0|[1-9][0-9]{0,9}", compact) or int(compact) > LIMIT:
        raise ValueError("官方任务出现尚不支持的数值规则，已保留旧目录")
    return int(compact)


def _balanced(text: str, start: int) -> tuple[str, int]:
    if start >= len(text) or text[start] not in "([{":
        raise ValueError("官方目录缺少静态配置表，已保留旧目录")
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack, quote, escaped = [], "", False
    for offset in range(start, len(text)):
        character = text[offset]
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = ""
            continue
        if character in "\"'":
            quote = character
        elif character in pairs:
            stack.append(pairs[character])
            if len(stack) > 64:
                raise ValueError("官方目录嵌套结构无法识别，已保留旧目录")
        elif character in ")]}" and (not stack or character != stack.pop()):
            raise ValueError("官方目录括号结构无效，已保留旧目录")
        elif character in ")]}" and not stack:
            return text[start:offset + 1], offset + 1
    raise ValueError("官方目录表不完整，已保留旧目录")


def _declaration(text: str, name: str, opening: str) -> str:
    if len(text) > 16 * 1024 * 1024:
        raise ValueError("官方目录文本过大，已保留旧目录")
    match = re.search(r"\b" + re.escape(name) + r"\s*:[^=;]{1,80}=\s*", text)
    if not match or text[match.end():match.end() + 1] != opening:
        raise ValueError("官方目录 " + name + " 规则已变化，已保留旧目录")
    return _balanced(text, match.end())[0]


def _arguments(text: str) -> list[str]:
    result, stack, quote, escaped, begin = [], [], "", False, 0
    pairs = {"(": ")", "[": "]", "{": "}"}
    for position, character in enumerate(text):
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = ""
        elif character in "\"'":
            quote = character
        elif character in pairs:
            stack.append(pairs[character])
        elif character in ")]}" and (not stack or character != stack.pop()):
            raise ValueError("官方目录参数结构无效")
        elif character == "," and not stack:
            result.append(text[begin:position].strip())
            begin = position + 1
    if stack or quote:
        raise ValueError("官方目录参数不完整")
    result.append(text[begin:].strip())
    return result


def parse_tasks(text: str) -> dict:
    table = _declaration(text, "TASK_CONFIG", "[")
    tasks, seen = [], set()
    for constructor in _arguments(table[1:-1]):
        match = re.match(r"new\s+DiamondTaskDefine\s*", constructor)
        if not match:
            raise ValueError("官方任务出现尚不支持的定义，已保留旧目录")
        arguments, end = _balanced(constructor, match.end())
        if constructor[end:].strip():
            raise ValueError("官方任务出现尚不支持的动态定义，已保留旧目录")
        fields = _arguments(arguments[1:-1])
        if len(fields) < 7:
            raise ValueError("官方任务参数不完整，已保留旧目录")
        identifier, name = _number(fields[0]), json.loads(fields[1])
        if not identifier or identifier in seen or not isinstance(name, str) or not name.strip():
            raise ValueError("官方任务标识、名称无效或重复，已保留旧目录")
        seen.add(identifier)
        row = {"id": identifier, "name": name}
        row.update(zip(("dayFinish", "dayActive", "weekFinish", "weekDailyMax", "weekActive"), map(_number, fields[2:7])))
        tasks.append(row)
    if not tasks or len(tasks) > 10000:
        raise ValueError("官方任务表未完整解析，已保留旧目录")
    result = {"tasks": tasks}
    for constant, field in (("DAY_PRIZE_PROGRESS1", "dayPrizeThresholds"), ("WEEK_PRIZE_PROGRESS", "weekPrizeThresholds")):
        raw = _declaration(text, constant, "[")
        numbers = [_number(value) for value in _arguments(raw[1:-1])]
        if not numbers or any(value <= 0 for value in numbers) or numbers != sorted(set(numbers)):
            raise ValueError("官方活跃度奖励档位无效，已保留旧目录")
        result[field] = numbers
    return result


def _date(text) -> bool:
    if text == "":
        return True
    if not isinstance(text, str) or not re.fullmatch(r"[0-9]{8}", text):
        return False
    try:
        datetime.strptime(text, "%Y%m%d")
        return True
    except ValueError:
        return False


def parse_hud(text: str) -> tuple[list[dict], list[dict]]:
    root = json.loads(_declaration(text, "DATA", "{"))
    if not isinstance(root, dict) or not isinstance(root.get("hud"), (dict, list)):
        raise ValueError("官方 HUD 活动配置无法识别，已保留旧目录")
    entries, unsupported, pending, walked = {}, [], [(root["hud"], 0)], 0
    while pending:
        value, depth = pending.pop()
        walked += 1
        if depth > 64 or walked > 100000:
            raise ValueError("官方 HUD 活动目录结构无法识别，已保留旧目录")
        if isinstance(value, list):
            pending.extend((child, depth + 1) for child in reversed(value))
            continue
        if not isinstance(value, dict):
            continue
        key, name, service = value.get("key"), value.get("name"), value.get("tryGetService", "")
        candidate = isinstance(key, str) and key.strip() and isinstance(name, str) and name.strip() and (
            "startTime" in value or "redPointId" in value or isinstance(service, str) and "NewActivityService" in service)
        if candidate:
            date, red_id = value.get("startTime", ""), value.get("redPointId", 0)
            if not _date(date) or not _integer(red_id):
                unsupported.append({"kind": "activity", "key": key, "reason": "日期或红点不是可识别的静态值"})
            else:
                row = {"key": key, "name": name, "startTime": date, "redPointId": red_id}
                previous = entries.get(key)
                if previous and previous != row:
                    unsupported.append({"kind": "activity-variant", "key": key, "reason": "同名活动存在多份入口，采用最新日期的定义"})
                    row = max((previous, row), key=lambda entry: (entry["startTime"], bool(entry["redPointId"]), entry["name"]))
                entries[key] = row
        pending.extend((child, depth + 1) for child in reversed(list(value.values())))
    if not entries or len(entries) > 10000:
        raise ValueError("官方 HUD 没有可识别的活动，已保留旧目录")
    return sorted(entries.values(), key=lambda row: (-int(row["startTime"] or "0"), row["key"])), unsupported


def parse_red_points(text: str) -> dict[str, list[int]]:
    if len(text) > 16 * 1024 * 1024:
        raise ValueError("官方红点目录文本过大")
    graph, references = {}, 0
    for match in re.finditer(r"new\s+RedPointConfigNode\s*", text):
        encoded, _ = _balanced(text, match.end())
        fields = _arguments(encoded[1:-1])
        if not 1 <= len(fields) <= 2:
            raise ValueError("官方红点关系出现尚不支持的规则，已保留旧目录")
        identifier = _number(fields[0])
        if identifier <= 0 or str(identifier) in graph:
            raise ValueError("官方红点标识无效或重复，已保留旧目录")
        children = []
        if len(fields) == 2:
            children = json.loads(fields[1])
            if not isinstance(children, list) or any(not _integer(child, 1) for child in children):
                raise ValueError("官方红点子节点出现尚不支持的规则，已保留旧目录")
        graph[str(identifier)] = sorted(set(children))
        references += 1 + len(children)
        if references > 50000:
            raise ValueError("官方红点目录结构无法识别，已保留旧目录")
    if not graph:
        raise ValueError("官方红点关系未完整解析，已保留旧目录")
    return graph


def activities_with_red_points(entries: list[dict], graph: dict[str, list[int]]) -> list[dict]:
    result = []
    for entry in entries:
        pending, seen = [entry["redPointId"]], set()
        while pending:
            identifier = pending.pop()
            if identifier <= 0 or identifier in seen:
                continue
            seen.add(identifier)
            if len(seen) > 50000:
                raise ValueError("官方红点关系结构过大，已保留旧目录")
            pending.extend(graph.get(str(identifier), []))
        result.append({**entry, "redPointIds": sorted(seen)})
    return result


def catalog_valid(root: dict) -> bool:
    source = root.get("source")
    cache = root.get("parserCache")
    if root.get("schema") != 1 or not isinstance(source, dict) or source.get("parserVersion") != PARSER_VERSION or not isinstance(cache, dict):
        return False
    resources = source.get("resources")
    if not isinstance(resources, dict) or any(not isinstance(resources.get(label), dict) for label, _, _ in RESOURCES):
        return False
    tasks, activities = root.get("tasks"), root.get("activities")
    if not isinstance(tasks, list) or not tasks or len(tasks) > 10000 or not isinstance(activities, list) or not activities or len(activities) > 10000:
        return False
    seen = set()
    for row in tasks:
        if not isinstance(row, dict) or not _integer(row.get("id"), 1) or row["id"] in seen or not isinstance(row.get("name"), str) or not row["name"].strip():
            return False
        seen.add(row["id"])
        if any(not _integer(row.get(field)) for field in ("dayFinish", "dayActive", "weekFinish", "weekDailyMax", "weekActive")):
            return False
    seen.clear()
    for row in activities:
        if not isinstance(row, dict) or not isinstance(row.get("key"), str) or not row["key"] or row["key"] in seen or not isinstance(row.get("name"), str) or not row["name"].strip():
            return False
        seen.add(row["key"])
        if not _date(row.get("startTime")) or not _integer(row.get("redPointId")) or not isinstance(row.get("redPointIds"), list) or any(not _integer(value, 1) for value in row["redPointIds"]):
            return False
    for field in ("dayPrizeThresholds", "weekPrizeThresholds"):
        values = root.get(field)
        if not isinstance(values, list) or not values or any(not _integer(value, 1) for value in values) or values != sorted(set(values)):
            return False
    unsupported = root.get("unsupportedRules")
    if not isinstance(unsupported, list) or any(not isinstance(value, dict) for value in unsupported):
        return False
    graph = cache.get("redPointGraph")
    return isinstance(graph, dict) and bool(graph) and all(str(key).isdecimal() and int(key) > 0 and isinstance(values, list) and all(_integer(value, 1) for value in values) for key, values in graph.items())


def update_routines(updater, versions: dict) -> bool:
    """Return whether a new directory was committed; preserve it on any failure."""
    path = updater.catalog / FILENAME
    current = _read(path)
    if not catalog_valid(current):
        baseline = _read(updater.baseline / FILENAME)
        current = baseline if catalog_valid(baseline) else {}
    previous = current.get("source", {}).get("resources", {})
    changed = {}
    for label, resource, _ in RESOURCES:
        version = versions.get(resource, "")
        if not re.fullmatch(r"[0-9]{8,20}", version):
            raise ValueError("官方版本清单缺少日常/活动资源：" + resource)
        changed[label] = not current or previous.get(label, {}).get("version") != version
    if not any(changed.values()):
        if _read(path) != current:
            _atomic_json(path, current)
        _emit("日常/活动目录已是最新：%d 个任务，%d 个活动；未支持规则 %d 项" % (
            len(current["tasks"]), len(current["activities"]), len(current.get("unsupportedRules", []))))
        return False
    resources, scripts = dict(previous), {}
    for label, resource, selected in RESOURCES:
        if not changed[label]:
            continue
        _emit("正在更新官方" + {"tasks": "日常与周常任务", "hud": "活动入口目录", "redPoints": "活动红点关系"}[label])
        swf, source = updater.resource(resource, versions)
        script = updater.export(swf, updater.scratch / ("routines-" + label), selected)
        scripts[label] = script.read_text(encoding="utf-8-sig")
        resources[label] = {**source, "key": resource, "configSha256": hashlib.sha256(scripts[label].encode("utf-8")).hexdigest()}
    tasks = parse_tasks(scripts["tasks"]) if changed["tasks"] else {key: current[key] for key in ("tasks", "dayPrizeThresholds", "weekPrizeThresholds")}
    if changed["hud"]:
        entries, unsupported = parse_hud(scripts["hud"])
    else:
        entries = [{key: row[key] for key in ("key", "name", "startTime", "redPointId")} for row in current["activities"]]
        unsupported = current.get("unsupportedRules", [])
    graph = parse_red_points(scripts["redPoints"]) if changed["redPoints"] else current["parserCache"]["redPointGraph"]
    result = {"schema": 1, "generatedAt": datetime.now(timezone.utc).isoformat(), **tasks,
              "activities": activities_with_red_points(entries, graph), "unsupportedRules": unsupported,
              "parserCache": {"redPointGraph": graph},
              "source": {"kind": "aoqi-official-routine-directory", "parserVersion": PARSER_VERSION,
                         "startVersion": updater.start_version, "resources": resources}}
    result["source"]["counts"] = {"tasks": len(tasks["tasks"]), "activities": len(entries), "redPointNodes": len(graph), "unsupportedRules": len(unsupported)}
    if not catalog_valid(result):
        raise ValueError("日常/活动目录校验未通过，已保留旧目录")
    _atomic_json(path, result)
    _emit("已更新日常/活动目录：%d 个任务，%d 个活动；未支持规则 %d 项" % (len(tasks["tasks"]), len(entries), len(unsupported)))
    return True
