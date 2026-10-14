"""Declare the union of public resources needed by the workbench and replay.

This module does not download, decompress, execute game code, or write files.
Flash resource identities are the official version-map keys, with formats kept
separately (SWF and FairyGUI FUI must not overwrite each other's requirements).
H5 resource identities include their suffix. Recorded catalogs supply evidence
of earlier references only; they never establish the current version or hash.
The resource session owns current version resolution and all download checks.
"""
from __future__ import annotations

import copy
import json
import re
from urllib.parse import urlsplit


_KEY = re.compile(r"[A-Za-z0-9_./@-]+\Z")
_FLASH_URL = re.compile(r"/play/([A-Za-z0-9_/@-]+)~([0-9]{1,20})\.(swf|fui)\Z")
_H5_URL = re.compile(r"/h5/([A-Za-z0-9_./@-]+)~([0-9]{1,20})\.(aqz|json|js)\Z")
_H5_SUFFIXES = (".aqz", ".json", ".js")
_EVIDENCE_FIELDS = ("version", "sha256", "bytes", "retrievedUtc", "verifiedUtc", "manifestSha256")


def _valid_resource(kind, resource):
    if kind not in ("flash", "h5") or not isinstance(resource, str) or len(resource) > 512:
        return False
    if not _KEY.fullmatch(resource) or resource.startswith("/") or any(
            part in ("", ".", "..") for part in resource.split("/")):
        return False
    return not resource.endswith((".swf", ".fui")) if kind == "flash" else resource.endswith(_H5_SUFFIXES)


def _declaration(kind, resource, components, reason, *, formats=None, version_only=False):
    return {"kind": kind, "resource": resource, "components": list(components),
            "reason": reason, "formats": list(formats or (["swf"] if kind == "flash" else [resource.rsplit(".", 1)[-1]])),
            "versionOnly": version_only, "sourceStatus": "declared-dependency"}


def workbench_dependencies() -> list[dict]:
    """Existing updater requirements, including version-only invalidators.

    Dynamic activity SWFs, referenced FUI panels and actual star/pet icon IDs
    are registered from their recorded identities or by discovery. No wildcard
    resource, guessed URL, or complete-game download is invented here.
    """
    import public_data_updater as public
    import public_names_updater as names
    import public_skill_updater as skills
    import public_icon_updater as icons
    import public_routine_updater as routines
    import public_activity_exchange_updater as activities
    import activity_evolution_selector as evolution
    import public_cultivation_supplement as cultivation_supplement

    rows = []

    def flash(resource, component, reason, **options):
        rows.append(_declaration("flash", resource, [component], reason, **options))

    for label, resource, _ in public.PET_RESOURCES:
        flash(resource, "pets", "精灵基础与增量字典：" + label)
    for label, resource in (*public.POWER_RESOURCES, *public.CULTIVATION_RESOURCES, *names.NAME_RESOURCES):
        flash(resource, "pets", "养成与名称字典：" + label)
    flash(names.SACRED_SOURCE_RESOURCE[1], "pets", "源兽名称与对应源兽定义")
    for _, resource, table, _ in cultivation_supplement.SUPPLEMENT_RESOURCES:
        flash(resource, "pets", "完整养成名称资料：" + table)
    flash(public.RESOURCE, "shop", "指定精灵兑换商店配置")
    for resource, _ in activities.PACKAGE_RESOURCES:
        flash(resource, "shop", "礼包内容与自选/随机分类")
    for resource in (activities.CONFIG_RESOURCE, activities.EMERGENCY_RESOURCE, activities.BLOCK_RESOURCE):
        flash(resource, "shop", "官方活动注册表、入口及覆盖规则")
    for resource in ("library/interfaces", "strengthencombo/strengthencomboservice"):
        flash(resource, "shop", "公共养成奖励类型、价格与说明")
    for resource in ("library/interfaces", "library/gameconst", "library/common", "library/util",
                     "strengthencombo/strengthencomboservice"):
        flash(resource, "shop", "活动奖励解析缓存的官方版本依赖", version_only=True)
    for label, resource, _ in evolution.RESOURCES:
        flash(resource, "shop", "活动进化适用范围：" + label)
    for label, resource, _ in routines.RESOURCES:
        flash(resource, "routines", "日常任务、活动入口与红点关系：" + label)
    flash("pet/petdataservice", "images", "精灵图片原始外观规则")
    flash("loader", "images", "未单独列版精灵图片的官方缺省版本依据")
    flash("loader", "icons", "未单独列版星神图标的官方缺省版本依据")
    flash("loader", "shop", "未单独列版活动模块的官方缺省版本依据")
    for resource in (icons.ATTRIBUTE_RESOURCE, icons.ATTRIBUTE_RESOURCE + "@res"):
        flash(resource, "icons", "属性图标 FairyGUI 目录与图片资源", formats=["fui"])
    for resource in (*skills.ARCHIVES, *skills.DIRECT):
        rows.append(_declaration("h5", resource, ["skills"], "工作台官方技能与关联配置"))
    return build_plan(rows, ())


def build_plan(workbench, replay, historical=()) -> list[dict]:
    """Return a deterministic union, preserving every consuming component.

    Merely declaring a dependency never marks it downloaded or current. Rows in
    ``historical`` retain their evidence but are explicitly unverified for this
    run; a current resource session must resolve/download them independently.
    ``versionOnly`` is true only if every recorded use needs the version alone.
    """
    union = {}
    for values, historical_input in ((workbench, False), (replay, False), (historical, True)):
        for value in values:
            if not isinstance(value, dict) or not _valid_resource(value.get("kind"), value.get("resource")):
                raise ValueError("Invalid public resource identity")
            components = value.get("components", [])
            if not isinstance(components, (list, tuple)) or any(not isinstance(item, str) or not item for item in components):
                raise ValueError("Invalid public resource components")
            reasons = value.get("reasons", [])
            if not isinstance(reasons, (list, tuple)) or any(not isinstance(item, str) or not item for item in reasons):
                raise ValueError("Invalid public resource reasons")
            reasons = list(reasons)
            if value.get("reason"):
                if not isinstance(value["reason"], str):
                    raise ValueError("Invalid public resource reason")
                if value["reason"] != "；".join(sorted(set(reasons))):
                    reasons.append(value["reason"])
            kind, resource = value["kind"], value["resource"]
            formats = value.get("formats", ["swf"] if kind == "flash" else [resource.rsplit(".", 1)[-1]])
            allowed = {"swf", "fui"} if kind == "flash" else {"aqz", "json", "js"}
            if not isinstance(formats, (list, tuple)) or not formats or any(item not in allowed for item in formats):
                raise ValueError("Invalid public resource formats")
            row = union.setdefault((kind, resource), {"kind": kind, "resource": resource,
                "components": set(), "reasons": set(), "formats": set(), "versionOnly": True,
                "sourceStatuses": set(), "evidence": {}})
            row["components"].update(components)
            row["reasons"].update(reasons)
            row["formats"].update(formats)
            row["versionOnly"] = row["versionOnly"] and value.get("versionOnly") is True
            statuses = value.get("sourceStatuses", [value.get("sourceStatus", "declared-dependency")])
            if not isinstance(statuses, (list, tuple)) or any(not isinstance(item, str) or not item for item in statuses):
                raise ValueError("Invalid public resource source status")
            row["sourceStatuses"].update(["historical-evidence"] if historical_input else statuses)
            evidence = value.get("evidence", [])
            if isinstance(evidence, dict):
                evidence = [evidence]
            if not isinstance(evidence, (list, tuple)) or any(not isinstance(item, dict) for item in evidence):
                raise ValueError("Invalid public resource evidence")
            for record in evidence:
                item = copy.deepcopy(record)
                if historical_input:
                    item["status"] = "historical-evidence"
                else:
                    item.setdefault("status", "recorded-not-revalidated")
                row["evidence"][json.dumps(item, sort_keys=True, ensure_ascii=False, allow_nan=False)] = item
    result = []
    for key in sorted(union):
        row = union[key]
        for field in ("components", "reasons", "formats", "sourceStatuses"):
            row[field] = sorted(row[field])
        row["reason"] = "；".join(row["reasons"])
        row["evidence"] = [row["evidence"][item] for item in sorted(row["evidence"])]
        result.append(row)
    return result


def _url_identity(value):
    if not isinstance(value, str):
        return None
    try:
        url = urlsplit(value)
    except ValueError:
        return None
    if url.scheme != "https" or url.netloc != "aoqi.100bt.com" or url.fragment:
        return None
    match = _FLASH_URL.fullmatch(url.path)
    if match:
        return "flash", match[1], match[3], match[2]
    match = _H5_URL.fullmatch(url.path)
    if match:
        return "h5", match[1] + "." + match[3], match[3], match[2]
    return None


def _explicit_identity(value):
    if not isinstance(value, str) or "/" not in value:
        return None
    if value.startswith(("config/", "js/")) and value.endswith(_H5_SUFFIXES) and _valid_resource("h5", value):
        return "h5", value, value.rsplit(".", 1)[-1], None
    for suffix in (".swf", ".fui"):
        if value.endswith(suffix) and _valid_resource("flash", value[:-len(suffix)]):
            return "flash", value[:-len(suffix)], suffix[1:], None
    if "." not in value and _valid_resource("flash", value):
        return "flash", value, "swf", None
    return None


def recorded_dependencies(catalog, component) -> list[dict]:
    """Read only explicit recorded URLs/identities; never reconstruct a URL.

    Dynamic module map keys, version/resource maps and panel-period ``key``
    fields are recognized. Plain prose, local ``file`` paths, ID/name matches,
    and unrelated HTTPS URLs do not produce dependencies. Original hashes are
    evidence, never an approval to reuse those bytes for a current version.
    """
    if not isinstance(component, str) or not component:
        raise ValueError("A component is required for recorded dependencies")
    result, remaining = [], 2_000_000

    def add(identity, value, context, url=None):
        if identity is None or not _valid_resource(identity[0], identity[1]):
            return
        kind, resource, extension, version = identity
        proof = {key: copy.deepcopy(value[key]) for key in _EVIDENCE_FIELDS if key in value and
                 isinstance(value[key], (str, int, float, bool, type(None)))}
        proof.update(location=context, status="recorded-not-revalidated")
        if version:
            proof["version"] = version
        if url:
            proof["url"] = url
        row = _declaration(kind, resource, [component], "已有资料明确记录的官方依赖", formats=[extension])
        row.update(sourceStatus="recorded-not-revalidated", evidence=[proof])
        result.append(row)

    def visit(value, context, depth=0, identity_key=None):
        nonlocal remaining
        remaining -= 1
        if remaining < 0 or depth > 64:
            raise ValueError("Recorded resource graph exceeds the traversal limit")
        if isinstance(value, dict):
            urls = [(key, _url_identity(value.get(key))) for key in ("url", "swfUrl")]
            found_url = False
            for key, identity in urls:
                if identity:
                    add(identity, value, context + "/" + key, value[key])
                    found_url = True
            if not found_url:
                for key in ("resource", "module", "key"):
                    identity = _explicit_identity(value.get(key))
                    if identity and key == "key" and "/panelPeriods/" in context and identity[0] == "flash":
                        identity = (*identity[:2], "fui", identity[3])
                    add(identity, value, context + "/" + key)
                add(_explicit_identity(identity_key), value, context)
            for key, item in value.items():
                if key in ("resources", "modules", "versions") and isinstance(item, dict):
                    for resource, record in item.items():
                        if isinstance(record, (dict, list)):
                            visit(record, context + "/" + key + "/" + resource, depth + 1, resource)
                        else:
                            add(_explicit_identity(resource), {"version": record}, context + "/" + key + "/" + resource)
                else:
                    visit(item, context + "/" + str(key), depth + 1)
        elif isinstance(value, list):
            for index, item in enumerate(value):
                visit(item, context + "/" + str(index), depth + 1)

    visit(catalog, "")
    return build_plan(result, ())
