#!/usr/bin/env python3
"""Explicit, one-shot public-data update. Never contacts account/game APIs.

All formal updates prepare an isolated complete candidate, then use the shared
native publisher. The low-level builders only write their candidate workspace;
they never replace active public catalog files.
"""
from __future__ import annotations

import argparse
import copy
import contextlib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_pet_detail_data import (PET_DICTIONARY_SCHEMA, merge_pet_sources, parse_astrolabe_catalog, parse_stargod_catalog,
                                     parse_badges, parse_sacred, parse_sacred_plans, parse_items, swf_text)
from generate_shop_exchange_data import CONFIG_CLASS, RESOURCE, parse_config
from generate_stargod_icons import symbol_character_id, png_details
from public_names_updater import (NAME_RULE_VERSION, NAME_RESOURCES, SACRED_SOURCE_RESOURCE,
                                  needs_names_update, update_names, equipment_source_names)
from public_resource_session import PublicResourceSession, VERSION_RULE, resource_matches
from public_cultivation_supplement import needs_supplement, refresh_supplement

OFFICIAL = "https://aoqi.100bt.com/play/"
OFFICIAL_ROOTS = (OFFICIAL, "https://aoqi.100bt.com/h5/")
RUNTIMES = {
    "java": {"url": "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jre_x64_windows_hotspot_21.0.12.1_1.zip",
             "sha256": "d35f31e712f0fcf6ac5a093edc90204fbff22f720ba3950bd09d331d5e621636",
             "entry": "jdk-21.0.12.1+1-jre/bin/java.exe"},
    "ffdec": {"url": "https://github.com/jindrapetrik/jpexs-decompiler/releases/download/version26.2.1/ffdec_26.2.1.zip",
              "sha256": "0333b56998a55bd83f4e0deb678a811fcdc45607582b4f5dd438309c8c3ad5ce",
              "entry": "ffdec.jar"},
}
COMPONENTS = {"pets": "pet-detail-data.json", "skills": "pet-skill-data.json",
              "shop": "shop-exchange-data.json", "images": "pet-image-index.json",
              "icons": "public-icon-index.json", "routines": "routine-overview.json",
              "battleReplay": "battle-replay-catalog.brpack"}
# Builders run in this order inside an isolated candidate. Dependency-group
# retention and publication are owned by public_dataset_store and the native
# publisher, respectively.
COMPONENT_LABELS = (("pets", "精灵与养成资料"), ("skills", "精灵技能资料"),
                    ("shop", "指定精灵商店"), ("images", "精灵图片索引"),
                    ("icons", "星神与属性图标"), ("routines", "日常任务与活动"),
                    ("battleReplay", "战斗回放资料"))
POWER_RULE_VERSION = 1
CULTIVATION_RULE_VERSION = 1
PET_RESOURCES = (("petDictionary", "pet/petdictionarydata", "mmo.pet.petdictionarydata.PetDictionaryDataContents"),
                 ("petDictionaryUpdate", "pet/petdictionarydataupdate", "mmo.pet.petdictionarydata.PetDictionaryDataContentsUpdate"))
POWER_RESOURCES = (("stargodService", "stargodv3/stargodservice"), ("materialService", "material/materialservice"),
                   ("interfaces", "library/interfaces"), ("astrolabeService", "astrolabe/astrolabeservice"))
CULTIVATION_RESOURCES = (("badgeService", "petbadge/petbadgeservice"),
                         ("sacredService", "sacredequipment/sacredequipmentservice"),
                         ("materialData", "library/materialdata"),
                         ("materialDataUpdate", "library/materialdataupdate"))


def cultivation_rules_valid(catalog: dict) -> bool:
    if catalog.get("cultivationRuleVersion") != CULTIVATION_RULE_VERSION:
        return False
    def integer(value, minimum=0):
        return type(value) is int and minimum <= value <= 2**31 - 1
    def costs(value):
        return isinstance(value, list) and all(isinstance(item, dict) and integer(item.get("type"), 1) and
            integer(item.get("id")) and integer(item.get("count"), 1) and
            ("extra" not in item or integer(item["extra"])) for item in value)
    badges, sacred, items = (catalog.get(name) for name in ("badges", "sacredEquipment", "items"))
    if not all(isinstance(value, dict) and value for value in (badges, sacred, items)):
        return False
    for badge in badges.values():
        if (not isinstance(badge, dict) or badge.get("type") not in (0, 1) or
                not integer(badge.get("maxLevel"), 1) or not costs(badge.get("activationCost"))):
            return False
        levels = badge.get("levels")
        if not isinstance(levels, dict):
            return False
        if badge["type"] == 0 and set(levels) != {str(n) for n in range(1, badge["maxLevel"] + 1)}:
            return False
        if any(not isinstance(level, dict) or not integer(level.get("battlePower")) or not costs(level.get("cost"))
               for level in levels.values()):
            return False
        if any(item["type"] == 4 and not items.get(str(item["id"]), {}).get("name") for item in badge["activationCost"]):
            return False
    if any(not isinstance(value, dict) or not integer(value.get("sourceId"), 1) or
           not isinstance(value.get("sourceName"), str) for value in sacred.values()):
        return False
    for name in ("sacredStarPlans", "sacredStagePlans"):
        plans = catalog.get(name)
        if not isinstance(plans, dict) or not plans:
            return False
        for plan in plans.values():
            if not isinstance(plan, dict) or not integer(plan.get("maxLevel"), 1) or not isinstance(plan.get("levels"), dict):
                return False
            if set(plan["levels"]) != {str(n) for n in range(1, plan["maxLevel"] + 1)}:
                return False
            if any(not isinstance(level, dict) or not integer(level.get("battlePower")) or
                   not integer(level.get("equipmentCount")) or not costs(level.get("cost")) for level in plan["levels"].values()):
                return False
    return all(costs(node.get("lightUpMaterials")) for node in catalog.get("astrolabe", {}).values())


def power_rules_valid(catalog: dict) -> bool:
    if catalog.get("powerRuleVersion") != POWER_RULE_VERSION:
        return False
    stars, astrolabe = catalog.get("stargods"), catalog.get("astrolabe")
    if not isinstance(stars, dict) or not stars or not isinstance(astrolabe, dict) or not astrolabe:
        return False
    def integer(value, minimum=0):
        return type(value) is int and minimum <= value <= 2**31 - 1
    for star in stars.values():
        if (not isinstance(star, dict) or not integer(star.get("type"), 1) or
                not integer(star.get("quality")) or star["quality"] > 6 or
                type(star.get("limited")) is not bool or type(star.get("changeable")) is not bool or
                not isinstance(star.get("limitJobs"), list) or not all(integer(job, 1) for job in star["limitJobs"]) or
                not isinstance(star.get("battlePower"), dict) or
                (star["quality"] >= 2 and not star["battlePower"]) or
                not all(str(level).isdecimal() and int(level) > 0 and integer(power) for level, power in star["battlePower"].items())):
            return False
    return all(isinstance(node, dict) and type(node.get("isTBD")) is bool and integer(node.get("locatedType")) and integer(node.get("battlePower"))
               for node in astrolabe.values())


def matching_resource(previous: dict, label: str, key: str, versions: dict) -> bool:
    return resource_matches(previous.get(label, {}), key, versions)


def emit(event: str, **values) -> None:
    # Progress lines are machine input, so they are written as pure ASCII:
    # json.dumps escapes non-ASCII as \uXXXX and the consumer decodes the same
    # text back. The alternative (raw UTF-8 bytes) is identical only while every
    # caller sets PYTHONIOENCODING, which a standalone script run does not do.
    print(json.dumps({"event": event, **values}, ensure_ascii=True), flush=True)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_object(path: Path) -> dict:
    try:
        if path.stat().st_size > 32 * 1024 * 1024:
            return {}
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(name)


def fetch(url: str, output: Path, *, runtime: bool = False) -> dict:
    if not runtime and not url.startswith(OFFICIAL_ROOTS):
        raise ValueError("resource is outside the official game origin")
    request = urllib.request.Request(url, headers={"User-Agent": "KQPetPublicData/2.0"})
    limit = (96 if runtime else 32) * 1024 * 1024
    total = 0
    with urllib.request.urlopen(request, timeout=60) as response, output.open("wb") as stream:
        final = urllib.parse.urlsplit(response.url)
        if final.scheme != "https" or (not runtime and not response.url.startswith(OFFICIAL_ROOTS)):
            raise ValueError("resource redirected outside the allowed HTTPS origin")
        while chunk := response.read(128 * 1024):
            total += len(chunk)
            if total > limit:
                raise ValueError("resource exceeds the download limit")
            stream.write(chunk)
    return {"url": url, "sha256": digest(output), "bytes": total}


def extract_zip(archive: Path, destination: Path) -> None:
    with zipfile.ZipFile(archive) as package:
        entries = package.infolist()
        if len(entries) > 10000 or sum(item.file_size for item in entries) > 512 * 1024 * 1024:
            raise ValueError("runtime archive is unexpectedly large")
        for item in entries:
            name = PurePosixPath(item.filename.replace("\\", "/"))
            if name.is_absolute() or ".." in name.parts or any(":" in p for p in name.parts):
                raise ValueError("runtime archive contains an unsafe path")
            if (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("runtime archive contains a symbolic link")
        package.extractall(destination)


def runtime_entry(tools: Path, name: str, scratch: Path) -> Path:
    definition = RUNTIMES[name]
    directory = tools / name
    marker = read_object(directory / "verified.json")
    entry = directory / definition["entry"]
    if marker.get("archiveSha256") == definition["sha256"] and entry.is_file() and marker.get("entrySha256") == digest(entry):
        return entry
    emit("progress", message="首次准备本地数据解析工具：" + name)
    archive = scratch / (name + ".zip")
    fetch(definition["url"], archive, runtime=True)
    if digest(archive) != definition["sha256"]:
        raise ValueError(name + " runtime checksum mismatch")
    staged = scratch / name
    staged.mkdir()
    extract_zip(archive, staged)
    candidate = staged / definition["entry"]
    if not candidate.is_file():
        raise ValueError(name + " runtime entry missing")
    atomic_json(staged / "verified.json", {"archiveSha256": definition["sha256"], "entrySha256": digest(candidate)})
    # This directory is owned by this updater, never a user-selected directory.
    if directory.exists():
        shutil.rmtree(directory)
    shutil.move(str(staged), str(directory))
    archive.unlink()
    return entry


class Updater:
    def __init__(self, root: Path, baseline: Path, scratch: Path, *, fetcher=fetch, exporter=None,
                 isolated_workspace=False, runtime_root=None):
        if isolated_workspace is not True:
            raise ValueError("Low-level Updater requires an isolated candidate workspace")
        self.root, self.baseline, self.scratch = root, baseline, scratch
        self.catalog = root / "catalog"
        self.tools = root / "data-tools"
        self.runtime_tools = Path(runtime_root) if runtime_root is not None else self.tools
        self.force_components = set()
        self.public_resources = PublicResourceSession(scratch, fetcher)
        self.fetch = self.public_resources.fetch
        self.exporter = exporter
        self.runtimes = None
        self.start_version = ""
        self.image_failures = 0
        self.icon_failures = []
        self.activity_exchange_failures = []
        self.activity_exchange_pending = []
        self.resource_cache = {}
        self.export_cache = {}
        self.quality_results = {}
        self.shared_dependency_failures = {}

    def assess_quality(self, component, candidate, previous, *, source_coverage=None, reference_evidence=None,
                       source_fidelity=None, replay_regression=None):
        from public_catalog_quality import enforce_catalog_quality, CatalogQualityError
        self.stage('validating', component)
        try:
            result = enforce_catalog_quality(component, candidate, previous,
                self.tools / 'quality', source_coverage=source_coverage, reference_evidence=reference_evidence,
                source_fidelity=source_fidelity, replay_regression=replay_regression)
        except CatalogQualityError as error:
            self.quality_results[component] = error.metadata
            raise
        self.quality_results[component] = result
        return result

    def stage(self, phase, component='', completed=-1, total=-1):
        callback = getattr(self, 'progress_callback', None)
        if callback:
            callback(phase, component, completed, total)

    def export(self, swf: Path, target: Path, selected: str) -> Path:
        key = (digest(swf), selected)
        cached = self.export_cache.get(key)
        if cached and cached[0].is_file():
            source, fingerprint, relative = cached
            if digest(source) != fingerprint:
                raise ValueError("本次公共解析缓存校验失败")
            output = target / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            if source.resolve() != output.resolve():
                shutil.copyfile(source, output)
            return output
        if self.exporter:
            result = self.exporter(swf, target, selected)
            self.export_cache[key] = (result, digest(result), result.relative_to(target))
            return result
        if self.runtimes is None:
            self.tools.mkdir(parents=True, exist_ok=True)
            self.runtimes = (runtime_entry(self.runtime_tools, "java", self.scratch), runtime_entry(self.runtime_tools, "ffdec", self.scratch))
        java, ffdec = self.runtimes
        persistent = getattr(self, "source_cache", None)
        cache_directory = None
        if persistent is not None:
            from public_dataset_store import canonical, immutable_write, parse_json, read_bytes, sha
            if not hasattr(self, "_extractor_fingerprint"):
                self._extractor_fingerprint = {"javaSha256": digest(java), "ffdecSha256": digest(ffdec)}
            identity = {"sourceSha256": key[0], "selectedClass": selected,
                        "extractor": self._extractor_fingerprint,
                        "parserSha256": getattr(self, "parser_identity", ""), "format": "actionscript-utf8-v1"}
            cache_directory = Path(persistent) / "extracts" / sha(canonical(identity))
            if (cache_directory / "record.json").is_file():
                record = parse_json(read_bytes(cache_directory / "record.json"))
                raw = read_bytes(cache_directory / "script.as", 16 * 1024 * 1024)
                relative = PurePosixPath(record.get("relative", ""))
                if record.get("identity") != identity or record.get("sha256") != sha(raw) or not relative.parts \
                        or relative.is_absolute() or any(part in (".", "..") or ":" in part or "\\" in part for part in relative.parts):
                    raise ValueError("持久公共解析缓存的字节或规则身份不匹配")
                output = target.joinpath(*relative.parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(raw)
                self.export_cache[key] = (output, sha(raw), output.relative_to(target))
                return output
        command = [str(java), "-Xmx2g", "-Djava.awt.headless=true", "-jar", str(ffdec),
                   "-selectclass", selected, "-export", "script", str(target), str(swf)]
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=600, creationflags=flags)
        if completed.returncode:
            raise ValueError("官方资源解析失败，保留原数据")
        matches = list(target.rglob(selected.rsplit(".", 1)[-1] + ".as"))
        if len(matches) != 1 or matches[0].stat().st_size > 16 * 1024 * 1024:
            raise ValueError("官方资源结构已改变，保留原数据")
        self.export_cache[key] = (matches[0], digest(matches[0]), matches[0].relative_to(target))
        if cache_directory is not None:
            raw = matches[0].read_bytes()
            record_raw = canonical({"identity": identity, "sha256": sha(raw),
                                    "relative": matches[0].relative_to(target).as_posix()})
            if not hasattr(self, "_persistent_cache_bytes"):
                existing = list((Path(persistent) / "extracts").glob("*/*"))
                self._persistent_cache_bytes = sum(path.stat().st_size for path in existing if path.is_file())
                self._persistent_cache_files = len(existing)
            # Filling the bounded cache must never require deleting a possible
            # in-flight reader's objects. A full cache simply stops new writes.
            if self._persistent_cache_bytes + len(raw) + len(record_raw) <= 128 * 1024 * 1024 and self._persistent_cache_files < 4096:
                immutable_write(cache_directory / "script.as", raw)
                immutable_write(cache_directory / "record.json", record_raw)
                self._persistent_cache_bytes += len(raw) + len(record_raw)
                self._persistent_cache_files += 2
        return matches[0]

    def versions(self) -> dict[str, str]:
        emit("progress", message="检查官方公共数据版本")
        versions, _, chain = self.public_resources.flash_chain()
        self.start_version = chain['versiondata']['version']
        atomic_json(self.catalog / "official-versions.json", {"startVersion": self.start_version,
                    "versionRule": VERSION_RULE, "versions": versions, "source": chain})
        return versions

    def resource(self, key: str, versions: dict) -> tuple[Path, dict]:
        version = versions.get(key, "2000")
        if not re.fullmatch(r"[A-Za-z0-9_/-]+", key) or '..' in key or key.startswith('/'):
            raise ValueError("无效的官方资源路径")
        if not re.fullmatch(r"(?:2000|\d{8,20})", version):
            raise ValueError("官方清单缺少资源版本：" + key)
        cached = self.resource_cache.get((key, version))
        if cached and cached[0].is_file():
            if digest(cached[0]) != cached[1]['sha256']:
                raise ValueError("本次共享资源缓存校验失败：" + key)
            return cached[0], dict(cached[1])
        path = self.scratch / (hashlib.sha256((key + '~' + version).encode()).hexdigest()[:16] + ".swf")
        source = self.fetch(OFFICIAL + f"{key}~{version}.swf", path)
        source["version"] = version
        self.resource_cache[(key, version)] = (path, dict(source))
        return path, source

    def current(self, component: str) -> dict:
        filename = COMPONENTS[component]
        existing = read_object(self.catalog / filename)
        field = {"pets": "pets", "skills": "skills", "shop": "shops", "images": "faces",
                 "icons": "stargods", "routines": "tasks"}[component]
        if existing.get(field) and isinstance(existing.get("source", {}), dict):
            return existing
        return read_object(self.baseline / filename)

    def pets(self, versions: dict) -> bool:
        current = self.current("pets")
        prior_catalog = copy.deepcopy(current)
        if "pets" in self.force_components:
            # Invalidate only the candidate's prior parser decisions. Raw rows
            # and prior_catalog stay intact for the independent loss check.
            current = copy.deepcopy(current)
            for field in ("petDictionarySchema", "powerRuleVersion", "cultivationRuleVersion",
                          "nameRuleVersion", "cultivationSupplementRuleVersion"):
                current[field] = -1
        previous = current.get("source", {}).get("resources", {})
        dictionary_changed = (current.get("petDictionarySchema") != PET_DICTIONARY_SCHEMA or len(current.get("pets", {})) < 9000 or
            any(not isinstance(pet, dict) or not isinstance(pet.get("sign"), str) for pet in current.get("pets", {}).values()) or
            not all(matching_resource(previous, label, key, versions) for label, key, _ in PET_RESOURCES))
        rules_upgrade = not power_rules_valid(current)
        stars_changed = rules_upgrade or not all(matching_resource(previous, label, key, versions)
                                                 for label, key in POWER_RESOURCES[:3])
        astrolabe_changed = rules_upgrade or not matching_resource(previous, *POWER_RESOURCES[3], versions)
        cultivation_upgrade = not cultivation_rules_valid(current)
        badges_changed = cultivation_upgrade or not matching_resource(previous, *CULTIVATION_RESOURCES[0], versions)
        sacred_changed = (cultivation_upgrade or not matching_resource(previous, *CULTIVATION_RESOURCES[1], versions)
                          or not matching_resource(previous, *POWER_RESOURCES[1], versions)
                          or not matching_resource(previous, *SACRED_SOURCE_RESOURCE[:2], versions))
        items_changed = cultivation_upgrade or not all(matching_resource(previous, label, key, versions)
                                                      for label, key in CULTIVATION_RESOURCES[2:])
        names_changed = needs_names_update(current, versions)
        supplement_changed = needs_supplement(current, versions)
        astrolabe_changed = astrolabe_changed or cultivation_upgrade
        if not any((dictionary_changed, stars_changed, astrolabe_changed, badges_changed, sacred_changed, items_changed, names_changed, supplement_changed)):
            self.assess_quality("pets", current, prior_catalog)
            if read_object(self.catalog / COMPONENTS["pets"]) != current:
                atomic_json(self.catalog / COMPONENTS["pets"], current)
            return False
        # Build the complete component in memory; a rules failure cannot commit
        # a new dictionary together with stale or half-parsed power metadata.
        resources = dict(previous)
        if dictionary_changed:
            roots = []
            for label, key, selected in PET_RESOURCES:
                swf, resources[label] = self.resource(key, versions)
                emit("progress", message="正在整理精灵" + ("基础" if label == "petDictionary" else "增量") + "字典；首次更新可能需要几分钟")
                script = self.export(swf, self.scratch / label, selected)
                roots.append(script.parent)
            pets = merge_pet_sources(roots)
            if len(pets) < 9000 or len(pets) < len(current.get("pets", {})) * 0.9:
                raise ValueError("精灵字典不完整，保留旧字典")
            current["pets"] = pets
            current["petDictionarySchema"] = PET_DICTIONARY_SCHEMA
        if stars_changed:
            emit("progress", message="正在更新星神战力、搭配限制与职业规则")
            swfs = {}
            for label, key in POWER_RESOURCES[:3]:
                swfs[label], resources[label] = self.resource(key, versions)
            material = self.export(swfs["materialService"], self.scratch / "power-material", "mmo.material.stargod.StarGodItemService")
            jobs = self.export(swfs["interfaces"], self.scratch / "power-jobs", "mmo.interfaces.pet.data.PetJob")
            stars = parse_stargod_catalog(swfs["stargodService"], material, jobs)
            if len(stars) < 20 or len(stars) < len(current.get("stargods", {})) * 0.9:
                raise ValueError("星神规则不完整，保留旧精灵数据")
            current["stargods"] = stars
        if astrolabe_changed:
            emit("progress", message="正在更新星轮战斗力与节点规则")
            label, key = POWER_RESOURCES[3]
            swf, resources[label] = self.resource(key, versions)
            astrolabe = parse_astrolabe_catalog(swf)
            if len(astrolabe) < 20 or len(astrolabe) < len(current.get("astrolabe", {})) * 0.9:
                raise ValueError("星轮规则不完整，保留旧精灵数据")
            current["astrolabe"] = astrolabe
        if badges_changed:
            emit("progress", message="正在更新元魂等级与觉醒材料")
            label, key = CULTIVATION_RESOURCES[0]
            swf, resources[label] = self.resource(key, versions)
            badges = parse_badges(swf)
            if len(badges) < 20 or len(badges) < len(current.get("badges", {})) * 0.9:
                raise ValueError("元魂规则不完整，保留旧精灵数据")
            current["badges"] = badges
        if sacred_changed:
            emit("progress", message="正在更新源兽升星、升阶与对应源兽消耗")
            label, key = CULTIVATION_RESOURCES[1]
            swf, resources[label] = self.resource(key, versions)
            prefix = "mmo.sacredequipment.config.common."
            scripts = {name: self.export(swf, self.scratch / "cultivation-sacred", prefix + name)
                       for name in ("SE_SacredEquipmentConfig", "SE_StarUpgradeConfig", "SE_StageUpgradeConfig")}
            source_swf, resources["materialService"] = self.resource(POWER_RESOURCES[1][1], versions)
            source_items = self.export(source_swf, self.scratch / "cultivation-source-items", "mmo.material.equipment4pet.Equipment4PetItemService")
            sacred = parse_sacred(scripts["SE_SacredEquipmentConfig"], source_items)
            label, key, selected = SACRED_SOURCE_RESOURCE
            supplement_swf, resources[label] = self.resource(key, versions)
            supplement_script = self.export(supplement_swf, self.scratch / "source-equipment-names", selected)
            extra_names = equipment_source_names(supplement_script.read_text(encoding="utf-8-sig"))
            for item in sacred.values():
                extra_name = extra_names.get(str(item["sourceId"]))
                if not item.get("sourceName") and extra_name:
                    item["sourceName"] = extra_name
                    item["sourceNameKnown"] = True
                    item["sourceNameSource"] = "E4PV2_EInfos"
            if len(sacred) < 20 or len(sacred) < len(current.get("sacredEquipment", {})) * 0.9:
                raise ValueError("源兽规则不完整，保留旧精灵数据")
            current["sacredEquipment"] = sacred
            current["sacredStarPlans"] = parse_sacred_plans(scripts["SE_StarUpgradeConfig"], "Star")
            current["sacredStagePlans"] = parse_sacred_plans(scripts["SE_StageUpgradeConfig"], "Stage")
        if items_changed:
            emit("progress", message="正在更新元魂与星轮精华等材料名称")
            items = {}
            for index, (label, key) in enumerate(CULTIVATION_RESOURCES[2:]):
                swf, resources[label] = self.resource(key, versions)
                selected = "mmo.materialdata.ItemData" if index == 0 else "mmo.materialdata.update.ItemData_Update"
                script = self.export(swf, self.scratch / ("cultivation-items-" + str(index)), selected)
                parsed = parse_items(script.parent)
                if not parsed:
                    raise ValueError("材料名称表不完整，保留旧精灵数据")
                items.update(parsed)
            if len(items) < len(current.get("items", {})) * 0.9:
                raise ValueError("材料名称表不完整，保留旧精灵数据")
            current["items"] = items
        if names_changed:
            emit("progress", message="正在更新属性、职业组合与货币名称")
            update_names(self, current, versions, resources)
        if supplement_changed:
            emit("progress", message="正在更新源兽、传说石与潜能完整名称和说明")
            refresh_supplement(self, current, versions, resources)
        current["powerRuleVersion"] = POWER_RULE_VERSION
        current["cultivationRuleVersion"] = CULTIVATION_RULE_VERSION
        if not power_rules_valid(current):
            raise ValueError("战斗力规则结构不完整，保留旧精灵数据")
        if not cultivation_rules_valid(current):
            raise ValueError("养成材料规则结构不完整，保留旧精灵数据")
        current["source"] = {**current.get("source", {}), "kind": "aoqi-official-pet-dictionary",
                             "startVersion": self.start_version, "powerRuleVersion": POWER_RULE_VERSION,
                             "cultivationRuleVersion": CULTIVATION_RULE_VERSION,
                             "nameRuleVersion": NAME_RULE_VERSION,
                             "petDictionaryVersion": resources["petDictionaryUpdate"]["version"], "resources": resources}
        self.assess_quality("pets", current, prior_catalog)
        atomic_json(self.catalog / COMPONENTS["pets"], current)
        return True

    def shop(self, versions: dict) -> bool:
        from public_activity_exchange_updater import load_selectable_packages
        self.selectable_package_source = load_selectable_packages(self, versions)
        package_versions = self.selectable_package_source["versions"]
        package_hashes = self.selectable_package_source.get("dependencyHashes", {})
        original = self.current("shop")
        current = original
        # Parser upgrades also invalidate same-version caches: old catalogs
        # either omitted selectable rewards or included unrelated material rows.
        changed = ("shop" in self.force_components or not current.get("shops") or
                   not resource_matches(current.get("source", {}).get("resources", {}).get("shop", {}), RESOURCE, versions)
                   or current.get("source", {}).get("shopParserVersion") != 5
                   or current.get("source", {}).get("selectablePackageVersions", {}) != package_versions
                   or current.get("source", {}).get("selectablePackageHashes", {}) != package_hashes)
        if changed:
            swf, resource = self.resource(RESOURCE, versions)
            emit("progress", message="正在整理指定精灵兑换项目")
            script = self.export(swf, self.scratch / "shop", CONFIG_CLASS)
            source = {"clientVersion": self.start_version, "shopVersion": resource["version"], "shopParserVersion": 5, "resources": {"shop": resource},
                      "selectablePackageVersions": package_versions,
                    "selectablePackageHashes": package_hashes,
                      "configSha256": digest(script), "extractor": "JPEXS 26.2.1; public_data_updater.py"}
            current = parse_config(script.read_text(encoding="utf-8-sig"), source, include_manual=True,
                                   selectable_packages=self.selectable_package_source["packages"])
        try:
            changed = self.activity_exchanges(versions) or changed
        except Exception as error:
            self.activity_exchange_failures.append(str(error))
        activity = read_object(self.catalog / "activity-exchange-data.json")
        if activity.get("schema") == 1 and isinstance(activity.get("shops"), list):
            if any(not isinstance(shop, dict) or not shop.get("sourceKey") for shop in activity["shops"]):
                self.activity_exchange_failures.append("活动目录来源标识无效，保留已有活动")
            else:
                current = {**current, "shops": [shop for shop in current.get("shops", []) if not shop.get("sourceKey")] + activity["shops"],
                           "activitySource": {key: activity.get("source", {}).get(key) for key in
                               ("kind","configVersion","configResource")}, "activityCoverage": activity.get("coverage", {}),
                           "activityPending": activity.get("pending", [])}
        elif changed:
            # A newly updated SEF module must not discard previously saved
            # activities merely because their independent refresh failed.
            current = {**current, "shops": [shop for shop in current.get("shops", []) if not shop.get("sourceKey")] +
                       [shop for shop in original.get("shops", []) if shop.get("sourceKey")]}
        from generate_shop_exchange_data import relevant_shops
        from activity_reward_structures import upgrade_cultivation_description
        support = activity.get('source', {}).get('rewardSupport', {})
        current = {**current, 'shops': relevant_shops(current.get('shops', [])),
                   'rewardDescriptions': {key: support.get(key, {}) for key in ('descriptions', 'descriptionParameters')}}
        for shop in current['shops']:
            for good in shop['goods']:
                good['description'] = upgrade_cultivation_description(good.get('description', ''), good.get('rewardRaw', ''),
                                                                      good.get('enhanceType', ''), support)
        changed = changed or current != original
        if read_object(self.catalog / COMPONENTS["shop"]) != current:
            atomic_json(self.catalog / COMPONENTS["shop"], current)
        return changed

    def activity_exchanges(self, versions: dict) -> bool:
        from public_activity_exchange_updater import update_activity_exchanges
        return update_activity_exchanges(self, versions)

    def skills(self, versions: dict) -> bool:
        from public_skill_updater import update_skills
        return update_skills(self, atomic_json, versions)

    def images(self, versions: dict) -> bool:
        # Filled from the official, versioned resource manifest; never infer a
        # race from a translated name or reuse another pet's picture.
        current = self.current("images")
        from public_icon_updater import loader_default
        default = loader_default(self, current)
        exceptions, rules = self.icon_exceptions(versions, current)
        faces = image_entries(versions, self.current("pets").get("pets", {}).keys(), exceptions,
                              default_version=default["defaultVersion"])
        if not faces:
            raise ValueError("官方清单中没有可识别的精灵图片资源，保留旧索引")
        revision = hashlib.sha256(json.dumps(faces, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        # This explicit check also installs the extractor once, making future
        # missing-picture requests possible without a surprise runtime download.
        self.tools.mkdir(parents=True, exist_ok=True)
        if self.runtimes is None:
            self.runtimes = (runtime_entry(self.runtime_tools, "java", self.scratch), runtime_entry(self.runtime_tools, "ffdec", self.scratch))
        if current.get("revision") == revision:
            if (current.get("source", {}).get("iconRules") != rules or
                    current.get("source", {}).get("loaderDefault") != default):
                current.setdefault("source", {})["iconRules"] = rules
                current["source"]["loaderDefault"] = default
                current["faceIdExceptions"] = exceptions
                atomic_json(self.catalog / COMPONENTS["images"], current)
            # Same index: only a revision change (or an earlier failed refresh,
            # which never rewrote its metadata) needs a download. Skip hashing
            # every cached picture on each repeated manual check.
            self.image_failures = refresh_cached_images(self.root, current, faces, verify_content=False)
            return False
        atomic_json(self.catalog / COMPONENTS["images"], {"schemaVersion": 1, "revision": revision,
                    "source": {"kind": "aoqi-official-resource-manifest", "startVersion": self.start_version,
                               "iconRules": rules, "loaderDefault": default},
                    "faceIdExceptions": exceptions, "images": {}, "faces": faces})
        # Only an explicit public update replaces a previously cached picture.
        # Missing pictures stay lazy; no mass pre-download of thousands of pets.
        self.image_failures = refresh_cached_images(self.root, current, faces)
        if self.image_failures:
            # Keep the usable source selection when a cached picture cannot be
            # refreshed. Successfully extracted new-version files stay separate
            # and may be reused on the next explicit attempt.
            atomic_json(self.catalog / COMPONENTS["images"], current)
            return False
        return True

    def icon_exceptions(self, versions: dict, current: dict) -> tuple[list[int], dict]:
        resource = "pet/petdataservice"
        previous = current.get("source", {}).get("iconRules", {})
        exceptions = current.get("faceIdExceptions")
        if "images" not in self.force_components and resource_matches(previous, resource, versions) and isinstance(exceptions, list):
            return exceptions, previous
        swf, source = self.resource(resource, versions)
        emit("progress", message="正在更新精灵外观规则")
        script = self.export(swf, self.scratch / "icon-rules", "mmo.pet.dataservice.config.PetIconConfig")
        match = re.search(r"AVAILABLE_FACEID_AFTER10000:Array\s*=\s*\[([\d,\s]*)\]", script.read_text(encoding="utf-8-sig"))
        if not match:
            raise ValueError("官方精灵外观规则无法识别，保留原图片索引")
        exceptions = [int(v.strip()) for v in match[1].split(",") if v.strip()]
        if len(exceptions) > 10000 or any(v < 10000 for v in exceptions):
            raise ValueError("官方精灵外观规则无效")
        return exceptions, source

    def prepare_resource_plan(self, selected):
        from public_resource_plan import workbench_dependencies, build_plan, recorded_dependencies
        workbench = workbench_dependencies()
        historical = []
        # Keep requirements already evidenced by either installed or embedded
        # catalogs. They remain explicitly historical until this run checks them.
        for component, filename in COMPONENTS.items():
            if filename.endswith('.json'):
                for directory in (self.baseline, self.catalog):
                    historical.extend(recorded_dependencies(read_object(directory / filename), component))
        for directory in (self.baseline, self.catalog):
            historical.extend(recorded_dependencies(read_object(directory / 'activity-exchange-data.json'), 'shop'))
        replay, failures = [], {}
        if 'battleReplay' in selected:
            try:
                from public_battle_replay_updater import prepare_replay_update, replay_dependencies
                replay = replay_dependencies(prepare_replay_update(self))
            except Exception as error:
                failures['battleReplay'] = str(error)
        self.public_resources.plan = build_plan(workbench, replay, historical)
        fixed = [row for row in self.public_resources.plan if 'declared-dependency' in row['sourceStatuses']]
        emit('progress', message='已建立工作台与回放的资源并集：' + str(len(fixed)) +
             ' 项固定依赖，官方动态引用继续追加；各自完整资料均保留')
        atomic_json(self.catalog / 'public-resource-manifest.json',
                    dict(self.public_resources.audit(), state='prepared', selected=selected, preparationErrors=failures))
        emit('progress', message='正在校验所选类别的完整资源并集，共用下载及版本快照')
        prepared = self.public_resources.prefetch([name for name in selected if name not in failures])
        failures.update(prepared)
        return failures

    def run(self, components=None) -> dict:
        selected = [name for name, _ in COMPONENT_LABELS if components is None or name in components]
        if not selected:
            raise ValueError("没有选择要更新的数据")
        self.resource_cache.clear()
        self.export_cache.clear()
        self.public_resources.reset()
        self.quality_results.clear()
        self.shared_dependency_failures.clear()
        self._prepared_replay = None
        self.image_failures = 0
        self.icon_failures = []
        self.activity_exchange_failures = []
        self.activity_exchange_pending = []
        self.stage('fetchingResources')
        preparation_errors = self.prepare_resource_plan(selected)
        # Every Flash consumer sees the same start.v/versiondata/u.f snapshot.
        # H5-only skills do not depend on the Flash endpoint being available.
        versions, version_error = {}, None
        if any(name not in ("skills", "battleReplay") for name in selected):
            try:
                versions = self.versions()
            except Exception as error:
                version_error = str(error)
        results = {}
        for name, label in COMPONENT_LABELS:
            if name not in selected:
                continue
            emit("progress", message="检查" + label)
            self.stage('parsing', name)
            self.public_resources.component = name
            try:
                if name in preparation_errors:
                    raise ValueError(preparation_errors[name])
                if name not in ("skills", "battleReplay") and version_error is not None:
                    raise ValueError(version_error)
                changed = getattr(self, name)(versions)
                results[name] = {"status": "updated" if changed else "unchanged"}
                if name == "images" and self.image_failures:
                    results[name]["error"] = f"{self.image_failures} 张图片更新失败，已保留原图；下次检查更新可重试"
                    results[name]['retryable'] = True
                if name == "icons" and self.icon_failures:
                    results[name]["error"] = f"{len(self.icon_failures)} 项图标未更新，已保留已有图片：" + "；".join(map(str,self.icon_failures[:3]))
                    results[name]['retryable'] = True
                if name == "pets":
                    catalog = self.current("pets")
                    results[name]["counts"] = {key: len(catalog.get(key, {})) for key in
                        ("pets","stargods","badges","sacredEquipment","astrolabe","items","attributes","jobs","money",
                         "sourceBeasts","legendStones","proficiencies")}
                    missing = [f"{value.get('name', key)}：对应源兽材料 {value.get('sourceId')} 待官方补名"
                               for key, value in catalog.get("sacredEquipment", {}).items() if not value.get("sourceName")]
                    if missing:
                        results[name]["pendingDefinitions"] = missing
                        results[name]["error"] = "仍有官方字段待补齐：" + "；".join(missing[:3])
                if name == "shop":
                    catalog = self.current("shop")
                    scanned_goods = [good for shop in catalog.get("shops", []) for good in shop.get("goods", [])]
                    results[name]["scanCounts"] = {"shops": len(catalog.get("shops", [])),
                        "automaticGoods": sum(not good.get("manualSelectionRequired") for good in scanned_goods),
                        "manualGoods": sum(bool(good.get("manualSelectionRequired")) for good in scanned_goods)}
                    activity_shops = [shop for shop in catalog.get("shops", []) if shop.get("sourceKey")]
                    activity_goods = [good for shop in activity_shops for good in shop.get("goods", [])]
                    results[name]["activityCounts"] = {
                        "shops": len(activity_shops), "goods": len(activity_goods),
                        "activityShopGoods": sum(good.get("exchangeKind") != "diamond" for good in activity_goods),
                        "diamondActivityGoods": sum(good.get("exchangeKind") == "diamond" for good in activity_goods),
                        "hudShops": sum(shop.get("activityEvidence") == "hud" for shop in activity_shops),
                        "recentReleaseShops": sum(shop.get("activityEvidence") == "recent-release" for shop in activity_shops),
                        "linkedShops": sum(shop.get("activityEvidence") == "referenced" for shop in activity_shops)}
                    supported = {"11","31","32","34","39","39$1","39$3","41","42","43","44","62","84","85","86","89","89$1","91","92","94","95"}
                    def supported_code(code):
                        if code in supported or code == "33":
                            return True
                        match = re.fullmatch(r"33\$([0-9]+)",code)
                        return bool(match and 1 <= int(match[1]) <= 2147483647)
                    pending = sorted({code.strip() for shop in catalog.get("shops", []) for good in shop.get("goods", [])
                                      for code in str(good.get("enhanceType", "")).split("-")
                                      if code.strip() and not supported_code(code.strip())})
                    if pending:
                        results[name]["unsupportedTypes"] = pending
                        results[name]["error"] = "奖励已识别，部分培养适用判断待适配：" + "、".join(pending[:16])
                    problems = list(self.activity_exchange_failures)
                    if problems:
                        results[name]['retryable'] = True
                    if self.activity_exchange_pending:
                        results[name]["pendingActivities"] = self.activity_exchange_pending
                        problems.append(f"{len(self.activity_exchange_pending)} 条活动兑换配置待识别")
                    if problems:
                        results[name]["error"] = "；".join(filter(None,[results[name].get("error"),*problems[:4]]))
                if name == "skills":
                    catalog = self.current("skills")
                    results[name]["counts"] = {key: len(catalog.get(key, {})) for key in
                                                ("pets", "skills", "entries", "buffs")}
                if name == "routines":
                    catalog = read_object(self.catalog / COMPONENTS[name])
                    results[name]["counts"] = catalog.get("source", {}).get("counts", {})
                    pending = catalog.get("unsupportedRules", [])
                    if pending:
                        results[name]["error"] = f"{len(pending)} 条日常活动规则待适配"
                if name == "battleReplay":
                    results[name].update(self.battle_replay_result)
                if name in self.quality_results:
                    results[name]["quality"] = self.quality_results[name]
                if name in self.shared_dependency_failures:
                    results[name]["dependencyFailure"] = self.shared_dependency_failures[name]
                emit("component", component=name, **results[name])
            except Exception as error:
                results[name] = {"status": "failed", "error": str(error), "retryable": True}
                if name in self.quality_results:
                    results[name]["quality"] = self.quality_results[name]
                emit("component", component=name, **results[name])
            self.stage('parsing', '', len(results), len(selected))
        atomic_json(self.catalog / 'public-resource-manifest.json',
                    dict(self.public_resources.audit(), state='finished', selected=selected,
                         preparationErrors=preparation_errors, components=results))
        # A partial update keeps the last known result of the parts it skipped.
        status_path = self.catalog / "public-update-status.json"
        merged = read_object(status_path).get("components", {})
        merged = {key: value for key, value in merged.items() if key in COMPONENTS and isinstance(value, dict)}
        merged.update(results)
        atomic_json(status_path, {"schema": 1, "startVersion": self.start_version, "checked": selected,
                    "components": merged,
                    "complete": len(merged) == len(COMPONENTS) and
                                all(v.get("status") != "failed" and not v.get("error") for v in merged.values())})
        return results

    def icons(self, versions: dict) -> bool:
        from public_icon_updater import update_icons
        return update_icons(self, versions)

    def routines(self, versions: dict) -> bool:
        from public_routine_updater import update_routines
        return update_routines(self, versions)

    def battleReplay(self, versions: dict) -> bool:
        # These optional scripts are exported only for replay/all updates.
        from public_battle_replay_updater import update_battle_replay
        changed, self.battle_replay_result = update_battle_replay(self)
        return changed


def image_entries(versions: dict, pet_ids=(), exceptions=(), *, default_version=None) -> dict:
    result = {}
    allowed = set(map(int, exceptions))
    faces = {int(value) for value in pet_ids if str(value).isdecimal() and int(value) > 0}
    for resource, version in versions.items():
        match = re.fullmatch(r"peticon/peticon([1-9]\d*)", resource)
        if match and re.fullmatch(r"\d{8,20}", version):
            faces.add(int(match[1]))
    for face in faces | allowed:
        # Official PetIconContainer.calcAvailableFaceId uses modulo 10000,
        # except IDs explicitly kept by current PetDataService.PetIconConfig.
        actual = face % 10000 if face >= 10000 and face not in allowed else face
        if actual <= 0:
            continue
        resource = f"peticon/peticon{actual}"
        version = versions.get(resource, default_version)
        if not isinstance(version, str) or not re.fullmatch(r"\d{1,20}", version):
            raise ValueError("图片默认版本未由官方加载器确认，保留旧索引")
        entry = {"swfUrl": OFFICIAL + f"{resource}~{version}.swf",
                 "symbol": f"mmo.pet.peticon{actual}_L", "revision": version}
        entry["sourceIdentity"] = picture_source_identity(entry)
        result[str(face)] = entry
    return result


def installed_runtime(root: Path, name: str) -> Path:
    definition = RUNTIMES[name]
    directory = root / "data-tools" / name
    entry = directory / definition["entry"]
    marker = read_object(directory / "verified.json")
    if (marker.get("archiveSha256") != definition["sha256"] or not entry.is_file()
            or marker.get("entrySha256") != digest(entry)):
        raise ValueError("请先在设置中点击“全部检查更新”或“精灵图片索引”，准备本地图片解析工具")
    return entry


def picture_entry(index: dict, visual_key: str) -> dict:
    if not re.fullmatch(r"[1-9]\d*_[0-9]+", visual_key):
        raise ValueError("invalid picture key")
    direct = index.get("images", {}).get(visual_key)
    if isinstance(direct, dict):
        return direct
    race, face = visual_key.split("_")
    selected = face if int(face) else race
    faces = index.get("faces", {})
    if selected in faces:
        return faces[selected]
    # The index contains explicit high-face exceptions. Unlisted high values
    # follow the official modulo rule and only use a known local base entry.
    fallback = str(int(selected) % 10000)
    return faces.get(fallback, {}) if int(selected) >= 10000 and int(selected) not in index.get("faceIdExceptions", []) else {}


def picture_source_identity(entry: dict) -> str:
    """Identity of one exact official symbol at one approved source version."""
    url, symbol, revision = entry.get("swfUrl", ""), entry.get("symbol", ""), entry.get("revision", "")
    match = re.fullmatch(r"https://aoqi\.100bt\.com/play/peticon/peticon([1-9]\d*)~(\d{1,20})\.swf", url)
    if not match or symbol != f"mmo.pet.peticon{match[1]}_L" or revision != match[2]:
        raise ValueError("unsupported official picture resource/symbol/version binding")
    identity = hashlib.sha256(json.dumps([url, symbol, revision], ensure_ascii=False,
                                        separators=(",", ":")).encode()).hexdigest()
    if entry.get("sourceIdentity", identity) != identity:
        raise ValueError("picture source identity mismatch")
    return identity


def picture_cache_path(root: Path, visual_key: str, entry: dict) -> Path:
    if not re.fullmatch(r"[1-9]\d*_[0-9]+", visual_key):
        raise ValueError("invalid picture key")
    return root / "images" / "pets" / "sources" / picture_source_identity(entry) / (visual_key + ".png")


def picture_cache_matches(path: Path, entry: dict, *, verify_content=True) -> bool:
    metadata = read_object(path.with_suffix(".json"))
    return (path.is_file() and metadata.get("url") == entry.get("swfUrl") and
            metadata.get("symbol") == entry.get("symbol") and metadata.get("revision") == entry.get("revision") and
            isinstance(metadata.get("pngSha256"), str) and
            re.fullmatch(r"[0-9a-f]{64}", metadata.get("pngSha256", "")) is not None and
            (not verify_content or metadata["pngSha256"] == digest(path)))


def commit_picture(raster: Path, output: Path, entry: dict, metadata: dict) -> None:
    """Publish one immutable PNG identity, or repair its recorded original bytes."""
    candidate_hash = digest(raster)
    previous = read_object(output.with_suffix(".json"))
    expected = previous.get("pngSha256", "") if (previous.get("url") == entry["swfUrl"] and
        previous.get("symbol") == entry["symbol"] and previous.get("revision") == entry["revision"]) else ""
    if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
        expected = ""
    if expected and expected != candidate_hash:
        raise ValueError("同一图片来源的内容已变化，保留原对象")
    existing = digest(output) if output.is_file() else ""
    if existing and existing != candidate_hash and expected != candidate_hash:
        raise ValueError("同一图片来源已有不同内容，保留原对象")
    output.parent.mkdir(parents=True, exist_ok=True)
    if existing != candidate_hash:
        # A private completed file can be linked exclusively without exposing a
        # partial PNG or overwriting a concurrent first publisher. Legacy input
        # is copied first, so modifying its old path cannot mutate this object.
        with tempfile.NamedTemporaryFile(prefix=".picture-", dir=output.parent, delete=False) as staged:
            temporary = Path(staged.name)
            with raster.open("rb") as incoming:
                shutil.copyfileobj(incoming, staged)
        try:
            if existing:
                # Only the digest previously committed for this exact source
                # may repair corruption; another official PNG is not a repair.
                os.replace(temporary, output)
            else:
                try:
                    os.link(temporary, output)
                except FileExistsError:
                    if digest(output) != candidate_hash:
                        raise ValueError("并发写入的图片内容不同，保留先写入的对象")
        finally:
            temporary.unlink(missing_ok=True)
    atomic_json(output.with_suffix(".json"), {**metadata, "sourceIdentity": picture_source_identity(entry),
                                             "pngSha256": candidate_hash})


def extract_image(root: Path, visual_key: str, *, replace=False, expected_source_identity=None,
                  expected_content_sha256=None, image_index_object=None) -> dict:
    from public_dataset_store import image_index
    index = image_index(root, image_index_object)
    entry = picture_entry(index, visual_key)
    if not entry:
        raise ValueError("本地图片索引未收录此外观，请在设置中点击“全部检查更新”")
    identity = picture_source_identity(entry)
    if expected_source_identity is not None and identity != expected_source_identity:
        raise ValueError("图片来源快照已变化，保留旧图片请求")
    race, face = map(int, visual_key.split("_"))
    selected = face or race
    actual = selected % 10000 if selected >= 10000 and selected not in index.get("faceIdExceptions", []) else selected
    if entry["symbol"] != f"mmo.pet.peticon{actual}_L":
        raise ValueError("图片资源与精灵外观编号不一致")
    output = picture_cache_path(root, visual_key, entry)
    if expected_content_sha256 is not None and not re.fullmatch(r"[0-9a-f]{64}", expected_content_sha256):
        raise ValueError("invalid expected picture content identity")
    if picture_cache_matches(output, entry):
        actual_hash = digest(output)
        if expected_content_sha256 and expected_content_sha256 != actual_hash:
            raise ValueError("图片内容与请求快照不同，保留原对象")
        return {"status": "cached", "path": str(output), "sourceIdentity": identity, "contentSha256": actual_hash}
    # Only a cache with the exact official source, symbol and content proof can
    # migrate. Unknown/name-derived files are retained, never renamed or guessed.
    legacy = root / "images" / "pets" / (visual_key + ".png")
    if picture_cache_matches(legacy, entry):
        metadata = read_object(legacy.with_suffix(".json"))
        if expected_content_sha256 and expected_content_sha256 != metadata["pngSha256"]:
            raise ValueError("图片内容与请求快照不同，保留原对象")
        commit_picture(legacy, output, entry, metadata)
        return {"status": "cached", "path": str(output), "sourceIdentity": identity, "contentSha256": metadata["pngSha256"]}
    url, symbol = entry.get("swfUrl", ""), entry.get("symbol", "")
    java, ffdec = installed_runtime(root, "java"), installed_runtime(root, "ffdec")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="picture-", dir=root / "data-tools") as temporary:
        scratch = Path(temporary)
        swf = scratch / "picture.swf"
        resource = fetch(url, swf)
        character = symbol_character_id(swf.read_bytes(), symbol)
        export = scratch / "export"
        command = [str(java), "-Xmx1024m", "-Djava.awt.headless=true", "-jar", str(ffdec),
                   "-selectid", str(character), "-select", f"{character}:1", "-ignorebackground", "-export", "sprite", str(export), str(swf)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=90, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if result.returncode:
            raise ValueError("官方精灵图片解析失败")
        images = list(export.rglob("*.png"))
        if len(images) != 1 or images[0].name != "1.png":
            raise ValueError("官方精灵图片帧缺失或不唯一")
        info = png_details(images[0])
        if expected_content_sha256 and expected_content_sha256 != info["sha256"]:
            raise ValueError("同版本图片内容与请求快照不同，保留原对象")
        if info["width"] * info["height"] > 16 * 1024 * 1024 or info["bytes"] > 16 * 1024 * 1024:
            raise ValueError("官方精灵图片过大")
        # An immutable object explicitly pins the old request, so a concurrent
        # current-pointer switch must not cancel that valid old snapshot.
        latest = picture_entry(image_index(root, image_index_object), visual_key)
        if latest != entry:
            raise ValueError("图片索引已更新，旧来源下载结果已忽略")
        commit_picture(images[0], output, entry, {"url": url, "revision": entry.get("revision", ""), "resource": resource,
                    "sourceIdentity": identity,
                    "symbol": symbol, "pngSha256": info["sha256"]})
    return {"status": "saved", "path": str(output), "sourceIdentity": identity, "contentSha256": info["sha256"]}


def refresh_cached_images(root: Path, previous: dict, faces: dict, *, verify_content: bool = True) -> int:
    directory = root / "images" / "pets"
    if not directory.exists():
        return 0
    new = {"faces": faces, "images": {}}
    failures = 0
    seen = set()
    for path in directory.rglob("*.png"):
        key = path.stem
        if not re.fullmatch(r"[1-9]\d*_[0-9]+", key) or key in seen:
            continue
        entry = picture_entry(new, key)
        old = picture_entry(previous, key)
        if not entry or not old or not picture_cache_matches(path, old, verify_content=False):
            continue
        seen.add(key)
        output = picture_cache_path(root, key, entry)
        if picture_cache_matches(output, entry, verify_content=verify_content):
            continue
        try:
            emit("progress", message="更新已缓存图片 " + key)
            extract_image(root, key, replace=True)
        except Exception as error:
            # Individual image failures never replace a valid old PNG.
            emit("imageFailed", visualKey=key, error=str(error))
            failures += 1
    return failures


@contextlib.contextmanager
def update_lock(root: Path):
    root.mkdir(parents=True, exist_ok=True)
    stream = (root / "data-update.lock").open("a+b")
    try:
        if stream.tell() == 0:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        stream.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", required=True, type=Path)
    parser.add_argument("--baseline", type=Path, default=Path(__file__).resolve().parent / "baseline")
    parser.add_argument("--extract-image", action="store_true")
    parser.add_argument("--visual-key")
    parser.add_argument("--image-source-identity")
    parser.add_argument("--image-content-sha256")
    parser.add_argument("--image-index-object")
    parser.add_argument("--prepare-only", action="store_true",
                        help="prepare a candidate for application by the running workbench")
    parser.add_argument('--attempt-id', help='existing preparation request identity from the workbench')
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--components", default="",
                        help="comma-separated subset of: " + ",".join(COMPONENTS) + " (default: all)")
    args = parser.parse_args()
    components = None
    if args.components.strip():
        components = {part.strip() for part in args.components.split(",") if part.strip()}
        unknown = components - set(COMPONENTS)
        if unknown:
            emit("finished", success=False, error="未知的数据类别：" + "、".join(sorted(unknown)), components={})
            return 1
    root = args.data_root.resolve()
    active_attempt = None
    try:
        if args.extract_image:
            result = extract_image(root, args.visual_key or "", replace=args.replace,
                                   expected_source_identity=args.image_source_identity,
                                   expected_content_sha256=args.image_content_sha256,
                                   image_index_object=args.image_index_object)
            emit("finished", success=True, **result)
            return 0
        with update_lock(root):
            from public_dataset_store import prepare_candidate, apply_candidate, ensure_migrated, begin_request
            request = begin_request(root, components, args.attempt_id)
            active_attempt = request['attempt']
            ensure_migrated(root)
            candidate = prepare_candidate(root, args.baseline.resolve(), components, emit=emit, attempt_id=active_attempt)
            complete = all(row.get("status") not in ("failed", "retained") and not row.get("error")
                           for row in candidate["results"].values())
            if args.prepare_only:
                emit("finished", success=True, candidatePassed=True, diskCommitted=False, sessionApplied=False,
                     complete=complete,
                     attempt=candidate["attempt"], revision=candidate["revision"],
                     message="候选资料已准备，等待工作台检查并应用", components=candidate["results"])
                return 0
            committed = apply_candidate(root, candidate)
            emit("finished", success=True, candidatePassed=True, diskCommitted=True, sessionApplied=False,
                 complete=complete,
                 attempt=candidate["attempt"], revision=candidate["revision"],
                 message="公共资料版本已保存；运行中的工作台需完成快照重载后才算应用", components=candidate["results"],
                 publication=committed)
            return 0
    except Exception as error:
        if active_attempt:
            from public_dataset_store import record_preparation_failure
            record_preparation_failure(root, active_attempt, error)
        emit("finished", success=False, error=str(error), components={})
        return 1


if __name__ == "__main__":
    sys.exit(main())
