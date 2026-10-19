"""Static local/enum flows require a real consumer, never an event-name rule."""
import json
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_activity_exchange_updater import parse_module
from activity_call_rewards import literal_enum_constants


def parse(scripts, support=None):
    shops, pending, _ = parse_module(scripts, 'future/service', {'activityName': '未来服务'}, include_manual=True, reward_support=support)
    return [good for shop in shops for good in shop['goods']], pending, shops


def local_fixture(prefix='Future', effect='35$71:1', race=9001):
    scripts = {
        'Config.as': f'class Config {{static const EFFECT:String="{effect}";static const RACE:int={race};static const TARGETS:Array=[RACE];}}',
        'Controller.as': '''class Controller {private var _client:Client;
          function receive():void {var self:Controller=null;self=this;
            forSvc(IStrengthenComboService,function(service:IStrengthenComboService):void {
              var pay:Function=null;
              pay=function(item:StrengthenComboItem,details:AQSOutDetails,done:Function):void {
                self._client.take(details,function(packet:Object):void {self._model.markDone();done();});
              };
              var effect:String=Config.EFFECT;var races:Array=Config.TARGETS;
              var reward:StrengthenComboItem=StrengthenComboItem.createByString(effect);
              service.showStrengthenPanel(reward,StrengthenComboPaymentFactory.getSimple(pay),races);
            });
          }
        }''',
        'Client.as': '''class Client {static const TAKE:String="881_999_7";
          function take(details:AQSOutDetails,done:Function):void {this.request(Client.TAKE,done,details.params);}}
        ''',
        'Model.as': '''class Model {
          function isDone():Boolean {return this._done;}
          function markDone():void {this._done=true;this.dispatchEventX(Events.DONE);}
        }''',
        'Panel.as': '''class Panel {private var _controller:Controller;
          function bind(map:Object):void {map["claimButton"]=function():void {self._controller.receive();};}
          function update():void {
            if(this._model.hasAccess()) {
              this.setVisible(["gift"],true);
              if(this._model.isDone()) {this.setVisible(["gift","claimButton"],false);}
              else {this.setVisible(["gift","claimButton"],true);
                this.setEnabled(["gift","claimButton"],{"enabled":this._model.membership(),"disabledTips":"需成为活动会员"});}
            } else {this.setVisible(["gift"],false);}
          }
        }''',
    }
    for cls in ('Config', 'Controller', 'Client', 'Model', 'Panel'):
        scripts = {key.replace(cls, prefix + cls): re.sub(r'\b' + cls + r'\b', prefix + cls, value) for key, value in scripts.items()}
    return scripts


def enum_fixture(prefix='Future', multiplier=1):
    codes = ['11', '61', '25$6', '37$6', '53$5']
    rows = [{'caption': '奖励' + str(i), 'points': amount * multiplier,
             'cap': cap + multiplier, 'effect': code, 'species': [9001, 9002]}
            for i, (code, amount, cap) in enumerate(zip(codes, [20, 40, 25, 25, 25], [6, 6, 18, 24, 30]))]
    scripts = {
        'Config.as': 'class Config {static const ROWS:Array=' + json.dumps(rows) + ';}',
        'Entry.as': '''class Entry extends Prize {
          static function initialize():void {Config.ROWS.forEach(make);}
          function Entry(value:Object) {for(field in value) {this["_"+field]=ENUM.initField(this,value,field);}}
          static function make(value:Object,index:int,rows:Array):Entry {var entry:Entry=new Entry(value);return entry;}
          function get item():StrengthenComboItem {return StrengthenComboItem.createByString(this._effect);}
          function get races():Array {return this._species.concat();}
          function get price():int {return this._points;}
          function get maximum():int {return this._cap;}
          function get caption():String {return this._caption;}
        }''',
        'Controller.as': '''class Controller {
          function exchange(argument:Entry):void {var entry:Entry=argument;
            var pay:Function=function(item:Object,details:Object,done:Function):void {
              self._client.exchange(entry,details,function():void {self._model.credits-=entry.price;done();});
            };
            service.showStrengthenPanel(entry.item,StrengthenComboPaymentFactory.getSimple(pay),entry.races);
          }
        }''',
        'Panel.as': '''class Panel {
          function update():void {var row:Entry=getSelected();
            if(this._model.getUsed(row)>=row.maximum) {hide();}
            else if(this._model.credits<row.price) {setEnabled("button",{"enabled":false,"disabledTips":"星光不足"});}
          }
        }''',
    }
    for cls in ('Config', 'Entry', 'Controller', 'Panel'):
        scripts = {key.replace(cls, prefix + cls): re.sub(r'\b' + cls + r'\b', prefix + cls, value) for key, value in scripts.items()}
    return scripts


class CallRewardTests(unittest.TestCase):
    def test_enum_field_completes_tip_and_missing_field_is_not_silently_omitted(self):
        scripts = local_fixture()
        scripts['FutureConfig.as'] += 'static const RANGE:Season=Season.LATER;'
        scripts['FuturePanel.as'] = scripts['FuturePanel.as'].replace('"需成为活动会员"', '"成为" + FutureConfig.RANGE.year + "会员即可领取"')
        good, = parse(scripts)[0]
        self.assertIn('提示尚未完整解析', good['unlock'])
        self.assertNotIn('成为会员即可领取', good['unlock'])
        enum = '''class Season {
          static const LATER:Season=make(2099,"2099/01/01");
          static function make(value:int,date:String):Season {
            holder=new Season(); holder._year=value; return holder;
          }
          function get year():int {return this._year;}
        }'''
        values = literal_enum_constants({'Season.as': enum})
        self.assertEqual(values, {'Season.LATER': {'year': 2099}})
        good, = parse(scripts, {'publicEnumConstants': values})[0]
        self.assertIn('成为2099会员即可领取', good['unlock'])
        self.assertIn('当前账号资格尚未确认', good['unlock'])

    def test_inline_callback_wrapper_inherited_field_and_claim_stage(self):
        for prefix, amount in [('Future', 288), ('Another', 499)]:
            scripts = local_fixture()
            scripts['FutureConfig.as'] += f'''static const SLOT:int=1;static const CLAIM:int=10;
              static const PACKAGES:Array=[{{"index":0,"name":"材料"}},{{"index":1,"name":"组合","params":"11-62","price":{amount}}}];'''
            scripts['FutureController.as'] = '''class FutureController {private var _client:FutureClient;
              function receive(callback:Function):void {
                var info:Object=FutureConfig.PACKAGES[FutureConfig.SLOT];
                var reward:StrengthenComboItem=StrengthenComboItem.createByString(info["params"]);
                service.showStrengthenPanel(reward,StrengthenComboPaymentFactory.getSimple(function(item:StrengthenComboItem,details:AQSOutDetails,done:Function):void {
                  finish(function():void {done();});
                }),[9001]);
              }
              function finish(done:Function):void {
                this._client.take(FutureConfig.CLAIM,function():void {_model.markDone();done();});
              }
              function buy(index:int):void {var price:int=0;price=int(FutureConfig.PACKAGES[index]["price"]);
                if(ActUtil.getMyDiamond()<price) {return;} }
            }'''
            scripts['FutureClient.as'] = '''class FutureClient extends ClientSA {
              function take(index:int,done:Function):void {var params:Object={}; params["bi"]=index;
                this.request(ClientSA.CMD_GAIN_BONUS,done,params);}
            }'''
            scripts['FutureModel.as'] = '''class FutureModel {
              function get isDone():Boolean {return this._done;}
              function markDone():void {this._done=true;this.notify(Events.DONE);}
            }'''
            scripts['Base.as'] = 'class Base {protected var _controller:FutureController;}'
            scripts['FuturePanel.as'] = '''class FuturePanel extends Base {
              function bind(map:Object):void {map["claimButton"]=function():void {_controller.receive(function():void {});};}
              function update():void {
                setFrame(["gift","item_"+index,"button"],this._model.isDone ? 3 : (this._model.purchased ? 2 : 1));
                setEnabled(["gift","item_"+index,"button","claimButton"],!this._model.isDone,"已领取");
              }
            }'''
            scripts = {name.replace('Future', prefix): source.replace('Future', prefix) for name, source in scripts.items()}
            goods, pending, _ = parse(scripts)
            self.assertFalse(pending)
            good, = [g for g in goods if g.get('source', {}).get('callEvidence', {}).get('consumer') == 'IStrengthenComboService.showStrengthenPanel']
            self.assertEqual(good['enhanceType'], '11-62')
            self.assertEqual(good['limitCount'], 1)
            self.assertEqual(good['cost'], '')
            self.assertFalse(good['costKnown'])
            self.assertIn('已购养成套餐的领取阶段', good['costDescription'])
            facts = good['source']['tradeFacts']
            self.assertEqual(facts['payment']['selectors'], {'bi': 10})
            self.assertEqual(facts['purchaseEntitlement']['purchaseAmount'], amount)
            self.assertFalse(facts['purchaseEntitlement']['claimDebitKnown'])
            self.assertNotIn('quotaObservation', good)

    def test_local_constants_and_variables_preserve_full_effect_and_targets(self):
        for prefix, race, effect in [('Spring', 9001, '35$71:1'), ('Winter', 9100, '35$88:3')]:
            goods, pending, _ = parse(local_fixture(prefix, effect, race))
            self.assertFalse(pending)
            good, = goods
            self.assertEqual((good['enhanceType'], good['raceIds']), (effect, [race]))
            self.assertTrue(good['manualSelectionRequired'])
            self.assertEqual(good['acquisitionKind'], 'reward')
            self.assertFalse(good['costKnown'])
            self.assertEqual(good['limitCount'], 1)
            self.assertNotIn('quotaObservation', good)
            facts = good['source']['tradeFacts']
            self.assertEqual(facts['payment']['command'], '881_999_7')
            controls = facts['entryConditions'][0]['controls']
            self.assertTrue(any('hasAccess()' in str(control['when']) for control in controls))
            self.assertTrue(any('isDone()' in str(control['when']) and any(p['value'] is False for p in control['when']) for control in controls))
            self.assertTrue(any(control.get('enabledExpression') == 'this._model.membership()' for control in controls))
            self.assertIn('需成为活动会员', good['unlock'])

    def test_mutated_array_or_sibling_scope_does_not_invent_target(self):
        scripts = local_fixture()
        source = scripts['FutureController.as']
        scripts['FutureController.as'] = source.replace('service.showStrengthenPanel', 'races.push(runtimeRace);service.showStrengthenPanel')
        self.assertEqual(parse(scripts)[0], [])
        scripts['FutureController.as'] = source.replace('var races:Array=FutureConfig.TARGETS;', 'if(flag) {var races:Array=FutureConfig.TARGETS;}')
        self.assertEqual(parse(scripts)[0], [])

    def test_unused_constructor_never_becomes_reward(self):
        scripts = local_fixture()
        scripts['FutureController.as'] = scripts['FutureController.as'].replace('.showStrengthenPanel(', '.drawIcon(')
        self.assertEqual(parse(scripts)[0], [])

    def test_callback_name_alone_does_not_establish_one_time_claim(self):
        scripts = local_fixture()
        scripts['FutureClient.as'] = scripts['FutureClient.as'].replace('this.request(FutureClient.TAKE,done,details.params);', 'done();')
        good, = parse(scripts)[0]
        self.assertFalse(good['quotaKnown'])
        self.assertNotIn('claim', good['source'].get('tradeFacts', {}))

    def test_success_marker_without_claim_button_gate_is_not_a_quota(self):
        scripts = local_fixture()
        scripts['FuturePanel.as'] = scripts['FuturePanel.as'].replace('this.setVisible(["gift","claimButton"],false);', 'this.setVisible(["gift","claimedIcon"],true);')
        good, = parse(scripts)[0]
        self.assertFalse(good['quotaKnown'])
        self.assertNotIn('claim', good['source'].get('tradeFacts', {}))

    def test_enum_getters_and_consumers_bind_five_rows_under_future_names(self):
        for prefix, multiplier in [('Spring', 1), ('Winter', 3)]:
            goods, pending, shops = parse(enum_fixture(prefix, multiplier))
            self.assertFalse(pending)
            self.assertEqual([g['enhanceType'] for g in goods], ['11', '61', '25$6', '37$6', '53$5'])
            self.assertEqual([g['limitCount'] for g in goods], [6 + multiplier, 6 + multiplier, 18 + multiplier, 24 + multiplier, 30 + multiplier])
            self.assertEqual([g['activityCosts'][0] for g in goods], [{'name': '星光', 'count': amount * multiplier} for amount in [20, 40, 25, 25, 25]])
            self.assertTrue(all(g['raceIds'] == [9001, 9002] and g['cost'] == '' and g['manualSelectionRequired'] for g in goods))
            self.assertTrue(all(g['displayIdentityOnly'] and 'quotaObservation' not in g for g in goods))
            self.assertTrue(all(not s['observation']['requests'] for s in shops))

    def test_unconsumed_getter_and_removed_enum_binding_do_not_make_offers(self):
        scripts = enum_fixture()
        original = scripts['FutureController.as']
        scripts['FutureController.as'] = original.replace('.showStrengthenPanel(', '.drawIcon(')
        self.assertEqual(parse(scripts)[0], [])
        scripts['FutureController.as'] = original
        scripts['FutureEntry.as'] = scripts['FutureEntry.as'].replace('FutureConfig.ROWS.forEach(make);', '')
        self.assertEqual(parse(scripts)[0], [])

    def test_unproven_currency_stays_unknown_and_does_not_become_diamonds(self):
        scripts = enum_fixture()
        scripts['FutureController.as'] = scripts['FutureController.as'].replace('self._model.credits-=entry.price;', '')
        goods, _, _ = parse(scripts)
        self.assertTrue(all(g['cost'] == '' and not g['costKnown'] and 'activityCosts' not in g for g in goods))


if __name__ == '__main__':
    unittest.main()
