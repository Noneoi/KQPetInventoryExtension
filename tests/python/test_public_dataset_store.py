"""Publication isolation contracts, using synthetic non-official fixture data."""
from __future__ import annotations

import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import public_dataset_store as store
import public_data_updater as public
from battle_replay.catalog_pack import pack_bytes
from test_battle_replay_catalog_pack import synthetic_catalog
from public_catalog_quality import enforce_catalog_quality


class CandidateStoreTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "shared"
        self.baseline = Path(temporary.name) / "baseline"
        self.baseline.mkdir()
        self.root.mkdir()
        self.replay = pack_bytes(store.canonical(synthetic_catalog()))
        self.initial = {
            "pets": {"pets": {"1": {"name": "old"}}, "source": {"kind": "synthetic-fixture"}},
            "skills": {"pets": {}, "skills": {"1": {"name": "old"}}},
            "shop": {"shops": [], "source": {}},
            "images": {"schemaVersion": 1, "faces": {}, "images": {}},
            "icons": {"schemaVersion": 1, "stargods": {}, "attributes": {}},
            "routines": {"schema": 1, "tasks": [], "source": {"kind": "legacy-fixture"}},
        }
        for name, filename in store.COMPONENTS.items():
            raw = self.replay if name == "battleReplay" else store.canonical(self.initial[name])
            store.atomic_write(self.root / "catalog" / filename, raw)
        store.atomic_write(self.root / "catalog" / "shop-selection.json", b'{"keep":"account selection"}')
        self.legacy = {path.name: path.read_bytes() for path in (self.root / "catalog").iterdir()}
        self.changes, self.failures, self.omit_quality = {}, set(), set()
        self.result_flags = {}
        self.stable_error = None
        self.last = None
        blocked = patch("urllib.request.urlopen", side_effect=AssertionError("No network in candidate contracts"))
        blocked.start()
        self.addCleanup(blocked.stop)

    def factory(self, root, baseline, scratch, **options):
        owner = self
        self.assertTrue(options["isolated_workspace"])
        self.assertEqual(options["runtime_root"], self.root / "data-tools")
        class FakeUpdater:
            def __init__(self):
                self.root, self.catalog = root, root / "catalog"
                self.tools = root / "data-tools"
                self.quality_results = {}
                self.public_resources = types.SimpleNamespace(assert_stable=self.stable)
            def stable(self):
                if owner.stable_error:
                    raise ValueError(owner.stable_error)
            def run(self, selected):
                self.selected = selected
                result = {}
                for name in store.COMPONENTS:
                    if name not in selected:
                        continue
                    path = self.catalog / store.COMPONENTS[name]
                    old = store.validate_payload(path.read_bytes(), "brpack" if name == "battleReplay" else "json")
                    changed = copy.deepcopy(old)
                    if name in owner.changes:
                        changed.update(owner.changes[name])
                        raw = store.canonical(changed)
                        if name == "battleReplay":
                            raw = pack_bytes(raw)
                        store.atomic_write(path, raw)
                    if name in ("pets", "skills", "battleReplay") and name not in owner.omit_quality:
                        self.quality_results[name] = enforce_catalog_quality(name, changed, old, self.tools / "quality")
                    if name in owner.failures:
                        result[name] = {"status": "failed", "error": "synthetic failure"}
                    else:
                        result[name] = {"status": "updated" if name in owner.changes else "unchanged"}
                    result[name].update(owner.result_flags.get(name, {}))
                return result
        self.last = FakeUpdater()
        return self.last

    def candidate(self, requested=None):
        return store.prepare_candidate(self.root, self.baseline, requested, updater_factory=self.factory)

    def manifest(self, candidate):
        return store.parse_json((self.root / "public/releases" / candidate["revision"] / "manifest.json").read_bytes())

    def activate_for_test(self, candidate):
        # Native publication is covered by its own C++ tests. This fixture
        # simulates a pointer created by that publisher, not a Python API.
        store.atomic_write(self.root / "public/current.json", store.canonical({"schema": 1,
            "revision": candidate["revision"], "previousRevision": "", "generation": "a" * 32,
            "attempt": candidate["attempt"]}))

    def assert_legacy_unchanged(self):
        self.assertEqual({path.name: path.read_bytes() for path in (self.root / "catalog").iterdir()}, self.legacy)

    def test_requires_explicit_isolated_low_level_workspace(self):
        with self.assertRaisesRegex(ValueError, "isolated"):
            public.Updater(self.root, self.baseline, self.root / "scratch")

    def test_complete_candidate_never_switches_current_or_legacy(self):
        candidate = self.candidate({"routines"})
        self.assertEqual(candidate["effectiveComponents"], ["routines"])
        self.assertEqual(set(self.manifest(candidate)["components"]), set(store.COMPONENTS))
        self.assertFalse((self.root / "public/current.json").exists())
        self.assert_legacy_unchanged()
        transaction = self.root / "public/transactions" / candidate["attempt"]
        report = (transaction / "validation.json").read_bytes()
        self.assertEqual(store.sha(report), candidate["validation"]["reportSha256"])
        receipt = store.parse_json((transaction / "receipt.json").read_bytes())
        self.assertTrue(receipt["checked"] and receipt["candidatePassed"])
        self.assertFalse(receipt["diskCommitted"] or receipt["sessionApplied"])

    def test_repeated_check_reuses_manifest_and_objects(self):
        first = self.candidate({"routines"})
        self.activate_for_test(first)
        objects = sorted(path.parent.name for path in (self.root / "public/objects").glob("*/data"))
        second = self.candidate({"routines"})
        self.assertEqual(first["revision"], second["revision"])
        self.assertNotEqual(first["attempt"], second["attempt"])
        self.assertEqual(objects, sorted(path.parent.name for path in (self.root / "public/objects").glob("*/data")))
        self.assertEqual(second["baseCurrentSha256"], store.sha((self.root / "public/current.json").read_bytes()))

    def test_successful_legacy_check_records_parser_once_without_replacing_payload_or_provenance(self):
        first = self.candidate({'routines'})
        ref = self.manifest(first)['components']['routines']
        self.assertEqual(ref['sha256'], store.sha(self.legacy[store.COMPONENTS['routines']]))
        self.assertEqual(ref['origin']['kind'], 'legacy-catalog')
        self.assertEqual(ref['origin']['file'], store.COMPONENTS['routines'])
        self.assertEqual(ref['origin']['parserSha256'], store.parser_identity()['sha256'])
        self.assertEqual(self.last.force_components, {'routines'})
        self.activate_for_test(first)
        second = self.candidate({'routines'})
        self.assertEqual(self.last.force_components, set())
        self.assertEqual(first['revision'], second['revision'])
        self.assertEqual(self.manifest(second)['components']['routines'], ref)

    def test_failed_legacy_check_keeps_origin_unstamped_and_next_attempt_forced(self):
        self.failures = {'routines'}
        first = self.candidate({'routines'})
        ref = self.manifest(first)['components']['routines']
        self.assertNotIn('parserSha256', ref['origin'])
        self.activate_for_test(first)
        second = self.candidate({'routines'})
        self.assertEqual(self.last.force_components, {'routines'})
        self.assertEqual(second['revision'], first['revision'])

    def test_selection_closure_is_registered_and_not_based_on_names(self):
        for component in (*store.CORE, "images", "icons"):
            self.assertEqual(set(store.expanded_components({component})), {*store.CORE, "images", "icons"})
        self.assertEqual(store.expanded_components({"routines"}), ["routines"])
        with self.assertRaises(ValueError):
            self.candidate({"skill-looking-file"})

    def test_failed_shared_definition_retains_whole_core_and_derived_images(self):
        base = self.candidate()
        self.activate_for_test(base)
        self.changes = {"pets": {"newMetadata": "do not publish"}, "routines": {"newRule": 2}}
        self.failures = {"skills"}
        result = self.candidate()
        old, new = self.manifest(base), self.manifest(result)
        for name in (*store.CORE, "images", "icons"):
            self.assertEqual(new["components"][name], old["components"][name])
            self.assertEqual(result["results"][name]["status"], "retained")
        self.assertNotEqual(new["components"]["routines"]["sha256"], old["components"]["routines"]["sha256"])
        self.assert_legacy_unchanged()

    def test_independent_image_failure_does_not_reject_core(self):
        self.changes = {"pets": {"newMetadata": "accepted"}}
        self.failures = {"images"}
        result = self.candidate({"pets"})
        self.assertEqual(result["results"]["pets"]["status"], "updated")
        self.assertEqual(result["results"]["images"]["status"], "retained")
        raw = store.read_object_ref(self.root, self.manifest(result)["components"]["pets"])
        self.assertEqual(store.parse_json(raw)["newMetadata"], "accepted")

    def test_failed_shared_reward_consumer_retains_core_despite_partial_builder_output(self):
        base = self.candidate()
        self.activate_for_test(base)
        self.changes = {'pets': {'newMetadata': 'not compatible with retained rewards'}}
        self.result_flags = {'shop': {'dependencyFailure': {'kind': 'reward-definition-consumer',
                              'resources': ['library/interfaces'], 'error': 'consumer changed'}}}
        result = self.candidate({'pets'})
        for name in (*store.CORE, 'images', 'icons'):
            self.assertEqual(self.manifest(result)['components'][name], self.manifest(base)['components'][name])
            self.assertEqual(result['results'][name]['status'], 'retained')

    def test_changed_core_without_independent_report_rejects_candidate(self):
        self.changes = {"pets": {"newMetadata": "must reject"}}
        self.omit_quality = {"pets"}
        with self.assertRaisesRegex(ValueError, "independent quality"):
            self.candidate({"pets"})
        self.assertFalse((self.root / "public/current.json").exists())
        self.assertEqual(list((self.root / "public/transactions").glob("*/candidate.json")), [])
        self.assert_legacy_unchanged()

    def test_parser_identity_change_without_independent_report_rejects_same_payload(self):
        self.omit_quality = {'pets'}
        with self.assertRaisesRegex(ValueError, 'independent quality'):
            self.candidate({'pets'})
        self.assertFalse((self.root / 'public/current.json').exists())
        self.assert_legacy_unchanged()

    def test_official_source_change_during_run_rejects(self):
        self.changes = {"routines": {"newRule": 2}}
        self.stable_error = "official source switched"
        with self.assertRaisesRegex(ValueError, "switched"):
            self.candidate({"routines"})
        self.assertFalse((self.root / "public/current.json").exists())
        self.assert_legacy_unchanged()

    def test_missing_legacy_without_explicit_baseline_is_not_invented(self):
        (self.root / "catalog" / store.COMPONENTS["routines"]).unlink()
        with self.assertRaisesRegex(ValueError, "Invalid public-data file"):
            self.candidate({"routines"})

    def test_existing_missing_component_uses_explicit_baseline_origin(self):
        path = self.root / "catalog" / store.COMPONENTS["routines"]
        store.atomic_write(self.baseline / path.name, path.read_bytes())
        path.unlink()
        result = self.candidate({"routines"})
        self.assertEqual(self.manifest(result)["components"]["routines"]["origin"]["kind"], "embedded-baseline")

    def test_corrupt_pointer_does_not_fall_back_to_legacy(self):
        store.atomic_write(self.root / "public/current.json", b'{"schema":1,"revision":"bad"}')
        with self.assertRaises(ValueError):
            self.candidate({"routines"})
        self.assert_legacy_unchanged()

    def test_object_corruption_rejected_before_builders_run(self):
        result = self.candidate({"routines"})
        self.activate_for_test(result)
        ref = self.manifest(result)["components"]["pets"]
        store.object_path(self.root, ref).write_bytes(b'{"changed":true}')
        self.last = None
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.candidate({"routines"})
        self.assertIsNone(self.last)

    def test_immutable_object_conflict_never_overwrites(self):
        path = self.root / "public/objects" / ("c" * 64) / "data"
        store.immutable_write(path, b'{"first":1}')
        with self.assertRaisesRegex(ValueError, "conflicts"):
            store.immutable_write(path, b'{"second":2}')
        self.assertEqual(path.read_bytes(), b'{"first":1}')

    def test_account_files_do_not_enter_manifest(self):
        result = self.candidate()
        self.assertNotIn("shop-selection.json", self.manifest(result)["attachments"])
        self.assert_legacy_unchanged()

    def test_fixed_image_object_survives_current_update_without_reading_replay(self):
        entry = public.image_entries({}, ("123",), default_version="2000")["123"]
        raw = store.canonical({"schemaVersion": 1, "faces": {"123": entry}, "images": {}})
        fixed = store.put_object(self.root, raw, "json", {"kind": "fixture"})
        store.atomic_write(self.root / "catalog/pet-image-index.json", store.canonical({"faces": {}}))
        with patch.object(store, "load_current", side_effect=AssertionError("fixed lookup must not read current")):
            self.assertEqual(store.image_index(self.root, fixed["sha256"])["faces"]["123"], entry)
        with self.assertRaises(ValueError):
            store.image_index(self.root, "../catalog/pet-image-index.json")

    def test_current_image_lookup_only_reads_selected_object(self):
        result = self.candidate()
        self.activate_for_test(result)
        manifest = self.manifest(result)
        with patch.object(store, "_verified_object_bytes", wraps=store._verified_object_bytes) as checked:
            store.image_index(self.root)
        self.assertEqual(checked.call_count, 1)
        self.assertEqual(checked.call_args.args[1], manifest["components"]["images"])

    def test_publish_requires_native_helper_and_binds_revision(self):
        result = self.candidate()
        with self.assertRaisesRegex(ValueError, "缺失"):
            store.apply_candidate(self.root, result)
        helper = self.root / "data-tools/scripts/KQPublicDatasetCheck.exe"
        store.atomic_write(helper, b"synthetic executable placeholder; never run")
        runner = Mock(return_value=types.SimpleNamespace(returncode=0, stdout=store.canonical({
            "status": "diskCommitted", "revision": result["revision"], "sessionApplied": False}), stderr=b""))
        applied = store.apply_candidate(self.root, result, runner=runner)
        self.assertFalse(applied["sessionApplied"])
        self.assertIn("--apply-candidate", runner.call_args.args[0])
        runner.return_value.stdout = b'{"status":"diskCommitted","revision":"wrong"}'
        with self.assertRaisesRegex(ValueError, "版本不匹配"):
            store.apply_candidate(self.root, result, runner=runner)

    def test_manifest_does_not_include_attempt_clock_or_check_status(self):
        result = self.candidate()
        manifest = self.manifest(result)
        self.assertEqual(set(manifest), {"schema", "dependencySchema", "components", "attachments"})
        self.assertEqual(manifest["attachments"], {})

    def test_icon_freeze_retains_original_and_content_address(self):
        png = b"synthetic PNG bytes, image decoding separately tested"
        expected = store.sha(png)
        store.atomic_write(self.root / "images/stargods/1.png", png)
        self.initial["icons"]["stargods"]["1"] = {"file": "images/stargods/1.png", "png": {"sha256": expected}}
        store.atomic_write(self.root / "catalog/public-icon-index.json", store.canonical(self.initial["icons"]))
        result = self.candidate({"icons"})
        ref = self.manifest(result)["components"]["icons"]
        index = store.parse_json(store.read_object_ref(self.root, ref))
        self.assertEqual(index["stargods"]["1"]["file"], f"images/public-icons/{expected}.png")
        self.assertEqual((self.root / f"images/public-icons/{expected}.png").read_bytes(), png)
        self.assertEqual((self.root / "images/stargods/1.png").read_bytes(), png)

    def test_fidelity_input_bytes_are_preserved_with_exact_original_mapping(self):
        transaction = self.root / 'public/transactions' / ('b' * 32)
        source = transaction / 'workspace/raw/skills.json'
        raw = b'{"actual":"independently read input"}'
        store.atomic_write(source, raw)
        row = {'path': str(source), 'sha256': store.sha(raw), 'bytes': len(raw), 'file': 'skills.json'}
        store.atomic_write(transaction / 'quality/skills.json', store.canonical({'sourceFidelity': {'inputs': {'skills': row}}}))
        refs = {'skills': {'file': 'quality/skills.json'}}
        evidence = store._preserve_evidence(self.root, transaction, refs)
        store._cleanup_workspace(self.root, 'b' * 32, transaction / 'workspace')
        self.assertFalse(source.exists())
        self.assertEqual(evidence[0]['originalPath'], str(source))
        self.assertEqual((self.root / evidence[0]['file']).read_bytes(), raw)

    def test_fidelity_input_change_is_not_approved_by_report_self_description(self):
        transaction = self.root / 'public/transactions' / ('b' * 32)
        source = transaction / 'workspace/raw/skills.json'
        raw = b'{"actual":"input"}'
        row = {'path': str(source), 'sha256': store.sha(raw), 'bytes': len(raw)}
        store.atomic_write(source, b'{"actual":"wrong"}')
        store.atomic_write(transaction / 'quality/skills.json', store.canonical({'sourceFidelity': {'inputs': {'skills': row}}}))
        with self.assertRaisesRegex(ValueError, 'changed after'):
            store._preserve_evidence(self.root, transaction, {'skills': {'file': 'quality/skills.json'}})

    def test_prepare_only_cli_does_not_call_publisher_or_claim_applied(self):
        candidate = {'attempt': 'c' * 32, 'revision': 'd' * 64, 'results': {'routines': {'status': 'unchanged'}}}
        output = io.StringIO()
        with patch.object(sys, 'argv', ['public_data_updater.py', '--data-root', str(self.root), '--prepare-only']), \
                patch.object(store, 'ensure_migrated'), \
                patch.object(store, 'prepare_candidate', return_value=candidate), \
                patch.object(store, 'apply_candidate', side_effect=AssertionError('must not publish')) as publish, \
                contextlib.redirect_stdout(output):
            self.assertEqual(public.main(), 0)
        publish.assert_not_called()
        final = json.loads(output.getvalue().splitlines()[-1])
        self.assertTrue(final['candidatePassed'])
        self.assertFalse(final['diskCommitted'] or final['sessionApplied'])

    def test_standalone_migration_delegates_to_native_and_reuses_existing_pointer(self):
        candidate = self.candidate({'routines'})
        helper = self.root / 'data-tools/scripts/KQPublicDatasetCheck.exe'
        store.atomic_write(helper, b'synthetic placeholder, never executed')
        def migrate(arguments, **kwargs):
            self.assertEqual(arguments[1:], ['--migrate', str(self.root)])
            self.activate_for_test(candidate)
            return types.SimpleNamespace(returncode=0, stderr=b'', stdout=store.canonical({
                'status': 'diskCommitted', 'revision': candidate['revision']}))
        runner = Mock(side_effect=migrate)
        store.ensure_migrated(self.root, runner=runner)
        pointer = (self.root / 'public/current.json').read_bytes()
        store.ensure_migrated(self.root, runner=runner)
        self.assertEqual(runner.call_count, 1)
        self.assertEqual((self.root / 'public/current.json').read_bytes(), pointer)

    def test_native_failure_reports_stdout_reason_and_does_not_claim_commit(self):
        candidate = self.candidate({'routines'})
        helper = self.root / 'data-tools/scripts/KQPublicDatasetCheck.exe'
        store.atomic_write(helper, b'synthetic placeholder, never executed')
        runner = Mock(return_value=types.SimpleNamespace(returncode=1, stderr=b'',
                      stdout=store.canonical({'status': 'conflict', 'error': 'actual pointer conflict'})))
        with self.assertRaisesRegex(ValueError, 'actual pointer conflict'):
            store.apply_candidate(self.root, candidate, runner=runner)
        self.assertFalse((self.root / 'public/current.json').exists())

    def test_persistent_extract_cache_binds_source_parser_and_extractor_bytes(self):
        swf = self.root / 'input.swf'
        store.atomic_write(swf, b'synthetic SWF source bytes')
        java, ffdec = self.root / 'java.exe', self.root / 'ffdec.jar'
        store.atomic_write(java, b'fake java identity')
        store.atomic_write(ffdec, b'fake ffdec identity')
        calls = []
        def exported(command, **kwargs):
            calls.append(command)
            destination = Path(command[-2]) / 'scripts/test/Selected.as'
            store.atomic_write(destination, b'public class Selected {}')
            return types.SimpleNamespace(returncode=0)
        def run(index, parser='a' * 64):
            work = self.root / ('work-' + str(index))
            subject = public.Updater(work, self.baseline, work / 'scratch', isolated_workspace=True)
            subject.runtimes = (java, ffdec)
            subject.source_cache = self.root / 'public/source-cache'
            subject.parser_identity = parser
            return subject.export(swf, work / 'export', 'test.Selected')
        with patch.object(public.subprocess, 'run', side_effect=exported):
            first = run(1)
            self.assertEqual(run(2).read_bytes(), first.read_bytes())
            self.assertEqual(len(calls), 1)
            run(3, 'b' * 64)
            self.assertEqual(len(calls), 2)
            store.atomic_write(swf, b'changed same-version SWF source bytes')
            run(4)
            self.assertEqual(len(calls), 3)
            store.atomic_write(ffdec, b'changed extraction executable')
            run(5)
            self.assertEqual(len(calls), 4)

    def test_existing_request_attempt_is_preserved_and_completed_id_cannot_be_reused(self):
        attempt = 'e' * 32
        request = store.begin_request(self.root, ['routines'], attempt)
        events = []
        def observed(event, **values):
            events.append((event, values))
            if event == 'candidate':
                directory = self.root / 'public/transactions' / attempt
                candidate = store.parse_json((directory / 'candidate.json').read_bytes())
                report = (directory / 'validation.json').read_bytes()
                self.assertEqual(store.sha(report), candidate['validation']['reportSha256'])
                change = store.parse_json(report)['changeReport']
                self.assertEqual(store.sha((directory / change['file']).read_bytes()), change['sha256'])
        candidate = store.prepare_candidate(self.root, self.baseline, ['routines'], updater_factory=self.factory,
                                            attempt_id=attempt, emit=observed)
        self.assertEqual(candidate['attempt'], attempt)
        directory = self.root / 'public/transactions' / attempt
        receipt = store.parse_json((directory / 'receipt.json').read_bytes())
        self.assertEqual(receipt['createdUtc'], request['createdUtc'])
        self.assertGreaterEqual(receipt['timings']['wallSeconds'], 0)
        self.assertEqual(store.parse_json((self.root / 'public/reports/latest-request.json').read_bytes())['attempt'], attempt)
        self.assertTrue(any(event == 'stage' and value['phase'] == 'validating' for event, value in events))
        with self.assertRaisesRegex(ValueError, 'already used'):
            store.begin_request(self.root, ['routines'], attempt)

    def test_request_failures_and_cancellation_do_not_overwrite_completed_publication(self):
        request = store.begin_request(self.root, ['routines'], 'f' * 32)
        store.record_preparation_failure(self.root, request['attempt'], 'bootstrap failed')
        path = self.root / 'public/transactions' / request['attempt'] / 'receipt.json'
        failure = store.parse_json(path.read_bytes())
        self.assertEqual(failure['state'], 'failed')
        self.assertFalse(failure['diskCommitted'])
        failure.update(diskCommitted=True, state='diskCommitted')
        store.atomic_write(path, store.canonical(failure))
        before = path.read_bytes()
        store.record_preparation_failure(self.root, request['attempt'], 'late error')
        self.assertEqual(path.read_bytes(), before)
        with self.assertRaises(ValueError):
            store.begin_request(self.root, ['routines'], '../outside')

    def test_change_summary_preserves_exact_empty_text_and_does_not_invent_names(self):
        first = self.candidate({'routines'})
        self.activate_for_test(first)
        before = self.manifest(first)['components']
        after = copy.deepcopy(before)
        old = {'skills': {'1': {'name': '同名', 'description': '官方原文'}, '2': {'name': '删除'},
                          '3': {'name': '同名', 'description': '另一个'}}}
        new = {'skills': {'1': {'name': '同名', 'description': ''}, '3': {'name': '同名', 'description': '另一个'},
                          '4': {'name': '新增', 'description': '真实说明'}}}
        before['skills'] = store.put_object(self.root, store.canonical(old), 'json', {'kind': 'synthetic-test'})
        after['skills'] = store.put_object(self.root, store.canonical(new), 'json', {'kind': 'synthetic-test'})
        report = store.build_change_report(self.root,before,after,attempt='a'*32,base_revision='b'*64,revision='c'*64)
        self.assertEqual(report['total'], 3)
        changed = next(row for row in report['rows'] if row['kind'] == 'changed')
        self.assertEqual(changed['name'], '同名')
        self.assertEqual(changed['beforeText'], '官方原文')
        self.assertEqual(changed['afterText'], '')
        self.assertEqual(changed['technical']['id'], '1')
        self.assertFalse(any(row['technical']['id'] == '3' for row in report['rows']))
        limited = store.build_change_report(self.root,before,after,attempt='a'*32,base_revision='b'*64,revision='c'*64,limit=1)
        self.assertTrue(limited['truncated'])
        self.assertEqual(limited['total'], 3)
        self.assertEqual(len(limited['rows']), 1)

    def test_change_summary_distinguishes_technical_only_from_user_objects(self):
        candidate = self.candidate({'routines'})
        refs = self.manifest(candidate)['components']; after = copy.deepcopy(refs)
        changed = {**self.initial['pets'], 'source': {'kind': 'same-official-new-source-identity'}}
        after['pets'] = store.put_object(self.root,store.canonical(changed),'json',{'kind':'test'})
        report = store.build_change_report(self.root,refs,after,attempt='a'*32,base_revision='b'*64,revision='c'*64)
        self.assertEqual(report['rows'], [])
        self.assertEqual(report['total'], 0)
        self.assertEqual(report['technicalOnly'], 1)
        self.assertEqual(report['technicalChanges'][0]['table'], 'source')

    def test_icon_source_repackaging_is_technical_and_real_pixel_change_uses_official_name(self):
        candidate = self.candidate({'routines'})
        refs = self.manifest(candidate)['components']; after = copy.deepcopy(refs)
        definitions = {'pets': {}, 'stargods': {'1': {'name': '真实星神名称'}}}
        refs['pets'] = after['pets'] = store.put_object(self.root,store.canonical(definitions),'json',{'kind':'test'})
        old = {'stargods': {'1': {'id': 1, 'file': 'images/stargods/1.png', 'swfSha256': 'a'*64,
                                 'swfBytes': 50, 'png': {'sha256': 'b'*64}}}}
        new = {'stargods': {'1': {'id': 1, 'file': 'images/public-icons/picture.png',
                                 'source': {'sha256': 'a'*64, 'bytes': 50}, 'png': {'sha256': 'b'*64}}}}
        refs['icons'] = store.put_object(self.root,store.canonical(old),'json',{'kind':'test'})
        after['icons'] = store.put_object(self.root,store.canonical(new),'json',{'kind':'test'})
        report = store.build_change_report(self.root,refs,after,attempt='a'*32,base_revision='b'*64,revision='c'*64)
        self.assertEqual(report['total'], 0)
        self.assertEqual(report['technicalOnly'], 1)
        new['stargods']['1']['png']['sha256'] = 'c'*64
        after['icons'] = store.put_object(self.root,store.canonical(new),'json',{'kind':'test'})
        report = store.build_change_report(self.root,refs,after,attempt='a'*32,base_revision='b'*64,revision='c'*64)
        self.assertEqual(report['total'], 1)
        self.assertEqual(report['rows'][0]['name'], '真实星神名称')

    def test_read_object_payload_uses_one_schema_parse_and_rejects_changed_bytes(self):
        ref = store.put_object(self.root, self.replay, 'brpack', {'kind': 'test'})
        original = store.validate_payload
        with patch.object(store, 'validate_payload', wraps=original) as validate:
            value = store.read_object_payload(self.root, ref)
            self.assertIsInstance(value, dict)
            self.assertEqual(validate.call_count, 1)
        store.atomic_write(store.object_path(self.root, ref), b'corrupted')
        with self.assertRaisesRegex(ValueError, 'digest or size'):
            store.read_object_payload(self.root, ref)

    def test_failed_dependency_changes_are_not_reported_as_applied_object_changes(self):
        self.changes['pets'] = {'pets': {'1': {'name': 'rejected candidate name'}}}
        self.failures.add('skills')
        candidate = self.candidate({'pets'})
        directory = self.root / 'public/transactions' / candidate['attempt']
        summary = store.parse_json((directory / 'changes.json').read_bytes())
        self.assertEqual(summary['rows'], [])
        self.assertTrue(candidate['results']['pets']['retryable'])


if __name__ == "__main__":
    unittest.main()
