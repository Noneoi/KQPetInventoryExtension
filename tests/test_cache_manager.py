"""Cache actions run only inside disposable directories, without a client."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile


class CacheManagerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kq-cache-test-')
        self.base = Path(self.temp.name)
        self.root = self.base / 'cache'
        self.client = self.base / 'client'
        self.root.mkdir()
        self.client.mkdir()
        source = Path(__file__).resolve().parents[1] / 'tools' / 'cache-manager.ps1'
        self.script = self.base / 'cache-manager.ps1'
        self.script.write_text(source.read_text(encoding='utf-8-sig'), encoding='utf-8-sig')
        self.write('accounts/123/details/456.json', '{"schema":4,"pet":{"id":456}}')
        self.write('accounts/123/operations/preserved.json', '{}')
        self.write('catalog/pet-detail-data.json', '{}')

    def tearDown(self):
        self.temp.cleanup()

    def write(self, relative, value):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(value, encoding='utf-8')

    def run_action(self, mode, **options):
        request = self.base / 'request.json'
        request.write_text(json.dumps(dict(mode=mode, root=str(self.root),
                                         clientRoot=str(self.client), **options)), encoding='utf-8')
        executable = str(Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe')
        process = subprocess.run([executable, '-NoProfile', '-NonInteractive', '-ExecutionPolicy',
                                  'Bypass', '-File', str(self.script), '-RequestFile', str(request)],
                                 capture_output=True, encoding='utf-8', timeout=30,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        result = json.loads(process.stdout.strip().splitlines()[-1])
        self.assertEqual(process.returncode == 0, result['ok'], process.stderr)
        return result

    def test_inspect_and_exact_deletion(self):
        result = self.run_action('inspect', imageKeys=['1_0', '1_0'])
        self.assertEqual(result['summary']['details'], 1)
        self.assertEqual(result['summary']['missingImages'], 1)
        self.write('accounts/123/derived/pets/456.json', '{}')
        self.assertTrue(self.run_action('clear-detail', account='123', instanceId='456')['ok'])
        self.assertFalse((self.root / 'accounts/123/details/456.json').exists())
        self.assertFalse((self.root / 'accounts/123/derived/pets/456.json').exists())
        self.assertTrue((self.root / 'accounts/123/operations/preserved.json').exists())

    def test_backup_restore_preserves_existing_by_default(self):
        archive = self.base / 'backup.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        self.write('catalog/pet-detail-data.json', '{"new":true}')
        (self.root / 'accounts/123/details/456.json').unlink()
        self.assertTrue(self.run_action('restore', destination=str(archive))['ok'])
        self.assertTrue((self.root / 'accounts/123/details/456.json').exists())
        self.assertIn('new', (self.root / 'catalog/pet-detail-data.json').read_text())
        self.assertTrue(self.run_action('restore', destination=str(archive), overwrite=True)['ok'])
        self.assertEqual((self.root / 'catalog/pet-detail-data.json').read_text(), '{}')

    def test_restore_rejects_aliases_before_writing(self):
        for bad in ['accounts/../escape.json', 'accounts/.. /escape.json', 'images/CON.png', 'catalog/a.json.']:
            archive = self.base / 'bad.zip'
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('catalog/good.json', '{}')
                output.writestr(bad, '{}')
            self.assertFalse(self.run_action('restore', destination=str(archive))['ok'], bad)
            self.assertFalse((self.root / 'catalog/good.json').exists())

    def test_clear_account_and_all_preserve_journals(self):
        self.assertTrue(self.run_action('clear-account', account='123')['ok'])
        self.assertTrue((self.root / 'catalog/pet-detail-data.json').exists())
        self.assertTrue(self.run_action('clear-all')['ok'])
        self.assertFalse((self.root / 'catalog/pet-detail-data.json').exists())
        self.assertTrue((self.root / 'accounts/123/operations/preserved.json').exists())

    def test_migration_commits_after_copy_and_preserves_source(self):
        target = self.base / 'new-cache'
        self.assertTrue(self.run_action('schedule-root', destination=str(target), copyExisting=True)['ok'])
        config = self.client / 'KQPetDataRoot.json'
        self.assertEqual(json.loads(config.read_text())['dataRoot'], str(self.root))
        self.assertTrue(self.run_action('migrate', destination=str(target))['ok'])
        self.assertEqual(json.loads(config.read_text())['dataRoot'], str(target))
        self.assertTrue((self.root / 'accounts/123/details/456.json').exists())
        self.assertTrue((target / 'accounts/123/details/456.json').exists())

    def test_invalid_destination_does_not_change_config(self):
        self.assertFalse(self.run_action('schedule-root', destination=str(self.root / 'nested'), copyExisting=True)['ok'])
        self.assertFalse((self.client / 'KQPetDataRoot.json').exists())


if __name__ == '__main__':
    unittest.main()
