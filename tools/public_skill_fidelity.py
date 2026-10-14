#!/usr/bin/env python3
"""Independently bind the compact skill catalogue to supplied official JSON.

This module does not import the catalogue builder, trust a catalogue's own
source/evidence fields, download data, or certify the supplied files as current.
The public updater owns version resolution and extraction.  Reviewed consumer
anchors and the distinction between original text and local presentation
normalisation are recorded in RULES and in qa/source-fidelity-20261014.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import html
import json
from pathlib import Path
import re


MAX_INPUT_BYTES = 32 * 1024 * 1024
MAX_FINDINGS = 2000
_MISSING = object()

# Zero-based JSON indices, independently traced through the official
# PetDictionaryDataService.create(n[...]) call and AQLib module 737.
PET_COLUMNS = {
    "name": 1, "attributes": 8, "jobs": 9, "signs": 28,
    "strategy": 38, "position": 39, "quality": 53,
}
SLOT_COLUMNS = {"normal": 14, "ultimate": 15, "super": 16, "hero": 17,
                "large": 31, "dragonJob": 35, "yuanLi": 36, "psychics": 37,
                "fate": 46, "transform": 49}
ANIMATION_FIELDS = ("action", "effect", "jump", "shock", "black", "hideother", "movieType", "ef")

EVIDENCE = {
    "aqlib": {"url": "https://aoqi.100bt.com/h5/js/aqlib~2026093092452521.js",
              "sha256": "b8d10e7b4841fad5729174d46c4ebfd11ae16c46baeba2cf5589c8941232ed2c"},
    "dictionary": {"url": "https://aoqi.100bt.com/h5/js/services/petdictionarydataservice~2026070239666739.js",
                   "sha256": "ffb8ed682972b063df6384c4bd056248a34da420b39e8a3bde7db6ed4406e0c9"},
    "formation": {"url": "https://aoqi.100bt.com/h5/js/services/formationservice~2026073132857772.js",
                  "sha256": "08f9fe8111a727fbaa6d23921b3f63921276ef5f2a2cbf62c89515bf907697ab"},
    "battleconfig": {"sha256": "0fb38bedb59a49e8a7f370acf873e5782d43f3aca28495108e1647fde0809dae",
                     "resource": "js/services/battleconfig.js",
                     "url": "https://aoqi.100bt.com/h5/js/services/battleconfig~2026040933332041.js"},
}
RULES = {
    "skill-text": "AQLib module 480 SkillDefine.parseDefine: same skill ID, pipe columns 0=name, 1=description, 2=encoding. Empty name AND description omit the compact row.",
    "skill-animation": "AQLib module 480 and battleconfig module 23: exact skill ID indexes skill_config; preserve eight explicitly retained animation fields without default synthesis.",
    "buff-text": "AQLib module 471 BufferDefine.parseDefine: pipe columns 0=name, 1=uiMessage, 3=buffEffect. The local compact display omits unnamed rows and empty optional text; this does NOT classify real Buff/state semantics.",
    "pet-columns": "Dictionary service module 1 -> AQLib module 737 create: exact dictionary key/raceId and reviewed 67-column positions. Original dictionary orgName is not canonical skin identity resolution.",
    "pet-huan": "AQLib module 737 huanSkillNo uses yuanLi column 36 only for exact comma-separated signs 寰力量 or 异形族; SkillIcon calls getHuanDefine(huanSkillNo); battleconfig module 19 uses that exact key. Never race-ID suffix matching.",
    "pet-almighty": "AQLib SkillIcon calls getAlmightyDefine(dictionary.raceId); battleconfig module 10 keys by exact row.id. No modulo or form-group inheritance. AQLib module 469 supplies hn/hd/sn/sd/pn/pd/psci.",
    "pet-traits": "AQLib module 737 findSign splits the dictionary sign field by comma and uses exact token equality. The local six-tag display subset preserves its fixed display order; substring matches are forbidden.",
    "combo-pairs": "battleconfig module 16: skillId selects record; buffs and targetSkillIds split by comma and pair by identical index, whose order controls priority.",
    "entry-text": "battleconfig module 18 and AQLib EntryDefine: n is name, d is description. Check the compact name-to-description projection; duplicate nonidentical cleaned names require review.",
    "transform-columns": "formationservice modules 70/71: new TransformSkillDefine(...row): [1]name [2]desc [3]autoEffectDesc [4]effectParams [5]autoEffectParams [6]skillType. Existing output labels targets/effects retain these arrays, not inferred mechanics.",
    "raw-tables": "formationservice modules 9/11/12/13 load hero/lingchu/psychics/shenyun arrays or maps; AlmightyConfigParser reads almighty rows. Exact JSON retention checks values, types, object keys and array order; they do not establish the meaning of every raw field.",
    "display-text": "Local presentation contract only: unescape HTML/escaped quotes, turn br/newline escapes into newline, remove markup/color controls/#pet#, collapse horizontal whitespace and excessive blank lines, trim surrounding separators. No paraphrase, number replacement, or effect inference.",
}
INPUTS = {
    "dictionary": (("petdictionarydata.json/petdictionarydata.json", "pet/petdictionarydata.json"), dict),
    "skillText": (("battleconfig/skill_desc_config.json",), dict),
    "animations": (("battleconfig/skill_config.json",), dict),
    "buffs": (("battleconfig/buffer_config.json",), dict),
    "combos": (("battleconfig/combo_buffs_config.json",), dict),
    "entries": (("battleconfig/entry_config.json",), list),
    "transforms": (("formation/transformskillconfig.json",), dict),
    "heroSkills": (("formation/heroskillconfig.json",), dict),
    "psychicsSkills": (("formation/psychicsskillconfig.json",), dict),
    "lingchuRules": (("formation/lingchuskillsulingconfig.json",), dict),
    "shenyunRules": (("formation/shenyunskillqiyunconfig.json",), dict),
    "almightySkills": (("battleconfig/almighty-skill-config.json",), list),
    "huan": (("battleconfig/huan_skill_config.json",), dict),
}


def _ptr(*parts):
    return "/" + "/".join(str(v).replace("~", "~0").replace("/", "~1") for v in parts)


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _text(value):
    # This is deliberately independent of generate_pet_skill_data.clean_text.
    value = html.unescape(str(value or "").replace("\\'", "'").replace('\\"', '"'))
    value = re.sub(r"\$c\w?", "", value).replace("c$", "")
    value = re.sub(r"<br\s*/?>|\\n|\r?\n", "\n", value, flags=re.IGNORECASE)
    value = re.sub(r"<[^>]+>", "", value).replace("#pet#", "")
    value = re.sub(r"[ \t]+", " ", value)
    return re.sub(r"\n{3,}", "\n\n", value).strip(" ;；\n")


def _safe_value(value):
    if value is _MISSING:
        return {"missing": True}
    encoded = _canonical(value)
    if len(encoded) <= 240:
        return value
    return {"type": type(value).__name__, "sha256": hashlib.sha256(encoded.encode("utf8")).hexdigest(),
            "excerpt": encoded[:200], "characters": len(encoded)}


class _Audit:
    def __init__(self, root, catalog):
        self.root = Path(root).resolve()
        self.catalog = catalog
        self.inputs, self.values, self.findings = {}, {}, []
        self.counts = Counter()
        self.stats = defaultdict(Counter)
        self.field_stats = defaultdict(Counter)
        self.uncovered = Counter()
        self.correction_targets = []

    def issue(self, kind, path, rule, *, source=None, expected=_MISSING, actual=_MISSING, message=""):
        self.counts["errors"] += 1
        if len(self.findings) < MAX_FINDINGS:
            self.findings.append({"kind": kind, "path": path, "rule": rule, "severity": "error",
                                  "source": source, "expected": _safe_value(expected),
                                  "actual": _safe_value(actual), "message": message or {
                                      "row-missing": "官方对应记录在候选显示表中缺失",
                                      "row-unexpected": "候选显示表包含不符合本次官方原文投影的记录",
                                      "value-mismatch": "候选值与同编号官方原始记录的已核验投影不一致",
                                      "field-type-or-presence": "候选字段的存在性或类型与官方原文投影不一致",
                                      "array-length": "候选数组长度与官方原文投影不一致",
                                  }.get(kind, "本次原始表结构不满足已核验的字段映射，需复核资料")})

    def source(self, name, *parts):
        return {"input": name, "pointer": _ptr(*parts) if parts else "",
                "sha256": self.inputs.get(name, {}).get("sha256")}

    def read(self):
        def pairs(items):
            result = {}
            for key, value in items:
                if key in result:
                    raise ValueError("duplicate JSON object key: " + key)
                result[key] = value
            return result

        def reject_constant(value):
            raise ValueError("non-finite JSON value: " + value)

        for name, (alternatives, kind) in INPUTS.items():
            try:
                paths = [self.root / p for p in alternatives if (self.root / p).is_file()]
                if len(paths) != 1:
                    raise ValueError("missing or ambiguous input")
                p = paths[0]
                if not p.resolve().is_relative_to(self.root):
                    raise ValueError("input escapes extraction root")
                with p.open("rb") as stream:
                    raw = stream.read(MAX_INPUT_BYTES + 1)
                if len(raw) > MAX_INPUT_BYTES:
                    raise ValueError("input exceeds size limit")
                self.inputs[name] = {"file": p.relative_to(self.root).as_posix(), "path": str(p.resolve()),
                                     "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
                value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=pairs, parse_constant=reject_constant)
                if type(value) is not kind:
                    raise ValueError("unsupported input root type")
                self.values[name] = value
                self.inputs[name]["rows"] = len(value)
            except (OSError, ValueError, UnicodeError) as exc:
                self.issue("source-invalid", "/inputs/" + name, "raw-tables", message=str(exc))

    def compare(self, actual, expected, path, rule, source, field=None):
        field = field or path
        parts = path.split("/")
        normal = "/".join("*" if i == 2 or (i > 2 and part.isdecimal()) else part
                          for i, part in enumerate(parts))
        self.field_stats[normal]["checks"] += 1
        self.stats[rule]["checks"] += 1
        if type(actual) is not type(expected):
            self.stats[rule]["mismatches"] += 1
            self.field_stats[normal]["mismatches"] += 1
            self.issue("field-type-or-presence", path, rule, source=source, expected=expected, actual=actual)
            return
        if isinstance(expected, dict):
            for k in expected.keys() | actual.keys():
                self.compare(actual.get(k, _MISSING), expected.get(k, _MISSING), path + _ptr(k), rule, source, field)
        elif isinstance(expected, list):
            if len(actual) != len(expected):
                self.stats[rule]["mismatches"] += 1
                self.field_stats[normal]["mismatches"] += 1
                self.issue("array-length", path, rule, source=source, expected=len(expected), actual=len(actual))
            for i in range(min(len(actual), len(expected))):
                self.compare(actual[i], expected[i], path + _ptr(i), rule, source, field)
        elif actual != expected:
            self.stats[rule]["mismatches"] += 1
            self.field_stats[normal]["mismatches"] += 1
            self.issue("value-mismatch", path, rule, source=source, expected=expected, actual=actual)
        self.counts["fieldChecks"] += 1

    def table(self, name, keys, rule, source):
        actual = self.catalog.get(name, _MISSING)
        self.stats[rule]["tables"] += 1
        self.stats[rule]["expectedRows"] += len(keys)
        if type(actual) is not dict:
            self.issue("table-type-or-presence", _ptr(name), rule, source=source, expected={}, actual=actual)
            return {}
        expected_keys = set(keys)
        for key in expected_keys - actual.keys():
            self.issue("row-missing", _ptr(name, key), rule, source=source, expected={"rowPresent": True})
        for key in actual.keys() - expected_keys:
            self.issue("row-unexpected", _ptr(name, key), rule, source=source, actual=actual[key])
        return actual

    def mapping(self, name, expected, rule, source):
        actual = self.table(name, expected, rule, source)
        for key in expected.keys() & actual.keys():
            self.compare(actual[key], expected[key], _ptr(name, key), rule, source)

    def valid_rows(self, value, kind, source_name):
        if any(type(v) is not kind for v in value.values()):
            self.issue("source-row-type", "/inputs/" + source_name, "raw-tables", source=self.source(source_name))
            return False
        return True

    def run(self):
        if type(self.catalog) is not dict:
            self.issue("catalog-type", "", "raw-tables", expected={}, actual=self.catalog)
            return
        self.read()
        v = self.values
        if "skillText" in v and self.valid_rows(v["skillText"], str, "skillText"):
            expected = {}
            for key, raw in v["skillText"].items():
                parts = raw.split("|", 2)
                parts += [""] * (3 - len(parts))
                name, desc = _text(parts[0]), _text(parts[1])
                if not name and not desc:
                    self.counts["officialEmptySkillsOmitted"] += 1
                    continue
                expected[key] = {"name": name, "description": desc}
                if parts[2]:
                    expected[key]["encoding"] = parts[2]
                if "animations" in v and key in v["animations"]:
                    animation = v["animations"][key]
                    if type(animation) is not dict:
                        self.issue("source-row-type", _ptr("inputs", "animations", key), "skill-animation")
                    else:
                        expected[key]["animation"] = {f: animation[f] for f in ANIMATION_FIELDS if f in animation}
            actual = self.table("skills", expected, "skill-text", self.source("skillText"))
            for key in expected.keys() & actual.keys():
                row = actual[key]
                if type(row) is not dict:
                    self.compare(row, expected[key], _ptr("skills", key), "skill-text", self.source("skillText", key))
                    continue
                for field in set(expected[key]) | set(row):
                    rule = "skill-animation" if field == "animation" else "skill-text"
                    origin = "animations" if field == "animation" else "skillText"
                    self.compare(row.get(field, _MISSING), expected[key].get(field, _MISSING),
                                 _ptr("skills", key, field), rule, self.source(origin, key))

        if "buffs" in v:
            raw_buffs = v["buffs"].get("buff")
            if type(raw_buffs) is not dict or not self.valid_rows(raw_buffs, str, "buffs"):
                self.issue("source-buff-map", "/inputs/buffs/buff", "buff-text")
            else:
                expected = {}
                for key, raw in raw_buffs.items():
                    parts = raw.split("|")
                    if not _text(parts[0]):
                        self.counts["officialUnnamedBuffsOmitted"] += 1
                        continue
                    row = {"name": _text(parts[0])}
                    for field, index in (("description", 1), ("effect", 3)):
                        if len(parts) > index and _text(parts[index]):
                            row[field] = _text(parts[index])
                    expected[key] = row
                actual = self.table("buffs", expected, "buff-text", self.source("buffs", "buff"))
                for key in actual.keys() & expected.keys():
                    self.compare(actual[key], expected[key], _ptr("buffs", key), "buff-text", self.source("buffs", "buff", key))

        if "entries" in v:
            expected, origins = {}, {}
            for index, row in enumerate(v["entries"]):
                if type(row) is not dict or not all(type(row.get(f)) is str for f in ("n", "d")):
                    self.issue("source-entry-row", _ptr("inputs", "entries", index), "entry-text")
                    continue
                name, desc = _text(row["n"]), _text(row["d"])
                if name and desc:
                    if name in expected and expected[name] != desc:
                        self.issue("source-ambiguous-entry-name", _ptr("inputs", "entries", index), "entry-text")
                    expected[name], origins[name] = desc, index
            actual = self.table("entries", expected, "entry-text", self.source("entries"))
            for name in actual.keys() & expected.keys():
                self.compare(actual[name], expected[name], _ptr("entries", name), "entry-text", self.source("entries", origins[name], "d"))

        if "combos" in v:
            combos, expected = v["combos"].get("combo"), {}
            if type(combos) is not list:
                self.issue("source-combo-map", "/inputs/combos/combo", "combo-pairs")
            else:
                for index, row in enumerate(combos):
                    try:
                        if type(row) is not dict or type(row.get("skillId")) is not int or row["skillId"] <= 0:
                            raise ValueError("invalid source skill ID")
                        pairs = []
                        for field in ("buffs", "targetSkillIds"):
                            raw = row.get(field)
                            if type(raw) is not str or not re.fullmatch(r"[1-9][0-9]*(?:,[1-9][0-9]*)*", raw):
                                raise ValueError("invalid positional source IDs")
                            pairs.append([int(t) for t in raw.split(",")])
                        if len(pairs[0]) != len(pairs[1]):
                            raise ValueError("unequal positional source arrays")
                        key = str(row["skillId"])
                        if key in expected:
                            raise ValueError("duplicate source combo skill ID")
                        expected[key] = [{"buff": b, "skill": s} for b, s in zip(*pairs)]
                    except ValueError as exc:
                        self.issue("source-combo-row", _ptr("inputs", "combos", "combo", index), "combo-pairs", message=str(exc))
                self.mapping("combos", expected, "combo-pairs", self.source("combos", "combo"))

        if "transforms" in v:
            expected = {}
            for key, row in v["transforms"].items():
                if type(row) is not list or len(row) != 7 or type(row[0]) is not int or str(row[0]) != key:
                    self.issue("source-transform-row", _ptr("inputs", "transforms", key), "transform-columns")
                    continue
                if any(type(row[i]) is not str for i in (1, 2, 3)) or any(type(row[i]) is not list for i in (4, 5)) or type(row[6]) is not int:
                    self.issue("source-transform-field-type", _ptr("inputs", "transforms", key), "transform-columns")
                    continue
                expected[key] = {f: _text(row[i]) if i < 4 else row[i] for f, i in
                                 (("name", 1), ("description", 2), ("teamDescription", 3),
                                  ("targets", 4), ("effects", 5), ("type", 6))}
            actual = self.table("transformSkills", expected, "transform-columns", self.source("transforms"))
            for key in expected.keys() & actual.keys():
                self.compare(actual[key], expected[key], _ptr("transformSkills", key), "transform-columns", self.source("transforms", key))

        for table in ("heroSkills", "psychicsSkills", "lingchuRules", "shenyunRules", "almightySkills"):
            if table in v:
                self.stats["raw-tables"]["tables"] += 1
                self.stats["raw-tables"]["expectedRows"] += len(v[table])
                self.compare(self.catalog.get(table, _MISSING), v[table], _ptr(table), "raw-tables", self.source(table))
        self.pets()
        covered = {"pets", "skills", "buffs", "entries", "combos", "transformSkills", "heroSkills", "psychicsSkills", "lingchuRules", "shenyunRules", "almightySkills"}
        for key in self.catalog.keys() - covered - {"source", "schema"}:
            self.uncovered[_ptr(key)] += 1

    def pets(self):
        v = self.values
        if "dictionary" not in v:
            return
        dictionary = v["dictionary"]
        actual = self.table("pets", dictionary, "pet-columns", self.source("dictionary"))
        almighty = {}
        for index, row in enumerate(v.get("almightySkills", [])):
            if type(row) is not dict or not re.fullmatch(r"[1-9][0-9]*", str(row.get("id", ""))):
                self.issue("source-almighty-row", _ptr("inputs", "almightySkills", index), "pet-almighty")
                continue
            if any(field in row and type(row[field]) is not str for field in ("hn", "hd", "sn", "sd", "pn", "pd")):
                self.issue("source-almighty-text-type", _ptr("inputs", "almightySkills", index), "pet-almighty")
                continue
            key = str(row["id"])
            # IDs are strings in the official H5 table; parse only decimal text.
            psci = row.get("psci", "0")
            if not re.fullmatch(r"[0-9]+", str(psci)):
                self.issue("source-almighty-called-id", _ptr("inputs", "almightySkills", index, "psci"), "pet-almighty")
                continue
            value = {"id": int(key), "calledRaceId": int(psci)}
            value.update({out: _text(row.get(raw, "")) for out, raw in
                          (("heroName", "hn"), ("heroDescription", "hd"), ("summonName", "sn"),
                           ("summonDescription", "sd"), ("psychicsName", "pn"), ("psychicsDescription", "pd"))})
            if key in almighty:
                self.issue("source-almighty-duplicate-id", _ptr("inputs", "almightySkills", index), "pet-almighty")
            almighty[key] = (value, index)
        for race, row in dictionary.items():
            if type(row) is not list or len(row) != 67 or not re.fullmatch(r"[1-9][0-9]*", race) \
                    or type(row[0]) is not int or str(row[0]) != race:
                self.issue("source-dictionary-identity-or-columns", _ptr("inputs", "dictionary", race), "pet-columns")
                continue
            for index in (*SLOT_COLUMNS.values(), 59):
                if type(row[index]) is not int or row[index] < 0:
                    self.issue("source-dictionary-integer", _ptr("inputs", "dictionary", race, index), "pet-columns")
            for index in (*PET_COLUMNS.values(), 19, 20, 65):
                if type(row[index]) is not str:
                    self.issue("source-dictionary-text-type", _ptr("inputs", "dictionary", race, index), "pet-columns")
            pet = actual.get(race)
            if pet is None:
                continue
            if type(pet) is not dict:
                self.issue("pet-row-type", _ptr("pets", race), "pet-columns", actual=pet)
                continue
            for field, index in PET_COLUMNS.items():
                expected = str(row[index] or "") if field in ("attributes", "jobs") else _text(row[index])
                self.compare(pet.get(field, _MISSING), expected, _ptr("pets", race, field), "pet-columns", self.source("dictionary", race, index))
            expected_slots = {slot: row[index] for slot, index in SLOT_COLUMNS.items()}
            self.compare(pet.get("slots", _MISSING), expected_slots, _ptr("pets", race, "slots"), "pet-columns", self.source("dictionary", race))
            self.compare(pet.get("qiYunType", _MISSING), row[59], _ptr("pets", race, "qiYunType"), "pet-columns", self.source("dictionary", race, 59))
            entries = [x for x in str(row[65] or "").split("#") if x]
            self.compare(pet.get("skillEntries", _MISSING), entries, _ptr("pets", race, "skillEntries"), "pet-columns", self.source("dictionary", race, 65))
            star = {"name": _text(row[19]), "description": _text(row[20])} if row[19] else _MISSING
            self.compare(pet.get("starGodSkill", _MISSING), star, _ptr("pets", race, "starGodSkill"), "pet-columns", self.source("dictionary", race, 19))
            signs = str(row[28] or "").split(",")
            traits = [tag for tag in ("天觉者", "吞噬技", "携同技", "寰力量", "异形族", "双英雄技") if tag in signs]
            self.compare(pet.get("traits", _MISSING), traits, _ptr("pets", race, "traits"), "pet-traits", self.source("dictionary", race, 28))
            self.correction_targets.append((race, "traits", traits, "pet-traits", self.source("dictionary", race, 28), []))
            if "huan" in v:
                key = str(row[36]) if ("寰力量" in signs or "异形族" in signs) and type(row[36]) is int and row[36] > 0 else None
                expected = _MISSING
                if key is not None and key in v["huan"]:
                    huan_row = v["huan"][key]
                    if type(huan_row) is not list or any(type(x) is not str for x in huan_row):
                        self.issue("source-huan-row", _ptr("inputs", "huan", key), "pet-huan")
                    else:
                        expected = [_text(x) for x in huan_row if _text(x)]
                self.compare(pet.get("huan", _MISSING), expected, _ptr("pets", race, "huan"), "pet-huan", self.source("huan", key or ""))
                self.correction_targets.append((race, "huan", expected, "pet-huan", self.source("huan", key or ""),
                                                [self.source("dictionary", race, 28), self.source("dictionary", race, 36)]))
            if "almightySkills" in v:
                expected = [almighty[race][0]] if race in almighty else _MISSING
                origin = self.source("almightySkills", almighty[race][1]) if race in almighty else self.source("almightySkills")
                self.compare(pet.get("almighty", _MISSING), expected, _ptr("pets", race, "almighty"), "pet-almighty", origin)
                self.correction_targets.append((race, "almighty", expected, "pet-almighty", origin, [self.source("dictionary", race, 0)]))
            covered = set(PET_COLUMNS) | {"slots", "qiYunType", "skillEntries", "starGodSkill", "huan", "almighty", "traits"}
            for field in pet.keys() - covered:
                self.uncovered["/pets/*/" + field] += 1


def _value_digest(value):
    return None if value is _MISSING else hashlib.sha256(_canonical(value).encode("utf8")).hexdigest()


def audit_skill_fidelity(source_root, catalog, *, previous=None):
    """Return a read-only, bounded fidelity report; any mismatch fails passed.

    A pass covers only the listed projections.  See coverage.notChecked for
    association fields outside this audit and boundaries for the trust split.
    """
    audit = _Audit(source_root, catalog)
    audit.run()
    try:
        candidate_digest = hashlib.sha256(_canonical(catalog).encode("utf8")).hexdigest()
    except (TypeError, ValueError):
        candidate_digest = None
        audit.issue("catalog-not-json", "", "raw-tables")
    corrections = []
    if not audit.counts["errors"] and type(previous) is dict and type(previous.get("pets")) is dict:
        for race, field, expected, rule, source, support in audit.correction_targets:
            prior = previous["pets"].get(race)
            if type(prior) is not dict:
                continue
            before = prior.get(field, _MISSING)
            if (before is _MISSING) == (expected is _MISSING) and _value_digest(before) == _value_digest(expected):
                continue
            corrections.append({"path": _ptr("pets", race, field), "rule": rule,
                                "previousPresent": before is not _MISSING, "candidatePresent": expected is not _MISSING,
                                "previousValueSha256": _value_digest(before), "candidateValueSha256": _value_digest(expected),
                                "source": source, "supportingSources": support,
                                "reason": "Candidate equals the reviewed exact official mapping; previous value does not. This receipt applies only to this exact path and both bound values."})
    return {
        "schemaVersion": 1, "audit": "workbench-skill-source-fidelity",
        "passed": not audit.counts["errors"], "candidateContentSha256": candidate_digest,
        "summary": {**dict(audit.counts), "errors": audit.counts["errors"], "inputFiles": len(audit.inputs),
                    "ruleGroupsChecked": len(audit.stats), "findingsRecorded": len(audit.findings),
                    "findingsOmitted": max(0, audit.counts["errors"] - len(audit.findings))},
        "inputs": audit.inputs, "findings": audit.findings,
        "coverage": {"checked": [{"rule": key, "mapping": RULES[key], **dict(counts)} for key, counts in sorted(audit.stats.items())],
                     "fieldChecks": [{"path": key, **dict(counts)} for key, counts in sorted(audit.field_stats.items())],
                     "notChecked": [{"path": path, "rows": rows} for path, rows in sorted(audit.uncovered.items())]},
        "allowedCorrections": corrections,
        "rules": RULES, "reviewedConsumerEvidence": EVIDENCE,
        "boundaries": [
            "Input SHA values are computed from supplied files, not candidate metadata. The updater must separately verify official version, download provenance and extraction.",
            "This audit neither downloads current consumer scripts nor certifies that their interpretation has not changed since the reviewed hashes.",
            "Cleaned display text is a documented local projection of official text; empty official text and unnamed omitted Buff rows are valid. No effect or stack count is invented.",
            "Raw retained rule arrays are checked exactly, including order and JSON types; this is not proof of every field's combat meaning.",
            "Canonical skin-to-original identity resolution, server combat formulas, final animation fallback/timing, and fields in notChecked are outside this audit.",
        ],
    }


def proven_skill_corrections(source_root, candidate, previous):
    """Return only exact, source-proven correction receipts from a passing audit."""
    return audit_skill_fidelity(source_root, candidate, previous=previous)["allowedCorrections"]
