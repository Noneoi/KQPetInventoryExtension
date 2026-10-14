"""Offline evidence tests; all generated fixture data is SYNTHETIC, not official."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/battle_replay'))
import update_generated_evidence as evidence


# Synthetic text fragments needed to exercise the guarded consumers. They do
# not execute, and their test hashes are never used as production approvals.
SYNTHETIC_REFERENCE_CONSUMERS = {'battle_config': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                  'return optValue(SkillConfigAdapter.getSkillConfigObj(param1),"name","");\n'
                  'return optValue(SkillConfigAdapter.getSkillConfigObj(param1),"desc","");',
 'battle_fighter': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                   'if(!SkillDefine.checkSkillDefineAvailableInBattle(skillDefine))\n'
                   'throw new Error("缺少技能配置 in skill-config.xml,skill id=" + skillDefine.id);',
 'formation_tips': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                   'if(!pddi.isLingChu())\n'
                   'if(this._formationPetInfo.petBuild == null)\n'
                   'suLingEffectDefine = formationServcie.getSuLingEffectDefineById(pddi.qiYunType);',
 'huan_config': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                'this.parseXMLConfig(this._xmlClass);\n'
                'if(!this._id2Define.containsKey(param1))\n'
                'return [];',
 'legend_config': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                  'public static function getLegendSkillDefinesById(param1:int) : Array\n'
                  'if(_loc4_.id == param1)\n'
                  'if(skillDefines.length == 0)\n'
                  'throw new Error("No any defines for skill "',
 'legend_controller': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                      'if(petId == -1)\n'
                      '_loc1_.getMyPets(PetDataType.TOTAL,false)\n'
                      '_model.setPets(petLoc,LgsPet.valuesFrom(_loc2_));\n'
                      '_model.setPets(petLoc,LgsPet.valuesFrom(param1));',
 'legend_pet': '// SYNTHETIC TEST CONSUMER, never an official export\n'
               'return LgsPet(rest[0]).dictData.legendSkillNo != 0 && !LgsPet(rest[0]).dictData.isShenYun();\n'
               'return Vector.<LgsPet>(param1.map(FIX_MAPPER).filter(FIX_FILTER));\n'
               '_loc1_.getLegendSkillDefineAvailable(this.dictData.legendSkillNo,_loc2_)',
 'legend_stone_module': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                        'if((this._info = param1) == null || !param1.dictData.isLegend())\n'
                        '_loc4_.getLegendSkillDefineAvailable(param1.dictData.legendSkillNo,_loc3_)',
 'qiyun_config': '// SYNTHETIC TEST CONSUMER, never an official export\nif(_loc3_.id == param1)\nreturn null;',
 'skill_adapter': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                  'if(SkillConfigSimpleAC.MAP.hasOwnProperty(_loc2_))\n'
                  'return SkillConfigSimple3000.MAP[_loc2_];\n'
                  'return SkillConfigSimple6000.MAP[_loc2_];\n'
                  'return SkillConfigSimple9999.MAP[_loc2_];',
 'skill_config': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                 'var _loc2_:SkillDefine = this._id2define.getValue(param1);\n'
                 '_loc3_ = this.skillDescConfigXML.getSkillDescDefineById(param1);\n'
                 '_loc4_ = int(SkillDefine.getOrigSkillId(param1));\n'
                 '_loc2_ = SkillDefine.wrapDescOnly(_loc3_);',
 'skill_definition': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                     'public static function wrapDescOnly(param1:SkillDescDefine) : SkillDefine\n'
                     'var _loc2_:SkillDefine = new SkillDefine();\n'
                     'if(param1.actionFrame > 0 && param1.pos > 0 && (param1.effectFrame > 0 || param1.isSpine))\n'
                     'if(!checkIsAutoChessSkillId(param1))',
 'skill_desc_config': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                      '_loc3_ = new XML(<s id="1" nm="" desc=""/>);\n'
                      '_loc3_.@id = param1;\n'
                      '_loc2_ = SkillDescDefine.parseDefine(_loc3_);',
 'skill_desc_definition': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                          'return sBattleConfig.getSkillName(this._nId);\n'
                          'return sBattleConfig.getSkillDesc(this._nId);',
 'skill_icon': '// SYNTHETIC TEST CONSUMER, never an official export\n'
               'if(param1.dicData.isLingChu())\n'
               'else if(param1.dicData.isShenYun())\n'
               'else if(param1.dicData.isLegend())\n'
               'var _loc9_:int = int(param1.dicData.legendSkillNo);\n'
               'shenYunSkillNo = int(context.dicData.shenYunSkillNo);\n'
               'shenYunSkillDefine = context.battleConfig.getSkillDefineById(shenYunSkillNo);\n'
               'var _loc4_:int = int(param2.dicData.shenYunSkillNo);\n'
               '_loc11_ = _formationServcie.getLegendSkillDefinesById(_loc9_);\n'
               '_loc12_ = _loc11_[_loc11_.length - 1];\n'
               'throw new Error("缺少传说技配置：" + _loc9_);\n'
               'param1.battleConfig.getSkillDefineById(param1.dicData.norSkillNo)\n'
               'if(param1.dicData.isGodJob())\n'
               'if(param1.dicData.ultSkillNo > 0)\n'
               '_loc21_ = int(param1.dicData.ultSuperSkillNo);\n'
               'var _loc3_:Boolean = param1.dicData.heroSkillNo > 0;\n'
               'var _loc4_:int = int(param1.dicData.heroSkillNo);\n'
               'if(!context.dicData.isYiXingZu())\n'
               'huanDefines = context.battleConfig.getHuanDefine(context.dicData.huanSkillNo);\n'
               'title = effectDefine == null ? "异族审判" : effectDefine.name;\n'
               'qiYunEffectDefine = _formationServcie.getQiYunEffectDefineById(qiYunType);\n'
               'if(context.petBriefInfo == null || context.petBriefInfo.ownership != 1)\n'
               'desc = qiYunEffectDefine.getDescByValueAndNumKey(i,propValue);',
 'special_attributes': '// SYNTHETIC TEST CONSUMER, never an official export\n'
                       'suLingEffectDefine = '
                       'this._formationService.getSuLingEffectDefineById(this._petInfo.dictData.qiYunType);\n'
                       'if(this._petInfo.dictData.isLingChu())\n'
                       'suLingEffectDefine.getDescByValueAndNumKey(param2,_values[param2])',
 'suling_config': '// SYNTHETIC TEST CONSUMER, never an official export\nif(_loc3_.id == param1)\nreturn null;'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


class GeneratedEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='kq-replay-evidence-test-')
        # Windows TEMP may use an 8.3 alias; production evidence paths resolve
        # it, so fixture paths must use the same canonical root from the start.
        self.root = Path(self.temporary.name).resolve()
        self.addCleanup(self.temporary.cleanup)

    def file(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(value, (dict, list)):
            path.write_bytes(evidence.encode_evidence(value))
        else:
            path.write_text(value, encoding='utf-8')
        return path

    @staticmethod
    def approvals(inputs, keys):
        return {key: sha(inputs[key].read_bytes()) for key in keys}

    def package(self, resource):
        path = self.file('packages/' + resource.replace('/', '_') + '.swf',
                         'SYNTHETIC PACKAGE ONLY: ' + resource)
        return {'resource': resource, 'version': 'SYNTHETIC',
                'url': 'https://aoqi.100bt.com/play/TEST-ONLY/' + resource + '.swf',
                'file': str(path), 'sha256': sha(path.read_bytes())}

    def fortune(self):
        keys = ['critical', 'antiCritical', 'hit', 'evasion', 'strike', 'block', 'strength', 'resist']
        definition = 'public static const KEYS:Array = ' + json.dumps(keys) + ';\n'
        definition += 'public static const CN_KEYS:Array = ' + json.dumps(['test ' + key for key in keys]) + ';'
        qi_definition = 'public static const KEYS:Array = ' + json.dumps(keys[:6]) + ';\n'
        qi_definition += 'public static const CN_KEYS:Array = ' + json.dumps(['test ' + key for key in keys[:6]]) + ';'
        qi = [7] + ['0.1|0.2'] * 6 + ['first|second'] * 6
        su = [9] + ['0.1|0.2'] * 8 + ['first #V1#|second #V2#'] * 8 + ['+1|+2'] * 8 + ['untriggered'] * 8

        def constructor(cls, values):
            return 'new ' + cls + '(' + ','.join(json.dumps(value) for value in values) + ');'

        qih5 = {key + suffix: value for key in keys[:6] for suffix, value in
                [('T', '0.1|0.2'), ('E', 'first|second')]}
        suh5 = {key + suffix: value for key in keys for suffix, value in
                [('T', '0.1|0.2'), ('E', 'first #V1#|second #V2#'), ('C', '+1|+2'), ('NT', 'untriggered')]}
        values = {
            'qiyun_definition': qi_definition, 'suling_definition': definition,
            'qiyun_config': constructor('QiYunEffectDefine', qi),
            'suling_config': constructor('SulingEffectDefine', su) + '\nRULE_STRS:Array = ["test rule"];',
            'dictionary_definition': 'function get legendSkillNo() {}\nfunction get shenYunSkillNo() {}',
            'h5_qiyun': {'7': qih5}, 'h5_suling': {'9': suh5},
            'h5_suling_descriptions': ['test rule'],
        }
        inputs = {key: self.file('fortune/' + key, values.get(key, 'SYNTHETIC REVIEWED CONSUMER ' + key))
                  for key in evidence.FORTUNE_INPUTS}
        return inputs, self.approvals(inputs, evidence.FORTUNE_CONSUMERS)

    def legend(self):
        fields = ('id', 'name', 'desc', 'trigger', 'triggerParam', 'prop', 'battlePower',
                  'descX', 'iconFrame', 'effectName', 'effectValue')
        rows = [[17, 'synthetic legend', '', 1, '1:0-4', '', 0, 'first', 2, '', ''],
                [17, 'synthetic legend', '', 1, '1:5-9', '', 0, 'second', 2, '', '']]
        config = '\n'.join('new LegendSkillDefine(' + ','.join(json.dumps(value) for value in row) + ');' for row in rows)
        config += '\n      public static function getLegendSkillDefinesById() {}'
        definition = '\n'.join(f'this._{field} = param{i};' for i, field in enumerate(fields, 1))
        definition += '\n      private static function triggerWithItemGroup() {}\n      public function get id() {}\n      public function isAvailable() {}'
        h5 = {'17': [{'n': 'synthetic legend', 't': 1, 'tP': row[4], 'p': '', 'b': 0,
                     'dX': row[7], 'iF': 2, 'eN': '', 'eV': ''} for row in rows]}
        values = {'legend_config': config, 'legend_definition': definition,
                  'icon_consumer': 'SYNTHETIC REVIEWED ICON SOURCE', 'h5_legend': h5,
                  'h5_aqlib': 'SYNTHETIC REVIEWED H5 DEFAULTS'}
        inputs = {key: self.file('legend/' + key, value) for key, value in values.items()}
        return inputs, self.approvals(inputs, evidence.LEGEND_CONSUMERS)

    def race(self):
        skins = [{'availablePets': [{'raceId': 20, 'raceIdNeed': 10}], 'show': True, 'fuse': None}]
        values = {
            'pets': {'10': {'name': 'SYNTHETIC'}}, 'carry_xml': '<config><s c="30#1" ced="40#2"/></config>',
            'skin_config': 'SKIN_DEFINES_SRC:Array = ' + json.dumps(skins) + ';',
            'skin_service': 'Cpsv3SkinSummary.getRaceIds(param1,true,true)',
            'skin_definition': 'SYNTHETIC REVIEWED SKIN', 'skin_summary': 'SYNTHETIC REVIEWED SUMMARY',
            'form_config': 'FORM_CONF:Array = [new MultipleFormPetFormDefine([10,30]),new MultipleFormPetFormDefine([30,40])];',
            'form_controller': 'return _loc2_.group;',
            'race_util': 'param1=resetRealPetRaceIds(param1);param1=checkAddAllMultiFormIds(param1);returncheckAddAllSkinIds(param1);\n'
                         'arrayTemp=skinSvc.getSwitchSkinPetRaceIds(raceId);skinRaceId=int(arrayTemp[0]);\n'
                         'arrayTemp=multipleFormPetService.getSwitchPetRaceIds(raceId);',
        }
        names = {'skin_config': 'Cpsv3SkinConfig.as', 'skin_service': 'ChangePetSkinService.as',
                 'skin_definition': 'Cpsv3Skin.as', 'skin_summary': 'Cpsv3SkinSummary.as',
                 'form_config': 'MultipleFormPetConfig.as', 'form_controller': 'MultipleFormPetController.as',
                 'race_util': 'PetRaceUtil.as'}
        inputs = {key: self.file('race/' + names.get(key, key), value) for key, value in values.items()}
        packages = {resource: self.package(resource) for resource in
                    ('changepetskinv3/changepetskinservice', 'multipleformpet/multipleformpetservice', 'library/common')}
        return inputs, self.approvals(inputs, evidence.RACE_CONSUMERS), packages

    def reference(self, race='1', skill='9001'):
        inputs = {key: self.file('reference/' + key + '.as', text)
                  for key, text in SYNTHETIC_REFERENCE_CONSUMERS.items()}
        dictionary = self.file('reference/current-dictionary.as', 'SYNTHETIC CURRENT DICTIONARY ' + race)
        pet = {'raceId': race, 'source': {'file': str(dictionary), 'sha256': sha(dictionary.read_bytes())},
               'sign': '灵初', 'legendReferenceNamespace': 'none', 'qiYunType': '0', 'mechanismFamily': '',
               'rawLegendSkillNo': '0', 'otherReferences': {}, 'skillLinks': [
                   {'field': 'norSkillNo', 'skillId': skill, 'resolved': False, 'label': '普通技能'}]}
        mechanism = {'pets': {race: pet}}
        raw = {'flashTextMaps': {key: {'rows': []} for key in
                               ('SkillConfigSimpleAC', 'SkillConfigSimple3000', 'SkillConfigSimple6000', 'SkillConfigSimple9999')},
               'flashSkillConfig': {'s': []}, 'flashSkillDesc': {'rows': []},
               'h5SkillConfig': {'data': {}}, 'h5SkillDesc': {'data': {}}}
        inputs['pet_mechanisms'] = self.file('reference/fresh-pet-mechanisms.json', mechanism)
        inputs['raw_skill_tables'] = self.file('reference/fresh-raw-skill-tables.json', raw)
        resources = ['interact/interacthelper', 'petpackage/petpackage', 'legend/legendservice',
                     'formation/formationservice', 'battleconfig/battleconfig', 'library/interfaces', 'battle/battle']
        packages = [self.package(resource) for resource in resources]
        inputs['consumer_package_provenance'] = self.file('reference/consumer-package-provenance.json', {'packages': packages[:3]})
        inputs['verified_consumer_provenance'] = self.file('reference/verified-consumer-provenance.json', {'packages': packages[3:]})
        return inputs, self.approvals(inputs, evidence.REFERENCE_CONSUMERS), mechanism, raw, packages

    def build_reference(self, inputs, approvals):
        return evidence.build_reference_consumer_evidence(inputs, approved_sha256=approvals,
            generated_utc='2026-01-01T00:00:00+00:00', snapshot_provenance_path=self.root / 'new-provenance.json')

    def test_fortune_reads_all_channels_without_evaluating_formula(self):
        inputs, approvals = self.fortune()
        result = evidence.build_fortune_rules(inputs, approved_sha256=approvals)
        channel = result['suLing']['plans']['9']['channels'][0]
        self.assertEqual(channel['formulas'], ['+1', '+2'])
        self.assertEqual(channel['thresholds'], [0.1, 0.2])
        self.assertEqual(len(result['qiYun']['plans']['7']['channels']), 6)
        self.assertEqual(result['explicitBattleBuffLinks'], [])
        self.assertNotIn('2026-10-09', result['scope'])

    def test_missing_or_changed_consumer_approval_rejected(self):
        inputs, approvals = self.fortune()
        with self.assertRaisesRegex(ValueError, 'Missing reviewed consumer'):
            evidence.build_fortune_rules(inputs, approved_sha256={})
        inputs['flash_capability_panel'].write_text('changed consumer', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Unreviewed consumer bytes'):
            evidence.build_fortune_rules(inputs, approved_sha256=approvals)

    def test_fortune_changed_h5_table_rejected(self):
        inputs, approvals = self.fortune()
        inputs['h5_qiyun'].write_bytes(evidence.encode_evidence({'8': {}}))
        with self.assertRaisesRegex(ValueError, 'Flash/H5 mismatch'):
            evidence.build_fortune_rules(inputs, approved_sha256=approvals)

    def test_fortune_malformed_and_nonfinite_rows_rejected(self):
        inputs, approvals = self.fortune()
        original = inputs['qiyun_config'].read_text(encoding='utf-8')
        for text, message in [(original.replace('0.1|0.2', 'nan|0.2'), 'Nonfinite'),
                              (original + '\n' + original, 'duplicate'),
                              ('new QiYunEffectDefine(7,"0.1");', 'arity')]:
            with self.subTest(message=message):
                inputs['qiyun_config'].write_text(text, encoding='utf-8')
                approvals['qiyun_config'] = sha(inputs['qiyun_config'].read_bytes())
                with self.assertRaisesRegex(ValueError, message):
                    evidence.build_fortune_rules(inputs, approved_sha256=approvals)

    def test_legend_retains_order_variants_and_exact_companion_hash(self):
        inputs, approvals = self.legend()
        raw_path = self.root / 'not-written/legend-raw.json'
        result, raw = evidence.build_legend_mechanics(inputs, approved_sha256=approvals, raw_tables_path=raw_path)
        self.assertEqual([row['triggerParam'] for row in result['definitions']['17']['variants']], ['1:0-4', '1:5-9'])
        self.assertEqual(result['report']['h5Comparison']['remainingDifferences'], [])
        self.assertEqual(result['sources']['rawTables']['sha256'], sha(evidence.encode_evidence(raw)))
        self.assertFalse(raw_path.exists())

    def test_legend_constructor_change_requires_explicit_failure(self):
        inputs, approvals = self.legend()
        inputs['legend_definition'].write_text('changed definition', encoding='utf-8')
        approvals['legend_definition'] = sha(inputs['legend_definition'].read_bytes())
        with self.assertRaisesRegex(ValueError, 'Official consumer schema changed'):
            evidence.build_legend_mechanics(inputs, approved_sha256=approvals, raw_tables_path=self.root / 'raw.json')

    def test_race_adds_new_carry_seeds_without_transitive_form_guess(self):
        inputs, approvals, packages = self.race()
        result = evidence.build_race_expansion_evidence(inputs, approved_sha256=approvals, packages=packages)
        self.assertEqual(result['byRaceId']['10'], ['10', '20', '30'])
        self.assertEqual(result['byRaceId']['30'], ['10', '20', '30'])
        self.assertIn('40', result['byRaceId'])
        self.assertNotIn('40', result['byRaceId']['10'])

    def test_race_rejects_mismatched_package_and_unknown_consumer(self):
        inputs, approvals, packages = self.race()
        packages['library/common']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'package proof'):
            evidence.build_race_expansion_evidence(inputs, approved_sha256=approvals, packages=packages)
        packages['library/common'] = self.package('library/common')
        inputs['race_util'].write_text('changed selector', encoding='utf-8')
        approvals['race_util'] = sha(inputs['race_util'].read_bytes())
        with self.assertRaisesRegex(ValueError, 'Official consumer schema changed'):
            evidence.build_race_expansion_evidence(inputs, approved_sha256=approvals, packages=packages)

    def test_reference_binding_uses_current_dictionary_and_tables(self):
        inputs, approvals, mechanism, _, _ = self.reference()
        before = inputs['pet_mechanisms'].read_bytes()
        first, missing, provenance = self.build_reference(inputs, approvals)
        self.assertEqual(first['pets']['1']['references'][0]['id'], '9001')
        self.assertEqual(first['pets']['1']['dictionarySourceSha256'], mechanism['pets']['1']['source']['sha256'])
        self.assertEqual(missing['bySkillId']['9001']['flashConfigPresent'], False)
        self.assertEqual(first['evidence']['table-snapshot-provenance']['sha256'], sha(evidence.encode_evidence(provenance)))
        self.assertEqual(inputs['pet_mechanisms'].read_bytes(), before)
        inputs, approvals, mechanism, _, _ = self.reference('2', '9002')
        second, _, _ = self.build_reference(inputs, approvals)
        self.assertEqual(set(second['pets']), {'2'})
        self.assertEqual(second['pets']['2']['references'][0]['id'], '9002')
        self.assertNotEqual(first['pets']['1']['dictionarySourceSha256'], second['pets']['2']['dictionarySourceSha256'])

    def test_reference_new_resolved_skill_loses_old_missing_finding(self):
        inputs, approvals, mechanism, raw, _ = self.reference()
        mechanism['pets']['1']['skillLinks'][0]['resolved'] = True
        raw['flashSkillConfig']['s'] = [{'id': '9001'}]
        inputs['pet_mechanisms'].write_bytes(evidence.encode_evidence(mechanism))
        inputs['raw_skill_tables'].write_bytes(evidence.encode_evidence(raw))
        result, missing, _ = self.build_reference(inputs, approvals)
        self.assertEqual(result['pets'], {})
        self.assertEqual(missing['bySkillId'], {})

    def test_reference_contradictory_current_missing_skill_refuses_stale_claim(self):
        inputs, approvals, _, raw, _ = self.reference()
        raw['flashSkillConfig']['s'] = [{'id': '9001'}]
        inputs['raw_skill_tables'].write_bytes(evidence.encode_evidence(raw))
        with self.assertRaisesRegex(ValueError, 'specific fallback analysis'):
            self.build_reference(inputs, approvals)

    def test_reference_changed_consumer_or_package_rejected(self):
        inputs, approvals, _, _, packages = self.reference()
        inputs['skill_icon'].write_text('changed code', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Unreviewed consumer'):
            self.build_reference(inputs, approvals)
        inputs['skill_icon'].write_text(SYNTHETIC_REFERENCE_CONSUMERS['skill_icon'], encoding='utf-8')
        Path(packages[0]['file']).write_text('changed package', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Package provenance mismatch'):
            self.build_reference(inputs, approvals)

    def test_production_wrapper_builds_current_evidence_without_old_snapshots(self):
        inputs, approvals, mechanisms, raw, packages = self.reference()
        old_reference, _, _ = self.build_reference(inputs, approvals)
        # Deliberately broken old table locations: the wrapper must never open them.
        for key in ('exact-table-snapshot', 'mechanism-table-snapshot', 'table-snapshot-provenance'):
            old_reference['evidence'][key]['file'] = str(self.root / 'source-snapshots/DO-NOT-READ.json')
        race_inputs, race_approvals, race_packages = self.race()
        old_race = evidence.build_race_expansion_evidence(race_inputs, approved_sha256=race_approvals, packages=race_packages)
        source, auxiliary, verified = self.root / 'source', self.root / 'auxiliary', self.root / 'verified'
        all_packages = packages + list(race_packages.values())
        self.file('source/provenance.json', {'packages': packages[4:]})
        self.file('verified/flash-resolved/provenance.json', {'files': all_packages})
        self.file('auxiliary/pets.json', {'10': {'name': 'SYNTHETIC CURRENT PET'}})
        self.file('source/xml/mmo.battleconfig.data.CarryConfigXML__xmlClass.xml', race_inputs['carry_xml'].read_text(encoding='utf-8'))
        args = {'source': source, 'auxiliary': auxiliary, 'verified_source': verified,
                'mechanism_rules': self.file('rules.json', {}), 'legend_mechanics': self.file('legends.json', {}),
                'reference_consumers': self.file('old-reference.json', old_reference),
                'race_expansions': self.file('old-race.json', old_race)}
        approved = {str(inputs[key]): value for key, value in approvals.items()}
        approved.update({str(race_inputs[key]): value for key, value in race_approvals.items()})
        with mock.patch('extract_skill_tables.extract_skill_tables', return_value={'skills': {}, 'rawTables': raw}), \
             mock.patch('extract_pet_mechanisms.extract_pet_mechanisms', return_value=mechanisms) as pets:
            updated = evidence.prepare_reference_and_race(args, project_root=self.root,
                approved_sources=approved, generated_utc='2026-01-01T00:00:00Z')
        self.assertNotEqual(updated['reference_consumers'], args['reference_consumers'])
        self.assertEqual(args['reference_consumers'], self.root / 'old-reference.json')
        fresh = json.loads(updated['reference_consumers'].read_text(encoding='utf-8'))
        self.assertEqual(fresh['pets']['1']['references'][0]['id'], '9001')
        for key in ('exact-table-snapshot', 'mechanism-table-snapshot', 'table-snapshot-provenance'):
            record = fresh['evidence'][key]
            self.assertEqual(sha(Path(record['file']).read_bytes()), record['sha256'])
            self.assertTrue(Path(record['file']).is_relative_to(self.root / 'generated-evidence'))
        self.assertIsNone(pets.call_args.kwargs['consumer_evidence_path'])

    def test_input_size_limit_preserves_source_bytes(self):
        inputs, approvals = self.fortune()
        previous = {key: path.read_bytes() for key, path in inputs.items()}
        with mock.patch.object(evidence, 'MAX_INPUT_BYTES', 10), self.assertRaisesRegex(ValueError, 'size limit'):
            evidence.build_fortune_rules(inputs, approved_sha256=approvals)
        self.assertEqual(previous, {key: path.read_bytes() for key, path in inputs.items()})


if __name__ == '__main__':
    unittest.main()
