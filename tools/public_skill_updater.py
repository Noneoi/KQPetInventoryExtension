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
    "config/equipment4pet.aqz", "config/multipleformpet.aqz",
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
    from public_update_support import child
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        entries = package.infolist()
        if len(entries) > 5000 or sum(item.file_size for item in entries) > 96 * 1024 * 1024:
            raise ValueError('official skill archive exceeds extraction limit')
        seen = set()
        for item in entries:
            name = PurePosixPath(item.filename.replace('\\', '/'))
            if name.is_absolute() or ".." in name.parts or item.file_size > 16 * 1024 * 1024 or name.as_posix().casefold() in seen:
                raise ValueError("unsafe official skill archive")
            seen.add(name.as_posix().casefold())
            target = child(destination, name.as_posix())
            target.parent.mkdir(parents=True, exist_ok=True)
            if not item.is_dir():
                target.write_bytes(package.read(item))


def update_skills(updater, writer, _versions=None) -> bool:
    from concurrent.futures import ThreadPoolExecutor
    from public_update_support import PublicSources, canonical, sha, file_sha
    sources = getattr(updater, 'sources', None)
    if sources is None:
        def transport(url, output):
            output.write_bytes(fetch_bytes(url[len(H5):]))
        sources = PublicSources(updater.scratch, transport)
    chain = sources.h5()
    release = chain['version']
    current = updater.current('skills')
    paths = (*ARCHIVES, *DIRECT)
    with ThreadPoolExecutor(max_workers=4) as pool:
        fetched = list(pool.map(sources.h5_resource, paths))
    resources = {name: record for name, (_, record) in zip(paths, fetched)}
    rules = {name: file_sha(Path(__file__).with_name(name)) for name in
             ('public_skill_updater.py', 'generate_pet_skill_data.py', 'public_pet_relations.py')}
    fingerprint = sha(canonical({'resources': resources, 'rules': rules, 'schema': 2}))
    content = {key: value for key, value in current.items() if key != 'source'}
    previous_source = current.get('source', {})
    if current.get('schema') == 1 and previous_source.get('fingerprint') == fingerprint and \
            previous_source.get('contentSha256') == sha(canonical(content)) and \
            len(current.get('pets', {})) >= 9000 and len(current.get('skills', {})) >= 20000:
        target = updater.catalog / 'pet-skill-data.json'
        if not target.is_file():
            writer(target, current)
        return False
    source_root = updater.scratch / 'skill-h5'
    for name, (path, _) in zip(paths, fetched):
        sources.operations.check()
        with sources.operations.measure('unpack', component='skills', resource=name):
            if name in ARCHIVES:
                safe_extract(path.read_bytes(), source_root / Path(name).name.removesuffix('.aqz'))
            else:
                target = source_root / name.removeprefix('config/')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(path.read_bytes())
    catalog = build_catalog(source_root, {'kind': 'aoqi-official-h5-skill-data', 'version': release,
                                         'resources': resources, 'fingerprint': fingerprint, 'rules': rules})
    if len(catalog.get('pets', {})) < 9000 or len(catalog.get('skills', {})) < 20000 or len(catalog.get('entries', {})) < 100:
        raise ValueError('官方技能资料不完整，现用资料保留')
    relations = catalog.get('relationRules', {})
    if relations.get('schema') != 1 or any(relations.get(group, {}).get('available') is not True
                                           for group in ('summon', 'carry')):
        raise ValueError('官方召唤或携同关系配置不完整，现用资料保留')
    catalog['source']['contentSha256'] = sha(canonical({key: value for key, value in catalog.items() if key != 'source'}))
    writer(updater.catalog / 'pet-skill-data.json', catalog)
    return catalog != current
