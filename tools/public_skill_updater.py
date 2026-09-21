#!/usr/bin/env python3
"""Update the local pet-skill catalog from official AoQi H5 resources."""

from __future__ import annotations

import hashlib
import io
import json
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

from generate_pet_skill_data import build_catalog


H5 = "https://aoqi.100bt.com/h5/"
ARCHIVES = (
    "config/battleconfig.aqz", "config/formation.aqz", "config/petdictionarydata.json.aqz",
    "config/petdata.json.aqz", "config/relation.aqz", "config/changepetskin.aqz",
    "config/equipment4pet.aqz",
)
DIRECT = ("config/pet/petinfoconfig.json", "config/pet/petevoconfig.json", "config/pet/tiebaconfig.json")


def fetch_bytes(path: str, limit: int = 16 * 1024 * 1024) -> bytes:
    url = urllib.parse.urljoin(H5, path)
    request = urllib.request.Request(url, headers={"User-Agent": "KQPetPublicData/2.0"})
    total = bytearray()
    with urllib.request.urlopen(request, timeout=60) as response:
        final = urllib.parse.urlsplit(response.url)
        if final.scheme != "https" or final.hostname != "aoqi.100bt.com":
            raise ValueError("skill resource redirected outside the official HTTPS origin")
        while chunk := response.read(128 * 1024):
            total.extend(chunk)
            if len(total) > limit:
                raise ValueError("skill resource exceeds the download limit")
    return bytes(total)


def versioned(path: str, version: str) -> str:
    head, dot, suffix = path.rpartition(".")
    return f"{head}~{version}.{suffix}" if dot else f"{path}~{version}"


def safe_extract(data: bytes, destination: Path) -> None:
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        for item in package.infolist():
            name = PurePosixPath(item.filename)
            if name.is_absolute() or ".." in name.parts or item.file_size > 16 * 1024 * 1024:
                raise ValueError("unsafe official skill archive")
            target = destination.joinpath(*name.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            if not item.is_dir():
                target.write_bytes(package.read(item))


def update_skills(updater, writer, _versions=None) -> bool:
    start = json.loads(fetch_bytes(f"start~{int(time.time() * 1000)}.json", 2 * 1024 * 1024))
    release = str(start.get("version", ""))
    if not release.isdecimal():
        raise ValueError("official H5 skill version is invalid")
    current = updater.current("skills")
    if current.get("schema") == 1 and current.get("source", {}).get("version") == release \
            and len(current.get("pets", {})) >= 9000 and len(current.get("skills", {})) >= 20000:
        target = updater.catalog / "pet-skill-data.json"
        if not target.is_file():
            writer(target, current)
        return False

    manifest = json.loads(fetch_bytes(f"version~{release}.json", 4 * 1024 * 1024))
    if not isinstance(manifest, dict):
        raise ValueError("official H5 version manifest is invalid")
    source_root = updater.scratch / "skill-h5"
    resources = {}
    for path in ARCHIVES:
        revision = str(manifest.get(path, ""))
        if not revision.isdecimal():
            raise ValueError("official H5 skill archive is missing: " + path)
        data = fetch_bytes(versioned(path, revision))
        folder = Path(path).name.removesuffix(".aqz")
        safe_extract(data, source_root / folder)
        resources[path] = {"version": revision, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    for path in DIRECT:
        revision = str(start.get("files", {}).get(path) or manifest.get(path, ""))
        if not revision.isdecimal():
            raise ValueError("official H5 skill sheet is missing: " + path)
        data = fetch_bytes(versioned(path, revision))
        target = source_root / path.removeprefix("config/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        resources[path] = {"version": revision, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}

    catalog = build_catalog(source_root, {"kind": "aoqi-official-h5-skill-data", "version": release,
                                          "resources": resources})
    if len(catalog.get("pets", {})) < 9000 or len(catalog.get("skills", {})) < 20000 \
            or len(catalog.get("entries", {})) < 100:
        raise ValueError("official skill catalog is incomplete; the previous cache was kept")
    writer(updater.catalog / "pet-skill-data.json", catalog)
    return catalog != current
