"""Future reskins must retain consumer-backed facts without cross-table guesses."""
import json
import re
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_activity_exchange_updater import parse_module
from activity_periods import apply_graph_periods, panel_resources


def fixture(name='Future', *, price=83, maximum=2, claimed=False):
    row = {'serverIndex': 7, 'index': 0, 'rewardType': 'Strengthen', 'rewardParams': '92$1',
           'filter': [8101], 'discount': price}
    if not claimed:
        row['total_limited'] = maximum
    scripts = {
        'Config.as': 'class Config {static const ACT_ID:int=909; static const ROWS:Array=' + json.dumps([row]) + ';}',
        'Reward.as': '''class Reward extends ActReward {
          public function get discount():int {return fragmentCollection.getFrag("discount").configInfo.asInt;}
          public function get limit():ActRewardFragmentLimited {return fragmentCollection.getFrag("total_limited") as ActRewardFragmentLimited;}
          public function confirmClaim(callback:Function):void {showTip(Tip.diamondSelf(this.discount,"",callback));}
        }''',
        'Controller.as': '''function getInfo():void {data = new ActRewardsData();
          data.setConsistentConfig({"total_limited_key":"b%d"});
          CLAIM
          data.init(Config.ROWS,packet,Reward);}
        '''.replace('CLAIM', 'data.setClaimedBiKey("oo");' if claimed else ''),
        'Client.as': '''class Client extends ClientSA {
          function Client(){super(Config.ACT_ID);}
          function info(callback:Function):void {request(ClientSA.CMD_GET_INFO,callback);}
        }''',
    }
    for cls in ('Config', 'Reward', 'Controller', 'Client'):
        scripts = {key.replace(cls, name + cls): re.sub(r'\b' + cls + r'\b', name + cls, value) for key, value in scripts.items()}
    return scripts


def parse(scripts):
    shops, pending, _ = parse_module(scripts, 'future/renamed/activity', {'activityName': '下期全新活动'}, include_manual=True)
    if pending:
        raise AssertionError(pending)
    return shops


def progress_fixture(name='Future', costs=(50, 10, 100, 10, 150)):
    rows = [{'index': i, 'serverIndex': 20 + i, 'cost': cost,
             'rewardType': 'Strengthen', 'rewardParams': '11-62', 'filter': [8101]}
            for i, cost in enumerate(costs)]
    scripts = {
        'Config.as': 'class Config {static const ROWS:Array=' + json.dumps(rows) + ';}',
        'Reward.as': '''class Reward extends ActReward {
          function initConfig(data:Object):ActReward {this._amount=data["cost"];return this;}
          function get amount():int {return this._amount;}
        }''',
        'Model.as': '''class Model {
          function init(packet:Object):void {this._rewards=[];
            Config.ROWS.forEach(function(row:*,i:*,all:*):* {
              _rewards.push(new Reward().initConfig(row).initData(packet));
            }); this._progress=packet["tokens"];
          }
          function get rewards():Array {return this._rewards;}
          function get progress():int {return this._progress;}
        }''',
        'Panel.as': '''class Panel {private var _model:Model;
          function update():void {var progress:int=this._model.progress;
            var current:Reward;var sum:int=0;var i:int=0;
            while(i<this._model.rewards.length) {
              current=this._model.rewards[i]; sum+=current.amount;
              if(i==this._model.rewards.length-1) {showDirectPurchase(current);break;}
              if(!current.claimed && progress>=sum) {showClaim(current);break;}
              if(progress<sum) {showProgress(current);break;}
              i++;
            }
          }
        }''',
    }
    for cls in ('Config', 'Reward', 'Model', 'Panel'):
        scripts = {key.replace(cls, name + cls): re.sub(r'\b' + cls + r'\b', name + cls, value) for key, value in scripts.items()}
    return scripts


class RewardFactsTest(unittest.TestCase):
    def test_prefix_sum_progress_is_not_price_and_future_names_are_irrelevant(self):
        for name, costs, expected in [('NextSeason', (50, 10, 100, 10, 150), [50, 60, 160, 170]),
                                      ('Renamed2099', (21, 3, 90, 6, 200), [21, 24, 114, 120])]:
            goods = parse(progress_fixture(name, costs))[0]['goods']
            self.assertEqual([good['progressRequirement']['amount'] for good in goods[:-1]], expected)
            self.assertTrue(all(good['acquisitionKind'] == 'progress' and good['cost'] == '' and not good['costKnown'] for good in goods[:-1]))
            self.assertIn('不是本次扣费', goods[2]['costDescription'])
            self.assertEqual(goods[2]['progressRequirement']['statePath'], ['tokens'])
            self.assertNotIn('progressRequirement', goods[-1])
            self.assertEqual(goods[-1]['acquisitionKind'], 'exchange')

    def test_prefix_sum_requires_actual_zero_origin_and_comparison(self):
        for before, after in [('var sum:int=0', 'var sum:int=100'), ('progress>=sum', 'progress>=current.amount')]:
            scripts = progress_fixture()
            scripts['FuturePanel.as'] = scripts['FuturePanel.as'].replace(before, after)
            self.assertTrue(all('progressRequirement' not in good for shop in parse(scripts) for good in shop['goods']))

    def test_progress_enrichment_preserves_existing_product_identity(self):
        scripts = progress_fixture()
        after = parse(scripts)[0]['goods']
        scripts['FuturePanel.as'] = scripts['FuturePanel.as'].replace('sum+=current.amount;', '')
        before = parse(scripts)[0]['goods']
        for left, right in zip(before, after):
            self.assertEqual(left['itemServerId'], right['itemServerId'])
            self.assertEqual(left['rewardRaw'], right['rewardRaw'])
            self.assertEqual(left['enhanceType'], right['enhanceType'])

    def test_future_names_and_prices_are_taken_from_consumers(self):
        for name, amount, maximum in [('Spring2099', 83, 2), ('Winter2100', 187, 7)]:
            shop = parse(fixture(name, price=amount, maximum=maximum))[0]
            good = shop['goods'][0]
            self.assertEqual(good['cost'], f'8:2:{amount}')
            self.assertEqual(good['limitCount'], maximum)
            self.assertEqual(good['quotaObservation']['path'], ['b7', 'b7'])
            self.assertEqual(shop['observation']['requests'][0]['params'], {'ai': 909})
            self.assertEqual(good['exchangeKind'], 'diamond')

    def test_boolean_claim_and_zero_limit_have_different_meanings(self):
        good = parse(fixture(claimed=True, price=0))[0]['goods'][0]
        self.assertEqual(good['limitCount'], 1)
        self.assertEqual(good['quotaObservation']['encoding'], 'boolean-count')
        self.assertEqual(good['quotaObservation']['path'], ['b7', 'oo'])
        self.assertIn('免费', good['costDescription'])
        good = parse(fixture(maximum=0))[0]['goods'][0]
        self.assertFalse(good['quotaKnown'])
        self.assertNotIn('quotaObservation', good)
        self.assertIn('未设置', good['quotaDescription'])

    def test_numeric_price_alone_does_not_prove_currency(self):
        scripts = fixture()
        scripts['FutureReward.as'] = scripts['FutureReward.as'].replace('showTip(Tip.diamondSelf(this.discount,"",callback));', '')
        good = parse(scripts)[0]['goods'][0]
        self.assertFalse(good['costKnown'])
        self.assertEqual(good['cost'], '')

    def test_unconsumed_table_does_not_inherit_price_or_counter(self):
        scripts = fixture()
        scripts['FutureConfig.as'] += '''static const UNUSED:Array=[{"index":8,"discount":999,"total_limited":8,
            "rewardType":"Strengthen","rewardParams":"92$1","filter":[8101]}];'''
        good = next(s for s in parse(scripts) if s['sourceKey'].endswith('.UNUSED'))['goods'][0]
        self.assertFalse(good['costKnown'])
        self.assertFalse(good['quotaKnown'])
        self.assertNotIn('quotaObservation', good)

    def test_unrelated_second_account_request_cannot_supply_counters(self):
        scripts = fixture()
        scripts['OtherClient.as'] = '''class OtherClient extends ClientSA {function OtherClient(){super(991);}
          function info(callback:Function):void {request(ClientSA.CMD_GET_INFO,callback);}}'''
        good = parse(scripts)[0]['goods'][0]
        self.assertEqual(good['limitCount'], 2)
        self.assertEqual(good['cost'], '8:2:83')
        self.assertNotIn('quotaObservation', good)

    def test_two_tables_reset_consistent_config(self):
        scripts = fixture()
        scripts['FutureConfig.as'] += '''static const SECOND:Array=[{"index":8,"discount":999,"total_limited":8,
            "rewardType":"Strengthen","rewardParams":"92$1","filter":[8101]}];'''
        scripts['FutureController.as'] += '''function another():void {data = new ActRewardsData();
            data.init(FutureConfig.SECOND,packet,FutureReward);}'''
        good = next(s for s in parse(scripts) if s['sourceKey'].endswith('.SECOND'))['goods'][0]
        self.assertEqual(good['limitCount'], 8)
        self.assertNotIn('quotaObservation', good)

    def test_purchase_count_price_tiers_are_not_a_fixed_discount(self):
        scripts = fixture(price=258)
        scripts['FutureReward.as'] = scripts['FutureReward.as'].replace(
            'return fragmentCollection.getFrag("discount").configInfo.asInt;', '''var base:int = int(fragmentCollection.getFrag("discount").configInfo.asInt);
            return this._times == 1 ? int(base - 10) : (this._times >= 2 ? int(base - 20) : base);''')
        scripts['FutureReward.as'] += 'function initData(packet:Object):void {this._times = packet["bt"];}'
        good = parse(scripts)[0]['goods'][0]
        self.assertFalse(good['costKnown'])
        self.assertEqual([p['cost'] for p in good['priceOptions']], ['8:2:258', '8:2:248', '8:2:238'])
        self.assertEqual(good['priceOptions'][-1]['when'], {'requestKey': 'state', 'path': ['bt'], 'op': 'gte', 'value': 2})

    def test_pass_progress_is_not_a_level_or_a_debit(self):
        rows = [{'index': 30, 'num': 600, 'prizeNormal': 'CommonEnhancePrize,-1,1,92,8101'}]
        scripts = {'FuturePass.as': 'static const REWARDS:Array=' + json.dumps(rows) + ';'}
        good = parse(scripts)[0]['goods'][0]
        self.assertIn('累计进度：600', good['costDescription'])
        self.assertNotIn('等级', good['costDescription'])
        self.assertFalse(good['costKnown'])
        self.assertEqual(good['cost'], '')

    def test_price_vector_uses_qualified_or_local_saturating_selector(self):
        for selector in ('FutureReward.cap', 'cap'):
            scripts = fixture(price=[91, 82, 73])
            scripts['FutureReward.as'] = scripts['FutureReward.as'].replace(
                'return fragmentCollection.getFrag("discount").configInfo.asInt;',
                'var row:Object = FutureConfig.ROWS[serverIndex]; var prices:Array = row["discount"] as Array; '
                f'return prices[{selector}(FutureConfig.model.times)];')
            scripts['FutureReward.as'] += '''
                function cap(value:int):int {if(value >= 2) {return 2;} return Math.max(0,value);}
                function get times():int {return this._times;}
                function initData(packet:Object):void {this._times = packet["bt"];}
            '''
            good = parse(scripts)[0]['goods'][0]
            self.assertEqual([p['cost'] for p in good['priceOptions']], ['8:2:91', '8:2:82', '8:2:73'])
            self.assertEqual(good['priceOptions'][-1]['when']['op'], 'gte')
            self.assertFalse(good['costKnown'])

    def test_guard_closes_only_its_consumed_table(self):
        scripts = fixture()
        scripts['FutureConfig.as'] += '''static function get permitsShop():Boolean {return !DateUtil.isAfterTimeWithDelayClose("20991016 06:00:00");}
          static const OTHER:Array=[{"index":8,"discount":10,"rewardType":"Strengthen","rewardParams":"92$1","filter":[8101]}];'''
        scripts['Panel.as'] = '''function showShop():void {if(!FutureConfig.permitsShop) {hide();return;}
            FutureConfig.ROWS.forEach(render);}
            function unrelated():void {FutureConfig.OTHER.forEach(render);}'''
        shops = parse(scripts)
        self.assertEqual(next(s for s in shops if s['sourceKey'].endswith('.ROWS'))['goods'][0]['endsAt'], '2099-10-16T06:00:00+08:00')
        self.assertEqual(next(s for s in shops if s['sourceKey'].endswith('.OTHER'))['goods'][0]['endsAt'], '')
        del scripts['Panel.as']
        self.assertTrue(all(not g['endsAt'] for s in parse(scripts) for g in s['goods']))

    def test_container_resource_and_nested_deadline(self):
        scripts = {'Container.as': 'new TabFrame().setUrl(Config.url("res.fui")).setCls("MainPanel");',
                   'Config.as': 'private const ROOT:String="future/container/";'}
        self.assertEqual(panel_resources(scripts, 'future/container/container'), ['future/container/res'])
        good = {'source': {'periodBounds': {}}}
        modules = {
            'outer': {'panelPeriods': [{'key': 'outer/res', 'version': '20991001', 'period': {'endTime': '20991016'}}],
                      'periodMetadata': {'tabs': [{'route': 'outer_1', 'targetAlias': 'inner', 'fullPanel': True, 'period': {}, 'table': 'Tabs'}]}},
            'inner': {'periodMetadata': {'tabs': [{'route': 'inner_2', 'targetAlias': 'leaf', 'fullPanel': True, 'period': {}, 'table': 'Tabs'}]}},
            'leaf': {'shops': [{'goods': [good]}]},
        }
        from unittest.mock import patch
        with patch('public_activity_exchange_updater.resolve_alias', side_effect=lambda alias, _: (alias, {})):
            apply_graph_periods(modules, {})
        self.assertEqual(good['endsAt'], '2099-10-17T00:00:00+08:00')

    def test_chinese_period_label_and_iso_tab_boundaries(self):
        from activity_periods import fui_period, tab_range, calendar_range
        import tempfile
        import zipfile
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'future.fui'
            with zipfile.ZipFile(path, 'w') as z:
                z.writestr('package.xml', '<resources><component id="main" name="MainRes" exported="true"/></resources>')
                z.writestr('main.xml', '<component><displayList><text text="活动时间：9月24日-10月29日"/></displayList></component>')
            self.assertEqual(fui_period(path, '20990924'), {})
            result = fui_period(path, '20990924', 'MainRes')
            self.assertEqual((result['startTime'], result['endTime']), ('20990924', '20991029'))
            self.assertEqual(fui_period(path, '20990101', 'MainRes'), {})
        self.assertEqual(tab_range('2099-09-24-2099-10-29'),
                         {'startTime': '2099-09-24 02:00:00', 'endTime': '2099-10-29 02:00:00'})
        self.assertEqual(calendar_range('2099年9月24日-2099年10月29日'),
                         {'startTime': '20990924', 'endTime': '20991029'})


if __name__ == '__main__':
    unittest.main()
