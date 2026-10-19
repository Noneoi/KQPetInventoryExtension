"""Publication and recovery tests with synthetic public data and no game calls."""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_update_store import COMPONENTS, run_update, catalog_path, pointer, version_files, read_json
from public_update_support import atomic_bytes, canonical, UpdateCancelled
from public_data_quality import validate_semantic_transition


class StoreTests(unittest.TestCase):
    def test_embedded_updater_deploys_every_local_python_import(self):
        import ast
        import xml.etree.ElementTree as ET
        root = Path(__file__).resolve().parents[2]
        resources = ET.parse(root / 'src/extension/resources.qrc')
        bundled = {row.attrib['alias'].split('/')[-1] for row in resources.iter('file')
                   if row.attrib.get('alias', '').startswith('data-updater/') and row.attrib['alias'].endswith('.py')}
        deployment = (root / 'src/application/runtime/data_update_service.cpp').read_text(encoding='utf-8')
        for name in bundled:
            for node in ast.walk(ast.parse((root / 'tools' / name).read_text(encoding='utf-8-sig'))):
                imports = [node.module] if isinstance(node, ast.ImportFrom) else [item.name for item in node.names] if isinstance(node, ast.Import) else []
                for module in imports:
                    helper = (module or '').split('.')[0] + '.py'
                    if (root / 'tools' / helper).is_file():
                        self.assertIn(helper, bundled, f'{name} requires an unbundled helper')
                        self.assertIn(f'QStringLiteral("{helper}")', deployment, f'{helper} is bundled but never deployed')

    def test_semantic_guards_allow_official_changes_but_reject_lost_interpretation(self):
        rules = {'relationRules': {'schema': 1, 'summon': {'available': True, 'bySourceRaceId': {'1': [2]}}}}
        with self.assertRaisesRegex(ValueError, '关系表'):
            validate_semantic_transition('skills', rules, {})
        with self.assertRaisesRegex(ValueError, '养成规则'):
            validate_semantic_transition('pets', {'stargods': {'1': {}}}, {'stargods': {}})
        before = {'shops': [{'shopId': 1, 'goods': [{'id': 3, 'rewardRaw': 'same', 'enhanceType': '34', 'raceIds': [1]}]}]}
        after = {'shops': [{'shopId': 1, 'goods': [{'id': 3, 'rewardRaw': 'same', 'enhanceType': '', 'raceIds': []}]}]}
        with self.assertRaisesRegex(ValueError, '官方奖励未变'):
            validate_semantic_transition('shop', before, after)
        after['shops'][0]['goods'][0]['rewardRaw'] = 'official replacement'
        validate_semantic_transition('shop', before, after)
        validate_semantic_transition('shop', before, {'shops': []})
        validate_semantic_transition('skills', rules, rules)

    def test_semantic_loss_keeps_linked_previous_public_snapshot(self):
        self.semantic_loss = True
        prior = dict(self.data['skills'], relationRules={'schema': 1,
            'summon': {'available': True, 'bySourceRaceId': {'1': [2]}}})
        atomic_bytes(self.baseline / COMPONENTS['skills'], canonical(prior))
        result = self.run_update()
        self.assertEqual(result['components']['skills']['status'], 'retained')
        self.assertEqual(result['components']['pets']['status'], 'retained')
        self.assertIn('关系表', result['components']['skills']['error'])
        self.assertEqual(read_json(catalog_path(self.root, COMPONENTS['skills'])), prior)
        self.assertEqual(result['components']['routines']['status'], 'updated')

    def test_embedded_snapshot_gate_rejects_tampering_and_missing_time(self):
        from sync_embedded_public_data import check, FILES, MANIFEST
        from public_update_support import sha
        assets = Path(self.temp.name) / 'assets'
        values = {name: {} for name in FILES}
        values['pet-detail-data.json'] = {'pets': {'1': {}}}
        values['pet-skill-data.json'] = {'pets': {'1': {}}, 'relationRules': {'schema': 1,
            'summon': {'available': True}, 'carry': {'available': True}}}
        values['routine-overview.json'] = {'tasks': [{'id': 1}], 'activities': []}
        records = {}
        for name, value in values.items():
            payload = canonical(value)
            atomic_bytes(assets / name, payload)
            records[name] = {'bytes': len(payload), 'sha256': sha(payload)}
        manifest = {'schema': 1, 'sourceRevision': 'a' * 64, 'verifiedUtc': '2026-10-19T00:00:00Z', 'files': records}
        atomic_bytes(assets / MANIFEST, canonical(manifest))
        check(assets)
        atomic_bytes(assets / 'pet-detail-data.json', b'{}')
        with self.assertRaises(ValueError):
            check(assets)
        manifest['verifiedUtc'] = ''
        atomic_bytes(assets / MANIFEST, canonical(manifest))
        with self.assertRaises(ValueError):
            check(assets)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'data'
        self.baseline = Path(self.temp.name) / 'baseline'
        self.baseline.mkdir()
        self.data = {'pets': {'pets': {'1': {'name': 'old pet'}}},
                     'skills': {'skills': {'1': {'name': 'old skill'}}},
                     'shop': {'shops': [{'shopId': 1, 'goods': [{'id': 1, 'description': 'old goods'}]}]},
                     'images': {'faces': {}}, 'icons': {'stargods': {}, 'attributes': {}}, 'routines': {'tasks': []}}
        for key, value in self.data.items():
            atomic_bytes(self.baseline / COMPONENTS[key], canonical(value))
        self.accounts = self.root / 'accounts/example/details/1.json'
        atomic_bytes(self.accounts, b'{"personal":"retain exactly"}')
        self.failure = None
        self.cancel = False
        self.unstable = False
        self.notices = []
        self.partial_failure = False
        self.semantic_loss = False

    def factory(self, workspace, baseline, scratch, **options):
        parent = self
        class Fake:
            def __init__(self):
                self.activity_exchange_failures = []
                def stable():
                    if parent.unstable:
                        raise ValueError('official version changed mid-update')
                self.sources = types.SimpleNamespace(assert_stable=stable)
            def run(self, selected):
                if parent.cancel:
                    raise UpdateCancelled()
                results = {}
                for key in selected:
                    path = workspace / 'catalog' / COMPONENTS[key]
                    value = read_json(path)
                    if key == 'pets':
                        value['pets']['1']['name'] = 'new pet'
                    elif key == 'skills':
                        value['skills']['1']['name'] = 'new skill'
                        if parent.semantic_loss:
                            value['relationRules'] = {}
                    elif key == 'routines':
                        value['tasks'] = [{'id': 1, 'name': 'new task'}]
                    atomic_bytes(path, canonical(value))
                    results[key] = {'status': 'failed' if key == parent.failure else 'updated'}
                    if key == parent.failure:
                        results[key]['error'] = 'synthetic parser failure'
                    if key == 'shop' and parent.notices:
                        results[key]['notices'] = parent.notices
                    if key == 'shop' and parent.partial_failure:
                        results[key]['error'] = 'one activity download timed out'
                        self.activity_exchange_failures = ['one activity download timed out']
                return results
        return Fake()

    def run_update(self, selected=None, **kwargs):
        return run_update(self.root, self.baseline, selected, updater_factory=self.factory, **kwargs)

    def test_atomic_version_contains_six_categories_and_preserves_account_files(self):
        result = self.run_update()
        self.assertTrue(result['diskCommitted'])
        self.assertFalse(result['sessionApplied'])
        current = pointer(self.root)
        files, manifest = version_files(self.root, current['revision'])
        self.assertEqual(set(manifest['components']), set(COMPONENTS))
        self.assertEqual(read_json(files[COMPONENTS['pets']])['pets']['1']['name'], 'new pet')
        self.assertEqual(self.accounts.read_bytes(), b'{"personal":"retain exactly"}')
        previous, _ = version_files(self.root, current['previousRevision'])
        self.assertEqual(read_json(previous[COMPONENTS['pets']])['pets']['1']['name'], 'old pet')

    def test_failed_core_preserves_group_but_independent_routines_commit(self):
        self.failure = 'skills'
        result = self.run_update()
        self.assertEqual(result['components']['pets']['status'], 'retained')
        self.assertEqual(result['components']['shop']['status'], 'retained')
        self.assertEqual(read_json(catalog_path(self.root, COMPONENTS['pets']))['pets']['1']['name'], 'old pet')
        self.assertEqual(read_json(catalog_path(self.root, COMPONENTS['routines']))['tasks'][0]['name'], 'new task')
        self.assertFalse(result['complete'])

    def test_prepare_only_does_not_change_active_pointer(self):
        before = self.run_update(['pets'])
        old = (self.root / 'catalog/current.json').read_bytes()
        result = self.run_update(['skills'], prepare_only=True)
        self.assertEqual(result['state'], 'prepared')
        self.assertFalse(result['diskCommitted'])
        self.assertEqual((self.root / 'catalog/current.json').read_bytes(), old)
        files, _ = version_files(self.root, result['revision'])
        self.assertEqual(read_json(files[COMPONENTS['skills']])['skills']['1']['name'], 'new skill')
        self.assertEqual(pointer(self.root)['revision'], before['revision'])

    def test_cancel_never_publishes_candidate(self):
        self.cancel = True
        result = self.run_update()
        self.assertEqual(result['state'], 'cancelled')
        self.assertIsNone(pointer(self.root))

    def test_changed_official_snapshot_keeps_active_revision(self):
        self.run_update()
        before = pointer(self.root)
        self.unstable = True
        result = self.run_update()
        self.assertEqual(result['state'], 'failed')
        self.assertEqual(pointer(self.root), before)

    def test_same_data_reuses_version_and_keeps_rollback_target(self):
        self.run_update()
        old = pointer(self.root)
        result = self.run_update()
        self.assertEqual(result['revision'], old['revision'])
        self.assertEqual(pointer(self.root)['previousRevision'], old['previousRevision'])
        self.assertEqual(result['components']['pets']['status'], 'unchanged')

    def test_corrupt_active_payload_is_not_replaced_by_legacy_fallback(self):
        self.run_update()
        catalog_path(self.root, COMPONENTS['skills']).write_bytes(b'corrupted')
        result = self.run_update()
        self.assertEqual(result['state'], 'failed')
        self.assertFalse(result['diskCommitted'])
        self.assertIn('校验失败', result['error'])

    def test_attempt_cannot_be_reused(self):
        attempt = 'a' * 32
        self.run_update(attempt=attempt)
        with self.assertRaises(ValueError):
            self.run_update(attempt=attempt)

    def test_recognition_notices_do_not_fail_publication_or_become_retryable(self):
        self.notices = [{'kind': 'extraction', 'message': '入口关联待确认',
                         'items': [{'activityName': str(i), 'reason': 'pending'} for i in range(25)]}]
        events = []
        result = self.run_update(emit=lambda event, **fields: events.append((event, fields)))
        self.assertTrue(result['complete'])
        self.assertTrue(result['diskCommitted'])
        self.assertEqual(result['components']['shop']['status'], 'unchanged')
        self.assertFalse(result['components']['shop'].get('retryable', False))
        persisted = read_json(self.root / 'catalog/public-update-status.json')
        self.assertEqual(len(persisted['components']['shop']['notices'][0]['items']), 25)
        projected = [fields['report'] for event, fields in events if event == 'report'][-1]
        self.assertEqual(len(projected['components']['shop']['notices'][0]['items']), 12)
        self.assertEqual(projected['components']['shop']['notices'][0]['itemCount'], 25)
        self.partial_failure = True
        result = self.run_update()
        self.assertFalse(result['complete'])
        self.assertEqual(result['components']['shop']['status'], 'partial')
        self.assertTrue(result['components']['shop']['retryable'])

    def test_shop_change_counts_do_not_count_new_groups_as_products(self):
        from public_update_store import changes
        old = {'shops': [{'shopId': 1, 'goods': [{'id': 1}]}]}
        new = {'shops': [{'shopId': 1, 'goods': [{'id': 1}, {'id': 2}]}, {'shopId': 2, 'goods': [{'id': 3}]}]}
        result = changes('shop', old, new)
        self.assertEqual(result['added'], 3)
        self.assertEqual(result['shops']['added'], 1)
        self.assertEqual(result['goods']['added'], 2)


if __name__ == '__main__':
    unittest.main()
