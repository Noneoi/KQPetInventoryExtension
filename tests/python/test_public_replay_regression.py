"""Transport/binding failures must never approve a candidate (synthetic IO only)."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import public_replay_regression as subject
import public_catalog_quality as quality


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class RegressionTransportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.file_patch = mock.patch.object(subject, '__file__', str(self.root / 'public_replay_regression.py'))
        self.file_patch.start(); self.addCleanup(self.file_patch.stop)
        (self.root / 'KQBattleReplayUpdateCheck.exe').write_bytes(b'synthetic checker; never executed')
        for name in subject.RUNTIME_FILES:
            (self.root / name).write_bytes(b'synthetic runtime; never loaded')
        self.manifest = {'schemaVersion': 1, 'cases': [{'id': 'sample', 'sha256': 'a' * 64, 'expected': {'actions': 1}}]}
        self.manifest_path = self.root / 'battle-replay-regression-manifest.json'
        self.manifest_path.write_bytes(encoded(self.manifest))
        self.candidate = {'pets': {'1': {'name': 'sample'}}, 'skills': {}, 'buffs': {}}
        self.path = self.root / 'candidate.json'
        self.path.write_bytes(encoded(self.candidate))
        self.report = {'schemaVersion': 1, 'mode': 'candidate-regression', 'passed': True,
            'catalogSha256': sha(self.path.read_bytes()), 'catalogFileSha256': sha(self.path.read_bytes()),
            'fixtureSetSha256': sha(self.manifest_path.read_bytes()), 'errors': [], 'caseCount': 1,
            'cases': [{'id': 'sample', 'passed': True, 'fixtureSha256': 'a' * 64}]}
        self.exit_code = 0
        self.no_output = False
        def run(arguments, **kwargs):
            self.assertEqual(arguments[1::2], ['--catalog', '--report'])
            self.assertEqual(arguments[2], str(self.path))
            self.assertEqual(kwargs['timeout'], 90)
            if not self.no_output:
                Path(arguments[-1]).write_bytes(encoded(self.report))
            return types.SimpleNamespace(returncode=self.exit_code)
        self.process = mock.patch.object(subject.subprocess, 'run', side_effect=run).start()
        self.addCleanup(mock.patch.stopall)

    def check(self):
        return subject.audit_replay_regression(self.path, self.candidate, report_directory=self.root / 'scratch')

    def test_success_is_bound_and_runs_again_when_candidate_unchanged(self):
        first, second = self.check(), self.check()
        self.assertTrue(first['passed']); self.assertTrue(second['passed'])
        self.assertEqual(first['candidateContentSha256'], quality._digest(self.candidate))
        self.assertEqual(self.process.call_count, 2)

    def test_wrong_catalog_or_fixture_binding_rejected(self):
        for key in ('catalogSha256', 'catalogFileSha256', 'fixtureSetSha256'):
            with self.subTest(key=key):
                old = self.report[key]; self.report[key] = 'f' * 64
                self.assertFalse(self.check()['passed']); self.report[key] = old

    def test_incomplete_failed_or_duplicate_case_rejected(self):
        original = copy.deepcopy(self.report)
        for values in ({'caseCount': 0}, {'cases': []}, {'passed': False}, {'errors': ['failure']},
                       {'mode': 'baseline-observation'},
                       {'cases': [{'id': 'other', 'passed': True, 'fixtureSha256': 'a' * 64}]},
                       {'cases': [{'id': 'sample', 'passed': False, 'fixtureSha256': 'a' * 64}]}):
            self.report = dict(original, **values)
            self.assertFalse(self.check()['passed'], values)

    def test_program_failure_and_missing_report_rejected(self):
        self.exit_code = 1
        self.assertFalse(self.check()['passed'])
        self.exit_code = 0; self.no_output = True
        self.assertFalse(self.check()['passed'])

    def test_timeout_and_missing_helper_rejected(self):
        self.process.side_effect = subject.subprocess.TimeoutExpired('checker', 90)
        self.assertFalse(self.check()['passed'])
        (self.root / 'KQBattleReplayUpdateCheck.exe').unlink()
        self.assertFalse(self.check()['passed'])

    def test_candidate_file_does_not_match_object(self):
        self.path.write_bytes(encoded({'pets': {}}))
        self.assertFalse(self.check()['passed'])
        self.process.assert_not_called()

    def test_candidate_changed_during_check_rejected(self):
        original = self.process.side_effect
        def changed(*args, **kwargs):
            result = original(*args, **kwargs)
            self.path.write_bytes(self.path.read_bytes() + b' ')
            return result
        self.process.side_effect = changed
        self.assertFalse(self.check()['passed'])

    def test_unfrozen_manifest_rejected(self):
        self.manifest['cases'][0]['expected'] = {}
        self.manifest_path.write_bytes(encoded(self.manifest))
        self.assertFalse(self.check()['passed']); self.process.assert_not_called()


class FidelityGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(); self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.old = {'pets': {'1': {'huan': ['wrong suffix skill'], 'slots': {}}}, 'skills': {}, 'buffs': {}}
        self.new = copy.deepcopy(self.old); del self.new['pets']['1']['huan']
        self.audit = {'schemaVersion': 1, 'passed': True, 'candidateContentSha256': quality._digest(self.new),
            'inputs': [{'file': 'official', 'sha256': 'a' * 64}], 'findings': [],
            'allowedCorrections': [{'path': '/pets/1/huan', 'rule': 'pet-huan',
                'previousPresent': True, 'candidatePresent': False,
                'previousValueSha256': quality._digest(self.old['pets']['1']['huan']),
                'candidateValueSha256': None, 'source': {'input': 'official', 'pointer': '/1/36', 'sha256': 'a' * 64}}]}

    def check(self):
        return quality.enforce_catalog_quality('skills', self.new, self.old, self.root, source_fidelity=self.audit)

    def test_exact_proven_legacy_join_correction_passes(self):
        self.assertTrue(self.check()['passed'])
        report = json.loads((self.root / 'skills-latest-attempt.json').read_bytes())
        self.assertEqual(report['findings'][0]['resolution'], 'legacy-association-corrected-from-official-consumer')

    def test_wrong_old_new_hash_or_audit_binding_cannot_waive_loss(self):
        original = copy.deepcopy(self.audit)
        for where, key in (('audit', 'candidateContentSha256'), ('receipt', 'previousValueSha256'), ('receipt', 'candidateValueSha256')):
            self.audit = copy.deepcopy(original)
            target = self.audit if where == 'audit' else self.audit['allowedCorrections'][0]
            target[key] = 'f' * 64
            with self.subTest(key=key), self.assertRaises(quality.CatalogQualityError): self.check()

    def test_other_field_loss_and_structural_error_not_waived(self):
        self.old['pets']['1']['description'] = 'must remain'
        with self.assertRaises(quality.CatalogQualityError): self.check()
        del self.old['pets']['1']['description']
        self.new['pets']['1']['slots'] = []
        self.audit['candidateContentSha256'] = quality._digest(self.new)
        with self.assertRaises(quality.CatalogQualityError): self.check()

    def test_failed_source_and_native_reports_are_rejected(self):
        self.audit['passed'] = False
        with self.assertRaises(quality.CatalogQualityError): self.check()
        with self.assertRaises(quality.CatalogQualityError):
            quality.enforce_catalog_quality('battleReplay', self.new, self.new, self.root,
                                           replay_regression={'passed': False, 'candidateContentSha256': quality._digest(self.new)})

    def test_verified_hero_set_receipt_is_bound_to_exact_array_and_kind(self):
        before = {'replaySupplements': {'hero': {'rows': [{'effectiveRaceIds': [1, 2, 3]}]}}}
        after = {'replaySupplements': {'hero': {'rows': [{'effectiveRaceIds': [1, 2]}]}}}
        path = '/replaySupplements/hero/rows/0/effectiveRaceIds'
        audit = {'schemaVersion': 1, 'passed': True, 'inputs': [{'file': 'official'}], 'findings': [],
            'candidateContentSha256': quality._digest(after), 'allowedCorrections': [{
                'path': path, 'rule': 'official-shown-skin-set', 'source': {'sha256': 'a' * 64},
                'previousPresent': True, 'candidatePresent': True,
                'previousValueSha256': quality._digest([1, 2, 3]), 'candidateValueSha256': quality._digest([1, 2])}]}
        findings = [{'kind': 'array-shortened', 'path': path, 'severity': 'review'},
                    {'kind': 'field-removed', 'path': path, 'severity': 'review'},
                    {'kind': 'array-shortened', 'path': path + '/unrelated', 'severity': 'review'},
                    {'kind': 'invalid-reference', 'path': path, 'severity': 'error'}]
        report = {'component': 'battleReplay', 'findings': copy.deepcopy(findings)}
        quality._merge_fidelity(report, after, before, audit)
        self.assertEqual([r['severity'] for r in report['findings']], ['info', 'review', 'review', 'error'])
        audit['allowedCorrections'][0]['previousValueSha256'] = 'f' * 64
        report = {'component': 'battleReplay', 'findings': copy.deepcopy(findings)}
        quality._merge_fidelity(report, after, before, audit)
        self.assertEqual([r['severity'] for r in report['findings']], ['review', 'review', 'review', 'error'])


if __name__ == '__main__': unittest.main()
