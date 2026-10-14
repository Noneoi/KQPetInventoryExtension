"""One official version snapshot and verified download cache per public update.

Workbench and replay parsers keep their complete output contracts. This module
only unifies their inputs; downloaded code is never executed.
"""
from __future__ import annotations

import copy
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import shutil
import threading
import time
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET

from battle_replay.fetch_reference import flash_versions

FLASH = 'https://aoqi.100bt.com/play/'
H5 = 'https://aoqi.100bt.com/h5/'
H5_MAIN_SHA256 = '70d24ceb961dfb193becd15a874a51d596245898f13e9157286db931a181b670'
VERSION_RULE = 2
MAX_DOWNLOAD = 32 * 1024 * 1024
VERSION = re.compile(r'(?:2000|[0-9]{8,20})\Z')


def sha(data):
    return hashlib.sha256(data).hexdigest()


class ResourceVersions(dict):
    """Official revisions plus separate, freshly verified content identities."""
    def __init__(self, values, source_hashes=None, bootstrap_loader=None):
        super().__init__(values)
        self.source_hashes = dict(source_hashes or {})
        self.bootstrap_loader = dict(bootstrap_loader or {})


def resource_matches(previous: dict, resource: str, versions: dict) -> bool:
    version = versions.get(resource, '2000')
    if (not isinstance(previous, dict) or not isinstance(version, str) or
            not VERSION.fullmatch(version) or previous.get('version') != version):
        return False
    current_hash = getattr(versions, 'source_hashes', {}).get(resource)
    return not current_hash or previous.get('sha256') == current_hash


class PublicResourceSession:
    def __init__(self, scratch: Path, fetcher):
        self.root = Path(scratch) / 'official-resources'
        self.transport = fetcher
        self.component = ''
        self._guard = threading.RLock()
        self.reset()

    def reset(self):
        self._cache, self._locks, self._chains, self._errors = {}, {}, {}, {}
        self._chain_locks = {'flash': threading.RLock(), 'h5': threading.RLock()}
        self.requests, self.plan = [], []
        self.component = ''

    def fetch(self, url, output, *, runtime=False):
        if runtime:
            return self.transport(url, output, runtime=True)
        if not url.startswith((FLASH, H5)):
            raise ValueError('公共资料只能来自官方资源目录')
        # Entry points are always fresh. A versioned payload is shared only
        # inside this run, after rechecking the cached bytes against their SHA.
        parts = urlsplit(url)
        fresh = parts.path.endswith('/start.xml') or bool(re.search(r'/start~\d+\.json$', parts.path))
        with self._guard:
            lock = self._locks.setdefault(url, threading.Lock())
        with lock:
            self.root.mkdir(parents=True, exist_ok=True)
            output = Path(output)
            output.parent.mkdir(parents=True, exist_ok=True)
            cached = None if fresh else self._cache.get(url)
            if cached:
                path, record = cached
                if not path.is_file() or path.stat().st_size > MAX_DOWNLOAD or sha(path.read_bytes()) != record['sha256']:
                    raise ValueError('本次共享资源缓存校验失败：' + url)
            else:
                path = self.root / (sha(url.encode()) + ('.' + str(time.time_ns()) if fresh else '') + '.download')
                self.transport(url, path)
                if not path.is_file() or path.stat().st_size > MAX_DOWNLOAD:
                    raise ValueError('官方公共资源超过大小限制')
                data = path.read_bytes()
                record = {'url': url, 'sha256': sha(data), 'bytes': len(data)}
                if not fresh:
                    with self._guard:
                        self._cache[url] = (path, record)
            if path.resolve() != output.resolve():
                shutil.copyfile(path, output)
            with self._guard:
                self.requests.append(dict(record, component=self.component, reused=bool(cached)))
            return dict(record)

    def _snapshot(self, kind, prepare):
        with self._chain_locks[kind]:
            if kind in self._errors:
                raise ValueError(self._errors[kind])
            if kind not in self._chains:
                try:
                    self._chains[kind] = prepare()
                except Exception as error:
                    self._errors[kind] = str(error)
                    raise
            return self._chains[kind]

    def flash_chain(self, destination=None):
        def prepare():
            target = self.root / 'flash'
            start_path = target / 'start.xml'
            entry = self.fetch(FLASH + 'start.xml', start_path)
            start = start_path.read_bytes()
            version = ET.fromstring(start).findtext('v') or ''
            if not VERSION.fullmatch(version):
                raise ValueError('官方 Flash 入口 v 版本无效，保留原资料')
            package = target / 'versiondata.swf'
            evidence = self.fetch(FLASH + 'versiondata~' + version + '.swf', package)
            versions = flash_versions(start, package.read_bytes())
            if len(versions) < 100 or any(not isinstance(k, str) or not k or len(k) > 512 or
                    not isinstance(v, str) or not VERSION.fullmatch(v) for k, v in versions.items()):
                raise ValueError('官方 Flash 版本表不完整或结构变化')
            evidence.update(resource='versiondata', version=version, manifestSha256=entry['sha256'])
            return {'versions': versions, 'start': start, 'entry': entry,
                    'versiondata': evidence, 'package': package}
        chain = self._snapshot('flash', prepare)
        if destination is not None:
            target = Path(destination)
            target.mkdir(parents=True, exist_ok=True)
            (target / 'start.xml').write_bytes(chain['start'])
            if chain['package'].resolve() != (target / 'versiondata.swf').resolve():
                shutil.copyfile(chain['package'], target / 'versiondata.swf')
        with self._guard:
            cached = list(self._cache.items())
        hashes = {}
        for url, (_, record) in cached:
            match = re.fullmatch(re.escape(FLASH) + r'([A-Za-z0-9_/@-]+)~([0-9]+)\.(?:swf|fui)', url)
            if match and chain['versions'].get(match[1], '2000') == match[2]:
                hashes[match[1]] = record['sha256']
        bootstrap_url = FLASH + 'loader~' + chain['versiondata']['version'] + '.swf'
        bootstrap = next((dict(record) for url, (_, record) in cached if url == bootstrap_url), {})
        return ResourceVersions(chain['versions'], hashes, bootstrap), chain['start'], {k: dict(chain[k]) for k in ('entry', 'versiondata')}

    def prefetch(self, selected):
        """Verify the declared union before any selected parser can reuse output.

        Historical/dynamic references keep their existing discovery policy;
        listing an old reference does not claim its bytes were refreshed.
        """
        selected = set(selected)
        jobs, failures = [], {}
        for row in self.plan:
            owners = selected.intersection(row['components'])
            if (not owners or row.get('versionOnly') or
                    'declared-dependency' not in row.get('sourceStatuses', [])):
                continue
            if row['kind'] == 'flash' and row['resource'] == 'loader':
                # This one identity has two independently evidenced consumers.
                # Isolate failures as well as downloads for partial updates.
                for strategy, consumers in (('module', {'battleReplay'}),
                                             ('bootstrap', {'shop', 'images', 'icons'})):
                    if owners & consumers:
                        jobs.extend((dict(row, loaderStrategy=strategy), extension, owners & consumers)
                                    for extension in row['formats'])
            else:
                jobs.extend((row, extension, owners) for extension in row['formats'])

        def download(row, extension):
            resource = row['resource']
            identity = row['kind'] + ':' + resource + ':' + row.get('loaderStrategy', '')
            target = self.root / 'union' / (sha(identity.encode()) + '.' + extension)
            if row['kind'] == 'h5':
                if resource == 'js/main.js':
                    return self.h5_chain()['main']
                return self.h5_resource(resource, target)[1]
            versions, start, proof = self.flash_chain()
            version = versions.get(resource, '2000')
            if row.get('loaderStrategy') == 'bootstrap':
                version = proof['versiondata']['version']
            return self.fetch(FLASH + resource + '~' + version + '.' + extension, target)

        with ThreadPoolExecutor(max_workers=4) as pool:
            pending = {pool.submit(download, row, extension): (row, owners) for row, extension, owners in jobs}
            progress = getattr(self, 'progress_callback', None)
            if progress:
                progress(0, len(pending))
            for completed, future in enumerate(as_completed(pending), 1):
                row, owners = pending[future]
                try:
                    future.result()
                except Exception as error:
                    for owner in owners:
                        failures.setdefault(owner, []).append(row['resource'] + '：' + str(error))
                if progress:
                    progress(completed, len(pending))
        return {owner: '；'.join(sorted(set(messages))) for owner, messages in failures.items()}

    def h5_chain(self, destination=None):
        def prepare():
            target = self.root / 'h5'
            start_path = target / 'start.json'
            entry = self.fetch(H5 + 'start~' + str(time.time_ns() // 1000000) + '.json', start_path)
            start = start_path.read_bytes()
            value = json.loads(start)
            version = str(value.get('version', ''))
            if not VERSION.fullmatch(version):
                raise ValueError('官方 H5 入口版本无效')
            manifest_path = target / 'version.json'
            manifest = self.fetch(H5 + 'version~' + version + '.json', manifest_path)
            versions = json.loads(manifest_path.read_bytes())
            overrides = value.get('files', {})
            if not isinstance(versions, dict) or not versions or not isinstance(overrides, dict):
                raise ValueError('官方 H5 版本表或 files 覆盖结构变化')
            versions.update(overrides)
            main_path = target / 'main.js'
            main = self.fetch(H5 + 'js/main~' + version + '.js', main_path)
            if main['sha256'] != H5_MAIN_SHA256:
                raise ValueError('官方 H5 加载代码变化，需先适配公共更新器')
            main.update(resource='js/main.js', version=version)
            return {'version': version, 'versions': versions, 'overrides': overrides,
                    'start': start, 'entry': entry, 'manifest': manifest, 'main': main,
                    'manifestPath': manifest_path, 'mainPath': main_path}
        chain = self._snapshot('h5', prepare)
        if destination is not None:
            target = Path(destination)
            target.mkdir(parents=True, exist_ok=True)
            (target / 'h5-start.json').write_bytes(chain['start'])
            shutil.copyfile(chain['manifestPath'], target / 'h5-version.json')
            shutil.copyfile(chain['mainPath'], target / 'h5-main.js')
        return copy.deepcopy(chain)

    def resolve_h5(self, resource):
        if (not re.fullmatch(r'[A-Za-z0-9_./-]+\.(?:json|js|aqz)', resource) or
                '..' in resource or resource.startswith('/')):
            raise ValueError('无效的官方 H5 资源路径')
        chain = self.h5_chain()
        stem, suffix = resource.rsplit('.', 1)
        alias = stem + '.mix'
        key = alias if alias in chain['versions'] else resource
        version = str(chain['versions'].get(key, '20211126'))
        if not VERSION.fullmatch(version):
            raise ValueError('官方 H5 资源版本无效：' + resource)
        return {'resource': resource, 'version': version, 'url': H5 + stem + '~' + version + '.' + suffix,
                'resolution': 'start-files' if key in chain['overrides'] else
                'version-manifest' if key in chain['versions'] else 'default-20211126'}

    def h5_resource(self, resource, destination):
        resolved = self.resolve_h5(resource)
        record = self.fetch(resolved['url'], destination)
        return Path(destination), {**record, **resolved}

    def h5_signature(self, resources):
        chain = self.h5_chain()
        return sha(json.dumps({'versionRule': VERSION_RULE, 'mainSha256': chain['main']['sha256'],
                               'resources': [self.resolve_h5(key) for key in sorted(set(resources))]},
                              sort_keys=True, separators=(',', ':')).encode())

    def assert_stable(self, kinds=('flash', 'h5')):
        for kind in kinds:
            if kind not in self._chains:
                continue
            chain = self._chains[kind]
            path = self.root / (kind + '-recheck-' + str(time.time_ns()))
            url = (FLASH + 'start.xml?public_recheck=' + str(time.time_ns()) if kind == 'flash'
                   else H5 + 'start~' + str(time.time_ns() // 1000000) + '.json')
            self.fetch(url, path)
            fresh = path.read_bytes()
            if kind == 'flash':
                def semantic(raw):
                    node = ET.fromstring(raw)
                    overrides = {r.get('n'): r.get('v') for r in node.findall('./u/f')}
                    return node.findtext('v'), sorted(overrides.items())
            else:
                def semantic(raw):
                    node = json.loads(raw)
                    return node.get('version'), node.get('files', {})
            if semantic(fresh) != semantic(chain['start']):
                raise ValueError('官方资源在更新期间切换版本，未发布的新资料已保留不用，请重新检查')

    def audit(self):
        resources = {}
        for request in self.requests:
            key = (request['url'], request['sha256'])
            row = resources.setdefault(key, {k: request[k] for k in ('url', 'sha256', 'bytes')})
            row.setdefault('components', set()).add(request['component'])
            row['downloads'] = row.get('downloads', 0) + (not request['reused'])
            row['reuses'] = row.get('reuses', 0) + request['reused']
        rows = [{**r, 'components': sorted(r['components'] - {''})} for r in resources.values()]
        return {'schema': 1, 'versionRule': VERSION_RULE, 'initialPlan': self.plan,
                'versions': {kind: (c['versiondata']['version'] if kind == 'flash' else c['version'])
                             for kind, c in self._chains.items()},
                'versionErrors': self._errors, 'resources': rows,
                'uniqueDownloadedBytes': sum(r['bytes'] * r['downloads'] for r in rows),
                'reusedDownloads': sum(r['reuses'] for r in rows)}
