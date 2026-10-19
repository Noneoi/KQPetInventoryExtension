import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from activity_navigation import navigation_metadata, apply_navigation_graph, valid_route


def entry(body, imports='', extra=''):
    return {'Entry.as': f'package sample {{ {imports} public class Entry {{ {extra} '
                       f'public function initActivity(input:Object):void {{ {body} }} }} }}'}


ACTIVITY = {'cls': 'sample.Entry', 'activityAlias': 'futurereward'}


def record(alias, metadata, shops=True):
    return {'status': 'supported', 'activity': {'activityAlias': alias}, 'navigationMetadata': metadata,
            'shops': [{'navigationLink': '', 'source': {}, 'goods': [{'id': 8}]}] if shops else []}


class NavigationTests(unittest.TestCase):
    def test_explicit_entry_and_unreachable_lookalike(self):
        scripts = entry('if(input == NewActivityParamsKey.SHOW_MAIN_PANEL) { C.instance.checkShowPanel(); }')
        self.assertTrue(navigation_metadata(scripts, ACTIVITY)['main'])
        scripts = entry('if(input.action == "showMainPanel") { C.instance.show(); }',
                        extra='public function unrelated(input:Object):void { if(input == "showMainPanel") { C.instance.show(); } }')
        self.assertFalse(navigation_metadata(scripts, ACTIVITY)['main'])
        self.assertFalse(navigation_metadata(scripts, {**ACTIVITY, 'cls': 'sample.Missing'})['main'])

    def test_controller_casts_and_copy(self):
        for cast in ('String(copy)', '(copy as String)'):
            scripts = entry('Other.instance().accept(input);', 'import logic.Other;')
            scripts['Other.as'] = f'''package logic {{ public class Other {{
              public function accept(arg:Object):void {{ var copy:Object=arg;
                if({cast}.match("show.+Panel.*") != null) {{ this.fetchAndShow(copy); }} }}
              private function fetchAndShow(arg:Object):void {{ self.showMainPanel(true); }}
            }} }}'''
            self.assertTrue(navigation_metadata(scripts, ACTIVITY)['main'])
            scripts['Entry.as'] = scripts['Entry.as'].replace('accept(input)', 'accept(input.action)')
            self.assertFalse(navigation_metadata(scripts, ACTIVITY)['main'])

    def test_adapter_data_flow(self):
        scripts = entry('Controller.instance.accept(input);', 'import logic.Controller;')
        scripts['Controller.as'] = '''package logic { import logic.Adapter; public class Controller {
          public function accept(p:Object):void { var params:Object=p;
            this._a=new Adapter(params);
            if(this._a.action.match(/show.+Panel/) != null) { this.fetch(); } }
          private function fetch():void { if(self._a.action == "showMainPanel") { self.showMainPanel(self._a); } }
        } }'''
        scripts['Adapter.as'] = '''package logic { public class Adapter {
          public function Adapter(p:Object) { var parts:Array=null;
            if(isTypeOf(p,String)) { parts=String(p).match(/(show.+Panel)(_(.+))?/); this._value=parts[1]; } }
          public function get action():String { return this._value; }
        } }'''
        self.assertTrue(navigation_metadata(scripts, ACTIVITY)['main'])
        scripts['Adapter.as'] = scripts['Adapter.as'].replace('parts[1]', 'parts[3]')
        self.assertFalse(navigation_metadata(scripts, ACTIVITY)['main'])

    def test_numeric_tab_uses_consumed_definition_not_caller_tab(self):
        scripts = entry('if(input is String && input.indexOf(NewActivityParamsKey.SHOW_MAIN_PANEL)==0) { Controller.instance.open(input.toString()); }', 'import logic.Controller;')
        scripts['Controller.as'] = '''package logic { import views.Panel; public class Controller {
          public function open(p:String):void { var tab:int=0; var str:String=p; var parts:Array=str.split("_");
            tab=parseInt(parts[parts.length-1]); this.info(function():void { showMainPanel(tab); }); }
          public function showMainPanel(p:int):void { new Panel(p).show(AlignType.FULL_SCREEN); }
        } }'''
        scripts['Panel.as'] = '''package views { public class Panel {
          public function Panel(p:int) { this._initial=p; }
          private function initTab():void { var index:int=Config.ORDER.indexOf(this._initial);
            index=index==-1?0:index; var host:BriefTabHost=new BriefTabHost(getChildEx("host"),Config.ORDER.length,index,this.select,this.check); }
        } }'''
        scripts['Config.as'] = 'package views { public class Config { public static const ORDER:Array=[7,12]; } }'
        metadata = navigation_metadata(scripts, ACTIVITY)
        self.assertEqual(metadata['mainAction'], 'showMainPanel_7')
        graph = {'child': record('futurereward', metadata), 'unrelated': record('unrelated', {'references': ['btnNewAct_futurereward_showMainPanel_4']}, False)}
        apply_navigation_graph(graph, {})
        self.assertEqual(graph['child']['shops'][0]['navigationLink'], 'btnNewAct_futurereward_showMainPanel_7')
        scripts['Panel.as'] = scripts['Panel.as'].replace('index==-1?0:index', 'index')
        self.assertNotIn('mainAction', navigation_metadata(scripts, ACTIVITY))

    def test_tab_frame_bound_table_and_target(self):
        scripts = entry('var action:Object=input; this._tabs.handleServiceCall(action);', extra='''private var _tabs:TabFrame;
          public function initWhenDownLoaded(p:String):void { this._tabs=new TabFrame().setTabArray(Config.ROWS).init(); }''')
        tabs = [{'route': 'btnNewAct_futurereward_showMainPanel_9', 'targetAlias': 'child', 'fullPanel': True, 'table': 'Config.ROWS'},
                {'route': 'btnNewAct_futurereward_showMainPanel_2', 'targetAlias': 'wrong', 'fullPanel': True, 'table': 'Unused.ROWS'},
                {'route': 'btnNewAct_futurereward_showMainPanel_3', 'targetAlias': 'partial', 'fullPanel': False, 'table': 'Config.ROWS'}]
        result = navigation_metadata(scripts, ACTIVITY, {'tabs': tabs})
        self.assertEqual(result['tabs'], tabs[:1])

    def test_disabled_ambiguous_and_removed_parent(self):
        tab = {'route': 'btnNewAct_parent_showMainPanel_19', 'targetAlias': 'child', 'table': 'Config.ROWS'}
        base = {'parent': record('parent', {'tabs': [tab]}, False), 'child': record('child', {'main': True})}
        graph = copy.deepcopy(base)
        apply_navigation_graph(graph, {})
        self.assertEqual(graph['child']['shops'][0]['navigationLink'], tab['route'])
        graph['parent']['navigationMetadata']['tabs'] = []
        apply_navigation_graph(graph, {})
        self.assertEqual(graph['child']['shops'][0]['navigationLink'], 'btnNewAct_child_showMainPanel')
        for discovery in ({'blocked': ['parent']}, {'entries': {'parent': {'online': 'false'}}}):
            graph = copy.deepcopy(base); apply_navigation_graph(graph, discovery)
            self.assertEqual(graph['child']['shops'][0]['navigationLink'], 'btnNewAct_child_showMainPanel')
        graph = copy.deepcopy(base)
        graph['other'] = record('other', {'tabs': [{**tab, 'route': 'btnNewAct_other_showMainPanel_8'}]}, False)
        apply_navigation_graph(graph, {})
        self.assertEqual(graph['child']['shops'][0]['navigationLink'], 'btnNewAct_child_showMainPanel')
        self.assertEqual(graph['child']['shops'][0]['goods'], [{'id': 8}])

    def test_service_uses_registered_open_method_only(self):
        scripts = {'Pass.as': 'package service { public class Pass { public function showMainPanelWithTab(tab:int):void { adapter.show(tab); } } }'}
        activity = {'serviceBody': True, 'cls': 'service.Pass', 'serviceRoute': 'FuturePass#showMainPanelWithTab#-1'}
        self.assertEqual(navigation_metadata(scripts, activity)['serviceRoute'], 'btnService_FuturePass_showMainPanelWithTab_-1')
        for route in ('FuturePass#buyItem#1', 'FuturePass#showMissing#1', 'FuturePass#showMainPanelWithTab#callback()'):
            self.assertNotIn('serviceRoute', navigation_metadata(scripts, {**activity, 'serviceRoute': route}))
        self.assertFalse(valid_route('btnService_FuturePass_buyItem_1'))
        self.assertFalse(valid_route('btnNewAct_activity_showMainPanel\n'))


if __name__ == '__main__':
    unittest.main()
