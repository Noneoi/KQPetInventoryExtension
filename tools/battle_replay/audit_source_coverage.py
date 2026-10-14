"""Inventory local catalog inputs against explicit, reviewed parser contracts.

This tool neither downloads nor executes official AS/JS. A mapped contract is
not proof that a parser ran, or that a field's complete game meaning is known.
Hashes verify local bytes against local provenance only, never today's server.
Empty official descriptions remain valid observations, not parse failures.
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ET


SCHEMA_VERSION = 1
ROLES = (
    "auxiliary_source", "verified_source", "mechanism_rules", "legend_mechanics",
    "skin_source", "title_source", "formation_source", "race_expansions",
    "cultivation_source", "cultivation_h5_source", "cultivation_consumer_source",
    "reference_consumers",
)
HISTORICAL_ROLES = {"mechanism_rules", "legend_mechanics", "race_expansions", "reference_consumers"}
PARSER_DIR = Path(__file__).resolve().parent


def _attrs(selector, names):
    return tuple(selector + "/@" + name for name in names.split())


@dataclass(frozen=True)
class Contract:
    key: str
    roles: tuple[str, ...]
    patterns: tuple[str, ...]
    parser: str
    reader: str
    consumed: tuple[str, ...] = ()
    retained: bool = True
    format: str = "xml"
    pipe_rows: str = ""
    pipe_columns: tuple[str, ...] = ()
    map_paths: tuple[str, ...] = ()


def _xml(name, parser, reader, consumed, *, suffix="xmlClass", roles=("source",)):
    return Contract(name + "__" + suffix, roles, (f"xml/*.{name}__{suffix}.xml",),
                    parser, reader, tuple(consumed))


# Explicit selectors/fields from the named Python functions, not filename or
# Chinese-name heuristics. Copying arbitrary attributes counts as retention;
# it does not silently make a newly encountered field a consumed field.
CONTRACTS = [
    _xml("BufferConfigXML", "extract_buff_tables.py", "extract_buff_tables/_decode_h5",
         _attrs("/buff/s", "id nm did pos once df hd buffWaitType buffWaitEndFrame need2HidePlayerUi spine") +
         _attrs("/pfmap/m", "raceId buffId") + _attrs("/pfmap/def", "buffId")),
    _xml("BufferDescConfigXML", "extract_buff_tables.py", "extract_buff_tables",
         _attrs("/buff/s", "id nm mess beffect bhs")),
    _xml("BufferExecuteXML", "link_battle_tables.py", "link_battle_tables",
         _attrs("/s", "bufferId skillId lightedSkillId")),
    _xml("ComboBuffsConfigXML", "link_battle_tables.py", "link_battle_tables",
         _attrs("/combo/s", "skillId buffs targetSkillIds")),
    _xml("SkillConfigXML", "extract_skill_tables.py", "extract_skill_tables/runtime_define",
         tuple(field for row in ("/s", "/last") for field in
               _attrs(row, "id action pos jump effect shock black hideother last central playOrderType ef spine swf"))),
    _xml("SkillDescConfigXML", "extract_skill_tables.py", "extract_skill_tables/runtime_define",
         _attrs("/pet/s", "id attackType skillBaseType skillType bossSkill ft")),
    _xml("EntryConfigXML", "extract_entry_tables.py", "extract_entry_tables/_index_rows",
         _attrs("/pet/s", "n d t")),
    _xml("RaceTypeConfigXML", "extract_presentation_config.py", "extract_presentation_config",
         _attrs("/map/m", "id index") + _attrs("/restraint_coefficient/r", "nm value") +
         _attrs("/race_type/s", "id rcs")),
    _xml("SkillExecuteXML", "build_catalog.py", "build:execution", _attrs("//s", "code id")),
    _xml("HuanConfigXML", "extract_pet_mechanisms.py", "parse_huan_rows",
         _attrs("//s", "id name tip ttte vtte icon")),
    _xml("SummonConfigXML", "extract_replay_supplements.py", "parse_summons",
         _attrs("/pet/s", "s sed effect isShow team pt startTime des sdes")),
    _xml("SummonConfigXML", "extract_replay_supplements.py", "parse_summons",
         _attrs("/pet/s", "s sed effect isShow team pt startTime des sdes"), suffix="hideConfigXmlClass"),
    _xml("CarryConfigXML", "extract_replay_supplements.py", "parse_carries",
         _attrs("/pet/s", "c ced type bpr gimmick sdes")),
    _xml("AlmightyConfigXML", "extract_replay_supplements.py", "parse_almighty",
         _attrs("/pet/s", "id psci hn hd sn sd pn pd")),
]
for _name in ("BattleAvatarConfigXML", "BattleChangeAvatarConfigXML", "ActionMovieConfigXML",
              "PreDownloadConfigXML", "BattleBgConfigXML"):
    # The extractor retains xml_records in full; this audit deliberately does
    # not claim to have field-level consumption evidence for those groups.
    CONTRACTS.append(_xml(_name, "extract_replay_supplements.py", "parse_resources:rawAttributes", ()))

_BUFF_PIPE = tuple("nm mess did beffect bhs pos once df hd buffWaitType buffWaitEndFrame need2HidePlayerUi loc spineFlag".split())
CONTRACTS += [
    Contract("h5-buffer", ("source",), ("h5-tables/buffer_config.json",), "extract_buff_tables.py", "_decode_h5",
             ("/buff", "/buff/*") + tuple(f"/buff/*/|{i}" for i in range(14)), format="json",
             pipe_rows="/buff/*", pipe_columns=_BUFF_PIPE, map_paths=("/buff",)),
    Contract("h5-spine-buff", ("source",), ("h5-tables/spine_buffer_config.json",), "extract_buff_tables.py",
             "extract_buff_tables:spine_ids", ("/id", "/id/[]"), format="json"),
    Contract("h5-skill-description", ("source",), ("h5-tables/skill_desc_config.json", "skill_desc_config.json"),
             "extract_skill_tables.py", "decode_h5", ("/*", "/*/|0", "/*/|1", "/*/|2"), format="json",
             pipe_rows="/*", pipe_columns=("name", "description", "encodedMetadata"), map_paths=("/",)),
    Contract("h5-entry", ("source",), ("h5-tables/entry_config.json",), "extract_entry_tables.py", "_index_rows",
             ("/[]/n", "/[]/d", "/[]/t"), format="json"),
]
for _filename, _parser in (
    ("buffer_bg_config.json", "extract_buff_tables.py"),
    ("player_buffer_execute.json", "extract_buff_tables.py"),
    ("combo_buffs_config.json", "extract_buff_tables.py"),
    ("skill_config.json", "extract_skill_tables.py"),
    ("skill_embed_effect.json", "extract_skill_embed_effect.py"),
):
    _patterns = ("h5-tables/" + _filename,) + ((_filename,) if _filename == "skill_config.json" else ())
    CONTRACTS.append(Contract("h5-raw-" + _filename, ("source",), _patterns,
                              _parser, "rawTables:separate-H5-namespace", format="json",
                              map_paths=("/",) if _filename == "skill_config.json" else ()))
for _name in ("SkillConfigSimpleAC", "SkillConfigSimple3000", "SkillConfigSimple6000", "SkillConfigSimple9999"):
    CONTRACTS.append(Contract(_name, ("source",), ("battleconfig/scripts/mmo/battleconfig/data/direct/" + _name + ".as",),
                             "extract_skill_tables.py", "parse_map/text_for", ("/MAP/*/name", "/MAP/*/desc"), format="as-map"))

_CULTIVATION = (
    ("Sgsv3Config_sXMLClass", "defineId name pinYinChain supplyExp", "level exp battlePower battleProperty desc"),
    ("PetBadgeConfig_sXMLClass", "defineId name type effectDesc propsStr battlePower materialStrToActivate",
     "level name battlePower effectDesc propsStr materialStrToUpgrade"),
    ("AstrolabeConfig_sXMLClass", "defineId iconId name isTBD allSigns locatedTypeId exclusive basicBuff twinkleBuff lightUpCost battlePower twinkleProps", ""),
)
for _name, _base, _level in _CULTIVATION:
    CONTRACTS.append(Contract(_name, ("cultivation_source",), (f"**/*.{_name}.xml", f"*.{_name}.xml"),
                             "extract_replay_cultivation.py", "parse_cultivation_table", _attrs("/s", _base) +
                             (_attrs("/s/l", _level) if _level else ())))
for _relative in ("config/stargod/stargodconfig.json", "config/petbadge/petbadgeconfig.json", "config/astrolabe/astrolabeconfig.json"):
    CONTRACTS.append(Contract("h5-cultivation-" + _relative, ("cultivation_h5_source",),
        (_relative, "raw/" + _relative), "extract_replay_cultivation.py", "compare_h5",
        ("/[]/defineId",), format="json"))
for _name in ("pets.json", "transform-skills.json"):
    CONTRACTS.append(Contract("auxiliary-" + _name, ("source", "auxiliary_source"), (_name,),
        "build_catalog.py", "build:retained-auxiliary-json", format="json"))
for _relative in (
    "petdictionarydata/scripts/mmo/pet/petdictionarydata/PetDictionaryDataContents.as",
    "petdictionarydataupdate/scripts/mmo/pet/petdictionarydata/PetDictionaryDataContentsUpdate.as",
):
    CONTRACTS.append(Contract("pet-dictionary-" + Path(_relative).stem, ("source", "auxiliary_source"),
        (_relative,), "extract_pet_mechanisms.py", "parse_pet_rows:positional-constructor-contract-not-expanded-here",
        format="script"))

# Additional supplied arguments are evidence consumed by these entry points.
# A prior generated JSON is not reclassified as newly obtained official data.
ARGUMENT_READERS = {
    "mechanism_rules": ("extract_pet_mechanisms.py", "extract_pet_mechanisms:rules"),
    "legend_mechanics": ("extract_pet_mechanisms.py", "extract_pet_mechanisms:legend"),
    "race_expansions": ("extract_replay_supplements.py", "verify_race_expansions"),
    "reference_consumers": ("extract_pet_mechanisms.py", "attach_reference_consumption"),
}


def _generated_provenance_fields(role):
    """Local producer fields, not official game-field semantic coverage."""
    if role == "reference_consumers":
        from update_generated_evidence import REFERENCE_CONSUMERS
        fields = {"/consumerApprovals": "object"}
        for name in REFERENCE_CONSUMERS:
            base = "/consumerApprovals/" + name
            fields.update({base: "object", base + "/file": "string", base + "/sha256": "string"})
        return fields, "build_reference_consumer_evidence/_Inputs.approvals"
    if role == "race_expansions":
        return {"/inputTables": "array", "/inputTables/[]": "object", "/inputTables/[]/file": "string",
                "/inputTables/[]/sha256": "string"}, "build_race_expansion_evidence:inputTables"
    return {}, ""


def _sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _matches(path, pattern):
    # '*' is one structural segment, so /buff/* does not consume new pipe
    # columns or descendants just because the parent string is understood.
    parts, wanted = path.strip("/").split("/"), pattern.strip("/").split("/")
    if pattern.startswith("//"):
        parts = parts[-len(wanted):]
    return len(parts) == len(wanted) and all(fnmatch.fnmatchcase(a, b) for a, b in zip(parts, wanted))


def _type(value):
    return "null" if value is None else "boolean" if isinstance(value, bool) else "number" if isinstance(value, (int, float)) else \
        "string" if isinstance(value, str) else "array" if isinstance(value, list) else "object"


def _field_add(fields, path, value, *, count=1):
    record = fields.setdefault(path, {"types": set(), "occurrences": 0, "emptyValues": 0})
    record["types"].add(_type(value)); record["occurrences"] += count
    if value == "" or value is None: record["emptyValues"] += count


def _xml_fields(path):
    # Refuse custom entity declarations. The official table parser does not
    # require DTD/network/entity expansion for any registered contract.
    data = path.read_bytes()
    if re.search(br"<!\s*(?:DOCTYPE|ENTITY)\b", data, re.I):
        raise ValueError("XML DTD/entity declarations are outside the static table contract")
    root = ET.fromstring(data); fields = {}; elements = Counter()
    def walk(node, prefix):
        elements[prefix or "/"] += 1
        for key, value in node.attrib.items(): _field_add(fields, prefix + "/@" + key, value)
        if node.text and node.text.strip(): _field_add(fields, prefix + "/text()", node.text)
        for child in node: walk(child, prefix + "/" + child.tag)
    walk(root, "")
    return fields, dict(sorted(elements.items()))


def _json_fields(value, contract=None):
    fields = {}; containers = Counter()
    def walk(node, prefix):
        if isinstance(node, dict):
            containers[prefix or "/"] += 1
            # Numeric dictionary keys are record identities, not field names.
            # This is structural normalization, not a semantic name guess.
            identities = bool(node) and (all(re.fullmatch(r"-?\d+", key) for key in node) or
                bool(contract and (prefix or "/") in contract.map_paths))
            for key, child in node.items():
                step = "*" if identities else key.replace("~", "~0").replace("/", "~1")
                here = prefix + "/" + step
                _field_add(fields, here, child); walk(child, here)
        elif isinstance(node, list):
            containers[prefix or "/"] += 1
            for child in node:
                _field_add(fields, prefix + "/[]", child); walk(child, prefix + "/[]")
        elif contract and contract.pipe_rows and _matches(prefix, contract.pipe_rows) and isinstance(node, str):
            for index, item in enumerate(node.split("|")): _field_add(fields, prefix + "/|" + str(index), item)
    walk(value, "")
    return fields, dict(sorted(containers.items()))


def _script_dependencies():
    """Literal source references are an index, not an inferred field contract."""
    readers = {}
    for parser in sorted(PARSER_DIR.glob("extract_*.py")):
        tree = ast.parse(parser.read_text(encoding="utf-8-sig"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                value = node.value.replace("\\", "/")
                if value.endswith(".as") and "/" in value and not any(ch in value for ch in "\r\n\"'"):
                    readers.setdefault(value, set()).add(parser.name)
    # The extractor builds these exact paths from a tuple of literal names.
    for name in ("Cpsv3SkinConfig", "Cpsv3SkinSummary", "Cpsv3Skin", "Cpsv3AvailablePet"):
        readers.setdefault("scripts/mmo/changepetskinv3/other/" + name + ".as", set()).add("extract_pet_identity.py")
    for name in ("PetDragonDefines", "PetDragonDefine", "PetDragonGene", "PetDragonLevel"):
        readers.setdefault("dragon-official/interfaces/scripts/mmo/interfaces/petdragon/" + name + ".as", set()).add("extract_pet_dragons.py")
    return readers


def _under(path, root):
    try: path.resolve().relative_to(root.resolve()); return True
    except ValueError: return False


def _manifest_records(value, inherited=None):
    if isinstance(value, dict):
        context = dict(inherited or {})
        context.update({key: value[key] for key in ("resource", "package", "version", "retrievedUtc") if key in value})
        filename = value.get("file", value.get("path"))
        if isinstance(filename, str) and isinstance(value.get("sha256"), str):
            yield dict(context, file=filename, sha256=value["sha256"], bytes=value.get("bytes"))
        if isinstance(value.get("decompiledFile"), str) and isinstance(value.get("decompiledSha256"), str):
            yield dict(context, file=value["decompiledFile"], sha256=value["decompiledSha256"], bytes=None,
                       representation="decompiled-script")
        for item in value.values(): yield from _manifest_records(item, context)
    elif isinstance(value, list):
        for item in value: yield from _manifest_records(item, inherited)


def _manifest(path):
    return path.name in {"provenance.json", "extraction.json", "manifest.json", "h5-provenance.json", "pet-provenance.json",
                         "formation-provenance.json", "decompile-provenance.json", "battleconfig-provenance.json"} or path.name.endswith("-provenance.json")


def _discover(root, role):
    if root.is_file(): return [root]
    # Source table/script trees are enumerated completely. Auxiliary roots may
    # contain years of captures: only actual build-input subtrees are scanned.
    scopes = [root] if role not in {"source", "auxiliary_source"} else [root / name for name in (
        "xml", "h5-tables", "battleconfig/scripts", "interfaces/scripts", "battle/scripts", "h5-scripts",
        "pet-interface/scripts", "dragon-official", "presentation-battle", "presentation-interfaces",
        "formation", "pet-data", "pet-dictionary")]
    paths = set(p for p in root.iterdir() if p.is_file() and (p.suffix.lower() in {".swf", ".xml"} or _manifest(p)))
    if role in {"source", "auxiliary_source"}:
        paths.update(root / name for name in ("pets.json", "transform-skills.json", "skill_config.json", "skill_desc_config.json")
                     if (root / name).is_file())
    if role == "source":
        # Exact loader inputs copied by PublicResourceSession.h5_chain. They
        # retain raw fields and receive byte-backed control proof in the updater.
        paths.update(root / name for name in ("h5-start.json", "h5-version.json") if (root / name).is_file())
    for scope in scopes:
        if not scope.is_dir(): continue
        for path in scope.rglob("*"):
            if not path.is_file() or path.is_symlink() or not _under(path, root): continue
            if path.suffix.lower() in {".xml", ".as", ".js", ".json", ".swf", ".aqz", ".bin"}: paths.add(path)
    return sorted(paths)


def _canonical_artifacts(report):
    """One observed file/byte sequence, with every overlapping input role kept.

    Path identity is required. Independent copies with equal hashes stay
    independent inputs, including the H5 root copies and archive members.
    """
    groups = {}
    for row in report.get("artifacts", []):
        identity = (os.path.normcase(str(Path(row["sourcePath"]).resolve())), row["sha256"])
        groups.setdefault(identity, []).append(row)
    result, aliases = [], []
    for rows in groups.values():
        def rank(row):
            specific = any(item["contract"] != "literal-script-reference" for item in row["parserContracts"])
            identifiers = {item["contract"] for item in row["parserContracts"]}
            consumed = sum(len(contract.consumed) for contract in CONTRACTS if contract.key in identifiers)
            root = report.get("inputs", {}).get(row["role"], {}).get("sourcePath", "")
            # Explicit readers win, then the narrowest matching input root.
            return (-specific, -consumed, -len(Path(root).parts), row["key"])
        primary = dict(min(rows, key=rank))
        memberships = []
        for row in rows:
            memberships.append({key: row[key] for key in ("key", "role", "relativePath", "sourcePath", "sha256")})
            memberships.extend(row.get("aliases", []))
        by_key = {row["key"]: row for row in memberships}
        primary["roles"] = sorted({row["role"] for row in by_key.values()})
        primary["aliases"] = [dict(row, aliasOf=primary["key"]) for key, row in sorted(by_key.items()) if key != primary["key"]]
        aliases.extend(primary["aliases"])
        readers = {json.dumps(reader, sort_keys=True): reader for row in [primary, *rows] for reader in row["parserContracts"]}
        primary["parserContracts"] = list(readers.values())
        # A secondary role may hold a useful declaration for the same actual
        # file. Preserve it without treating an alias's generic scan as a new
        # table, new parser, or a different field interpretation.
        claims = {json.dumps(claim, sort_keys=True): claim for row in rows for claim in row["provenanceClaims"]}
        primary["provenanceClaims"] = list(claims.values())
        if claims:
            primary["byteVerification"] = "local-bytes-match-declarations" if all(
                claim["sha256"] == primary["sha256"] for claim in claims.values()) else "declared-hash-mismatch"
        result.append(primary)
    return sorted(result, key=lambda row: row["key"]), sorted(aliases, key=lambda row: row["key"])


def compare_coverage(current, previous):
    old = {row["key"]: row for row in _canonical_artifacts(previous)[0]}
    new = {row["key"]: row for row in _canonical_artifacts(current)[0]}
    old_files = {(os.path.normcase(str(Path(row["sourcePath"]).resolve())), row["sha256"]): row for row in old.values()}
    diff = {"added": sorted(new.keys() - old.keys()), "missing": sorted(old.keys() - new.keys()),
            "bytesChanged": [], "fieldChanges": [], "parserChanges": []}
    review = []
    for key in diff["added"]:
        row = new[key]
        prior = old_files.get((os.path.normcase(str(Path(row["sourcePath"]).resolve())), row["sha256"]))
        prior_alias = next((alias for alias in prior.get("aliases", []) if alias["key"] == key), None) if prior else None
        if prior_alias is not None:
            # A file argument may now point to freshly generated evidence.
            # Its unchanged old file remains under a parent root, promoting
            # a previously recorded alias. This is not a new physical input.
            diff.setdefault("roleReassignments", []).append({"key": key, "previousKey": prior["key"],
                "sourcePath": row["sourcePath"], "sha256": row["sha256"], "previousAlias": prior_alias,
                "meaning": "same-file-same-bytes; previously-recorded-alias-became-primary"})
            continue
        if row["kind"] == "table" and not row["parserContracts"]:
            review.append({"kind": "new-unmapped-table", "key": key,
                "meaning": "newly-discovered-in-this-input-set; absent-from-prior-materials; not-proof-of-official-addition"})
    for key in diff["missing"]:
        row = old[key]
        if row.get("parserContracts"):
            review.append({"kind": "consumed-input-missing", "key": key})
    for key in sorted(new.keys() & old.keys()):
        a, b = old[key], new[key]
        if a.get("sha256") != b.get("sha256"):
            diff["bytesChanged"].append(key)
            if b["kind"] == "script" and b["parserContracts"]:
                review.append({"kind": "consumer-script-changed", "key": key})
        before, after = a.get("fields", {}), b.get("fields", {})
        added, removed = sorted(after.keys() - before.keys()), sorted(before.keys() - after.keys())
        types = [{"field": field, "before": before[field]["types"], "after": after[field]["types"]}
                 for field in sorted(before.keys() & after.keys()) if before[field]["types"] != after[field]["types"]]
        if added or removed or types: diff["fieldChanges"].append({"key": key, "added": added, "removed": removed, "typeChanges": types})
        for field in added:
            if after[field]["coverage"] not in {"consumed-by-explicit-contract", "local-generated-provenance"}:
                review.append({"kind": "new-unrecognized-field", "key": key, "field": field})
        for field in removed:
            if before[field].get("coverage") == "consumed-by-explicit-contract":
                review.append({"kind": "consumed-field-missing", "key": key, "field": field})
        for change in types: review.append({"kind": "field-type-changed", "key": key, **change})
    prior_parsers = previous.get("parsers", {})
    for key, parser in current["parsers"].items():
        if key in prior_parsers and parser.get("sha256") != prior_parsers[key].get("sha256"):
            diff["parserChanges"].append(key); review.append({"kind": "parser-contract-needs-recheck", "parser": key})
    return diff, review


def audit_source_coverage(source, *, auxiliary_source=None, verified_source=None, mechanism_rules=None,
                          legend_mechanics=None, skin_source=None, title_source=None, formation_source=None,
                          race_expansions=None, cultivation_source=None, cultivation_h5_source=None,
                          cultivation_consumer_source=None, reference_consumers=None, previous=None):
    arguments = locals().copy(); arguments.pop("previous")
    roots = {role: Path(value).resolve() for role, value in arguments.items() if value is not None}
    if "auxiliary_source" not in roots: roots["auxiliary_source"] = roots["source"]
    report = {"schemaVersion": SCHEMA_VERSION, "auditPerformedUtc": datetime.now(timezone.utc).isoformat(),
              "scope": "local-input-structure-and-explicit-parser-contracts", "officialFreshnessVerified": False,
              "parserExecutionVerified": False, "inputs": {}, "artifacts": [], "parsers": {}, "contracts": [],
              "issues": [], "reviewRequired": [], "diff": None,
              "limits": ["Local hashes do not prove current official-server freshness.",
                         "A parser contract is static coverage, not a successful build or a complete semantic explanation.",
                         "Raw-retained and discovered fields are not counted as consumed.",
                         "Newly discovered inputs may have been omitted from the old bundle; inventory additions do not prove official additions.",
                         "Empty names/descriptions and optional empty values are not parsing failures."]}
    def issue(kind, **context):
        # A partial source bundle may retain declarations for optional exports
        # it did not package. That gap needs review, not a false parse failure.
        severity = "review" if kind == "declared-material-missing" else "error"
        report["issues"].append({"kind": kind, "severity": severity, **context})
    dependencies = _script_dependencies()
    parser_names = {c.parser for c in CONTRACTS} | {p for values in dependencies.values() for p in values} | {p[0] for p in ARGUMENT_READERS.values()} | {"update_generated_evidence.py"}
    for name in sorted(parser_names):
        path = PARSER_DIR / name
        report["parsers"][name] = {"sourcePath": str(path), "sha256": _sha(path) if path.is_file() else None}
    artifacts_by_key = {}
    hash_cache, manifest_cache = {}, {}
    def local_hash(path):
        stat = path.stat()
        key = (path.resolve(), stat.st_size, stat.st_mtime_ns)
        if key not in hash_cache:
            hash_cache[key] = _sha(path)
        return hash_cache[key], stat.st_size
    for role, root in roots.items():
        report["inputs"][role] = {"sourcePath": str(root), "exists": root.exists(),
            "evidenceStatus": "historical-evidence" if role in HISTORICAL_ROLES else "local-material-no-freshness-claim"}
        if not root.exists(): issue("input-root-missing", role=role, sourcePath=str(root)); continue
        if role == "auxiliary_source" and root == roots["source"]:
            report["inputs"][role]["aliasOf"] = "source"
            continue
        base = root if root.is_dir() else root.parent
        paths = _discover(root, role); declarations = {}
        for manifest in list(paths):
            if not _manifest(manifest): continue
            try:
                manifest_digest, _ = local_hash(manifest)
                cache_key = (manifest.resolve(), manifest_digest)
                if cache_key not in manifest_cache:
                    manifest_cache[cache_key] = _read_json(manifest)
                metadata = manifest_cache[cache_key]
            except (OSError, ValueError) as error:
                issue("manifest-unreadable", role=role, sourcePath=str(manifest), detail=str(error)); continue
            for record in _manifest_records(metadata):
                declared = Path(record["file"])
                candidates = [declared] if declared.is_absolute() else [base / declared, manifest.parent / declared, base.parent / declared]
                matches = list(dict.fromkeys(p.resolve() for p in candidates if p.is_file() and _under(p, base)))
                if len(matches) == 1:
                    local = matches[0]
                    declarations.setdefault(local, []).append({**record, "manifestPath": str(manifest), "manifestSha256": manifest_digest})
                    if local not in paths: paths.append(local)
                elif len(matches) > 1:
                    issue("ambiguous-manifest-path", role=role, file=record["file"], manifestPath=str(manifest))
                else:
                    issue("declared-material-missing", role=role, file=record["file"], manifestPath=str(manifest), declaredSha256=record["sha256"])
        for path in sorted(set(paths)):
            relative = path.relative_to(base).as_posix(); key = role + ":" + relative
            contracts = [c for c in CONTRACTS if role in c.roles and any(fnmatch.fnmatchcase(relative, pattern) for pattern in c.patterns)]
            readers = [{"contract": c.key, "parser": c.parser, "reader": c.reader} for c in contracts]
            if path.suffix.lower() == ".as":
                for suffix, parsers in dependencies.items():
                    if relative == suffix or relative.endswith("/" + suffix):
                        for parser in sorted(parsers):
                            if not any(r["parser"] == parser for r in readers):
                                readers.append({"contract": "literal-script-reference", "parser": parser,
                                                "reader": "static-source-reference; statement coverage not established"})
            if role in ARGUMENT_READERS and path == root:
                parser, reader = ARGUMENT_READERS[role]
                readers.append({"contract": "historical-argument", "parser": parser, "reader": reader})
            kind = "manifest" if _manifest(path) else "script" if path.suffix.lower() in {".as", ".js"} else \
                "table" if path.suffix.lower() in {".xml", ".json"} else "resource"
            try: digest, size = local_hash(path)
            except OSError as error:
                issue("material-unreadable", key=key, detail=str(error)); continue
            claims = declarations.get(path.resolve(), [])
            verification = "no-declared-hash"
            if claims:
                verification = "local-bytes-match-declarations" if all(c["sha256"] == digest for c in claims) else "declared-hash-mismatch"
                if verification == "declared-hash-mismatch": issue(verification, key=key, actualSha256=digest)
                if any(isinstance(c.get("bytes"), int) and not isinstance(c["bytes"], bool) and c["bytes"] != size for c in claims):
                    issue("declared-size-mismatch", key=key, actualBytes=size)
            artifact = {"key": key, "role": role, "relativePath": relative, "sourcePath": str(path.resolve()),
                "kind": kind, "bytes": size, "sha256": digest, "parserContracts": readers,
                "integration": "mapped-parser-contract" if readers else "provenance-only" if kind == "manifest" else "not-integrated",
                "evidenceStatus": "historical-evidence" if role in HISTORICAL_ROLES else "local-material-no-freshness-claim",
                "byteVerification": verification, "provenanceClaims": claims, "fields": {}, "structure": {}}
            artifact["fieldInspection"] = "complete-structural-inventory" if kind == "table" else "literal-map-fields" if contracts and contracts[0].format == "as-map" else "not-a-table-or-not-expanded"
            artifacts_by_key[key] = artifact
    report["artifacts"] = [artifacts_by_key[key] for key in sorted(artifacts_by_key)]
    # Resolve overlapping ownership before parsing any table or AS data. The
    # same physical file is inspected once under its explicit reader contract.
    report["artifacts"], report["aliases"] = _canonical_artifacts(report)
    contracts_by_id = {contract.key: contract for contract in CONTRACTS}
    for artifact in report["artifacts"]:
        key, kind, role = artifact["key"], artifact["kind"], artifact["role"]
        path, root = Path(artifact["sourcePath"]), roots[role]
        contracts = [contracts_by_id[row["contract"]] for row in artifact["parserContracts"] if row["contract"] in contracts_by_id]
        generated_fields, producer_reader = _generated_provenance_fields(role)
        try:
            if kind == "table" and path.suffix.lower() == ".xml": fields, structure = _xml_fields(path)
            elif kind == "table" and path.suffix.lower() == ".json": fields, structure = _json_fields(_read_json(path), contracts[0] if contracts else None)
            elif contracts and contracts[0].format == "as-map":
                from extract_skill_tables import parse_map
                rows = parse_map(path.read_text(encoding="utf-8-sig")); fields = {}
                for row in rows:
                    for name, value in row["fieldPairs"]: _field_add(fields, "/MAP/*/" + name, value)
                structure = {"MAP/rows": len(rows)}
            else: fields, structure = {}, {}
            artifact["structure"] = structure
            for field, observed in sorted(fields.items()):
                consumed = [c.key for c in contracts if any(_matches(field, selector) for selector in c.consumed)]
                coverage = "consumed-by-explicit-contract" if consumed else "retained-not-consumed" if any(c.retained for c in contracts) else "unknown-field"
                local_provenance = path == root and observed["types"] == {generated_fields.get(field)}
                if local_provenance:
                    coverage = "local-generated-provenance"
                artifact["fields"][field] = {**observed, "types": sorted(observed["types"]), "coverage": coverage,
                                                   "consumerContracts": consumed, "meaningVerified": False}
                if local_provenance:
                    artifact["fields"][field]["producerContract"] = {"producer": "update_generated_evidence.py", "reader": producer_reader}
                    artifact["fields"][field]["origin"] = "local-evidence-provenance; not-an-official-game-field"
                for contract in contracts:
                    match = re.fullmatch(r"(.+)/\|(\d+)", field)
                    if match and contract.pipe_rows and _matches(match[1], contract.pipe_rows) and int(match[2]) < len(contract.pipe_columns):
                        artifact["fields"][field]["decodedField"] = contract.pipe_columns[int(match[2])]
        except (OSError, ValueError, ET.ParseError, RecursionError) as error:
            issue("structure-unreadable", key=key, detail=str(error)); artifact["structureStatus"] = "unreadable"
    for contract in CONTRACTS:
        keys = [a["key"] for a in report["artifacts"] if any(r["contract"] == contract.key for r in a["parserContracts"])]
        report["contracts"].append({"id": contract.key, "parser": contract.parser, "reader": contract.reader,
            "roles": list(contract.roles), "patterns": list(contract.patterns), "consumedSelectors": list(contract.consumed),
            "matchedInputs": keys, "status": "present" if keys else "not-present"})
    if previous is not None:
        prior = _read_json(Path(previous)) if isinstance(previous, (str, Path)) else previous
        if not isinstance(prior, dict) or prior.get("schemaVersion") != SCHEMA_VERSION or not isinstance(prior.get("artifacts"), list):
            raise ValueError("previous must be a compatible source-coverage report")
        report["diff"], report["reviewRequired"] = compare_coverage(report, prior)
    else:
        report["reviewRequired"] = [{"kind": "unmapped-table", "key": row["key"]} for row in report["artifacts"]
                                    if row["kind"] == "table" and not row["parserContracts"]]
    report["reviewRequired"].extend(report["issues"])
    field_counts = Counter(f["coverage"] for a in report["artifacts"] for f in a["fields"].values())
    report["coverage"] = {"artifactCount": len(report["artifacts"]), "byKind": dict(Counter(a["kind"] for a in report["artifacts"])),
        "byIntegration": dict(Counter(a["integration"] for a in report["artifacts"])), "fieldCounts": dict(field_counts),
        "aliasCount": len(report["aliases"]), "issueCount": len(report["issues"]), "reviewRequiredCount": len(report["reviewRequired"])}
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    for role in ROLES: parser.add_argument("--" + role.replace("_", "-"), type=Path)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = vars(parser.parse_args(argv)); output = args.pop("output")
    report = audit_source_coverage(**args)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), **report["coverage"]}, ensure_ascii=False))
    return 1 if any(issue["severity"] == "error" for issue in report["issues"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
