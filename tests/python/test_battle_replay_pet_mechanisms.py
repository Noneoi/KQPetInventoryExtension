import json
import copy
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/battle_replay'))
from extract_pet_mechanisms import (parse_pet_rows, parse_huan_rows, verify_schema,
                                    attach_reference_consumption, FIELDS)


class PetMechanismReferencesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = json.loads((ROOT / 'tests/fixtures/battle_replay_pet_mechanisms.json').read_text(encoding='utf-8'))['rows']

    def parse(self, identifier, known=()):
        row = self.rows[identifier]
        return parse_pet_rows(row['call'], {'file': row['source'], 'sha256': row['sha256']}, dict.fromkeys(known))[identifier]

    def test_skin_uses_explicit_passive_reference_not_race_prefix(self):
        pet = self.parse('6461', ['926228'])
        refs = {row['field']: row for row in pet['skillLinks']}
        self.assertEqual(refs['legendSkillNo']['skillId'], '926228')
        self.assertNotEqual(refs['legendSkillNo']['skillId'], '926461')
        self.assertTrue(refs['legendSkillNo']['resolved'])
        self.assertFalse(refs['heroSkillNo']['resolved'])
        self.assertEqual((pet['mechanismFamily'], pet['qiYunType']), ('qiYun', '1'))

    def test_lingchu_precedes_shared_shenyun_sign(self):
        pet = self.parse('7412')
        self.assertIn('神运', pet['sign'])
        self.assertEqual((pet['mechanismFamily'], pet['qiYunType']), ('suLing', '11'))
        self.assertEqual({r['field']: r['skillId'] for r in pet['skillLinks']}['legendSkillNo'], '927396')

    def test_official_missing_plan_is_not_repaired(self):
        pet = self.parse('6685')
        self.assertEqual((pet['mechanismFamily'], pet['qiYunType']), ('suLing', '1'))
        self.assertEqual(self.parse('5482')['qiYunType'], '0')

    def test_old_legend_field_uses_separate_namespace(self):
        pet = self.parse('3679', ['903679'])
        self.assertEqual(pet['otherReferences']['legendSkillNo'], '903679')
        self.assertEqual(pet['legendReferenceNamespace'], 'legacyLegend')
        self.assertEqual(pet['rawLegendSkillNo'], '903679')
        self.assertNotIn('legendSkillNo', [row['field'] for row in pet['skillLinks']])

    def test_elite_raw_legend_field_is_not_forced_into_either_namespace(self):
        row = self.rows['3679']
        # Keep the literal field and vary only the official sign discriminator.
        from generate_pet_detail_data import iter_create_args
        args = next(iter(iter_create_args(row['call'])))
        args[FIELDS['sign'] - 1] = json.dumps('精英', ensure_ascii=False)
        call = 'PetDictionaryDataItem.create(' + ','.join(args) + ')'
        pet = parse_pet_rows(call, {'file': 'synthetic-schema-case'}, {'903679': {'name': 'same ID'}})['3679']
        self.assertEqual(pet['rawLegendSkillNo'], '903679')
        self.assertEqual(pet['legendReferenceNamespace'], 'unclassified')
        self.assertEqual(pet['otherReferences']['unclassifiedLegendSkillNo'], '903679')
        self.assertNotIn('legendSkillNo', pet['otherReferences'])
        self.assertFalse(any(link['field'] == 'legendSkillNo' for link in pet['skillLinks']))
        self.assertEqual(pet['legendReferenceCandidates'][0]['status'], 'unverifiedNamespace')
        # An absent candidate remains absent; neither numbering nor the elite
        # sign creates a new battle-skill definition.
        unknown = parse_pet_rows(call, {}, {})['3679']
        self.assertEqual(unknown['legendReferenceCandidates'], [])

    def test_confirmed_legend_sign_prevents_same_id_battle_table_fallback(self):
        pet = self.parse('3679', ['903679'])
        self.assertEqual(pet['legendReferenceNamespace'], 'legacyLegend')
        self.assertEqual(pet['legendReferenceCandidates'], [])
        shenyun = self.parse('6461', ['926228'])
        self.assertEqual(shenyun['legendReferenceNamespace'], 'battleSkill')

    def test_schema_shift_rejected_before_parsing(self):
        declarations = '\n'.join(f'var {name}:{"String" if name == "sign" else "int"} = param{index};' for name, index in FIELDS.items())
        methods = '''public function get shenYunSkillNo() : int { return this._legendSkillNo; }
public function isShenYun() : Boolean { return this.findSign("神运"); }
public function isLingChu() : Boolean { return this.findSign("灵初"); }
public function isHuan() : Boolean { return this.findSign("寰力量"); }
public function isYiXingZu() : Boolean { return this.findSign("异形族"); }
public function isSkin() : Boolean { return this.findSign("皮肤"); }
public function isLegend() : Boolean { return this.findSign("传说"); }
public function isElite() : Boolean { return this.findSign("精英"); }
public function get yuanLiSkillNo() : int {
    if(this.isHuan() || this.isYiXingZu()) { return 0; }
    return this._yuanLiSkillNo;
}
public function get huanSkillNo() : int {
    if(!(this.isHuan() || this.isYiXingZu())) { return 0; }
    return this._yuanLiSkillNo;
}'''
        verify_schema(declarations + methods)
        with self.assertRaisesRegex(ValueError, 'legendSkillNo'):
            verify_schema((declarations + methods).replace('param47;', 'param48;'))
        with self.assertRaisesRegex(ValueError, 'Huan namespace'):
            verify_schema((declarations + methods).replace('return 0;', 'return 1;', 1))

    def test_huan_and_alien_getters_use_their_own_table(self):
        for race, identifier in [('3873', '703873'), ('4567', '704567'), ('4832', '704832')]:
            pet = self.parse(race, [identifier])
            self.assertEqual(pet['otherReferences']['huanSkillNo'], identifier)
            self.assertNotIn('yuanLiSkillNo', [row['field'] for row in pet['skillLinks']])
        normal = self.parse('2237', ['702237'])
        self.assertNotIn('huanSkillNo', normal['otherReferences'])
        self.assertTrue(next(row for row in normal['skillLinks'] if row['field'] == 'yuanLiSkillNo')['resolved'])

    def test_huan_retains_each_official_threshold_without_inferring_activation(self):
        definitions = parse_huan_rows((ROOT / 'tests/fixtures/battle_replay_huan.xml').read_text(encoding='utf-8'), {'file': 'official-excerpt'})
        variants = definitions['703873']['variants']
        self.assertEqual([row['valueToTakeEffect'] for row in variants], [0, 16000, 20000, 21500])
        self.assertIn('等级上限提升为125级', variants[0]['tip'])
        self.assertIn('持续2回合', variants[3]['tip'])
        self.assertEqual(definitions['704567']['variants'][0]['name'], '异族审判')
        self.assertNotIn('704832', definitions)
        self.assertNotIn('active', json.dumps(definitions))

    def test_skill_and_plan_references_do_not_create_buff_links(self):
        pet = self.parse('7412', ['927396'])
        self.assertNotIn('buffId', json.dumps(pet))
        self.assertNotIn('active', pet)
        self.assertEqual(pet['source']['createRow'], 1)
        self.assertEqual(pet['otherReferences']['transformSkillNo'], '507396')
        self.assertNotIn('transformSkillNo', [row['field'] for row in pet['skillLinks']])

    def consumer_document(self, pet, source):
        return {'schemaVersion': 1, 'scope': 'Synthetic consumer contract; does not resolve missing definitions.',
                'evidence': {'lookup': {'file': str(source),
                                      'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}},
                'pets': {pet['raceId']: {
                    'dictionarySourceSha256': pet['source']['sha256'],
                    **{key: pet[key] for key in ('sign', 'legendReferenceNamespace', 'qiYunType', 'mechanismFamily')},
                    'references': [{'field': 'legendSkillNo', 'id': pet['rawLegendSkillNo'],
                        'namespace': 'unclassified', 'status': 'unresolvedWithConsumerEvidence',
                        'label': '引用消费核对', 'summary': '空回退并不等于取得定义。',
                        'evidenceIds': ['lookup'],
                        'branches': [{'consumer': 'example', 'selection': 'gated',
                                      'evidenceIds': ['lookup']}]}]}}}

    def test_consumer_findings_do_not_resolve_or_reclassify_references(self):
        from generate_pet_detail_data import iter_create_args
        args = next(iter(iter_create_args(self.rows['3679']['call'])))
        args[FIELDS['sign'] - 1] = json.dumps('精英', ensure_ascii=False)
        pet = parse_pet_rows('PetDictionaryDataItem.create(' + ','.join(args) + ')',
                             {'sha256': 'dictionary-hash'}, {'903679': {'name': 'candidate'}})['3679']
        original = copy.deepcopy(pet)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'consumer.as'
            source.write_text('official consumer excerpt', encoding='utf-8')
            path = Path(directory) / 'evidence.json'
            path.write_text(json.dumps(self.consumer_document(pet, source)), encoding='utf-8')
            result = attach_reference_consumption({'3679': pet}, path)
        self.assertEqual(result['boundReferences'], 1)
        self.assertEqual(pet.pop('referenceConsumption')[0]['status'], 'unresolvedWithConsumerEvidence')
        self.assertEqual(pet, original)
        self.assertEqual(pet['legendReferenceNamespace'], 'unclassified')
        self.assertEqual(pet['legendReferenceCandidates'][0]['status'], 'unverifiedNamespace')

    def test_consumer_import_rejects_changed_source_and_selector(self):
        pet = self.parse('3679')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'consumer.as'
            source.write_text('verified source', encoding='utf-8')
            document = self.consumer_document(pet, source)
            path = Path(directory) / 'evidence.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            source.write_text('changed source', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'source differs'):
                attach_reference_consumption({'3679': pet}, path)
            source.write_text('verified source', encoding='utf-8')
            document['pets']['3679']['sign'] = '精英'
            path.write_text(json.dumps(document), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'selector mismatch'):
                attach_reference_consumption({'3679': pet}, path)
            self.assertNotIn('referenceConsumption', pet)

    def test_consumer_binding_rejection_is_atomic_and_requires_evidence(self):
        pet = self.parse('3679')
        other = self.parse('6461')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'consumer.as'
            source.write_text('verified source', encoding='utf-8')
            document = self.consumer_document(pet, source)
            document['pets'].update(self.consumer_document(other, source)['pets'])
            document['pets']['6461']['references'][0]['id'] = '903679'
            path = Path(directory) / 'evidence.json'
            path.write_text(json.dumps(document), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'binding mismatch'):
                attach_reference_consumption({'3679': pet, '6461': other}, path)
            self.assertNotIn('referenceConsumption', pet)
            self.assertNotIn('referenceConsumption', other)
            document = self.consumer_document(pet, source)
            document['pets']['3679']['references'][0]['branches'][0]['evidenceIds'] = ['missing-source']
            path.write_text(json.dumps(document), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'missing source evidence'):
                attach_reference_consumption({'3679': pet}, path)


if __name__ == '__main__':
    unittest.main()
