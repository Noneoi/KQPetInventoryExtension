#!/usr/bin/env python3
"""Build the manually reviewed, static supplement for the pet skill UI.

This tool is intentionally separate from ``public_skill_updater.py``.  Its
output is bundled into a release and is never replaced by the in-app game-data
updater.  Every mechanism note is assembled from text already present in the
official catalog; when the game only mentions a term in context, the note says
so instead of inventing a standalone definition.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path


TERM = re.compile(r"\[([^\[\]\r\n]{1,80})\]")
PARAMETER = re.compile(r"^(.*?)[·-]([^·-]+)$")
SENTENCE_BREAK = re.compile(r"(?<=[。；;！？!?])|\n+")
ROMAN_SUFFIX = re.compile(r"[·-]?[ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩIVX]+$", re.I)


THEMES = (
    ("伤害输出", ("伤害", "攻击", "暴击", "破击", "秒杀", "斩杀", "毁灭", "追击")),
    ("生存保护", ("生命", "灵盾", "护盾", "复生", "保命", "免疫", "减伤", "受伤降低", "恢复")),
    ("行动控制", ("无法行动", "无法出手", "控制", "眩晕", "混乱", "睡眠", "嘲讽", "禁用")),
    ("团队支援", ("己阵", "友方", "队友", "治疗", "增益", "气势", "全阵", "全体")),
    ("效果削弱", ("降低", "负面", "清除", "吸收", "封印", "崩甲", "无视", "减少")),
)

GENERIC_CORE_TERMS = {
    "灵初", "神运", "星迹", "天启", "启元", "传说", "超神", "神属", "神职",
}


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def unique(values):
    result = []
    seen = set()
    for value in values:
        value = str(value or "").strip()
        if value and value not in seen:
            seen.add(value)
            result.append(value)
    return result


def official_texts(catalog: dict) -> list[str]:
    """Return only displayable official descriptions, never generated notes."""
    values: list[str] = []

    for skill in catalog.get("skills", {}).values():
        if isinstance(skill, dict):
            values.append(skill.get("description", ""))
    values.extend(catalog.get("entries", {}).values())
    for buff in catalog.get("buffs", {}).values():
        if isinstance(buff, dict):
            values.extend((buff.get("description", ""), buff.get("effect", "")))
    for transform in catalog.get("transformSkills", {}).values():
        if isinstance(transform, dict):
            values.extend((transform.get("description", ""), transform.get("teamDescription", "")))

    for pet in catalog.get("pets", {}).values():
        if not isinstance(pet, dict):
            continue
        values.extend((pet.get("officialRole", ""), pet.get("strategy", "")))
        star_god = pet.get("starGodSkill", {})
        if isinstance(star_god, dict):
            values.append(star_god.get("description", ""))
        values.extend(value for value in pet.get("huan", []) if isinstance(value, str))
        for relation in pet.get("relations", []):
            for phase in relation.get("phases", []) if isinstance(relation, dict) else []:
                if isinstance(phase, dict):
                    values.append(phase.get("effect", ""))
        for key in ("summons", "carry"):
            for item in pet.get(key, []):
                if isinstance(item, dict):
                    values.extend((item.get("description", ""), item.get("effect", "")))
        for item in pet.get("almighty", []):
            if isinstance(item, dict):
                values.extend((item.get("heroDescription", ""), item.get("summonDescription", ""),
                               item.get("psychicsDescription", "")))

    def walk(value):
        if isinstance(value, str):
            values.append(value)
        elif isinstance(value, dict):
            for nested in value.values():
                walk(nested)
        elif isinstance(value, list):
            for nested in value:
                walk(nested)

    walk(catalog.get("lingchuRules", {}))
    walk(catalog.get("shenyunRules", {}))
    return unique(values)


def excerpts(text: str, term: str) -> list[str]:
    needle = f"[{term}]"
    if needle not in text:
        return []
    parts = [part.strip(" \t\r\n") for part in SENTENCE_BREAK.split(text) if part.strip()]
    matches = [part for part in parts if needle in part]
    return matches or [text.strip()]


def parameterized_entry(term: str, entries: dict) -> tuple[str, str, str] | None:
    match = PARAMETER.match(term)
    if match and match.group(1) in entries and "ARG" in str(entries[match.group(1)]):
        base, argument = match.group(1), match.group(2)
        return base, argument, str(entries[base])

    base = ROMAN_SUFFIX.sub("", term)
    if base != term and base in entries and "ARG" in str(entries[base]):
        return base, term[len(base):].lstrip("·-"), str(entries[base])
    return None


def build_mechanisms(catalog: dict, texts: list[str]) -> tuple[dict, dict]:
    entries = {str(key): str(value) for key, value in catalog.get("entries", {}).items()}
    references = sorted({match.group(1).strip() for text in texts for match in TERM.finditer(text)
                         if match.group(1).strip()})
    missing = [term for term in references if term not in entries]

    buff_by_name: dict[str, list[str]] = {}
    for buff in catalog.get("buffs", {}).values():
        if not isinstance(buff, dict):
            continue
        name = str(buff.get("name", "")).strip()
        evidence = unique((buff.get("description", ""), buff.get("effect", "")))
        if name and evidence:
            buff_by_name.setdefault(name, []).extend(evidence)

    result = {}
    for term in missing:
        parameterized = parameterized_entry(term, entries)
        if parameterized:
            base, argument, evidence = parameterized
            result[term] = {
                "text": evidence.replace("ARG", argument or "X"),
                "source": "parameterized-official-entry",
                "baseEntry": base,
                "argument": argument,
                "evidence": [evidence],
            }
            continue

        buff_evidence = unique(buff_by_name.get(term, []))[:4]
        if buff_evidence:
            result[term] = {
                "text": "\n".join(buff_evidence),
                "source": "official-buff",
                "evidence": buff_evidence,
            }
            continue

        needle = f"[{term}]"
        related = [(name, description) for name, description in entries.items() if needle in description]
        if related:
            chosen = related[:4]
            result[term] = {
                "text": "\n".join(f"「{name}」：{description}" for name, description in chosen),
                "source": "related-official-entry",
                "relatedEntries": [name for name, _ in chosen],
                "evidence": [description for _, description in chosen],
            }
            continue

        context = unique(excerpt for text in texts for excerpt in excerpts(text, term))
        explicit = [line for line in context if re.search(rf"\[{re.escape(term)}\]\s*[：:]", line)]
        chosen = (explicit or context)[:4]
        is_definition = bool(explicit)
        label = "对应技能原文：" if is_definition else "相关技能原文（未提供独立释义）："
        result[term] = {
            "text": label + "\n" + "\n".join(chosen),
            "source": "official-skill-definition" if is_definition else "official-skill-evidence",
            "evidence": chosen,
        }

    coverage = {
        "referenced": len(references),
        "officialDefinitions": len(set(references) & set(entries)),
        "supplemented": len(result),
        "uncovered": len([term for term in missing if not result.get(term, {}).get("evidence")]),
    }
    return result, coverage


def pet_skill_text(catalog: dict, pet: dict) -> tuple[list[int], str]:
    skills = catalog.get("skills", {})
    skill_ids = sorted({int(value) for value in pet.get("slots", {}).values()
                        if isinstance(value, int) and value > 0})
    descriptions = []
    for skill_id in skill_ids:
        skill = skills.get(str(skill_id), {})
        if isinstance(skill, dict) and skill.get("description"):
            descriptions.append(str(skill["description"]))
        transform = catalog.get("transformSkills", {}).get(str(skill_id), {})
        if isinstance(transform, dict):
            descriptions.extend((str(transform.get("description", "")),
                                 str(transform.get("teamDescription", ""))))
    star_god = pet.get("starGodSkill", {})
    if isinstance(star_god, dict):
        descriptions.append(str(star_god.get("description", "")))
    return skill_ids, "\n".join(value for value in descriptions if value)


def build_evaluations(catalog: dict) -> dict:
    result = {}
    for race_id, pet in catalog.get("pets", {}).items():
        if not isinstance(pet, dict) or "灵初" not in str(pet.get("signs", "")):
            continue
        skill_ids, text = pet_skill_text(catalog, pet)
        scored = []
        for order, (label, keywords) in enumerate(THEMES):
            score = sum(1 for keyword in keywords if keyword in text)
            if score:
                scored.append((-score, order, label))
        themes = [item[2] for item in sorted(scored)[:2]] or ["技能联动"]

        found_terms = [match.group(1).strip() for match in TERM.finditer(text)
                       if match.group(1).strip() not in GENERIC_CORE_TERMS]
        counts = Counter(found_terms)
        first_seen = {term: found_terms.index(term) for term in counts}
        mechanisms = sorted(counts, key=lambda term: (-counts[term], first_seen[term]))[:2]

        position = str(pet.get("position", "")).strip()
        location = f"定位为{position}" if position else "站位未单独标明"
        sentence = f"技能文字显示其{location}，重点偏向{'与'.join(themes)}"
        if mechanisms:
            sentence += "；主要围绕" + "、".join(f"「{term}」" for term in mechanisms) + "展开"
        sentence += "。实际触发顺序与数值以本页技能原文为准。"
        result[str(race_id)] = {
            "text": sentence,
            "petName": str(pet.get("name", "")),
            "skillIds": skill_ids,
            "themes": themes,
            "mechanisms": mechanisms,
        }
    return result


def build_supplement(catalog: dict) -> dict:
    texts = official_texts(catalog)
    mechanisms, coverage = build_mechanisms(catalog, texts)
    return {
        "schema": 1,
        "kind": "pet-skill-manual-supplement",
        "updatePolicy": "manual-static",
        "sourceVersion": str(catalog.get("source", {}).get("version", "")),
        "mechanisms": mechanisms,
        "mechanismCoverage": coverage,
        "evaluations": build_evaluations(catalog),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    supplement = build_supplement(load_json(args.catalog))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(supplement, ensure_ascii=False, separators=(",", ":")),
                           encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
