import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/battle_replay'))
from extract_replay_cultivation import (
    APPLICABILITY, _verified_file, _xml_rows, compare_h5,
    extract_replay_cultivation, parse_cultivation_table,
    parse_property_types, property_readings, verify_consumers,
)


SOURCE = {'file': 'official-fixture.xml', 'sha256': 'fixture', 'resource': 'official-fixture'}
STAR = '''<config>
<s defineId="1" name="小碎片" pinYinChain="xsp_xiaosuipian" supplyExp="0" />
<s defineId="81" name="乘胜追击" pinYinChain="cszj" supplyExp="30">
<l level="1" exp="0" battlePower="20" battleProperty="null" desc="每次触发后的条件原文{10}" />
<l level="2" exp="120" battlePower="30" battleProperty="暴击率+0.15" desc="第二等级说明，不代表本场装备" />
</s></config>'''
BADGE = '''<config>
<s defineId="1" name="神速·命闪" type="0">
<l name="1级神速·命闪" level="1" effectDesc="命中率+3%，闪避率+1%，每次闪避后伤害提升2%" propsStr="" battlePower="120" materialStrToUpgrade="" />
</s>
<s defineId="300113" name="专属资料" type="1" effectDesc="激活后条件原文" propsStr="速度+20" battlePower="300" materialStrToActivate="4:1384:20" />
</config>'''
WHEEL = '''<config>
<s defineId="0" iconId="" name="星迹·核心" isTBD="0" allSigns="基础" locatedTypeId="0" exclusive="0" basicBuff="全属性率+0.15" twinkleBuff="" lightUpCost="" battlePower="700" twinkleProps="" />
<s defineId="38" iconId="2" name="条件星迹" isTBD="1" allSigns=",攻击" locatedTypeId="2" exclusive="1" basicBuff="全属性率+0.02" twinkleBuff="受到伤害时的官方条件" lightUpCost="8:33:4000#4:2078:100" battlePower="190" twinkleProps="全属性率+-0.2#速度+-60" />
</config>'''
PROPERTY_SOURCE = '''
public static const CRITICAL:PetBattlePropertyType = PetBattlePropertyType.create(60,"暴击率");
public static const SPEED:PetBattlePropertyType = PetBattlePropertyType.create(37,"速度");
public static const EQUIP_SPEED:PetBattlePropertyType = PetBattlePropertyType.create(1208,"速度装备");
public static const ALL:PetBattlePropertyType = PetBattlePropertyType.createContainer(204,"全属性率",[SPEED]);
'''


class CultivationReferenceTests(unittest.TestCase):
    def setUp(self):
        self.props = parse_property_types(PROPERTY_SOURCE, SOURCE)

    def parse(self, kind, xml):
        return parse_cultivation_table(kind, xml, SOURCE, property_types=self.props)

    def test_preserves_every_level_and_official_condition_without_selecting_one(self):
        rows = self.parse('stargods', STAR)
        self.assertEqual(set(rows), {'1', '81'})
        self.assertEqual(rows['1']['levels'], {})
        star = rows['81']
        self.assertEqual(list(star['levels']), ['1', '2'])
        self.assertEqual(star['levels']['1']['raw']['battleProperty'], 'null')
        self.assertEqual(star['levels']['1']['description'], '每次触发后的条件原文{10}')
        self.assertIn('2级：第二等级说明', star['description'])
        self.assertEqual(star['levels']['2']['source']['path'], '/config/s[2]/l[2]')
        self.assertNotIn('active', star)
        self.assertNotIn('equipped', star)
        self.assertEqual(star['levels']['2']['propertyReadings'][0]['propertyId'], '60')

    def test_badge_type_namespace_and_activation_are_not_buff_mapping(self):
        rows = self.parse('badges', BADGE)
        self.assertEqual(rows['1']['type'], 'job')
        self.assertEqual(rows['300113']['type'], 'exclusive')
        self.assertEqual(rows['300113']['raw']['materialStrToActivate'], '4:1384:20')
        self.assertEqual(rows['300113']['propertyReadings'][0]['condition'], 'exclusive-activated')
        self.assertEqual(rows['300113']['propertyReadings'][0]['terms'][0]['lookupName'], '速度装备')
        self.assertEqual(rows['300113']['references'], [])
        self.assertNotIn('buffId', json.dumps(rows, ensure_ascii=False))
        self.assertTrue(any(r['label'] == '元魂类型' and r['value'] == '专属元魂'
                            for r in rows['300113']['readings']))

    def test_wheel_buff_named_fields_keep_property_and_description_namespaces(self):
        rows = self.parse('astrolabe', WHEEL)
        self.assertIn('0', rows)  # defineId 0 is the actual core, not an empty sentinel.
        row = rows['38']
        self.assertEqual(row['description'], '受到伤害时的官方条件')
        self.assertEqual(row['propertyReadings'][0]['field'], 'basicBuff')
        self.assertEqual(row['propertyReadings'][0]['terms'][0]['propertyId'], '204')
        self.assertEqual(row['propertyReadings'][1]['condition'], 'lighted-and-selected')
        self.assertEqual(row['propertyReadings'][1]['terms'][1]['operator'], '-')
        self.assertEqual(row['propertyReadings'][1]['terms'][1]['value'], '60')
        self.assertEqual(row['propertyReadings'][1]['terms'][1]['raw'], '速度+-60')
        self.assertEqual(row['references'][0]['namespace'], 'astrolabe-icon')
        self.assertEqual(rows['0']['references'][0]['id'], '0')

    def test_unmatched_property_keeps_raw_and_does_not_guess(self):
        rows = property_readings('未知属性+3#300113#生命率+0.5', property_types=self.props)
        self.assertEqual([r['status'] for r in rows],
                         ['property-name-not-resolved', 'unparsed-property-token', 'property-name-not-resolved'])
        self.assertEqual(rows[1]['raw'], '300113')
        self.assertNotIn('propertyId', rows[1])
        self.assertNotIn('buffId', json.dumps(rows))

    def test_description_only_mechanism_is_not_dropped_when_properties_are_empty(self):
        row = self.parse('badges', BADGE)['1']['levels']['1']
        self.assertEqual(row['propertyReadings'], [])
        self.assertIn('每次闪避后', row['description'])

    def test_duplicate_and_nonsequential_rows_fail_closed(self):
        with self.assertRaisesRegex(ValueError, 'duplicate defineId'):
            self.parse('stargods', STAR.replace('defineId="81"', 'defineId="1"'))
        with self.assertRaisesRegex(ValueError, 'non-sequential'):
            self.parse('stargods', STAR.replace('level="2"', 'level="3"'))
        with self.assertRaisesRegex(ValueError, 'unknown official badge type'):
            self.parse('badges', BADGE.replace('type="1"', 'type="7"'))
        with self.assertRaisesRegex(ValueError, 'missing fields'):
            self.parse('astrolabe', WHEEL.replace(' twinkleProps=""', ''))

    def test_full_flash_h5_comparison_is_by_id_not_file_order(self):
        rows = _xml_rows(STAR)
        mirrored = list(reversed(rows))
        self.assertEqual(compare_h5(rows, mirrored, 'star')['definitionsCompared'], 2)
        changed = json.loads(json.dumps(mirrored))
        changed[0]['l'][1]['desc'] = '不同描述'
        with self.assertRaisesRegex(ValueError, 'Flash/H5 mismatch'):
            compare_h5(rows, changed, 'star')
        changed = json.loads(json.dumps(mirrored))
        changed[0]['l'].reverse()
        with self.assertRaisesRegex(ValueError, 'Flash/H5 mismatch'):
            compare_h5(rows, changed, 'star')

    def test_hash_changed_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'source.xml'
            path.write_bytes(STAR.encode('utf-8'))
            record = {'file': 'source.xml', 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            self.assertEqual(_verified_file(Path(tmp), record)[1], STAR.encode('utf-8'))
            path.write_text(STAR + ' ', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'source hash mismatch'):
                _verified_file(Path(tmp), record)

    def test_missing_consumer_is_not_treated_as_verified(self):
        with self.assertRaisesRegex(ValueError, 'unsupported cultivation consumer'):
            verify_consumers({})

    def test_missing_equipment_never_implies_unequipped_or_active(self):
        self.assertEqual(APPLICABILITY['status'], 'reference-only-until-equipment-snapshot')
        self.assertIn('专属激活状态', APPLICABILITY['requiredInputs']['badges'])
        self.assertIn('选中状态', APPLICABILITY['requiredInputs']['astrolabe'])
        self.assertEqual(APPLICABILITY['battleBuffBridge'], 'not-established')


AUDIT = ROOT / 'build-battle-reference/effect-origin-audit-20261009'
H5 = ROOT / 'build-battle-reference/cross-resource-audit-20261009/h5-all-config'


@unittest.skipUnless((AUDIT / 'flash-resolved/provenance.json').exists(),
                     'official downloaded source bundle is not part of the test checkout')
class CultivationOfficialBundleTests(unittest.TestCase):
    def test_verified_bundle_counts_and_separate_id_namespaces(self):
        data = extract_replay_cultivation(AUDIT, H5)
        self.assertEqual(data['summary']['groups']['stargods']['definitions'], 89)
        self.assertEqual(data['summary']['groups']['stargods']['levels'], 632)
        self.assertEqual(data['summary']['groups']['badges']['definitions'], 652)
        self.assertEqual(data['summary']['groups']['badges']['levels'], 201)
        self.assertEqual(data['summary']['groups']['astrolabe']['definitions'], 490)
        self.assertEqual(data['summary']['battleBuffLinks'], 0)
        self.assertEqual(data['summary']['unmatchedCount'], 0)
        self.assertTrue(all(r['status'] == 'all-fields-match'
                            for r in data['verification']['flashH5'].values()))
        self.assertTrue(all(data['sources'][key]['flash']['payloadVerified']
                            for key in ('stargods', 'badges', 'astrolabe')))


if __name__ == '__main__':
    unittest.main()
