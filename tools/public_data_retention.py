"""Keep the applied public catalog and its rollback version; reclaim old data.

The native caller holds catalog/publication.lock. This process additionally
holds the updater's byte-range lock, so neither publication nor preparation
can race deletion. No account, image, tool or selection files are traversed.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import stat
import sys

# Windows' bundled isolated Python does not automatically search the script
# directory. Only this trusted helper directory is added, as in the updater.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from public_update_store import COMPONENTS, ATTACHMENTS, pointer, read_json, version_files
from public_update_support import atomic_bytes, canonical, child, plain, sha

SHA = re.compile(r'[a-f0-9]{64}\Z')
ATTEMPT = re.compile(r'[a-f0-9]{32}\Z')
TERMINAL = {'completed', 'partial', 'sessionApplied', 'diskCommitted', 'cancelled', 'failed', 'interrupted', 'applyFailed'}
LEGACY_NAMES = {*COMPONENTS.values(), *ATTACHMENTS, 'battle-replay-catalog.brpack', 'public-resource-manifest.json'}


def checked(root, path):
    path = plain(path)
    if path == root or not path.is_relative_to(root) or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('清理路径超出公共资料目录')
    if path.is_symlink() or (path.exists() and getattr(path.lstat(), 'st_file_attributes', 0) & 0x400):
        raise ValueError('公共资料清理路径包含链接或重解析点')
    return path


def strings(value):
    todo = [value]
    while todo:
        item = todo.pop()
        if isinstance(item, str):
            yield item.replace('\\', '/')
        elif isinstance(item, dict):
            todo.extend(item.values())
        elif isinstance(item, list):
            todo.extend(item)


def plan_retention(root, expected_revision):
    root = plain(root)
    current = pointer(root)
    if not current or current.get('revision') != expected_revision:
        raise ValueError('现用公共资料已变化，暂不清理')
    receipt_path = child(root, 'catalog/public-update-status.json')
    receipt = read_json(receipt_path)
    if (receipt.get('state') not in {'completed', 'partial'} or not receipt.get('sessionApplied') or
            not receipt.get('diskCommitted') or receipt.get('revision') != current['revision'] or
            receipt.get('attempt') != current.get('attempt')):
        raise ValueError('公共资料尚未确认载入，暂不清理')
    keep = {current['revision']}
    previous = current.get('previousRevision', '')
    if previous:
        if not SHA.fullmatch(previous):
            raise ValueError('上一版公共资料身份无效，暂不清理')
        keep.add(previous)
    references = set()
    for revision in keep:
        files, manifest = version_files(root, revision)
        if not all(manifest['components'][key]['available'] for key in COMPONENTS):
            raise ValueError('保留版本资料不完整，暂不清理旧数据')
        for file in files.values():
            references.update(value for value in strings(read_json(file))
                              if 'public/' in value or 'catalog/' in value)
    protected_objects = {m[1] for value in references for m in re.finditer(r'public/objects/([a-f0-9]{64})(?:/|$)', value)}
    plan = {'schema': 1, 'root': str(root), 'keptRevisions': sorted(keep),
            'revision': current['revision'], 'attempt': current.get('attempt'),
            'pointerSha256': sha(child(root, 'catalog/current.json').read_bytes()),
            'receiptSha256': sha(receipt_path.read_bytes()), 'files': {}, 'directories': [],
            'removedVersions': [], 'skipped': [], 'protectedObjects': sorted(protected_objects)}
    directory_set = set()

    def add_tree(path):
        if not path.exists():
            return
        todo = [checked(root, path)]
        while todo:
            item = checked(root, todo.pop())
            info = item.stat()
            relative = item.relative_to(root).as_posix()
            if stat.S_ISDIR(info.st_mode):
                directory_set.add(relative)
                todo.extend(item.iterdir())
            elif stat.S_ISREG(info.st_mode):
                plan['files'][relative] = {'bytes': info.st_size, 'modifiedNs': info.st_mtime_ns}
            else:
                raise ValueError('公共资料目录中有非普通文件')
            if len(plan['files']) + len(directory_set) > 100000:
                raise ValueError('待清理文件过多，本次未执行')

    versions = child(root, 'catalog/versions')
    for directory in versions.iterdir():
        checked(root, directory)
        if directory.name in keep:
            continue
        if not directory.is_dir() or not SHA.fullmatch(directory.name):
            plan['skipped'].append(directory.relative_to(root).as_posix())
            continue
        # Old, unreferenced versions may be partially removed after an abrupt
        # exit. Only their owned flat filenames are required; the two retained
        # versions above must always pass full digest validation.
        if not {p.name for p in directory.iterdir()}.issubset({*COMPONENTS.values(), *ATTACHMENTS, 'manifest.json'}):
            plan['skipped'].append(directory.relative_to(root).as_posix())
            continue
        add_tree(directory)
        plan['removedVersions'].append(directory.name)

    # A candidate which has not finished is kept for retry. A later successful
    # publication may reclaim its terminal task, never the currently applied one.
    for base, result_name in (('catalog/update-tasks', 'result.json'), ('public/transactions', 'receipt.json')):
        directory = child(root, base)
        if not directory.exists():
            continue
        for task in directory.iterdir():
            checked(root, task)
            if not task.is_dir() or not ATTEMPT.fullmatch(task.name) or task.name == current.get('attempt'):
                continue
            receipt_file = checked(root, task / result_name)
            if receipt_file.is_file() and read_json(receipt_file).get('state') in TERMINAL:
                add_tree(task)
            else:
                plan['skipped'].append(task.relative_to(root).as_posix())

    # This layout belonged to the old object-store updater. The current and
    # rollback JSONs are checked for live object references before migration GC.
    for relative in ('public/source-cache', 'public/releases', 'public/reports', 'public/current.json'):
        if any(relative in value for value in references):
            plan['skipped'].append(relative)
        else:
            add_tree(child(root, relative))
    objects = child(root, 'public/objects')
    if objects.exists():
        for directory in objects.iterdir():
            checked(root, directory)
            if directory.is_dir() and SHA.fullmatch(directory.name) and directory.name not in protected_objects:
                add_tree(directory)

    catalog = child(root, 'catalog')
    for file in catalog.iterdir():
        checked(root, file)
        name = file.name.split('.bak-', 1)[0]
        if file.is_file() and name in LEGACY_NAMES:
            if not any('catalog/' + file.name in value for value in references):
                add_tree(file)
        elif file.is_dir() and file.name.startswith('backup-before-'):
            children = list(file.iterdir())
            if all(checked(root, f).is_file() and f.name in LEGACY_NAMES for f in children):
                add_tree(file)
    plan['directories'] = sorted(directory_set, key=lambda name: (-name.count('/'), name))
    plan['bytes'] = sum(row['bytes'] for row in plan['files'].values())
    return plan


def apply_retention(plan):
    root = plain(plan['root'])
    def unchanged():
        if sha(child(root, 'catalog/current.json').read_bytes()) != plan['pointerSha256'] or sha(child(root, 'catalog/public-update-status.json').read_bytes()) != plan['receiptSha256']:
            raise ValueError('公共资料状态已变化，停止清理')
    unchanged()
    # Validate the entire inventory before the first removal. Never follow a
    # directory link and never recursively delete a computed directory.
    for relative, record in plan['files'].items():
        file = checked(root, child(root, relative)); info = file.stat()
        if not file.is_file() or info.st_size != record['bytes'] or info.st_mtime_ns != record['modifiedNs']:
            raise ValueError('待清理文件已变化，本次未执行')
    for relative in plan['directories']:
        if not checked(root, child(root, relative)).is_dir():
            raise ValueError('待清理目录已变化，本次未执行')
    removed = 0
    for relative, record in plan['files'].items():
        unchanged()
        checked(root, child(root, relative)).unlink()
        removed += record['bytes']
    for relative in plan['directories']:
        unchanged()
        checked(root, child(root, relative)).rmdir()
    return {'schema': 1, 'ok': True, 'revision': plan['revision'], 'attempt': plan['attempt'],
            'checkedUtc': datetime.now(timezone.utc).isoformat(), 'keptRevisions': plan['keptRevisions'],
            'removedVersions': len(plan['removedVersions']), 'removedFiles': len(plan['files']),
            'removedBytes': removed, 'skipped': plan['skipped'], 'protectedObjects': plan['protectedObjects']}


@contextmanager
def update_lock(root):
    path = child(root, 'data-update.lock')
    with path.open('a+b') as stream:
        if path.stat().st_size == 0:
            stream.write(b'0'); stream.flush()
        stream.seek(0)
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-root', required=True, type=Path)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--publication-owner', required=True, type=int)
    args = parser.parse_args()
    root = plain(args.data_root)
    try:
        publication = child(root, 'catalog/publication.lock')
        owner = publication.read_text(encoding='utf-8').splitlines()[0]
        if owner != str(args.publication_owner) or args.publication_owner <= 0:
            raise ValueError('公共资料发布锁身份不一致')
        with update_lock(root):
            plan = plan_retention(root, args.revision)
            result = apply_retention(plan)
            atomic_bytes(child(root, 'catalog/public-retention.json'), canonical(result))
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}, ensure_ascii=True))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
