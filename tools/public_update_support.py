"""Verified public inputs, bounded extraction reuse and operation measurements.

Only source extraction is cached here. Business rules and validation remain the
caller's responsibility; a cached success flag never substitutes for file bytes.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

FLASH = 'https://aoqi.100bt.com/play/'
H5 = 'https://aoqi.100bt.com/h5/'
VERSION = re.compile(r'(?:2000|[0-9]{8,20})\Z')


class UpdateCancelled(BaseException):
    pass


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def file_sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def plain(path):
    path = Path(os.path.abspath(path))
    for item in (path, *path.parents):
        if item.exists() and (item.is_symlink() or getattr(item.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('公共资料路径包含链接或重解析点')
    return path


def child(root, relative):
    name = PurePosixPath(str(relative).replace('\\', '/'))
    if name.is_absolute() or not name.parts or any(part in ('.', '..') or ':' in part for part in name.parts):
        raise ValueError('公共资料相对路径无效')
    base = plain(root)
    path = plain(base.joinpath(*name.parts))
    if not path.is_relative_to(base):
        raise ValueError('公共资料路径越界')
    return path


def atomic_bytes(path, raw):
    path = plain(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Operations:
    def __init__(self, emit=lambda *args, **kwargs: None, cancelled=lambda: False):
        self.emit, self.cancelled = emit, cancelled
        self.started = time.perf_counter()
        self.rows = []
        self.lock = threading.RLock()

    def check(self):
        if self.cancelled():
            raise UpdateCancelled('检查已取消，现用资料保留')

    @contextmanager
    def measure(self, kind, component='', resource=''):
        self.check()
        start = time.perf_counter()
        row = dict(kind=kind, component=component, resource=resource, bytes=0, reused=False, succeeded=False)
        self.emit('phase', phase=kind, component=component, resource=resource)
        try:
            yield row
            row['succeeded'] = True
        finally:
            row['seconds'] = time.perf_counter() - start
            with self.lock:
                self.rows.append(row)
                self.emit('operation', operation=row, wallSeconds=time.perf_counter() - self.started)

    def report(self):
        with self.lock:
            totals = {}
            for row in self.rows:
                value = totals.setdefault(row['kind'], dict(seconds=0, calls=0, reused=0, bytes=0))
                value['seconds'] += row['seconds']
                value['calls'] += 1
                value['reused'] += int(row['reused'])
                value['bytes'] += row['bytes']
            return dict(wallSeconds=time.perf_counter() - self.started, operations=list(self.rows), totals=totals,
                        timingNote='Operations can overlap; their summed durations are not wall time.')


class SourceVersions(dict):
    def __init__(self, values=(), hashes=None):
        super().__init__(values)
        self.hashes = dict(hashes or {})


class PublicSources:
    """One fresh official snapshot per run; same-run URL downloads are shared."""
    def __init__(self, scratch, transport, operations=None):
        self.root = Path(scratch) / 'resources'
        self.transport = transport
        self.operations = operations or Operations()
        self.cache, self.locks, self.snapshots = {}, {}, {}
        self.guard = threading.RLock()

    def fetch(self, url, output, *, runtime=False, fresh=False):
        if runtime:
            with self.operations.measure('toolsDownload', resource=url) as metric:
                result = self.transport(url, output, runtime=True)
                metric['bytes'] = Path(output).stat().st_size
                return result
        if not url.startswith((FLASH, H5)):
            raise ValueError('仅允许官方公共资料来源')
        with self.guard:
            lock = self.locks.setdefault(url, threading.RLock())
        with lock, self.operations.measure('download', resource=url) as metric:
            cached = None if fresh else self.cache.get(url)
            if cached:
                source, record = cached
                if not source.is_file() or file_sha(source) != record['sha256']:
                    raise ValueError('同次来源缓存内容不匹配')
                metric['reused'] = True
            else:
                self.root.mkdir(parents=True, exist_ok=True)
                source = self.root / (sha(url.encode()) + ('-' + str(time.time_ns()) if fresh else ''))
                self.transport(url, source)
                if source.stat().st_size > 32 * 1024 * 1024:
                    raise ValueError('官方资源超过读取限制')
                record = dict(url=url, sha256=file_sha(source), bytes=source.stat().st_size)
                metric['bytes'] = record['bytes']
                if not fresh:
                    self.cache[url] = (source, record)
            output = Path(output)
            output.parent.mkdir(parents=True, exist_ok=True)
            if output.resolve() != source.resolve():
                shutil.copyfile(source, output)
            return dict(record)

    def flash(self, swf_text):
        if 'flash' in self.snapshots:
            return self.snapshots['flash']
        start = self.root / 'flash-start.xml'
        self.fetch(FLASH + 'start.xml', start, fresh=True)
        entry = ET.fromstring(start.read_bytes())
        version = entry.findtext('v') or ''
        if not VERSION.fullmatch(version):
            raise ValueError('官方 Flash 入口缺少有效加载版本 v')
        path = self.root / 'versiondata.swf'
        record = self.fetch(FLASH + 'versiondata~' + version + '.swf', path)
        values = dict(re.findall(r'<f\b[^>]*\bn="([^"]+)"[^>]*\bv="([0-9]+)"', swf_text(path)))
        if len(values) < 100:
            raise ValueError('官方资源版本表不完整')
        overrides = {item.get('n'): item.get('v') for item in entry.findall('./u/f') if item.get('n')}
        if any(not VERSION.fullmatch(value or '') for value in overrides.values()):
            raise ValueError('官方临时版本覆盖无效')
        values.update(overrides)
        result = dict(version=version, versions=SourceVersions(values), overrides=overrides, source=record)
        self.snapshots['flash'] = result
        return result

    def h5(self):
        if 'h5' in self.snapshots:
            return self.snapshots['h5']
        start = self.root / 'h5-start.json'
        self.fetch(H5 + 'start~' + str(time.time_ns() // 1000000) + '.json', start, fresh=True)
        entry = json.loads(start.read_bytes())
        version = str(entry.get('version', ''))
        if not VERSION.fullmatch(version):
            raise ValueError('官方 H5 入口版本无效')
        path = self.root / 'h5-version.json'
        self.fetch(H5 + 'version~' + version + '.json', path)
        values = json.loads(path.read_bytes())
        overrides = entry.get('files', {})
        if not isinstance(values, dict) or not values or not isinstance(overrides, dict):
            raise ValueError('官方 H5 版本表或覆盖结构无效')
        values.update(overrides)
        result = dict(version=version, versions=values, overrides=overrides)
        self.snapshots['h5'] = result
        return result

    def h5_resource(self, resource):
        if not re.fullmatch(r'[A-Za-z0-9_./-]+\.(?:json|aqz|js)', resource) or '..' in resource:
            raise ValueError('H5 资源路径无效')
        chain = self.h5()
        stem, suffix = resource.rsplit('.', 1)
        alias = stem + '.mix'
        key = alias if alias in chain['versions'] else resource
        version = str(chain['versions'].get(key, '20211126'))
        if not VERSION.fullmatch(version):
            raise ValueError('H5 资源版本无效：' + resource)
        path = self.root / ('h5-' + sha(resource.encode()))
        row = self.fetch(H5 + stem + '~' + version + '.' + suffix, path)
        return path, dict(row, resource=resource, version=version, resolution=key)

    def assert_stable(self):
        for kind, previous in self.snapshots.items():
            path = self.root / (kind + '-recheck')
            if kind == 'flash':
                self.fetch(FLASH + 'start.xml', path, fresh=True)
                root = ET.fromstring(path.read_bytes())
                version = root.findtext('v')
                overrides = {item.get('n'): item.get('v') for item in root.findall('./u/f') if item.get('n')}
            else:
                self.fetch(H5 + 'start~' + str(time.time_ns() // 1000000) + '.json', path, fresh=True)
                root = json.loads(path.read_bytes())
                version, overrides = str(root.get('version', '')), root.get('files', {})
            if version != previous['version'] or overrides != previous['overrides']:
                raise ValueError('官方资料在检查期间切换版本，请重新检查')


class ExtractCache:
    def __init__(self, root, operations=None, maximum_bytes=256 * 1024 * 1024):
        self.root = plain(root)
        self.operations = operations or Operations()
        self.maximum_bytes = maximum_bytes
        self.locks, self.guard = {}, threading.RLock()

    def extract(self, source, target, selection, tools, produce):
        identity = dict(schema=1, sourceSha256=file_sha(source), selection=selection, tools=tools,
                        extractionCodeSha256=file_sha(Path(__file__)), format='actionscript-utf8')
        key = sha(canonical(identity))
        with self.guard:
            lock = self.locks.setdefault(key, threading.RLock())
        with lock:
            directory = child(self.root, key)
            marker = directory / 'record.json'
            target = Path(target)
            if marker.is_file():
                with self.operations.measure('extractCache', resource=selection) as metric:
                    if marker.stat().st_size > 2 * 1024 * 1024:
                        raise ValueError('脚本缓存清单过大')
                    record = json.loads(marker.read_bytes())
                    if record.get('identity') != identity or not isinstance(record.get('files'), dict) or not record['files']:
                        raise ValueError('脚本提取缓存身份无效')
                    if len(record['files']) > 5000:
                        raise ValueError('脚本缓存文件数量超限')
                    total = 0
                    for relative, expected in record['files'].items():
                        path = child(directory / 'files', relative)
                        total += path.stat().st_size
                        if total > 32 * 1024 * 1024 or file_sha(path) != expected:
                            raise ValueError('脚本提取缓存内容校验失败')
                    for relative in record['files']:
                        output = child(target, relative)
                        output.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(child(directory / 'files', relative), output)
                    metric.update(reused=True, bytes=total)
                    return sorted(record['files'])
            target.mkdir(parents=True, exist_ok=True)
            with self.operations.measure('decompile', resource=selection):
                produce(target)
            files = sorted(target.rglob('*.as'))
            if not files or len(files) > 5000 or sum(p.stat().st_size for p in files) > 32 * 1024 * 1024:
                raise ValueError('官方脚本导出为空或超过大小限制')
            records = {p.relative_to(target).as_posix(): file_sha(plain(p)) for p in files}
            self.root.mkdir(parents=True, exist_ok=True)
            existing = [p for p in self.root.rglob('*') if p.is_file()]
            used = sum(p.stat().st_size for p in existing)
            if len(existing) + len(files) + 1 <= 12000 and used + sum(p.stat().st_size for p in files) <= self.maximum_bytes:
                with tempfile.TemporaryDirectory(prefix='.extract-', dir=self.root) as temporary:
                    staged = Path(temporary) / key
                    for relative in records:
                        output = child(staged / 'files', relative)
                        output.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(child(target, relative), output)
                    atomic_bytes(staged / 'record.json', canonical(dict(identity=identity, files=records)))
                    if not directory.exists():
                        os.replace(staged, directory)
            return sorted(records)
