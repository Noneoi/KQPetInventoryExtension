"""Exercise the shipped QtCore gate with real fixtures and corrupted candidates.

This test changes copies of catalogs only. It never regenerates fixture golden
observations, downloads data, or writes into the user's active workbench.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib


def sha(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def encode(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    source = args.catalog.read_bytes()
    catalog = json.loads(source)
    fixture_manifest = json.loads(args.fixtures.read_bytes())
    expected_set = sha(args.fixtures.read_bytes())
    checks = []
    with tempfile.TemporaryDirectory(prefix="kq-replay-update-check-") as directory:
        work = Path(directory)

        def run(name: str, input_file: Path, should_pass: bool, *, manifest: Path | None = None,
                expected_json_sha: str | None = None) -> dict:
            output = work / (name + "-result.json")
            command = [str(args.checker), "--catalog", str(input_file), "--report", str(output)]
            if manifest is not None:
                command += ["--fixtures", str(manifest)]
            result = subprocess.run(command, capture_output=True, timeout=90,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            assert output.is_file(), (name, "report missing", result.returncode, result.stderr[:300])
            report = json.loads(output.read_bytes())
            assert report["schemaVersion"] == 1, name
            assert report["passed"] is should_pass, (name, report.get("errors"))
            assert (result.returncode == 0) is should_pass, (name, result.returncode)
            if expected_json_sha:
                assert report["catalogSha256"] == expected_json_sha, name
            if should_pass:
                assert report["fixtureSetSha256"] == expected_set, name
                assert report["caseCount"] == len(fixture_manifest["cases"]), name
                assert all(row["passed"] and row["detail"]["rawInvariantAudit"]["errors"] == 0
                           and row["detail"]["actionRebuilds"] == row["observed"]["actions"]
                           for row in report["cases"]), name
            checks.append({"case": name, "passed": True, "elapsedMs": report.get("elapsedMs")})
            return report

        baseline = run("valid-json", args.catalog, True, expected_json_sha=sha(source))
        body = zlib.compress(source)
        packed = struct.pack(">8sQQ32s", b"KQBRPK1\n", len(source), len(body), hashlib.sha256(source).digest()) + body
        pack_file = work / "catalog.brpack"
        pack_file.write_bytes(packed)
        run("valid-package", pack_file, True, expected_json_sha=sha(source))
        pack_file.write_bytes(packed[:-7])
        run("truncated-package", pack_file, False)

        # 30098 is a verified display restore operation. Turning its actual
        # wait flag off must fail even though row counts and names stay equal.
        buff = catalog["buffs"]["30098"]
        previous = buff["buffWaitType"]
        buff["buffWaitType"] = "0"
        changed_file = work / "changed.json"
        changed = encode(catalog)
        changed_file.write_bytes(changed)
        broken = run("wrong-display-wait", changed_file, False, expected_json_sha=sha(changed))
        assert any("orderedProjectionSha256" in row["differences"] for row in broken["cases"])
        buff["buffWaitType"] = previous

        execution = catalog["execution"]["1"]
        previous = execution["type"]
        execution["type"] = "2"
        changed = encode(catalog)
        changed_file.write_bytes(changed)
        run("wrong-execution-policy", changed_file, False, expected_json_sha=sha(changed))
        execution["type"] = previous

        # Text is deliberately not ratcheted to old wording. The source
        # fidelity gate owns exact ID->official-text assignments; this gate
        # binds the values consumed so the composite report can prove that.
        skill = catalog["skills"]["107405"]
        original_description = skill["description"]
        skill["description"] = original_description + " (text-only test mutation)"
        changed = encode(catalog)
        changed_file.write_bytes(changed)
        text_update = run("mutable-description-observed", changed_file, True, expected_json_sha=sha(changed))
        before = {case["id"]: case["consumedDefinitionValuesSha256"] for case in baseline["cases"]}
        assert any(case["consumedDefinitionValuesSha256"] != before[case["id"]] for case in text_update["cases"])
        skill["description"] = original_description

        fixture_copy = work / "fixtures"
        fixture_copy.mkdir()
        for case in fixture_manifest["cases"]:
            shutil.copyfile(args.fixtures.parent / case["file"], fixture_copy / case["file"])
        copied_manifest = fixture_copy / "manifest.json"
        copied_manifest.write_bytes(args.fixtures.read_bytes())
        first = fixture_copy / fixture_manifest["cases"][0]["file"]
        first.write_bytes(first.read_bytes() + b" ")
        run("changed-fixture-rejected", args.catalog, False, manifest=copied_manifest)
        assert args.catalog.read_bytes() == source, "Test modified the actual catalog"
    print(json.dumps({"passed": True, "checks": checks}, ensure_ascii=False))


if __name__ == "__main__":
    main()
