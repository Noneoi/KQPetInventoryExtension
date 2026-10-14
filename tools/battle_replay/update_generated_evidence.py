"""Offline generators for a complete replay catalog update.

Every input path is explicit; this module never downloads. Individual generators
are pure readers; prepare_reference_and_race writes only a new staging directory.
The caller must first verify the current official package -> selected export
binding. ``approved_sha256`` is a logical-input-name -> reviewed consumer SHA256
map, NOT a map synthesized from newly downloaded files. This module rechecks the
exact bytes before retaining previously reviewed explanations. A data/consumer
class sharing one file is conservatively pinned as a whole until its new code
has been reviewed by the caller; this module never approves new code itself.

Use encode_evidence() for returned companion documents BEFORE handing their
paths to build_catalog. Generated companion source hashes cover those exact
bytes. No function reads old source-snapshots or a previous catalog implicitly.
The root updater owns network policy, current-package proofs, output activation,
and cleanup; relative input paths are relative only to the caller's CWD.
"""
from __future__ import annotations

from bisect import bisect_right
from collections import Counter
from html.parser import HTMLParser
import copy
import hashlib
import json
import math
from pathlib import Path
import re
from typing import Mapping
import xml.etree.ElementTree as ET

from extract_skill_tables import LiteralParser
from extract_pet_identity import (parse_skin_definitions, parse_multiple_form_groups,
                                  build_race_expansions)
from extract_replay_supplements import attach_title_resource_verification

MAX_INPUT_BYTES = 128 * 1024 * 1024
FORTUNE_INPUTS = (
    'qiyun_definition', 'suling_definition', 'qiyun_config', 'suling_config',
    'dictionary_definition', 'h5_qiyun', 'h5_suling', 'h5_suling_descriptions',
    'flash_capability_panel', 'flash_formation_tips', 'h5_aqlib',
    'h5_formationservice', 'h5_petpackage',
)
FORTUNE_CONSUMERS = (
    'qiyun_definition', 'suling_definition', 'qiyun_config', 'suling_config',
    'dictionary_definition', 'flash_capability_panel', 'flash_formation_tips',
    'h5_aqlib', 'h5_formationservice', 'h5_petpackage',
)
LEGEND_INPUTS = ('legend_config', 'legend_definition', 'icon_consumer', 'h5_legend', 'h5_aqlib')
LEGEND_CONSUMERS = ('legend_config', 'legend_definition', 'icon_consumer', 'h5_aqlib')
RACE_INPUTS = ('pets', 'carry_xml', 'skin_config', 'skin_service', 'skin_definition',
               'skin_summary', 'form_config', 'form_controller', 'race_util')
RACE_CONSUMERS = ('skin_service', 'skin_definition', 'skin_summary', 'form_controller', 'race_util')
REFERENCE_CONSUMERS = (
    'skill_icon', 'legend_stone_module', 'legend_pet', 'legend_controller', 'special_attributes',
    'legend_config', 'qiyun_config', 'suling_config', 'formation_tips', 'skill_config',
    'skill_desc_config', 'skill_adapter', 'battle_config', 'skill_desc_definition',
    'skill_definition', 'battle_fighter', 'huan_config',
)
REFERENCE_INPUTS = REFERENCE_CONSUMERS + (
    'pet_mechanisms', 'raw_skill_tables', 'consumer_package_provenance', 'verified_consumer_provenance',
)


def _reject_constant(value):
    raise ValueError('Nonfinite JSON constant: ' + value)


def encode_evidence(value: dict) -> bytes:
    """Canonical companion encoding used by generated file/hash references."""
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_bytes(path: Path) -> bytes:
    with path.open('rb') as stream:
        if path.stat().st_size > MAX_INPUT_BYTES:
            raise ValueError('Evidence input exceeds size limit: ' + str(path))
        data = stream.read(MAX_INPUT_BYTES + 1)
    if len(data) > MAX_INPUT_BYTES:
        raise ValueError('Evidence input exceeds size limit: ' + str(path))
    return data


class _Inputs:
    def __init__(self, paths: Mapping[str, Path], required, approvals, consumers):
        missing = sorted(set(required) - set(paths))
        if missing:
            raise ValueError('Missing explicit evidence inputs: ' + ', '.join(missing))
        self.paths = {key: Path(paths[key]).resolve(strict=True) for key in required}
        self.data = {key: _read_bytes(path) for key, path in self.paths.items()}
        self.approvals = {}
        for key in consumers:
            expected = approvals.get(key, '')
            if not isinstance(expected, str) or not re.fullmatch('[0-9a-fA-F]{64}', expected):
                raise ValueError('Missing reviewed consumer SHA256: ' + key)
            if _sha(self.data[key]) != expected.lower():
                raise ValueError('Unreviewed consumer bytes: ' + key)
            self.approvals[key] = self.source(key)

    def text(self, key):
        return self.data[key].decode('utf-8-sig')

    def json(self, key):
        return json.loads(self.text(key), parse_constant=_reject_constant)

    def source(self, key):
        return {'file': str(self.paths[key]), 'sha256': _sha(self.data[key])}


def _generated_source(path, value):
    return {'file': str(Path(path).resolve()), 'sha256': _sha(encode_evidence(value))}


def _require(text, *fragments):
    compact = re.sub(r'\s+', '', text)
    for fragment in fragments:
        if re.sub(r'\s+', '', fragment) not in compact:
            raise ValueError('Official consumer schema changed: ' + fragment)


def _literal_array(text, name):
    matches = list(re.finditer(r'\b' + re.escape(name) + r'\s*:\s*Array\s*=', text))
    if len(matches) != 1:
        raise ValueError('Missing or ambiguous literal array: ' + name)
    parser = LiteralParser(text, matches[0].end())
    value = parser.value()
    parser.space()
    if not isinstance(value, list) or text[parser.pos:parser.pos + 1] != ';':
        raise ValueError('Unsupported literal array: ' + name)
    return value


def _constructor_rows(text, class_name, source):
    result = []
    for match in re.finditer(r'\bnew\s+' + re.escape(class_name) + r'\s*\(', text):
        parser = LiteralParser(text, match.end())
        values = []
        while True:
            values.append(parser.value())
            parser.space()
            token = text[parser.pos:parser.pos + 1]
            if token == ')':
                parser.pos += 1
                break
            if token != ',':
                raise ValueError('Unsupported constructor at character ' + str(parser.pos))
            parser.pos += 1
        result.append({'arguments': values, 'rawConstructor': text[match.start():parser.pos],
                       'source': {**source, 'line': text.count('\n', 0, match.start()) + 1,
                                  'characterOffset': match.start()}})
    if not result:
        raise ValueError('No literal official constructors: ' + class_name)
    return result


def _fortune_kind(context, kind, title):
    prefix = 'qiyun' if kind == 'qiYun' else 'suling'
    class_name = 'QiYunEffectDefine' if kind == 'qiYun' else 'SulingEffectDefine'
    definition = context.text(prefix + '_definition')
    keys = _literal_array(definition, 'KEYS')
    labels = _literal_array(definition, 'CN_KEYS')
    expected_count = 6 if kind == 'qiYun' else 8
    if (len(keys) != expected_count or len(labels) != len(keys) or len(set(keys)) != len(keys)
            or not all(isinstance(value, str) and value for value in keys + labels)):
        raise ValueError('Unsupported fortune channel schema: ' + kind)
    plans = {}
    for row in _constructor_rows(context.text(prefix + '_config'), class_name, context.source(prefix + '_config')):
        args = row['arguments']
        count = len(keys)
        if len(args) != 1 + count * (4 if kind == 'suLing' else 2):
            raise ValueError('Unsupported fortune constructor arity: ' + kind)
        if type(args[0]) is not int or args[0] < 0 or str(args[0]) in plans:
            raise ValueError('Invalid or duplicate fortune plan ID')
        if not all(isinstance(value, str) for value in args[1:]):
            raise ValueError('Non-string fortune channel')
        channels = []
        for index, (key, label) in enumerate(zip(keys, labels)):
            threshold, effect = args[1 + index], args[1 + count + index]
            formula = args[1 + count * 2 + index] if kind == 'suLing' else None
            not_triggered = args[1 + count * 3 + index] if kind == 'suLing' else '未触发'
            channel = {'key': key, 'label': label, 'valueUnit': 'ratio' if index < 6 else 'points',
                       'thresholdRaw': threshold, 'thresholds': [float(x) for x in threshold.split('|')],
                       'effectRaw': effect, 'effects': effect.split('|'),
                       'formulaRaw': formula, 'formulas': formula.split('|') if formula is not None else None,
                       'notTriggeredRaw': not_triggered,
                       'sourcePath': f'new {class_name}(id={args[0]}).{key}'}
            if not all(math.isfinite(value) for value in channel['thresholds']):
                raise ValueError('Nonfinite fortune threshold')
            if (len(channel['thresholds']) != len(channel['effects']) or
                    (channel['formulas'] is not None and len(channel['thresholds']) != len(channel['formulas']))):
                raise ValueError('Fortune threshold/effect/formula count mismatch')
            channels.append(channel)
        plans[str(args[0])] = {'id': args[0], 'name': title + '方案 ' + str(args[0]),
                              'nameIsGeneratedLabel': True, 'channels': channels,
                              'source': row['source'], 'rawConstructor': row['rawConstructor'], 'rawArguments': args}
    return {'plans': plans, 'definitionSource': context.source(prefix + '_definition'),
            'configSource': context.source(prefix + '_config')}


def _compare_fortune_h5(context, kind, flash, key):
    h5 = context.json(key)
    if not isinstance(h5, dict):
        raise ValueError('Fortune H5 table must be an object: ' + key)
    errors = []
    for plan_id, plan in flash['plans'].items():
        candidate = h5.get(plan_id)
        if not isinstance(candidate, dict):
            errors.append({'planId': plan_id, 'error': 'Missing plan'})
            continue
        for channel in plan['channels']:
            fields = [('T', 'thresholdRaw'), ('E', 'effectRaw')]
            if kind == 'suLing':
                fields += [('C', 'formulaRaw'), ('NT', 'notTriggeredRaw')]
            for suffix, field in fields:
                if candidate.get(channel['key'] + suffix) != channel[field]:
                    errors.append({'planId': plan_id, 'field': channel['key'] + suffix,
                                   'flash': channel[field], 'h5': candidate.get(channel['key'] + suffix)})
    extra = sorted(set(h5) - set(flash['plans']))
    if errors or extra:
        raise ValueError('Fortune Flash/H5 mismatch: ' + json.dumps({'differences': errors, 'extraH5Plans': extra}, ensure_ascii=False))
    return {'source': context.source(key), 'differences': errors, 'extraH5Plans': extra, 'rawTable': h5}


def build_fortune_rules(inputs: Mapping[str, Path], *, approved_sha256: Mapping[str, str]) -> dict:
    """Return rules.json; required inputs/approvals are FORTUNE_INPUTS/CONSUMERS."""
    context = _Inputs(inputs, FORTUNE_INPUTS, approved_sha256, FORTUNE_CONSUMERS)
    qi = _fortune_kind(context, 'qiYun', '气运')
    su = _fortune_kind(context, 'suLing', '溯灵')
    field_map = {"norSkillNo": 15, "ultSkillNo": 16, "ultSuperSkillNo": 17,
                 "heroSkillNo": 18, "sign": 29, "largeSkillNo": 32,
                 "dragonJobSkillNo": 36, "yuanLiSkillNo": 37, "psychicsSkillNo": 38,
                 "legendSkillNo": 47, "transformSkillNo": 50, "qiYunType": 60}
    result = {
        "schemaVersion": 1,
        "scope": "Static extraction of the explicitly supplied official Flash resources, compared with the supplied H5 data. Descriptions and client display rules do not establish server Buff origin.",
        "qiYun": qi,
        "suLing": su,
        "petDictionaryBinding": {
            "source": context.source("dictionary_definition"),
            "createFields": {key: {"parameterIndex1Based": index, "arrayIndex0Based": index - 1}
                             for key, index in field_map.items()},
            "qiYunTypeMeaning": "Selects a QiYunEffectDefine or SulingEffectDefine plan; not a skill or battle Buff ID.",
            "eraSelection": {"isShenYun": {"sign": "神运"}, "isLingChu": {"sign": "灵初"},
                             "method": "Exact findSign matching of the parsed sign field, not name matching."},
            "explicitSkillAlias": {"shenYunSkillNo": "legendSkillNo", "evidenceLines": [1181, 1188]},
        },
        "clientSemantics": {
            "thresholdSelection": "Read thresholds in source order. Stop at the first threshold greater than the input value. The selected stage is the number of preceding thresholds <= input; equality qualifies. No sorting or interpolation.",
            "qiYunUntriggered": "Return the literal 未触发 when selected stage is zero; otherwise return effects[stage-1]. Throw if stage exceeds effect count.",
            "suLingUntriggered": "Return notTriggeredRaw immediately when selected stage is zero, before formula evaluation or placeholder replacement. An untriggered description can still describe the threshold condition; it is not proof the condition was met.",
            "suLingFormula": "For a selected stage, take formulas[stage-1]. Extract the suffix beginning at the first character matching [+-*/()0-9.]; prepend the numeric input. Apply the official arithmetic parser (+,-,*,/,parentheses). Empty suffix yields the input. Replace the first #V<stage># in effects[stage-1] by ceil(result*100).",
            "inputOrder": ["criticalRate", "antiCriticalRate", "hitRate", "evasionRate", "strikeRate", "blockRate", "strengthenPower", "resistPower"],
            "flashCapabilityPanel": "SpeAttrSubModule reads these eight PetBuild.battleProperty values; it displays separate base, stargod, badge and astrolabe property contributions.",
            "flashFormationTips": "NewFormationTipsSprite requires isLingChu and a non-null petBuild, selects suLing by qiYunType and formats eight property tips.",
            "scopeOfMath": "These are client description-selection/placeholder rules. The parser and text do not identify executed Buff IDs or reproduce server combat calculations.",
        },
        "clientDifferences": [
            {"area": "suLing threshold/effect length mismatch", "flash": "Keeps empty text if selected stage is beyond effects length, then proceeds to formula lookup.", "h5": "Throws a threshold/effect mismatch Error.", "impactOnCurrentTables": "None observed in the current extracted plans: all threshold/effect/formula lengths were checked."},
            {"area": "suLing description color", "flash": "Returns source HTML color as written.", "h5": "On the selected-stage path, replaces #00FFFF with #009999 after placeholder replacement."},
            {"area": "H5 capability-panel inputs for indices 6/7", "flash": "Uses strengthenPower and resistPower.", "h5": "Current petpackageservice.js passes criticalRate for both index 6 and index 7 in its getDescByValueAndNumKey calls.", "interpretation": "Observed client discrepancy; do not copy it into a claim about server behavior or infer activation."},
        ],
        "h5Comparison": {"qiYun": _compare_fortune_h5(context, "qiYun", qi, "h5_qiyun"),
                         "suLing": _compare_fortune_h5(context, "suLing", su, "h5_suling")},
        "suLingSupplementalRules": {"flashRaw": _literal_array(context.text("suling_config"), "RULE_STRS"),
                                   "h5Raw": context.json("h5_suling_descriptions"), "h5Source": context.source("h5_suling_descriptions")},
        "explicitBattleBuffLinks": [],
        "limitations": ["No plan-to-Buff ID field or operation is present in these extracted plan records.",
                        "The snapshot fields hp/maxHp/shp/vigour do not provide the six rate inputs or full equipment needed to evaluate these plans for a recorded fight.",
                        "Current official dictionary data can document a race's plan; it does not establish the data version or property values used by an older battle log."]
    }
    result['consumerEvidence'] = context.approvals
    dictionary = context.text('dictionary_definition')
    lines = []
    for name in ('legendSkillNo', 'shenYunSkillNo'):
        match = re.search(r'function\s+get\s+' + name + r'\s*\(', dictionary)
        if match is None:
            raise ValueError('Missing reviewed dictionary getter: ' + name)
        lines.append(dictionary.count('\n', 0, match.start()) + 1)
    result['petDictionaryBinding']['explicitSkillAlias']['evidenceLines'] = lines
    return result


LEGEND_FIELDS = ("id", "name", "desc", "trigger", "triggerParam", "prop", "battlePower", "descX", "iconFrame", "effectName", "effectValue")
LEGEND_H5_FIELDS = {"name": "n", "trigger": "t", "triggerParam": "tP", "prop": "p", "battlePower": "b", "descX": "dX", "iconFrame": "iF", "effectName": "eN", "effectValue": "eV"}


class HtmlTokens(HTMLParser):
    def __init__(self, value):
        super().__init__(convert_charrefs=True)
        self.tokens = []
        self.plain = []
        self.feed(value)
        self.close()

    def handle_starttag(self, tag, attrs):
        self.tokens.append(("start", tag, tuple(attrs)))
        if tag.lower() == "br":
            self.plain.append("\n")

    def handle_endtag(self, tag):
        self.tokens.append(("end", tag))

    def handle_startendtag(self, tag, attrs):
        self.tokens.append(("self", tag, tuple(attrs)))
        if tag.lower() == "br":
            self.plain.append("\n")

    def handle_data(self, data):
        self.tokens.append(("text", data))
        self.plain.append(data)


def build_legend_mechanics(inputs: Mapping[str, Path], *, approved_sha256: Mapping[str, str],
                           raw_tables_path: Path) -> tuple[dict, dict]:
    """Return (legend-mechanics, legend-raw-tables); write the second using
    encode_evidence at raw_tables_path before importing the first.
    Every official variant is retained; this never selects equipment tiers.
    """
    context = _Inputs(inputs, LEGEND_INPUTS, approved_sha256, LEGEND_CONSUMERS)
    text = context.text("legend_config")
    define_text = context.text("legend_definition")
    line_starts = [0] + [match.end() for match in re.finditer("\n", text)]
    config_source = context.source("legend_config")
    # Match current official constructor storage, not names inferred from H5 abbreviations.
    for index, field in enumerate(LEGEND_FIELDS, 1):
        _require(define_text, f"this._{field} = param{index};")
    definitions = {}
    total = 0
    for parsed in _constructor_rows(text, "LegendSkillDefine", config_source):
        offset, args = parsed["source"]["characterOffset"], parsed["arguments"]
        if len(args) != len(LEGEND_FIELDS):
            raise ValueError("Unsupported LegendSkillDefine arity")
        if type(args[0]) is not int or args[0] < 0:
            raise ValueError("Invalid legend ID")
        row = dict(zip(LEGEND_FIELDS, args))
        key = str(row["id"])
        row["rawParameters"] = args
        row["source"] = {**config_source, "line": bisect_right(line_starts, offset), "characterOffset": offset}
        if key not in definitions:
            definitions[key] = {"id": row["id"], "name": row["name"], "variants": []}
        definitions[key]["variants"].append(row)
        total += 1
    h5 = context.json("h5_legend")
    differences = []
    html_equivalent = 0
    plain_equivalent = []
    exact_difference_count = 0
    defaults = {"t": 1, "b": 0, "p": ""}  # AQLib module 560 DEFAULT_VALUE.
    for key, definition in definitions.items():
        h5_variants = h5.get(key)
        if not isinstance(h5_variants, list):
            differences.append({"id": key, "kind": "missing H5 definition"})
            continue
        if len(definition["variants"]) != len(h5_variants):
            differences.append({"id": key, "kind": "variant count", "flash": len(definition["variants"]), "h5": len(h5_variants)})
        for variant_index, (flash, packed) in enumerate(zip(definition["variants"], h5_variants)):
            expected = {"id": int(key), "desc": ""}
            expected.update({field: packed.get(short, defaults.get(short)) for field, short in LEGEND_H5_FIELDS.items()})
            for field in LEGEND_FIELDS:
                if flash[field] == expected[field]:
                    continue
                exact_difference_count += 1
                if field in ("desc", "descX") and isinstance(expected[field], str) and HtmlTokens(flash[field]).tokens == HtmlTokens(expected[field]).tokens:
                    html_equivalent += 1
                    continue
                if field in ("desc", "descX") and isinstance(expected[field], str) and "".join(HtmlTokens(flash[field]).plain) == "".join(HtmlTokens(expected[field]).plain):
                    plain_equivalent.append({"id": key, "variantIndex": variant_index, "field": field})
                    continue
                differences.append({"id": key, "variantIndex": variant_index, "field": field, "flash": flash[field], "h5": expected[field]})

    trigger_counts = Counter(str(row["trigger"]) for definition in definitions.values() for row in definition["variants"])
    variants_per_id = Counter(str(len(value["variants"])) for value in definitions.values())
    define_lines = [0] + [match.end() for match in re.finditer("\n", define_text)]
    trigger_start = define_text.index("      private static function triggerWithItemGroup")
    trigger_end = define_text.index("      public function get id", trigger_start)
    range_start = define_text.index("      public function isAvailable")
    selected_methods_start = text.index("      public static function getLegendSkillDefinesById")
    icon_lines = context.text("icon_consumer").splitlines()
    raw_tables = {"flash": {"source": config_source, "constructorFieldOrder": list(LEGEND_FIELDS)},
                  "h5": {"source": context.source("h5_legend"), "rawTable": h5}}
    result = {
        "schemaVersion": 1,
        "definitions": definitions,
        "sources": {"flashConfig": config_source, "flashDefine": context.source("legend_definition"), "flashIconConsumer": context.source("icon_consumer"), "h5Config": context.source("h5_legend"), "rawTables": _generated_source(raw_tables_path, raw_tables)},
        "report": {
            "namespace": "formation.LegendSkillConfig / LegendSkillDefine; separate from BattleConfig skill definitions",
            "definitionCount": len(definitions), "variantCount": total,
            "variantsPerDefinitionCounts": dict(sorted(variants_per_id.items())), "triggerCounts": dict(sorted(trigger_counts.items())),
            "order": "Every constructor row is retained, in official source order within each ID. Array position is not exported as a skill level.",
            "fieldOrder": list(LEGEND_FIELDS),
            "fieldSemanticsSource": "Current LegendSkillDefine constructor and getters; no field renamed or inferred from similar IDs.",
            "selectionSemantics": {
                "trigger1": "Sum slot.level for nonempty PetLegendStone slots whose getLengedItemDefine().group.id occurs in triggerParam before ':'.",
                "trigger2": "Sum slot.level for nonempty PetLegendStone slots whose defineId occurs in triggerParam before ':'.",
                "range": "The part after ':' is minimum-maximum (or a single value). isAvailable is inclusive at both ends.",
                "selection": "LegendSkillConfig computes levelSum using the first definition, then scans definitions from last to first and returns the first whose range contains that sum. Missing/null/unsupported equipment input can throw; it does not become maximum level.",
                "iconWithoutEquipment": "NewSkillIconHelperV2 uses the last entry for an unowned/unknown-loadout preview and passes selector 'last' to the legend service. This is a display fallback, not proof that an actual battle used that entry.",
                "replay": "All variants retained. No variant is selected for a recorded fight because its PetLegendStone equipment is not established here."
            },
            "h5Comparison": {"source": context.source("h5_legend"), "definitions": len(h5), "extraH5Ids": sorted(set(h5)-set(definitions)),
                             "exactFieldDifferences": exact_difference_count, "htmlTokenEquivalentFields": html_equivalent,
                             "plainTextEqualButMarkupDifferentFields": len(plain_equivalent),
                             "plainTextEqualButMarkupDifferentLocations": plain_equivalent,
                             "remainingDifferences": differences,
                             "normalization": "Report exact HTML differences separately from equal parsed tokens or equal text with different markup. Equal text does not assert identical styling. Original strings remain unchanged in Flash variants and the raw H5 table.",
                             "officialH5Defaults": defaults},
            "selectionCode": {**config_source, "firstLine": bisect_right(line_starts, selected_methods_start), "text": text[selected_methods_start:]},
            "triggerCode": {**context.source("legend_definition"), "firstLine": bisect_right(define_lines, trigger_start), "text": define_text[trigger_start:trigger_end]},
            "rangeCode": {**context.source("legend_definition"), "firstLine": bisect_right(define_lines, range_start), "text": define_text[range_start:]},
            "legacyConsumerCode": {**context.source("icon_consumer"), "firstLine": 1, "lastLine": len(icon_lines), "text": "\n".join(icon_lines)},
            "limitations": ["Dictionary legendSkillNo branches by official era tags: legacy legend uses this formation table; ShenYun/LingChu uses its shenYunSkillNo alias in BattleConfig for skill text.",
                            "These are equipment-conditioned official descriptions and property strings. No record identifies a battle Buff applier or proves a target unnamed Buff's origin."]
        }
    }
    result['consumerEvidence'] = context.approvals
    return result, raw_tables


def build_race_expansion_evidence(inputs: Mapping[str, Path], *,
        approved_sha256: Mapping[str, str], packages: Mapping[str, dict]) -> dict:
    """Return race-expansions.json from CURRENT pets/carry/skin/form tables.

    ``pets`` is the plain ID-keyed pets.json dictionary, not a prior catalog.
    ``packages`` must bind resource names to this run's verified package records
    (file, sha256, resource, version, url); the caller also verifies selected AS
    exports came from those packages. This is compatibility membership only,
    never canonical identity or a recorded battle's actual carry selection.
    """
    context = _Inputs(inputs, RACE_INPUTS, approved_sha256, RACE_CONSUMERS)
    bindings = {
        'skin_config': 'changepetskinv3/changepetskinservice',
        'skin_service': 'changepetskinv3/changepetskinservice',
        'skin_definition': 'changepetskinv3/changepetskinservice',
        'skin_summary': 'changepetskinv3/changepetskinservice',
        'form_config': 'multipleformpet/multipleformpetservice',
        'form_controller': 'multipleformpet/multipleformpetservice',
        'race_util': 'library/common',
    }
    for resource in set(bindings.values()):
        package = packages.get(resource, {})
        if (package.get('resource') != resource or not package.get('file') or
                not re.fullmatch('[0-9a-fA-F]{64}', package.get('sha256', '')) or
                _sha(_read_bytes(Path(package['file']))) != package['sha256'].lower()):
            raise ValueError('Current race-expansion package proof missing/mismatched: ' + resource)
    _require(context.text('race_util'),
        'param1=resetRealPetRaceIds(param1);param1=checkAddAllMultiFormIds(param1);returncheckAddAllSkinIds(param1);',
        'arrayTemp=skinSvc.getSwitchSkinPetRaceIds(raceId);', 'skinRaceId=int(arrayTemp[0]);',
        'arrayTemp=multipleFormPetService.getSwitchPetRaceIds(raceId);')
    _require(context.text('skin_service'), 'Cpsv3SkinSummary.getRaceIds(param1,true,true)')
    _require(context.text('form_controller'), 'return_loc2_.group;')
    skins = parse_skin_definitions(context.text('skin_config'))
    forms = parse_multiple_form_groups(context.text('form_config'))
    pets = context.json('pets')
    if not isinstance(pets, dict) or not pets:
        raise ValueError('Current pet dictionary is empty or invalid')
    race_ids = set(pets)
    xml = context.text('carry_xml')
    if '<!DOCTYPE' in xml.upper() or '<!ENTITY' in xml.upper():
        raise ValueError('Carry XML must contain table literals only')
    for row in ET.fromstring(xml).iter('s'):
        for field in ('c', 'ced'):
            if row.get(field):
                value = row.get(field).split('#')[0]
                if not re.fullmatch(r'[0-9]+', value):
                    raise ValueError('Invalid official carry race ID')
                race_ids.add(str(int(value)))
    if any(not isinstance(value, str) or not re.fullmatch(r'[0-9]+', value) for value in race_ids):
        raise ValueError('Invalid pet dictionary race ID')
    expansions = build_race_expansions(sorted(race_ids, key=int), skins, forms)
    evidence = [{**context.source(key), 'resource': resource,
                 'packageSha256': packages[resource]['sha256']} for key, resource in bindings.items()]
    return {'format': 1, 'byRaceId': expansions, 'sourceEvidence': evidence,
        'inputTables': [context.source('pets'), context.source('carry_xml')],
        'formGroups': forms,
        'scope': 'Sorted membership of official PetRaceUtil.getAllSkinAndPetRaceIds([seed]); public skin getter, first matching battle-form group, then skins. Not canonical identities, not actual equipped Carry pairs, not transitive form closure, not UI ordering.'}


def verify_title_resource_evidence(titles: dict, verification_path: Path) -> tuple[dict, list[dict]]:
    """Verify a caller-supplied CURRENT title inspection document, offline.

    Downloading the explicitly declared title IDs and generating their static
    exports is the root updater's job. It must bind those exports to current
    manifest-selected packages before calling. This checks recorded paths,
    hashes, sizes, IDs and classes through the existing verified parser; it
    never supplies missing semantic title names or carries over old inspection
    findings itself. Returns a copy and its checked source records.
    """
    path = Path(verification_path).resolve(strict=True)
    raw = _read_bytes(path)
    verification = json.loads(raw.decode('utf-8-sig'), parse_constant=_reject_constant)
    result = copy.deepcopy(titles)
    evidence = {'file': str(path), 'sha256': _sha(raw), 'bytes': len(raw)}
    sources = attach_title_resource_verification(result, verification, evidence)
    if result.get('resourceVerificationIssues') or result.get('verifiedResourceCount') != len(result['rows']):
        raise ValueError('Current title resource inspection is incomplete or mismatched')
    return result, sources


def build_reference_consumer_evidence(inputs: Mapping[str, Path], *,
        approved_sha256: Mapping[str, str], generated_utc: str,
        snapshot_provenance_path: Path) -> tuple[dict, dict, dict]:
    """Return (reference-consumers, missing-skill-exact-lookup, input-provenance).

    pet_mechanisms and raw_skill_tables MUST be this run's fresh table outputs.
    The caller writes input-provenance with encode_evidence at the supplied new
    snapshot_provenance_path; no previous snapshot is opened or copied here.
    Package manifests must contain this run's official URL/version/SHA/file
    records with paths already resolved to the current staging root.
    """
    if not isinstance(generated_utc, str) or not generated_utc:
        raise ValueError('An explicit generation timestamp is required')
    context = _Inputs(inputs, REFERENCE_INPUTS, approved_sha256, REFERENCE_CONSUMERS)
    paths = context.paths
    mechanism_path, raw_path = paths['pet_mechanisms'], paths['raw_skill_tables']
    mechanism, raw = context.json('pet_mechanisms'), context.json('raw_skill_tables')
    pets = mechanism['pets']
    snapshot_provenance = {'capturedUtc': generated_utc,
        'scope': 'This update run: exact supplied table outputs before attaching consumer findings.',
        'files': [context.source('pet_mechanisms'), context.source('raw_skill_tables')]}
    helper, stone_module = paths['skill_icon'], paths['legend_stone_module']
    lgs_pet, lgs_controller = paths['legend_pet'], paths['legend_controller']
    spe_attr, fighter = paths['special_attributes'], paths['battle_fighter']
    package_sources = {}
    for key in ('consumer_package_provenance', 'verified_consumer_provenance'):
        for package in context.json(key)['packages']:
            package_path = Path(package['file']).resolve(strict=True)
            if _sha(_read_bytes(package_path)) != package['sha256']:
                raise ValueError('Package provenance mismatch: ' + package['resource'])
            record = {name: package[name] for name in ('resource', 'url', 'version', 'sha256', 'file')}
            previous = package_sources.get(package['resource'])
            if previous is not None and previous != record:
                raise ValueError('Conflicting current package provenance: ' + package['resource'])
            package_sources[package['resource']] = record
    evidence = {}
    keys_by_path = {path: key for key, path in paths.items()}

    def add(identifier, path, markers=(), resource=None, label=''):
        key = keys_by_path[path]
        text = context.text(key)
        locations = []
        for marker in markers:
            offset = text.find(marker)
            if offset < 0:
                raise ValueError('Official consumer changed: ' + identifier + ' / ' + marker)
            locations.append({'line': text.count('\n', 0, offset) + 1, 'text': marker})
        row = {**context.source(key), 'label': label, 'locations': locations}
        if resource:
            row['package'] = package_sources[resource]
        evidence[identifier] = row
        return identifier

    add('primary-passive-selector', helper,
        ['if(param1.dicData.isLingChu())', 'else if(param1.dicData.isShenYun())',
         'else if(param1.dicData.isLegend())', 'var _loc9_:int = int(param1.dicData.legendSkillNo);'],
        'interact/interacthelper', '主技能图标按时代标记分流')
    add('passive-battle-lookup', helper,
        ['shenYunSkillNo = int(context.dicData.shenYunSkillNo);',
         'shenYunSkillDefine = context.battleConfig.getSkillDefineById(shenYunSkillNo);',
         'var _loc4_:int = int(param2.dicData.shenYunSkillNo);'],
        'interact/interacthelper', '神运/灵初被动说明使用战斗技能表')
    add('backpack-legend-selector', stone_module,
        ['if((this._info = param1) == null || !param1.dictData.isLegend())',
         '_loc4_.getLegendSkillDefineAvailable(param1.dictData.legendSkillNo,_loc3_)'],
        'petpackage/petpackage', '背包传说石模块要求传说标记')
    add('legacy-stone-list', lgs_pet,
        ['return LgsPet(rest[0]).dictData.legendSkillNo != 0 && !LgsPet(rest[0]).dictData.isShenYun();',
         'return Vector.<LgsPet>(param1.map(FIX_MAPPER).filter(FIX_FILTER));',
         '_loc1_.getLegendSkillDefineAvailable(this.dictData.legendSkillNo,_loc2_)'],
        'legend/legendservice', '独立传说石列表过滤较宽，不排除精英')
    add('legacy-stone-list-input', lgs_controller,
        ['if(petId == -1)', '_loc1_.getMyPets(PetDataType.TOTAL,false)',
         '_model.setPets(petLoc,LgsPet.valuesFrom(_loc2_));',
         '_model.setPets(petLoc,LgsPet.valuesFrom(param1));'],
        'legend/legendservice', '列表来自已拥有的背包/仓库精灵')
    add('legacy-table-lookup', paths['legend_config'],
        ['public static function getLegendSkillDefinesById(param1:int) : Array',
         'if(_loc4_.id == param1)', 'if(skillDefines.length == 0)',
         'throw new Error("No any defines for skill "'],
        'formation/formationservice', '旧传说定义按原编号精确查询；缺失报错，无同号技能表回退')
    add('legacy-icon-missing', helper,
        ['_loc11_ = _formationServcie.getLegendSkillDefinesById(_loc9_);',
         '_loc12_ = _loc11_[_loc11_.length - 1];', 'throw new Error("缺少传说技配置：" + _loc9_);'],
        'interact/interacthelper', '无养成资料时取旧传说最后方案，缺失仍报错')
    add('battle-skill-resolution', paths['skill_config'],
        ['var _loc2_:SkillDefine = this._id2define.getValue(param1);',
         '_loc3_ = this.skillDescConfigXML.getSkillDescDefineById(param1);',
         '_loc4_ = int(SkillDefine.getOrigSkillId(param1));', '_loc2_ = SkillDefine.wrapDescOnly(_loc3_);'],
        'battleconfig/battleconfig', '技能配置、说明与包装回退顺序')
    add('empty-description-fallback', paths['skill_desc_config'],
        ['_loc3_ = new XML(<s id="1" nm="" desc=""/>);', '_loc3_.@id = param1;',
         '_loc2_ = SkillDescDefine.parseDefine(_loc3_);'],
        'battleconfig/battleconfig', '缺少说明行时创建空说明包装')
    add('skill-text-routing', paths['skill_adapter'],
        ['if(SkillConfigSimpleAC.MAP.hasOwnProperty(_loc2_))',
         'return SkillConfigSimple3000.MAP[_loc2_];', 'return SkillConfigSimple6000.MAP[_loc2_];',
         'return SkillConfigSimple9999.MAP[_loc2_];'],
        'battleconfig/battleconfig', '文本按编号精确取四张内嵌 MAP，没有外部补表加载')
    add('skill-text-empty', paths['battle_config'],
        ['return optValue(SkillConfigAdapter.getSkillConfigObj(param1),"name","");',
         'return optValue(SkillConfigAdapter.getSkillConfigObj(param1),"desc","");'],
        'battleconfig/battleconfig', '未取得文本时名称和说明返回空字符串')
    add('description-name-getter', paths['skill_desc_definition'],
        ['return sBattleConfig.getSkillName(this._nId);', 'return sBattleConfig.getSkillDesc(this._nId);'],
        'library/interfaces', '说明对象取词条文本的真实出口')
    add('skill-wrapper-validity', paths['skill_definition'],
        ['public static function wrapDescOnly(param1:SkillDescDefine) : SkillDefine',
         'var _loc2_:SkillDefine = new SkillDefine();',
         'if(param1.actionFrame > 0 && param1.pos > 0 && (param1.effectFrame > 0 || param1.isSpine))',
         'if(!checkIsAutoChessSkillId(param1))'],
        'library/interfaces', '空包装不产生动作配置；非自走棋编号不改写')
    add('ordinary-attack-validation', fighter,
        ['if(!SkillDefine.checkSkillDefineAvailableInBattle(skillDefine))',
         'throw new Error("缺少技能配置 in skill-config.xml,skill id=" + skillDefine.id);'],
        'battle/battle', '普通战斗行动检查有效技能配置')
    add('normal-ultimate-selector', helper,
        ['param1.battleConfig.getSkillDefineById(param1.dicData.norSkillNo)',
         'if(param1.dicData.isGodJob())', 'if(param1.dicData.ultSkillNo > 0)',
         '_loc21_ = int(param1.dicData.ultSuperSkillNo);'],
        'interact/interacthelper', '普通技能直读；神职超杀优先普通超杀，零值才选超级超杀')
    add('hero-selector', helper, ['var _loc3_:Boolean = param1.dicData.heroSkillNo > 0;',
         'var _loc4_:int = int(param1.dicData.heroSkillNo);'],
        'interact/interacthelper', '英雄技能由字典正编号开启')
    add('huan-empty-array', paths['huan_config'],
        ['this.parseXMLConfig(this._xmlClass);', 'if(!this._id2Define.containsKey(param1))', 'return [];'],
        'battleconfig/battleconfig', '寰力量/异族审判共用表，缺编号返回空列表')
    add('alien-huan-presentation', helper,
        ['if(!context.dicData.isYiXingZu())',
         'huanDefines = context.battleConfig.getHuanDefine(context.dicData.huanSkillNo);',
         'title = effectDefine == null ? "异族审判" : effectDefine.name;'],
        'interact/interacthelper', '异形族空列表仅保留默认异族审判标题，不含具体效果')
    add('qiyun-null-lookup', paths['qiyun_config'],
        ['if(_loc3_.id == param1)', 'return null;'],
        'formation/formationservice', '气运精确查表，无编号时返回 null')
    add('qiyun-owned-presentation', helper,
        ['qiYunEffectDefine = _formationServcie.getQiYunEffectDefineById(qiYunType);',
         'if(context.petBriefInfo == null || context.petBriefInfo.ownership != 1)',
         'desc = qiYunEffectDefine.getDescByValueAndNumKey(i,propValue);'],
        'interact/interacthelper', '气运查表后仅本人精灵的属性提示读取阈值')
    add('suling-null-lookup', paths['suling_config'],
        ['if(_loc3_.id == param1)', 'return null;'],
        'formation/formationservice', '溯灵精确查表，无默认编号替代')
    add('suling-formation-presentation', paths['formation_tips'],
        ['if(!pddi.isLingChu())', 'if(this._formationPetInfo.petBuild == null)',
         'suLingEffectDefine = formationServcie.getSuLingEffectDefineById(pddi.qiYunType);'],
        'formation/formationservice', '布阵溯灵说明要求灵初及养成资料')
    add('suling-backpack-presentation', spe_attr,
        ['suLingEffectDefine = this._formationService.getSuLingEffectDefineById(this._petInfo.dictData.qiYunType);',
         'if(this._petInfo.dictData.isLingChu())',
         'suLingEffectDefine.getDescByValueAndNumKey(param2,_values[param2])'],
        'petpackage/petpackage', '背包同样按原溯灵编号读取')
    add('exact-table-snapshot', raw_path, label='本次已核四张 Flash 文本表、技能配置/说明及 H5 对应表')
    add('mechanism-table-snapshot', mechanism_path, label='本次精确引用与气运/溯灵/传说/寰力量表快照')
    evidence['table-snapshot-provenance'] = {**_generated_source(snapshot_provenance_path, snapshot_provenance),
        'label': '本次输入表的固定字节及哈希', 'locations': []}
    for key, name in (('consumer_package_provenance', 'consumer-package-provenance'),
                      ('verified_consumer_provenance', 'verified-consumer-provenance')):
        add(name, paths[key], label='官方 URL、清单版本、包哈希及静态解包记录')

    findings = {}
    def put(pet, item):
        binding = findings.setdefault(pet['raceId'], {
            'dictionarySourceSha256': pet['source']['sha256'],
            **{key: pet[key] for key in ('sign', 'legendReferenceNamespace', 'qiYunType', 'mechanismFamily')},
            'references': []})
        binding['references'].append(item)

    def branch(consumer, selection, lookup, label, summary, ids):
        return {'consumer': consumer, 'selection': selection, 'lookupResult': lookup,
                'label': label, 'summary': summary, 'evidenceIds': ids}

    skill_details = {}
    text_ids = {key: {row['id'] for row in value['rows']} for key, value in raw['flashTextMaps'].items()}
    config_ids = {row['id'] for row in raw['flashSkillConfig']['s']}
    desc_ids = {row['id'] for row in raw['flashSkillDesc']['rows']}
    for pet in pets.values():
        if pet['legendReferenceNamespace'] == 'unclassified':
            identifier = pet['rawLegendSkillNo']
            put(pet, {'field': 'legendSkillNo', 'id': identifier, 'namespace': 'unclassified',
                'status': 'unresolvedWithConsumerEvidence', 'label': '精英原始字段：命名空间仍待核实',
                'summary': '主技能图标和背包传说石模块不读取这类精英的传说字段；独立传说石列表过滤较宽，选中已拥有精灵时仍可能按旧传说表查询。同号战斗技能说明只保留候选，未证实就是此字段的战斗机制。',
                'evidenceIds': ['primary-passive-selector', 'backpack-legend-selector', 'legacy-stone-list', 'legacy-stone-list-input', 'legacy-table-lookup'],
                'branches': [
                    branch('primarySkillIcon', 'gated', 'notRead', '主技能图标：此分支不读取', '没有传说、神运或灵初标记，不进入这些技能说明分支。', ['primary-passive-selector']),
                    branch('backpackLegendStone', 'gated', 'notRead', '背包传说石：此分支不读取', '该模块要求传说标记。', ['backpack-legend-selector']),
                    branch('legacyStoneOwnedSelection', 'conditional', 'throwsMissingDefinition', '独立传说石列表：有条件可达', '列表只要求原字段非零且不是神运；若该精灵实际在背包或仓库并被选择，仍会精确查旧传说表，当前对应行未取得。', ['legacy-stone-list', 'legacy-stone-list-input', 'legacy-table-lookup'])]})
        elif pet['legendReferenceNamespace'] == 'legacyLegend' and not pet['legendResolved']:
            identifier = pet['otherReferences']['legendSkillNo']
            put(pet, {'field': 'legendSkillNo', 'id': identifier, 'namespace': 'legacyLegend',
                'status': 'unresolvedWithConsumerEvidence', 'label': '旧传说配置：已确认读取路径，定义未取得',
                'summary': '官方按原编号查询旧传说配置。拥有传说石时选择满足条件的方案，无养成资料时取最后方案；当前编号无匹配项，两条读取路径均无战斗技能同号回退。',
                'evidenceIds': ['primary-passive-selector', 'legacy-table-lookup', 'legacy-icon-missing', 'mechanism-table-snapshot'],
                'branches': [branch('legendSkillDefinition', 'selected', 'throwsMissingDefinition', '旧传说精确查表', '缺少定义时官方报配置错误；不能用同号普通技能代替。', ['legacy-table-lookup', 'legacy-icon-missing'])]})
        if 'huanSkillNo' in pet['otherReferences'] and not pet['huanResolved']:
            identifier = pet['otherReferences']['huanSkillNo']
            put(pet, {'field': 'huanSkillNo', 'id': identifier, 'namespace': 'huan',
                'status': 'unresolvedWithConfirmedEmptyFallback', 'label': '异族审判：官方返回空配置列表',
                'summary': '该精灵带异形族标记，原元力字段由官方 getter 转入寰力量共用表。当前编号查不到时返回空列表，界面保留默认“异族审判”标题；这不证明没有战斗效果，也没有取得具体名称、阈值或说明。',
                'evidenceIds': ['huan-empty-array', 'alien-huan-presentation', 'mechanism-table-snapshot'],
                'branches': [branch('huanDefinition', 'selected', 'emptyArray', '空列表回退', '官方没有按其他编号补取定义。', ['huan-empty-array', 'alien-huan-presentation'])]})
        if pet['mechanismFamily'] and not pet['planResolved']:
            family = pet['mechanismFamily']
            qiyun = family == 'qiYun'
            ids = ['qiyun-null-lookup', 'qiyun-owned-presentation'] if qiyun else ['suling-null-lookup', 'suling-formation-presentation', 'suling-backpack-presentation']
            put(pet, {'field': 'qiYunType', 'id': pet['qiYunType'], 'namespace': family,
                'status': 'unresolvedWithConsumerEvidence', 'label': ('气运' if qiyun else '溯灵') + '方案：原编号查表无匹配',
                'summary': ('官方会用原 qiYunType 查气运表，缺项返回 null；非本人或无精灵养成资料时不继续显示气运属性阈值。本人养成分支没有默认方案或空值回退。' if qiyun else
                            '官方按灵初标记读取溯灵表，并使用原 qiYunType；当前编号未匹配。布阵提示有养成资料门控，背包养成详情也按原编号读取，没有自动改成其他方案的分支。'),
                'evidenceIds': ids + ['mechanism-table-snapshot'],
                'branches': [branch('mechanismPlan', 'conditional', 'nullNoFallback', '查表为 null，不能替换成默认方案', '这里只确认客户端读取方式，不判定服务端是否实际启用该机制。', ids)]})
        for link in pet['skillLinks']:
            if link['resolved']:
                continue
            identifier = link['skillId']
            detail = skill_details.setdefault(identifier, {
                'flashConfigPresent': identifier in config_ids, 'flashDescPresent': identifier in desc_ids,
                'flashTextTables': [key for key, ids in text_ids.items() if identifier in ids],
                'h5ConfigPresent': identifier in raw['h5SkillConfig']['data'],
                'h5DescPresent': identifier in raw['h5SkillDesc']['data'], 'references': []})
            detail['references'].append({'raceId': pet['raceId'], 'field': link['field'], 'source': pet['source']})
            if detail['flashConfigPresent'] or detail['flashDescPresent'] or detail['flashTextTables']:
                raise ValueError('A formerly missing skill needs specific fallback analysis: ' + identifier)
            selector = ('hero-selector' if link['field'] == 'heroSkillNo' else
                        'passive-battle-lookup' if link['field'] == 'legendSkillNo' else 'normal-ultimate-selector')
            ids = [selector, 'battle-skill-resolution', 'empty-description-fallback',
                   'skill-text-routing', 'skill-text-empty', 'description-name-getter',
                   'skill-wrapper-validity', 'ordinary-attack-validation', 'exact-table-snapshot']
            put(pet, {'field': link['field'], 'id': identifier, 'namespace': 'battleSkill',
                'status': 'unresolvedWithConfirmedEmptyFallback', 'label': link['label'] + '：官方空包装回退',
                'summary': '当前官方技能配置、说明行及四张文本表均未取得此编号。查询会创建名称和说明为空的包装对象；它不是已补全的技能定义，也没有合法动作配置。若作为普通战斗出手技能播放，官方有效性检查会报缺少配置。',
                'evidenceIds': ids,
                'branches': [branch('battleSkillLookup', 'selected', 'emptyDescriptionWrapper', '空说明包装', '保留原编号，名称和说明为空；不按相似编号或精灵名补写。', ids[1:7]),
                    branch('ordinaryAttack', 'conditional', 'rejectedByAvailabilityGuard', '普通出手播放：配置无效', '该结论只针对普通行动进入 attack() 后的检查，不能据此认定字典对象永远不会出现或其他分支不会读取。', ['skill-wrapper-validity', 'ordinary-attack-validation'])]})

    counters = Counter(item['status'] for row in findings.values() for item in row['references'])
    summary = {'pets': len(findings), 'references': sum(len(row['references']) for row in findings.values()),
               'statuses': dict(counters), 'unclassifiedEliteReferences': sum(p['legendReferenceNamespace'] == 'unclassified' for p in pets.values()),
               'missingBattleSkillIds': len(skill_details), 'missingBattleSkillReferences': sum(len(row['references']) for row in skill_details.values()),
               'definitionsRecovered': 0}
    document = {'schemaVersion': 1, 'generatedUtc': generated_utc,
        'scope': '仅确认已查官方客户端的具体读取分支与缺失时行为，不证明整个官方不存在定义。空对象、空列表、被某界面门控排除均不等于该战斗机制已解析；不改变原始引用、resolved 或未决命名空间。',
        'summary': summary, 'evidence': evidence, 'pets': findings}
    document['consumerApprovals'] = context.approvals
    missing = {'tableFile': str(raw_path), 'tableSha256': _sha(context.data['raw_skill_tables']),
        'bySkillId': skill_details,
        'scope': 'No semantic name or action definition is synthesized from the official empty-wrapper fallback.'}
    return document, missing, snapshot_provenance


def prepare_reference_and_race(arguments: Mapping[str, Path], *, project_root: Path,
        approved_sources: Mapping[str, str], generated_utc: str,
        current_packages: Mapping[str, dict] | None = None) -> dict:
    """Rebuild dynamic evidence after this run refreshed dictionaries/tables.

    ``arguments`` uses build_catalog.build keyword names, including source,
    auxiliary, verified_source, mechanism_rules, legend_mechanics,
    reference_consumers and race_expansions. Returns a NEW arguments dictionary
    pointing to generated reference/race files. The old reference/race JSON is
    read ONLY as a locator for explicitly reviewed consumer files/package
    records; old bindings, table snapshots and negative conclusions are unused.

    ``approved_sources`` maps restored absolute AS paths to approved original
    byte hashes from the verified bundle. Every used consumer is checked again.
    ``current_packages`` can provide this run's resource -> package records;
    otherwise source/verified_source manifests take precedence over original
    consumer package records. Reused records keep their original provenance.
    The caller must verify unchanged consumer-package bytes before calling.

    Writes only a NEW project_root/generated-evidence directory. It must not
    already exist and must be in an isolated update staging root, never the
    active source or user catalog directory. This function does not activate a
    catalog, download data, or write outside that directory.
    """
    from extract_skill_tables import extract_skill_tables
    from extract_pet_mechanisms import extract_pet_mechanisms

    root = Path(project_root).resolve(strict=True)
    required = ('source', 'auxiliary', 'verified_source', 'mechanism_rules',
                'legend_mechanics', 'reference_consumers', 'race_expansions')
    if any(not arguments.get(key) for key in required):
        raise ValueError('Complete explicit catalog arguments are required')

    def contained(path):
        path = Path(path).resolve(strict=True)
        if not path.is_relative_to(root):
            raise ValueError('Evidence input is outside the isolated project root: ' + str(path))
        return path

    paths = {key: contained(arguments[key]) for key in required}

    def read_json(path):
        return json.loads(_read_bytes(contained(path)).decode('utf-8-sig'), parse_constant=_reject_constant)

    approvals = {str(Path(path).resolve()): digest for path, digest in approved_sources.items()}
    old_reference = read_json(paths['reference_consumers'])
    old_race = read_json(paths['race_expansions'])
    reference_locations = {
        'skill_icon': 'primary-passive-selector', 'legend_stone_module': 'backpack-legend-selector',
        'legend_pet': 'legacy-stone-list', 'legend_controller': 'legacy-stone-list-input',
        'special_attributes': 'suling-backpack-presentation', 'legend_config': 'legacy-table-lookup',
        'qiyun_config': 'qiyun-null-lookup', 'suling_config': 'suling-null-lookup',
        'formation_tips': 'suling-formation-presentation', 'skill_config': 'battle-skill-resolution',
        'skill_desc_config': 'empty-description-fallback', 'skill_adapter': 'skill-text-routing',
        'battle_config': 'skill-text-empty', 'skill_desc_definition': 'description-name-getter',
        'skill_definition': 'skill-wrapper-validity', 'battle_fighter': 'ordinary-attack-validation',
        'huan_config': 'huan-empty-array',
    }
    # Prefer the source tree refreshed for the current build, where present.
    source_class_paths = {
        'skill_config': 'battleconfig/scripts/mmo/battleconfig/data/SkillConfigXML.as',
        'skill_desc_config': 'battleconfig/scripts/mmo/battleconfig/data/SkillDescConfigXML.as',
        'skill_adapter': 'battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigAdapter.as',
        'battle_config': 'battleconfig/scripts/mmo/battleconfig/BattleConfig.as',
        'skill_desc_definition': 'interfaces/scripts/mmo/interfaces/battleconfig/define/SkillDescDefine.as',
        'skill_definition': 'interfaces/scripts/mmo/interfaces/battleconfig/define/SkillDefine.as',
        'battle_fighter': 'battle/scripts/mmo/battle/fighter/BattleFighter.as',
        'huan_config': 'battleconfig/scripts/mmo/battleconfig/data/HuanConfigXML.as',
    }
    reference_inputs = {}
    for key, identifier in reference_locations.items():
        path = contained(old_reference['evidence'][identifier]['file'])
        candidate = paths['source'] / source_class_paths[key] if key in source_class_paths else None
        if candidate is not None and candidate.is_file():
            path = contained(candidate)
        reference_inputs[key] = path
    race_names = {
        'skin_config': 'Cpsv3SkinConfig.as', 'skin_service': 'ChangePetSkinService.as',
        'skin_definition': 'Cpsv3Skin.as', 'skin_summary': 'Cpsv3SkinSummary.as',
        'form_config': 'MultipleFormPetConfig.as', 'form_controller': 'MultipleFormPetController.as',
        'race_util': 'PetRaceUtil.as',
    }
    race_inputs = {}
    for key, name in race_names.items():
        matches = [record['file'] for record in old_race['sourceEvidence'] if Path(record['file']).name == name]
        if len(matches) != 1:
            raise ValueError('Missing/ambiguous explicit race consumer locator: ' + key)
        race_inputs[key] = contained(matches[0])
    race_inputs['pets'] = contained(paths['auxiliary'] / 'pets.json')
    race_inputs['carry_xml'] = contained(paths['source'] / 'xml/mmo.battleconfig.data.CarryConfigXML__xmlClass.xml')

    def logical_approvals(inputs, names):
        result = {}
        for key in names:
            path = inputs[key]
            expected = approvals.get(str(path), '')
            if not isinstance(expected, str) or not re.fullmatch('[0-9a-fA-F]{64}', expected):
                raise ValueError('Source is not in the approved restored bundle: ' + str(path))
            if _sha(_read_bytes(path)) != expected.lower():
                raise ValueError('Consumer changed since approval: ' + key)
            result[key] = expected
        return result

    reference_approvals = logical_approvals(reference_inputs, REFERENCE_CONSUMERS)
    race_approvals = logical_approvals(race_inputs, RACE_CONSUMERS)
    packages = {}
    provenance_inputs = {}
    for key, identifier in (('consumer_package_provenance', 'consumer-package-provenance'),
                            ('verified_consumer_provenance', 'verified-consumer-provenance')):
        original = contained(old_reference['evidence'][identifier]['file'])
        document = read_json(original)
        provenance_inputs[key] = document
        for record in document['packages']:
            packages[record['resource']] = dict(record)
    required_packages = set(packages) | {
        'changepetskinv3/changepetskinservice', 'multipleformpet/multipleformpetservice', 'library/common'}

    def add_manifest(manifest_path, row_key, base):
        for record in read_json(manifest_path)[row_key]:
            if record.get('error') or record.get('resource') not in required_packages:
                continue
            if not record.get('file'):
                raise ValueError('Current package proof lacks an explicit file')
            item = dict(record)
            # Select the most specific current record first; source's core
            # packages can replace duplicate verified-source paths not retained
            # in the minimal bundle. Validate chosen paths below, after merging.
            item['file'] = str((base / item['file']).resolve())
            packages[item['resource']] = item

    add_manifest(paths['verified_source'] / 'flash-resolved/provenance.json', 'files', paths['verified_source'])
    add_manifest(paths['source'] / 'provenance.json', 'packages', paths['source'])
    if current_packages is not None:
        for resource, record in current_packages.items():
            if resource not in required_packages:
                continue
            if record.get('resource') != resource:
                raise ValueError('Current package key/resource disagreement: ' + resource)
            packages[resource] = dict(record)
    for resource, record in packages.items():
        record['file'] = str(contained(record['file']))
        if _sha(_read_bytes(Path(record['file']))) != record['sha256']:
            raise ValueError('Current package bytes do not match provenance: ' + resource)

    output = root / 'generated-evidence'
    if output.exists():
        raise ValueError('Generated-evidence directory must be new for every update')
    output.mkdir()

    def write(name, value):
        path = output / name
        path.write_bytes(encode_evidence(value))
        return path

    # These are current extraction results, never the old source-snapshots files.
    skills = extract_skill_tables(paths['source'])
    mechanisms = extract_pet_mechanisms(paths['auxiliary'], skills['skills'],
        paths['mechanism_rules'], paths['legend_mechanics'],
        paths['source'] / 'xml/mmo.battleconfig.data.HuanConfigXML__xmlClass.xml',
        consumer_evidence_path=None)
    reference_inputs['pet_mechanisms'] = write('pet-mechanisms.json', mechanisms)
    reference_inputs['raw_skill_tables'] = write('raw-skill-tables.json', skills['rawTables'])
    for key, original in provenance_inputs.items():
        records = [packages[row['resource']] for row in original['packages']]
        # Do not copy an old export command or timestamp onto new package bytes.
        document = {'packages': records,
                    'scope': 'Verified package inputs for this build; consumer source approval is recorded separately.'}
        reference_inputs[key] = write(key.replace('_', '-') + '.json', document)
    snapshot_path = output / 'input-table-provenance.json'
    reference, missing, snapshot = build_reference_consumer_evidence(reference_inputs,
        approved_sha256=reference_approvals, generated_utc=generated_utc,
        snapshot_provenance_path=snapshot_path)
    snapshot_path.write_bytes(encode_evidence(snapshot))
    reference_path = write('reference-consumers.json', reference)
    write('missing-skill-exact-lookup.json', missing)
    race = build_race_expansion_evidence(race_inputs, approved_sha256=race_approvals, packages=packages)
    race_path = write('race-expansions.json', race)
    result = dict(arguments)
    result.update(reference_consumers=reference_path, race_expansions=race_path)
    return result
