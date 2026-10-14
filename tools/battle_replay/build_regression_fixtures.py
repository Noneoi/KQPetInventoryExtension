"""Maintain reviewed real-report fixtures; never invoked by the data updater.

Preparation uses an already verified native preview and catalog. All originals
remain read-only. Expectations are frozen only with --freeze-observation after
the QtCore checker observes that same hash-pinned baseline, never a candidate.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


SOURCES = (
    ("team-execution", "20260924193432-1-fail.txt"),
    ("summon-and-sequence", "20260924194155-3-win.txt"),
    ("clone-entry", "20260924192435-86-win.txt"),
)
PLAYER_REFERENCE_FIELDS = frozenset(("cpn", "tpn", "pan", "pbn", "pn", "player"))
NUMERIC_PLAYER_REFERENCE_FIELDS = {"cpi": "cpn", "tpi": "tpn", "pi": "pn"}


def anonymous_player_number(alias: str) -> int:
    """A public role identity, never a hash or copy of the account number."""
    match = re.fullmatch(r"side([01])-player([0-9]+)", alias)
    if not match or int(match[2]) >= 100:
        raise ValueError("Unsupported anonymous player role")
    return 1000 + int(match[1]) * 100 + int(match[2])


def player_number(value: object, path: str) -> int:
    if type(value) is int:
        return value
    if isinstance(value, str) and re.fullmatch(r"-?(0|[1-9][0-9]*)", value):
        return int(value)
    raise ValueError("Player reference is not an exact integer at " + path)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def encode(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def sanitize(document: dict) -> tuple[dict, list[dict], dict[str, str]]:
    """Enumerated context-aware edits, with type checks and no combat coercion."""
    if not isinstance(document.get("br"), dict):
        raise ValueError("A fixture must be a reviewed ordinary battle envelope")
    result = {"br": copy.deepcopy(document["br"])}
    edits = [{"path": "/" + key, "operation": "remove-envelope-field"}
             for key in document if key != "br"]
    battle = result["br"]
    for key in ("bi", "ir"):
        if key in battle:
            del battle[key]
            edits.append({"path": "/br/" + key, "operation": "remove-battle-id-or-opening-decoration"})
    aliases: dict[str, str] = {}
    declared_numbers: dict[str, int] = {}
    for ti, team in enumerate(battle["or"]["team"]):
        if type(team["id"]) is not int or team["id"] not in (0, 1):
            raise ValueError("Expected the already reviewed two-side report shape")
        for pi, player in enumerate(team["players"]):
            old = player["name"]
            if not isinstance(old, str) or not old or old in aliases:
                raise ValueError("Player aliases must be unambiguous for these fixtures")
            alias = f"side{team['id']}-player{pi}"
            aliases[old] = alias
            if "id" in player:
                declared_numbers[old] = player_number(player["id"], f"/br/or/team/{ti}/players/{pi}/id")
            prefix = f"/br/or/team/{ti}/players/{pi}"
            player["name"] = alias
            edits.append({"path": prefix + "/name", "operation": "pseudonymize-player-reference"})
            for key in ("id", "nn", "items"):
                if key in player:
                    del player[key]
                    edits.append({"path": prefix + "/" + key, "operation": "remove-account-or-decoration-field"})
            for fi, fighter in enumerate(player["cts"]):
                # Raw game species names remain. Player custom nicknames do not.
                if "customName" in fighter:
                    del fighter["customName"]
                    edits.append({"path": f"{prefix}/cts/{fi}/customName", "operation": "remove-custom-nickname"})

    observed_numbers: dict[str, int] = {}

    def visit(value: object, path: str) -> None:
        if isinstance(value, dict):
            for number_key, name_key in NUMERIC_PLAYER_REFERENCE_FIELDS.items():
                if number_key not in value:
                    continue
                # Bare pi elsewhere can be a pet/material identifier. Only
                # pn/pi is a player pair; cpi/tpi always require their names.
                if name_key not in value and number_key == "pi":
                    continue
                name = value.get(name_key)
                if not isinstance(name, str) or name not in aliases:
                    raise ValueError("Unrecognized numeric player reference at " + path + "/" + number_key)
                number = player_number(value[number_key], path + "/" + number_key)
                if number <= 0:
                    continue  # Preserve the client's unknown/NPC sentinels.
                declared = declared_numbers.get(name, number)
                if declared > 0 and declared != number:
                    raise ValueError("Player name/number reference disagrees at " + path + "/" + number_key)
                if observed_numbers.setdefault(name, number) != number:
                    raise ValueError("Player number changes within one report at " + path + "/" + number_key)
                replacement = anonymous_player_number(aliases[name])
                value[number_key] = str(replacement) if isinstance(value[number_key], str) else replacement
                edits.append({"path": path + "/" + number_key, "operation": "pseudonymize-numeric-player-reference"})
            for key, item in value.items():
                child = path + "/" + key.replace("~", "~0").replace("/", "~1")
                if key in PLAYER_REFERENCE_FIELDS and isinstance(item, str):
                    if item == "" and key in ("pan", "pbn"):
                        continue  # An absent segment-side queue remains absent.
                    if item not in aliases:
                        raise ValueError(f"Unrecognized player reference at {child}")
                    value[key] = aliases[item]
                    edits.append({"path": child, "operation": "pseudonymize-player-reference"})
                else:
                    visit(item, child)
        elif isinstance(value, list):
            for index, item in enumerate(value):
                visit(item, f"{path}/{index}")
    visit(battle, "/br")
    return result, edits, aliases


def normalize_projection(report: dict, aliases: dict[str, str]) -> list:
    """Compare old native projections modulo the documented player alias edit."""
    def key(value):
        if isinstance(value, str):
            for old, new in aliases.items():
                for team in (0, 1):
                    prefix = f"{team}:{len(old)}:{old}:"
                    if value.startswith(prefix):
                        return f"{team}:{len(new)}:{new}:" + value[len(prefix):]
        return value

    def raw(value):
        if isinstance(value, dict):
            result = {k: aliases.get(v, v) if k in PLAYER_REFERENCE_FIELDS and isinstance(v, str) else raw(v)
                      for k, v in value.items()}
            for number_key, name_key in NUMERIC_PLAYER_REFERENCE_FIELDS.items():
                if number_key in value and name_key in value:
                    number = player_number(value[number_key], "/projection/" + number_key)
                    if number > 0:
                        replacement = anonymous_player_number(aliases.get(value[name_key], value[name_key]))
                        result[number_key] = str(replacement) if isinstance(value[number_key], str) else replacement
            return result
        if isinstance(value, list):
            return [raw(x) for x in value]
        return value

    def canon(value):
        return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))

    normalized = []
    for step in report["steps"]:
        rows = []
        for encoded in step["rows"]:
            row = json.loads(encoded)
            row[2], row[3], row[7] = key(row[2]), key(row[3]), raw(row[7])
            rows.append(canon(row))
        commands = [dict(command, target=key(command["target"])) for command in step["displayCommands"]]
        normalized.append({**{k: v for k, v in step.items() if k not in ("rows", "displayCommands")},
                           "rows": sorted(rows), "displayCommands": commands})
    return normalized


def projection_sha(steps: list) -> str:
    """The already frozen native raw-effect hash framing, including step paths."""
    digest = hashlib.sha256()
    for step in steps:
        digest.update((step["path"] + "\n").encode("utf-8"))
        for row in sorted(step["rows"]):
            digest.update((row + "\n").encode("utf-8"))
    return digest.hexdigest()


def numeric_identity_changes(before: object, after: object, path: str = "") -> list[dict]:
    """Prove all other values, types, keys and array order remain byte-semantic."""
    if type(before) is not type(after):
        raise ValueError("Privacy cleanup changed a value type at " + path)
    changes = []
    if isinstance(before, dict):
        if before.keys() != after.keys():
            raise ValueError("Privacy cleanup changed object fields at " + path)
        for key in before:
            child = path + "/" + key.replace("~", "~0").replace("/", "~1")
            if key in NUMERIC_PLAYER_REFERENCE_FIELDS and before[key] != after[key]:
                name_key = NUMERIC_PLAYER_REFERENCE_FIELDS[key]
                name = before.get(name_key)
                if not isinstance(name, str) or name != after.get(name_key):
                    raise ValueError("Privacy cleanup changed a player join at " + child)
                number = player_number(before[key], child)
                replacement = anonymous_player_number(name)
                expected = str(replacement) if isinstance(before[key], str) else replacement
                if number <= 0 or after[key] != expected or type(before[key]) is not type(after[key]):
                    raise ValueError("Privacy cleanup changed a non-player value at " + child)
                changes.append({"path": child, "operation": "pseudonymize-numeric-player-reference"})
            else:
                changes.extend(numeric_identity_changes(before[key], after[key], child))
    elif isinstance(before, list):
        if len(before) != len(after):
            raise ValueError("Privacy cleanup changed array size at " + path)
        for index, (left, right) in enumerate(zip(before, after)):
            changes.extend(numeric_identity_changes(left, right, path + "/" + str(index)))
    elif before != after:
        raise ValueError("Privacy cleanup changed a non-player value at " + path)
    return changes


def upgrade_numeric_identities(args) -> None:
    """Privacy-only maintenance of previously frozen reports; no native runs."""
    manifest_path = args.output / "manifest.json"
    manifest = json.loads(manifest_path.read_bytes())
    if manifest.get("privacySchema") == 2:
        raise ValueError("Numeric player identities were already migrated")
    staged = []
    for case in manifest["cases"]:
        path = args.output / case["file"]
        before_bytes = path.read_bytes()
        if sha(before_bytes) != case["sha256"]:
            raise ValueError("Existing frozen fixture does not match its manifest")
        original_bytes = (args.samples / case["source"]["name"]).read_bytes()
        if sha(original_bytes) != case["source"]["sha256"]:
            raise ValueError("Read-only original differs from its existing verified hash")
        before = json.loads(before_bytes)
        clean, edits, _aliases = sanitize(json.loads(original_bytes))
        normalized_before, _edits, _ = sanitize(before)
        if normalized_before != clean:
            raise ValueError("New privacy cleanup is not equivalent to the original verified report")
        changes = numeric_identity_changes(before, clean)
        if not changes:
            raise ValueError("Expected the reviewed numeric-reference privacy omission")
        projection_bytes = (args.legacy_projections / (case["id"] + "-sanitized.json")).read_bytes()
        projection = json.loads(projection_bytes)
        prior_effects = case["source"]["verifiedSanitizedEffectsSha256"]
        if (projection["sha256"] != case["sha256"] or projection.get("auditErrors") or
                projection["effectsSha256"] != prior_effects or projection_sha(projection["steps"]) != prior_effects):
            raise ValueError("Legacy native projection is not the verified old fixture observation")
        normalized_steps = normalize_projection(projection, {})
        fixture = (json.dumps(clean, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
        proof = {"schema": 1, "priorFixtureSha256": case["sha256"], "fixtureSha256": sha(fixture),
                 "normalizedStructureSha256": sha(encode(clean)), "onlyNumericPlayerReferencesChanged": True,
                 "unitIdentitiesPreserved": True, "nonIdentityValuesAndOrderPreserved": True,
                 "changedReferences": len(changes), "changes": changes,
                 "legacyNativeProjectionSha256": sha(projection_bytes), "priorEffectsSha256": prior_effects,
                 "normalizedLegacyEffectsSha256": projection_sha(normalized_steps),
                 "priorExpectedSha256": sha(encode(case["expected"])), "nativeRefreezePending": True}
        case["privacyMigration"] = proof
        case["sha256"], case["bytes"], case["privacyEdits"] = sha(fixture), len(fixture), edits
        staged.append((path, fixture))
    manifest["privacySchema"] = 2
    manifest["fixtureVersion"] = "20261014-real-v2"
    manifest["privacyRebaselinePending"] = True
    for path, data in staged:
        path.write_bytes(data)
    manifest_path.write_bytes(encode(manifest))
    print(json.dumps({"migratedCases": len(staged), "privacyRebaselinePending": True,
                      "proof": "Exact structural comparison permits only paired numeric player-reference substitutions"}))


def prepare(args) -> None:
    prior = json.loads(args.verified_corpus.read_bytes())
    catalog_bytes, exe_bytes = args.baseline_catalog.read_bytes(), args.baseline_preview.read_bytes()
    if prior.get("passed") is not True or sha(exe_bytes) != prior["previewExeSha256"]:
        raise ValueError("Baseline executable must match an already passed corpus report")
    if sha(catalog_bytes) != prior["catalogJsonSha256"]:
        raise ValueError("Baseline catalog must match that same verified corpus report")
    source_rows = {Path(row["file"]).name: row for row in prior["results"]}
    args.output.mkdir(parents=True, exist_ok=True)
    args.evidence.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["PATH"] = str(args.qt_bin) + os.pathsep + env.get("PATH", "")
    cases = []
    for identity, source_name in SOURCES:
        old = source_rows[source_name]
        source = args.samples / source_name
        source_bytes = source.read_bytes()
        if sha(source_bytes) != old["sha256"] or not old["passed"]:
            raise ValueError("Original real report differs from its verified receipt")
        clean, edits, aliases = sanitize(json.loads(source_bytes))
        fixture = (json.dumps(clean, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
        filename = identity + ".json"
        target = args.output / filename
        target.write_bytes(fixture)
        reports = []
        for suffix, path in (("original", source), ("sanitized", target)):
            output = args.evidence / f"{identity}-{suffix}.json"
            run = subprocess.run([str(args.baseline_preview), "--catalog-json", str(args.baseline_catalog),
                                  "--file", str(path), "--effect-projection", str(output)],
                                 env=env, capture_output=True, timeout=120,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            (args.evidence / f"{identity}-{suffix}.log").write_bytes(run.stdout + run.stderr)
            if run.returncode:
                raise ValueError(f"Verified native baseline failed for {identity}: {run.returncode}")
            reports.append(json.loads(output.read_bytes()))
        original, sanitized = reports
        if original["effectsSha256"] != old["effectsSha256"] or original["auditErrors"] or sanitized["auditErrors"]:
            raise ValueError("Legacy raw-invariant audit or already verified projection changed")
        if normalize_projection(original, aliases) != normalize_projection(sanitized, {}):
            raise ValueError("Privacy cleanup changed a combat projection beyond player aliases")
        if source.read_bytes() != source_bytes:
            raise ValueError("Original source changed during fixture preparation")
        cases.append({"id": identity, "file": filename, "sha256": sha(fixture), "bytes": len(fixture),
                      "source": {"name": source_name, "sha256": sha(source_bytes), "bytes": len(source_bytes),
                                 "verifiedOriginalEffectsSha256": original["effectsSha256"],
                                 "verifiedSanitizedEffectsSha256": sanitized["effectsSha256"],
                                 "cleaningProjectionEquivalent": True},
                      "privacyEdits": edits, "expected": {}})
    manifest = {
        "schemaVersion": 1, "projectionSchema": "qtcore-domain-v1", "fixtureVersion": "20261014-real-v2", "privacySchema": 2,
        "scope": {"source": "Three complete existing user-supplied real battle reports; not simulated battles",
                  "checks": ["raw numeric values and paths", "ordered action/moment/state projection",
                             "client presentation cues", "action reconstruction", "frozen snapshot navigation"],
                  "exclusions": ["live game login", "GUI animation timing", "all species and all future mechanisms",
                                 "server damage formulas", "group battle/story/rewards/opening items",
                                 "summon-skill entry branch: these reports supply summon records/display restoration, not a confirmed summon-skill execution"],
                  "mutableOfficialText": "Not frozen by historical names: validated separately against official source assignments"},
        "baseline": {"catalogJsonSha256": sha(catalog_bytes), "previewExeSha256": sha(exe_bytes),
                     "verifiedCorpusReportSha256": sha(args.verified_corpus.read_bytes()),
                     "legacyProjectionComparison": "Each original remains equal to its previous verified projection; sanitized projection differs only by documented player aliases"},
        "cases": cases,
    }
    (args.output / "manifest.json").write_bytes(encode(manifest))
    print(json.dumps({"cases": len(cases), "fixtureBytes": sum(c["bytes"] for c in cases),
                      "baselineCatalogSha256": sha(catalog_bytes)}, ensure_ascii=False))


def freeze(args) -> None:
    manifest_file = args.output / "manifest.json"
    manifest_bytes = manifest_file.read_bytes()
    manifest = json.loads(manifest_bytes)
    observation = json.loads(args.freeze_observation.read_bytes())
    if observation.get("mode") != "baseline-observation" or observation.get("passed") is not False:
        raise ValueError("Only an explicit non-approved maintenance observation may create golden expectations")
    if observation.get("observationComplete") is not True or observation.get("errors"):
        raise ValueError("Baseline observation did not pass its independent raw/rebuild checks")
    if observation["catalogSha256"] != manifest["baseline"]["catalogJsonSha256"]:
        raise ValueError("A candidate may never redefine the reviewed baseline")
    if observation["fixtureSetSha256"] != sha(manifest_bytes):
        raise ValueError("Observation does not cover exactly this manifest")
    observed = {row["id"]: row for row in observation["cases"]}
    for case in manifest["cases"]:
        row = observed[case["id"]]
        if not row["passed"] or row["fixtureSha256"] != case["sha256"]:
            raise ValueError("Incomplete or mismatched baseline case")
        expected = row["observed"]
        migration = case.get("privacyMigration")
        if migration and migration.get("nativeRefreezePending"):
            fixture_bytes = (args.output / case["file"]).read_bytes()
            if (sha(fixture_bytes) != case["sha256"] or case["sha256"] != migration["fixtureSha256"] or
                    sha(encode(json.loads(fixture_bytes))) != migration["normalizedStructureSha256"] or
                    sha(encode(case["expected"])) != migration["priorExpectedSha256"]):
                raise ValueError("Privacy identity-equivalence proof no longer matches the frozen inputs")
            if expected["effectsSha256"] != migration["normalizedLegacyEffectsSha256"]:
                raise ValueError("Native raw projection differs beyond the proven identity normalization")
            allowed = {"effectsSha256", "orderedProjectionSha256"}
            if set(expected) != set(case["expected"]) or any(
                    expected[key] != value for key, value in case["expected"].items() if key not in allowed):
                raise ValueError("Privacy cleanup changed non-identity battle state, cues, counts or coverage")
            migration["nativeRefreezePending"] = False
            migration["nativeObservationSha256"] = sha(args.freeze_observation.read_bytes())
            case["source"]["verifiedSanitizedEffectsSha256"] = expected["effectsSha256"]
        elif expected["effectsSha256"] != case["source"]["verifiedSanitizedEffectsSha256"]:
            raise ValueError("New native checker changed the old verified raw projection")
        case["expected"] = expected
    manifest["baseline"]["qtcoreObservationSha256"] = sha(args.freeze_observation.read_bytes())
    if manifest.get("privacyRebaselinePending"):
        manifest["privacyRebaselinePending"] = False
    manifest_file.write_bytes(encode(manifest))
    print(json.dumps({"frozenCases": len(manifest["cases"]), "fixtureSetSha256": sha(manifest_file.read_bytes())}))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--freeze-observation", type=Path)
    parser.add_argument("--upgrade-numeric-identities", action="store_true")
    parser.add_argument("--legacy-projections", type=Path)
    parser.add_argument("--verified-corpus", type=Path)
    parser.add_argument("--baseline-catalog", type=Path)
    parser.add_argument("--baseline-preview", type=Path)
    parser.add_argument("--samples", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--qt-bin", type=Path)
    args = parser.parse_args()
    if args.upgrade_numeric_identities:
        if not args.samples or not args.legacy_projections:
            parser.error("Identity migration requires read-only original samples and the already verified native projections")
        upgrade_numeric_identities(args)
    elif args.freeze_observation:
        freeze(args)
    elif all((args.verified_corpus, args.baseline_catalog, args.baseline_preview, args.samples, args.evidence, args.qt_bin)):
        prepare(args)
    else:
        parser.error("Preparation requires the pinned corpus, catalog, preview, source samples, Qt runtime and evidence directory")


if __name__ == "__main__":
    main()
