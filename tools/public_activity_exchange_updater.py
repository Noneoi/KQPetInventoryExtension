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
from generate_shop_exchange_data import official_date, selectable_reward, selectable_package_ids, relevant_shops, targeted_reward, excluded_activity
from activity_reward_structures import (normalize_tables, reward_hints, symbolic, description_parameters,
                                        upgrade_cultivation_description, module_deadline, apply_activity_period)
from activity_periods import (period_hints, period_metadata, entry_opening, load_panel_periods,
                             panel_periods_reusable, apply_graph_periods, table_deadlines)
from activity_trade_facts import table_contracts, enrich_reward_facts

PARSER_VERSION = 18
FILENAME = "activity-exchange-data.json"
CONFIG_RESOURCE = "config/config"
EMERGENCY_RESOURCE = "configinemergency/configinemergency"
BLOCK_RESOURCE = "configselfblock/configselfblock"
MAIN_SHOP_RESOURCE = "newactivityext/newact20260313/storeexchangeframework/storeexchangeframework"
RESOURCE_PATTERN = re.compile(r"newactivityext/newact[0-9]{8}/[A-Za-z0-9_]+/[A-Za-z0-9_]+")
MAX_SWF = 64 * 1024 * 1024
MAX_MODULES = 512
STATIC_DECLARATION = re.compile(r"\bstatic\s+(?:const|var)\s+(\w+)\s*:\s*[\w.*<>]+\s*=\s*")

PACKAGE_RESOURCES = (("library/materialdata", "mmo.materialdata.OpenablePackageData"),
                     ("library/materialdataupdate", "mmo.materialdata.update.OpenablePackageData_Update"))


def parse_selectable_packages(text):
    """Official OpenablePackage.isRandomItemPackage checks positive % rates."""
    packages = {}
    for match in re.finditer(r'new\s+OpenablePackage\s*\(', text):
        expression, _ = _balanced(text, match.end() - 1)
        args = LiteralReader("[" + expression[1:-1] + "]").parse()
        if len(args) not in (5, 6) or type(args[0]) is not int or args[0] <= 0 or not isinstance(args[2], str):
            raise ValueError("官方可开启礼包定义结构待适配")
        parts = args[2].split("#")
        if any(not re.fullmatch(r'(?:[1-9]\d*:[1-9]\d*:[1-9]\d*|Pet(?:Skin|Gain):[1-9]\d*)(?:%(?:\d+(?:\.\d+)?))?', part) for part in parts):
            raise ValueError("官方礼包内容结构待适配")
        selectable = not any(float(part.split("%",1)[1]) > 0 for part in parts if "%" in part)
        # Keep random rows as tombstones so an update can change an old choice
        # into a random package without inheriting the old classification.
        packages[str(args[0])] = {"name": args[1], "content": args[2], "selectable": selectable}
    if "new OpenablePackage" not in text and not re.search(r'\bdata\s*:\s*Object\s*=\s*\{\s*\}', text):
        raise ValueError("官方礼包静态目录未识别")
    return packages


def load_selectable_packages(updater, versions):
    dependency = {key: versions[key] for key, _ in PACKAGE_RESOURCES if key in versions}
    previous = _read(updater.root / "catalog" / FILENAME).get("source", {}).get("selectablePackages", {})
    if len(dependency) != len(PACKAGE_RESOURCES):
        return {"versions": {}, "packages": {}}
    if previous.get("versions") == dependency and isinstance(previous.get("packages"), dict):
        return previous
    packages, resources = {}, {}
    for key, cls in PACKAGE_RESOURCES:
        swf, source = updater.resource(key, versions)
        path = updater.export(swf, updater.scratch / ("choice-packages-" + key.rsplit("/",1)[-1]), cls)
        packages.update(parse_selectable_packages(path.read_text(encoding="utf-8-sig")))
        resources[key] = source
    return {"versions": dependency, "resources": resources,
            "packages": {key: row for key, row in packages.items() if row["selectable"]}}


def load_reward_support(updater, versions):
    resources = (('library/interfaces', 'mmo.interfaces.strengthencombo.StrengthenComboBasicType'),
                 ('library/interfaces', 'mmo.interfaces.strengthencombo.StrengthenComboDisplayType'),
                 ('strengthencombo/strengthencomboservice', 'mmo.strengthencombo.update.StrengthenComboContents'),
                 ('strengthencombo/strengthencomboservice', 'mmo.strengthencombo.util.StrengthenComboDescUtil'))
    dependency = {key: versions[key] for key, _ in resources if key in versions}
    previous = _read(updater.root / 'catalog' / FILENAME).get('source', {}).get('rewardSupport', {})
    if previous.get('versions') == dependency and previous.get('schema') == 2 and not previous.get('error'):
        return previous
    result = {'schema': 2, 'versions': dependency, 'basicTypes': {}, 'prices': {}, 'pets': {}, 'descriptions': {}, 'descriptionParameters': {}}
    if len(dependency) < 2:
        return result
    try:
        emit('更新公共养成奖励与强化类型定义')
        for key, cls in resources:
            swf, _ = updater.resource(key, versions)
            file = updater.export(swf, updater.scratch / 'reward-support', cls)
            source = file.read_text(encoding='utf-8-sig')
            if cls.endswith('StrengthenComboContents'):
                _, constants = parse_tables({file.name: source})
                prices = constants.get('StrengthenComboContents.PRICE_CONTENTS')
                pets = constants.get('StrengthenComboContents.PET_CONTENTS')
                if not isinstance(prices, dict) or not isinstance(pets, list) or not prices or not pets or symbolic(prices) or symbolic(pets):
                    raise ValueError('公共养成奖励目录结构待适配')
                result['prices'] = prices
                result['pets'] = {str(p['race']): p for p in pets if isinstance(p, dict) and 'race' in p}
            elif cls.endswith('StrengthenComboBasicType'):
                for match in re.finditer(r'\bconst\s+(\w+)\s*:\s*StrengthenComboBasicType\s*=\s*create\((\d+),\s*("(?:\\.|[^"\\])*"),', source):
                    name = json.loads(match[3]).split('|')[0]
                    result['basicTypes'][match[1]] = {'id': int(match[2]), 'name': name}
                    result['descriptions'][match[2]] = name
                if not result['basicTypes']:
                    raise ValueError('公共强化类型定义结构待适配')
            elif cls.endswith('StrengthenComboDescUtil'):
                result['descriptionParameters'] = description_parameters(source, result['basicTypes'])
            else:
                match = re.search(r'\bTESTING_HIDDEN\s*:\s*StrengthenComboDisplayType\s*=\s*create\((\d+),', source)
                if not match:
                    raise ValueError('公共养成测试项目的隐藏规则待适配')
                result['hiddenDisplayId'] = int(match[1])
    except Exception as error:
        # Independent event tables can still update if a shared dependency
        # fails. Never reuse stale public package contents as current facts.
        result.update(error=str(error), prices={}, pets={})
        updater.activity_exchange_failures.append('公共养成奖励配置读取失败：' + str(error))
    return result


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
    # An alias can be reissued in a later week.  Preserve the newest official
    # row and the week that supplied it instead of relying on XML traversal
    # order (which used to make "new activity" detection ambiguous).
    for week in sorted(weeks, key=lambda value: value.get("version", "")):
        for node in week.findall("a"):
            alias = node.get("name", "")
            if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{2,100}", alias):
                entries[alias] = {**node.attrib, "releaseVersion": week.get("version", "")}
        if week.get("version") >= cutoff:
            recent.update(node.get("name") for node in week.findall("a"))
    hud = json.loads(_declaration(hud_text, "DATA", "{"))
    pending, hud_entries = [hud], {}
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            service = value.get("tryGetService", "")
            pieces = service.split("#") if isinstance(service, str) else []
            if len(pieces) >= 3 and pieces[0] == "NewActivityService" and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{2,100}", pieces[2]):
                alias = pieces[2]
                hud_entries[alias] = {"name": value.get("name", ""), "startTime": value.get("startTime", ""),
                                      "endTime": value.get("endTime", ""), "tryGetService": service}
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    evidence = {alias: {"kind": "recent-release", "date": entries.get(alias, {}).get("releaseVersion", "")}
                for alias in recent if alias}
    for alias, value in hud_entries.items():
        evidence[alias] = {"kind": "hud", "date": value.get("startTime", "") or latest}
    return {"latestRelease": latest, "recentSince": cutoff, "entries": entries,
            "hud": hud_entries, "rootEvidence": evidence,
            "roots": sorted(recent | set(hud_entries))}


def navigation_link(alias: str, row: dict, hud: dict) -> str:
    service = hud.get("tryGetService", "") if isinstance(hud, dict) else ""
    pieces = service.split("#") if isinstance(service, str) else []
    if len(pieces) >= 4 and pieces[:2] == ["NewActivityService", "loadAndInitNormalActivity"] and pieces[2] == alias:
        candidate = "btnNewAct_" + alias + "_" + "_".join(pieces[3:])
    else:
        candidate = row.get("link", "")
        if not isinstance(candidate, str) or not candidate.startswith("btnNewAct_"):
            # A likely showMainPanel name is not proof that the activity
            # actually exposes that action.  Leave navigation disabled unless
            # the official HUD/registry or the module's RESHOW_SELF_ACT_KEY
            # supplies an exact route.
            return ""
    return candidate if len(candidate) <= 256 and re.fullmatch(
        r"btnNewAct_[A-Za-z][A-Za-z0-9]{1,100}(?:_[A-Za-z0-9]{1,64}){1,8}", candidate) else ""


def resolve_alias(alias: str, discovery: dict) -> tuple[str, dict] | None:
    seen = set()
    requested = alias
    requested_evidence = discovery.get("rootEvidence", {}).get(requested, {"kind": "referenced", "date": ""})
    direct_link = ""
    period_evidence = []
    while alias not in seen:
        seen.add(alias)
        if alias in discovery.get("blocked", []):
            return None
        row = discovery["entries"].get(alias, {})
        hud = discovery.get("hud", {}).get(alias, {})
        for kind, entry in (('registry', row), ('hud', hud)):
            period = {key: entry[key] for key in ('startTime', 'endTime') if entry.get(key)}
            if period:
                period_evidence.append({**period, 'kind': kind, 'alias': alias})
        if str(row.get("online", "true")).lower() == "false":
            return None
        path = row.get("file", "")
        if RESOURCE_PATTERN.fullmatch(path):
            metadata = dict(row)
            hud = discovery.get("hud", {}).get(alias, {})
            metadata["activityName"] = hud.get("name") or row.get("desc") or alias
            metadata["startTime"] = hud.get("startTime") or row.get("startTime", "")
            metadata["endTime"] = hud.get("endTime") or row.get("endTime", "")
            metadata["activityAlias"] = alias
            metadata['periodEvidence'] = period_evidence
            metadata["navigationLink"] = direct_link or navigation_link(alias, row, hud)
            target_evidence = discovery.get("rootEvidence", {}).get(alias, {})
            evidence = target_evidence if target_evidence.get("kind") == "hud" else requested_evidence
            metadata["activityEvidence"] = evidence.get("kind", "referenced")
            metadata["activityEvidenceDate"] = evidence.get("date", "")
            return path, metadata
        link = row.get("link", "")
        match = re.match(r"btnNewAct_([A-Za-z0-9]+)_", link)
        if not match:
            return None
        if not direct_link:
            direct_link = link if navigation_link(match[1], {"link": link}, {}) else ""
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
                        discovery.setdefault("rootEvidence", {})[alias] = {
                            "kind": "emergency", "date": discovery.get("latestRelease", "")}
                        roots.add(alias)
            for node in xml.iter("hud"):
                pieces = node.get("tryGetService", "").split("#")
                if len(pieces) >= 3 and pieces[0] == "NewActivityService":
                    discovery["hud"][pieces[2]] = {"name": node.get("name", ""), "startTime": node.get("startTime", ""),
                                                    "endTime": node.get("endTime", ""), "tryGetService": node.get("tryGetService", "")}
                    discovery.setdefault("rootEvidence", {})[pieces[2]] = {
                        "kind": "hud", "date": node.get("startTime", "") or discovery.get("latestRelease", "")}
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
    pattern = re.compile(r'\s*("(?:\\.|[^"\\])*"|-?\d+(?:\.\d+)?|[A-Za-z_$][A-Za-z0-9_$.]*|[][{}:,+()])')
    def __init__(self, text, *, static_calls=False):
        self.tokens, self.at = [], 0
        self.static_calls = static_calls
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
    def atom(self, depth=0):
        result = self.primary(depth)
        while self.at < len(self.tokens) and self.tokens[self.at] == "[":
            self.take()
            index = self.value(depth + 1)
            if self.take() != "]":
                raise ValueError("静态索引不完整")
            result = {"$index": result, "$key": index}
        return result
    def primary(self, depth=0):
        if depth > 64:
            raise ValueError("静态配置嵌套过深")
        token = self.take()
        if token == "new":
            constructor = self.take()
            if not re.fullmatch(r"[A-Za-z_$][A-Za-z0-9_$.]*", constructor) or self.take() != "(":
                raise ValueError("静态构造参数无效")
            args = []
            while self.tokens[self.at] != ")":
                args.append(self.value(depth + 1))
                if self.tokens[self.at] != ")" and self.take() != ",":
                    raise ValueError("静态构造参数分隔符无效")
            self.take()
            return {"$new": constructor, "$args": args}
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
        if token == "(":
            result = self.value(depth + 1)
            if self.take() != ")":
                raise ValueError("静态括号不完整")
            return result
        if self.at < len(self.tokens) and self.tokens[self.at] == "(":
            if not self.static_calls:
                raise ValueError('函数调用只能由静态配置解析器解析为符号')
            self.take()
            args = []
            while self.tokens[self.at] != ")":
                args.append(self.value(depth + 1))
                if self.tokens[self.at] != ")" and self.take() != ",":
                    raise ValueError("静态函数参数分隔符无效")
            self.take()
            return {"$call": token, "$args": args}
        return {"$ref": token}
    def value(self, depth=0):
        result = self.atom(depth)
        if self.at >= len(self.tokens) or self.tokens[self.at] != "+":
            return result
        pieces = [result]
        while self.at < len(self.tokens) and self.tokens[self.at] == "+":
            self.take()
            pieces.append(self.atom(depth))
        # AS '+' is intentionally supported only for a visibly string-based
        # constant concatenation.  Numeric arithmetic and runtime expressions
        # remain rejected rather than being evaluated or guessed.
        if not any(isinstance(piece, str) for piece in pieces) or any(
                type(piece) not in (str, int) and not (
                    isinstance(piece, dict) and set(piece) == {"$ref"})
                for piece in pieces):
            raise ValueError("只支持静态字符串常量拼接")
        return {"$concat": pieces}
    def parse(self):
        result = self.value()
        if self.at != len(self.tokens):
            raise ValueError("静态配置包含动态表达式")
        return result


def parse_tables(scripts: dict[str, str]) -> tuple[dict, dict]:
    constants, tables = {}, {}
    constructors, functions = {}, {}
    for filename, text in scripts.items():
        cls = Path(filename).stem
        for match in re.finditer(r'\bstatic\s+function\s+(\w+)\s*\(([^)]*)\)\s*:\s*[\w.*<>]+\s*\{\s*return\s+([^;]+);\s*\}', text):
            expression = match[3].strip()
            # A copied static array and a read-only index are the only method
            # bodies interpreted here. No AS code or arbitrary call executes.
            expression = re.sub(r'^\((.+)\s+as\s+Array\)\.concat\(\)$', r'\1', expression)
            expression = re.sub(r'\.concat\(\)$', '', expression)
            try:
                functions[cls + "." + match[1]] = (re.findall(r'(\w+)\s*:', match[2]), LiteralReader(expression, static_calls=True).parse())
            except (ValueError, IndexError):
                pass
    for filename, text in scripts.items():
        cls = Path(filename).stem
        match = re.search(r"function\s+" + re.escape(cls) + r"\s*\([^)]*\)\s*\{", text)
        if not match:
            continue
        try:
            body, _ = _balanced(text, match.end() - 1)
        except ValueError:
            continue
        fields = dict(re.findall(r"this\.(\w+)\s*=\s*(param\d+)\s*;", body))
        mapping = {field: int(param[5:]) - 1 for field, param in fields.items() if not field.startswith("_")}
        for getter in re.finditer(r"function\s+get\s+(\w+)\s*\(\s*\)\s*:\s*[\w.<>]+\s*\{\s*return\s+this\.(\w+)\s*;\s*\}", text):
            if getter[2] in fields:
                mapping[getter[1]] = int(fields[getter[2]][5:]) - 1
        if mapping:
            constructors[cls] = mapping
    for filename, text in scripts.items():
        cls = Path(filename).stem
        for match in STATIC_DECLARATION.finditer(text):
            key, begin = cls + "." + match[1], match.end()
            try:
                if text[begin:begin + 1] in ("[", "{"):
                    expression, end = _balanced(text, begin)
                else:
                    end = text.index(";", begin)
                    expression = text[begin:end]
                vector = re.fullmatch(r'Vector\.<[\w.]+>\s*\((.*)\)', expression.strip(), re.S)
                value = LiteralReader(vector[1] if vector else expression, static_calls=True).parse()
                constants[key] = value
                if isinstance(value, (dict, list)) and "$ref" not in value:
                    tables[key] = (filename, value)
                elif isinstance(value, str) and re.match(r"!?(?:Material|CommonEnhancePrize|Pet|Choice|SelectPrizes),", value):
                    tables[key] = (filename, {"simpleParams": value})
            except (ValueError, IndexError, json.JSONDecodeError):
                continue
    def resolve(value, namespace="", stack=(), arguments=None):
        if len(stack) > 64:
            return {"$ref": "static-resolution-depth"}
        if isinstance(value, dict):
            if set(value) == {"$new", "$args"}:
                mapping = constructors.get(value["$new"].rsplit(".", 1)[-1])
                if not mapping or any(index >= len(value["$args"]) for index in mapping.values()):
                    return {"$ref": "unresolved-constructor"}
                return {field: resolve(value["$args"][index], namespace, stack) for field, index in mapping.items()}
            if set(value) == {"$ref"}:
                name = value["$ref"]
                if arguments and name in arguments:
                    return arguments[name]
                qualified = name if name in constants else namespace + "." + name
                if qualified in constants and qualified not in stack:
                    return resolve(constants[qualified], qualified.split(".", 1)[0], stack + (qualified,))
                return value
            if set(value) == {"$index", "$key"}:
                target = resolve(value["$index"], namespace, stack, arguments)
                key = resolve(value["$key"], namespace, stack, arguments)
                if isinstance(target, list) and type(key) is int and 0 <= key < len(target):
                    return target[key]
                if isinstance(target, dict) and isinstance(key, str) and key in target:
                    return target[key]
                return {"$ref": "unresolved-static-index"}
            if set(value) == {"$call", "$args"}:
                name = value["$call"]
                qualified = name if name in functions else namespace + "." + name
                args = [resolve(arg, namespace, stack, arguments) for arg in value["$args"]]
                if qualified in functions and qualified not in stack:
                    params, expression = functions[qualified]
                    if len(params) == len(args):
                        return resolve(expression, qualified.split(".", 1)[0], stack + (qualified,), dict(zip(params, args)))
                return {"$ref": "unresolved-call:" + name}
            if set(value) == {"$concat"}:
                pieces = [resolve(child, namespace, stack) for child in value["$concat"]]
                # AS string + integer is a string (common for tab navigation).
                if pieces and isinstance(pieces[0], str) and all(type(piece) in (str, int) for piece in pieces):
                    return "".join(str(piece) for piece in pieces)
                return {"$concat": pieces}
            return {key: resolve(child, namespace, stack, arguments) for key, child in value.items()}
        if isinstance(value, list):
            return [resolve(child, namespace, stack, arguments) for child in value]
        return value
    def symbolic(value):
        if isinstance(value, dict):
            return "$ref" in value or "$concat" in value or any(symbolic(child) for child in value.values())
        return isinstance(value, list) and any(symbolic(child) for child in value)
    resolved = {key: (filename, resolve(value, key.split(".", 1)[0]))
                for key, (filename, value) in tables.items()}
    return ({key: pair for key, pair in resolved.items() if not symbolic(pair[1])},
            {key: resolve(value, key.split(".", 1)[0]) for key, value in constants.items()})


def positive_ids(value) -> list[int]:
    if not isinstance(value, list) or not value or any(type(n) is not int or n <= 0 or n > 2**31 - 1 for n in value):
        raise ValueError("指定精灵列表不是明确的正整数 ID")
    return list(dict.fromkeys(value))


def supported_enhancement_expression(value) -> bool:
    # These optional parameters have been checked against the official
    # StrengthenComboItem / star-flow implementations. Other $ arguments stay
    # explicit pending instead of being silently discarded.
    # Parameter variants stay evidence-bound.  39$3 and 89$1 are present in
    # the official 2026-09-18 cultivation sale; other new variants remain
    # pending until an official table gives them a concrete meaning.
    # The official return/new-player tables use 35$<star-definition-id>.
    # Preserve the parameter; do not treat it as the general red-star effect.
    token = r"(?:[1-9]\d*|(?:33|35)\$[1-9]\d*|39\$(?:1|3)|89\$1)"
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


def reward_row(value):
    if not isinstance(value, dict):
        return None
    if "simpleParams" in value or value.get("type") == "Strengthen" and 'params' in value:
        return value
    for key in ("prizeParams", "prize", "prizes", "reward", "rewards", "rewardParams"):
        raw = value.get(key)
        if isinstance(raw, list) and raw and all(isinstance(item, str) for item in raw):
            raw = "|".join(raw)
        if isinstance(raw, str) and re.match(r"!?(?:Material|CommonEnhancePrize|Pet|Choice|SelectPrizes),", raw):
            return {**value, "simpleParams": raw}
    return None


def additional_reward_fields(value):
    if not isinstance(value, dict):
        return []
    primary = reward_row(value)
    result = []
    for key, raw in value.items():
        # Passes and staged events keep several reward lanes in one row.
        # Preserve each lane separately; its id/eligibility is not the row id.
        if not re.match(r"(?:prize|reward|bonus|extBonus)", key, re.I):
            continue
        if not isinstance(raw, str) or not re.match(r"!?(?:Material|CommonEnhancePrize|Pet|Choice|SelectPrizes),", raw):
            continue
        if primary is not None and raw == primary.get("simpleParams"):
            continue
        result.append((key, {**value, "simpleParams": raw, "_rewardField": key}))
    return result


def leaf_arrays(value, path=""):
    if isinstance(value, list):
        rows = [reward_row(row) for row in value]
        rows = [row for row in rows if row is not None]
        if rows:
            yield path, rows
        extras = {}
        for row in value:
            for key, projected in additional_reward_fields(row):
                extras.setdefault(key, []).append(projected)
        for key, projected in extras.items():
            yield path + "/" + key, projected
        literal_prizes = [{"simpleParams": item} for item in value if isinstance(item, str) and
                          re.match(r"!?(?:Material|CommonEnhancePrize|Pet|Choice|SelectPrizes),", item)]
        if literal_prizes:
            yield path + "/prizes", literal_prizes
        for index, child in enumerate(value):
            if isinstance(child, (list, dict)) and reward_row(child) is None and not additional_reward_fields(child):
                yield from leaf_arrays(child, path + "/" + str(index))
    elif isinstance(value, dict):
        row = reward_row(value)
        if row is not None:
            yield path, [row]
        extras = additional_reward_fields(value)
        for key, projected in extras:
            yield path + "/" + key, [projected]
        if row is None and not extras:
            for name, child in value.items():
                if isinstance(child, (list, dict)):
                    yield from leaf_arrays(child, path + "/" + name)


def acquisition_kind(row, table, activity, commands):
    if row.get('_acquisitionKind') in ('exchange', 'progress', 'signin', 'reward', 'lottery'):
        return row['_acquisitionKind']
    if row.get("_acquisitionUnknown"):
        return "unknown"
    if 'probability' in row or 'guaranteed' in row:
        return 'lottery'
    has_cost = any(key in row for key in ("cost", "costD", "prices", "price", "discount", "orgPrice"))
    if has_cost:
        return "exchange"
    if "daibi" in row:
        if any("exchange" in name.lower() for name in commands):
            return "exchange"
        if any("progress" in name.lower() for name in commands):
            return "progress"
        return "unknown"
    label = table.lower() + " " + activity.get("activityName", "")
    if any(word in label for word in ("signin", "sign_in", "qiandao", "签到")):
        return "signin"
    if any(word in label for word in ("progress", "levelprize", "passtask", "累计")):
        return "progress"
    return "reward"


def reward_branches(value, config_text):
    """Keep explicitly filtered player tiers in separate identity namespaces.

    Tier values are catalog variants, not evidence of the current player's
    tier. Their counters/prices must not be applied to the account.
    """
    tier_filter = re.search(r'\["lv"\]\s*==\s*[A-Za-z_]\w*\b', config_text)
    for branch, rows in leaf_arrays(value):
        if tier_filter and rows and all(type(row.get("lv")) is int and row["lv"] > 0 for row in rows):
            for level in sorted({row["lv"] for row in rows}):
                yield branch + "/lv=" + str(level), [row for row in rows if row["lv"] == level], level
        else:
            yield branch, rows, None


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
    profile['stateReadUnambiguous'] = len(requests) == 1
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
                elif (any(type(row.get("buyId")) is int and type(row.get("index")) is int for row in rows) and
                      re.search(r'(\w+)\s*=\s*\w+\["b"\s*\+\s*(\w+)\.buyId\][\s\S]{0,400}?'
                                r'int\(\s*\1\["by"\s*\+\s*\2\.index\]\s*\)', text)):
                    profile["quotaStyle"] = "nested-buy-index"
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
    if isinstance(limit, str) and re.fullmatch(r"\d+:\d+", limit):
        kind, maximum = map(int, limit.split(":"))
        if kind <= 4 and (style == "bundle-bi" or style == "total-bi" and kind == 4):
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
        elif style == "nested-buy-index":
            observation.update(path=["b" + str(row["buyId"]), "by" + str(row["index"])])
        elif style == "p-index":
            observation.update(path=["p", {"find": "i", "equals": row.get("index", identity)}, "l"], missingValue=0)
        if observation.get("path") and all(value != '' for value in observation['path']) and maximum >= 0:
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

    # Only direct evidence from the official price/currency implementation may
    # place an entry in the diamond activity section.  Names such as "特惠" are
    # deliberately ignored because they are not a currency contract.
    standard_costs = [item.get("cost", "")]
    standard_costs.extend(option.get("cost", "") for option in item.get("priceOptions", [])
                          if isinstance(option, dict))
    item["exchangeKind"] = "diamond" if any(
        isinstance(cost, str) and cost.startswith("8:2:") for cost in standard_costs) else "activity"


def constructed_reward_tables(scripts: dict[str, str], tables: dict, constants: dict) -> dict:
    """Normalize the official typed reward wrapper without executing its AS.

    The Sep-30 sale moved literal rows behind a constructor. Follow only the
    explicit table, parameter, race and diamond-cost mappings in that wrapper;
    never infer a currency or pet from a display name.
    """
    tables = copy.deepcopy(tables)
    for filename, text in scripts.items():
        wrapper = Path(filename).stem
        reward = re.search(r'this\.(\w+)\.indexOf\(":"\)\s*>=\s*0\s*\?\s*"Material,"\s*\+\s*this\.\1\s*:\s*"CommonEnhancePrize,,,"\s*\+\s*this\.\1\s*\+\s*","\s*\+\s*(\w+)\.(\w+)', text)
        if not reward:
            continue
        parameter = re.search(r'this\.' + re.escape(reward[1]) + r'\s*=\s*\w+\["(\w+)"\]', text)
        race = constants.get(reward[2] + "." + reward[3])
        if not parameter or type(race) is not int or race <= 0:
            continue
        required_mappings = ("index", "cost", "limit", "serverIdForTimes")
        if any(not re.search(r'this\._' + name + r'\s*=\s*\w+\["' + name + r'"\]', text) for name in required_mappings):
            continue
        diamond = False
        for consumer in scripts.values():
            for variable in re.findall(r'\bvar\s+(\w+)\s*:\s*' + re.escape(wrapper) + r'\b', consumer):
                diamond |= bool(re.search(r'enableConsume\(AQGDialog\.CT_DIAMOND,\s*' + re.escape(variable) + r'\.cost\)', consumer))
        if not diamond:
            continue
        for key, (source_file, rows) in list(tables.items()):
            if not key.startswith(reward[2] + ".") or not isinstance(rows, list):
                continue
            if not re.search(r'for each\(\w+\s+in\s+' + re.escape(key) + r'\)', text):
                continue
            normalized = []
            for row in rows:
                if not isinstance(row, dict):
                    continue
                param, cost, quota = row.get(parameter[1]), row.get("cost"), row.get("limit")
                identity = row.get("serverIdForBuying", row.get("serverIdForTimes"))
                if not isinstance(param, str) or type(cost) is not int or cost <= 0 or type(identity) is not int or identity < 0:
                    continue
                normalized.append({**row, "serverId": identity,
                    "simpleParams": "Material," + param if ":" in param else f"CommonEnhancePrize,,,{param},{race}",
                    "cost": f"8:2:{cost}", "limit": f"4:{quota}" if type(quota) is int and quota > 0 else ""})
            if normalized:
                tables[key] = source_file, normalized
    return tables


def parse_module(scripts: dict[str, str], module: str, activity: dict, expand=None, *, include_manual=False, selectable_packages=(), reward_support=None) -> tuple[list, list, bool]:
    tables, constants = parse_tables(scripts)
    tables = constructed_reward_tables(scripts, tables, constants)
    tables, structural_issues = normalize_tables(scripts, tables, constants, reward_support)
    contracts = table_contracts(scripts, tables, constants)
    table_ends = table_deadlines(scripts, tables, constants)
    # A dynamically constructed shop can still contain complete literal bonus
    # objects. Resolve only verified selectable packages from those objects;
    # the surrounding runtime prices and claim IDs remain unknown.
    if include_manual and selectable_packages:
        for filename, text in scripts.items():
            for match in re.finditer(r'\bstatic\s+(?:const|var)\s+(\w+)\s*:\s*[\w.<>]+\s*=\s*', text):
                table = Path(filename).stem + "." + match[1]
                if (table in tables and not symbolic(tables[table][1])) or text[match.end():match.end()+1] not in ("[", "{"):
                    continue
                try:
                    expression, _ = _balanced(text, match.end())
                except ValueError:
                    continue
                rows = []
                for candidate in re.finditer(r'\{\s*"', expression):
                    try:
                        literal, _ = _balanced(expression, candidate.start())
                        row = LiteralReader(literal).parse()
                        bonus = row.get("bonus")
                        if not isinstance(bonus,str) or not re.fullmatch(r'[1-9]\d*:[1-9]\d*:[1-9]\d*(?:#[1-9]\d*:[1-9]\d*:[1-9]\d*)*', bonus):
                            continue
                        reward = "Material," + bonus
                        if not selectable_package_ids(reward,selectable_packages) or not (row.get("name") or row.get("desc")):
                            continue
                        rows.append({**row,"simpleParams":reward,"_rewardField":"bonus","_acquisitionUnknown":True})
                    except (ValueError,TypeError,IndexError):
                        continue
                if rows:
                    tables[table + ".staticPackageRewards"] = (filename,rows)
    pending = [{**issue, 'module': module, 'activityName': activity.get('activityName', '')}
               for issue in structural_issues]
    shops = []
    all_text = "\n".join(scripts.values())
    # Respect a module-wide entry gate only when its getter and caller both
    # provide evidence. A date constant elsewhere could gate an unrelated tab.
    opening_gates = set()
    for filename, text in scripts.items():
        cls = Path(filename).stem
        for match in re.finditer(r'static\s+function\s+(\w+)\(\s*\)\s*:\s*Boolean\s*\{\s*return\s+DateUtil\.isAfterTimeWithDelayClose\(\s*([\w.]+)\s*\)\s*;\s*\}', text):
            if not re.search(r'if\s*\(\s*!\s*' + re.escape(cls + '.' + match[1]) + r'\(\s*\)\s*\)', all_text):
                continue
            date = constants.get(match[2], constants.get(cls + '.' + match[2]))
            if isinstance(date, str) and official_date(date):
                opening_gates.add(date)
    module_start = next(iter(opening_gates)) if len(opening_gates) == 1 else ''
    if module_start and ' ' not in module_start:
        module_start += ' 02:00:00'
    entry_start = entry_opening(scripts, activity, constants)
    if entry_start:
        from activity_reward_structures import period_point
        module_start = max(filter(None, (module_start, entry_start)), key=period_point)
    module_end = module_deadline(scripts, constants)
    commands = {match[1]: match[2] for match in re.finditer(r'\b(?:const|var)\s+(\w+)\s*:\s*String\s*=\s*"(\d+(?:_[A-Za-z0-9]+)+)"', all_text)}
    direct_links = {value for key, value in constants.items()
                    if key.rsplit(".", 1)[-1].replace("_", "").lower() == "reshowselfactkey" and isinstance(value, str) and
                    re.fullmatch(r"btnNewAct_[A-Za-z0-9]+_[A-Za-z0-9_]+", value)}
    direct_navigation = next(iter(direct_links)) if len(direct_links) == 1 else activity.get("navigationLink", "")
    navigation_source = "module" if len(direct_links) == 1 else "activity" if direct_navigation else ""
    has_evolution = False
    for filename, text in scripts.items():
        for match in STATIC_DECLARATION.finditer(text):
            key = Path(filename).stem + "." + match[1]
            if key in tables:
                continue
            end = text.find(";", match.end())
            expression = text[match.end():end if end >= 0 else len(text)]
            if re.search(r'(?:CommonEnhancePrize|Choice|SelectPrizes),|"rewardType"\s*:\s*"(?:Strengthen|NChoose1|ArbitraryChoice)"|"rewardTypes"', expression):
                pending.append({"module": module, "activityName": activity.get("activityName", ""),
                                "table": key, "reason": "养成或任选奖励表包含尚未支持的动态表达式"})
    for table, (filename, value) in tables.items():
        cls = table.split(".", 1)[0]
        for branch, rows, tier in reward_branches(value, scripts[filename]):
            shop_id = next((constants[cls + "." + name] for name in ("SHOP_ID", "ExchangeShopId", "ShopId")
                            if cls + "." + name in constants), 1)
            if type(shop_id) is not int or shop_id <= 0:
                shop_id = 1
            profile = trade_profile(scripts, constants, tables, cls, rows, shop_id)
            goods = []
            for row in rows:
                simple = row.get("simpleParams", "")
                strengthen = row.get("type") == "Strengthen"
                automatic = isinstance(simple, str) and "CommonEnhancePrize," in simple or strengthen
                if not automatic and not (include_manual and (selectable_reward(simple, selectable_packages) or targeted_reward(simple))):
                    continue
                kind = acquisition_kind(row, table, activity, commands)
                if kind != "exchange" and not include_manual:
                    continue
                identity = next((row[key] for key in ("serverId", "serverIndex", "bi", "dataIndex", "id", "index") if type(row.get(key)) is int), None)
                try:
                    synthetic = identity is None or identity < 0 or bool(row.get("_rewardField"))
                    if synthetic and include_manual:
                        # Display identity only. Never used for server counters or claims.
                        identity_basis = row
                        if row.get('_structure'):
                            identity_basis = {key: row[key] for key in ('_rewardField', 'serverId', 'serverIndex', 'bi', 'dataIndex', 'id', 'index', 'idN', 'idE', 'day', 'days', 'level', 'campaignId', 'needToken', 'needChargeMonth', 'openDiamond') if key in row}
                            identity_basis['reward'] = simple
                        identity = int(hashlib.sha256(json.dumps(identity_basis, sort_keys=True, ensure_ascii=False).encode()).hexdigest()[:7], 16)
                    if identity is None or identity < 0 or identity > 2**31 - 1:
                        raise ValueError("奖励条目缺少明确的本地编号")
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
                    elif row.get('_typedCodes'):
                        codes, races = row['_typedCodes'], positive_ids([int(v) for v in simple.split(',')[4].split('#')])
                    elif automatic:
                        has_evolution |= "@sb" in simple
                        codes, races = enhancement(simple, expand)
                    else:
                        codes, races = "", []
                    raw_cost = {key: row[key] for key in ("cost", "costD", "daibi", "prices", "price", "discount", "orgPrice", "coin", "vipCoin") if key in row}
                    cost = row.get("cost", "")
                    cost_known = isinstance(cost, str) and bool(re.fullmatch(r"[1-9]\d*:[0-9]+:[1-9]\d*(?:[:][0-9]+)?(?:#[1-9]\d*:[0-9]+:[1-9]\d*(?:[:][0-9]+)?)*", cost))
                    cost_known = cost_known and not conditional and kind == "exchange" and tier is None
                    # For event currencies, retain their literal quantities
                    # and official labels without inventing an item ID.
                    cost_description = ""
                    if type(row.get("daibi")) is int:
                        label = constants.get(cls + ".DaibiName")
                        if isinstance(label, str):
                            cost_description = str(row["daibi"]) + " " + label
                    if kind != "exchange":
                        cost_description = {"signin": "签到领取；签到天数与领取状态以活动内为准",
                                            "progress": "达成进度后领取；门槛以活动内为准",
                                            "lottery": "抽中后领取或选择；概率、保底与参与条件以活动内为准",
                                            "reward": "活动奖励；领取方式与条件请在活动内查看",
                                            "unknown": "领取或兑换方式尚待确认；请在活动内查看"}[kind]
                    raw_start = row.get("shelfTime") or row.get('openTime') or module_start or activity.get("startTime", "")
                    raw_end = row.get("removalTime") or row.get('endTime') or activity.get("endTime", "")
                    start = official_date(raw_start) if isinstance(raw_start, str) and raw_start else ""
                    end = official_date(raw_end) if isinstance(raw_end, str) and raw_end else ""
                    description = row.get("basicDescription") or row.get("desc") or row.get("name") or ("指定精灵养成" if automatic or targeted_reward(simple) else "任选奖励")
                    if not isinstance(description, str):
                        description = '指定精灵养成' if automatic or targeted_reward(simple) else '任选奖励'
                    description = upgrade_cultivation_description(description, simple, codes, reward_support)
                    if row.get("_rewardField"):
                        position = row.get("index")
                        if type(position) is int and position >= 0:
                            description += f" · 第 {position} 项"
                        cost_description += "；奖励所属档位、解锁条件和领取状态请在活动内确认"
                    item = {"id": row.get("id", identity), "itemServerId": identity,
                            "description": description,
                            "manualSelectionRequired": not automatic or kind != "exchange" or tier is not None or synthetic or bool(row.get('_manualReward')),
                            "acquisitionKind": kind, "displayIdentityOnly": synthetic or tier is not None or bool(row.get('_structure')),
                            "tab": row.get("tab", row.get("tabId", 0)), "cost": cost if cost_known else "", "costKnown": cost_known,
                            "costRaw": raw_cost, "costDescription": cost_description, "enhanceType": codes, "raceIds": races,
                             "shelfTime": start, "removalTime": end, "availableKnown": bool(start),
                             "officialShelfTime": raw_start, "officialRemovalTime": raw_end,
                            "limit": str(row.get("limit", "")), "limitIndex": -1, "limitCount": -1,
                            "limitKey": "", "limitLabel": "", "quotaKnown": False,
                            "quotaRaw": {key: row[key] for key in ("limit", "maxNum", "limitType", "specBi", "baseOnId", "unlock", "ypFlag", "lvFlag", "lv", "buyId", "gainId", "addGainTimesNum") if key in row},
                            "unlock": ("关联条目 %s 的条件兑换，适用状态待确认" % row["baseOnId"] if conditional else row.get("unlock") or row.get("lvFlag", "")),
                            "conditionsRaw": {key: row[key] for key in ("index", "baseOnId", "specBi", "filter", "lvFlag", "ypFlag") if key in row},
                            "rewardRaw": simple or json.dumps(row, ensure_ascii=False, sort_keys=True),
                            "selectablePackageIds": selectable_package_ids(simple, selectable_packages),
                            "source": {"configClass": table, "configFile": filename, "commands": commands,
                                       "shelfTimeSource": "row" if row.get("shelfTime") or row.get('openTime') else "module" if module_start else "activity",
                                       "removalTimeSource": "row" if row.get("removalTime") or row.get('endTime') else "activity",
                                       "periodBounds": {'rowStart': row.get('shelfTime') or row.get('openTime') or '',
                                                        'rowEnd': row.get('removalTime') or row.get('endTime') or '',
                                                        'moduleStart': module_start, 'moduleEnd': module_end,
                                                        'tableEnd': table_ends.get(table, '')},
                                       "selection": row.get('_structure') or ("explicit-literal-filter" if strengthen else "CommonEnhancePrize")}}
                    apply_activity_period(item, activity)
                    if row.get('_structure'):
                        item['rewardStructure'] = row['_structure']
                        item['rewardOptions'] = row.get('_rewardOptions', [])
                        item['targetingRaw'] = row.get('_targeting')
                        item['officialRewardRaw'] = row.get('_rawReward', '')
                        # Shared ActReward counters/prices are not equivalent
                        # to the legacy shop protocol. Preserve literal facts,
                        # never bind guessed requests or cross-table counters.
                        if row.get('_targetScopeUnknown'):
                            item['unlock'] = '可选择精灵养成；具体适用精灵范围请在活动内确认'
                        if row.get('_choiceScopeExternal'):
                            item['unlock'] = '任选奖励；可选范围由活动公共配置提供，请在活动内查看'
                        for key, label in [('needToken', '所需进度'), ('needChargeMonth', '累计充值月数'), ('openDiamond', '累计钻石数量'), ('needUsedScore', '累计消耗积分'), ('day', '签到天数')]:
                            if type(row.get(key)) is int and row[key] > 0:
                                item['costDescription'] += ('；' if item['costDescription'] else '') + f'{label}：{row[key]}'
                    if kind == "exchange" and not synthetic and tier is None and not row.get('_structure'):
                        enrich_trade_item(item, row, profile)
                    else:
                        item["exchangeKind"] = "activity"
                    if tier is not None:
                        item["conditionsRaw"]["lv"] = tier
                        item["unlock"] = f"奖励档位 {tier}；你的适用档位和领取状态以活动内为准"
                        item["costDescription"] += ("；" if item["costDescription"] else "") + item["unlock"]
                    if kind != "exchange":
                        conditions = {key: value for key, value in row.items() if key not in ("simpleParams", "basicDescription", "desc", "name")}
                        item["claimConditionsRaw"] = conditions
                        # Keep literal evidence for diagnostics without exposing
                        # implementation fields as a player-facing condition.
                        day = row.get("day", row.get("days"))
                        if kind in ("signin", "progress") and str(day).isdigit() and 0 < int(day) <= 366:
                            item["costDescription"] += f"；活动天数：{int(day)} 天"
                    enrich_reward_facts(item, row, table, contracts, profile)
                    if any(previous["itemServerId"] == identity for previous in goods):
                        raise ValueError("同表兑换编号重复，不能合并不同条目")
                    goods.append(item)
                except (ValueError, TypeError) as error:
                    pending.append({"module": module, "activityName": activity.get("activityName", ""),
                                    "table": table + branch, "itemId": identity, "reason": str(error)})
            if goods:
                observation = {"schema": 1, "requests": profile["requests"] if any(g.get("acquisitionKind") == "exchange" and not g.get("displayIdentityOnly") for g in goods) else []}
                if profile.get("pendingRequest"):
                    observation["pendingRequest"] = profile["pendingRequest"]
                    observation["unavailableReason"] = "该官方读取需要客户端生成变更序号，静态配置可用，当前次数尚未读取"
                shops.append({"sourceKey": module + "#" + table + branch, "shopId": shop_id,
                              "name": "活动·" + activity.get("activityName", activity.get("name", "")) +
                                      (f" · 奖励档位 {tier}" if tier is not None else ""),
                              "rewardTier": tier,
                              "activityName": activity.get("activityName", ""), "goods": goods, "observation": observation,
                              "navigationLink": direct_navigation,
                              "activityEvidence": activity.get("activityEvidence", "referenced"),
                              "activityEvidenceDate": activity.get("activityEvidenceDate", ""),
                              "source": {"module": module, "activityAlias": activity.get("activityAlias", activity.get("name", "")),
                                         "class": table, "commands": commands,
                                         "navigationLinkSource": navigation_source,
                                         "quotaSupported": any("quotaObservation" in good for good in goods)}})
    if not shops and not pending and re.search(r'"!?(?:CommonEnhancePrize|Choice|SelectPrizes),[^"\n]*"\s*\+', all_text):
        # Overview pages use CommonEnhancePrize only to render icons and link
        # elsewhere. They have no claim rows. Follow their official links in
        # discovery; do not invent an unparsed-reward item for their posters.
        overview = any(isinstance(row, dict) and "jumpStr" in row and "mStrForActIcon" in row
                       for _, value in tables.values() if isinstance(value, list) for row in value)
        if not overview and not any("CommonEnhancePrize" in json.dumps(value, ensure_ascii=False) or
                                    "Choice," in json.dumps(value, ensure_ascii=False) or
                                    "SelectPrizes," in json.dumps(value, ensure_ascii=False)
                                    for _, value in tables.values()):
            pending.append({"module": module, "activityName": activity.get("activityName", ""),
                            "reason": "奖励由运行时代码组装，尚无可验证的静态奖励表"})
    # Preserve partial coverage as partial. An unresolved row next to a valid
    # row must never disappear merely because the module produced one shop.
    for table, (_, value) in tables.items():
        def unresolved_rewards(value):
            if isinstance(value, dict):
                if ('simpleParams' in value and symbolic(value['simpleParams']) or
                    value.get('rewardType') in ('Strengthen', 'NChoose1', 'ArbitraryChoice') and
                    'simpleParams' not in value):
                    return True
                return any(unresolved_rewards(v) for v in value.values())
            return isinstance(value, list) and any(unresolved_rewards(v) for v in value)
        if unresolved_rewards(value) and not any(p.get('table') == table for p in pending):
            pending.append({'module': module, 'activityName': activity.get('activityName', ''),
                            'table': table, 'reason': '相关奖励包含未解析的动态参数或选项'})
    if not shops and not pending and re.search(r'\.show(?:StrengthenPanel|PanelCombo)\(', all_text):
        pending.append({'module': module, 'activityName': activity.get('activityName', ''),
                        'reason': '发现精灵强化入口，奖励由公共服务或运行时配置提供，尚未提取完整奖励表'})
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
    for prior in relevant_shops(previous["shops"]):
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
    packages = getattr(updater, "selectable_package_source", None)
    if packages is None:
        packages = load_selectable_packages(updater, versions)
    reward_support = load_reward_support(updater, versions)
    config_version = versions.get(CONFIG_RESOURCE)
    if not re.fullmatch(r"\d{8,20}", config_version or ""):
        raise ValueError("官方活动目录资源版本缺失")
    discovery = previous_source.get("discovery", {})
    discovery_versions = {key: versions[key] for key in (CONFIG_RESOURCE, EMERGENCY_RESOURCE, BLOCK_RESOURCE) if key in versions}
    reward_dependencies = {key: versions[key] for key in ('library/interfaces', 'library/gameconst', 'library/common', 'library/util', 'strengthencombo/strengthencomboservice') if key in versions}
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
                    not reward_support.get('error') and
                    panel_periods_reusable(previous, versions, activity) and
                    previous.get('rewardDependencies', {}) == reward_dependencies and
                    (not previous.get("hasSelectablePackage") or previous.get("packageVersions") == packages["versions"]) and
                    (not previous.get("hasEvolutionSelector") or previous.get("evolutionVersion") == evo_version))
        if reusable and previous.get("status") != "failed":
            record = copy.deepcopy(previous)
            # A HUD rename or explicit opening date can change while its
            # versioned exchange module stays byte-identical.
            for shop in record.get("shops", []):
                shop["activityName"] = activity.get("activityName", "")
                shop["name"] = "活动·" + shop["activityName"] + (f" · 奖励档位 {shop['rewardTier']}" if shop.get("rewardTier") else "")
                # A module-owned RESHOW_SELF_ACT_KEY remains valid while that
                # exact module version is reused. Activity-owned HUD/registry
                # routes follow the current discovery metadata instead.
                if shop.get("source", {}).get("navigationLinkSource") != "module":
                    shop["navigationLink"] = activity.get("navigationLink", "")
                    if isinstance(shop.get("source"), dict):
                        shop["source"]["navigationLinkSource"] = "activity" if shop["navigationLink"] else ""
                shop["activityEvidence"] = activity.get("activityEvidence", "referenced")
                shop["activityEvidenceDate"] = activity.get("activityEvidenceDate", "")
                if isinstance(shop.get("source"), dict):
                    shop["source"]["activityAlias"] = activity.get("activityAlias", activity.get("name", ""))
                for good in shop.get("goods", []):
                    if 'periodBounds' in good.get('source', {}):
                        apply_activity_period(good, activity)
                        if good.get('catalogStale'):
                            good['availableKnown'] = False
                        continue
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
                timing, panels = {}, []
                if reward_hints(strings) or period_hints(strings):
                    scripts = export_scripts(updater, path, updater.scratch / ("activity-export-" + hashlib.sha256(module.encode()).hexdigest()[:20]))
                    timing = period_metadata(scripts, activity)
                    references.update(tab['targetAlias'] for tab in timing['tabs'])
                if reward_hints(strings):
                    def expand(seeds):
                        from activity_evolution_selector import expand_selector
                        return expand_selector(updater, versions, seeds)
                    module_shops, unsupported, has_evolution = parse_module(scripts, module, activity, expand, include_manual=True,
                                                                          selectable_packages=packages["packages"], reward_support=reward_support)
                    module_shops = relevant_shops(preserve_pending_rows(previous, module_shops, unsupported))
                if module_shops or timing.get('tabs'):
                    panels = load_panel_periods(updater, versions, scripts, module, activity, previous)
                    for panel in panels:
                        if panel.get('error'):
                            updater.activity_exchange_failures.append(activity.get('activityName', alias) + '：有效期面板读取失败，保留已确认的截止时间：' + panel['error'])
                record = {"versionToken": token, "version": version, "source": resource,
                          "status": "partial" if module_shops and unsupported else "supported" if module_shops else "pending" if unsupported else "no-relevant-rewards",
                          "diagnostic": ("已解析部分相关奖励，仍有未支持结构，原因见 pending" if module_shops and unsupported else
                                         "已解析指定精灵养成奖励，包含明确的养成或材料分支" if module_shops else
                                         "发现相关奖励但尚未完整解析，原因见 pending" if unsupported else
                                         "未发现非自选的指定精灵养成奖励；自选、普通奖励和仅导航/宣传的页面不列入手动添加"),
                          "shops": module_shops, "pending": unsupported, "references": sorted(references),
                          'periodMetadata': timing, 'panelPeriods': panels,
                          "hasSelectablePackage": any(re.search(r'(?:^|[,#])139:', value) for value in strings),
                          "packageVersions": packages["versions"],
                          "rewardDependencies": reward_dependencies,
                          "hasEvolutionSelector": has_evolution, "evolutionVersion": evo_version}
            except Exception as error:
                record = {**previous, "status": "failed", "error": str(error)}
                updater.activity_exchange_failures.append(activity.get("activityName", alias) + "：" + str(error))
        record["shops"] = relevant_shops(record.get("shops", []))
        record['activity'] = activity
        if excluded_activity(module):
            record['pending'] = []
            record['status'] = 'excluded'
            record['diagnostic'] = '此活动已按用户要求移除'
        modules[module] = record
        shops.extend(record.get("shops", []))
        pending.extend(record.get("pending", []))
        # Traverse the complete reachable graph. visited and MAX_MODULES bound
        # cycles/work; a new event behind four tabs must not silently vanish.
        queue.extend((name, depth + 1) for name in record.get("references", []))
    apply_graph_periods(modules, discovery)
    result = {"schema": 1, "parserVersion": PARSER_VERSION,
              "selection": "designated-pet cultivation, including a verified cultivation-or-material alternative; exclude pet/material choices and selectable packages",
              "source": {"kind": "aoqi-official-activity-registry", "configVersion": config_version,
                         "configResource": config_source, "discoveryVersions": discovery_versions,
                         "selectablePackages": packages,
                         "rewardSupport": reward_support,
                         "overrideResources": override_resources, "loaderDefault": default, "discovery": discovery},
              "shops": sorted(shops, key=lambda shop: shop["sourceKey"]), "pending": pending,
              "modules": modules, "coverage": {"activityModules": len(modules), "shops": len(shops),
                          "goods": sum(len(shop["goods"]) for shop in shops), "pending": len(pending),
                          "partialModules": sum(record.get('status') == 'partial' for record in modules.values()),
                          "pendingModules": sum(record.get('status') == 'pending' for record in modules.values()),
                          "withoutRelevantRewards": sum(record.get("status") == "no-relevant-rewards" for record in modules.values())}}
    changed = result != current
    if result != on_disk:
        _atomic_json(cache_path, result)
    updater.activity_exchange_pending = pending
    activity_goods = sum(1 for shop in shops for good in shop.get("goods", []) if good.get("exchangeKind") != "diamond")
    diamond_goods = sum(1 for shop in shops for good in shop.get("goods", []) if good.get("exchangeKind") == "diamond")
    emit(f"活动兑换：活动商店 {activity_goods} 项、钻石兑换 {diamond_goods} 项；本次检查 {checked} 个变更模块" +
         (f"；{len(pending)} 项规则待适配" if pending else ""))
    return changed
