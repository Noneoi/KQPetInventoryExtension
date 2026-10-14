"""Refresh the three display dictionaries omitted by the old public updater.

Source ownership is confirmed by official package exports, not similar names:
* equipment4petver2/equipment4petver2service ->
  mmo.equipment4petver2.config.E4PV2_EInfos (E4PV2_EInfo constructors).
* legend/legendservice -> mmo.legend.other.LgsConfig.ITEM_DEFINES_SRC.
  The reviewed 2022081934756433 package SHA256 is
  e2695c5cf2be749819a26eb7625d7ea732f256ab24d731fa389b2fee2118292f.
* proficient/proficient -> the named DefineBinaryData payload below. Official
  mmo.proficient.data.ProficientManager.init reads proinfos.proinfo id/name/desc;
  its ByteArrayAsset embed identifies exactly this symbol. The reviewed
  2025041161536653 package SHA256 is
  8e2dde882c6ca3aa1647656b64ba2f52505f7d4ebb40fbea61634da0caaa392e.

Historical hashes document the inspected source only. The shared official
version session chooses current packages; literal/XML data is parsed without
executing client code. Existing generator projections are reused unchanged.
All three tables and source records are staged before mutating current, and
the caller atomically commits the entire pet component only after validation.
"""
from __future__ import annotations

import hashlib
from html import unescape
from pathlib import Path
import re
import xml.etree.ElementTree as ET

from generate_pet_detail_data import parse_source_beasts, parse_legend_stones, parse_proficiencies
from battle_replay.fetch_reference import binary_payloads
from public_resource_session import resource_matches


RULE_VERSION = 1
RULE_FIELD = "cultivationSupplementRuleVersion"
PROFICIENT_SYMBOL = "mmo.proficient.data.ProficientManager__xmlProficientInfoClass"
SUPPLEMENT_RESOURCES = (
    ("sourceBeastCatalog", "equipment4petver2/equipment4petver2service", "sourceBeasts",
     "mmo.equipment4petver2.config.E4PV2_EInfos"),
    ("legendStoneCatalog", "legend/legendservice", "legendStones", "mmo.legend.other.LgsConfig"),
    ("proficientCatalog", "proficient/proficient", "proficiencies", PROFICIENT_SYMBOL),
)
MINIMUM_ROWS = {"sourceBeasts": 20, "legendStones": 18, "proficiencies": 20}
MAX_SOURCE_BYTES = 32 * 1024 * 1024
_VERSION = re.compile(r"(?:2000|[0-9]{8,20})\Z")


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _read(path):
    path = Path(path)
    if path.stat().st_size > MAX_SOURCE_BYTES:
        raise ValueError("补充养成字典来源超过大小限制")
    with path.open("rb") as stream:
        raw = stream.read(MAX_SOURCE_BYTES + 1)
    if len(raw) > MAX_SOURCE_BYTES:
        raise ValueError("补充养成字典来源在读取时超过限制")
    return raw


def _valid_table(table, value):
    if not isinstance(value, dict) or len(value) < MINIMUM_ROWS[table]:
        return False
    for identifier, row in value.items():
        if (not isinstance(identifier, str) or not re.fullmatch(r"[1-9][0-9]*", identifier) or
                not isinstance(row, dict) or not isinstance(row.get("name"), str) or not row["name"]):
            return False
        if any(key in row and not isinstance(row[key], str) for key in ("attr", "desc")):
            return False
        if "levels" in row:
            if not isinstance(row["levels"], dict) or any(
                    not isinstance(level, str) or not re.fullmatch(r"[1-9][0-9]*", level) or
                    not isinstance(detail, dict) or not isinstance(detail.get("desc"), str)
                    for level, detail in row["levels"].items()):
                return False
    return True


def needs_supplement(current, versions) -> bool:
    """A rule-version bump repairs same-official-version historical catalogs."""
    if not isinstance(current, dict) or current.get(RULE_FIELD) != RULE_VERSION:
        return True
    source = current.get("source", {})
    resources = source.get("resources", {}) if isinstance(source, dict) else {}
    if not isinstance(resources, dict):
        return True
    for label, resource, table, _ in SUPPLEMENT_RESOURCES:
        version = versions.get(resource, "2000")
        prior = resources.get(label, {})
        if (not isinstance(version, str) or not _VERSION.fullmatch(version) or
                not isinstance(prior, dict) or not resource_matches(prior, resource, versions) or
                prior.get("resource") != resource or prior.get("parserRuleVersion") != RULE_VERSION or
                not _valid_table(table, current.get(table))):
            return True
    return False


def refresh_supplement(updater, current, versions, resources) -> None:
    """Read current official definitions; publish no partial dictionary set.

    Updater.resource and export provide this run's shared downloaded bytes and
    bounded decompiler. Temporary extraction remains below updater.scratch.
    No installed catalog file is read or replaced by this helper.
    """
    staged, proofs = {}, {}
    parsers = {"sourceBeasts": parse_source_beasts, "legendStones": parse_legend_stones}
    for label, resource, table, selected in SUPPLEMENT_RESOURCES:
        version = versions.get(resource, "2000")
        if not isinstance(version, str) or not _VERSION.fullmatch(version):
            raise ValueError("补充养成字典缺少有效官方版本：" + resource)
        package, source = updater.resource(resource, versions)
        payload = _read(package)
        if source.get("sha256") != _sha(payload) or source.get("version") != version:
            raise ValueError("补充养成字典来源校验失败：" + resource)
        proof = {**source, "resource": resource, "parserRuleVersion": RULE_VERSION}
        if table == "proficiencies":
            matches = [raw for symbol, raw in binary_payloads(payload) if symbol == PROFICIENT_SYMBOL]
            if len(matches) != 1:
                raise ValueError("官方潜能定义的指定内嵌数据缺失或不唯一")
            xml_bytes = matches[0]
            root = ET.fromstring(xml_bytes)
            rows = root.findall("./proinfos/proinfo")
            if root.tag != "config" or not rows:
                raise ValueError("官方潜能定义结构已变化，保留原养成资料")
            target = Path(updater.scratch) / "cultivation-supplement-proficiencies"
            target.mkdir(parents=True, exist_ok=True)
            binary = target / (PROFICIENT_SYMBOL + ".bin")
            binary.write_bytes(xml_bytes)
            result = parse_proficiencies(target)
            # The existing parser's compact fields must come from precisely
            # the same rows the inspected official manager reads.
            expected = {row.get("id"): {"name": row.get("name"), **({"desc": row.get("desc")} if row.get("desc") else {})}
                        for row in rows}
            # The historical compact parser retains attribute entity spelling.
            # Compare decoded values for proof without changing its projection.
            decoded = {identifier: {key: unescape(value) for key, value in item.items()}
                       for identifier, item in result.items()}
            if len(expected) != len(rows) or decoded != expected:
                raise ValueError("官方潜能定义行未完整解析，保留原养成资料")
            proof.update(symbol=PROFICIENT_SYMBOL, payloadSha256=_sha(xml_bytes), payloadBytes=len(xml_bytes))
        else:
            script = updater.export(package, Path(updater.scratch) / ("cultivation-supplement-" + table), selected)
            raw = _read(script)
            result = parsers[table](Path(script).parent)
            proof.update(decompiledClass=selected, decompiledSha256=_sha(raw))
        previous = current.get(table, {})
        previous_count = len(previous) if isinstance(previous, dict) else 0
        if not _valid_table(table, result) or len(result) < previous_count * .9:
            raise ValueError("补充养成字典不完整，保留原资料：" + table)
        staged[table] = result
        proofs[label] = dict(proof, definitions=len(result))
    current.update(staged)
    current[RULE_FIELD] = RULE_VERSION
    resources.update(proofs)
