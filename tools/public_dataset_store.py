"""Build immutable public-data candidates; publication belongs to the Qt helper.

Every file written by the legacy component builders lives in the transaction's
workspace. This module never writes public/current.json or a live catalog file.
"""
from __future__ import annotations

import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import uuid

COMPONENTS = {"pets": "pet-detail-data.json", "skills": "pet-skill-data.json",
              "shop": "shop-exchange-data.json", "images": "pet-image-index.json",
              "icons": "public-icon-index.json", "routines": "routine-overview.json",
              "battleReplay": "battle-replay-catalog.brpack"}
ATTACHMENTS = ("activity-exchange-data.json", "activity-evolution-data.json")
CORE = ("pets", "skills", "shop", "battleReplay")
SHA = re.compile(r"[0-9a-f]{64}\Z")
ATTEMPT = re.compile(r"[0-9a-f]{32}\Z")
MAX_BYTES = 128 * 1024 * 1024
DEPENDENCY_GROUPS = [
    {"id": "core-definitions", "components": list(CORE),
     "reason": "Shared official definitions and existing cross-view consumers must advance together",
     "evidence": [
         {"components": ["pets", "battleReplay"], "resources": ["pet/petdictionarydata", "pet/petdictionarydataupdate", "library/interfaces"],
          "consumers": ["public_data_updater.PET_RESOURCES", "public_battle_replay_updater.refresh_data_packages", "battle_replay.extract_replay_cultivation"]},
         {"components": ["skills", "battleReplay"], "resources": ["config/battleconfig.aqz", "config/petdictionarydata.json.aqz", "config/formation.aqz"],
          "consumers": ["public_skill_updater.ARCHIVES", "public_battle_replay_updater.replay_dependencies"]},
         {"components": ["pets", "shop"], "resources": ["library/materialdata", "library/materialdataupdate"],
          "consumers": ["public_names_updater.NAME_RESOURCES", "public_activity_exchange_updater.PACKAGE_RESOURCES", "ShopExchangeCatalog"]}]},
    {"id": "pet-pictures", "components": ["images"], "requires": ["pets"],
     "reason": "Updater.images reads current pets IDs; an unavailable picture does not redefine a pet",
     "evidence": ["public_data_updater.Updater.images"]},
    {"id": "public-icons", "components": ["icons"], "requires": ["pets"],
     "reason": "update_icons reads current stargod definitions; unavailable pixels do not redefine a stargod",
     "evidence": ["public_icon_updater.update_icons"]},
    {"id": "routine-rules", "components": ["routines"],
     "reason": "The seven registered routine packages are parsed as one independent directory",
     "evidence": ["public_routine_updater.RESOURCES", "public_routine_rules.SPECS"]},
]


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def canonical(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def read_bytes(path: Path, limit=MAX_BYTES) -> bytes:
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ValueError("Invalid public-data file: " + str(path))
    with path.open("rb") as stream:
        raw = stream.read(limit + 1)
    if not 0 < len(raw) <= limit:
        raise ValueError("Public-data file exceeds its size limit")
    return raw


def parse_json(raw: bytes) -> dict:
    def unique(pairs):
        out = {}
        for key, value in pairs:
            if key in out:
                raise ValueError("Duplicate public-data JSON key")
            out[key] = value
        return out
    value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=unique,
                       parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Non-finite JSON number")))
    if not isinstance(value, dict):
        raise ValueError("Public-data JSON must be an object")
    return value


def atomic_write(path: Path, raw: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".candidate-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def immutable_write(path: Path, raw: bytes):
    """Publish fully written bytes without ever replacing an existing object."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if read_bytes(path) != raw:
            raise ValueError("Immutable public-data object conflicts with existing bytes")
        return
    handle, temporary = tempfile.mkstemp(prefix=".object-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            if read_bytes(path) != raw:
                raise ValueError("Concurrent immutable public-data object conflict")
    finally:
        Path(temporary).unlink(missing_ok=True)


def validate_payload(raw: bytes, form: str):
    if form == "brpack":
        from battle_replay.catalog_pack import unpack_bytes, validate_catalog_bytes
        unpacked = unpack_bytes(raw)
        return validate_catalog_bytes(unpacked)
    if form != "json":
        raise ValueError("Unsupported public-data object format")
    return parse_json(raw)


def object_path(root: Path, reference: dict) -> Path:
    value = reference.get("sha256")
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise ValueError("Invalid public-data object digest")
    path = root / "public" / "objects" / value / "data"
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Public-data object path escaped its data root")
    return path


def _verified_object_bytes(root: Path, reference: dict) -> bytes:
    if type(reference.get("bytes")) is not int or not 0 < reference["bytes"] <= MAX_BYTES:
        raise ValueError("Invalid public-data object length")
    raw = read_bytes(object_path(root, reference))
    if sha(raw) != reference["sha256"] or len(raw) != reference["bytes"]:
        raise ValueError("Public-data object digest or size does not match")
    return raw


def read_object_ref(root: Path, reference: dict) -> bytes:
    raw = _verified_object_bytes(root, reference)
    validate_payload(raw, reference.get('format'))
    return raw


def read_object_payload(root: Path, reference: dict) -> dict:
    """Same byte/hash/schema gate, returning the single parsed value to its caller."""
    return validate_payload(_verified_object_bytes(root, reference), reference.get('format'))


def put_object(root: Path, raw: bytes, form: str, origin: dict) -> dict:
    validate_payload(raw, form)
    reference = {"sha256": sha(raw), "bytes": len(raw), "format": form, "origin": copy.deepcopy(origin)}
    immutable_write(object_path(root, reference), raw)
    return reference


def load_current(root: Path, *, validate_objects=True):
    pointer = root / "public" / "current.json"
    if not pointer.exists():
        return "", "", None
    raw = read_bytes(pointer, 64 * 1024)
    value = parse_json(raw)
    revision = value.get("revision", "")
    if value.get("schema") != 1 or not SHA.fullmatch(revision) or not ATTEMPT.fullmatch(value.get("generation", "")):
        raise ValueError("Invalid current public-data pointer")
    manifest_raw = read_bytes(root / "public" / "releases" / revision / "manifest.json", 4 * 1024 * 1024)
    if sha(manifest_raw) != revision:
        raise ValueError("Public-data manifest digest does not match")
    manifest = parse_json(manifest_raw)
    if manifest.get("schema") != 1 or manifest.get("dependencySchema") != 1 or set(manifest.get("components", {})) != set(COMPONENTS):
        raise ValueError("Incomplete public-data manifest")
    if not set(manifest.get("attachments", {})).issubset(ATTACHMENTS):
        raise ValueError("Unknown public-data attachment")
    for name, reference in manifest["components"].items():
        if reference.get("format") not in (("json", "brpack") if name == "battleReplay" else ("json",)):
            raise ValueError("Public-data component format mismatch")
        if validate_objects:
            read_object_ref(root, reference)
    for reference in manifest.get("attachments", {}).values():
        if reference.get("format") != "json":
            raise ValueError("Public-data attachment format mismatch")
        if validate_objects:
            read_object_ref(root, reference)
    return revision, sha(raw), manifest


def expanded_components(requested) -> list[str]:
    selected = set(COMPONENTS) if requested is None else set(requested)
    if not selected or not selected.issubset(COMPONENTS):
        raise ValueError("Invalid requested public-data components")
    if selected.intersection((*CORE, "images", "icons")):
        selected.update((*CORE, "images", "icons"))
    return [name for name in COMPONENTS if name in selected]


def parser_identity() -> dict:
    """The entire shipped Python parser/rule closure, not a guessed version."""
    directory = Path(__file__).resolve().parent
    files = sorted(directory.glob("*.py")) + sorted((directory / "battle_replay").rglob("*.py"))
    rows = {path.relative_to(directory).as_posix(): sha(read_bytes(path)) for path in files}
    return {"sha256": sha(canonical(rows)), "files": rows, "formatSchema": 1}


def seed_workspace(root: Path, baseline: Path, workspace: Path, manifest):
    refs, attachments = {}, {}
    for name, filename in COMPONENTS.items():
        if manifest:
            reference = manifest["components"][name]
            raw = read_object_ref(root, reference)
        else:
            legacy = root / "catalog" / filename
            source = legacy if legacy.is_file() else baseline / filename
            raw = read_bytes(source)
            origin = {"kind": "legacy-catalog" if source == legacy else "embedded-baseline", "file": filename}
            # The original payload retains its own provenance and source age.
            reference = put_object(root, raw, "brpack" if name == "battleReplay" else "json", origin)
        working_raw = raw
        if name == "battleReplay" and reference["format"] == "json":
            from battle_replay.catalog_pack import pack_bytes
            working_raw = pack_bytes(raw)
        atomic_write(workspace / "catalog" / filename, working_raw)
        refs[name] = copy.deepcopy(reference)
    for filename in ATTACHMENTS:
        if manifest and filename in manifest.get("attachments", {}):
            reference = manifest["attachments"][filename]
            raw = read_object_ref(root, reference)
        elif manifest:
            continue
        else:
            legacy = root / "catalog" / filename
            source = legacy if legacy.is_file() else baseline / filename
            if not source.is_file():
                continue
            raw = read_bytes(source)
            reference = put_object(root, raw, "json", {"kind": "legacy-catalog" if source == legacy else "embedded-baseline", "file": filename})
        attachments[filename] = copy.deepcopy(reference)
        atomic_write(workspace / "catalog" / filename, raw)
    return refs, attachments


def _safe_image(root: Path, file: str) -> Path:
    if not isinstance(file, str) or not re.fullmatch(r"images/(?:stargods|attributes|public-icons)/[A-Za-z0-9_-]+\.png", file):
        raise ValueError("Invalid public icon cache path")
    return root / file


def seed_icons(root: Path, workspace: Path):
    index = parse_json(read_bytes(workspace / "catalog" / COMPONENTS["icons"]))
    for section in ("stargods", "attributes"):
        for key, row in index.get(section, {}).items():
            if not isinstance(row, dict) or not re.fullmatch(r"[1-9][0-9]{0,5}", key):
                continue
            file = row.get("file", f"images/{section}/{key}.png")
            source = _safe_image(root, file)
            if not source.is_file():
                continue
            raw = read_bytes(source, 16 * 1024 * 1024)
            if sha(raw) == row.get("png", {}).get("sha256"):
                atomic_write(workspace / f"images/{section}/{key}.png", raw)


def freeze_icons(root: Path, workspace: Path):
    path = workspace / "catalog" / COMPONENTS["icons"]
    index = parse_json(read_bytes(path))
    changed = False
    for section in ("stargods", "attributes"):
        for key, row in index.get(section, {}).items():
            if not isinstance(row, dict) or not re.fullmatch(r"[1-9][0-9]{0,5}", key):
                continue
            expected = row.get("png", {}).get("sha256", "")
            if not isinstance(expected, str) or not SHA.fullmatch(expected):
                continue  # A legacy unproven icon is not promoted to a proven cache.
            source = workspace / f"images/{section}/{key}.png"
            if not source.is_file():
                source = _safe_image(root, row.get("file", f"images/{section}/{key}.png"))
            if not source.is_file():
                continue
            raw = read_bytes(source, 16 * 1024 * 1024)
            if sha(raw) != expected:
                raise ValueError("Public icon content does not match its recorded digest")
            file = "images/public-icons/" + expected + ".png"
            immutable_write(root / file, raw)
            if row.get("file") != file:
                row["file"] = file
                changed = True
    if changed:
        atomic_write(path, canonical(index))


def _quality_reports(updater, directory, workspace, published, refs, checked):
    records = {}
    for name in ("pets", "skills", "battleReplay"):
        if name not in checked:
            continue
        metadata = updater.quality_results.get(name)
        if not metadata or metadata.get("passed") is not True:
            if name in published:
                raise ValueError("Changed core component lacks its independent quality check: " + name)
            continue
        path = Path(metadata.get("reportFile", ""))
        expected_root = (workspace / "data-tools" / "quality").resolve()
        if path.resolve().parent != expected_root:
            raise ValueError("Quality report lies outside this candidate workspace")
        raw = read_bytes(path)
        report = parse_json(raw)
        payload = read_bytes(workspace / "catalog" / COMPONENTS[name])
        content = sha(canonical(validate_payload(payload, refs[name]["format"])))
        if sha(raw) != metadata.get("reportSha256") or report.get("passed") is not True or report.get("candidateContentSha256") != content:
            raise ValueError("Independent quality report is not bound to the actual candidate")
        relative = "quality/" + name + ".json"
        atomic_write(directory / relative, raw)
        records[name] = {"file": relative, "sha256": sha(raw), "bytes": len(raw), "candidateContentSha256": content}
    return records


def _preserve_evidence(root: Path, directory: Path, quality: dict) -> list[dict]:
    """Pin the exact files independently read by fidelity checks, not self-proof."""
    cache = root / "public/source-cache/evidence"
    existing = list(cache.glob("*/data"))
    total = sum(path.stat().st_size for path in existing if path.is_file())
    count = len(existing)
    rows = []
    for component, reference in quality.items():
        report = parse_json(read_bytes(directory / reference["file"]))
        inputs = report.get("sourceFidelity", {}).get("inputs", {})
        entries = list(inputs.items()) if isinstance(inputs, dict) else [(str(i), value) for i, value in enumerate(inputs)]
        for key, record in entries:
            if not isinstance(record, dict) or not isinstance(record.get("path"), str):
                raise ValueError("Independent fidelity input lacks its actual path")
            expected = record.get("sha256", "")
            if not isinstance(expected, str) or not SHA.fullmatch(expected) or type(record.get("bytes")) is not int:
                raise ValueError("Independent fidelity input lacks its byte identity")
            source = Path(record["path"])
            raw = read_bytes(source)
            if sha(raw) != expected or len(raw) != record["bytes"]:
                raise ValueError("Independent fidelity input changed after it was checked")
            destination = cache / expected / "data"
            if not destination.exists():
                if total + len(raw) > 512 * 1024 * 1024 or count >= 8192:
                    raise ValueError("独立原文证据缓存已到保留上限；本次候选未发布，需在关闭工作台后清理无引用证据")
                total += len(raw)
                count += 1
            immutable_write(destination, raw)
            rows.append({"component": component, "role": record.get("role", key),
                         "originalPath": record["path"], "file": destination.relative_to(root).as_posix(),
                         "sha256": expected, "bytes": len(raw)})
    return rows


def _preserve_diagnostics(directory: Path, workspace: Path):
    """Keep bounded reports, never retain a duplicate downloaded replay tree."""
    audit = {}
    names = ("official-versions.json", "public-resource-manifest.json", "public-update-status.json")
    for name in names:
        path = workspace / "catalog" / name
        if path.is_file():
            raw = read_bytes(path)
            atomic_write(directory / "diagnostics" / name, raw)
            audit[name] = {"file": "diagnostics/" + name, "sha256": sha(raw), "bytes": len(raw)}
    for name in ("pets", "skills", "battleReplay"):
        path = workspace / "data-tools/quality" / (name + "-latest-attempt.json")
        if path.is_file():
            raw = read_bytes(path)
            atomic_write(directory / "quality" / (name + ".json"), raw)
    return audit


def _cleanup_workspace(root: Path, attempt: str, workspace: Path):
    if not ATTEMPT.fullmatch(attempt):
        raise ValueError("Invalid transaction cleanup identity")
    expected_parent = (root / "public/transactions" / attempt).resolve()
    resolved = workspace.resolve()
    if not expected_parent.is_relative_to(root.resolve()) or resolved.parent != expected_parent or resolved.name != "workspace" or workspace.is_symlink():
        raise ValueError("Transaction workspace cleanup escaped its owned directory")
    if workspace.exists():
        shutil.rmtree(workspace)


def begin_request(root: Path, requested, attempt=None):
    """Bind preparation (including a failed bootstrap) to one actual request."""
    attempt = attempt or uuid.uuid4().hex
    if not isinstance(attempt, str) or not ATTEMPT.fullmatch(attempt):
        raise ValueError("Invalid public update attempt")
    requested = [name for name in COMPONENTS if requested is None or name in requested]
    expanded_components(requested)
    directory = root / 'public/transactions' / attempt
    if not directory.resolve().is_relative_to(root.resolve()):
        raise ValueError('Public update transaction escaped its data root')
    directory.mkdir(parents=True, exist_ok=True)
    request_file = directory / 'request.json'
    if request_file.exists():
        request = parse_json(read_bytes(request_file, 65536))
        receipt_file = directory / 'receipt.json'
        prior = parse_json(read_bytes(receipt_file, 65536)) if receipt_file.exists() else {}
        if (request.get('schema') != 1 or request.get('attempt') != attempt or
                not isinstance(request.get('createdUtc'), str) or
                set(request.get('requestedComponents', [])) != set(requested)
                or prior.get('diskCommitted') or prior.get('checked') or prior.get('state') == 'cancelled'
                or (directory / 'candidate.json').exists() or (directory / 'validation.json').exists()):
            raise ValueError('Public update attempt is already used or does not match its request')
        try:
            if datetime.fromisoformat(request['createdUtc'].replace('Z', '+00:00')).utcoffset() is None:
                raise ValueError('Public update request time must include its time zone')
        except ValueError as error:
            raise ValueError('Invalid public update request time') from error
    else:
        if any(directory.iterdir()):
            raise ValueError('Public update transaction already exists without a matching request')
        request = {'schema': 1, 'attempt': attempt, 'requestedComponents': requested,
                   'createdUtc': datetime.now(timezone.utc).isoformat()}
        atomic_write(request_file, canonical(request))
    if not (directory / 'receipt.json').exists():
        atomic_write(directory / 'receipt.json', canonical({**request, 'checked': False, 'state': 'preparing',
            'candidatePassed': False, 'diskCommitted': False, 'sessionApplied': False}))
    atomic_write(root / 'public/reports/latest-request.json', canonical(request))
    return request


def record_preparation_failure(root, attempt, error):
    if not isinstance(attempt, str) or not ATTEMPT.fullmatch(attempt):
        return
    path = root / 'public/transactions' / attempt / 'receipt.json'
    if not path.is_file():
        return
    receipt = parse_json(read_bytes(path))
    if receipt.get('attempt') != attempt or receipt.get('diskCommitted'):
        return
    receipt.update(state='failed', error=str(error), checkedUtc=datetime.now(timezone.utc).isoformat())
    atomic_write(path, canonical(receipt))


# These are existing catalog namespaces, not names inferred from numeric IDs.
_CHANGE_TABLES = {
    'pets': {'pets': '精灵资料', **{key: '养成资料' for key in (
        'stargods', 'badges', 'sacredEquipment', 'astrolabe', 'items', 'attributes', 'jobs',
        'money', 'sourceBeasts', 'legendStones', 'proficiencies', 'fusionJobs', 'sacredStagePlans', 'sacredStarPlans')}},
    'skills': {key: '技能与效果' for key in ('pets', 'skills', 'entries', 'buffs', 'transformSkills',
        'combos', 'heroSkills', 'psychicsSkills', 'lingchuRules', 'shenyunRules', 'almightySkills')},
    'battleReplay': {key: '技能与效果' for key in ('pets', 'skills', 'buffs', 'officialEntries',
        'officialEntryMentions', 'transformSkills', 'combos', 'petMechanics', 'attributes')},
    'images': {'faces': '精灵图片', 'images': '精灵图片'},
    'icons': {'stargods': '星神图标', 'attributes': '属性图标'},
    'routines': {'tasks': '日常任务', 'activities': '活动入口'},
    'shop': {'shops': '兑换活动'},
}
_TEXT_FIELDS = ('description', 'descX', 'desc', 'mess', 'beffect', 'tip', 'descriptionTemplate',
                'autoEffectDescription', 'reasonLabel')
_NAME_FIELDS = ('name', 'title', 'nm', 'label', 'heroSkillName', 'summonSkillName')
_CHANGE_METADATA = {'source', 'sources', 'raw', 'sourceEvidence', 'evidence', 'generatedAt', 'checkedUtc',
                    'rawFields', 'sourcePath', 'file', 'pngObject', 'swfSha256', 'swfBytes'}


def _change_rows(component, table, data):
    values = data.get(table, {})
    if isinstance(values, dict):
        return {str(key): value for key, value in values.items()}
    if not isinstance(values, list):
        return {}
    rows = {}
    for row in values:
        if not isinstance(row, dict):
            continue
        if component == 'routines':
            key = row.get('id' if table == 'tasks' else 'key')
        elif component == 'shop':
            key = canonical([row.get('sourceKey'), row.get('shopId')]).decode('utf-8')
        elif component == 'battleReplay' and table == 'combos':
            key = canonical([row.get('skillId'), row.get('buffs')]).decode('utf-8')
        elif component == 'pets' and table == 'fusionJobs':
            key = canonical(row.get('jobs')).decode('utf-8') if isinstance(row.get('jobs'), list) else None
        else:
            key = row.get('id')
        if key is None or str(key) in rows:
            # No positional matching: ambiguous rows stay in the raw catalog.
            raise ValueError('Change-summary record identity is missing or duplicated')
        rows[str(key)] = row
    return rows


def _change_name(component, table, key, row, data):
    if not isinstance(row, dict):
        return key if table in ('entries', 'officialEntries', 'officialEntryMentions') and not key.isdecimal() else ''
    if table in ('pets', 'petMechanics') and component == 'battleReplay':
        if table == 'petMechanics':
            row = data.get('pets', {}).get(key, {})
        identity = row.get('identity', {})
        return identity.get('canonicalName', '') if identity.get('status') in ('native', 'skin') else ''
    if table == 'pets' and component == 'pets':
        seen = {key}
        while row.get('groupRaceId') not in (None, '', 0, '0', -1, '-1'):
            parent = str(row['groupRaceId'])
            if parent in seen or not isinstance(data.get(table, {}).get(parent), dict):
                return ''
            seen.add(parent)
            row = data[table][parent]
    name = next((row[field] for field in _NAME_FIELDS if isinstance(row.get(field), str) and row[field].strip()), '')
    if not name and table in ('entries', 'officialEntries', 'officialEntryMentions') and not key.isdecimal():
        return key
    return name


def _change_text(row):
    if isinstance(row, str):
        return row
    if not isinstance(row, dict):
        return ''
    texts = []
    for field in _TEXT_FIELDS:
        if field == 'desc' and row.get('descX'):
            continue
        value = row.get(field)
        if isinstance(value, str) and value and value not in texts:
            texts.append(value)
    return '\n'.join(texts)


def build_change_report(root, before, after, *, attempt, base_revision, revision, limit=500):
    """Readable projection only; exact original objects remain in immutable storage.

    Missing official descriptions stay empty. Metadata-only changes are counted
    separately rather than falsely described as a new skill/buff effect.
    """
    rows, total, technical_only, changed_components, unprojected = [], 0, 0, [], []
    remaining_bytes = 8 * 1024 * 1024
    pet_definitions = {}
    def named(component, table, key, row, data, side):
        # These are the same exact joins used by update_icons, Updater.images
        # and the skill catalog; no fuzzy-name or cross-namespace lookup.
        linked = table if component == 'icons' else 'pets' if component == 'images' or component == 'skills' and table == 'pets' else ''
        identity = key.split('_', 1)[0] if component == 'images' and table == 'images' else key
        if linked:
            if side not in pet_definitions:
                pet_definitions[side] = read_object_payload(root, (before if side == 'before' else after)['pets'])
            metadata = pet_definitions[side]
            definition = metadata.get(linked, {}).get(identity)
            if definition is not None:
                if isinstance(definition, str):
                    return definition
                return _change_name('pets', linked, identity, definition, metadata)
        return _change_name(component, table, key, row, data)
    for component in COMPONENTS:
        if before[component]['sha256'] == after[component]['sha256']:
            continue
        changed_components.append(component)
        old = read_object_payload(root, before[component])
        new = read_object_payload(root, after[component])
        for table in sorted((old.keys() | new.keys()) - _CHANGE_TABLES.get(component, {}).keys()):
            if old.get(table) == new.get(table) and (table in old) == (table in new):
                continue
            technical_only += 1
            unprojected.append({'component': component, 'table': table,
                'beforeObjectSha256': before[component]['sha256'], 'afterObjectSha256': after[component]['sha256'],
                'reason': 'original-record-retained; no additional user-facing semantics inferred'})
        for table, category in _CHANGE_TABLES.get(component, {}).items():
            old_rows, new_rows = _change_rows(component, table, old), _change_rows(component, table, new)
            for key in sorted(old_rows.keys() | new_rows.keys()):
                previous, current = old_rows.get(key), new_rows.get(key)
                if previous == current and (key in old_rows) == (key in new_rows):
                    continue
                prior_name = named(component, table, key, previous, old, 'before') if key in old_rows else ''
                new_name = named(component, table, key, current, new, 'after') if key in new_rows else ''
                kind = 'added' if key not in old_rows else 'removed' if key not in new_rows else 'changed'
                fields = sorted(set(previous or {}) | set(current or {})) if isinstance(previous, dict) and isinstance(current, dict) else []
                fields = [field for field in fields if previous.get(field) != current.get(field)]
                meaningful = [field for field in fields if field not in _CHANGE_METADATA]
                if kind == 'changed' and isinstance(current, dict) and not meaningful:
                    technical_only += 1
                    continue
                total += 1
                if len(rows) >= limit:
                    continue
                item = {'component': component, 'category': category, 'kind': kind,
                    'name': new_name or prior_name or category, 'beforeName': prior_name, 'afterName': new_name,
                    'beforeText': _change_text(previous), 'afterText': _change_text(current),
                    'technical': {'table': table, 'id': key, 'changedFields': fields,
                        'beforePresent': key in old_rows, 'afterPresent': key in new_rows,
                        'beforeObjectSha256': before[component]['sha256'], 'afterObjectSha256': after[component]['sha256']}}
                changes = {}
                for field in fields:
                    entry = {'beforePresent': field in previous, 'afterPresent': field in current,
                             'before': previous.get(field), 'after': current.get(field)}
                    raw = canonical(entry)
                    if len(raw) <= 4096:
                        changes[field] = entry
                    else:
                        changes[field] = {'detailsInOriginalObjects': True, 'bytes': len(raw), 'sha256': sha(raw)}
                item['technical']['fields'] = changes
                item_size = len(canonical(item))
                if item_size <= remaining_bytes:
                    rows.append(item)
                    remaining_bytes -= item_size
        del old, new
    return {'schema': 1, 'attempt': attempt, 'baseRevision': base_revision, 'revision': revision,
            'total': total, 'truncated': total > len(rows), 'technicalOnly': technical_only,
            'changedComponents': changed_components, 'rows': rows, 'technicalChanges': unprojected}


def prepare_candidate(root: Path, baseline: Path, requested=None, *, updater_factory=None, emit=None, attempt_id=None):
    """Keep all output isolated, preserve failed dependency groups, write receipt."""
    from public_data_updater import Updater
    root, baseline = Path(root).resolve(), Path(baseline).resolve()
    emit = emit or (lambda *args, **kwargs: None)
    if requested is not None and (not set(requested) or not set(requested).issubset(COMPONENTS)):
        raise ValueError("Invalid requested public-data components")
    requested = [name for name in COMPONENTS if requested is None or name in requested]
    effective = expanded_components(requested)
    request = begin_request(root, requested, attempt_id)
    attempt = request['attempt']
    directory = root / "public" / "transactions" / attempt
    workspace = directory / "workspace"
    receipt = {"schema": 1, "attempt": attempt, "checked": False, "candidatePassed": False,
               "diskCommitted": False, "sessionApplied": False, "createdUtc": request['createdUtc'],
               "state": "preparing", "requestedComponents": requested, "effectiveComponents": effective}
    atomic_write(directory / "receipt.json", canonical(receipt))
    started = time.perf_counter()
    timings, active_stage = [], None
    def stage(phase, component='', completed=-1, total=-1):
        nonlocal active_stage
        now = time.perf_counter()
        identity = (phase, component)
        if active_stage and active_stage[:2] != identity:
            timings.append({'phase': active_stage[0], 'component': active_stage[1], 'seconds': now - active_stage[2]})
        if not active_stage or active_stage[:2] != identity:
            active_stage = (*identity, now)
        emit('stage', attempt=attempt, phase=phase, component=component, completed=completed, total=total)
    try:
        stage('preparingTools')
        revision, pointer_sha, previous_manifest = load_current(root)
        references, attachments = seed_workspace(root, baseline, workspace, previous_manifest)
        before = copy.deepcopy(references)
        (workspace / "data-tools").mkdir(parents=True, exist_ok=True)
        scratch = workspace / "scratch"
        scratch.mkdir()
        seed_icons(root, workspace)
        factory = updater_factory or Updater
        updater = factory(workspace, baseline, scratch, isolated_workspace=True, runtime_root=root / "data-tools")
        updater.source_cache = root / "public" / "source-cache"
        parser = parser_identity()
        updater.parser_identity = parser["sha256"]
        updater.progress_callback = stage
        updater.public_resources.progress_callback = lambda completed, total: stage('fetchingResources', '', completed, total)
        updater.force_components = {name for name in effective if references[name].get("origin", {}).get("parserSha256") != parser["sha256"]}
        if effective != requested:
            emit("progress", message="同时同步相关资料：" + "、".join(name for name in effective if name not in requested))
        stage('fetchingResources')
        results = updater.run(set(effective))
        stage('validating')
        if set(results) != set(effective):
            raise ValueError("Candidate update omitted an effective component")
        # An unchanged official version is still freshly downloaded and checked
        # by PublicResourceSession. Never replace this with manifest self-proof.
        updater.public_resources.assert_stable()
        if hasattr(updater.public_resources, "audit"):
            atomic_write(workspace / "catalog/public-resource-manifest.json", canonical({
                **updater.public_resources.audit(), "state": "validated", "selected": effective,
                "components": results, "sourceStable": True}))
        blocked = {name for name in effective if results[name].get("status") == "failed" or results[name].get("dependencyFailure")}
        if blocked.intersection(CORE):
            blocked.update(set(CORE).intersection(effective))
        # A picture index derived from rejected pet definitions cannot be mixed
        # with the retained pet catalog; independent picture failure is one-way.
        if "pets" in blocked:
            blocked.update({"images", "icons"}.intersection(effective))
        published = set()
        for name in effective:
            if name in blocked:
                results[name] = {**results[name], "status": "retained", "retained": True,
                                 "retryable": True, "reason": "dependency-group-rejected", "attemptStatus": results[name].get("status")}
                continue
            if name == "icons":
                freeze_icons(root, workspace)
            raw = read_bytes(workspace / "catalog" / COMPONENTS[name])
            form = "brpack" if name == "battleReplay" else "json"
            validate_payload(raw, form)
            if sha(raw) != before[name]["sha256"]:
                origin = {"kind": "official-candidate", "parserSha256": parser["sha256"], "formatSchema": 1}
                references[name] = put_object(root, raw, form, origin)
                published.add(name)
            else:
                # This successful check established the actual parser identity
                # even when its payload stayed byte-identical. Keep every old
                # provenance field; only the rule identity advances once.
                # Rejected groups never reach this branch.
                references[name]["origin"] = {**references[name].get("origin", {}),
                    "parserSha256": parser["sha256"], "formatSchema": 1}
            results[name]["candidateObjectSha256"] = references[name]["sha256"]
        if "shop" in effective and "shop" not in blocked:
            for filename in ATTACHMENTS:
                path = workspace / "catalog" / filename
                if path.is_file():
                    raw = read_bytes(path)
                    if filename not in attachments or sha(raw) != attachments[filename]["sha256"]:
                        attachments[filename] = put_object(root, raw, "json", {"kind": "official-candidate", "component": "shop", "parserSha256": parser["sha256"]})
        must_validate = published | (set(CORE).intersection(updater.force_components))
        quality = _quality_reports(updater, directory, workspace, must_validate, references, set(effective) - blocked)
        evidence = _preserve_evidence(root, directory, quality)
        diagnostics = _preserve_diagnostics(directory, workspace)
        manifest = {"schema": 1, "dependencySchema": 1, "components": references, "attachments": attachments}
        manifest_raw = canonical(manifest)
        candidate_revision = sha(manifest_raw)
        immutable_write(root / "public" / "releases" / candidate_revision / "manifest.json", manifest_raw)
        changes = build_change_report(root, before, references, attempt=attempt, base_revision=revision, revision=candidate_revision)
        change_raw = canonical(changes)
        atomic_write(directory / 'changes.json', change_raw)
        checker = {"kind": "public-dataset-python", "schema": 1, "sha256": sha(read_bytes(Path(__file__).resolve()))}
        report = {"schema": 1, "attempt": attempt, "baseRevision": revision, "baseCurrentSha256": pointer_sha,
                  "revision": candidate_revision, "components": references, "attachments": attachments,
                  "requestedComponents": requested, "effectiveComponents": effective,
                  "dependencyGroups": DEPENDENCY_GROUPS, "results": results,
                  "checks": {"objects": True, "dependencies": True, "sourceStable": True},
                  "qualityReports": quality, "checker": checker, "parserIdentity": parser,
                  "evidenceFiles": evidence,
                  "changeReport": {"file": "changes.json", "sha256": sha(change_raw), "bytes": len(change_raw)},
                  "diagnostics": diagnostics,
                  "changedComponents": [name for name in COMPONENTS if name in published], "passed": True}
        raw_report = canonical(report)
        atomic_write(directory / "validation.json", raw_report)
        candidate = {"schema": 1, "attempt": attempt, "baseRevision": revision, "baseCurrentSha256": pointer_sha,
                     "revision": candidate_revision, "requestedComponents": requested, "effectiveComponents": effective,
                     "results": results, "validation": {"passed": True, "manifestSha256": candidate_revision,
                         "reportSha256": sha(raw_report), "checker": checker}}
        atomic_write(directory / "candidate.json", canonical(candidate))
        receipt.update(checked=True, candidatePassed=True, revision=candidate_revision, results=results, state='candidatePassed')
        if active_stage:
            timings.append({'phase': active_stage[0], 'component': active_stage[1], 'seconds': time.perf_counter() - active_stage[2]})
        receipt['timings'] = {'wallSeconds': time.perf_counter() - started, 'stages': timings}
        receipt['checkedUtc'] = datetime.now(timezone.utc).isoformat()
        atomic_write(directory / "receipt.json", canonical(receipt))
        # No claimed publication here, including when candidate == current.
        emit("candidate", attempt=attempt, revision=candidate_revision)
        return candidate
    except Exception as error:
        try:
            _preserve_diagnostics(directory, workspace)
        except (OSError, ValueError):
            receipt["diagnosticsIncomplete"] = True
        receipt.update(error=str(error), checked=True, state='failed', checkedUtc=datetime.now(timezone.utc).isoformat())
        atomic_write(directory / "receipt.json", canonical(receipt))
        raise
    finally:
        try:
            _cleanup_workspace(root, attempt, workspace)
        except (OSError, ValueError):
            receipt["cleanupPending"] = True
            atomic_write(directory / "receipt.json", canonical(receipt))


def image_index(root: Path, digest=None):
    if digest is not None:
        if not isinstance(digest, str) or not SHA.fullmatch(digest):
            raise ValueError("Invalid fixed image-index object digest")
        raw = read_bytes(root / "public" / "objects" / digest / "data")
        if sha(raw) != digest:
            raise ValueError("Fixed image-index object digest does not match")
        return parse_json(raw)
    if (root / "public" / "current.json").exists():
        _, _, manifest = load_current(root, validate_objects=False)
        return read_object_payload(root, manifest["components"]["images"])
    return parse_json(read_bytes(root / "catalog" / COMPONENTS["images"]))


def _native_command(root: Path, arguments: list[str], *, runner=subprocess.run):
    helper = root / "data-tools" / "scripts" / "KQPublicDatasetCheck.exe"
    if not helper.is_file():
        raise ValueError("原生公共资料发布工具缺失，资料尚未应用；请使用完整新版工作台")
    result = runner([str(helper), *arguments],
                    cwd=helper.parent, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    timeout=180, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        record = parse_json(result.stdout)
    except (ValueError, UnicodeError):
        record = {}
    if result.returncode:
        message = record.get('error') or result.stderr.decode("utf-8", errors="replace")[-2048:]
        raise ValueError("原生候选检查或发布未通过：" + str(message))
    return record


def ensure_migrated(root: Path, *, runner=subprocess.run):
    """The standalone entry uses the same baseline trial-read as the app."""
    if (root / "public/current.json").exists():
        return
    record = _native_command(root, ["--migrate", str(root)], runner=runner)
    revision, _, _ = load_current(root)
    if not revision or record.get("revision") != revision or record.get("status") not in ("ready", "diskCommitted"):
        raise ValueError("原生公共资料迁移没有生成一致可读的基线")


def apply_candidate(root: Path, candidate: dict, *, runner=subprocess.run):
    record = _native_command(root, ["--apply-candidate", str(root), candidate["attempt"]], runner=runner)
    if record.get("status") not in ("diskCommitted", "unchanged") or record.get("revision") != candidate["revision"]:
        raise ValueError("原生发布工具返回的资料版本不匹配")
    return record
