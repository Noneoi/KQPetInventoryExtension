from copy import deepcopy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/battle_replay'))
from audit_reference_coverage import audit_reference_coverage, STATUSES


def catalog_fixture():
    return {
        'petMechanics': {'1': {'name': '资料皮肤名', 'skillLinks': [
            {'field': 'norSkillNo', 'skillId': '100001', 'resolved': False}],
            'otherReferences': {}, 'mechanismFamily': '', 'qiYunType': '0',
            'source': {'file': 'official.as', 'sha256': 'recorded', 'createRow': 1}}},
        'pets': {'1': {'name': '资料皮肤名', 'identity': {
            'status': 'skin', 'canonicalName': '官方原名', 'baseRaceId': '2'}}},
        'skills': {'100001': {'name': '普通技能', 'description': '官方说明'}},
        'buffs': {'100001': {'nm': '独立 Buff', 'mess': '独立 Buff 说明'}},
        'legendMechanics': {'definitions': {}}, 'huanMechanics': {},
        'mechanismRules': {'qiYun': {'plans': {'1': {'name': '气运方案'}}},
                           'suLing': {'plans': {'11': {'name': '溯灵方案'}}}},
        'officialEntryMentions': {}, 'entryMentionsByBuff': {}, 'entryMentionsBySkill': {},
    }


class ReferenceCoverageTests(unittest.TestCase):
    def test_zero_gaps_are_recorded_and_old_resolved_flag_is_not_trusted(self):
        c = catalog_fixture()
        result = audit_reference_coverage(c)
        self.assertEqual(result['items'], [])
        self.assertEqual(result['summary']['byStatus'], dict.fromkeys(STATUSES, 0))
        self.assertEqual(result['summary']['referenceChecks']['petSkills']['resolvedReferences'], 1)
        self.assertEqual(result['summary']['referenceChecks']['legacyLegend']['unmatchedReferences'], 0)
        c['petMechanics']['1']['skillLinks'][0].update(skillId='999', resolved=True)
        item = audit_reference_coverage(c)['items'][0]
        self.assertEqual((item['id'], item['status']), ('999', 'missing-reference'))
        self.assertEqual(item['references'][0]['name'], '官方原名')

    def test_same_id_in_buffs_or_ordinary_skills_cannot_satisfy_legacy_legend(self):
        c = catalog_fixture()
        c['skills']['900001'] = {'name': '黄龙1', 'description': '同号普通技能说明'}
        c['buffs']['900001'] = {'nm': '同号 Buff', 'mess': '不属于旧传说'}
        c['petMechanics']['1']['otherReferences']['legendSkillNo'] = '900001'
        result = audit_reference_coverage(c)
        self.assertEqual([(x['category'], x['id']) for x in result['items']],
                         [('legend-reference', '900001')])
        c['legendMechanics']['definitions']['900001'] = {'name': '明确旧传说定义'}
        self.assertEqual(audit_reference_coverage(c)['items'], [])

    def test_huan_and_plans_keep_separate_namespaces_and_family(self):
        c = catalog_fixture()
        p = c['petMechanics']['1']
        p['otherReferences']['huanSkillNo'] = '100001'
        p.update(mechanismFamily='suLing', qiYunType='1')
        result = audit_reference_coverage(c)
        self.assertEqual({(x['category'], x['id']) for x in result['items']},
                         {('huan-reference', '100001'), ('mechanism-plan-reference', 'suLing:1')})
        p.update(mechanismFamily='', qiYunType='987')
        self.assertEqual(audit_reference_coverage(c)['summary']['referenceChecks']['mechanismPlans']['checkedReferences'], 0)
        p.update(mechanismFamily='qiYun', qiYunType='0')
        item = next(x for x in audit_reference_coverage(c)['items'] if x['category'] == 'mechanism-plan-reference')
        self.assertIn('原记录为 0', item['reasonLabel'])
        self.assertEqual(item['id'], 'qiYun:0')

    def test_elite_unclassified_candidate_is_neither_resolved_nor_legacy_missing(self):
        c = catalog_fixture()
        c['skills']['920104'] = {'name': '现有同号说明', 'description': '不能证明消费者'}
        p = c['petMechanics']['1']
        p.update(legendReferenceNamespace='unclassified', rawLegendSkillNo='920104',
                 legendReferenceCandidates=[{'namespace': 'skills', 'id': '920104', 'status': 'unverifiedNamespace'}])
        p['otherReferences']['unclassifiedLegendSkillNo'] = '920104'
        r = audit_reference_coverage(c)
        self.assertEqual(r['summary']['byStatus']['missing-reference'], 0)
        self.assertEqual(r['summary']['referenceChecks']['legacyLegend']['checkedReferences'], 0)
        self.assertEqual(r['summary']['unclassifiedReferences']['count'], 1)
        self.assertEqual(r['items'][0]['status'], 'reference-unclassified')
        self.assertEqual(r['items'][0]['references'][0]['candidates'][0]['status'], 'unverifiedNamespace')

    def test_confirmed_elite_in_skill_links_is_checked_only_as_battle_skill(self):
        c = catalog_fixture()
        c['skills']['920104'] = {'name': '有消费者的技能', 'description': '明确资料'}
        c['petMechanics']['1']['skillLinks'].append({'field': 'legendSkillNo', 'skillId': '920104'})
        result = audit_reference_coverage(c)
        self.assertEqual(result['items'], [])
        self.assertEqual(result['summary']['referenceChecks']['petSkills']['resolvedReferences'], 2)
        self.assertEqual(result['summary']['referenceChecks']['legacyLegend']['checkedReferences'], 0)

    def test_all_missing_reference_owners_are_retained_without_truncation(self):
        c = catalog_fixture()
        template = c['petMechanics']['1']
        c['petMechanics'] = {}
        for n in range(1205):
            p = deepcopy(template)
            p['skillLinks'][0]['skillId'] = '999'
            c['petMechanics'][str(n)] = p
        r = audit_reference_coverage(c)
        self.assertFalse(r['summary']['truncated'])
        self.assertEqual(len(r['items']), 1)
        self.assertEqual(r['items'][0]['referenceCount'], 1205)
        self.assertEqual(len(r['items'][0]['references']), 1205)
        self.assertEqual(r['summary']['referenceChecks']['petSkills']['unmatchedTargets'], 1)

    def test_missing_text_is_not_missing_definition_and_buff_notes_do_not_fill_mess(self):
        c = catalog_fixture()
        c['skills']['100001']['name'] = ' '
        c['buffs']['100001'].update(nm='', mess='', beffect='官方效果字样', bhs='官方伤害来源')
        r = audit_reference_coverage(c)
        self.assertEqual(r['summary']['byStatus']['missing-reference'], 0)
        self.assertEqual(r['summary']['byStatus']['official-text-empty'], 2)
        buff = next(x for x in r['items'] if x['category'] == 'buff-text')
        self.assertEqual(buff['evidence']['emptyFields'], ['nm', 'mess'])
        self.assertTrue(buff['evidence']['definitionPresent'])
        self.assertEqual(buff['evidence']['separateSupplementalFields']['beffect'], '官方效果字样')

    def test_unresolved_identity_does_not_use_skin_name_as_canonical(self):
        c = catalog_fixture()
        c['pets']['1']['identity'].update(status='unresolved', canonicalName='', reason='missingExactSkinMapping')
        item = audit_reference_coverage(c)['items'][0]
        self.assertEqual(item['status'], 'identity-unresolved')
        self.assertEqual(item['name'], '原名待核实 [1]')
        self.assertEqual(item['evidence']['catalogDisplayName'], '资料皮肤名')

    def test_unmatched_terms_keep_all_owners_and_are_not_missing_mechanisms(self):
        c = catalog_fixture()
        c['officialEntryMentions'] = {
            '复生Ⅱ': {'displayLabel': '复生Ⅱ', 'lookupName': '复生Ⅱ', 'status': 'missing', 'description': ''},
            '已匹配': {'status': 'resolved', 'description': '官方解释'},
            '参数·ARG': {'status': 'unsupported_arguments', 'diagnostic': 'unsupported replacement'},
        }
        c['entryMentionsByBuff'] = {'100001': ['复生Ⅱ']}
        c['entryMentionsBySkill'] = {'100001': ['复生Ⅱ']}
        result = audit_reference_coverage(c)
        self.assertEqual(result['summary']['byStatus']['term-unmatched'], 2)
        self.assertEqual(result['summary']['byStatus']['missing-reference'], 0)
        item = next(x for x in result['items'] if x['id'] == '复生Ⅱ')
        self.assertEqual({x['namespace'] for x in item['references']}, {'skills', 'buffs'})
        self.assertIn('不等于缺失战斗机制', item['reasonLabel'])

    def test_does_not_mutate_input_or_inject_placeholder_definitions(self):
        c = catalog_fixture()
        c['petMechanics']['1']['skillLinks'][0]['skillId'] = '999'
        before = json.dumps(c, ensure_ascii=False, sort_keys=True)
        result = audit_reference_coverage(c)
        self.assertEqual(json.dumps(c, ensure_ascii=False, sort_keys=True), before)
        self.assertNotIn('999', c['skills'])
        result['items'][0]['references'][0]['source']['file'] = 'changed-only-in-report'
        self.assertEqual(c['petMechanics']['1']['source']['file'], 'official.as')

    def test_consumer_fallback_is_evidence_not_a_missing_definition_replacement(self):
        c = catalog_fixture()
        pet = c['petMechanics']['1']
        pet['skillLinks'][0]['skillId'] = '999'
        pet['referenceConsumption'] = [
            {'field': 'norSkillNo', 'id': '999', 'status': 'emptyDescriptionWrapper',
             'summary': '官方读取器返回空描述包装，未获得实际定义', 'evidenceIds': ['consumer-1']},
            {'field': 'ultSkillNo', 'id': '999', 'status': 'other-branch'},
            {'field': 'norSkillNo', 'id': '100001', 'status': 'other-id'},
        ]
        result = audit_reference_coverage(c)
        item = result['items'][0]
        self.assertEqual(item['status'], 'missing-reference')
        readings = item['references'][0]['referenceConsumption']
        self.assertEqual(len(readings), 1)
        self.assertEqual(readings[0]['status'], 'emptyDescriptionWrapper')
        self.assertEqual(result['summary']['referenceChecks']['petSkills']['unmatchedReferences'], 1)
        readings[0]['evidenceIds'].append('untrusted-change')
        self.assertEqual(pet['referenceConsumption'][0]['evidenceIds'], ['consumer-1'])

    def test_elite_and_plan_consumer_evidence_keeps_original_namespace_unknown(self):
        c = catalog_fixture()
        pet = c['petMechanics']['1']
        pet['otherReferences']['unclassifiedLegendSkillNo'] = '920104'
        pet.update(mechanismFamily='qiYun', qiYunType='0')
        pet['referenceConsumption'] = [
            {'field': 'legendSkillNo', 'id': '920104', 'status': 'conditional'},
            {'field': 'qiYunType', 'id': '0', 'status': 'nullNoFallback'},
        ]
        result = audit_reference_coverage(c)
        self.assertEqual(result['summary']['byStatus']['reference-unclassified'], 1)
        self.assertEqual(result['summary']['byStatus']['missing-reference'], 1)
        for item in result['items']:
            self.assertEqual(len(item['references'][0]['referenceConsumption']), 1)

    def test_absent_input_sections_are_not_claimed_as_audited_tables(self):
        r = audit_reference_coverage({})
        self.assertEqual(r['items'], [])
        self.assertFalse(any(r['summary']['inputSectionsPresent'].values()))
        self.assertEqual(r['summary']['textChecks']['skills']['definitionsChecked'], 0)

    def test_invalid_shape_fails_instead_of_reporting_zero_gaps(self):
        with self.assertRaisesRegex(ValueError, 'must be an object'):
            audit_reference_coverage({'skills': []})
        c = catalog_fixture()
        c['petMechanics']['1']['skillLinks'][0].pop('skillId')
        with self.assertRaisesRegex(ValueError, 'explicit reference ID'):
            audit_reference_coverage(c)


if __name__ == '__main__':
    unittest.main()
