"""Official display dictionaries; called only by an explicit public update."""
from __future__ import annotations

import json
import re

from generate_pet_detail_data import iter_call_args, parse_money, strict_int, swf_text

NAME_RULE_VERSION = 1
NAME_RESOURCES = (("displayInterfaces", "library/interfaces"),
                  ("moneyData", "library/materialdata"),
                  ("moneyDataUpdate", "library/materialdataupdate"))
SACRED_SOURCE_RESOURCE = ("sourceEquipment", "equipment4petver2/equipment4petver2service",
                          "mmo.equipment4petver2.config.E4PV2_EInfos")


def equipment_source_names(source: str) -> dict[str, str]:
    result = {}
    for args in iter_call_args(source, "new E4PV2_EInfo("):
        if len(args) < 2:
            raise ValueError("官方源兽名称补充表结构改变")
        identifier = str(strict_int(args[0], "equipment source id", 1))
        name = json.loads(args[1])
        if not isinstance(name, str) or not name or identifier in result:
            raise ValueError("官方源兽名称补充表无效")
        result[identifier] = name
    if not result:
        raise ValueError("未找到官方源兽名称补充表")
    return result


def enum_names(source: str, class_name: str) -> dict[str, str]:
    names = {}
    pattern = rf"const\s+\w+\s*:\s*{re.escape(class_name)}\s*=\s*enum__\("
    for match in re.finditer(pattern, source):
        args = next(iter_call_args(source[match.start():], "enum__("), [])
        if len(args) < 2:
            raise ValueError("官方属性或职业名称结构已改变")
        identifier = str(strict_int(args[0], "display dictionary id", 1))
        name = json.loads(args[1])
        if class_name == "PetJob" and len(args) > 3 and args[3] != "null":
            short_name = json.loads(args[3])
            if not isinstance(short_name, str):
                raise ValueError("官方职业简称无效")
            name = short_name or name
        if not isinstance(name, str) or not name.strip() or identifier in names:
            raise ValueError("官方属性或职业名称缺失或重复")
        names[identifier] = name
    if not names:
        raise ValueError("未找到官方属性或职业名称")
    return names


def fusion_jobs(source: str, jobs: dict[str, str]) -> list[dict]:
    marker = re.search(r"\bFUSION_JOBS\s*:\s*Array\s*=\s*", source)
    if not marker:
        raise ValueError("官方组合职业定义缺失")
    value, _ = json.JSONDecoder().raw_decode(source[marker.end():])
    if not isinstance(value, list):
        raise ValueError("官方组合职业定义无效")
    for item in value:
        if (not isinstance(item, dict) or not isinstance(item.get("name"), str) or not item["name"] or
                not isinstance(item.get("jobs"), list) or len(item["jobs"]) < 2):
            raise ValueError("官方组合职业字段无效")
        for group in item["jobs"]:
            if not isinstance(group, list) or not group or any(type(i) is not int or str(i) not in jobs for i in group):
                raise ValueError("官方组合职业引用未知职业")
    return value


def names_valid(catalog: dict) -> bool:
    if catalog.get("nameRuleVersion") != NAME_RULE_VERSION:
        return False
    for section in ("attributes", "jobs"):
        values = catalog.get(section)
        if not isinstance(values, dict) or not values:
            return False
        if any(not key.isdecimal() or not isinstance(name, str) or not name for key, name in values.items()):
            return False
    money = catalog.get("money")
    return (isinstance(catalog.get("fusionJobs"), list) and isinstance(money, dict) and bool(money) and
            all(isinstance(value, dict) and isinstance(value.get("name"), str) and value["name"] for value in money.values()))


def needs_names_update(current: dict, versions: dict) -> bool:
    previous = current.get("source", {}).get("resources", {})
    return not names_valid(current) or any(not versions.get(key) or previous.get(label, {}).get("version") != versions[key]
                                          for label, key in NAME_RESOURCES)


def update_names(updater, current: dict, versions: dict, resources: dict) -> bool:
    """Stage new sheets in current; caller commits the complete pet catalog."""
    previous = current.get("source", {}).get("resources", {})
    upgrade = not names_valid(current)
    changed = False
    label, key = NAME_RESOURCES[0]
    if upgrade or previous.get(label, {}).get("version") != versions.get(key):
        swf, resources[label] = updater.resource(key, versions)
        attributes_path = updater.export(swf, updater.scratch / "display-attributes", "mmo.interfaces.pet.data.PetAttr")
        jobs_path = updater.export(swf, updater.scratch / "display-jobs", "mmo.interfaces.pet.data.PetJob")
        attributes = enum_names(attributes_path.read_text(encoding="utf-8-sig"), "PetAttr")
        jobs_source = jobs_path.read_text(encoding="utf-8-sig")
        jobs = enum_names(jobs_source, "PetJob")
        if len(attributes) < (len(current.get("attributes", {}))-1)*0.9 or len(jobs) < len(current.get("jobs", {}))*0.9:
            raise ValueError("属性或职业名称表不完整，保留旧目录")
        current["attributes"] = {"0": "全部", **attributes}
        current["jobs"] = jobs
        current["fusionJobs"] = fusion_jobs(jobs_source, jobs)
        changed = True
    if upgrade or any(previous.get(label, {}).get("version") != versions.get(key) for label, key in NAME_RESOURCES[1:]):
        money = {}
        for index, (label, key) in enumerate(NAME_RESOURCES[1:]):
            swf, resources[label] = updater.resource(key, versions)
            selected = "mmo.materialdata.MoneyData" if index == 0 else "mmo.materialdata.update.MoneyData_Update"
            # Some official update modules have no money sheet. The base
            # sheet remains authoritative; absence of an update is legitimate.
            if index and selected.rsplit(".", 1)[-1] not in swf_text(swf):
                continue
            path = updater.export(swf, updater.scratch / ("display-money-"+str(index)), selected)
            values = parse_money(path.parent)
            if not values:
                raise ValueError("官方货币名称表未能解析")
            money.update(values)
        if not money or len(money) < len(current.get("money", {}))*0.9:
            raise ValueError("官方货币名称表不完整，保留旧目录")
        current["money"] = money
        changed = True
    current["nameRuleVersion"] = NAME_RULE_VERSION
    if not names_valid(current):
        raise ValueError("属性、职业或货币名称校验未完成")
    return changed
