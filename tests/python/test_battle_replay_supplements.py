import json
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/battle_replay'))
from extract_replay_supplements import (StaticLiteralParser, declaration, xml_records,
    parse_summons, parse_carries, parse_almighty, parse_formation_skills,
    parse_formation_teams, parse_power_full, parse_resources, parse_titles,
    link_supplements, extract_replay_supplements, apply_display_labels,
    add_hero_description_readings, parse_version_rules, verify_race_expansions,
    attach_title_resource_verification)


EVIDENCE = {'file': 'official-fixture.as', 'sha256': 'fixture'}


def xml(value):
    return xml_records(value, EVIDENCE)


class SupplementTablesTest(unittest.TestCase):
    def test_summon_hidden_table_overrides_exact_pair_without_dropping_rows(self):
        main = xml('<config><pet>\n<s s="01#2#" sed="3#4" effect="70" sdes="主表"/>\n'
                   '<s s="1" sed="3" effect="71"/></pet></config>')
        hidden = xml('<config><pet><s s="1" sed="3" effect="72" sdes="隐藏表"/></pet></config>')
        result = parse_summons(main, hidden)
        self.assertEqual(result['byPair'], {'1_3': 2, '1_4': 0, '2_3': 0, '2_4': 0})
        self.assertEqual(result['historyByPair']['1_3'], [0, 1, 2])
        self.assertEqual(result['rows'][2]['effectId'], '72')
        self.assertEqual(result['rows'][0]['source']['line'], 2)
        self.assertEqual(result['rows'][0]['rawAttributes']['s'], '01#2#')
        self.assertEqual(result['rows'][2]['descriptions']['skillDescription'], '隐藏表')
        self.assertEqual(result['hiddenSourceRaceIds'], ['1'])

    def test_carry_hash_tokens_are_not_actual_expansion(self):
        records = xml('<config><pet><s c="10#11" ced="20#21" type="4" sdes="官方携同说明"/></pet></config>')
        unresolved = parse_carries(records)
        self.assertEqual(unresolved['byPair'], {})
        self.assertEqual(unresolved['byDeclaredSourceRaceId'], {'10': [0]})
        self.assertEqual(unresolved['rows'][0]['expansionStatus'], 'unresolved')
        expanded = parse_carries(records, {'byRaceId': {'10': ['10', '12'], '20': ['22']},
                                           'sourceEvidence': [EVIDENCE]})
        self.assertEqual(expanded['byPair'], {'10_22': 0, '12_22': 0})
        self.assertNotIn('11_21', expanded['byPair'])
        self.assertEqual(expanded['rows'][0]['name'], '')
        self.assertEqual(expanded['rows'][0]['description'], '官方携同说明')

    def test_legitimate_empty_expansion_does_not_invent_seed_pair(self):
        records = xml('<config><pet><s c="10" ced="20" type="8"/></pet></config>')
        result = parse_carries(records, {'byRaceId': {'10': [], '20': ['20']}, 'sourceEvidence': [EVIDENCE]})
        self.assertEqual(result['rows'][0]['expansionStatus'], 'resolved')
        self.assertEqual(result['byPair'], {})
        self.assertEqual(result['unresolvedExpansion'], [])
        with self.assertRaisesRegex(ValueError, 'source evidence'):
            parse_carries(records, {'byRaceId': {'10': ['10']}})
        with self.assertRaisesRegex(ValueError, 'arrays'):
            parse_carries(records, {'byRaceId': {'10': '10'}, 'sourceEvidence': [EVIDENCE]})

    def test_carry_later_row_wins_after_shared_official_expansion(self):
        records = xml('<config><pet><s c="10" ced="20" type="4" sdes="早"/>'
                      '<s c="11" ced="20" type="4" sdes="晚"/></pet></config>')
        result = parse_carries(records, {'byRaceId': {'10': ['10', '11'], '11': ['10', '11'], '20': ['20']},
                                         'sourceEvidence': [EVIDENCE]})
        self.assertEqual(result['byPair']['10_20'], 1)
        self.assertEqual(result['historyByPair']['10_20'], [0, 1])

    def test_almighty_first_definition_wins_and_no_name_based_skill_join(self):
        records = xml('<config><pet><s id="7" hn="英雄技同名" hd="第一条" psci="8"/>'
                      '<s id="7" hn="第二条名" hd="第二条" psci="9"/></pet></config>')
        result = parse_almighty(records)
        self.assertEqual(result['byRaceId'], {'7': 0})
        self.assertEqual(result['historyByRaceId'], {'7': [0, 1]})
        self.assertEqual(result['rows'][0]['psychicsCalledPetRaceId'], '8')
        self.assertNotIn('heroSkillId', result['rows'][0])
        self.assertEqual(result['rows'][0]['descriptions']['heroSkillDesc'], '第一条')

    def test_hero_duplicate_conditions_keep_order_and_exact_parameters(self):
        text = ('private static const defines:Array = [\n'
                'new HeroSkillDefine(301,125,0.1,1,"LogicExpression","A&&B",1,"RaceTypeCreature","23#3","1",2),\n'
                'new HeroSkillDefine(301,0,0,1,"AllEffected","",0)];')
        result = parse_formation_skills(text, EVIDENCE, 'hero', {'301': {'name': '确切技能', 'description': '说明'}})
        self.assertEqual(result['bySkillId'], {'301': [0, 1]})
        self.assertEqual(result['rows'][0]['clsParams'], 'A&&B')
        self.assertEqual(result['rows'][0]['mutilIndex'], 2)
        self.assertEqual(result['rows'][1]['preCls'], None)
        self.assertFalse(result['rows'][1]['isShow'])
        self.assertEqual(result['rows'][0]['source']['line'], 2)
        self.assertEqual(result['rows'][0]['name'], '确切技能')

    def test_psychics_parameter_eleven_is_called_pet_not_hero_index(self):
        text = 'const defines:Array=[new PsychicsSkillDefine(801,0,0,0,"AllEffected","",0,"","","",99)];'
        result = parse_formation_skills(text, EVIDENCE, 'psychics')
        self.assertEqual(result['rows'][0]['psychicsCalledPetRaceId'], '99')
        self.assertNotIn('mutilIndex', result['rows'][0])
        self.assertEqual(result['rows'][0]['name'], '')

    def test_formation_team_conditions_stay_separate_from_battle_buff_ids(self):
        text = ('const buffs:Object={1:new FormationTeamBuff(1,"组合原名",[[7,8],[9]],'
                '[new FormationTeamBuffEffect(2,["第一档","第二档"])],'
                '[new FormationTeamBuffFormationEffect(4,2,"阵型说明")],1)};')
        group = parse_formation_teams(text, EVIDENCE)
        self.assertEqual(group['byId'], {'1': 0})
        self.assertEqual(group['rows'][0]['petAlternatives'], [['7', '8'], ['9']])
        self.assertEqual(group['rows'][0]['effects'][0]['effect'], ['第一档', '第二档'])
        self.assertEqual(group['rows'][0]['formationEffects'][0]['formationId'], 4)
        result = {key: {'rows': []} for key in ['summon', 'carry', 'almighty', 'hero', 'psychics', 'powerFull']}
        result['carry']['unresolvedExpansion'] = []
        result['formationTeams'] = group
        link_supplements(result, {}, {'1': {'nm': '恰好同号'}}, {'7': {}, '8': {}})
        self.assertTrue(all(row['targetTable'] == 'pets' for row in result['relations']))
        self.assertEqual([row['targetId'] for row in result['unresolved']], ['9'])

    def test_static_parser_rejects_executable_or_unknown_expressions(self):
        for text in ['const defines:Array=[load("url")];',
                     'const defines:Array=[new OtherClass(1)];',
                     'const defines:Array=[1+2];']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                declaration(text, 'defines', ['HeroSkillDefine'])
        with self.assertRaisesRegex(ValueError, 'declarations'):
            xml('<!DOCTYPE x [<!ENTITY y "large">]><config/>')

    def test_full_power_race_override_precedes_dictionary_then_default(self):
        records = xml('<config><pfmap><m raceId="1" buffId="10001"/>'
                      '<m raceId="1" buffId="10003"/><def buffId="10000"/></pfmap></config>')
        getter = 'private static const SUPER_JOB_BUFF:int=10005; if(this._ultSuperSkillNo>0){return SUPER_JOB_BUFF;}return -1;'
        pets = {'1': {'ultSuperSkillNo': 200}, '2': {'ultSuperSkillNo': 200}, '3': {'ultSuperSkillNo': 0}}
        result = parse_power_full(records, pets, getter, EVIDENCE)
        self.assertEqual([row['buffId'] for row in result['rows']], ['10003', '10005', '10000'])
        self.assertEqual([row['branch'] for row in result['rows']], ['explicit-race-map', 'pet-dictionary', 'default'])
        self.assertEqual(result['rows'][0]['dictionaryFullPowerBuffId'], '10005')
        self.assertEqual(len(result['rawMappingRows']), 2)
        with self.assertRaisesRegex(ValueError, 'schema changed'):
            parse_power_full(records, pets, getter.replace('>0', '>1'), EVIDENCE)

    def test_resources_preserve_unread_change_rows_and_excluded_reward_assets(self):
        result = parse_resources({
            'changeAvatar': xml('<config><change><s id="9"/></change></config>'),
            'preDownload': xml('<config><common><f file="battle/gainexp_res"/><f file="battle/ui_res"/></common>'
                               '<seq><f file="battle/ui_res"/></seq></config>'),
            'background': xml('<config><gaincoolpetstone><s itemid="1" name="reward"/></gaincoolpetstone>'
                              '<background><s scene="default"/></background></config>')})
        self.assertEqual(result['changeAvatar']['byId'], {})
        self.assertEqual(result['changeAvatar']['rawRowCount'], 1)
        self.assertEqual(result['changeAvatar']['runtimeEligibleRows'], 0)
        self.assertEqual([row['runtimeEligible'] for row in result['preDownload']['rows']], [False, True, False])
        self.assertEqual(result['background']['rows'][0]['rawAttributes']['name'], 'reward')
        self.assertFalse(result['background']['rows'][0]['runtimeEligible'])

    def test_title_demo_ids_do_not_supply_unproven_names_or_owners(self):
        data = ('if(params["title"] is Array){} petTitleArr=[params["title"]];'
                'var _loc3_:int=int(param1["titleId"]); data._titleIdMap[_loc3_]=param1;'
                'this._titleIdMap[param1]["content"];this._titleIdMap[param1]["extId"];')
        service = ('var movieUrl:String="pettitle/movie/pettitlemovie"+titleId;'
                   'var movieCls:String="mmo.pettitle.movie.TitleMovie"+titleId;')
        result = parse_titles('const BATTLE_DEMOS:Object={"1":null,"2":{"br":{"titleId":2}}};',
                              EVIDENCE, data, service)
        self.assertEqual(result['byId'], {'1': 0, '2': 1})
        self.assertFalse(result['rows'][0]['demoAvailable'])
        self.assertTrue(result['rows'][1]['demoAvailable'])
        self.assertEqual(result['rows'][1]['name'], '')
        self.assertNotIn('raceId', result['rows'][1])
        self.assertEqual(result['definitionStatus'], 'unavailable')
        self.assertEqual(result['rows'][1]['resource'], 'pettitle/movie/pettitlemovie2')

    def test_generated_labels_use_verified_original_names_without_inventing_skill_names(self):
        row = {'rowId': 0, 'sourceRaceIds': ['10'], 'targetRaceIds': ['20']}
        result = {'summon': {'rows': [row]}, 'carry': {'rows': []}, 'almighty': {'rows': []},
                  'powerFull': {'rows': [{'raceId': '10', 'readings': []}]},
                  'titles': {'rows': [{'titleId': '1'}]}, 'resources': {}}
        pets = {'10': {'name': '皮肤显示名', 'identity': {'canonicalName': '真实原名'}},
                '20': {'name': '未核实皮肤名'}}
        apply_display_labels(result, pets)
        self.assertEqual(row['name'], '真实原名 → 精灵 [20] · 配置配对')
        self.assertTrue(row['generatedLabel'])
        self.assertEqual(row['officialName'], '')
        self.assertNotIn('skillId', row)
        self.assertEqual(result['titles']['rows'][0]['name'], '称号 [1] · 表现资料')
        self.assertIn('不是战报附加状态', result['powerFull']['rows'][0]['readings'][0]['value'])

    def test_hero_description_uses_exact_delimiter_and_show_index_without_activation(self):
        source = ('const defines:Array=['
                  'new HeroSkillDefine(1,0,0,0,"RaceEffected","10,99",1,"","","",2),'
                  'new HeroSkillDefine(1,0,0,0,"AllEffected","",1,"","","",3),'
                  'new HeroSkillDefine(2,0,0,0,"AllEffected","",1,"","","",2)];')
        group = parse_formation_skills(source, EVIDENCE, 'hero', {
            '1': {'description': '2^效果甲@触发@\n@\n3^效果乙@触发@'},
            '2': {'description': '2^效果甲@触发@<br>@<br>3^效果乙@触发@'}})
        reader = ('public function getHeroSkillTips(){}'
                  'descriptionArr=skillDefine.description.split("\\n@\\n");'
                  'var _loc3_:* = param1.split("^");map[param1.mutilIndex]=param1;'
                  'description += desc.replace("@触发@","");')
        add_hero_description_readings(group, reader, EVIDENCE, {'byRaceId': {'10': ['10', '11']}})
        self.assertEqual(group['rows'][0]['conditionDescriptions'], ['效果甲'])
        self.assertEqual(group['rows'][1]['conditionDescriptions'], ['效果乙'])
        self.assertEqual(group['rows'][2]['conditionDescriptions'], [])
        self.assertEqual(group['rows'][2]['conditionDescriptionStatus'], 'unmatched-official-delimiter')
        self.assertEqual(group['rows'][0]['effectiveRaceIds'], ['10', '11'])
        self.assertEqual(group['rows'][0]['clsParams'], '10,99')
        self.assertNotIn('activated', group['rows'][0])

    def test_version_rules_keep_aqz_global_version_separate_from_swf_default(self):
        text = ('DEFAULT_VERSION:String="2000"; const EXTENSION_NAMES:Array=["swf","png"];'
                'for each(_loc4_ in param3.f){} versionMap[_loc4_.@n]=_loc4_.@v;'
                'public static function setVersionXml(){}setTempVersion(param2);'
                'public static function setTempVersion(){}for each(_loc2_ in param1.u.f){}'
                'versionMap[_loc2_.@n]=_loc2_.@v;private static function getVersion(){}'
                'if(version.length==0){return "";}if(EXTENSION_NAMES.indexOf(param2)!=-1){}'
                'return _loc3_+DEFAULT_VERSION;return "~"+version;'
                'public static function getVerURLRequest(){}'
                'var _loc4_:String=getVersion(param1,param2);param1=testFileMap[param1];'
                '_loc5_=param1+_loc4_+"."+param2;')
        loader = ('this.version=this.versionXML.v;new URLRequest("versiondata~"+this.version+".swf");'
                  'var _loc4_:XML=new XML(_loc3_["vconfig"]);'
                  'VersManager.setVersionXml(this.version,this.versionXML,_loc4_);'
                  'private function onLoadBytesSuccess(){}')
        result = parse_version_rules(text, EVIDENCE, loader, EVIDENCE)
        self.assertEqual(result['aqzVersionRule'], 'start.v')
        self.assertEqual(result['defaultMappedVersion'], '2000')
        self.assertNotIn('aqz', result['versionMapExtensions'])
        self.assertTrue(result['versionMapOrder'][1].startswith('start.xml.u.f'))
        with self.assertRaisesRegex(ValueError, 'schema changed'):
            parse_version_rules(text.replace('2000', '9999'), EVIDENCE, loader, EVIDENCE)

    def test_carry_expansion_hash_mismatch_is_rejected_before_pairing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'PetRaceUtil.as'
            path.write_text('changed source', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                verify_race_expansions({'byRaceId': {'1': ['2']},
                                        'sourceEvidence': [{'file': str(path), 'sha256': 'old'}]})

    def test_title_resource_manifest_requires_actual_matching_package_and_export_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'title1.swf'
            path.write_bytes(b'fixture package bytes')
            exported = Path(directory) / 'TitleMovie1.as'
            exported.write_bytes(b'fixture empty class')
            source = {'file': str(exported), 'sha256': hashlib.sha256(exported.read_bytes()).hexdigest()}
            proof = {'byTitleId': {'1': {'resource': 'title1', 'className': 'Title1', 'file': str(path),
                       'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size,
                       'status': 'downloaded', 'metadataStatus': 'noSemanticDefinitionInInspectedExports',
                       'sourceEvidence': [source]}}}
            group = {'rows': [{'titleId': '1', 'resource': 'title1', 'resourceClass': 'Title1', 'readings': []}], 'byId': {'1': 0}}
            attach_title_resource_verification(group, proof, EVIDENCE)
            self.assertEqual(group['verifiedResourceCount'], 1)
            self.assertIn('当前可读导出未取得', group['rows'][0]['readings'][0]['value'])
            path.write_bytes(b'changed package')
            attach_title_resource_verification(group, proof, EVIDENCE)
            self.assertEqual(group['verifiedResourceCount'], 0)
            self.assertEqual(group['rows'][0]['resourceVerification']['verificationStatus'], 'failed')
            self.assertTrue(group['resourceVerificationIssues'])
            attach_title_resource_verification(group, {'byTitleId': {}}, EVIDENCE)
            self.assertEqual(group['rows'][0]['resourceVerification']['verificationStatus'], 'missing')


class OfficialSnapshotIntegrationTest(unittest.TestCase):
    def test_retained_snapshot_rows_indexes_and_namespace_boundaries(self):
        source = ROOT / 'build-battle-reference/latest-tables'
        formation = ROOT / 'build-battle-reference/border-official'
        if not source.exists() or not formation.exists():
            self.skipTest('Official retained snapshot is not part of the portable test checkout')
        catalog = json.loads((ROOT / 'assets/battle-replay-catalog.json').read_text(encoding='utf-8-sig'))
        result = extract_replay_supplements(source, ROOT / 'build-battle-reference', formation_source=formation,
                                           skills=catalog['skills'], buffs=catalog['buffs'], pets=catalog['pets'])
        self.assertEqual(result['summary']['summonRows'], 1956)
        self.assertEqual(result['summary']['summonPairs'], 19655)
        self.assertEqual(result['summary']['carryRows'], 83)
        self.assertEqual(result['summary']['almightyRows'], 28)
        self.assertEqual(result['summary']['heroRows'], 1143)
        self.assertEqual(result['summary']['psychicsRows'], 732)
        self.assertEqual(result['summary']['formationTeamsRows'], 36)
        self.assertEqual(len(result['powerFull']['raceIdOverrides']), 441)
        self.assertEqual(result['resources']['changeAvatar']['runtimeEligibleRows'], 0)
        self.assertEqual(result['resources']['avatar']['rawRowCount'], 432)
        self.assertEqual(result['carry']['byPair'], {})
        self.assertTrue(all(row['generatedLabel'] and row['officialName'] == '' for row in result['almighty']['rows']))
        self.assertEqual(result['powerFull']['lookupKey'], 'FighterInfo.faceId')
        self.assertEqual(result['powerFull']['consumerEvidence'][0]['line'], 447)
        loading = result['resources']['loading']
        self.assertEqual(loading['rawRowCount'], 20)
        row = lambda key: loading['rows'][loading['byId'][key]]
        self.assertEqual(row('buff-swf')['template'], 'battlebuffer/buffer{displayId}')
        self.assertNotIn('buffId}', row('buff-swf')['template'])
        self.assertIn('faceId', row('full-power')['keyField'])
        self.assertFalse(row('equipped-background')['runtimeEligible'])
        self.assertEqual(row('action-aqz')['template'], 'battleaction/action{faceId}.aqz')
        self.assertTrue(all(row['source']['sha256'] for group in ['hero', 'psychics', 'summon', 'carry']
                            for row in result[group]['rows']))


if __name__ == '__main__':
    unittest.main()
