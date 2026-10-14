"""Run the bundled QtCore replay parser before activating shared replay data.

The executable and frozen real-report manifest are exported from the installed
workbench resources. Candidates never choose the program, fixtures or expected
results. All failures leave activation to the caller's rejecting quality gate.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

MAX_REPORT_BYTES = 4 * 1024 * 1024
TIMEOUT_SECONDS = 90
RUNTIME_FILES = ('Qt6Core.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')


def _sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')


def audit_replay_regression(catalog_path, candidate, *, report_directory):
    directory = Path(__file__).resolve().parent
    executable = directory / 'KQBattleReplayUpdateCheck.exe'
    manifest_path = directory / 'battle-replay-regression-manifest.json'
    catalog_path = Path(catalog_path).resolve()
    result = {'schemaVersion': 1, 'passed': False, 'candidateContentSha256': _sha(_canonical(candidate)),
              'errors': [], 'scope': 'Frozen real battle reports through the workbench domain parser; not a live server simulation.'}
    try:
        if not executable.is_file() or not all((directory / name).is_file() for name in RUNTIME_FILES) or not manifest_path.is_file():
            raise ValueError('内置战报核对工具不完整，请使用完整的新版工作台')
        manifest_raw = manifest_path.read_bytes()
        if len(manifest_raw) > 2 * 1024 * 1024:
            raise ValueError('内置战报核对依据过大')
        manifest = json.loads(manifest_raw)
        expected_cases = manifest.get('cases')
        if manifest.get('schemaVersion') != 1 or not isinstance(expected_cases, list) or not expected_cases \
                or any(not isinstance(row, dict) or not row.get('expected') or not row.get('sha256') or not row.get('id') for row in expected_cases):
            raise ValueError('内置战报核对依据无效或尚未冻结')
        source_raw = catalog_path.read_bytes()
        file_sha = _sha(source_raw)
        if source_raw.startswith(b'KQBRPK1\n'):
            from battle_replay.catalog_pack import unpack_bytes
            source_raw = unpack_bytes(source_raw)
        if _sha(_canonical(json.loads(source_raw))) != result['candidateContentSha256']:
            raise ValueError('待核对文件与候选资料不一致')
        raw_sha = _sha(source_raw)
        del source_raw
        result.update(checkerSha256=_sha(executable.read_bytes()),
                      qtCoreSha256=_sha((directory / 'Qt6Core.dll').read_bytes()),
                      runtimeSha256={name: _sha((directory / name).read_bytes()) for name in RUNTIME_FILES},
                      fixtureSetSha256=_sha(manifest_raw), catalogFileSha256=file_sha, catalogSha256=raw_sha)
        report_directory = Path(report_directory)
        report_directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='native-replay-check-', dir=report_directory) as scratch:
            output = Path(scratch) / 'report.json'
            log = Path(scratch) / 'process.log'
            # A local file bounds memory even if a broken native program is
            # noisy. Windows child processes inherit the updater's Job object.
            with log.open('wb') as stream:
                completed = subprocess.run([str(executable), '--catalog', str(catalog_path), '--report', str(output)],
                    cwd=directory, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                    timeout=TIMEOUT_SECONDS, check=False,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            result['processExitCode'] = completed.returncode
            with log.open('rb') as stream:
                stream.seek(max(0, log.stat().st_size - 2048))
                result['processLogTail'] = stream.read(2048).decode('utf-8', errors='replace')
            if not output.is_file() or output.stat().st_size > MAX_REPORT_BYTES:
                raise ValueError('战报核对工具未提供有效报告')
            native_raw = output.read_bytes()
            native = json.loads(native_raw)
            result['nativeReportSha256'] = _sha(native_raw)
            result['nativeReport'] = native
            if completed.returncode != 0 or type(native) is not dict or native.get('passed') is not True \
                    or native.get('schemaVersion') != 1 or native.get('mode') != 'candidate-regression' \
                    or native.get('catalogSha256') != raw_sha or native.get('catalogFileSha256') != file_sha \
                    or native.get('fixtureSetSha256') != result['fixtureSetSha256'] or native.get('errors') != []:
                raise ValueError('真实战报回归未通过，或核对报告与候选资料不匹配')
            rows = native.get('cases')
            if not isinstance(rows, list) or len(rows) != len(expected_cases) or native.get('caseCount') != len(expected_cases):
                raise ValueError('真实战报核对未完整执行')
            if len({row['id'] for row in expected_cases}) != len(expected_cases):
                raise ValueError('内置战报核对依据存在重复案例')
            for wanted, actual in zip(expected_cases, rows):
                if not isinstance(actual, dict) or actual.get('id') != wanted['id'] or actual.get('passed') is not True \
                        or actual.get('fixtureSha256') != wanted['sha256']:
                    raise ValueError('真实战报案例缺失、顺序错误或核对失败')
        if _sha(catalog_path.read_bytes()) != file_sha:
            raise ValueError('战报核对期间候选资料发生变化')
        result['passed'] = True
    except (OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as error:
        result['errors'].append(str(error))
    return result
