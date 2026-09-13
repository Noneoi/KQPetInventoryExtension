#!/usr/bin/env python3
"""Refresh pet identities from the current official AoQi base + update dictionary.

Other sheets in --catalog retain their existing data. Java/JPEXS locations are
explicit inputs, so this is reproducible without a developer's unpack directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

sys.dont_write_bytecode = True
from generate_pet_detail_data import merge_pet_sources, swf_text

BASE = "https://aoqi.100bt.com/play/"


def download(url: str, path: Path) -> bytes:
    if not url.startswith(BASE):
        raise ValueError("only official AoQi resources are supported")
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(32 * 1024 * 1024 + 1)
    if len(data) > 32 * 1024 * 1024:
        raise ValueError("official resource exceeds 32 MiB")
    path.write_bytes(data)
    return data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--java", type=Path, required=True)
    parser.add_argument("--ffdec", type=Path, required=True)
    parser.add_argument("--base-script", type=Path,
        help="reuse an existing PetDictionaryDataContents.as under a matching ~VERSION directory")
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    start = ET.fromstring(download(BASE + "start.xml", args.work_dir / "start.xml"))
    version = start.findtext("v", "")
    if not re.fullmatch(r"[0-9]{8,20}", version):
        raise ValueError("unexpected official start version")
    version_path = args.work_dir / f"versiondata~{version}.swf"
    download(BASE + f"versiondata~{version}.swf", version_path)
    versions = dict(re.findall(r'<f\b[^>]*\bn="([^"]+)"[^>]*\bv="([0-9]+)"', swf_text(version_path)))
    versions.update({node.attrib["n"]: node.attrib["v"] for node in start.findall("./u/f")})
    resources: dict[str, dict] = {}
    roots = []
    for key, name, cls in (
        ("petDictionary", "pet/petdictionarydata", "mmo.pet.petdictionarydata.PetDictionaryDataContents"),
        ("petDictionaryUpdate", "pet/petdictionarydataupdate", "mmo.pet.petdictionarydata.PetDictionaryDataContentsUpdate"),
    ):
        selected = versions[name]
        if not re.fullmatch(r"[0-9]{8,20}", selected):
            raise ValueError(f"unexpected version for {name}")
        url = BASE + f"{name}~{selected}.swf"
        swf = args.work_dir / f"{key}~{selected}.swf"
        data = download(url, swf)
        output = args.work_dir / f"{key}~{selected}-decomp"
        if key == "petDictionary" and args.base_script:
            if f"~{selected}" not in str(args.base_script) or args.base_script.name != "PetDictionaryDataContents.as":
                raise ValueError("cached base script does not match the current official base version")
            roots.append(args.base_script.parent)
            resources[key] = {"version": selected, "url": url, "sha256": hashlib.sha256(data).hexdigest()}
            continue
        completed = subprocess.run([str(args.java.resolve()), "-Xmx2g", "-Djava.awt.headless=true", "-jar", str(args.ffdec.resolve()),
            "-selectclass", cls, "-export", "script", str(output.resolve()), str(swf.resolve())],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True, timeout=180)
        (args.work_dir / f"{key}-decomp.log").write_bytes(completed.stdout)
        roots.append(output)
        resources[key] = {"version": selected, "url": url, "sha256": hashlib.sha256(data).hexdigest()}
    pets = merge_pet_sources(roots)
    if len(pets) < 9000:
        raise ValueError("official dictionary is incomplete; output has not been changed")
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    catalog["pets"] = pets
    # The provenance covers pets only. Badges, astrolabe, items and other sheets
    # remain at the version supplied by the baseline catalog.
    catalog["source"] = {"kind": "aoqi-official-pet-dictionary", "startVersion": version,
        "petDictionaryVersion": resources["petDictionaryUpdate"]["version"], "resources": resources}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    temporary.replace(args.output)
    (args.work_dir / "source.json").write_text(json.dumps(catalog["source"], ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"updated {len(pets)} pet identities from official version {resources['petDictionaryUpdate']['version']}")


if __name__ == "__main__":
    main()
