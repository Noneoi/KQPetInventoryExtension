import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from public_activity_exchange_updater import parse_module, parse_tables


def fixture():
    fields = [('id', 'int'), ('name', 'String'), ('price', 'int'), ('effect', 'String'),
              ('conditional', 'Boolean'), ('alternate', 'int')]
    definition = 'public function Offer(param1:int,param2:String,param3:int,param4:String,param5:Boolean=false,param6:int=0){super();'
    definition += ''.join(f'this._{name}=param{i};' for i, (name, _) in enumerate(fields, 1)) + '}'
    definition += ''.join(f'public function get {name}():{typ}{{return this._{name};}}' for name, typ in fields)
    config = '''public static const Rows:Array=[new Offer(10,"ordinary",120,"83"),
      new Offer(20,"stage",109,"92",true,90),new Offer(90,"alternate",79,"95")];
      public static const Visible:Array=[20,10]; public static const Race:int=1234;
      public static function find(param1:int):Offer {
        var row:Offer=null;var i:int=0;while(i<Rows.length){row=Rows[i];if(row.id==param1){return row;}i++;}return null;}
    '''
    ui = '''private function click(param1:int):void {
        var index:int=param1;var id:int=int(Config.Visible[index]);var offer:Offer=Config.find(id);
        _manager.purchase(function():*{refresh();},offer);
    }'''
    manager = '''public function purchase(param1:Function,param2:Offer):void {
      var def:Offer=param2;var item:StrengthenComboItem=StrengthenComboItem.createByString(def.effect);
      svc.showStrengthenPanel(item,StrengthenComboPaymentFactory.getSimple(function(param1:StrengthenComboItem,param2:AQSOutDetails,param3:Function):* {
        var aqsOutDetails:AQSOutDetails=param2;var targetDef:*=def;
        if(Boolean(targetDef.conditional)){
          currentSlot=aqsOutDetails.petBriefInfo.petSacredEquipment.getSlot();
          needSpecEq=(forSvc(ISacredEquipmentService) as ISacredEquipmentService).getStageUpgradeMaterial(currentSlot.defineId,currentSlot.stageUpgradePlanId,currentSlot.stageLevel).some(function(param1:String,... rest):Boolean{
            return new MaterialCommonData(param1).type==MaterialTypes.EQUIPMENT4PET_ITEM;
          });
          if(!needSpecEq){targetDef=Config.find(def.alternate);}
        }
        confirm(targetDef,aqsOutDetails,param3);
      }),[Config.Race]);
    }
    private function confirm(param1:Offer,param2:AQSOutDetails,param3:Function):void {
      var def:Offer=param1;if(!ActUtil.checkEnoughtMoney(def.price)){return;}
      client.gainBonus(function():*{},def.id,model.getNewCi(),param2.params);
    }'''
    return {'Offer.as': definition, 'Config.as': config, 'View.as': ui, 'Manager.as': manager}


class ConstructedOfferTests(unittest.TestCase):
    def parse(self, scripts):
        return parse_module(scripts, 'arbitrary/newmodule', {'activityName': 'test', 'startTime': '20260101'}, include_manual=True)

    def test_defaults_ui_order_and_hidden_conditional_price(self):
        scripts = fixture()
        _, constants = parse_tables(scripts)
        self.assertEqual(constants['Config.Rows'][0]['conditional'], False)
        shops, pending, _ = self.parse(scripts)
        self.assertEqual(pending, [])
        goods = shops[0]['goods']
        self.assertEqual([row['itemServerId'] for row in goods], [20, 10])
        self.assertEqual(goods[0]['enhanceType'], '92')
        self.assertFalse(goods[0]['costKnown'])
        self.assertEqual(goods[0]['cost'], '')
        self.assertEqual([r['amount'] for r in goods[0]['priceRule']['cases']], [109, 79])
        self.assertEqual(goods[0]['priceRule']['cases'][1]['rewardCode'], '95')
        self.assertEqual(goods[1]['cost'], '8:2:120')
        self.assertEqual(goods[1]['exchangeKind'], 'diamond')
        self.assertFalse(goods[1]['manualSelectionRequired'])

    def test_renamed_classes_fields_ids_and_species_remain_data(self):
        scripts = fixture()
        renamed = {}
        for filename, source in scripts.items():
            for before, after in [('Config', 'Catalog'), ('Offer', 'Definition'), ('Rows', 'Products'),
                                  ('Visible', 'Listed'), ('effect', 'upgrade'), ('price', 'amount'),
                                  ('1234', '60001'), ('109', '137'), ('79', '43')]:
                filename, source = filename.replace(before, after), source.replace(before, after)
            renamed[filename] = source
        shops, pending, _ = self.parse(renamed)
        self.assertEqual(pending, [])
        self.assertEqual(shops[0]['goods'][0]['raceIds'], [60001])
        self.assertEqual([r['amount'] for r in shops[0]['goods'][0]['priceRule']['cases']], [137, 43])

    def test_unverified_condition_is_pending_not_an_unconditional_low_price(self):
        scripts = fixture()
        scripts['Manager.as'] = scripts['Manager.as'].replace('MaterialTypes.EQUIPMENT4PET_ITEM', 'MaterialTypes.UNKNOWN_NEW_KIND')
        shops, pending, _ = self.parse(scripts)
        self.assertEqual(shops, [])
        self.assertTrue(any('条件替代' in item['reason'] for item in pending))

    def test_table_without_click_path_does_not_invent_visible_offers(self):
        scripts = fixture()
        scripts['View.as'] = ''
        shops, pending, _ = self.parse(scripts)
        self.assertEqual(shops, [])
        self.assertTrue(any('界面项目列表' in item['reason'] for item in pending))

    def test_nonliteral_constructor_defaults_stay_unresolved(self):
        scripts = fixture()
        scripts['Offer.as'] = scripts['Offer.as'].replace('param5:Boolean=false', 'param5:Boolean=calculate()')
        _, constants = parse_tables(scripts)
        self.assertIn('$ref', constants['Config.Rows'][0])

    def test_non_reward_navigation_table_has_no_false_offer_warning(self):
        scripts = fixture()
        scripts['Manager.as'] = ''
        scripts['Config.as'] = scripts['Config.as'].replace('new Offer(20,', 'new Offer(10,')
        shops, pending, _ = self.parse(scripts)
        self.assertEqual(shops, [])
        self.assertEqual(pending, [])

    def test_service_body_keeps_consumed_offers_and_excludes_old_static_stock(self):
        scripts = fixture()
        scripts['History.as'] = 'public static const OLD:Array=[{"id":70,"cost":"8:2:1","simpleParams":"CommonEnhancePrize,,,83,1234"}];'
        activity = {'activityName': 'renamed service', 'startTime': '20260101', 'serviceBody': True}
        shops, pending, _ = parse_module(scripts, 'arbitrary/service', activity, include_manual=True)
        self.assertEqual(pending, [])
        self.assertEqual(sum(len(shop['goods']) for shop in shops), 2)
        self.assertIn('History.OLD', activity['serviceUnboundTables'])


if __name__ == '__main__':
    unittest.main()
