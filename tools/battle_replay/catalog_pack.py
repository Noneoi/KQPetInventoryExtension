"""Build a local, byte-preserving battle-replay catalog package. Never downloads.

KQBRPK1 envelope: magic[8], JSON length[u64 BE], compressed length[u64 BE],
SHA-256 of the original JSON[32], then exactly one RFC 1950 zlib stream.
unpack_bytes validates the envelope, bounded expansion and digest; callers that
consume arbitrary packages must also validate_catalog_bytes on the result.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
from urllib.parse import urlsplit
import zlib


MAGIC = b"KQBRPK1\n"
HEADER = struct.Struct(">8sQQ32s")
MAX_JSON_BYTES = 128 * 1024 * 1024
MAX_PACK_BYTES = 32 * 1024 * 1024
MAX_JSON_DEPTH = 64
MAX_JSON_NODES = 6_000_000
MAX_STRING_UNITS = 96 * 1024 * 1024
REQUIRED_OBJECTS = (
    "provenance", "decompiledSourceSha256", "skills", "buffs", "skillLastEffects",
    "buffPowerFullMap", "relationsBySkill", "relationsByBuff", "officialEntries",
    "officialEntryMentions", "entryMentionsByBuff", "entryMentionsBySkill", "petMechanics",
    "mechanismRules", "legendMechanics", "huanMechanics", "petMechanicsSummary",
    "referenceConsumers", "petIdentitySummary", "clientPresentation", "petDragons",
    "replaySupplements", "replayCultivation", "tableJoinSummary", "pets", "transformSkills",
    "attributes", "execution", "bufferExecution", "referenceCoverage",
)
REQUIRED_ARRAYS = ("tableRelations", "combos")
CORE_TABLES = ("pets", "skills", "buffs", "attributes", "execution", "bufferExecution")
SHA256 = re.compile(r"[0-9a-fA-F]{64}\Z")


class CatalogPackError(ValueError):
    """An invalid source/package or an unsafe output request."""


def _checked_json_size(size: int) -> None:
    if not 0 < size <= MAX_JSON_BYTES:
        raise CatalogPackError("JSON byte length must be positive and at most 128 MiB")


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise CatalogPackError("duplicate JSON object key: " + key[:80])
        result[key] = value
    return result


def _finite_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise CatalogPackError("non-finite JSON number")
    return number


def _invalid_constant(value: str) -> None:
    raise CatalogPackError("non-JSON numeric constant: " + value)


def _check_tree(root: dict) -> None:
    nodes = string_units = 0

    def add_string(value: str) -> None:
        nonlocal string_units
        # QString uses UTF-16 units, including surrogate pairs for non-BMP text.
        string_units += len(value.encode("utf-16-le", errors="surrogatepass")) // 2
        if string_units > MAX_STRING_UNITS:
            raise CatalogPackError("catalog exceeds its UTF-16 string budget")

    def visit(value: object, depth: int) -> None:
        nonlocal nodes
        nodes += 1
        if depth > MAX_JSON_DEPTH or nodes > MAX_JSON_NODES:
            raise CatalogPackError("catalog exceeds its JSON depth/node budget")
        if isinstance(value, str):
            add_string(value)
        elif isinstance(value, dict):
            for key, child in value.items():
                add_string(key)
                visit(child, depth + 1)
        elif isinstance(value, list):
            for child in value:
                visit(child, depth + 1)

    visit(root, 0)


def _strings(row: dict, *keys: str) -> bool:
    return all(isinstance(row.get(key), str) for key in keys)


def _number_equals(value: object, expected: int) -> bool:
    return type(value) in (int, float) and value == expected


def _valid_row(table: str, key: str, row: dict) -> bool:
    if table == "pets":
        identity = row.get("identity")
        return (_strings(row, "name") and isinstance(identity, dict)
                and _strings(identity, "status", "canonicalName")
                and isinstance(row.get("nativeAttributes"), list)
                and type(row.get("maxPower")) in (int, float))
    id_key = "code" if table == "execution" else "bufferId" if table == "bufferExecution" else "id"
    if row.get(id_key) != key:
        return False
    if table == "skills":
        return (_strings(row, "name", "description", "configResolution")
                and isinstance(row.get("textSource"), dict)
                and isinstance(row.get("playbackAvailable"), bool))
    if table == "buffs":
        return _strings(row, "did", "nm", "df", "once", "pos", "beffect", "bhs", "mess", "nameStatus", "configSource")
    if table == "attributes":
        return _strings(row, "nm", "pri", "rcs")
    if table == "execution":
        return _strings(row, "nm", "eid", "type")
    return _strings(row, "actionResId", "skillId", "lightedSkillId", "desc")


def validate_catalog_bytes(raw: bytes) -> dict:
    """Check the supported format-3 structure, not source authenticity/mechanics.

    Empty official names/descriptions are legitimate. The original bytes,
    whitespace, key order and optional UTF-8 BOM are never normalized.
    """
    _checked_json_size(len(raw))
    try:
        root = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_unique_object,
                          parse_float=_finite_float, parse_constant=_invalid_constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise CatalogPackError("invalid UTF-8 JSON: " + str(error)) from error
    if not isinstance(root, dict) or not _number_equals(root.get("format"), 3):
        raise CatalogPackError("catalog requires format 3 and an object root")
    _check_tree(root)
    for key in REQUIRED_OBJECTS:
        if not isinstance(root.get(key), dict):
            raise CatalogPackError("missing catalog object: " + key)
    for key in REQUIRED_ARRAYS:
        if not isinstance(root.get(key), list):
            raise CatalogPackError("missing catalog array: " + key)
    for table in CORE_TABLES:
        rows = root[table]
        if not rows:
            raise CatalogPackError("empty catalog table: " + table)
        for key, row in rows.items():
            if not key or not isinstance(row, dict) or not _valid_row(table, key, row):
                raise CatalogPackError("invalid catalog row: " + table + "/" + key[:80])
    if not _number_equals(root["clientPresentation"].get("schemaVersion"), 1):
        raise CatalogPackError("unsupported clientPresentation schema")
    summary = root["tableJoinSummary"]
    skills, buffs = summary.get("skills"), summary.get("buffs")
    if not isinstance(skills, dict) or not isinstance(buffs, dict) or not all((
        _number_equals(skills.get("skills"), len(root["skills"])),
        _number_equals(buffs.get("canonicalBuffIds"), len(root["buffs"])),
        _number_equals(buffs.get("flashBufferExecuteRows"), len(root["bufferExecution"])),
        _number_equals(buffs.get("flashComboRows"), len(root["combos"])),
        _number_equals(root["petMechanicsSummary"].get("pets"), len(root["pets"])),
    )):
        raise CatalogPackError("catalog summary counts disagree with its tables")
    provenance = root["provenance"]
    versiondata = provenance.get("versiondata")
    packages = provenance.get("packages")
    if (not isinstance(provenance.get("manifestSha256"), str)
            or not SHA256.fullmatch(provenance["manifestSha256"])
            or not isinstance(versiondata, dict) or not _strings(versiondata, "version")
            or not versiondata["version"] or not isinstance(packages, list)):
        raise CatalogPackError("missing catalog source version/hash records")
    resources = set()
    for package in packages:
        if not isinstance(package, dict) or not _strings(package, "resource", "version", "url", "sha256"):
            raise CatalogPackError("invalid package source record")
        try:
            url = urlsplit(package["url"])
        except ValueError as error:
            raise CatalogPackError("invalid package source URL") from error
        resource = package["resource"]
        if (not resource or resource in resources or not package["version"]
                or not SHA256.fullmatch(package["sha256"]) or url.scheme != "https"
                or url.hostname != "aoqi.100bt.com" or not url.path.startswith("/play/")):
            raise CatalogPackError("incomplete or duplicate official package source record")
        resources.add(resource)
    if not {"library/interfaces", "battle/battle", "battleconfig/battleconfig"} <= resources:
        raise CatalogPackError("missing required replay package source records")
    return root


def unpack_bytes(packed: bytes) -> bytes:
    """Verify one bounded RFC 1950 stream and return the exact original bytes.

    No unbounded decompress/flush call is used. The declared JSON length plus
    one byte detects underdeclared expansion before allocating excess output.
    """
    if not HEADER.size < len(packed) <= MAX_PACK_BYTES:
        raise CatalogPackError("package size is invalid or exceeds 32 MiB")
    magic, json_size, compressed_size, expected_hash = HEADER.unpack_from(packed)
    if magic != MAGIC:
        raise CatalogPackError("unsupported catalog package magic")
    _checked_json_size(json_size)
    if compressed_size == 0 or compressed_size != len(packed) - HEADER.size:
        raise CatalogPackError("compressed byte length does not match the package")
    decoder = zlib.decompressobj(zlib.MAX_WBITS)
    try:
        raw = decoder.decompress(memoryview(packed)[HEADER.size:], json_size + 1)
    except zlib.error as error:
        raise CatalogPackError("invalid RFC 1950 zlib stream: " + str(error)) from error
    if len(raw) > json_size or decoder.unconsumed_tail:
        raise CatalogPackError("zlib expansion exceeds the declared JSON length")
    if not decoder.eof:
        raise CatalogPackError("truncated zlib stream")
    if decoder.unused_data:
        raise CatalogPackError("trailing bytes or multiple zlib streams are forbidden")
    if len(raw) != json_size:
        raise CatalogPackError("expanded byte length does not match the header")
    if hashlib.sha256(raw).digest() != expected_hash:
        raise CatalogPackError("JSON SHA-256 does not match the header")
    return raw


def pack_bytes(raw: bytes) -> bytes:
    validate_catalog_bytes(raw)
    compressed = zlib.compress(raw, level=9)
    if HEADER.size + len(compressed) > MAX_PACK_BYTES:
        raise CatalogPackError("compressed catalog package exceeds 32 MiB")
    packed = HEADER.pack(MAGIC, len(raw), len(compressed), hashlib.sha256(raw).digest()) + compressed
    if unpack_bytes(packed) != raw:
        raise CatalogPackError("package round-trip changed the original JSON bytes")
    return packed


def _different_files(source: Path, target: Path) -> None:
    if source == target or (target.exists() and os.path.samefile(source, target)):
        raise CatalogPackError("output must not be the input file or an alias of it")


def pack_file(input_path: Path, output_path: Path) -> dict:
    """Validate/read back before atomically replacing the explicit destination."""
    source = Path(input_path).expanduser().resolve(strict=True)
    target = Path(output_path).expanduser().resolve(strict=False)
    _different_files(source, target)
    if not source.is_file():
        raise CatalogPackError("input must be a regular JSON file")
    with source.open("rb") as stream:
        before = os.fstat(stream.fileno())
        _checked_json_size(before.st_size)
        raw = stream.read(MAX_JSON_BYTES + 1)
        after = os.fstat(stream.fileno())
    _checked_json_size(len(raw))
    if (len(raw) != before.st_size or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns):
        raise CatalogPackError("input changed while being read")
    packed = pack_bytes(raw)
    result = {"format": "KQBRPK1", "json_bytes": len(raw), "pack_bytes": len(packed),
              "sha256": hashlib.sha256(raw).hexdigest()}
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=target.name + ".", suffix=".tmp", dir=target.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            if stream.write(packed) != len(packed):
                raise CatalogPackError("incomplete temporary package write")
            stream.flush()
            os.fsync(stream.fileno())
        with temporary.open("rb") as stream:
            staged = stream.read(MAX_PACK_BYTES + 1)
        if staged != packed or unpack_bytes(staged) != raw:
            raise CatalogPackError("temporary package failed byte-preserving readback")
        _different_files(source, target)
        os.replace(temporary, target)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="existing format-3 JSON; kept byte-for-byte")
    parser.add_argument("--output", type=Path, required=True, help="explicit .brpack destination; no default workbench path")
    args = parser.parse_args(argv)
    try:
        result = pack_file(args.input, args.output)
    except (CatalogPackError, OSError, RuntimeError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=True), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
