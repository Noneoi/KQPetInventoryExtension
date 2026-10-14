"""Coverage is a byte-backed inventory, never a success/semantic guess."""
from pathlib import Path
import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/battle_replay"))
import audit_source_coverage as coverage


class SourceCoverageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="replay-input-coverage-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.source = self.work / "first-scratch" / "latest"
        self.source.mkdir(parents=True)

    def put(self, relative, value, root=None):
        path = (root or self.source) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(value, (dict, list)): value = json.dumps(value, ensure_ascii=False)
        path.write_bytes(value.encode("utf-8") if isinstance(value, str) else value)
        return path

    def xml(self, name, text):
        return self.put("xml/mmo.battleconfig.data." + name + "__xmlClass.xml", text)

    def audit(self, **kwargs):
        return coverage.audit_source_coverage(self.source, **kwargs)

    @staticmethod
    def row(report, suffix):
        matches = [row for row in report["artifacts"] if row["key"] == "source:" + suffix]
        if len(matches) != 1: raise AssertionError((suffix, [row["key"] for row in report["artifacts"]]))
        return matches[0]

    def test_explicit_fields_raw_retention_and_empty_description(self):
        self.xml("BufferDescConfigXML", '<config><buff><s id="3" nm="" mess="" beffect="" bhs="" future="opaque"/></buff></config>')
        self.xml("NewOfficialBuffTable", '<config><s id="9" damage="123"/></config>')
        report = self.audit()
        row = self.row(report, "xml/mmo.battleconfig.data.BufferDescConfigXML__xmlClass.xml")
        self.assertEqual(row["fields"]["/buff/s/@mess"]["coverage"], "consumed-by-explicit-contract")
        self.assertEqual(row["fields"]["/buff/s/@mess"]["emptyValues"], 1)
        self.assertEqual(row["fields"]["/buff/s/@future"]["coverage"], "retained-not-consumed")
        self.assertFalse(row["fields"]["/buff/s/@future"]["meaningVerified"])
        unknown = self.row(report, "xml/mmo.battleconfig.data.NewOfficialBuffTable__xmlClass.xml")
        self.assertEqual(unknown["parserContracts"], [])
        self.assertEqual(unknown["fields"]["/s/@damage"]["coverage"], "unknown-field")
        self.assertEqual(report["issues"], [])
        self.assertFalse(report["parserExecutionVerified"])

    def test_h5_pipe_extras_and_raw_only_tables_are_not_consumed(self):
        self.put("h5-tables/buffer_config.json", {"buff": {"7": "|".join([""] * 14 + ["unreviewed"]), "jia": "label|"}, "pfmap": {"7": 4}})
        self.put("h5-tables/skill_config.json", {"123": {"action": 3, "unknown": 9}, "200000c21": {"action": 1}})
        self.put("h5-tables/new_skills.json", {"123": {"name": "not automatically a known table"}})
        report = self.audit()
        buff = self.row(report, "h5-tables/buffer_config.json")
        self.assertEqual(buff["fields"]["/buff/*/|13"]["decodedField"], "spineFlag")
        self.assertEqual(buff["fields"]["/buff/*/|0"]["occurrences"], 2)
        self.assertFalse(any("/jia" in name for name in buff["fields"]))
        self.assertEqual(buff["fields"]["/buff/*/|14"]["coverage"], "retained-not-consumed")
        self.assertEqual(buff["fields"]["/pfmap/*"]["coverage"], "retained-not-consumed")
        skill = self.row(report, "h5-tables/skill_config.json")
        self.assertEqual(skill["fields"]["/*/action"]["coverage"], "retained-not-consumed")
        self.assertEqual(skill["fields"]["/*/action"]["occurrences"], 2)
        self.assertEqual(self.row(report, "h5-tables/new_skills.json")["integration"], "not-integrated")

    def test_diff_survives_scratch_relocation(self):
        self.xml("BufferDescConfigXML", '<config><buff><s id="1" nm="" mess=""/></buff></config>')
        before = self.audit()
        other = self.work / "another-run" / "latest"
        shutil.copytree(self.source, other)
        after = coverage.audit_source_coverage(other, previous=before)
        self.assertEqual(after["diff"], {"added": [], "missing": [], "bytesChanged": [], "fieldChanges": [], "parserChanges": []})
        self.assertNotEqual(before["artifacts"][0]["sourcePath"], after["artifacts"][0]["sourcePath"])
        self.assertEqual(before["artifacts"][0]["key"], after["artifacts"][0]["key"])
        self.assertEqual(after["reviewRequired"], [])

    def test_missing_consumed_fields_and_added_unknown_require_review(self):
        self.xml("BufferDescConfigXML", '<config><buff><s id="1" nm="name" mess=""/></buff></config>')
        self.xml("EntryConfigXML", '<config><pet><s n="term" d="" t="1"/></pet></config>')
        self.put("h5-tables/entry_config.json", [{"n": "term", "d": "", "t": 1}])
        before = self.audit()
        self.xml("BufferDescConfigXML", '<config><buff><s id="1" nm="name" future="value"/></buff></config>')
        (self.source / "xml/mmo.battleconfig.data.EntryConfigXML__xmlClass.xml").unlink()
        self.put("h5-tables/entry_config.json", [{"n": "term", "d": "", "t": "1"}])
        self.xml("NewTable", '<config><s x=""/></config>')
        after = self.audit(previous=before)
        kinds = {row["kind"] for row in after["reviewRequired"]}
        self.assertTrue({"consumed-field-missing", "consumed-input-missing", "new-unrecognized-field", "field-type-changed", "new-unmapped-table"} <= kinds)
        missing = [row for row in after["reviewRequired"] if row["kind"] == "consumed-field-missing"]
        self.assertEqual(missing[0]["field"], "/buff/s/@mess")
        self.assertFalse(any(row.get("field") == "/[]/d" for row in after["reviewRequired"]))

    def test_new_identity_rows_are_not_new_fields(self):
        self.put("h5-tables/skill_config.json", {"1": {"action": 3}})
        before = self.audit()
        self.put("h5-tables/skill_config.json", {"1": {"action": 2}, "40000000": {"action": 3}})
        after = self.audit(previous=before)
        self.assertEqual(after["diff"]["fieldChanges"], [])
        self.assertEqual(after["reviewRequired"], [])
        self.assertEqual(after["diff"]["bytesChanged"], ["source:h5-tables/skill_config.json"])

    def test_manifest_hashes_and_historical_proof_are_separate_from_freshness(self):
        table = self.xml("EntryConfigXML", '<config><pet><s n="term" d="" t="1"/></pet></config>')
        digest = hashlib.sha256(table.read_bytes()).hexdigest()
        self.put("provenance.json", {"retrievedUtc": "2020-01-01T00:00:00Z", "xml": {"x": {
            "file": table.relative_to(self.source).as_posix(), "sha256": digest, "bytes": table.stat().st_size,
            "resource": "battleconfig/battleconfig", "version": "old-version"}}})
        history = self.put("old/consumer.json", {"sourceEvidence": [], "schemaVersion": 1}, self.work)
        report = self.audit(reference_consumers=history)
        row = self.row(report, "xml/mmo.battleconfig.data.EntryConfigXML__xmlClass.xml")
        self.assertEqual(row["byteVerification"], "local-bytes-match-declarations")
        self.assertEqual(row["provenanceClaims"][0]["version"], "old-version")
        self.assertEqual(row["provenanceClaims"][0]["retrievedUtc"], "2020-01-01T00:00:00Z")
        self.assertFalse(report["officialFreshnessVerified"])
        historical = [a for a in report["artifacts"] if a["role"] == "reference_consumers"][0]
        self.assertEqual(historical["evidenceStatus"], "historical-evidence")
        self.assertEqual(historical["sha256"], hashlib.sha256(history.read_bytes()).hexdigest())
        table.write_text('<config><pet/></config>', encoding="utf-8")
        changed = self.audit(previous=report)
        self.assertIn("declared-hash-mismatch", {row["kind"] for row in changed["issues"]})

    def test_manifest_missing_and_ambiguous_material_is_reported(self):
        self.put("provenance.json", {"files": [{"file": "missing.swf", "sha256": "0" * 64}]})
        report = self.audit()
        self.assertEqual(report["issues"][0]["kind"], "declared-material-missing")
        self.assertEqual(report["issues"][0]["declaredSha256"], "0" * 64)
        self.assertEqual(report["issues"][0]["severity"], "review")
        with patch("builtins.print"):
            self.assertEqual(coverage.main([str(self.source), "--output", str(self.work / "partial.json")]), 0)

    def test_literal_as_is_a_data_parser_not_execution(self):
        relative = "battleconfig/scripts/mmo/battleconfig/data/direct/SkillConfigSimpleAC.as"
        self.put(relative, 'package sample { public class SkillConfigSimpleAC { public static const MAP:Object = {"1":{"name":"","desc":"","future":"kept"}}; }}')
        report = self.audit()
        row = self.row(report, relative)
        self.assertEqual(row["structure"]["MAP/rows"], 1)
        self.assertEqual(row["fields"]["/MAP/*/desc"]["coverage"], "consumed-by-explicit-contract")
        self.assertEqual(row["fields"]["/MAP/*/future"]["coverage"], "retained-not-consumed")
        self.put(relative, 'public static const MAP:Object = {"1":executeAnything()};')
        after = self.audit(previous=report)
        self.assertIn("structure-unreadable", {row["kind"] for row in after["issues"]})
        self.assertIn("consumer-script-changed", {row["kind"] for row in after["reviewRequired"]})

    def test_script_reference_does_not_claim_statement_or_field_coverage(self):
        relative = "battleconfig/scripts/mmo/battleconfig/data/EntryConfigXML.as"
        self.put(relative, "public class EntryConfigXML { public var arbitraryNewField:int; }")
        row = self.row(self.audit(), relative)
        self.assertTrue(row["parserContracts"])
        self.assertEqual(row["fields"], {})
        self.assertEqual(row["fieldInspection"], "not-a-table-or-not-expanded")
        self.assertIn("statement coverage not established", row["parserContracts"][0]["reader"])

    def test_decompiled_manifest_path_is_kept_even_outside_usual_subtrees(self):
        path = self.put("petdictionarydata/scripts/mmo/pet/petdictionarydata/PetDictionaryDataContents.as", "// recorded historical dictionary")
        self.put("pet-provenance.json", {"packages": [{"resource": "pet/petdictionarydata", "version": "old", "sha256": "0" * 64,
            "decompiledFile": path.relative_to(self.source).as_posix(), "decompiledSha256": hashlib.sha256(path.read_bytes()).hexdigest()}]})
        row = self.row(self.audit(), path.relative_to(self.source).as_posix())
        self.assertEqual(row["byteVerification"], "local-bytes-match-declarations")
        self.assertEqual(row["parserContracts"][0]["parser"], "extract_pet_mechanisms.py")

    def test_cli_round_trip_and_incompatible_baseline(self):
        self.xml("BufferDescConfigXML", '<config><buff><s id="1" nm="" mess=""/></buff></config>')
        output = self.work / "audit.json"
        with patch("builtins.print"):
            self.assertEqual(coverage.main([str(self.source), "--output", str(output)]), 0)
        report = self.audit(previous=output)
        self.assertEqual(report["reviewRequired"], [])
        with self.assertRaises(ValueError): self.audit(previous={"schemaVersion": 900, "artifacts": []})

    def test_xml_entities_are_never_expanded(self):
        self.xml("BufferDescConfigXML", '<!DOCTYPE config [<!ENTITY data "untrusted">]><config><buff><s id="1" nm="&data;"/></buff></config>')
        report = self.audit()
        self.assertEqual(report["issues"][0]["kind"], "structure-unreadable")
        self.assertIn("DTD/entity", report["issues"][0]["detail"])

    def test_overlapping_roots_share_actual_paths_with_the_explicit_source_contract(self):
        before = self.audit(title_source=self.source.parent)
        self.put('h5-tables/skill_desc_config.json', {'1': 'name|description|metadata'})
        # The reader also reads these physical root copies. Equal content in
        # different paths is not aliasing, and each copy keeps its own contract.
        self.put('skill_desc_config.json', {'1': 'name|description|metadata'})
        self.put('skill_config.json', {'1': {'rawOnly': 2}})
        self.put('h5-tables/unknown.json', {'opaque': True})
        with patch.object(coverage, '_json_fields', wraps=coverage._json_fields) as inspect_fields:
            report = self.audit(title_source=self.source.parent, previous=before)
        self.assertEqual(inspect_fields.call_count, 4, 'Aliases must be resolved before structural parsing')
        self.assertEqual(len(report['artifacts']), 4)
        self.assertEqual(len(report['aliases']), 4)
        self.assertTrue(all(row['role'] == 'source' for row in report['artifacts']))
        self.assertTrue(all(alias['key'].startswith('title_source:latest/') for alias in report['aliases']))
        self.assertEqual({row['key'] for row in report['reviewRequired']}, {'source:h5-tables/unknown.json'})
        root = self.row(report, 'skill_desc_config.json')
        member = self.row(report, 'h5-tables/skill_desc_config.json')
        self.assertEqual(root['sha256'], member['sha256'])
        self.assertNotEqual(root['sourcePath'], member['sourcePath'])
        self.assertNotEqual(root['key'], member['key'])
        self.assertEqual(root['fields']['/*/|1']['coverage'], 'consumed-by-explicit-contract')
        self.assertEqual(self.row(report, 'skill_config.json')['fields']['/*/rawOnly']['coverage'], 'retained-not-consumed')
        for row in report['artifacts']:
            self.assertEqual(row['roles'], ['source', 'title_source'])
            self.assertEqual(row['aliases'][0]['aliasOf'], row['key'])

    def test_equal_bytes_in_independent_files_are_never_merged(self):
        self.put('h5-tables/known-name.json', {'raw': 1})
        other = self.work / 'separate-title-root'
        self.put('same-content.json', {'raw': 1}, other)
        report = self.audit(title_source=other)
        self.assertEqual(len(report['artifacts']), 2)
        self.assertEqual(report['aliases'], [])
        self.assertEqual(len(report['reviewRequired']), 2)

    def test_only_exact_h5_loader_controls_get_the_source_role(self):
        self.put('h5-start.json', {'version': '20261013', 'files': {}})
        self.put('h5-version.json', {'config/battleconfig.aqz': '20261013'})
        self.put('unknown.json', {'version': 'not a loader contract'})
        report = self.audit(title_source=self.source.parent)
        self.assertEqual({row['key'] for row in report['artifacts']},
                         {'source:h5-start.json', 'source:h5-version.json', 'title_source:latest/unknown.json'})
        self.assertEqual(len(report['aliases']), 2)
        self.assertTrue(all(field['coverage'] == 'unknown-field' for row in report['artifacts'] for field in row['fields'].values()))

    def test_overlapping_roles_select_specific_cultivation_contract_and_preserve_claims(self):
        verified = self.work / 'verified'
        path = self.put('xml/mmo.Sgsv3Config_sXMLClass.xml', '<root><s defineId="1" name="star"/></root>', verified)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        self.put('provenance.json', {'xml': {'star': {'file': 'xml/' + path.name, 'sha256': digest,
                                                    'package': 'stargodv3/stargodservice'}}}, verified)
        report = self.audit(verified_source=verified, cultivation_source=verified, title_source=self.work)
        rows = [row for row in report['artifacts'] if row['sourcePath'] == str(path.resolve())]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row['role'], 'cultivation_source')
        self.assertEqual(row['fields']['/s/@defineId']['coverage'], 'consumed-by-explicit-contract')
        self.assertEqual(row['byteVerification'], 'local-bytes-match-declarations')
        self.assertEqual(set(row['roles']), {'verified_source', 'cultivation_source', 'title_source'})
        self.assertTrue(row['provenanceClaims'])

    def test_known_generated_provenance_is_local_and_unknown_siblings_still_require_review(self):
        from update_generated_evidence import REFERENCE_CONSUMERS
        reference = self.put('generated/reference-consumers.json', {'schemaVersion': 1}, self.work)
        race = self.put('generated/race-expansions.json', {'format': 1}, self.work)
        before = self.audit(reference_consumers=reference, race_expansions=race)
        approvals = {name: {'file': '/local/source/' + name + '.as', 'sha256': 'a' * 64} for name in REFERENCE_CONSUMERS}
        reference.write_text(json.dumps({'schemaVersion': 1, 'consumerApprovals': approvals}), encoding='utf-8')
        race.write_text(json.dumps({'format': 1, 'inputTables': [{'file': '/local/pets.json', 'sha256': 'b' * 64}]}), encoding='utf-8')
        after = self.audit(reference_consumers=reference, race_expansions=race, previous=before)
        self.assertEqual(after['reviewRequired'], [])
        local_fields = [field for row in after['artifacts'] for field in row['fields'].values()
                        if field['coverage'] == 'local-generated-provenance']
        self.assertEqual(len(local_fields), 56)
        self.assertTrue(all(not field['meaningVerified'] and not field['consumerContracts'] for field in local_fields))
        self.assertTrue(all(field['producerContract']['producer'] == 'update_generated_evidence.py' for field in local_fields))
        approvals['new_unreviewed_consumer'] = {'file': '/local/new.as', 'sha256': 'c' * 64}
        approvals['skill_icon']['unreviewed'] = True
        approvals['battle_config']['sha256'] = 123
        reference.write_text(json.dumps({'schemaVersion': 1, 'consumerApprovals': approvals}), encoding='utf-8')
        race.write_text(json.dumps({'format': 1, 'inputTables': [{'file': '/local/pets.json', 'sha256': 'b' * 64, 'newMeaning': 2}]}), encoding='utf-8')
        unknown = self.audit(reference_consumers=reference, race_expansions=race, previous=after)
        fields = {row.get('field'): row['kind'] for row in unknown['reviewRequired']}
        self.assertEqual(fields['/consumerApprovals/skill_icon/unreviewed'], 'new-unrecognized-field')
        self.assertEqual(fields['/consumerApprovals/new_unreviewed_consumer/file'], 'new-unrecognized-field')
        self.assertEqual(fields['/consumerApprovals/battle_config/sha256'], 'field-type-changed')
        self.assertEqual(fields['/inputTables/[]/newMeaning'], 'new-unrecognized-field')

    def test_generated_field_names_in_official_tables_do_not_gain_a_local_contract(self):
        self.put('h5-tables/new-table.json', {'before': True})
        before = self.audit()
        self.put('h5-tables/new-table.json', {'before': True, 'consumerApprovals': {'skill_icon': {'file': 'x', 'sha256': 'a' * 64}}})
        after = self.audit(previous=before)
        row = self.row(after, 'h5-tables/new-table.json')
        self.assertTrue(all(field['coverage'] == 'unknown-field' for field in row['fields'].values()))
        self.assertTrue(any(item.get('field') == '/consumerApprovals/skill_icon/file' for item in after['reviewRequired']))

    def test_file_argument_moves_to_generated_output_while_parent_retains_the_unchanged_old_file(self):
        historical = self.put('identity-reference/race-expansions.json', {'format': 1}, self.source.parent)
        before = self.audit(title_source=self.source.parent, race_expansions=historical)
        original = historical.read_bytes()
        fresh = self.put('fresh/race-expansions.json', {'format': 1, 'inputTables': [{'file': 'pets.json', 'sha256': 'a' * 64}]}, self.work)
        after = self.audit(title_source=self.source.parent, race_expansions=fresh, previous=before)
        self.assertEqual(after['reviewRequired'], [])
        key = 'title_source:identity-reference/race-expansions.json'
        proof = after['diff']['roleReassignments']
        self.assertEqual(len(proof), 1)
        self.assertEqual((proof[0]['key'], proof[0]['previousKey']), (key, 'race_expansions:race-expansions.json'))
        self.assertEqual(proof[0]['sourcePath'], str(historical.resolve()))
        self.assertEqual(proof[0]['sha256'], hashlib.sha256(original).hexdigest())
        self.assertEqual(proof[0]['previousAlias']['key'], key)
        self.assertEqual(proof[0]['previousAlias']['aliasOf'], proof[0]['previousKey'])
        # Equal content at a different path is still a newly encountered input.
        self.put('identity-reference/copy.json', original, self.source.parent)
        copied = self.audit(title_source=self.source.parent, race_expansions=fresh, previous=before)
        self.assertTrue(any(row['kind'] == 'new-unmapped-table' and row['key'].endswith('/copy.json') for row in copied['reviewRequired']))
        # The prior alias cannot excuse changed bytes at its original path.
        historical.write_text('{"format":1,"unknown":true}', encoding='utf-8')
        changed = self.audit(title_source=self.source.parent, race_expansions=fresh, previous=before)
        self.assertTrue(any(row['kind'] == 'new-unmapped-table' and row['key'] == key for row in changed['reviewRequired']))
        self.assertNotIn('roleReassignments', changed['diff'])


if __name__ == "__main__":
    unittest.main()
