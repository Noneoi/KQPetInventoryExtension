"""Portable, bounded bundles of verified replay-build inputs.

The archive retains the input bytes, including historical provenance. Extraction
relocates JSON *copies* and updates JSON-to-JSON hash references in dependency
order. It never executes downloaded code, removes a hash check, or establishes
official authenticity merely because a local manifest is internally consistent.
"""
from __future__ import annotations

import argparse
from collections.abc import Mapping
import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import unicodedata
import zipfile


FORMAT = 'kq-battle-replay-update-sources'
VERSION = 1
MAX_FILES = 4096
MAX_FILE_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
CHUNK_BYTES = 64 * 1024
_HASH = re.compile(r'[0-9a-fA-F]{64}\Z')
_WINDOWS_ABSOLUTE = re.compile(r'^[A-Za-z]:[/\\]')
_PATH_FIELDS = {'file', 'path', 'source', 'sourcePath', 'decompiledFile',
                'packageFile', 'originalExport', 'sourceProvenance',
                'extractionManifest', 'package', 'input', 'output', 'manifest'}


class BundleError(ValueError):
    """The bundle cannot be created or restored without weakening its checks."""


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise BundleError('Duplicate JSON key: ' + key)
        result[key] = value
    return result


def _json(data):
    def invalid(value):
        raise BundleError('Non-finite JSON number: ' + value)
    return json.loads(data, object_pairs_hook=_pairs, parse_constant=invalid)


def _load(value):
    return _json(Path(value).read_bytes()) if isinstance(value, (str, os.PathLike)) else value


def _absolute(value: str) -> bool:
    return bool(_WINDOWS_ABSOLUTE.match(value)) or value.startswith(('/', '\\\\'))


def _key(value: str) -> str:
    """Canonical source identity, independent of the extraction host OS."""
    value = value.replace('\\', '/')
    if _WINDOWS_ABSOLUTE.match(value) or value.startswith('//'):
        return ntpath.normpath(value).replace('\\', '/').casefold()
    return os.path.normpath(value).replace('\\', '/')


def _under(value: str, root: str) -> bool:
    return value == root or value.startswith(root.rstrip('/') + '/')


def _member(value: str) -> str:
    if not isinstance(value, str) or not value or '\\' in value or value.startswith('/'):
        raise BundleError('Invalid archive member: ' + repr(value))
    parts = value.split('/')
    reserved = {'con', 'prn', 'aux', 'nul', *(f'com{i}' for i in range(1, 10)),
                *(f'lpt{i}' for i in range(1, 10))}
    for part in parts:
        if (part in ('', '.', '..') or part.rstrip(' .') != part or
                any(ord(c) < 32 or c in '<>:"|?*' for c in part) or
                part.split('.')[0].casefold() in reserved):
            raise BundleError('Unsafe archive member: ' + value)
    return value


def _member_key(value: str) -> str:
    return unicodedata.normalize('NFC', _member(value)).casefold()


def _same_file(a: Path, b: Path) -> bool:
    return a.resolve() == b.resolve() or (a.exists() and b.exists() and os.path.samefile(a, b))


def _root_from_trace(rows: list[dict]) -> Path:
    roots = set()
    for row in rows:
        relative = row.get('projectRelative')
        if relative is None:
            continue
        relative = _member(relative.replace('\\', '/'))
        path = Path(row['path']).resolve()
        root = path
        for _ in PurePosixPath(relative).parts:
            root = root.parent
        if _key(str(root / Path(relative))) != _key(str(path)):
            raise BundleError('Trace projectRelative does not match its file: ' + str(path))
        roots.add(root)
    if len(roots) != 1:
        raise BundleError('Trace must identify exactly one original project root')
    return roots.pop()


def create_bundle(read_manifest, argument_manifest, output, extra_files=()):
    """Write an atomic ZIP from the read trace and optional explicit extra files.

    ``read_manifest`` is the JSON array produced by trace_build_inputs.py;
    ``argument_manifest`` contains its ``arguments`` mapping (or is that mapping).
    Extra entries are paths, or dictionaries with a ``path`` and optional
    ``sha256``/``bytes``. All source bytes are verified before inclusion.
    """
    rows = _load(read_manifest)
    description = _load(argument_manifest)
    if not isinstance(rows, list) or not rows:
        raise BundleError('Read manifest must be a non-empty list')
    arguments = description.get('arguments', description) if isinstance(description, dict) else None
    if not isinstance(arguments, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                                               for k, v in arguments.items()):
        raise BundleError('Build arguments must be a string-to-path mapping')
    root = _root_from_trace(rows)
    selected = list(rows)
    for extra in extra_files:
        selected.append(dict(extra) if isinstance(extra, Mapping) else {'path': str(extra)})
    files, members, sources, total = [], set(), {}, 0
    for row in selected:
        path = Path(row['path'])
        if not path.is_absolute():
            path = root / path
        if path.is_symlink() or not path.is_file():
            raise BundleError('Source must be a regular, existing file: ' + str(path))
        path = path.resolve()
        identity = _key(str(path))
        size = path.stat().st_size
        if (size > MAX_FILE_BYTES or (identity not in sources and
                (len(files) + 1 > MAX_FILES or total + size > MAX_TOTAL_BYTES))):
            raise BundleError('Source bundle exceeds file/count/expanded-size limit')
        with path.open('rb') as stream:
            data = stream.read(size + 1)
        if len(data) != size:
            raise BundleError('Source changed while reading: ' + str(path))
        digest = _sha(data)
        if row.get('sha256', digest) != digest or row.get('bytes', len(data)) != len(data):
            raise BundleError('Source differs from read manifest: ' + str(path))
        if identity in sources:
            continue
        relative = path.relative_to(root).as_posix() if path.is_relative_to(root) else None
        if relative is not None:
            member = 'project/' + relative
        else:
            parent_id = _sha(_key(str(path.parent)).encode('utf-8'))[:16]
            member = 'external/' + parent_id + '/' + path.name
        canonical = _member_key(member)
        if canonical in members:
            raise BundleError('Colliding archive members: ' + member)
        members.add(canonical)
        total += len(data)
        if len(files) + 1 > MAX_FILES or len(data) > MAX_FILE_BYTES or total > MAX_TOTAL_BYTES:
            raise BundleError('Source bundle exceeds file/count/expanded-size limit')
        files.append({'member': member, 'originalPath': str(path), 'projectRelative': relative,
                      'bytes': len(data), 'sha256': digest})
        sources[identity] = path
    manifest = {'format': FORMAT, 'version': VERSION, 'originalProjectRoot': str(root),
                'arguments': arguments, 'files': sorted(files, key=lambda x: x['member']),
                'totalBytes': total,
                'scope': 'Pinned local build inputs; local hash consistency does not establish official authenticity.'}
    manifest_bytes = json.dumps(manifest, ensure_ascii=False, indent=2, allow_nan=False).encode('utf-8')
    if len(manifest_bytes) > MAX_MANIFEST_BYTES:
        raise BundleError('Source manifest exceeds limit')
    if total + len(manifest_bytes) > MAX_TOTAL_BYTES:
        raise BundleError('Source files and manifest exceed total expanded-size limit')
    output = Path(output)
    protected = list(sources.values()) + [Path(x) for x in (read_manifest, argument_manifest)
                                         if isinstance(x, (str, os.PathLike))]
    if any(_same_file(output, path) for path in protected):
        raise BundleError('Bundle output cannot overwrite an input')
    output.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary_name = tempfile.mkstemp(prefix='.' + output.name + '.', suffix='.tmp', dir=output.parent)
    os.close(handle)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            archive.writestr('manifest.json', manifest_bytes)
            for entry in manifest['files']:
                # Recheck when writing: an input may have changed during compression.
                with sources[_key(entry['originalPath'])].open('rb') as stream:
                    data = stream.read(entry['bytes'] + 1)
                if len(data) != entry['bytes'] or _sha(data) != entry['sha256']:
                    raise BundleError('Source changed while bundling: ' + entry['originalPath'])
                archive.writestr(entry['member'], data)
        if temporary.stat().st_size > MAX_ARCHIVE_BYTES:
            raise BundleError('Compressed bundle exceeds archive-size limit')
        with temporary.open('r+b') as stream:
            os.fsync(stream.fileno())
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return {'output': str(output.resolve()), 'files': len(files), 'sourceBytes': total,
            'archiveBytes': output.stat().st_size, 'manifest': manifest}


def _validated_manifest(archive):
    infos = archive.infolist()
    if len(infos) > MAX_FILES + 1:
        raise BundleError('Too many archive members')
    names, by_name = set(), {}
    for info in infos:
        canonical = _member_key(info.filename)
        mode = info.external_attr >> 16
        kind = stat.S_IFMT(mode)
        if canonical in names:
            raise BundleError('Duplicate archive member: ' + info.filename)
        if (info.is_dir() or kind not in (0, stat.S_IFREG) or info.flag_bits & 1 or
                (info.create_system == 0 and info.external_attr & 0x400)):
            raise BundleError('Links, special files, directories and encrypted members are forbidden')
        if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise BundleError('Unsupported ZIP compression method')
        limit = MAX_MANIFEST_BYTES if info.filename == 'manifest.json' else MAX_FILE_BYTES
        if info.file_size < 0 or info.file_size > limit:
            raise BundleError('Archive member exceeds expanded-size limit')
        names.add(canonical)
        by_name[info.filename] = info
    if 'manifest.json' not in by_name:
        raise BundleError('Source bundle has no manifest')
    manifest = _json(archive.read('manifest.json'))
    if not isinstance(manifest, dict) or manifest.get('format') != FORMAT or manifest.get('version') != VERSION:
        raise BundleError('Unsupported source-bundle format')
    files = manifest.get('files')
    if not isinstance(files, list) or not files or len(files) > MAX_FILES:
        raise BundleError('Invalid source-bundle file list')
    expected, original_names, total = {'manifest.json'}, set(), 0
    for entry in files:
        if not isinstance(entry, dict):
            raise BundleError('Invalid source-bundle file record')
        member = _member(entry.get('member'))
        if not member.startswith(('project/', 'external/')) or member in expected:
            raise BundleError('Invalid or duplicate manifest member: ' + member)
        relative = entry.get('projectRelative')
        if ((member.startswith('project/') and
             (not isinstance(relative, str) or member != 'project/' + _member(relative))) or
                (member.startswith('external/') and relative is not None)):
            raise BundleError('Manifest project alias does not match its archive member')
        original = entry.get('originalPath')
        if not isinstance(original, str) or not _absolute(original) or _key(original) in original_names:
            raise BundleError('Invalid or duplicate original source path')
        original_names.add(_key(original))
        size, digest = entry.get('bytes'), entry.get('sha256')
        if type(size) is not int or not 0 <= size <= MAX_FILE_BYTES or not isinstance(digest, str) or not _HASH.fullmatch(digest):
            raise BundleError('Invalid source-bundle size/hash')
        if member not in by_name or by_name[member].file_size != size:
            raise BundleError('Manifest/ZIP member length mismatch: ' + member)
        total += size
        expected.add(member)
    if (set(by_name) != expected or total + by_name['manifest.json'].file_size > MAX_TOTAL_BYTES or
            manifest.get('totalBytes') != total):
        raise BundleError('Manifest/ZIP contents or total size mismatch')
    root = manifest.get('originalProjectRoot')
    arguments = manifest.get('arguments')
    if not isinstance(root, str) or not _absolute(root) or not isinstance(arguments, dict) or any(
            not isinstance(k, str) or not isinstance(v, str) for k, v in arguments.items()):
        raise BundleError('Invalid source roots/build arguments')
    return manifest


def _relocate_json(manifest, stage: Path, destination: Path):
    files = manifest['files']
    by_original = {_key(row['originalPath']): row for row in files}
    root = _key(manifest['originalProjectRoot'])
    root_prefixes = [(root, 'project')]
    for row in files:
        if row['member'].startswith('external/'):
            root_prefixes.append((_key(row['originalPath']).rsplit('/', 1)[0],
                                  row['member'].rsplit('/', 1)[0]))
    root_prefixes.sort(key=lambda pair: len(pair[0]), reverse=True)
    documents, original_json, changed, dependencies, bindings, historical = {}, {}, {}, {}, {}, set()

    def target(row):
        return str(destination / Path(row['member']))

    def candidates(value, origin):
        key = _key(value)
        if _absolute(value):
            return [by_original[key]] if key in by_original else []
        parent = _key(origin).rsplit('/', 1)[0]
        possible = [parent + '/' + value, parent.rsplit('/', 1)[0] + '/' + value, root + '/' + value]
        found = {}
        for candidate in possible:
            row = by_original.get(_key(candidate))
            if row:
                found[row['member']] = row
        return list(found.values())

    def relocate(value):
        if _absolute(value) and _under(_key(value), _key(str(destination))):
            return value
        key = _key(value)
        direct = by_original.get(key)
        if direct:
            return target(direct)
        for prefix, member_prefix in root_prefixes:
            if _under(key, prefix):
                suffix = key[len(prefix):].lstrip('/')
                # Source aliases may use Windows case-insensitive identity, but
                # copied relative names are preserved when an exact file exists.
                return str(destination / member_prefix / Path(suffix))
        if not _absolute(value) and value.replace('\\', '/').startswith(('build-battle-reference/', 'tools/', 'build-v2/')):
            relative = _member(value.replace('\\', '/'))
            return str(destination / 'project' / Path(relative))
        if _absolute(value):
            historical.add(value)
        return value

    for row in files:
        if row['member'].lower().endswith('.json'):
            original_json[row['member']] = (stage / row['member']).read_bytes()
            documents[row['member']] = _json(original_json[row['member']])
            changed[row['member']] = False
            dependencies[row['member']] = set()
            bindings[row['member']] = []

    def walk(node, member, origin):
        if isinstance(node, list):
            for index, value in enumerate(node):
                if isinstance(value, str):
                    mapped = relocate(value)
                    changed[member] |= mapped != value
                    node[index] = mapped
                else:
                    walk(value, member, origin)
        elif isinstance(node, dict):
            path_values = {key: value for key, value in node.items()
                           if key in _PATH_FIELDS and isinstance(value, str)}
            for hash_key, value in list(node.items()):
                if not isinstance(value, str) or not _HASH.fullmatch(value) or not hash_key.lower().endswith(('sha256', 'sha')):
                    continue
                matches = {}
                direct_references = []
                for path_key, path_value in path_values.items():
                    for candidate in candidates(path_value, origin):
                        if path_key in ('file', 'path') and hash_key == 'sha256':
                            direct_references.append(candidate)
                        if candidate['sha256'].lower() == value.lower():
                            matches[candidate['member']] = (candidate, path_key)
                if direct_references and not matches:
                    raise BundleError('Recorded source hash does not match its bundled file in ' + member)
                if len(matches) > 1:
                    # Equal bytes at different aliases are safe only when they
                    # are not JSON copies whose relocated bytes can diverge.
                    json_matches = [x for x in matches if x in documents]
                    if json_matches:
                        raise BundleError('Ambiguous JSON hash reference in ' + member)
                if len(matches) == 1:
                    candidate, path_key = next(iter(matches.values()))
                    changed[member] |= node[path_key] != target(candidate)
                    node[path_key] = target(candidate)
                    if candidate['member'] in documents:
                        dependencies[member].add(candidate['member'])
                        bindings[member].append((node, hash_key, candidate['member']))
            for key, value in list(node.items()):
                if isinstance(value, str):
                    # Detect cycles even if the author omitted a hash field.
                    if key in _PATH_FIELDS and key in path_values:
                        for candidate in candidates(path_values[key], origin):
                            if candidate['member'] in documents:
                                dependencies[member].add(candidate['member'])
                    mapped = relocate(value)
                    changed[member] |= mapped != value
                    node[key] = mapped
                else:
                    walk(value, member, origin)

    for row in files:
        if row['member'] in documents:
            walk(documents[row['member']], row['member'], row['originalPath'])
    done, active, final_hashes, relocated = set(), set(), {}, []

    def finish(member):
        if member in active:
            raise BundleError('Cyclic JSON provenance dependencies: ' + member)
        if member in done:
            return
        active.add(member)
        for dependency in sorted(dependencies[member]):
            finish(dependency)
        for node, hash_key, dependency in bindings[member]:
            changed[member] |= node[hash_key] != final_hashes[dependency]
            node[hash_key] = final_hashes[dependency]
        # Downloaded H5 JSON and every other document without local references
        # keep exact original bytes; formatting changes are not official updates.
        payload = (json.dumps(documents[member], ensure_ascii=False, indent=2, allow_nan=False).encode('utf-8')
                   if changed[member] else original_json[member])
        if len(payload) > MAX_FILE_BYTES:
            raise BundleError('Relocated JSON exceeds single-file limit: ' + member)
        (stage / member).write_bytes(payload)
        final_hashes[member] = _sha(payload)
        relocated.append({'member': member, 'bytes': len(payload), 'sha256': final_hashes[member],
                          'rewritten': changed[member], 'jsonDependencies': sorted(dependencies[member])})
        active.remove(member)
        done.add(member)

    for member in documents:
        finish(member)
    total = sum((stage / row['member']).stat().st_size for row in files)
    if total > MAX_TOTAL_BYTES:
        raise BundleError('Relocated bundle exceeds total expanded-size limit')
    arguments = {}
    for key, value in manifest['arguments'].items():
        original = value if _absolute(value) else manifest['originalProjectRoot'].rstrip('/\\') + '/' + value
        mapped = relocate(original)
        if mapped == original or not Path(mapped).is_relative_to(destination):
            raise BundleError('Cannot safely relocate build argument: ' + key)
        arguments[key] = Path(mapped)
    path_map = {}
    for row in files:
        mapped = Path(target(row))
        for alias in (row['originalPath'], row['originalPath'].replace('\\', '/'), _key(row['originalPath'])):
            path_map[alias] = mapped
        if row.get('projectRelative') is not None:
            relative = row['projectRelative'].replace('\\', '/')
            path_map[relative] = mapped
            path_map[relative.replace('/', '\\')] = mapped
    return {'arguments': arguments, 'pathMap': path_map, 'sourceCount': len(files),
            'relocatedJson': relocated, 'relocatedBytes': total,
            'unmappedHistoricalPaths': sorted(historical)}


def extract_bundle(bundle, destination):
    """Verify and atomically restore an archive into a new or empty directory.

    Returns ``arguments`` as Paths for ``build_catalog.build(**arguments)``, a
    complete old-file-to-new-file ``pathMap``, and both original and relocated
    provenance records. Historical command strings are data and never executed.
    """
    bundle, requested_destination = Path(bundle), Path(destination)
    if requested_destination.is_symlink() or (hasattr(requested_destination, 'is_junction') and requested_destination.is_junction()):
        raise BundleError('Extraction destination cannot be a link')
    destination = requested_destination.resolve()
    if not bundle.is_file() or bundle.stat().st_size > MAX_ARCHIVE_BYTES:
        raise BundleError('Missing or oversized source archive')
    if destination.exists() and (not destination.is_dir() or destination.is_symlink() or any(destination.iterdir())):
        raise BundleError('Extraction destination must be new or empty')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.replay-source-extract-', dir=destination.parent))
    try:
        with zipfile.ZipFile(bundle) as archive:
            manifest = _validated_manifest(archive)
            expanded = 0
            for entry in manifest['files']:
                path = stage / Path(entry['member'])
                if not path.resolve().is_relative_to(stage):
                    raise BundleError('Archive target escaped extraction directory')
                path.parent.mkdir(parents=True, exist_ok=True)
                digest, count = hashlib.sha256(), 0
                with archive.open(entry['member']) as source, path.open('xb') as output:
                    while data := source.read(CHUNK_BYTES):
                        count += len(data)
                        expanded += len(data)
                        if count > entry['bytes'] or expanded > MAX_TOTAL_BYTES:
                            raise BundleError('Expanded archive exceeded declared limits')
                        digest.update(data)
                        output.write(data)
                if count != entry['bytes'] or digest.hexdigest() != entry['sha256'].lower():
                    raise BundleError('Source file checksum mismatch: ' + entry['member'])
        result = _relocate_json(manifest, stage, destination)
        result['manifest'] = manifest
        (stage / 'bundle-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
        report = {key: value for key, value in result.items() if key not in ('arguments', 'pathMap', 'manifest')}
        (stage / 'relocation-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        if destination.exists():
            destination.rmdir()  # Only the verified-empty, explicitly chosen directory.
        stage.replace(destination)
        return result
    except (zipfile.BadZipFile, UnicodeError, json.JSONDecodeError) as error:
        raise BundleError('Invalid source bundle: ' + str(error)) from error
    finally:
        if stage.exists():
            if stage.resolve().parent != destination.parent.resolve() or not stage.name.startswith('.replay-source-extract-'):
                raise BundleError('Refused cleanup outside the extraction staging directory')
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    create = sub.add_parser('create')
    create.add_argument('--reads', required=True, type=Path)
    create.add_argument('--arguments', required=True, type=Path)
    create.add_argument('--output', required=True, type=Path)
    create.add_argument('--extra', action='append', default=[], type=Path)
    extract = sub.add_parser('extract')
    extract.add_argument('bundle', type=Path)
    extract.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.command == 'create':
        result = create_bundle(args.reads, args.arguments, args.output, args.extra)
        print(json.dumps({key: value for key, value in result.items() if key != 'manifest'}, ensure_ascii=False))
    else:
        result = extract_bundle(args.bundle, args.destination)
        print(json.dumps({'arguments': {key: str(value) for key, value in result['arguments'].items()},
                          'files': result['sourceCount'], 'relocatedBytes': result['relocatedBytes']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
