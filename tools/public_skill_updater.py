#!/usr/bin/env python3
"""Update the complete workbench skill catalog through the shared H5 session.

The workbench and replay updater resolve and download their resource union in
one public-resource session. This module never opens an independent connection
or makes its own interpretation of the official H5 version/override chain.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import stat
import zipfile
from pathlib import Path, PurePosixPath

from generate_pet_skill_data import PET_FIELDS, SKILL_SLOTS, build_catalog


H5 = "https://aoqi.100bt.com/h5/"
ARCHIVES = (
    "config/battleconfig.aqz", "config/formation.aqz", "config/petdictionarydata.json.aqz",
    "config/petdata.json.aqz", "config/relation.aqz", "config/changepetskin.aqz",
    "config/equipment4pet.aqz",
)
DIRECT = ("config/pet/petinfoconfig.json", "config/pet/petevoconfig.json", "config/pet/tiebaconfig.json")
MAX_DOWNLOAD_BYTES = 32 * 1024 * 1024
MAX_ARCHIVE_FILES = 4096
MAX_MEMBER_BYTES = 32 * 1024 * 1024
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
_DEVICE_NAME = re.compile(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?\Z", re.IGNORECASE)

# build_catalog also supports partial offline research folders. The public
# updater must supply its complete input set: an omitted optional_json sheet
# must never silently erase a previously available display/association table.
# Alternatives are the generator's documented dictionary/data layouts only;
# unlike its offline fallback, these paths stay inside this fresh extraction.
REQUIRED_INPUTS = (
    (("petdictionarydata.json/petdictionarydata.json", "pet/petdictionarydata.json"), dict, list),
    (("battleconfig/skill_desc_config.json",), dict, str),
    (("battleconfig/skill_config.json",), dict, dict),
    (("battleconfig/combo_buffs_config.json",), dict, None),
    (("battleconfig/entry_config.json",), list, dict),
    (("battleconfig/buffer_config.json",), dict, None),
    (("formation/transformskillconfig.json",), dict, list),
    (("formation/heroskillconfig.json",), dict, list),
    (("formation/psychicsskillconfig.json",), dict, list),
    (("formation/lingchuskillsulingconfig.json",), dict, dict),
    (("formation/shenyunskillqiyunconfig.json",), dict, dict),
    (("relation/relationconfig.json",), dict, None),
    (("pet/petinfoconfig.json",), dict, list),
    (("pet/petevoconfig.json",), dict, list),
    (("pet/tiebaconfig.json",), list, dict),
    (("battleconfig/summon_config.json",), list, dict),
    (("battleconfig/carry_config.json",), list, dict),
    (("battleconfig/huan_skill_config.json",), dict, list),
    (("battleconfig/almighty-skill-config.json",), list, dict),
    (("petdata.json/petdata.json", "pet/petdata.json"), list, dict),
    (("changepetskin/changepetskinconfig.json",), dict, None),
)
_NESTED_INPUTS = {
    "battleconfig/combo_buffs_config.json": (("combo", list, dict),),
    "battleconfig/buffer_config.json": (("buff", dict, str),),
    "relation/relationconfig.json": tuple((key, list, list) for key in
                                           ("relationTeam", "relationPoint", "relationEffect")),
    "changepetskin/changepetskinconfig.json": (("defines", list, dict),),
}
_OUTPUT_TYPES = {"pets": (dict, dict), "skills": (dict, dict), "entries": (dict, str),
                 "buffs": (dict, dict), "combos": (dict, list), "transformSkills": (dict, dict),
                 "heroSkills": (dict, list), "psychicsSkills": (dict, list),
                 "lingchuRules": (dict, dict), "shenyunRules": (dict, dict),
                 "almightySkills": (list, dict)}
_PET_ASSOCIATIONS = {"baseStats": dict, "relations": list, "summons": list,
                     "carry": list, "skins": list, "forms": list, "huan": list,
                     "almighty": list, "starGodSkill": dict, "officialRole": str,
                     "hexagon": str, "evolutionFrom": int, "skillEntries": list,
                     "traits": list}


def versioned(path: str, version: str) -> str:
    """Kept for callers formatting a known URL; resolution belongs to session."""
    head, dot, suffix = path.rpartition(".")
    return f"{head}~{version}.{suffix}" if dot else f"{path}~{version}"


def safe_extract(data: bytes, destination: Path) -> list[dict]:
    """Extract every official member, rejecting ambiguous Windows ZIP paths.

    The full member list is checked before any output file is created. Reads
    and accumulated expansion are bounded independently of ZIP declarations.
    No dependency on replay-only modules is needed for a skill-only update.
    """
    if len(data) > MAX_DOWNLOAD_BYTES:
        raise ValueError("official skill archive exceeds the download limit")
    destination = Path(destination)
    root = destination.resolve()
    if destination.is_symlink():
        raise ValueError("unsafe official skill archive destination")
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        members = package.infolist()
        if len(members) > MAX_ARCHIVE_FILES:
            raise ValueError("official skill archive has too many members")
        planned = []
        seen = {}
        declared_total = 0
        for item in members:
            raw = getattr(item, "orig_filename", item.filename)
            raw_path = raw[:-1] if raw.endswith("/") else raw
            parts = raw_path.split("/")
            if not raw_path or raw != item.filename or "\\" in raw or "\0" in raw \
                    or PurePosixPath(raw).is_absolute() \
                    or any(not part or part in (".", "..") or part.endswith((" ", "."))
                           or any(char in part for char in '<>:"|?*')
                           or any(ord(char) < 32 for char in part)
                           or _DEVICE_NAME.fullmatch(part) for part in parts):
                raise ValueError("unsafe official skill archive path")
            mode = (item.external_attr >> 16) & 0xFFFF
            kind = stat.S_IFMT(mode)
            if item.flag_bits & 1 or kind not in (0, stat.S_IFREG, stat.S_IFDIR) \
                    or (kind == stat.S_IFDIR and not item.is_dir()) \
                    or (item.is_dir() and (kind == stat.S_IFREG or item.file_size)):
                raise ValueError("unsafe official skill archive member type")
            key = "/".join(parts).casefold()
            if key in seen:
                raise ValueError("duplicate official skill archive path")
            seen[key] = item.is_dir()
            if item.file_size < 0 or item.file_size > MAX_MEMBER_BYTES:
                raise ValueError("official skill archive member exceeds the size limit")
            declared_total += item.file_size
            if declared_total > MAX_ARCHIVE_BYTES:
                raise ValueError("official skill archive exceeds the expansion limit")
            target = destination.joinpath(*parts)
            if not target.resolve().is_relative_to(root):
                raise ValueError("unsafe official skill archive destination")
            probe = destination
            for part in parts:
                probe /= part
                if probe.is_symlink():
                    raise ValueError("unsafe official skill archive destination")
            if target.exists() and not (item.is_dir() and target.is_dir()):
                raise ValueError("official skill archive would overwrite an existing file")
            planned.append((item, target, parts))
        for _, _, parts in planned:
            for length in range(1, len(parts)):
                if seen.get("/".join(parts[:length]).casefold()) is False:
                    raise ValueError("official skill archive file/directory path conflict")

        expanded_total = 0
        evidence = []
        for item, target, _ in planned:
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            member_bytes = 0
            digest = hashlib.sha256()
            with package.open(item) as source, target.open("xb") as output:
                while chunk := source.read(64 * 1024):
                    member_bytes += len(chunk)
                    expanded_total += len(chunk)
                    if member_bytes > MAX_MEMBER_BYTES or member_bytes > item.file_size \
                            or expanded_total > MAX_ARCHIVE_BYTES:
                        raise ValueError("official skill archive exceeds the expansion limit")
                    digest.update(chunk)
                    output.write(chunk)
            if member_bytes != item.file_size:
                raise ValueError("official skill archive member is truncated")
            evidence.append({"member": item.filename, "sha256": digest.hexdigest(), "bytes": member_bytes})
        return evidence


def _digest(value) -> str:
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                    separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _parser_fingerprint() -> str:
    root = Path(__file__).resolve().parent
    return _digest({name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                    for name in ("public_skill_updater.py", "generate_pet_skill_data.py", "public_catalog_quality.py",
                                 "public_skill_fidelity.py")})


def _complete(catalog) -> bool:
    return isinstance(catalog, dict) and catalog.get("schema") == 1 \
        and all(isinstance(catalog.get(key), kind) and bool(catalog[key])
                for key, (kind, _) in _OUTPUT_TYPES.items()) \
        and all(isinstance(catalog.get(key), dict) and len(catalog[key]) >= minimum
                for key, minimum in (("pets", 9000), ("skills", 20000), ("entries", 100)))


def _rows(value):
    return value.values() if isinstance(value, dict) else value


def _shape(value, kind, row_kind, label):
    if not isinstance(value, kind) or (row_kind is not None and
            any(not isinstance(row, row_kind) for row in _rows(value))):
        raise ValueError("official skill sheet has an unsupported type: " + label)


def _json_bytes(raw):
    def reject(value):
        raise ValueError("nonfinite official skill JSON value: " + value)
    return json.loads(raw.decode("utf-8-sig"), parse_constant=reject)


def _validate_inputs(root: Path) -> dict:
    proof = {}
    for alternatives, kind, row_kind in REQUIRED_INPUTS:
        paths = [root / name for name in alternatives if (root / name).is_file()]
        if len(paths) != 1:
            raise ValueError("missing or ambiguous required official skill sheet: " + alternatives[0])
        path = paths[0]
        if path.stat().st_size > MAX_MEMBER_BYTES:
            raise ValueError("required official skill sheet exceeds the size limit")
        raw = path.read_bytes()
        if len(raw) > MAX_MEMBER_BYTES:
            raise ValueError("required official skill sheet exceeds the size limit")
        value = _json_bytes(raw)
        _shape(value, kind, row_kind, alternatives[0])
        for key, nested_kind, nested_row in _NESTED_INPUTS.get(alternatives[0], ()):
            _shape(value.get(key), nested_kind, nested_row, alternatives[0] + "/" + key)
        if alternatives[0].startswith("petdictionarydata.json/"):
            if any(len(row) < len(PET_FIELDS) for row in value.values()):
                raise ValueError("official skill pet dictionary has truncated rows")
            id_columns = [PET_FIELDS.index("raceId"), *(PET_FIELDS.index(name) for name in SKILL_SLOTS.values())]
            for key, row in value.items():
                if not re.fullmatch(r"[1-9][0-9]*", key) or row[id_columns[0]] != int(key):
                    raise ValueError("official skill pet dictionary has an inconsistent identity")
                if any(type(row[index]) is not int or row[index] < 0 for index in id_columns):
                    raise ValueError("official skill pet dictionary has an invalid identity or skill reference")
            if any(len(row) > len(PET_FIELDS) for row in value.values()):
                raise ValueError("official skill pet dictionary has new columns requiring parser review")
        proof[alternatives[0]] = {"file": path.relative_to(root).as_posix(),
                                 "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw),
                                 "rows": len(value)}
    return proof


def official_reference_evidence(root: Path) -> list[dict]:
    """Prove selected missing references directly from complete official JSON.

    This is independent of build_catalog: no generated output, display name or
    existing exception supplies a reference or proves a definition absent.
    Receipts describe only the three explicit projections checked here. They
    do not grant permission to publish or claim coverage of other references.
    """
    from generate_pet_skill_data import SKILL_SLOTS

    root = Path(root).resolve()

    def pointer(*parts):
        return "/" + "/".join(str(part).replace("~", "~0").replace("/", "~1") for part in parts)

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("ambiguous duplicate key in official reference evidence: " + key)
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError("nonfinite value in official reference evidence: " + value)

    def read(names, expected):
        paths = [root / name for name in names if (root / name).is_file()]
        if len(paths) != 1:
            raise ValueError("missing or ambiguous official reference evidence sheet: " + names[0])
        path = paths[0]
        if not path.resolve().is_relative_to(root):
            raise ValueError("official reference evidence sheet is outside the extraction root")
        if path.stat().st_size > MAX_MEMBER_BYTES:
            raise ValueError("official reference evidence sheet exceeds the size limit")
        with path.open("rb") as stream:
            raw = stream.read(MAX_MEMBER_BYTES + 1)
        if len(raw) > MAX_MEMBER_BYTES:
            raise ValueError("official reference evidence sheet exceeds the size limit")
        value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=pairs, parse_constant=reject_constant)
        if type(value) is not expected:
            raise ValueError("unsupported official reference evidence sheet type: " + names[0])
        return value, {"file": path.relative_to(root).as_posix(),
                       "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}

    def identifier(value, label, *, zero=False):
        if type(value) is not int or value < (0 if zero else 1):
            raise ValueError("invalid integer ID in official reference evidence: " + label)
        return str(value)

    def raw_reference(source, parts, value):
        return dict(source, pointer=pointer(*parts), value=value)

    dictionary, dictionary_source = read(
        ("petdictionarydata.json/petdictionarydata.json", "pet/petdictionarydata.json"), dict)
    skill_text, skill_source = read(("battleconfig/skill_desc_config.json",), dict)
    skins, skin_source = read(("changepetskin/changepetskinconfig.json",), dict)
    tieba, tieba_source = read(("pet/tiebaconfig.json",), list)
    race_column = PET_FIELDS.index("raceId")
    slot_columns = {slot: (field, PET_FIELDS.index(field)) for slot, field in SKILL_SLOTS.items()}
    for key, row in dictionary.items():
        if type(row) is not list or len(row) != len(PET_FIELDS):
            raise ValueError("unsupported official reference dictionary row: " + key)
        if identifier(row[race_column], key) != key:
            raise ValueError("official reference dictionary key/raceId mismatch: " + key)
        for _, (field, column) in slot_columns.items():
            identifier(row[column], key + "/" + field, zero=True)
    if any(not re.fullmatch(r"[1-9][0-9]*", key) or type(value) is not str
           for key, value in skill_text.items()):
        raise ValueError("unsupported official reference skill description row")

    result = []
    def receipt(path, namespace, target, owner, identity, mapping):
        definitions, source = (dictionary, dictionary_source) if namespace == "pets" else (skill_text, skill_source)
        # A present-but-empty definition is not an absent definition. A slim
        # output projection may omit it; that requires a different review.
        if target in definitions:
            return
        result.append({"kind": "missing-reference", "path": path,
                       "targetNamespace": namespace, "targetId": target,
                       "evidence": {
                           "kind": "official-raw-reference-with-absent-definition",
                           "owner": owner, "ownerIdentity": identity,
                           "target": dict(source, pointer=pointer(target), id=target,
                                          namespace=namespace, present=False, rows=len(definitions)),
                           "mapping": mapping}})

    identities = {key: raw_reference(dictionary_source, (key, race_column), row[race_column])
                  for key, row in dictionary.items()}
    for race, row in dictionary.items():
        for slot, (field, column) in slot_columns.items():
            value = row[column]
            if value:
                receipt(pointer("pets", race, "slots", slot), "skills", str(value),
                        raw_reference(dictionary_source, (race, column), value), identities[race],
                        {"rule": "pet-dictionary-skill-slot", "field": field, "column": column,
                         "outputSlot": slot})

    definitions = skins.get("defines")
    if type(definitions) is not list:
        raise ValueError("unsupported official reference skin definitions")
    skin_positions = {}
    for definition_index, definition in enumerate(definitions):
        if type(definition) is not dict:
            raise ValueError("unsupported official reference skin definition row")
        available = definition.get("availablePets")
        if available is None:
            continue  # The official optional collection has no associations.
        if type(available) is not list:
            raise ValueError("unsupported official reference availablePets collection")
        for available_index, item in enumerate(available):
            if type(item) is not dict:
                raise ValueError("unsupported official reference availablePets row")
            parts = ("defines", definition_index, "availablePets", available_index)
            base = identifier(item.get("raceIdNeed"), pointer(*parts, "raceIdNeed"), zero=True)
            target = identifier(item.get("raceId"), pointer(*parts, "raceId"), zero=True)
            if base == "0" or target == "0":
                continue  # The generator records only positive endpoint pairs.
            position = skin_positions.get(base, 0)
            skin_positions[base] = position + 1
            if base in identities:
                receipt(pointer("pets", base, "skins", position, "raceId"), "pets", target,
                        raw_reference(skin_source, (*parts, "raceId"), item["raceId"]), identities[base],
                        {"rule": "skin-availablePets-in-source-order", "outputIndex": position,
                         "ownerReference": raw_reference(skin_source, (*parts, "raceIdNeed"), item["raceIdNeed"])})

    # build_catalog assigns the entire raceIds list to every member. A later
    # row for the same owner replaces its earlier list, rather than merging it.
    groups = {}
    for row_index, row in enumerate(tieba):
        if type(row) is not dict or type(row.get("raceIds", [])) is not list:
            raise ValueError("unsupported official reference form group")
        for member_index, value in enumerate(row.get("raceIds", [])):
            race = identifier(value, pointer(row_index, "raceIds", member_index))
            groups[race] = (row_index, member_index, row["raceIds"])
    for race, (row_index, member_index, members) in groups.items():
        if race not in identities:
            continue
        for target_index, value in enumerate(members):
            receipt(pointer("pets", race, "forms", target_index), "pets", str(value),
                    raw_reference(tieba_source, (row_index, "raceIds", target_index), value), identities[race],
                    {"rule": "tieba-last-row-for-owner", "outputIndex": target_index,
                     "ownerReference": raw_reference(tieba_source, (row_index, "raceIds", member_index), int(race))})
    return result


def _catalog_counts(catalog, *, strict=False):
    counts = {}
    if not isinstance(catalog, dict):
        return counts
    for key, (kind, row_kind) in _OUTPUT_TYPES.items():
        table = catalog.get(key)
        if strict:
            _shape(table, kind, row_kind, "output/" + key)
        if not isinstance(table, kind):
            continue
        counts[key] = len(table)
        # Rules and raw hero/psychics rows remain complete, including fields
        # unknown to this updater. Count their values without projecting them.
        if key in ("lingchuRules", "shenyunRules", "heroSkills", "psychicsSkills", "combos"):
            counts[key + "/values"] = sum(len(row) for row in _rows(table)
                                           if isinstance(row, (dict, list)))
    for table_name, fields in (("pets", _PET_ASSOCIATIONS), ("skills", {"animation": dict})):
        table = catalog.get(table_name, {})
        if not isinstance(table, dict):
            continue
        for field, kind in fields.items():
            values = [row[field] for row in table.values() if isinstance(row, dict) and field in row]
            if strict and any(type(value) is not kind for value in values):
                raise ValueError("official skill association has an unsupported type: " + field)
            valid = [value for value in values if type(value) is kind and bool(value)]
            counts[table_name + "/" + field] = len(valid)
            if kind is list:
                counts[table_name + "/" + field + "/values"] = sum(map(len, valid))
    return counts


def _validate_preservation(catalog, current, baseline, *, exact_associations_verified=False):
    if not _complete(catalog):
        raise ValueError("official skill catalog is incomplete; the previous cache was kept")
    candidate = _catalog_counts(catalog, strict=True)
    prior_counts = [_catalog_counts(reference) for reference in (current, baseline)]
    for key in set().union(candidate, *prior_counts):
        # These three old projections contained suffix/group/substring joins.
        # After the independent per-pet official audit AND quality gate pass,
        # keeping their old inflated counts would preserve the proven errors.
        # All other tables/fields retain this coarse completeness guard.
        if exact_associations_verified and key in {
                'pets/' + field + suffix for field in ('huan', 'almighty', 'traits') for suffix in ('', '/values')}:
            continue
        reference = max((counts.get(key, 0) for counts in prior_counts), default=0)
        if candidate.get(key, 0) < reference * 0.9:
            raise ValueError("official skill catalog lost required data; the previous cache was kept: " + key)


def _baseline(updater):
    directory = getattr(updater, "baseline", None)
    path = Path(directory) / "pet-skill-data.json" if directory is not None else None
    if path is None or not path.is_file():
        return {}
    if path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("embedded skill catalog exceeds the size limit")
    with path.open("rb") as stream:
        raw = stream.read(MAX_ARCHIVE_BYTES + 1)
    if len(raw) > MAX_ARCHIVE_BYTES:
        raise ValueError("embedded skill catalog exceeds the size limit")
    return _json_bytes(raw)


def _record(row: dict) -> dict:
    # Scratch paths and retrieval timestamps must not make identical bytes look
    # like a new catalog. Keep complete public URL/version/resolution evidence.
    return {key: row[key] for key in ("url", "sha256", "bytes", "resource", "version", "resolution")
            if key in row}


def _resource_bytes(path: Path, record: dict) -> bytes:
    path = Path(path)
    if path.stat().st_size > MAX_DOWNLOAD_BYTES:
        raise ValueError("official skill resource exceeds the download limit")
    with path.open("rb") as stream:
        data = stream.read(MAX_DOWNLOAD_BYTES + 1)
    if len(data) > MAX_DOWNLOAD_BYTES:
        raise ValueError("official skill resource exceeds the download limit")
    if record.get("sha256") != hashlib.sha256(data).hexdigest() \
            or record.get("bytes") != len(data):
        raise ValueError("shared official skill resource cache failed validation")
    return data


def update_skills(updater, writer, _versions=None) -> bool:
    session = updater.public_resources
    chain = session.h5_chain()
    release = str(chain.get("version", ""))
    if not release.isdecimal():
        raise ValueError("official H5 skill version is invalid")

    source_root = updater.scratch / "skill-h5"
    download_root = updater.scratch / "skill-h5-downloads"
    resources, payloads = {}, {}
    # All ten dependencies are resolved/downloaded even when start.version did
    # not change: start.files, mix aliases and same-version bytes may change.
    for resource in ARCHIVES + DIRECT:
        path, record = session.h5_resource(resource, download_root / resource)
        payloads[resource] = _resource_bytes(path, record)
        resources[resource] = _record(record)

    source = {"kind": "aoqi-official-h5-skill-data", "version": release,
              "resources": resources, "resourceSignature": session.h5_signature(ARCHIVES + DIRECT),
              "contentFingerprint": _digest({path: row["sha256"] for path, row in resources.items()}),
              "parserFingerprint": _parser_fingerprint(),
              "h5Chain": {"entrySha256": chain["entry"]["sha256"],
                          "manifest": _record(chain["manifest"]), "main": _record(chain["main"])}}
    current = updater.current("skills")
    baseline = _baseline(updater)
    previous = current.get("source", {}) if isinstance(current, dict) else {}
    target = updater.catalog / "pet-skill-data.json"
    # Even an unchanged release is re-read from verified official bytes. A
    # cached candidate must not be its own evidence of correct descriptions.
    if source_root.exists():
        raise ValueError("official skill extraction directory is not fresh")
    source["extractedFiles"] = {}
    for resource in ARCHIVES:
        folder = PurePosixPath(resource).name.removesuffix(".aqz")
        source["extractedFiles"][resource] = safe_extract(payloads[resource], source_root / folder)
    for resource in DIRECT:
        path = source_root / resource.removeprefix("config/")
        path.parent.mkdir(parents=True, exist_ok=True)
        _json_bytes(payloads[resource])
        path.write_bytes(payloads[resource])
        source["extractedFiles"][resource] = [
            {"member": resource.removeprefix("config/"), "sha256": resources[resource]["sha256"],
             "bytes": resources[resource]["bytes"]}]
    source["validatedInputs"] = _validate_inputs(source_root)
    if 'skills' not in getattr(updater, 'force_components', ()) and _complete(current) and isinstance(previous, dict) \
            and previous.get("contentFingerprint") == source["contentFingerprint"] \
            and previous.get("parserFingerprint") == source["parserFingerprint"] \
            and isinstance(previous.get("extractedFiles"), dict) \
            and set(previous["extractedFiles"]) == set(ARCHIVES + DIRECT) \
            and isinstance(previous.get("validatedInputs"), dict) \
            and set(previous["validatedInputs"]) == {names[0] for names, _, _ in REQUIRED_INPUTS}:
        # A different official release can retain identical sheets. Refresh its
        # provenance without reparsing or omitting any of the catalog fields.
        catalog = dict(current, source=source)
    else:
        catalog = build_catalog(source_root, source)
    reference_evidence = official_reference_evidence(source_root)
    from public_skill_fidelity import audit_skill_fidelity
    fidelity = audit_skill_fidelity(source_root, catalog, previous=current or baseline or None)
    if hasattr(updater, 'assess_quality'):
        updater.assess_quality('skills', catalog, current or baseline or None,
                               reference_evidence=reference_evidence, source_fidelity=fidelity)
    else:
        from public_catalog_quality import enforce_catalog_quality
        enforce_catalog_quality('skills', catalog, current or baseline or None,
                                updater.catalog.parent / 'data-tools' / 'quality',
                                reference_evidence=reference_evidence, source_fidelity=fidelity)
    _validate_preservation(catalog, current, baseline, exact_associations_verified=fidelity['passed'])
    session.assert_stable(kinds=("h5",))
    changed = catalog != current
    if changed or not target.is_file():
        writer(target, catalog)
    return changed
