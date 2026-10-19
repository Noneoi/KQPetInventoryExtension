import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_data_retention import plan_retention, apply_retention, update_lock
from public_update_store import COMPONENTS, version_files
from public_update_support import atomic_bytes, canonical, sha


class RetentionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='public-retention-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'root'; self.root.mkdir()
        self.old = self.version('old'); self.previous = self.version('previous'); self.current = self.version('current')
        self.attempt = 'a' * 32
        self.publish(self.current, self.previous)

    def write(self, relative, value):
        atomic_bytes(self.root / relative, canonical(value))

    def version(self, tag, reference=''):
        payloads = {name: canonical({'tag': tag, 'reference': reference}) for name in COMPONENTS.values()}
        manifest = {'schema': 1, 'files': {name: {'bytes': len(data), 'sha256': sha(data)} for name, data in payloads.items()},
                    'components': {key: {'file': name, 'available': True} for key, name in COMPONENTS.items()}}
        raw = canonical(manifest); revision = sha(raw)
        folder = self.root / 'catalog/versions' / revision
        for name, data in payloads.items(): atomic_bytes(folder / name, data)
        atomic_bytes(folder / 'manifest.json', raw)
        return revision

    def publish(self, current, previous):
        self.write('catalog/current.json', {'schema': 1, 'revision': current, 'previousRevision': previous, 'attempt': self.attempt})
        self.write('catalog/public-update-status.json', {'schema': 2, 'state': 'completed', 'revision': current,
            'attempt': self.attempt, 'sessionApplied': True, 'diskCommitted': True})

    def test_keep_pointer_pair_and_clean_legacy_only(self):
        for name in ('accounts/user/details/1.json', 'images/pets/1_0.png', 'data-tools/extract-cache/keep.json', 'catalog/shop-selection.json'):
            self.write(name, {'keep': name})
        protected = {name: (self.root/name).read_bytes() for name in ('accounts/user/details/1.json', 'images/pets/1_0.png', 'data-tools/extract-cache/keep.json', 'catalog/shop-selection.json')}
        self.write('public/transactions/'+'b'*32+'/receipt.json', {'state': 'cancelled'})
        self.write('public/transactions/'+'b'*32+'/workspace/old.json', {'large': 'intermediate'})
        self.write('catalog/update-tasks/'+'c'*32+'/result.json', {'state': 'completed'})
        self.write('catalog/update-tasks/'+self.attempt+'/result.json', {'state': 'completed'})
        self.write('public/objects/'+'d'*64+'/data', {'old': True})
        self.write('public/source-cache/extracts/old.json', {'old': True})
        self.write('public/releases/'+'e'*64+'/manifest.json', {'old': True})
        self.write('public/current.json', {'old': True})
        self.write('catalog/pet-detail-data.json', {'legacy': True})
        self.write('catalog/pet-skill-data.json.bak-before-test', {'legacy': True})
        self.write('catalog/backup-before-test/shop-exchange-data.json', {'legacy': True})
        self.write('catalog/unrelated-notes.json', {'keep': True})
        result = apply_retention(plan_retention(self.root, self.current))
        self.assertEqual(result['removedVersions'], 1)
        self.assertGreater(result['removedBytes'], 0)
        self.assertEqual({d.name for d in (self.root/'catalog/versions').iterdir()}, {self.current, self.previous})
        for name, before in protected.items(): self.assertEqual((self.root/name).read_bytes(), before)
        self.assertTrue((self.root/'catalog/unrelated-notes.json').exists())
        self.assertTrue((self.root/'catalog/update-tasks'/self.attempt/'result.json').exists())
        self.assertFalse((self.root/'public/current.json').exists())
        self.assertFalse((self.root/'public/source-cache').exists())
        self.assertEqual(apply_retention(plan_retention(self.root, self.current))['removedFiles'], 0)

    def test_rollback_pair_survives_swapping_and_new_publication(self):
        apply_retention(plan_retention(self.root, self.current))
        self.publish(self.previous, self.current)
        self.assertEqual(apply_retention(plan_retention(self.root, self.previous))['removedVersions'], 0)
        next_revision = self.version('newer')
        self.publish(next_revision, self.previous)
        apply_retention(plan_retention(self.root, next_revision))
        self.assertEqual({d.name for d in (self.root/'catalog/versions').iterdir()}, {next_revision, self.previous})
        version_files(self.root, next_revision); version_files(self.root, self.previous)

    def test_unapplied_candidate_or_corrupt_retained_version_aborts_before_deletion(self):
        self.write('catalog/public-update-status.json', {'state': 'prepared', 'revision': self.current, 'attempt': self.attempt})
        with self.assertRaises(ValueError): plan_retention(self.root, self.current)
        self.assertTrue((self.root/'catalog/versions'/self.old).exists())
        self.publish(self.current, self.previous)
        self.write('catalog/versions/'+self.previous+'/pet-detail-data.json', {'corrupt': True})
        with self.assertRaises(ValueError): plan_retention(self.root, self.current)
        self.assertTrue((self.root/'catalog/versions'/self.old/'manifest.json').exists())

    def test_keep_objects_referenced_by_either_retained_version(self):
        digest='f'*64; other='e'*64
        previous = self.version('reference', 'public/objects/'+digest+'/data')
        self.publish(self.current, previous)
        self.write('public/objects/'+digest+'/data', {'keep': True})
        self.write('public/objects/'+other+'/data', {'old': True})
        result=apply_retention(plan_retention(self.root, self.current))
        self.assertEqual(result['protectedObjects'], [digest])
        self.assertTrue((self.root/'public/objects'/digest/'data').exists())
        self.assertFalse((self.root/'public/objects'/other).exists())

    def test_unknown_files_and_nonterminal_tasks_are_preserved(self):
        self.write('catalog/versions/'+self.old+'/unrelated.txt', {'keep': True})
        self.write('public/transactions/'+'b'*32+'/receipt.json', {'state': 'running'})
        result=apply_retention(plan_retention(self.root, self.current))
        self.assertEqual(result['removedVersions'], 0)
        self.assertEqual(len(result['skipped']), 2)

    def test_partial_old_removal_is_resumable(self):
        (self.root/'catalog/versions'/self.old/'manifest.json').unlink()
        result=apply_retention(plan_retention(self.root, self.current))
        self.assertEqual(result['removedVersions'], 1)
        self.assertFalse((self.root/'catalog/versions'/self.old).exists())

    def test_changed_pointer_or_file_stops_before_delete(self):
        plan=plan_retention(self.root, self.current)
        self.publish(self.previous, self.current)
        with self.assertRaises(ValueError): apply_retention(plan)
        self.publish(self.current, self.previous)
        plan=plan_retention(self.root, self.current)
        self.write('catalog/versions/'+self.old+'/pet-detail-data.json', {'changed': True})
        with self.assertRaises(ValueError): apply_retention(plan)
        self.assertTrue((self.root/'catalog/versions'/self.old/'manifest.json').exists())

    def test_directory_junction_is_rejected(self):
        outside=Path(self.temp.name)/'outside'; outside.mkdir()
        marker=outside/'keep'; marker.write_bytes(b'keep')
        link=self.root/'public/source-cache'; link.parent.mkdir(parents=True)
        if os.name == 'nt':
            run=subprocess.run(['cmd','/c','mklink','/J',str(link),str(outside)],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
            if run.returncode: self.skipTest('junction not available')
        else: link.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError): plan_retention(self.root, self.current)
        self.assertEqual(marker.read_bytes(),b'keep')
        self.assertTrue((self.root/'catalog/versions'/self.old/'manifest.json').exists())
        link.rmdir() if os.name == 'nt' else link.unlink()

    def test_update_lock_excludes_cleanup_process(self):
        (self.root/'catalog/publication.lock').write_text(str(os.getpid())+'\nfixture\nlocalhost\n',encoding='utf-8')
        script=Path(__file__).resolve().parents[2]/'tools/public_data_retention.py'
        with update_lock(self.root):
            run=subprocess.run([sys.executable,'-B',str(script),'--data-root',str(self.root),'--revision',self.current,
                '--publication-owner',str(os.getpid())],capture_output=True,timeout=10)
        self.assertEqual(run.returncode,1)
        self.assertFalse(json.loads(run.stdout)['ok'])
        self.assertTrue((self.root/'catalog/versions'/self.old/'manifest.json').exists())

    def test_helper_runs_with_isolated_python_search_path(self):
        (self.root/'catalog/publication.lock').write_text(str(os.getpid())+'\nfixture\nlocalhost\n',encoding='utf-8')
        script=Path(__file__).resolve().parents[2]/'tools/public_data_retention.py'
        run=subprocess.run([sys.executable,'-I','-B',str(script),'--data-root',str(self.root),'--revision',self.current,
            '--publication-owner',str(os.getpid())],capture_output=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertTrue(json.loads(run.stdout)['ok'])
        self.assertFalse((self.root/'catalog/versions'/self.old).exists())


if __name__=='__main__': unittest.main()
