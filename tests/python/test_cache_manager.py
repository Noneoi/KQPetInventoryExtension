"""Cache actions run only inside disposable directories, without a client."""
import ctypes
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
import zipfile


class CacheManagerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kq-cache-test-')
        # Expand 8.3 short names (e.g. RUNNER~1 on CI) so paths match what the
        # cache manager records after resolving the directory.
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / 'cache'
        self.client = self.base / 'client'
        self.root.mkdir()
        self.client.mkdir()
        source = Path(__file__).resolve().parents[2] / 'tools' / 'cache-manager.ps1'
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

    def public_version(self, marker, previous='', legacy_icon=False, skill_raw=None):
        def encoded(value):
            return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
        def put(raw):
            digest = hashlib.sha256(raw).hexdigest()
            target = self.root / 'public/objects' / digest / 'data'
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
            return dict(sha256=digest, bytes=len(raw), format='json', origin=dict(kind='synthetic-test-only'))
        icon = ('synthetic icon ' + marker).encode()
        digest = hashlib.sha256(icon).hexdigest()
        image = 'images/stargods/9.png' if legacy_icon else 'images/public-icons/' + digest + '.png'
        path = self.root / image
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(icon)
        components = {}
        original = put(encoded(dict(syntheticOriginal=marker)))
        for name in ('pets', 'skills', 'shop', 'routines', 'images', 'icons', 'battleReplay'):
            payload = dict(syntheticComponent=name, marker=marker)
            if name == 'pets':
                payload.update(schema=1, pets={'1': dict(name='TEST ONLY')})
            elif name == 'skills':
                payload.update(schema=1, pets={str(n): dict(name='TEST ONLY', slots={}) for n in range(1, 1001)},
                               skills={str(n): dict(name='TEST ONLY', description='TEST ONLY') for n in range(1, 1001)},
                               entries={'test': {}}, buffs={'1': {}},
                               # Shape observed in the real official skills object.
                               psychicsSkills={'': [dict(c='', z1=None, z2=None, t=None, iS=None, cpr=None)]},
                               testOnlyCaseDistinctKeys={'Name': 1, 'name': 2})
            elif name == 'shop':
                payload.update(protocol=dict(extension='test', getInfoCommand='test'), shops=[dict(shopId=1,
                    name='TEST ONLY', goods=[dict(itemServerId=1, description='TEST ONLY', cost='4:1:1',
                                                enhanceType='62', unlock='', shelfTime='20260101', raceIds=[1])])])
            elif name == 'routines':
                payload.update(schema=1, tasks=[dict(id=1, name='TEST ONLY', dayFinish=1, dayActive=0,
                    weekFinish=1, weekDailyMax=1, weekActive=0)], activities=[], dayPrizeThresholds=[], weekPrizeThresholds=[])
            elif name == 'images':
                payload.update(schemaVersion=1, images={})
            elif name == 'battleReplay':
                # Same explicitly synthetic storage fixture and retained source
                # wrapper as the native store test, never official game rows.
                keys = ('decompiledSourceSha256 skillLastEffects buffPowerFullMap relationsBySkill relationsByBuff '
                        'officialEntries officialEntryMentions entryMentionsByBuff entryMentionsBySkill petMechanics '
                        'mechanismRules legendMechanics huanMechanics referenceConsumers petIdentitySummary petDragons '
                        'replaySupplements replayCultivation transformSkills referenceCoverage').split()
                payload.update({key: {} for key in keys})
                payload.update(format=3, tableRelations=[], combos=[], clientPresentation=dict(schemaVersion=1),
                    pets={'1': dict(name='TEST ONLY', identity=dict(status='native', canonicalName='TEST ONLY'), nativeAttributes=['1'], maxPower=100)},
                    skills={'1': dict(id='1', name='TEST ONLY', description='TEST ONLY', configResolution='direct', textSource={}, playbackAvailable=False)},
                    buffs={'1': dict(id='1', **{key: '' for key in ('did nm df once pos beffect bhs mess nameStatus configSource').split()})},
                    attributes={'1': dict(id='1', nm='TEST ONLY', pri='1', rcs='1')},
                    execution={'1': dict(code='1', nm='TEST ONLY', eid='-1', type='1')},
                    bufferExecution={'1': dict(bufferId='1', actionResId='0', skillId='0', lightedSkillId='0', desc='TEST ONLY')},
                    petMechanicsSummary=dict(pets=1), tableJoinSummary=dict(skills=dict(skills=1),
                        buffs=dict(canonicalBuffIds=1, flashBufferExecuteRows=1, flashComboRows=0)))
                source = Path(__file__).resolve().parents[1] / 'fixtures/battle_replay_catalog_fixture.h'
                payload['provenance'] = json.loads(re.search(r'R"JSON\((.*?)\)JSON"', source.read_text(encoding='utf-8'), re.S)[1])
            if name == 'icons':
                payload.update(schemaVersion=1, stargods={'9': dict(file=image, png=dict(sha256=digest))}, attributes={})
            ref = put(skill_raw if name == 'skills' and skill_raw is not None else encoded(payload))
            if name == 'pets':
                ref['origin']['rawObject'] = original
            components[name] = ref
        manifest = encoded(dict(schema=1, dependencySchema=1, components=components, attachments={}))
        revision = hashlib.sha256(manifest).hexdigest()
        self.write('public/releases/' + revision + '/manifest.json', manifest.decode())
        self.write('public/current.json', json.dumps(dict(schema=1, revision=revision,
                   previousRevision=previous, generation='b' * 32, attempt='a' * 32)))
        self.write('public/transactions/' + 'a' * 32 + '/receipt.json', json.dumps(dict(revision=revision, sessionApplied=True)))
        self.write('public/transactions/' + 'a' * 32 + '/workspace/not-a-release.json', '{}')
        evidence = b'synthetic official evidence retained exactly'
        evidence_path = 'public/source-cache/evidence/' + hashlib.sha256(evidence).hexdigest() + '/data'
        self.write(evidence_path, evidence.decode())
        return dict(revision=revision, image=image, components=components, original=original, evidence=evidence_path)

    def native_helper(self):
        project = Path(__file__).resolve().parents[2]
        binary = project / 'build-v2/bin/Release/KQPublicDatasetCheck.exe'
        if not binary.is_file():
            self.skipTest('native public-data checker has not been built')
        target = self.root / 'data-tools/scripts'
        target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, target / binary.name)
        directories = [binary.parent, *(Path(item) for item in os.environ.get('PATH', '').split(os.pathsep) if item)]
        cache = project / 'build-v2/CMakeCache.txt'
        if cache.is_file():
            match = re.search(r'^Qt6Core_DIR:PATH=(.+)$', cache.read_text(encoding='utf-8'), re.M)
            if match:
                directories.append(Path(match[1].strip()).parents[2] / 'bin')
        for name in ('Qt6Core.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
            found = next((directory / name for directory in directories if (directory / name).is_file()), None)
            if found:
                shutil.copy2(found, target / name)

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

    def test_public_payload_empty_and_case_distinct_keys_are_preserved(self):
        current = self.public_version('legal-json-keys')
        path = self.root / 'public/objects' / current['components']['skills']['sha256'] / 'data'
        before = path.read_bytes()
        self.assertIn('', json.loads(before)['psychicsSkills'])
        self.assertEqual(json.loads(before)['testOnlyCaseDistinctKeys'], {'Name': 1, 'name': 2})
        pointer = (self.root / 'public/current.json').read_bytes()
        result = self.run_action('inspect', imageKeys=[])
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['changed'], 0)
        self.assertEqual(result['summary']['details'], 1)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual((self.root / 'public/current.json').read_bytes(), pointer)

    def test_hash_correct_malformed_or_non_object_payload_blocks_deletion(self):
        original_root = self.root
        for index, payload in enumerate((b'{"psychicsSkills":', b'[]', b'123')):
            with self.subTest(payload=payload):
                self.root = original_root / ('invalid-' + str(index))
                self.root.mkdir()
                current = self.public_version('invalid-json-root', skill_raw=payload)
                self.write('images/pets/unbound.png', 'must survive rejected preflight')
                pointer = (self.root / 'public/current.json').read_bytes()
                ref = current['components']['skills']
                self.assertEqual(ref['sha256'], hashlib.sha256(payload).hexdigest())
                self.assertFalse(self.run_action('inspect', imageKeys=[])['ok'])
                self.assertFalse(self.run_action('clear-images')['ok'])
                self.assertTrue((self.root / 'images/pets/unbound.png').exists())
                self.assertTrue((self.root / current['image']).exists())
                self.assertEqual((self.root / 'public/current.json').read_bytes(), pointer)
        self.root = original_root

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

    def test_clear_images_preserves_all_referenced_versions_and_bound_sources(self):
        old = self.public_version('old', legacy_icon=True)
        current = self.public_version('new', previous=old['revision'])
        bound = 'images/pets/sources/' + 'e' * 64 + '/2_0.png'
        self.write(bound, 'held by an older page, absent from current index')
        self.write('images/pets/flat-unbound.png', 'disposable')
        pointer = (self.root / 'public/current.json').read_bytes()
        result = self.run_action('clear-images')
        self.assertTrue(result['ok'])
        self.assertGreaterEqual(result['preserved'], 3)
        for path in (old['image'], current['image'], bound):
            self.assertTrue((self.root / path).exists(), path)
        self.assertFalse((self.root / 'images/pets/flat-unbound.png').exists())
        self.assertEqual((self.root / 'public/current.json').read_bytes(), pointer)
        self.assertTrue(self.run_action('clear-image', relativeFile=old['image'])['ok'])
        self.assertTrue((self.root / old['image']).exists())

    def test_clear_all_keeps_shared_versions_and_compatibility_but_clears_account_details(self):
        current = self.public_version('current')
        self.write('catalog/shop-manual-selection.json', '{"selected":["personal"]}')
        result = self.run_action('clear-all')
        self.assertTrue(result['ok'])
        self.assertTrue((self.root / current['image']).exists())
        self.assertTrue((self.root / 'catalog/pet-detail-data.json').exists())
        self.assertTrue((self.root / 'public/current.json').exists())
        self.assertFalse((self.root / 'accounts/123/details/456.json').exists())
        self.assertTrue((self.root / 'accounts/123/operations/preserved.json').exists())

    def test_backup_and_migration_keep_versions_originals_evidence_and_selection(self):
        old = self.public_version('old', legacy_icon=True)
        current = self.public_version('new', previous=old['revision'])
        self.write('catalog/shop-manual-selection.json', '{"selected":["personal"]}')
        archive = self.base / 'public-backup.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        expected = ['public/current.json', 'public/releases/' + old['revision'] + '/manifest.json',
                    'public/releases/' + current['revision'] + '/manifest.json', current['evidence'],
                    'public/objects/' + old['original']['sha256'] + '/data', old['image'], current['image'],
                    'public/transactions/' + 'a' * 32 + '/receipt.json', 'catalog/shop-manual-selection.json']
        with zipfile.ZipFile(archive) as contents:
            for path in expected:
                self.assertEqual(contents.read(path), (self.root / path).read_bytes(), path)
            self.assertFalse(any('/workspace/' in path for path in contents.namelist()))
        target = self.base / 'migrated-public'
        self.assertTrue(self.run_action('migrate', destination=str(target))['ok'])
        for path in expected:
            self.assertEqual((target / path).read_bytes(), (self.root / path).read_bytes(), path)
        self.assertFalse((target / ('public/transactions/' + 'a' * 32 + '/workspace')).exists())
        self.assertEqual(json.loads((self.client / 'KQPetDataRoot.json').read_text())['dataRoot'], str(target))

    def test_corrupt_public_reference_blocks_deletion_backup_and_root_commit(self):
        current = self.public_version('current')
        self.write('images/pets/flat.png', 'must remain when preflight fails')
        original = self.root / 'public/objects' / current['original']['sha256'] / 'data'
        original.write_bytes(b'corrupt')
        archive = self.base / 'bad-public.zip'
        target = self.base / 'bad-public-root'
        self.assertFalse(self.run_action('clear-images')['ok'])
        self.assertTrue((self.root / 'images/pets/flat.png').exists())
        self.assertFalse(self.run_action('backup', destination=str(archive))['ok'])
        self.assertFalse(archive.exists())
        self.assertFalse(self.run_action('migrate', destination=str(target))['ok'])
        self.assertFalse((self.client / 'KQPetDataRoot.json').exists())

    def test_backup_destination_cannot_replace_current_or_immutable_pixels(self):
        current = self.public_version('current')
        for relative in ('public/current.json', current['image']):
            path = self.root / relative
            before = path.read_bytes()
            self.assertFalse(self.run_action('backup', destination=str(path))['ok'])
            self.assertEqual(path.read_bytes(), before)

    def test_icon_digest_mismatch_does_not_commit_backup_or_migration(self):
        current = self.public_version('current', legacy_icon=True)
        (self.root / current['image']).write_bytes(b'different pixels')
        archive = self.base / 'bad-icon.zip'
        self.assertFalse(self.run_action('backup', destination=str(archive))['ok'])
        self.assertFalse(archive.exists())
        self.assertFalse(self.run_action('migrate', destination=str(self.base / 'bad-icon-root'))['ok'])
        self.assertFalse((self.client / 'KQPetDataRoot.json').exists())

    def test_restore_public_archive_to_empty_and_live_roots_uses_native_commit(self):
        old = self.public_version('backup', legacy_icon=True)
        archive = self.base / 'with-public.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        source_root = self.root
        self.root = self.base / 'empty-restored'
        self.root.mkdir()
        self.native_helper()
        result = self.run_action('restore', destination=str(archive))
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['publicRestoreRevision'], old['revision'])
        self.assertTrue(result['diskCommitted'])
        self.assertFalse(result['sessionApplied'])
        self.assertEqual(json.loads((self.root / 'public/current.json').read_bytes())['revision'], old['revision'])
        self.assertEqual((self.root / old['image']).read_bytes(), (source_root / old['image']).read_bytes())
        newest = self.public_version('live-new', previous=old['revision'])
        (self.root / 'accounts/123/details/456.json').unlink()
        result = self.run_action('restore', destination=str(archive), overwrite=True)
        self.assertTrue(result['ok'], result)
        pointer = json.loads((self.root / 'public/current.json').read_bytes())
        self.assertEqual(pointer['revision'], old['revision'])
        self.assertEqual(pointer['previousRevision'], newest['revision'])
        self.assertTrue((self.root / newest['image']).exists())
        self.assertTrue((self.root / 'accounts/123/details/456.json').exists())

    def test_missing_native_restore_tool_fails_before_account_changes(self):
        self.public_version('archive')
        archive = self.base / 'needs-native.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        self.write('accounts/123/details/456.json', '{"keep":"current"}')
        result = self.run_action('restore', destination=str(archive), overwrite=True)
        self.assertFalse(result['ok'])
        self.assertIn('检查器', result['message'])
        self.assertEqual((self.root / 'accounts/123/details/456.json').read_text(), '{"keep":"current"}')

    def test_native_restore_rejection_does_not_restore_account_files(self):
        current = self.public_version('bad-native-candidate')
        pointer = json.loads((self.root / 'public/current.json').read_bytes())
        manifest = json.loads((self.root / ('public/releases/' + current['revision'] + '/manifest.json')).read_bytes())
        reference = manifest['components']['pets']
        payload = json.loads((self.root / ('public/objects/' + reference['sha256'] + '/data')).read_bytes())
        payload['pets'] = {}  # Graph is complete, but the actual native reader must reject it.
        raw = json.dumps(payload, sort_keys=True, separators=(',', ':')).encode()
        digest = hashlib.sha256(raw).hexdigest()
        self.write('public/objects/' + digest + '/data', raw.decode())
        reference.update(sha256=digest, bytes=len(raw))
        raw_manifest = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
        revision = hashlib.sha256(raw_manifest).hexdigest()
        self.write('public/releases/' + revision + '/manifest.json', raw_manifest.decode())
        pointer['revision'] = revision
        self.write('public/current.json', json.dumps(pointer))
        archive = self.base / 'native-reject.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        self.root = self.base / 'native-rejection-target'
        self.root.mkdir()
        self.public_version('live-current')
        self.native_helper()
        self.write('accounts/123/details/456.json', '{"keep":"current"}')
        before = (self.root / 'public/current.json').read_bytes()
        result = self.run_action('restore', destination=str(archive), overwrite=True)
        self.assertFalse(result['ok'])
        self.assertNotIn('publicRestoreRevision', result)
        self.assertEqual((self.root / 'public/current.json').read_bytes(), before)
        self.assertEqual((self.root / 'accounts/123/details/456.json').read_text(), '{"keep":"current"}')

    def test_account_restore_failure_retains_public_commit_receipt_for_core_reload(self):
        old = self.public_version('backup')
        archive = self.base / 'partial-account-failure.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        self.public_version('new-current', previous=old['revision'])
        self.native_helper()
        target = self.root / 'accounts/123/details/456.json'
        target.unlink()
        target.mkdir()
        (target / 'user-owned.txt').write_text('preserve')
        result = self.run_action('restore', destination=str(archive), overwrite=True)
        self.assertFalse(result['ok'])
        self.assertEqual(result.get('publicRestoreRevision'), old['revision'], result)
        self.assertRegex(result['publicRestoreAttempt'], r'^[0-9a-f]{32}$')
        self.assertTrue(result['diskCommitted'])
        self.assertFalse(result['sessionApplied'])
        self.assertTrue(result['message'])
        self.assertEqual(json.loads((self.root / 'public/current.json').read_bytes())['revision'], old['revision'])
        self.assertEqual((target / 'user-owned.txt').read_text(), 'preserve')

    def test_legacy_backup_keeps_active_public_version_with_explicit_result(self):
        archive = self.base / 'legacy.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        self.public_version('live')
        before = (self.root / 'public/current.json').read_bytes()
        result = self.run_action('restore', destination=str(archive), overwrite=True)
        self.assertTrue(result['ok'], result)
        self.assertIn('旧备份不含公共资料版本', result['message'])
        self.assertNotIn('publicRestoreRevision', result)
        self.assertEqual((self.root / 'public/current.json').read_bytes(), before)

    def test_invalid_public_archive_is_rejected_before_existing_account_writes(self):
        current = self.public_version('archive')
        archive = self.base / 'valid-public.zip'
        self.assertTrue(self.run_action('backup', destination=str(archive))['ok'])
        broken = self.base / 'broken-public.zip'
        with zipfile.ZipFile(archive) as source, zipfile.ZipFile(broken, 'w') as target:
            for entry in source.infolist():
                if entry.filename == 'public/objects/' + current['original']['sha256'] + '/data':
                    continue
                target.writestr(entry.filename, source.read(entry.filename))
        self.write('accounts/123/details/456.json', '{"preserve":"current"}')
        self.assertFalse(self.run_action('restore', destination=str(broken), overwrite=True)['ok'])
        self.assertEqual((self.root / 'accounts/123/details/456.json').read_text(), '{"preserve":"current"}')

    def make_junction(self, link, target):
        # mklink writes its diagnostics in the console code page; only the exit
        # status is used here, so the bytes are not decoded.
        result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(target)],
                                capture_output=True, timeout=30,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        return result.returncode == 0 and Path(link).is_dir()

    def test_migration_rejects_reparse_and_contained_destinations(self):
        real = self.base / 'junction-target'
        real.mkdir()
        link = self.base / 'junction-link'
        if not self.make_junction(link, real):
            self.skipTest('directory junction creation is unavailable in this environment')
        self.assertFalse(self.run_action('migrate', destination=str(link))['ok'])
        self.assertFalse((self.client / 'KQPetDataRoot.json').exists())
        self.assertFalse(self.run_action('migrate', destination=str(self.root / 'nested'))['ok'])
        self.assertFalse(self.run_action('migrate', destination=str(self.base))['ok'])
        self.assertFalse((self.client / 'KQPetDataRoot.json').exists())
        self.assertTrue((self.root / 'accounts/123/details/456.json').exists())

    def test_short_name_destination_is_committed_in_canonical_spelling(self):
        """The cache manager canonicalizes the destination through GetFullPath.

        GetFullPath expands an 8.3 alias against the file system, so the root it
        commits is spelled differently from the request. The launcher can only
        accept that request by comparing directory identity, never by string.
        """
        target = self.base / 'LongAliasDestinationForCache'
        target.mkdir()
        buffer = ctypes.create_unicode_buffer(32768)
        length = ctypes.windll.kernel32.GetShortPathNameW(str(target), buffer, 32768)
        if not length or buffer.value.lower() == str(target).lower():
            self.skipTest('this volume exposes no 8.3 short alias')
        alias = buffer.value
        self.assertNotEqual(alias.lower(), str(target).lower())
        self.assertTrue(self.run_action('migrate', destination=alias)['ok'])
        committed = json.loads((self.client / 'KQPetDataRoot.json').read_text())['dataRoot']
        self.assertNotEqual(committed.lower(), alias.lower())
        self.assertTrue(os.path.samefile(committed, target))


if __name__ == '__main__':
    unittest.main()
