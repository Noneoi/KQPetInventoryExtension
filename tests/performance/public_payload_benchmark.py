"""Read-only same-object Python old/new comparison; fresh child process per run."""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import public_dataset_store as store
from battle_replay.catalog_pack import unpack_bytes, validate_catalog_bytes


def peak_working_set():
    if os.name != 'nt':
        return None
    class Counters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD),
                   *[(name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
                     'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                     'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]]
    counters = Counters(); counters.cb = ctypes.sizeof(counters)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return counters.PeakWorkingSetSize


def before_validation(raw):
    # Exact previous work: validated JSON was thrown away then decoded again.
    unpacked = unpack_bytes(raw)
    validate_catalog_bytes(unpacked)
    return store.parse_json(unpacked)


def child(root, mode):
    revision, _, manifest = store.load_current(root, validate_objects=False)
    reference = manifest['components']['battleReplay']
    start = time.perf_counter()
    if mode == 'before':
        raw = store._verified_object_bytes(root, reference)
        before_validation(raw)  # old read_object_ref validated before returning
        result = before_validation(raw)  # old caller parsed returned bytes again
    else:
        result = store.read_object_payload(root, reference)
    elapsed, peak = time.perf_counter() - start, peak_working_set()
    # This output-equivalence hash is outside timing and measured peak.
    semantic = store.sha(store.canonical(result))
    return {'mode': mode, 'seconds': elapsed, 'processPeakWorkingSetBytes': peak,
            'revision': revision, 'object': reference, 'semanticSha256': semantic,
            'counts': {key: len(result[key]) for key in ('pets', 'skills', 'buffs')}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--child', choices=('before', 'after'))
    args = parser.parse_args()
    if args.child:
        print(json.dumps(child(args.data_root, args.child)))
        return
    if not args.output:
        parser.error('--output is required for the comparison')
    runs = []
    for index in range(3):
        for mode in ('before', 'after') if index % 2 == 0 else ('after', 'before'):
            command = [sys.executable, '-B', '-X', 'utf8', str(Path(__file__).resolve()),
                       '--data-root', str(args.data_root.resolve()), '--child', mode]
            process = subprocess.run(command, capture_output=True, check=True, timeout=180)
            runs.append(json.loads(process.stdout))
    report = {'schema': 1, 'scope': 'same actual immutable replay object; no network or publication',
        'measurement': 'fresh child per run; warm OS caches; alternating before/after order; '
                       'Windows process peak includes imports/setup; excludes semantic hash after operation',
        'passed': len({row['semanticSha256'] for row in runs}) == 1 and len({row['object']['sha256'] for row in runs}) == 1,
        'toolSha256': store.sha(Path(__file__).read_bytes()),
        'parserSha256': store.sha(Path(store.__file__).read_bytes()), 'runs': runs,
        'median': {mode: {'seconds': statistics.median(row['seconds'] for row in runs if row['mode'] == mode),
                        'processPeakWorkingSetBytes': statistics.median(row['processPeakWorkingSetBytes'] for row in runs if row['mode'] == mode)}
                   for mode in ('before', 'after')}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'median': report['median']}))


if __name__ == '__main__':
    main()
