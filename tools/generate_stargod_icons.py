#!/usr/bin/env python3
"""Export official StarGod sprite frame 1 as unmodified transparent PNGs.

Requires Java and JPEXS FFDec. Only IDs present in pet-detail-data.json are
exported. Resource versions come from the official versiondata XML binary.
No ActionScript is run, and no generated artwork or image editing is used.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zlib


ROOT = Path(__file__).resolve().parents[1]
OFFICIAL_ROOT = "https://aoqi.100bt.com/play/"
MAX_RESOURCE_BYTES = 16 * 1024 * 1024


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbol_character_id(data: bytes, class_name: str) -> int:
    """Read top-level SWF SymbolClass (76) without decompiling scripts."""
    if len(data) < 12 or data[:3] not in (b"FWS", b"CWS"):
        raise ValueError("expected an FWS or CWS file")
    expected_length = struct.unpack_from("<I", data, 4)[0]
    if not 12 <= expected_length <= MAX_RESOURCE_BYTES:
        raise ValueError("invalid or excessive uncompressed SWF length")
    if data[:3] == b"CWS":
        decompressor = zlib.decompressobj()
        body = decompressor.decompress(data[8:], expected_length - 8 + 1)
        if not decompressor.eof or decompressor.unconsumed_tail:
            raise ValueError("invalid compressed SWF")
        data = data[:8] + body
    if len(data) != expected_length:
        raise ValueError("SWF length does not match header")
    # RECT's first five bits contain its bit width; then frame rate/count.
    offset = 8 + (5 + 4 * (data[8] >> 3) + 7) // 8 + 4
    matches = []
    sprites = set()
    while offset + 2 <= len(data):
        header = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        tag, size = header >> 6, header & 63
        if size == 63:
            if offset + 4 > len(data):
                raise ValueError("truncated SWF tag length")
            size = struct.unpack_from("<I", data, offset)[0]
            offset += 4
        end = offset + size
        if end > len(data):
            raise ValueError("truncated SWF tag")
        if tag == 39 and size >= 4:  # DefineSprite
            sprites.add(struct.unpack_from("<H", data, offset)[0])
        if tag == 76:
            if size < 2:
                raise ValueError("invalid SymbolClass tag")
            count = struct.unpack_from("<H", data, offset)[0]
            cursor = offset + 2
            for _ in range(count):
                if cursor + 2 > end:
                    raise ValueError("truncated SymbolClass entry")
                character = struct.unpack_from("<H", data, cursor)[0]
                cursor += 2
                name_end = data.find(b"\0", cursor, end)
                if name_end < 0:
                    raise ValueError("unterminated SymbolClass name")
                name = data[cursor:name_end].decode("utf-8")
                if name == class_name:
                    matches.append(character)
                cursor = name_end + 1
        offset = end
        if tag == 0:
            break
    if len(matches) != 1 or matches[0] not in sprites:
        raise ValueError(f"missing or ambiguous sprite class {class_name}")
    return matches[0]


def png_details(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError(f"invalid exported PNG: {path.name}")
    width, height, depth, color = struct.unpack_from(">IIBB", data, 16)
    if not width or not height or depth != 8 or color != 6:
        raise ValueError(f"expected a nonempty RGBA PNG: {path.name}")
    return {"width": width, "height": height, "bytes": len(data), "sha256": sha256(path)}


def fetch(url: str, target: Path) -> bytes:
    if target.exists():
        return target.read_bytes()
    # A bounded second attempt handles a transient network failure.
    for attempt in range(2):
        try:
            request = urllib.request.Request(url, headers={
                "User-Agent": "KQPetStarGodIconGenerator/1.0"})
            with urllib.request.urlopen(request, timeout=45) as response:
                data = response.read(MAX_RESOURCE_BYTES + 1)
            if len(data) > MAX_RESOURCE_BYTES or data[:3] not in (b"CWS", b"FWS"):
                raise ValueError("official response is not a supported SWF")
            target.write_bytes(data)
            return data
        except (OSError, ValueError):
            if attempt:
                raise
    raise AssertionError("unreachable")


def export_one(stargod_id: int, version: str, args: argparse.Namespace) -> tuple[dict, Path]:
    resource = f"stargodres/stargod{stargod_id}"
    url = f"{OFFICIAL_ROOT}{resource}~{version}.swf"
    swf = args.cache / "swf" / f"{stargod_id}~{version}.swf"
    data = fetch(url, swf)
    class_name = f"mmo.stargodres.StarGod{stargod_id}"
    character = symbol_character_id(data, class_name)
    # Each export gets a fresh owned directory, so no stale frame can pass QA.
    export_dir = Path(tempfile.mkdtemp(prefix=f"{stargod_id}-", dir=args.cache / "export"))
    command = [args.java, "-Djava.awt.headless=true", "-jar", str(args.ffdec_jar),
               "-selectid", str(character), "-select", f"{character}:1",
               "-ignorebackground", "-export", "sprite", str(export_dir), str(swf)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=90, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise RuntimeError(f"FFDec failed for {stargod_id}: " +
                           result.stdout.decode("utf-8", errors="replace")[-1500:])
    images = list(export_dir.rglob("*.png"))
    if len(images) != 1 or images[0].name != "1.png":
        raise ValueError(f"expected exactly sprite frame 1 for {stargod_id}")
    png = images[0]
    details = png_details(png)
    entry = {"id": stargod_id, "file": f"{stargod_id}.png", "url": url,
             "version": version, "versionSource": args.version_sources[stargod_id],
             "swfSha256": hashlib.sha256(data).hexdigest(),
             "swfBytes": len(data), "spriteClass": class_name,
             "characterId": character, "frame": 1, "png": details}
    return entry, png


def updated_qrc(qrc: Path, output: Path, ids: list[int]) -> str:
    text = qrc.read_text(encoding="utf-8")
    root = ET.fromstring(text)
    groups = [node for node in root.findall("qresource") if node.get("prefix") == "/kqpet"]
    if len(groups) != 1:
        raise ValueError("expected exactly one /kqpet resource group")
    existing = {node.get("alias"): node.text for node in groups[0].findall("file")}
    additions = []
    for stargod_id in ids:
        alias = f"stargod-icons/{stargod_id}.png"
        relative = Path(os.path.relpath(output / f"{stargod_id}.png", qrc.parent)).as_posix()
        if alias in existing and existing[alias] != relative:
            raise ValueError(f"conflicting qrc alias: {alias}")
        if alias not in existing:
            additions.append(f'    <file alias="{alias}">{relative}</file>\n')
    if additions:
        # Locate the correct group's close tag while preserving existing entries.
        match = re.search(r'<qresource\s+prefix="/kqpet"\s*>.*?</qresource>', text, re.S)
        if not match:
            raise ValueError("cannot locate /kqpet resource text")
        position = text.rfind("  </qresource>", match.start(), match.end())
        if position < 0:
            raise ValueError("unexpected qresource formatting")
        text = text[:position] + "".join(additions) + text[position:]
    ET.fromstring(text)
    return text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, default=ROOT / "assets/pet-detail-data.json")
    parser.add_argument("--versiondata", type=Path, required=True)
    parser.add_argument("--loader-version-source", type=Path,
                        help="Official decompiled VersManager.as, required for unlisted resource IDs")
    parser.add_argument("--ffdec-jar", type=Path, required=True)
    parser.add_argument("--java", default="java")
    parser.add_argument("--cache", type=Path, default=ROOT / "build-v2-stargod-icons")
    parser.add_argument("--output", type=Path, default=ROOT / "assets/stargod-icons")
    parser.add_argument("--qrc", type=Path, default=ROOT / "src/extension/resources.qrc")
    parser.add_argument("--workers", type=int, default=4, choices=range(1, 9))
    args = parser.parse_args()
    for name in ("metadata", "versiondata", "ffdec_jar", "cache", "output", "qrc"):
        setattr(args, name, getattr(args, name).resolve())
    if Path(args.java).exists():
        args.java = str(Path(args.java).resolve())
    if not args.ffdec_jar.is_file():
        parser.error("FFDec jar is missing")
    metadata = json.loads(args.metadata.read_text(encoding="utf-8"))
    ids = sorted(int(key) for key in metadata["stargods"])
    if not ids or len(ids) != len(set(ids)) or any(key <= 0 for key in ids):
        raise ValueError("invalid or empty stargod metadata IDs")
    version_root = ET.parse(args.versiondata).getroot()
    versions = {node.get("n"): node.get("v") for node in version_root.iter("f")}
    selected = {key: versions.get(f"stargodres/stargod{key}", "") for key in ids}
    missing = [key for key, value in selected.items() if not value or not value.isdecimal()]
    args.version_sources = {key: "versiondata" for key in ids}
    fallback_source = None
    if missing:
        if args.loader_version_source is None:
            raise ValueError(f"supply official --loader-version-source for unlisted IDs: {missing}")
        loader_text = args.loader_version_source.read_text(encoding="utf-8")
        default = re.search(r'const DEFAULT_VERSION:String\s*=\s*"(\d+)"', loader_text)
        if not default or 'return _loc3_ + DEFAULT_VERSION;' not in loader_text:
            raise ValueError("unsupported official loader default-version rule")
        fallback_source = {"file": args.loader_version_source.name,
                           "sha256": sha256(args.loader_version_source),
                           "class": "mmo.loader.scheduler.VersManager",
                           "method": "getVersion", "defaultVersion": default[1],
                           "rule": "Unlisted SWF resource uses ~DEFAULT_VERSION suffix"}
        for key in missing:
            selected[key] = default[1]
            args.version_sources[key] = "loader-default"
    for directory in (args.cache / "swf", args.cache / "export"):
        directory.mkdir(parents=True, exist_ok=True)
    records = {}
    failures = []
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        pending = {executor.submit(export_one, key, selected[key], args): key for key in ids}
        for future in as_completed(pending):
            key = pending[future]
            try:
                records[key] = future.result()
                print(f"[{len(records)}/{len(ids)}] StarGod{key} exported", flush=True)
            except Exception as error:
                failures.append(f"StarGod{key}: {error}")
    if failures or set(records) != set(ids):
        raise RuntimeError("Export incomplete; assets were not updated:\n" + "\n".join(failures))
    qrc_text = updated_qrc(args.qrc, args.output, ids)
    provenance = {"schema": 1, "source": "Official Aoqi StarGod SWF sprite resources",
                  "versiondata": {"file": args.versiondata.name,
                                  "sha256": sha256(args.versiondata),
                                  "releaseDate": version_root.get("releaseDate", "")},
                  "extractor": {"tool": "JPEXS FFDec", "jarSha256": sha256(args.ffdec_jar),
                                "script": "tools/generate_stargod_icons.py",
                                "selection": "SymbolClass sprite character ID; frame 1",
                                "ignoreBackground": True, "imageEdits": False},
                  "count": len(ids), "icons": [records[key][0] for key in ids]}
    if fallback_source:
        provenance["loaderDefault"] = fallback_source
    args.output.mkdir(parents=True, exist_ok=True)
    for key in ids:
        target = args.output / f"{key}.png"
        temporary = target.with_suffix(".png.tmp")
        shutil.copyfile(records[key][1], temporary)
        temporary.replace(target)
    (args.output / "sources.json").write_text(
        json.dumps(provenance, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    args.qrc.write_text(qrc_text, encoding="utf-8", newline="\n")
    total_bytes = sum(records[key][0]["png"]["bytes"] for key in ids)
    print(f"Complete: {len(ids)} official icons; {total_bytes} PNG bytes; missing=0")


if __name__ == "__main__":
    main()
