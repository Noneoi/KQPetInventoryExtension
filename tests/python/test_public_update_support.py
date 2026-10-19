"""Source freshness and extraction integrity contracts, without network access."""
import concurrent.futures
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_update_support import PublicSources, ExtractCache, Operations, UpdateCancelled, FLASH, H5


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.calls = []
        self.payloads = {}
        def transport(url, output):
            self.calls.append(url)
            key = 'h5-start' if url.startswith(H5 + 'start~') else url
            output.write_bytes(self.payloads[key])
        self.sources = PublicSources(self.root, transport)

    def test_flash_uses_v_and_hotfix_overrides_instead_of_lv(self):
        self.payloads[FLASH + 'start.xml'] = b'<r><v>20261016123456</v><lv>20261011123456</lv><u><f n="config/config" v="20261017123456"/></u></r>'
        self.payloads[FLASH + 'versiondata~20261016123456.swf'] = ''.join(
            '<f n="module/%d" v="20261016123456"/>' % n for n in range(120)).encode()
        chain = self.sources.flash(lambda p: p.read_text())
        self.assertEqual(chain['version'], '20261016123456')
        self.assertEqual(chain['versions']['config/config'], '20261017123456')
        self.assertNotIn(FLASH + 'versiondata~20261011123456.swf', self.calls)
        self.sources.assert_stable()
        self.payloads[FLASH + 'start.xml'] = b'<r><v>20261018123456</v><u/></r>'
        with self.assertRaisesRegex(ValueError, '切换版本'):
            self.sources.assert_stable()

    def test_same_version_bytes_are_fetched_on_a_new_session(self):
        url = FLASH + 'module~20261016123456.swf'
        self.payloads[url] = b'original'
        before = self.sources.fetch(url, self.root / 'a')
        self.payloads[url] = b'official hotfix under same version'
        other = PublicSources(self.root / 'second', self.sources.transport)
        after = other.fetch(url, self.root / 'b')
        self.assertNotEqual(before['sha256'], after['sha256'])
        self.assertEqual((self.root / 'b').read_bytes(), self.payloads[url])

    def test_shared_concurrent_url_fetches_once(self):
        url = FLASH + 'shared~20261016123456.swf'
        self.payloads[url] = b'shared payload'
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            result = list(pool.map(lambda n: self.sources.fetch(url, self.root / str(n)), range(8)))
        self.assertEqual(self.calls.count(url), 1)
        self.assertEqual(len({row['sha256'] for row in result}), 1)
        self.assertEqual(self.sources.operations.report()['totals']['download']['reused'], 7)

    def test_h5_override_and_mix_alias(self):
        self.payloads['h5-start'] = json.dumps({'version': '20261016123456', 'files': {'config/data.mix': '20261018123456'}}).encode()
        self.payloads[H5 + 'version~20261016123456.json'] = json.dumps({'config/data.mix': '20261011123456'}).encode()
        self.payloads[H5 + 'config/data~20261018123456.aqz'] = b'new configuration'
        path, record = self.sources.h5_resource('config/data.aqz')
        self.assertEqual(record['version'], '20261018123456')
        self.assertEqual(path.read_bytes(), b'new configuration')

    def test_corrupt_shared_download_does_not_silently_pass(self):
        url = FLASH + 'shared~20261016123456.swf'
        self.payloads[url] = b'official'
        self.sources.fetch(url, self.root / 'a')
        self.sources.cache[url][0].write_bytes(b'damaged')
        with self.assertRaisesRegex(ValueError, '缓存内容'):
            self.sources.fetch(url, self.root / 'b')


class ExtractionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.swf = self.root / 'module.swf'
        self.swf.write_bytes(b'official source fixture')
        self.calls = 0
        self.cache = ExtractCache(self.root / 'cache')

    def produce(self, directory):
        self.calls += 1
        (directory / 'config').mkdir()
        (directory / 'config/A.as').write_text('class A { static var price:int=10; }')
        (directory / 'config/B.as').write_text('class B { static var amount:int=3; }')

    def test_reuses_entire_module_and_reextracts_changed_bytes_or_tool(self):
        for index in range(2):
            self.cache.extract(self.swf, self.root / str(index), '*', {'tool': 'one'}, self.produce)
        self.assertEqual(self.calls, 1)
        self.assertEqual((self.root / '0/config/A.as').read_bytes(), (self.root / '1/config/A.as').read_bytes())
        self.swf.write_bytes(b'new official source')
        self.cache.extract(self.swf, self.root / '2', '*', {'tool': 'one'}, self.produce)
        self.cache.extract(self.swf, self.root / '3', '*', {'tool': 'two'}, self.produce)
        self.assertEqual(self.calls, 3)

    def test_corrupt_extraction_is_rejected(self):
        self.cache.extract(self.swf, self.root / 'a', '*', {'tool': 'one'}, self.produce)
        next((self.root / 'cache').rglob('A.as')).write_bytes(b'wrong output')
        with self.assertRaisesRegex(ValueError, '校验失败'):
            self.cache.extract(self.swf, self.root / 'b', '*', {'tool': 'one'}, self.produce)
        self.assertEqual(self.calls, 1)

    def test_cached_path_escape_is_rejected_before_copy(self):
        self.cache.extract(self.swf, self.root / 'a', '*', {}, self.produce)
        marker = next((self.root / 'cache').rglob('record.json'))
        record = json.loads(marker.read_bytes())
        record['files']['../../outside.as'] = '0' * 64
        marker.write_text(json.dumps(record))
        with self.assertRaises(ValueError):
            self.cache.extract(self.swf, self.root / 'b', '*', {}, self.produce)
        self.assertFalse((self.root / 'outside.as').exists())

    def test_cancelled_operation_never_starts_producer(self):
        cache = ExtractCache(self.root / 'cancelled', Operations(cancelled=lambda: True))
        with self.assertRaises(UpdateCancelled):
            cache.extract(self.swf, self.root / 'output', '*', {}, self.produce)
        self.assertEqual(self.calls, 0)


if __name__ == '__main__':
    unittest.main()
