"""Meaningful portability and hostile-container checks for source bundles."""
from pathlib import Path
import hashlib
import json
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/battle_replay'))
import update_source_bundle as bundle


class SourceBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='replay-source-bundle-')
        self.addCleanup(self.temporary.cleanup)
        # Windows CI may spell TEMP with an 8.3 alias. Build the synthetic
        # source trace in the same canonical form as the real input tracer.
        self.work = Path(self.temporary.name).resolve()
        self.project = self.work / 'original project'
        self.project.mkdir()
        self.rows = []
        self.archive = self.work / 'sources.zip'

    def add(self, relative, data, *, external=False):
        path = (self.work / 'external original' if external else self.project) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if not isinstance(data, bytes):
            data = json.dumps(data, ensure_ascii=False, indent=2).encode('utf-8')
        path.write_bytes(data)
        self.rows.append({'path': str(path), 'projectRelative': None if external else relative,
                          'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        return path

    def create(self, arguments=None, **kwargs):
        return bundle.create_bundle(self.rows, {'arguments': arguments or {'source': '.', 'exports': 'new-output'}},
                                    self.archive, **kwargs)

    def rewrite(self, transform):
        with zipfile.ZipFile(self.archive) as archive:
            entries = [(info, archive.read(info.filename)) for info in archive.infolist()]
        replacement = self.work / 'changed.zip'
        with zipfile.ZipFile(replacement, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for info, content in transform(entries):
                archive.writestr(info, content)
        return replacement

    def test_relocates_chain_and_preserves_original_bytes_and_aliases(self):
        script = self.add('evidence/消费.as', b'package mmo {}\r\n')
        external = self.add('legacy.as', b'official export', external=True)
        child = self.add('evidence/child.json', {'file': str(script), 'sha256': self.rows[0]['sha256'],
                                                   'external': str(external)})
        parent = self.add('evidence/parent.json', {'file': str(child), 'sha256': self.rows[-1]['sha256']})
        before = {row['path']: Path(row['path']).read_bytes() for row in self.rows}
        self.create()
        with zipfile.ZipFile(self.archive) as archive:
            self.assertEqual(archive.read('project/evidence/parent.json'), before[str(parent)])
        destination = self.work / '新 restored copy'
        result = bundle.extract_bundle(self.archive, destination)
        child_copy = result['pathMap'][str(child)]
        parent_copy = json.loads(result['pathMap'][str(parent)].read_bytes())
        self.assertEqual(parent_copy['file'], str(child_copy))
        self.assertEqual(parent_copy['sha256'], hashlib.sha256(child_copy.read_bytes()).hexdigest())
        child_json = json.loads(child_copy.read_bytes())
        self.assertEqual(child_json['file'], str(result['pathMap'][str(script)]))
        self.assertEqual(child_json['external'], str(result['pathMap'][str(external)]))
        self.assertEqual(result['pathMap']['evidence/child.json'], child_copy)
        self.assertEqual(result['pathMap']['evidence\\child.json'], child_copy)
        self.assertEqual(result['arguments']['source'], destination / 'project')
        self.assertEqual(result['pathMap'][str(script)].read_bytes(), before[str(script)])
        self.assertEqual(result['sourceCount'], 4)
        for name, content in before.items():
            self.assertEqual(Path(name).read_bytes(), content)

    def test_extra_files_are_explicit_and_included(self):
        self.add('core.as', b'core')
        extra = self.project / 'more.as'
        extra.write_bytes(b'new generator input')
        self.create(extra_files=['more.as'])
        result = bundle.extract_bundle(self.archive, self.work / 'restore')
        self.assertEqual(result['pathMap']['more.as'].read_bytes(), extra.read_bytes())

    def test_official_json_without_paths_retains_its_exact_bytes(self):
        raw = b'\xef\xbb\xbf{"name":"official","values":[1, 2]}\r\n'
        table = self.add('official/table.json', raw)
        self.add('proof.json', {'file': str(table), 'sha256': self.rows[0]['sha256']})
        self.create()
        result = bundle.extract_bundle(self.archive, self.work / 'restore')
        self.assertEqual(result['pathMap'][str(table)].read_bytes(), raw)
        proof = json.loads(result['pathMap']['proof.json'].read_bytes())
        self.assertEqual(proof['sha256'], hashlib.sha256(raw).hexdigest())

    def test_stale_trace_does_not_replace_previous_bundle(self):
        path = self.add('core.as', b'original')
        self.create()
        before = self.archive.read_bytes()
        path.write_bytes(b'changed')
        with self.assertRaisesRegex(bundle.BundleError, 'differs from read manifest'):
            self.create()
        self.assertEqual(self.archive.read_bytes(), before)

    def test_output_may_not_overwrite_source(self):
        path = self.add('core.as', b'original')
        with self.assertRaisesRegex(bundle.BundleError, 'overwrite an input'):
            bundle.create_bundle(self.rows, {'source': '.'}, path)
        self.assertEqual(path.read_bytes(), b'original')

    def test_tampered_bytes_fail_and_leave_destination_absent(self):
        self.add('core.as', b'original')
        self.create()
        damaged = self.rewrite(lambda entries: [(info, b'XXXXXXXX' if info.filename.endswith('.as') else data)
                                                for info, data in entries])
        destination = self.work / 'restore'
        with self.assertRaisesRegex(bundle.BundleError, 'checksum mismatch'):
            bundle.extract_bundle(damaged, destination)
        self.assertFalse(destination.exists())

    def test_rejects_traversal_absolute_names_and_windows_names(self):
        self.add('core.as', b'original')
        self.create()
        for name in ('../outside', '/outside', 'project/../../outside', 'project\\outside',
                     'project/C:/outside', 'project/CON.txt', 'project/file. '):
            with self.subTest(name=name):
                damaged = self.rewrite(lambda entries: entries + [(name, b'bad')])
                with self.assertRaises(bundle.BundleError):
                    bundle.extract_bundle(damaged, self.work / 'restore')

    def test_rejects_case_collisions_and_symbolic_links(self):
        self.add('core.as', b'original')
        self.create()
        collision = self.rewrite(lambda entries: entries + [('PROJECT/CORE.AS', b'original')])
        with self.assertRaisesRegex(bundle.BundleError, 'Duplicate archive member'):
            bundle.extract_bundle(collision, self.work / 'restore')
        link = zipfile.ZipInfo('project/link')
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        linked = self.rewrite(lambda entries: entries + [(link, b'../../outside')])
        with self.assertRaisesRegex(bundle.BundleError, 'Links, special files'):
            bundle.extract_bundle(linked, self.work / 'restore')
        reparse = zipfile.ZipInfo('project/reparse')
        reparse.create_system = 0
        reparse.external_attr = 0x400
        linked = self.rewrite(lambda entries: entries + [(reparse, b'outside')])
        with self.assertRaisesRegex(bundle.BundleError, 'Links, special files'):
            bundle.extract_bundle(linked, self.work / 'restore')

    def test_manifest_project_alias_cannot_escape(self):
        self.add('core.as', b'original')
        self.create()
        def alter(entries):
            result = []
            for info, content in entries:
                if info.filename == 'manifest.json':
                    manifest = json.loads(content)
                    manifest['files'][0]['projectRelative'] = '../outside'
                    content = json.dumps(manifest).encode('utf-8')
                result.append((info, content))
            return result
        with self.assertRaises(bundle.BundleError):
            bundle.extract_bundle(self.rewrite(alter), self.work / 'restore')

    def test_enforces_count_size_and_archive_limits(self):
        self.add('one.as', b'12345678')
        self.add('two.as', b'abcdefgh')
        self.create()
        for constant, limit in (('MAX_FILES', 1), ('MAX_FILE_BYTES', 7), ('MAX_TOTAL_BYTES', 15),
                                ('MAX_ARCHIVE_BYTES', 1), ('MAX_MANIFEST_BYTES', 1)):
            with self.subTest(constant=constant), patch.object(bundle, constant, limit):
                with self.assertRaises(bundle.BundleError):
                    bundle.extract_bundle(self.archive, self.work / 'restore')

    def test_builder_rejects_oversized_sources_before_reading(self):
        self.add('large.as', b'12345678')
        with patch.object(bundle, 'MAX_FILE_BYTES', 7), patch.object(Path, 'open', side_effect=AssertionError('must not read')):
            with self.assertRaisesRegex(bundle.BundleError, 'expanded-size limit'):
                self.create()

    def test_json_dependency_cycle_fails(self):
        a = self.project / 'a.json'
        b = self.project / 'b.json'
        self.add('a.json', {'file': str(b)})
        self.add('b.json', {'file': str(a)})
        self.create()
        with self.assertRaisesRegex(bundle.BundleError, 'Cyclic JSON'):
            bundle.extract_bundle(self.archive, self.work / 'restore')

    def test_ambiguous_equal_json_copies_fail_instead_of_guessing(self):
        a = self.add('a.json', {'raw': 'same'})
        b = self.add('b.json', {'raw': 'same'})
        self.add('parent.json', {'file': str(a), 'path': str(b), 'sha256': self.rows[0]['sha256']})
        self.create()
        with self.assertRaisesRegex(bundle.BundleError, 'Ambiguous JSON hash'):
            bundle.extract_bundle(self.archive, self.work / 'restore')

    def test_wrong_link_hash_is_not_silently_repaired(self):
        source = self.add('source.as', b'official source')
        self.add('proof.json', {'file': str(source), 'sha256': '0' * 64})
        self.create()
        with self.assertRaisesRegex(bundle.BundleError, 'Recorded source hash'):
            bundle.extract_bundle(self.archive, self.work / 'restore')

    def test_never_merges_into_existing_destination(self):
        self.add('core.as', b'original')
        self.create()
        destination = self.work / 'restore'
        destination.mkdir()
        sentinel = destination / 'user.txt'
        sentinel.write_text('keep', encoding='utf-8')
        with self.assertRaisesRegex(bundle.BundleError, 'new or empty'):
            bundle.extract_bundle(self.archive, destination)
        self.assertEqual(sentinel.read_text(encoding='utf-8'), 'keep')

    def test_rejects_arguments_outside_the_relocated_tree(self):
        self.add('core.as', b'original')
        self.create(arguments={'source': '../../not-a-bundled-source'})
        with self.assertRaisesRegex(bundle.BundleError, 'relocate build argument'):
            bundle.extract_bundle(self.archive, self.work / 'restore')


if __name__ == '__main__':
    unittest.main()
