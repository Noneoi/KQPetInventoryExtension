"""Manual-only official StarGod and attribute icons, kept at stable local paths.

Attribute PNGs are copied byte-for-byte from the official FairyGUI package.
StarGod SWFs are rendered from their named sprite's first frame by FFDec.
No ActionScript is executed and no images are recolored or generated.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET
import zipfile
import zlib

from generate_stargod_icons import symbol_character_id, png_details

OFFICIAL = "https://aoqi.100bt.com/play/"
INDEX = "public-icon-index.json"
ATTRIBUTE_RESOURCE = "common/icon/attributeicon"
MAX_BYTES = 16 * 1024 * 1024


def read_object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def valid_png(path: Path, expected: dict | None = None) -> dict:
    info = png_details(path)
    if info["bytes"] > MAX_BYTES or info["width"] * info["height"] > 16_000_000:
        raise ValueError("图标图片过大")
    data = path.read_bytes()
    offset, ended = 8, False
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        end = offset + length + 12
        if end > len(data) or zlib.crc32(data[offset + 4:end - 4]) & 0xffffffff != struct.unpack_from(">I", data, end - 4)[0]:
            raise ValueError("图标 PNG 内容损坏")
        tag = data[offset + 4:offset + 8]
        offset = end
        if tag == b"IEND":
            ended = length == 0 and offset == len(data)
            break
    if not ended:
        raise ValueError("图标 PNG 不完整")
    if expected and expected.get("sha256") != info["sha256"]:
        raise ValueError("图标 PNG 与保存记录不一致")
    return info


def same_icon(path: Path, record: dict, version: str) -> bool:
    if not isinstance(record, dict):
        return False
    if record.get("version") != version or not isinstance(record.get("png"), dict):
        return False
    try:
        valid_png(path, record["png"])
        return True
    except (OSError, ValueError):
        return False


def copy_icon(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(".png.tmp")
    try:
        shutil.copyfile(source, temporary)
        valid_png(temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def zip_entries(path: Path) -> dict[str, bytes]:
    # Read members directly, never extract archive paths onto the filesystem.
    result, total = {}, 0
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) > 10000:
            raise ValueError("官方图标包条目过多")
        for member in archive.infolist():
            name = member.filename
            if member.is_dir():
                continue
            if name in result or not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or member.file_size > MAX_BYTES:
                raise ValueError("官方图标包目录结构不支持")
            total += member.file_size
            if total > 64 * 1024 * 1024:
                raise ValueError("官方图标包过大")
            result[name] = archive.read(member)
    return result


def attribute_mapping(package: dict[str, bytes]) -> dict[int, str]:
    xml = ET.fromstring(package.get("package.xml", b""))
    result = {}
    for node in xml.findall("./resources/image"):
        match = re.fullmatch(r"attr_([1-9][0-9]{0,5})", node.get("name", ""))
        if not match:
            continue
        key = int(match[1])
        filename = node.get("file", "")
        if key > 100000 or key in result or not re.fullmatch(r"[A-Za-z0-9_-]+\.png", filename):
            raise ValueError("官方属性图标 ID 或文件映射无效")
        result[key] = filename
    if not result:
        raise ValueError("官方属性图标映射为空")
    return result


def loader_default(updater, current: dict) -> dict:
    previous = current.get("source", {}).get("loaderDefault", {})
    version = updater.start_version
    if previous.get("version") == version and re.fullmatch(r"\d{1,20}", previous.get("defaultVersion", "")):
        return previous
    if not re.fullmatch(r"\d{8,20}", version):
        raise ValueError("官方加载器版本无效")
    path = updater.scratch / "icon-loader.swf"
    source = updater.fetch(OFFICIAL + f"loader~{version}.swf", path)
    script = updater.export(path, updater.scratch / "icon-loader", "mmo.loader.scheduler.VersManager")
    text = script.read_text(encoding="utf-8-sig")
    match = re.search(r'const DEFAULT_VERSION:String\s*=\s*"(\d+)"', text)
    if not match or 'return _loc3_ + DEFAULT_VERSION;' not in text:
        raise ValueError("官方未列出资源的版本规则已变化，保留原图标")
    return {**source, "version": version, "defaultVersion": match[1]}


def export_star(updater, key: int, version: str, version_source: str) -> tuple[dict, Path]:
    if not re.fullmatch(r"\d{1,20}", version):
        raise ValueError("星神图标版本无效")
    resource = f"stargodres/stargod{key}"
    url = OFFICIAL + f"{resource}~{version}.swf"
    swf = updater.scratch / f"icon-star-{key}.swf"
    source = updater.fetch(url, swf)
    symbol = f"mmo.stargodres.StarGod{key}"
    character = symbol_character_id(swf.read_bytes(), symbol)
    export = updater.scratch / f"icon-star-{key}"
    java, ffdec = updater.runtimes
    result = subprocess.run([str(java), "-Xmx512m", "-Djava.awt.headless=true", "-jar", str(ffdec),
                             "-selectid", str(character), "-select", f"{character}:1", "-ignorebackground",
                             "-export", "sprite", str(export), str(swf)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise ValueError(f"星神 {key} 图标解析失败")
    images = list(export.rglob("*.png"))
    if len(images) != 1 or images[0].name != "1.png":
        raise ValueError(f"星神 {key} 图标第一帧缺失或不唯一")
    info = valid_png(images[0])
    return {"id": key, "file": f"images/stargods/{key}.png", "url": url, "version": version,
            "versionSource": version_source, "source": source, "spriteClass": symbol,
            "characterId": character, "frame": 1, "png": info}, images[0]


def update_icons(updater, versions: dict) -> bool:
    # Imported at call time to keep this module usable through Updater's own
    # resource export and deterministic fake transports in contract tests.
    from public_data_updater import emit, runtime_entry
    current = read_object(updater.root / "catalog" / INDEX)
    def records(section):
        values = current.get(section, {})
        if not isinstance(values, dict):
            return {}
        return {key: value for key, value in values.items() if re.fullmatch(r"[1-9][0-9]{0,5}", key)
                and int(key) <= 100000 and isinstance(value, dict)}
    stars, attributes = records("stargods"), records("attributes")
    source = dict(current["source"]) if isinstance(current.get("source"), dict) else {}
    updater.icon_failures = []
    changed = False

    try:
        emit("progress", message="检查官方属性图标")
        attr_version = versions.get(ATTRIBUTE_RESOURCE, "")
        res_version = versions.get(ATTRIBUTE_RESOURCE + "@res", "")
        if not all(re.fullmatch(r"\d{8,20}", value) for value in (attr_version, res_version)):
            raise ValueError("官方属性图标包版本缺失")
        signature = attr_version + ":" + res_version
        all_valid = attributes and all(same_icon(updater.root / f"images/attributes/{key}.png", value, signature)
                                       for key, value in attributes.items())
        if source.get("attributeVersion") != signature or not all_valid:
            metadata, payload = updater.scratch / "attributes.fui", updater.scratch / "attributes-res.fui"
            meta_source = updater.fetch(OFFICIAL + f"{ATTRIBUTE_RESOURCE}~{attr_version}.fui", metadata)
            data_source = updater.fetch(OFFICIAL + f"{ATTRIBUTE_RESOURCE}@res~{res_version}.fui", payload)
            mapping, contents = attribute_mapping(zip_entries(metadata)), zip_entries(payload)
            if len(mapping) < len(attributes) * 0.9:
                raise ValueError("官方属性图标目录不完整，保留原图标")
            staged = {}
            for key, filename in mapping.items():
                if filename not in contents:
                    raise ValueError(f"官方属性图标包缺少属性 {key}")
                path = updater.scratch / f"attribute-{key}.png"
                path.write_bytes(contents[filename])
                staged[key] = (path, valid_png(path))
            new_attributes = {}
            for key, (path, info) in staged.items():
                copy_icon(path, updater.root / f"images/attributes/{key}.png")
                new_attributes[str(key)] = {"id": key, "file": f"images/attributes/{key}.png", "version": signature, "png": info}
            # Keep obsolete files on disk for manual cache management, while
            # an unchanged package only verifies its current published IDs.
            attributes = new_attributes
            source.update(attributeVersion=signature, attributePackage=meta_source, attributeImages=data_source)
            changed = True
    except Exception as error:
        updater.icon_failures.append("属性图标：" + str(error))

    try:
        emit("progress", message="检查官方星神图标")
        definitions = updater.current("pets").get("stargods", {})
        ids = sorted(int(key) for key in definitions if re.fullmatch(r"[1-9][0-9]{0,5}", key) and int(key) <= 100000)
        if not ids:
            raise ValueError("本地没有星神定义，保留原图标")
        default = loader_default(updater, current) if any(f"stargodres/stargod{key}" not in versions for key in ids) else {}
        if default:
            source["loaderDefault"] = default
        baseline = updater.baseline / "stargod-icons"
        bundled = {str(value.get("id")): value for value in read_object(baseline / "sources.json").get("icons", [])
                   if isinstance(value, dict)}
        downloads = []
        for key in ids:
            resource = f"stargodres/stargod{key}"
            version = versions.get(resource, default.get("defaultVersion", ""))
            target = updater.root / f"images/stargods/{key}.png"
            if same_icon(target, stars.get(str(key), {}), version):
                continue
            record = bundled.get(str(key), {})
            embedded = baseline / f"{key}.png"
            if same_icon(embedded, record, version):
                copy_icon(embedded, target)
                stars[str(key)] = {**record, "file": f"images/stargods/{key}.png"}
                changed = True
            else:
                downloads.append((key, version, "versiondata" if resource in versions else "loader-default"))
        if downloads:
            if updater.runtimes is None:
                updater.runtimes = (runtime_entry(updater.tools, "java", updater.scratch),
                                    runtime_entry(updater.tools, "ffdec", updater.scratch))
            with ThreadPoolExecutor(max_workers=3) as pool:
                pending = {pool.submit(export_star, updater, *item): item[0] for item in downloads}
                for completed, future in enumerate(as_completed(pending), 1):
                    key = pending[future]
                    try:
                        record, png = future.result()
                        copy_icon(png, updater.root / f"images/stargods/{key}.png")
                        stars[str(key)] = record
                        changed = True
                    except Exception as error:
                        updater.icon_failures.append(f"星神 {key}：{error}")
                    emit("progress", message=f"更新星神图标 {completed}/{len(downloads)}")
    except Exception as error:
        updater.icon_failures.append("星神图标：" + str(error))

    index = {"schemaVersion": 1, "source": source, "stargods": stars, "attributes": attributes}
    if index != current:
        atomic_json(updater.root / "catalog" / INDEX, index)
        changed = True
    emit("progress", message=f"本地图标：星神 {len(stars)} 个，属性 {len(attributes)} 个" +
         (f"；{len(updater.icon_failures)} 项未更新，保留原图" if updater.icon_failures else ""))
    return changed
