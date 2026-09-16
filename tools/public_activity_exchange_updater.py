"""Discover designated-pet exchanges from the official live activity graph.

Only explicit public-data updates call this module. It reads static AS values,
SWF constant pools and the official activity registry; it never executes AS,
queries accounts, follows walkthroughs, or guesses pet IDs from display names.
"""
from __future__ import annotations

from datetime import datetime, timedelta
import copy
import hashlib
import json
import lzma
from pathlib import Path
import re
import struct
import subprocess
import xml.etree.ElementTree as ET
import zlib

from public_routine_updater import _atomic_json, _arguments, _balanced, _declaration, _read
from generate_shop_exchange_data import official_date

PARSER_VERSION = 3
FILENAME = "activity-exchange-data.json"
CONFIG_RESOURCE = "config/config"
EMERGENCY_RESOURCE = "configinemergency/configinemergency"
BLOCK_RESOURCE = "configselfblock/configselfblock"
MAIN_SHOP_RESOURCE = "newactivityext/newact20260313/storeexchangeframework/storeexchangeframework"
RESOURCE_PATTERN = re.compile(r"newactivityext/newact[0-9]{8}/[A-Za-z0-9_]+/[A-Za-z0-9_]+")
MAX_SWF = 64 * 1024 * 1024
MAX_MODULES = 512


def emit(message):
    # Pure ASCII progress line: the consumer decodes the same text back, and the
    # bytes no longer depend on the console code page of a standalone run.
    print(json.dumps({"event": "progress", "message": message}, ensure_ascii=True), flush=True)


def swf_body(data: bytes) -> bytes:
    if len(data) < 12:
        raise ValueError("活动资源 SWF 不完整")
    size = struct.unpack_from("<I", data, 4)[0] - 8
    if not 4 <= size <= MAX_SWF:
        raise ValueError("活动资源解压大小无效")
    signature = data[:3]
    if signature == b"FWS":
        body = data[8:]
    elif signature == b"CWS":
        decompressor = zlib.decompressobj()
        body = decompressor.decompress(data[8:], size + 1)
    elif signature == b"ZWS" and len(data) >= 17:
        props, dictionary = data[12], struct.unpack_from("<I", data, 13)[0]
        if props >= 9 * 5 * 5 or dictionary > MAX_SWF:
            raise ValueError("活动资源压缩参数不支持")
        decompressor = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[{
            "id": lzma.FILTER_LZMA1, "dict_size": dictionary, "lc": props % 9,
            "lp": props // 9 % 5, "pb": props // 45}])
        body = decompressor.decompress(data[17:], max_length=size + 1)
    else:
        raise ValueError("活动资源 SWF 格式不支持")
    if len(body) != size:
        raise ValueError("活动资源 SWF 大小不匹配")
    return body


def tags(body: bytes):
    offset = (5 + 4 * (body[0] >> 3) + 7) // 8 + 4
    while offset + 2 <= len(body):
        header = struct.unpack_from("<H", body, offset)[0]
        offset += 2
        kind, size = header >> 6, header & 63
        if size == 63:
            if offset + 4 > len(body):
                raise ValueError("活动 SWF 标签不完整")
            size = struct.unpack_from("<I", body, offset)[0]
            offset += 4
        if offset + size > len(body):
            raise ValueError("活动 SWF 标签超出文件")
        yield kind, body[offset:offset + size]
        offset += size
        if kind == 0:
            return


def registry_xml(swf: Path) -> ET.Element:
    matches = []
    for kind, data in tags(swf_body(swf.read_bytes())):
        if kind != 87 or len(data) < 7:
            continue
        try:
            root = ET.fromstring(data[6:])
        except ET.ParseError:
            continue
        schema = root.get("{http://www.w3.org/2001/XMLSchema-instance}noNamespaceSchemaLocation", "")
        if schema.endswith("newactivityconfig.xsd"):
            matches.append(root)
    if len(matches) != 1:
        raise ValueError("官方活动注册表缺失或不唯一")
    return matches[0]


def strings_in_swf(body: bytes) -> set[str]:
    result = set()
    for kind, payload in tags(body):
        if kind not in (72, 82):
            continue
        offset = 0
        if kind == 82:
            offset = payload.find(b"\0", 4) + 1
            if offset <= 4:
                raise ValueError("活动 ABC 标签没有名称终止符")
        data, cursor = payload[offset:], 4
        def uint():
            nonlocal cursor
            value = 0
            for index in range(5):
                if cursor >= len(data):
                    raise ValueError("活动 ABC 常量池不完整")
                byte = data[cursor]
                cursor += 1
                value |= (byte & 127) << (7 * index)
                if byte < 128:
                    return value
            raise ValueError("活动 ABC 数值无效")
        for _ in range(2):
            count = uint()
            if count > 1000000:
                raise ValueError("活动 ABC 常量过多")
            for _ in range(max(0, count - 1)):
                uint()
        count = uint()
        cursor += max(0, count - 1) * 8
        count = uint()
        if count > 1000000:
            raise ValueError("活动 ABC 字符串过多")
        for _ in range(max(0, count - 1)):
            length = uint()
            if cursor + length > len(data):
                raise ValueError("活动 ABC 字符串不完整")
            result.add(data[cursor:cursor + length].decode("utf-8", errors="strict"))
            cursor += length
    return result


def build_discovery(root: ET.Element, hud_text: str) -> dict:
    # The registry keeps historical weeks. Walk only the current official HUD
    # and six most recent release weeks, plus their explicit activity links.
    entries, recent = {}, set()
    weeks = [node for node in root.findall("week") if re.fullmatch(r"\d{8}", node.get("version", ""))]
    latest = max((node.get("version") for node in weeks), default="")
    if not latest:
        raise ValueError("官方活动注册表没有发布日期")
    cutoff = (datetime.strptime(latest, "%Y%m%d") - timedelta(days=35)).strftime("%Y%m%d")
    for node in reversed(list(root.iter("a"))):
        alias = node.get("name", "")
        if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{2,100}", alias):
            entries[alias] = dict(node.attrib)
    for week in weeks:
        if week.get("version") >= cutoff:
            recent.update(node.get("name") for node in week.findall("a"))
    hud = json.loads(_declaration(hud_text, "DATA", "{"))
    pending, hud_entries = [hud], {}
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            service = value.get("tryGetService", "")
            pieces = service.split("#") if isinstance(service, str) else []
            if len(pieces) >= 3 and pieces[0] == "NewActivityService":
                alias = pieces[2]
                hud_entries[alias] = {"name": value.get("name", ""), "startTime": value.get("startTime", "")}
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    return {"latestRelease": latest, "recentSince": cutoff, "entries": entries,
            "hud": hud_entries, "roots": sorted(recent | set(hud_entries))}


def resolve_alias(alias: str, discovery: dict) -> tuple[str, dict] | None:
    seen = set()
    while alias not in seen:
        seen.add(alias)
        if alias in discovery.get("blocked", []):
            return None
        row = discovery["entries"].get(alias, {})
        if str(row.get("online", "true")).lower() == "false":
            return None
        path = row.get("file", "")
        if RESOURCE_PATTERN.fullmatch(path):
            metadata = dict(row)
            hud = discovery.get("hud", {}).get(alias, {})
            metadata["activityName"] = hud.get("name") or row.get("desc") or alias
            metadata["startTime"] = hud.get("startTime") or row.get("startTime", "")
            return path, metadata
        match = re.match(r"btnNewAct_([A-Za-z0-9]+)_", row.get("link", ""))
        if not match:
            return None
        alias = match[1]
    return None


def apply_discovery_overrides(updater, versions: dict, discovery: dict) -> dict:
    resources = {}
    roots = set(discovery["roots"])
    if EMERGENCY_RESOURCE in versions:
        swf, source = updater.resource(EMERGENCY_RESOURCE, versions)
        resources[EMERGENCY_RESOURCE] = source
        activity_document = False
        for kind, data in tags(swf_body(swf.read_bytes())):
            if kind != 87 or len(data) < 7:
                continue
            try:
                xml = ET.fromstring(data[6:])
            except ET.ParseError:
                continue
            if xml.find("activity") is not None:
                activity_document = True
                for node in xml.findall("./activity/a"):
                    alias = node.get("name", "")
                    if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{2,100}", alias):
                        discovery["entries"][alias] = {**discovery["entries"].get(alias, {}), **node.attrib}
                        roots.add(alias)
            for node in xml.iter("hud"):
                pieces = node.get("tryGetService", "").split("#")
                if len(pieces) >= 3 and pieces[0] == "NewActivityService":
                    discovery["hud"][pieces[2]] = {"name": node.get("name", ""), "startTime": node.get("startTime", "")}
                    roots.add(pieces[2])
        if not activity_document:
            raise ValueError("官方紧急活动目录格式已改变，保留原目录")
    discovery["blocked"] = []
    if BLOCK_RESOURCE in versions:
        swf, source = updater.resource(BLOCK_RESOURCE, versions)
        resources[BLOCK_RESOURCE] = source
        script = updater.export(swf, updater.scratch / "activity-blocks", "mmo.configselfblock.SelfBlockActivityConfig")
        table = LiteralReader(_declaration(script.read_text(encoding="utf-8-sig"), "DATA", "{")).parse()
        if not isinstance(table, dict) or any(not isinstance(value, dict) or type(value.get("isBlocked")) is not bool for value in table.values()):
            raise ValueError("官方活动暂停列表格式已改变，保留原目录")
        discovery["blocked"] = sorted(key for key, value in table.items() if value["isBlocked"])
    discovery["roots"] = sorted(roots)
    return resources


class LiteralReader:
    """Small literal parser: no eval, function calls, arithmetic or AS runtime."""
    pattern = re.compile(r'\s*("(?:\\.|[^"\\])*"|-?\d+(?:\.\d+)?|[A-Za-z_$][A-Za-z0-9_$.]*|[][{}:,])')
    def __init__(self, text):
        self.tokens, self.at = [], 0
        cursor = 0
        while cursor < len(text):
            match = self.pattern.match(text, cursor)
            if not match:
                if not text[cursor:].strip():
                    break
                raise ValueError("不是可识别的静态字面量")
            self.tokens.append(match[1])
            cursor = match.end()
    def take(self):
        if self.at >= len(self.tokens):
            raise ValueError("静态配置不完整")
        token = self.tokens[self.at]
        self.at += 1
        return token
    def value(self, depth=0):
        if depth > 64:
            raise ValueError("静态配置嵌套过深")
        token = self.take()
        if token == "[":
            result = []
            while self.tokens[self.at] != "]":
                result.append(self.value(depth + 1))
                if self.tokens[self.at] != "]" and self.take() != ",":
                    raise ValueError("静态数组分隔符无效")
            self.take()
            return result
        if token == "{":
            result = {}
            while self.tokens[self.at] != "}":
                key = self.take()
                key = json.loads(key) if key.startswith('"') else key
                if key in result or self.take() != ":":
                    raise ValueError("静态对象字段重复或无效")
                result[key] = self.value(depth + 1)
                if self.tokens[self.at] != "}" and self.take() != ",":
                    raise ValueError("静态对象分隔符无效")
            self.take()
            return result
        if token.startswith('"'):
            return json.loads(token)
        if re.fullmatch(r"-?\d+(?:\.\d+)?", token):
            return float(token) if "." in token else int(token)
        if token in ("true", "false", "null"):
            return {"true": True, "false": False, "null": None}[token]
        return {"$ref": token}
    def parse(self):
        result = self.value()
        if self.at != len(self.tokens):
            raise ValueError("静态配置包含动态表达式")
        return result


def parse_tables(scripts: dict[str, str]) -> tuple[dict, dict]:
    constants, tables = {}, {}
    for filename, text in scripts.items():
        cls = Path(filename).stem
        for match in re.finditer(r"\bstatic\s+(?:const|var)\s+(\w+)\s*:\s*[\w.<>]+\s*=\s*", text):
            key, begin = cls + "." + match[1], match.end()
            try:
                if text[begin:begin + 1] in ("[", "{"):
                    expression, end = _balanced(text, begin)
                else:
                    end = text.index(";", begin)
                    expression = text[begin:end]
                value = LiteralReader(expression).parse()
                constants[key] = value
                if isinstance(value, (dict, list)) and "$ref" not in value:
                    tables[key] = (filename, value)
            except (ValueError, IndexError, json.JSONDecodeError):
                continue
    def resolve(value, stack=()):
        if isinstance(value, dict):
            if set(value) == {"$ref"}:
                name = value["$ref"]
                if name in constants and name not in stack:
                    return resolve(constants[name], stack + (name,))
                return value
            return {key: resolve(child, stack) for key, child in value.items()}
        if isinstance(value, list):
            return [resolve(child, stack) for child in value]
        return value
    return {key: (filename, resolve(value)) for key, (filename, value) in tables.items()}, constants


def positive_ids(value) -> list[int]:
    if not isinstance(value, list) or not value or any(type(n) is not int or n <= 0 or n > 2**31 - 1 for n in value):
        raise ValueError("指定精灵列表不是明确的正整数 ID")
    return list(dict.fromkeys(value))


def supported_enhancement_expression(value) -> bool:
    # These optional parameters have been checked against the official
    # StrengthenComboItem / star-flow implementations. Other $ arguments stay
    # explicit pending instead of being silently discarded.
    token = r"(?:[1-9]\d*|33\$[1-9]\d*|39\$1)"
    return isinstance(value, str) and bool(re.fullmatch(token + r"(?:-" + token + r")*", value))


def enhancement(simple: str, expand=None) -> tuple[str, list[int]]:
    choices = [item for item in simple.lstrip("!").split("|") if item.startswith("CommonEnhancePrize,")]
    if len(choices) != 1:
        raise ValueError("复杂养成奖励组合尚待适配")
    parts = choices[0].split(",")
    if len(parts) < 5 or not supported_enhancement_expression(parts[3]):
        raise ValueError("养成代码不是受支持的静态列表")
    raw = parts[4].split(":")
    if len(raw) > 2 or len(raw) == 2 and raw[1] not in ("false", "0"):
        raise ValueError("排除式指定精灵范围尚待适配")
    seeds, exact = [], []
    for token in raw[0].split("#"):
        match = re.fullmatch(r"([1-9]\d*)(@sb)?", token)
        if not match:
            raise ValueError("指定精灵选择器尚待适配")
        (seeds if match[2] else exact).append(int(match[1]))
    if seeds:
        if expand is None:
            raise ValueError("指定进化链选择器尚待适配")
        exact += expand(seeds)
    return parts[3], positive_ids(exact)


def leaf_arrays(value, path=""):
    if isinstance(value, list):
        rows = [row for row in value if isinstance(row, dict) and ("simpleParams" in row or row.get("type") == "Strengthen")]
        if rows:
            yield path, rows
        for index, child in enumerate(value):
            if isinstance(child, (list, dict)) and child not in rows:
                yield from leaf_arrays(child, path + "/" + str(index))
    elif isinstance(value, dict):
        for name, child in value.items():
            if isinstance(child, (list, dict)):
                yield from leaf_arrays(child, path + "/" + name)


def as_methods(text: str):
    for match in re.finditer(r"\bfunction\s+(\w+)\s*\(", text):
        try:
            _, end = _balanced(text, match.end() - 1)
            start = text.find("{", end)
            if start < 0 or ";" in text[end:start]:
                continue
            body, _ = _balanced(text, start)
            yield match[1], body
        except ValueError:
            continue


def static_value(name: str, cls: str, constants: dict):
    name = name.strip()
    if re.fullmatch(r"\d+", name):
        return int(name)
    return constants.get(name if "." in name else cls + "." + name)


def read_requests(scripts: dict, constants: dict, shop_id: int) -> list[dict]:
    """Recognize actual read-method bodies, never infer access from suffix _0."""
    requests = []
    for filename, text in scripts.items():
        cls = Path(filename).stem
        for method, body in as_methods(text):
            if not (method == "info" or method.lower().startswith("getinfo")):
                continue
            command, params, generator = "", None, ""
            call = re.search(r"(?:sendXtMessage|request|requestForGetInfo)\s*\(", body)
            if not call:
                continue
            try:
                arguments, _ = _balanced(body, call.end() - 1)
                parts = _arguments(arguments[1:-1])
                reference = parts[0]
                if reference == "ClientSA.CMD_GET_INFO" and "extends ClientSA" in text:
                    constructor = re.search(r"super\s*\(\s*([\w.]+)\s*\)", text)
                    ai = static_value(constructor[1], cls, constants) if constructor else None
                    if type(ai) is not int or ai <= 0:
                        continue
                    command, params = "1019_0", {"ai": ai}
                else:
                    command = static_value(reference, cls, constants)
                    if not isinstance(command, str) or not re.fullmatch(r"\d+(?:_[A-Za-z0-9]+)+", command):
                        continue
                    if "requestForGetInfo" not in call[0]:
                        expression = parts[2] if len(parts) > 2 else "null"
                        if "ActUtil.getNewChangeSetId()" in expression:
                            generator = "ActUtil.getNewChangeSetId"
                        else:
                            params = LiteralReader(expression).parse()
                            if isinstance(params, dict) and set(params) == {"i"} and isinstance(params["i"], dict) and "$ref" in params["i"]:
                                params = {"i": shop_id}
                            if params is not None and (not isinstance(params, dict) or any(type(value) not in (int, str, bool) for value in params.values())):
                                continue
                extension = "TimelinessActExtension" if command.startswith("1008_") else "SimpleActExtension" if command.startswith("1019_") else "null"
                spec = {"method": method, "command": command, "params": params, "extension": extension,
                        "sourceMethod": cls + "." + method}
                if generator:
                    spec["parameterGenerator"] = {"ci": generator}
                if not any(previous["command"] == command and previous["params"] == params for previous in requests):
                    requests.append(spec)
            except (ValueError, IndexError):
                continue
    return requests


def trade_profile(scripts: dict, constants: dict, tables: dict, cls: str, rows: list, shop_id: int) -> dict:
    text = "\n".join(scripts.values())
    profile = {"text": text, "requests": [], "constants": constants, "rows": rows}
    requests = read_requests(scripts, constants, shop_id)
    exchange_info = [item for item in requests if item["command"] == "1008_20260313_es_2"]
    selected = []
    if exchange_info:
        selected = [exchange_info[0]]
        profile["quotaStyle"] = "bundle-bi" if "createLimitedByBundle" in text else "total-bi"
        eligibility = next((item for item in requests if item not in selected and item["method"].lower() == "getinfo"), None)
        if eligibility and any("lvFlag" in row for row in rows):
            selected.append(eligibility)
            profile["eligibility"] = True
            prefixes = set(re.findall(r'"([^"\s]+\$)"\s*\+\s*\w+', text))
            if len(prefixes) == 1:
                profile["levelFlagPrefix"] = prefixes.pop()
    else:
        primary = next((item for item in requests if item["method"].lower() == "getinfoshop"), None)
        primary = primary or next((item for item in requests if item["method"].lower() in ("getinfo", "info")), None)
        if primary:
            selected = [primary]
            if primary["command"] == "1019_0":
                direct_nested = re.search(r'\w+\s*=\s*\w+\["b"\s*\+\s*(\w+)\][\s\S]{0,300}?\w+\["b"\s*\+\s*\1\]', text)
                model_nested = re.search(r'\w+\["b"\s*\+\s*\w+\.index\]', text) and re.search(r'int\(\w+\["b"\s*\+\s*index\]\)', text)
                if direct_nested or model_nested:
                    profile["quotaStyle"] = "simple-bi"
                    profile["missingQuotaZero"] = bool(direct_nested and re.search(r"function\s+parseBought\([\s\S]{0,450}?return 0;", text))
            elif any(type(row.get("daibi")) is int for row in rows) and re.search(r'\w+\s*=\s*\w+\["li"\]', text):
                profile["quotaStyle"] = "li"
            elif any(type(row.get("price")) is int for row in rows) and re.search(r'\w+\s*=\s*\w+\["bt"\]', text):
                profile["quotaStyle"] = "bt"
            elif "getMyItemQuantityById" in text and 'param1["l"]' in text:
                profile["quotaStyle"] = "p-index"
            elif 'params["ep"]' in text and '"total_limited"' in text:
                profile["quotaStyle"] = "pet-ep"
    for index, request in enumerate(selected):
        spec = {key: value for key, value in request.items() if key != "method"}
        spec["key"] = "state" if index == 0 else "eligibility"
        required = {"li": ["li", "c"], "bt": ["bt"], "p-index": ["p"], "total-bi": ["cn"], "simple-bi": ["ai"]}
        spec["requiredFields"] = ["lv"] if index > 0 else required.get(profile.get("quotaStyle"), [])
        if not spec.get("parameterGenerator"):
            profile["requests"].append(spec)
        else:
            profile["pendingRequest"] = spec
    primary = selected[0] if selected else None
    if primary:
        profile["command"] = primary["command"]
    profile["hasState"] = any(item["key"] == "state" for item in profile["requests"])

    # Read material IDs from actual currency-binding calls in this module.
    for key, value in constants.items():
        if type(value) is not int or value <= 0:
            continue
        if key.endswith(".COIN_ID") and re.search(r"getMyItemQuantityById\s*\(\s*" + re.escape(key) + r"\s*\)", text):
            profile["costItemId"] = value
        elif key.endswith(".UNIVERSAL_ITEM_ID") and re.search(r"setCoinBox\([^;]*" + re.escape(key) + r"\s*,[^;]*\.cost\)", text):
            profile["costItemId"] = value
    labels = [value for key, value in constants.items() if key.endswith(".ObjDaibiNames") and isinstance(value, dict)]
    if len(labels) == 1:
        profile["currencyNames"] = labels[0]
    profile["currencyName"] = constants.get(cls + ".DaibiName", "")
    selected_indexes = {int(value) for value in re.findall(r'\bprice\s*=\s*int\(\s*\w+\["prices"\]\[(\d+)\]\s*\)', text)}
    if len(selected_indexes) == 1 and "ActUtil.getMyDiamond()" in text:
        profile["fixedPriceIndex"] = selected_indexes.pop()
    if "ActUtil.getMyDiamond()" in text:
        profile["diamondPrice"] = True
    price_method = next((body for value in scripts.values() for name, body in as_methods(value) if name == "getPriceIndex"), "")
    price_guard = re.search(r'if\((\w+) < ([\w.]+)\)\s*\{\s*return \1;\s*\}\s*return \2 - 1;', price_method)
    if price_guard and "getTotalBuyTimes()" in text:
        ceiling = static_value(price_guard[2], cls, constants)
        total_method = next((body for value in scripts.values() for name, body in as_methods(value) if name == "getTotalBuyTimes"), "")
        member = re.search(r'return this\.(\w+);', total_method)
        source = re.search(r'this\.' + re.escape(member[1]) + r'\s*=\s*\w+\["([A-Za-z0-9_]+)"\]', text) if member else None
        if type(ceiling) is int and ceiling > 0 and source:
            profile["dynamicPricePath"], profile["dynamicPriceCount"] = [source[1]], ceiling
    pet_quotas = {}
    if profile.get("quotaStyle") == "pet-ep":
        for _, value in tables.values():
            if not isinstance(value, list):
                continue
            for row in value:
                if not isinstance(row, dict) or type(row.get("id")) is not int or not isinstance(row.get("exchangeIds"), str):
                    continue
                for entry in row["exchangeIds"].split("#"):
                    if re.fullmatch(r"\d+:[1-9]\d*", entry):
                        identifier, maximum = map(int, entry.split(":"))
                        if identifier not in pet_quotas:
                            pet_quotas[identifier] = {"petConfigId": row["id"], "maximum": maximum}
    profile["petQuotas"] = pet_quotas
    return profile


def enrich_trade_item(item: dict, row: dict, profile: dict) -> None:
    text = profile["text"]
    identity, style = item["itemServerId"], profile.get("quotaStyle", "")
    limit = row.get("limit")
    period_keys, period_labels = ("dl", "wl", "ml", "pl", "tl"), ("日", "周", "月", "期", "总")
    maximum = -1
    if isinstance(limit, str) and re.fullmatch(r"[0-4]:\d+", limit):
        kind, maximum = map(int, limit.split(":"))
        if style == "bundle-bi" or style == "total-bi" and kind == 4:
            item.update(limitIndex=kind, limitKey=period_keys[kind], limitLabel=period_labels[kind], quotaCycleKnown=True)
    elif type(limit) is int and limit > 0:
        maximum = limit
    if type(row.get("maxNum")) is int and row["maxNum"] > 0:
        maximum = row["maxNum"]
        kind = row.get("limitType")
        if style == "p-index" and "周可兑换次数" in text and "日可兑换次数" in text and kind in (0, 1, 2):
            item.update(limitLabel={0: "", 1: "周", 2: "日"}[kind], quotaCycleKnown=kind in (1, 2))
    quota_id = identity
    if type(row.get("baseOnId")) is int and row["baseOnId"] >= 0:
        quota_id = row["baseOnId"]
        base = next((value for value in profile["rows"] if value.get("bi") == quota_id), {})
        if type(base.get("limit")) is int and base["limit"] > 0:
            maximum = base["limit"]
            item["quotaSharedWith"] = quota_id
    if identity in profile.get("petQuotas", {}):
        maximum = profile["petQuotas"][identity]["maximum"]
        item.update(limitKey="tl", limitLabel="总", limitIndex=4, quotaCycleKnown=True)
    if maximum >= 0:
        item.update(limitCount=maximum, quotaKnown=True)
    if profile["hasState"]:
        observation = {"requestKey": "state", "valueKind": "used"}
        if style in ("bundle-bi", "total-bi"):
            key = item["limitKey"] if style == "bundle-bi" else "tl"
            observation.update(path=["bi" + str(identity), key])
            if style == "bundle-bi" and "setData(0)" in text:
                observation["missingValue"] = 0
        elif style == "simple-bi":
            key = "b" + str(quota_id)
            observation.update(path=[key, key])
            if profile.get("missingQuotaZero"):
                observation["missingValue"] = 0
        elif style == "li":
            observation.update(path=["li", str(row.get("dataIndex", identity))])
        elif style == "bt":
            observation.update(path=["bt", str(row.get("index", identity))])
        elif style == "p-index":
            observation.update(path=["p", {"find": "i", "equals": row.get("index", identity)}, "l"], missingValue=0)
        if observation.get("path") and maximum >= 0:
            item["quotaObservation"] = observation
    elif style == "pet-ep" and identity in profile["petQuotas"]:
        item["quotaObservationPending"] = {"requestKey": "state", "valueKind": "used", "encoding": "id-counts", "itemId": identity,
            "path": ["pl", {"find": "id", "equals": profile["petQuotas"][identity]["petConfigId"]}, "ep"]}

    cost = row.get("cost")
    if type(cost) is int and cost > 0 and profile.get("costItemId"):
        item.update(cost=f"4:{profile['costItemId']}:{cost}", costKnown=True)
    if type(row.get("price")) is int and row["price"] > 0 and profile.get("diamondPrice"):
        item.update(cost=f"8:2:{row['price']}", costKnown=True)
    prices = row.get("prices")
    price_index = profile.get("fixedPriceIndex")
    if isinstance(prices, list) and type(price_index) is int and price_index < len(prices) and type(prices[price_index]) is int and prices[price_index] > 0:
        item.update(cost=f"8:2:{prices[price_index]}", costKnown=True, priceSourceIndex=price_index)
        if price_index != 0 and type(prices[0]) is int and prices[0] > 0:
            item["originalCost"] = f"8:2:{prices[0]}"
    prices = row.get("price")
    if isinstance(prices, list) and prices and all(type(value) is int and value > 0 for value in prices) and profile.get("diamondPrice"):
        options = []
        for index, value in enumerate(prices):
            option = {"cost": f"8:2:{value}", "label": f"第 {index + 1} 档"}
            if profile.get("dynamicPricePath") and profile.get("dynamicPriceCount") == len(prices) and profile["hasState"]:
                option["when"] = {"requestKey": "state", "path": profile["dynamicPricePath"], "op": "gte" if index == len(prices) - 1 else "eq", "value": index}
            options.append(option)
        item.update(priceOptions=options, costDescription="钻石价格档位：" + " / ".join(map(str, prices)), costKnown=False, cost="")
    daibi = row.get("daibi")
    if type(daibi) is int and daibi > 0 and profile.get("currencyName"):
        currency = {"name": profile["currencyName"], "count": daibi}
        if profile["hasState"] and style == "li" and re.search(r'\w+\s*=\s*\w+\["c"\]', text):
            currency.update(requestKey="state", path=["c"])
        item.update(activityCosts=[currency], costKnown=True, costDescription=f"{currency['name']} ×{daibi}")
    elif isinstance(daibi, str) and re.fullmatch(r"[1-9]\d*:[1-9]\d*", daibi):
        currency_id, count = map(int, daibi.split(":"))
        name = profile.get("currencyNames", {}).get(str(currency_id))
        if name:
            currency = {"name": name, "count": count, "itemId": currency_id}
            if profile["hasState"] and style == "total-bi" and '["cn"]' in text:
                currency.update(requestKey="state", path=["cn"], encoding="id-counts", missingValue=0)
            item.update(activityCosts=[currency], costKnown=True, costDescription=f"{name} ×{count}")
    if profile.get("eligibility") and isinstance(row.get("lvFlag"), str):
        prefix = profile.get("levelFlagPrefix", "")
        match = re.fullmatch(re.escape(prefix) + r"(\d+)", row["lvFlag"]) if prefix else None
        if match:
            item["observationWhen"] = {"requestKey": "eligibility", "path": ["lv"], "op": "eq", "value": int(match[1])}
        else:
            item.pop("quotaObservation", None)
            item["observationUnavailableReason"] = "该条目的活动等级条件尚未识别"


def parse_module(scripts: dict[str, str], module: str, activity: dict, expand=None) -> tuple[list, list, bool]:
    tables, constants = parse_tables(scripts)
    pending, shops = [], []
    all_text = "\n".join(scripts.values())
    commands = {match[1]: match[2] for match in re.finditer(r'\b(?:const|var)\s+(\w+)\s*:\s*String\s*=\s*"(\d+(?:_[A-Za-z0-9]+)+)"', all_text)}
    has_evolution = False
    for filename, text in scripts.items():
        for match in re.finditer(r"\bstatic\s+(?:const|var)\s+(\w+)\s*:\s*[\w.<>]+\s*=\s*", text):
            key = Path(filename).stem + "." + match[1]
            if key in tables:
                continue
            end = text.find(";", match.end())
            expression = text[match.end():end if end >= 0 else len(text)]
            if "CommonEnhancePrize" in expression and any(name in expression for name in ('"cost"', '"prices"', '"price"', '"daibi"')):
                pending.append({"module": module, "activityName": activity.get("activityName", ""),
                                "table": key, "reason": "指定精灵兑换表包含尚未支持的动态表达式"})
    for table, (filename, value) in tables.items():
        cls = table.split(".", 1)[0]
        for branch, rows in leaf_arrays(value):
            shop_id = next((constants[cls + "." + name] for name in ("SHOP_ID", "ExchangeShopId", "ShopId")
                            if cls + "." + name in constants), 1)
            if type(shop_id) is not int or shop_id <= 0:
                shop_id = 1
            profile = trade_profile(scripts, constants, tables, cls, rows, shop_id)
            goods = []
            for row in rows:
                simple = row.get("simpleParams", "")
                strengthen = row.get("type") == "Strengthen"
                if not (isinstance(simple, str) and "CommonEnhancePrize," in simple or strengthen):
                    continue
                # Sign-in and pass-level rewards have the same reward syntax,
                # but no exchange cost. They are not inserted into the shop.
                if not any(key in row for key in ("cost", "costD", "daibi", "prices", "price")):
                    continue
                identity = next((row[key] for key in ("serverId", "serverIndex", "bi", "dataIndex", "id", "index") if type(row.get(key)) is int), None)
                try:
                    if set(row).intersection(("cost", "costD", "prices", "price")) == set() and "daibi" in row:
                        has_exchange = any("exchange" in name.lower() for name in commands)
                        if not has_exchange and any("progress" in name.lower() for name in commands):
                            # Cumulative ticket thresholds are progress prizes,
                            # not a currency expenditure / exchange cost.
                            continue
                        if not has_exchange:
                            raise ValueError("代币字段未确认是兑换扣费还是累计奖励门槛")
                    if identity is None or identity < 0 or identity > 2**31 - 1:
                        raise ValueError("兑换条目缺少明确的本地编号")
                    conditional = type(row.get("index")) is int and row["index"] < 0
                    if conditional and not (type(row.get("baseOnId")) is int and row["baseOnId"] >= 0):
                        raise ValueError("隐藏条件兑换分支没有明确的关联条目")
                    if strengthen:
                        # Accept a future activity with the same explicit
                        # assembly/filter pattern; no activity/class whitelist.
                        builder = re.search(r'"CommonEnhancePrize,-1,\d+,"\s*\+\s*\w+\["params"\]\s*\+\s*","\s*\+\s*' + re.escape(cls) + r'\.RACE_ID', all_text)
                        if not builder or not re.search(r'\["filter"\]', all_text):
                            raise ValueError("养成配置没有可识别的官方奖励组装/筛选规则")
                        races = positive_ids(row.get("filter"))
                        codes = row.get("params", "")
                        if not supported_enhancement_expression(codes):
                            raise ValueError("养成代码尚待适配")
                    else:
                        has_evolution |= "@sb" in simple
                        codes, races = enhancement(simple, expand)
                    raw_cost = {key: row[key] for key in ("cost", "costD", "daibi", "prices", "price") if key in row}
                    cost = row.get("cost", "")
                    cost_known = isinstance(cost, str) and bool(re.fullmatch(r"[1-9]\d*:[0-9]+:[1-9]\d*(?:[:][0-9]+)?(?:#[1-9]\d*:[0-9]+:[1-9]\d*(?:[:][0-9]+)?)*", cost))
                    cost_known = cost_known and not conditional
                    # For event currencies, retain their literal quantities
                    # and official labels without inventing an item ID.
                    cost_description = ""
                    if type(row.get("daibi")) is int:
                        label = constants.get(cls + ".DaibiName")
                        if isinstance(label, str):
                            cost_description = str(row["daibi"]) + " " + label
                    start = row.get("shelfTime") or activity.get("startTime", "")
                    end = row.get("removalTime") or activity.get("endTime", "")
                    start = official_date(start) if isinstance(start, str) and start else ""
                    end = official_date(end) if isinstance(end, str) and end else ""
                    item = {"id": row.get("id", identity), "itemServerId": identity,
                            "description": row.get("basicDescription") or row.get("desc") or row.get("name") or "指定精灵养成",
                            "tab": row.get("tab", row.get("tabId", 0)), "cost": cost if cost_known else "", "costKnown": cost_known,
                            "costRaw": raw_cost, "costDescription": cost_description, "enhanceType": codes, "raceIds": races,
                            "shelfTime": start, "removalTime": end, "availableKnown": bool(start),
                            "limit": str(row.get("limit", "")), "limitIndex": -1, "limitCount": -1,
                            "limitKey": "", "limitLabel": "", "quotaKnown": False,
                            "quotaRaw": {key: row[key] for key in ("limit", "maxNum", "limitType", "specBi", "baseOnId", "unlock", "ypFlag", "lvFlag", "lv") if key in row},
                            "unlock": ("关联条目 %s 的条件兑换，适用状态待确认" % row["baseOnId"] if conditional else row.get("unlock") or row.get("lvFlag", "")),
                            "conditionsRaw": {key: row[key] for key in ("index", "baseOnId", "specBi", "filter", "lvFlag", "ypFlag") if key in row},
                            "rewardRaw": simple or row,
                            "source": {"configClass": table, "configFile": filename, "commands": commands,
                                       "shelfTimeSource": "row" if row.get("shelfTime") else "activity",
                                       "removalTimeSource": "row" if row.get("removalTime") else "activity",
                                       "selection": "explicit-literal-filter" if strengthen else "CommonEnhancePrize"}}
                    enrich_trade_item(item, row, profile)
                    if any(previous["itemServerId"] == identity for previous in goods):
                        raise ValueError("同表兑换编号重复，不能合并不同条目")
                    goods.append(item)
                except (ValueError, TypeError) as error:
                    pending.append({"module": module, "activityName": activity.get("activityName", ""),
                                    "table": table + branch, "itemId": identity, "reason": str(error)})
            if goods:
                observation = {"schema": 1, "requests": profile["requests"]}
                if profile.get("pendingRequest"):
                    observation["pendingRequest"] = profile["pendingRequest"]
                    observation["unavailableReason"] = "该官方读取需要客户端生成变更序号，静态配置可用，当前次数尚未读取"
                shops.append({"sourceKey": module + "#" + table + branch, "shopId": shop_id,
                              "name": "活动·" + activity.get("activityName", activity.get("name", "")),
                              "activityName": activity.get("activityName", ""), "goods": goods, "observation": observation,
                              "source": {"module": module, "activityAlias": activity.get("name", ""),
                                         "class": table, "commands": commands, "quotaSupported": any("quotaObservation" in good for good in goods)}})
    if "CommonEnhancePrize" in all_text and not shops and not pending:
        # Normal rewards are explicitly recognized as non-exchanges, while a
        # dynamic cost/reward construction remains visible for later support.
        if not any("CommonEnhancePrize" in json.dumps(value, ensure_ascii=False) for _, value in tables.values()):
            pending.append({"module": module, "activityName": activity.get("activityName", ""),
                            "reason": "发现养成奖励引用，但静态兑换结构尚未识别"})
    return shops, pending, has_evolution


def export_scripts(updater, swf: Path, target: Path) -> dict[str, str]:
    if hasattr(updater, "activity_exporter"):
        return updater.activity_exporter(swf, target)
    from public_data_updater import runtime_entry
    if updater.runtimes is None:
        updater.runtimes = (runtime_entry(updater.tools, "java", updater.scratch), runtime_entry(updater.tools, "ffdec", updater.scratch))
    java, ffdec = updater.runtimes
    result = subprocess.run([str(java), "-Xmx512m", "-Djava.awt.headless=true", "-jar", str(ffdec),
                             "-export", "script", str(target), str(swf)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=120,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise ValueError("官方活动静态配置导出失败")
    paths = list(target.rglob("*.as"))
    if len(paths) > 5000 or sum(path.stat().st_size for path in paths) > MAX_SWF:
        raise ValueError("官方活动脚本过大")
    return {path.relative_to(target).as_posix(): path.read_text(encoding="utf-8-sig") for path in paths}


def preserve_pending_rows(previous: dict, shops: list, pending: list) -> list:
    """Keep only prior rows implicated by an unparsed new table/row."""
    if not pending or not previous.get("shops"):
        return shops
    result = copy.deepcopy(shops)
    current = {shop["sourceKey"]: shop for shop in result}
    for prior in previous["shops"]:
        table = prior.get("sourceKey", "").split("#", 1)[-1]
        issues = [issue for issue in pending if not issue.get("table") or issue.get("table") == table]
        if not issues:
            continue
        for row in prior.get("goods", []):
            if not any(issue.get("itemId") is None or issue.get("itemId") == row.get("itemServerId") for issue in issues):
                continue
            target = current.get(prior["sourceKey"])
            if target is None:
                target = {**copy.deepcopy(prior), "goods": []}
                current[prior["sourceKey"]] = target
                result.append(target)
            if any(good.get("itemServerId") == row.get("itemServerId") for good in target["goods"]):
                continue
            retained = copy.deepcopy(row)
            retained.update(catalogStale=True, availableKnown=False, costKnown=False)
            retained["unlock"] = "官方新配置暂未识别，沿用上次目录；适用状态待确认"
            target["goods"].append(retained)
    return result


def update_activity_exchanges(updater, versions: dict) -> bool:
    from public_icon_updater import loader_default
    from activity_evolution_selector import dependency_revision
    cache_path = updater.root / "catalog" / FILENAME
    on_disk = _read(cache_path)
    current = on_disk
    if not cache_path.exists():
        baseline = _read(updater.baseline / FILENAME)
        if (baseline.get("schema") == 1 and baseline.get("parserVersion") == PARSER_VERSION and
                isinstance(baseline.get("source"), dict) and isinstance(baseline.get("modules"), dict) and
                isinstance(baseline.get("shops"), list) and isinstance(baseline.get("pending"), list)):
            # This copy was embedded and validated with the release. Reuse its
            # positive and negative module records against today's versions;
            # a first manual check need not download unchanged modules again.
            current = baseline
    updater.activity_exchange_failures = []
    previous_source = current.get("source", {})
    previous_modules = current.get("modules", {})
    config_version = versions.get(CONFIG_RESOURCE)
    if not re.fullmatch(r"\d{8,20}", config_version or ""):
        raise ValueError("官方活动目录资源版本缺失")
    discovery = previous_source.get("discovery", {})
    discovery_versions = {key: versions[key] for key in (CONFIG_RESOURCE, EMERGENCY_RESOURCE, BLOCK_RESOURCE) if key in versions}
    if previous_source.get("discoveryVersions") != discovery_versions or not discovery.get("entries") or current.get("parserVersion") != PARSER_VERSION:
        emit("更新官方活动入口与当前发布目录")
        swf, config_source = updater.resource(CONFIG_RESOURCE, versions)
        hud = updater.export(swf, updater.scratch / "activity-hud", "mmo.config.CommonHudConfig")
        discovery = build_discovery(registry_xml(swf), hud.read_text(encoding="utf-8-sig"))
        override_resources = apply_discovery_overrides(updater, versions, discovery)
    else:
        config_source = previous_source.get("configResource", {})
        override_resources = previous_source.get("overrideResources", {})
    default = previous_source.get("loaderDefault", {})
    queue = [(alias, 0) for alias in discovery["roots"]]
    visited, modules, pending, shops = set(), {}, [], []
    checked = 0
    while queue:
        alias, depth = queue.pop(0)
        resolved = resolve_alias(alias, discovery)
        if not resolved:
            continue
        module, activity = resolved
        if module == MAIN_SHOP_RESOURCE or module in visited:
            continue
        if len(visited) >= MAX_MODULES:
            pending.append({"activityName": activity.get("activityName", ""), "module": module,
                            "reason": "当前活动引用数量超过可处理范围，保留已识别项目"})
            continue
        visited.add(module)
        version = versions.get(module)
        if not version:
            default = loader_default(updater, {"source": {"loaderDefault": default}})
            version = default["defaultVersion"]
        # Unversioned official resources use the loader default. Recheck them
        # when the official registry changes, not on every manual button press.
        token = version if module in versions else version + "@" + json.dumps(discovery_versions, sort_keys=True, separators=(",", ":"))
        previous = previous_modules.get(module, {})
        evo_version = dependency_revision(versions)
        reusable = (current.get("parserVersion") == PARSER_VERSION and previous.get("versionToken") == token and
                    (not previous.get("hasEvolutionSelector") or previous.get("evolutionVersion") == evo_version))
        if reusable and previous.get("status") != "failed":
            record = copy.deepcopy(previous)
            # A HUD rename or explicit opening date can change while its
            # versioned exchange module stays byte-identical.
            for shop in record.get("shops", []):
                shop["activityName"] = activity.get("activityName", "")
                shop["name"] = "活动·" + shop["activityName"]
                for good in shop.get("goods", []):
                    if good.get("source", {}).get("shelfTimeSource") == "activity":
                        date = activity.get("startTime", "")
                        good["shelfTime"] = official_date(date) if date else ""
                        good["availableKnown"] = bool(good["shelfTime"])
                    if good.get("source", {}).get("removalTimeSource") == "activity":
                        date = activity.get("endTime", "")
                        good["removalTime"] = official_date(date) if date else ""
                    if good.get("catalogStale"):
                        good["availableKnown"] = False
            for issue in record.get("pending", []):
                issue["activityName"] = activity.get("activityName", "")
        else:
            checked += 1
            emit("检查活动兑换：" + activity.get("activityName", alias))
            try:
                path = updater.scratch / ("activity-" + hashlib.sha256(module.encode()).hexdigest()[:20] + ".swf")
                resource = updater.fetch("https://aoqi.100bt.com/play/" + module + "~" + version + ".swf", path)
                body = swf_body(path.read_bytes())
                strings = strings_in_swf(body)
                references = set()
                for value in strings:
                    if value in discovery["entries"]:
                        references.add(value)
                    references.update(re.findall(r"btnNewAct_([A-Za-z0-9]+)_", value))
                module_shops, unsupported, has_evolution = [], [], False
                if any("CommonEnhancePrize" in value for value in strings):
                    scripts = export_scripts(updater, path, updater.scratch / ("activity-export-" + hashlib.sha256(module.encode()).hexdigest()[:20]))
                    def expand(seeds):
                        from activity_evolution_selector import expand_selector
                        return expand_selector(updater, versions, seeds)
                    module_shops, unsupported, has_evolution = parse_module(scripts, module, activity, expand)
                    module_shops = preserve_pending_rows(previous, module_shops, unsupported)
                record = {"versionToken": token, "version": version, "source": resource,
                          "status": "supported" if module_shops else "pending" if unsupported else "no-designated-exchange",
                          "shops": module_shops, "pending": unsupported, "references": sorted(references),
                          "hasEvolutionSelector": has_evolution, "evolutionVersion": evo_version}
            except Exception as error:
                record = {**previous, "status": "failed", "error": str(error)}
                updater.activity_exchange_failures.append(activity.get("activityName", alias) + "：" + str(error))
        modules[module] = record
        shops.extend(record.get("shops", []))
        pending.extend(record.get("pending", []))
        if depth < 3:
            queue.extend((name, depth + 1) for name in record.get("references", []))
        elif record.get("references"):
            for name in record["references"]:
                target = resolve_alias(name, discovery)
                if target and target[0] not in visited:
                    pending.append({"activityName": activity.get("activityName", ""), "module": target[0],
                                    "reason": "活动多层引用仍需进一步解析"})
    result = {"schema": 1, "parserVersion": PARSER_VERSION,
              "selection": "official live activities; explicit designated-pet enhancement exchanges only",
              "source": {"kind": "aoqi-official-activity-registry", "configVersion": config_version,
                         "configResource": config_source, "discoveryVersions": discovery_versions,
                         "overrideResources": override_resources, "loaderDefault": default, "discovery": discovery},
              "shops": sorted(shops, key=lambda shop: shop["sourceKey"]), "pending": pending,
              "modules": modules, "coverage": {"activityModules": len(modules), "shops": len(shops),
                          "goods": sum(len(shop["goods"]) for shop in shops), "pending": len(pending)}}
    changed = result != current
    if result != on_disk:
        _atomic_json(cache_path, result)
    updater.activity_exchange_pending = pending
    emit(f"活动兑换：{len(shops)} 个目录、{sum(len(shop['goods']) for shop in shops)} 项；本次检查 {checked} 个变更模块" +
         (f"；{len(pending)} 项规则待适配" if pending else ""))
    return changed
