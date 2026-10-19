"""Service-name-independent discovery, current adapter and coverage contracts."""
from pathlib import Path
import copy
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from activity_service_discovery import (hud_service_entries, parse_service_registry, trace_service,
                                       discover_service_roots, unresolved_alias_status, TIME_CLASS,
                                       service_module_entries, select_service_scripts)

REGISTRY = '''package mmo.interfaces { public class ServiceConfig {
    public static const services:Array=[Bind("FutureMembership","Label",IX,"mmo.fixture.Entry","future/service"),
        Disabled("Retired",IX,"mmo.old.Entry","archive/service")];
    private static function Bind(param1:String,param2:String,param3:*,param4:String,param5:String):ServiceInfo {
        return new ServiceInfo(param1,param3,param4,param5,null);
    }
    private static function Disabled(... rest):ServiceInfo { return null; }
}}'''
ENUM = '''package mmo.interfaces { public class FiscalRange {
    public static const OLD:FiscalRange=enum__(2030,"2030/01/01");
    public static const CURRENT:FiscalRange=enum__(2031,"2031/01/01");
    public static const FUTURE:FiscalRange=enum__(2032,"2032/01/01");
    public static function selected():FiscalRange {
        var now:Number=Number(DateUtil.getServerTimeInMS());
        var index:int=values.length - 1;
        while(index>=0) { if(now>=values[index]._beginTime) { return values[index]; } index--; }
        throw new Error("unavailable");
    }
    private static function enum__(param1:int,param2:String):FiscalRange {
        result._beginTime = new Date(param2 + DateUtil.getServerClosedTimeStr()).getTime();
        return result;
    }
}}'''
TIME = 'public class DateUtil { public static function getServerClosedTimeStr():String { return " 02:00:00"; } }'
ENTRY = '''package mmo.fixture {
    import mmo.interfaces.FiscalRange;
    import mmo.fixture.OldAdapter;
    import mmo.fixture.CurrentAdapter;
    import mmo.fixture.FutureAdapter;
    public class Entry {
        private static const CHOICES:Dictionary=new Dictionary();
        CHOICES[FiscalRange.OLD]=OldAdapter;
        CHOICES[FiscalRange.CURRENT]=CurrentAdapter;
        CHOICES[FiscalRange.FUTURE]=FutureAdapter;
        private var adapter:Object=createAdapter();
        private static function createAdapter():Object {
            var selected:Class=CHOICES[FiscalRange.selected()]; return new selected();
        }
        public function show():void { adapter.show(); }
    }
}'''


def fixture():
    result = {'Entry.as': ENTRY}
    for kind, alias in [('Old', 'obsoleteoffer'), ('Current', 'currentoffer'), ('Future', 'futureoffer')]:
        result[kind + 'Adapter.as'] = ('package mmo.fixture { public class ' + kind +
            'Adapter { public function show():void { new ' + kind + 'Panel(); } } }')
        result[kind + 'Panel.as'] = ('package mmo.fixture { public class ' + kind +
            'Panel { public function open():void { p.tabArray=' + kind + 'Config.TABS; } } }')
        result[kind + 'Config.as'] = ('package mmo.fixture { public class ' + kind +
            'Config { public static const TABS:Array=(function():Array {var items:*=[{"id":7,"desc":"offer",'
            '"actName":"' + alias + '","actParams":"showMainPanel","actType":AQTabDefine.DEFAULT_ACT_TYPE}];'
            'return Assert.checkElementFieldDuplicated(items,"id");})(); } }')
    return result


def trace(scripts=None, time='2031-10-18T22:00:00', enums=None):
    return trace_service(scripts or fixture(), 'mmo.fixture.Entry', 'show',
        enums if enums is not None else {'mmo.interfaces.FiscalRange': ENUM, TIME_CLASS: TIME}, time)


class ServiceDiscoveryTests(unittest.TestCase):
    def test_permanent_registrations_are_candidates_but_not_historical_roots(self):
        from public_activity_exchange_updater import build_discovery, resolve_alias
        root = ET.fromstring('''<config>
            <activity><a name="permanent" file="newactivityext/newact20000101/permanent/permanent"/>
                <a name="retired" file="newactivityext/newact20000101/retired/retired"/>
                <a name="reissued" file="newactivityext/newact20000101/reissued/reissued"/></activity>
            <allscene><a name="worldentry" file="newactivityext/newact20000101/worldentry/worldentry"/></allscene>
            <sceneactivity><scene name="town"><a name="sceneentry" file="newactivityext/newact20000101/sceneentry/sceneentry"/></scene></sceneactivity>
            <week version="20311018"><a name="reissued" file="newactivityext/newact20311018/reissued/reissued"/></week>
        </config>''')
        hud = 'const DATA:Object={"hud":[{"name":"Current permanent","tryGetService":"NewActivityService#loadAndInitNormalActivity#permanent#showMainPanel"}]};'
        result = build_discovery(root, hud)
        self.assertEqual(result['permanentEntries'], 5)
        self.assertEqual(result['roots'], ['permanent', 'reissued'])
        self.assertEqual(resolve_alias('permanent', result)[0], 'newactivityext/newact20000101/permanent/permanent')
        self.assertEqual(resolve_alias('reissued', result)[0], 'newactivityext/newact20311018/reissued/reissued')
        self.assertIn('sceneentry', result['entries'])
        self.assertIn('worldentry', result['entries'])

    def test_service_body_bridge_preserves_current_support_classes_only(self):
        scripts = fixture()
        current = trace(scripts)
        record = {**current, 'status': 'scanned',
            'registration': {'cls': 'mmo.fixture.Entry', 'module': 'future/service'},
            'entry': {'service': 'FutureMembership', 'name': 'Current'}}
        entries = service_module_entries({'records': {'FutureMembership#show': record}}, '20311018')
        entry = entries['service:FutureMembership#show']
        self.assertTrue(entry['activity']['serviceBody'])
        selected = select_service_scripts(scripts, entry['trace'])
        self.assertIn('CurrentConfig.as', selected)
        self.assertNotIn('OldConfig.as', selected)
        self.assertNotIn('FutureConfig.as', selected)
        changed = dict(scripts)
        changed['CurrentConfig.as'] += '// changed official bytes'
        with self.assertRaisesRegex(ValueError, '当前入口类图不一致'):
            select_service_scripts(changed, entry['trace'])

    def test_official_registration_helper_controls_resource_mapping(self):
        result = parse_service_registry(REGISTRY)
        self.assertEqual(result, {'FutureMembership': {'service': 'FutureMembership', 'cls': 'mmo.fixture.Entry', 'module': 'future/service'}})
        changed = REGISTRY.replace('FutureMembership', 'UnrelatedService').replace('future/service', 'entirely/different')
        self.assertEqual(parse_service_registry(changed)['UnrelatedService']['module'], 'entirely/different')

    def test_nested_hud_retains_service_method_and_parameters(self):
        result = hud_service_entries('static const DATA:Object=' + json.dumps({'list': [
            {'name': 'Current', 'tryGetService': 'FutureMembership#show#-1'},
            {'name': 'Event', 'tryGetService': 'NewActivityService#loadAndInitNormalActivity#event#showMainPanel'}]}) + ';')
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]['arguments'], ['-1'])

    def test_current_adapter_does_not_follow_historical_or_future_imports(self):
        result = trace()
        self.assertEqual(result['aliases'], ['currentoffer'])
        self.assertNotIn('mmo.fixture.OldAdapter', result['reachableClasses'])
        self.assertNotIn('mmo.fixture.FutureConfig', result['reachableClasses'])
        self.assertEqual(result['adapterSelections'][0]['value'], 'CURRENT')
        self.assertEqual(result['nextSelectionTime'], '2032-01-01T02:00:00+08:00')

    def test_adapter_changes_at_verified_server_close_time(self):
        self.assertEqual(trace(time='2032-01-01T01:59:59')['aliases'], ['currentoffer'])
        self.assertEqual(trace(time='2032-01-01T02:00:00')['aliases'], ['futureoffer'])
        self.assertEqual(trace(time='2031-12-31T18:00:00+00:00')['aliases'], ['futureoffer'])

    def test_missing_and_changed_dynamic_selector_fail_closed(self):
        with self.assertRaisesRegex(ValueError, '选择器定义缺失'):
            trace(enums={})
        with self.assertRaisesRegex(ValueError, '选择规则待适配'):
            trace(enums={'mmo.interfaces.FiscalRange': ENUM.replace('now>=values', 'now<=values'), TIME_CLASS: TIME})
        source = fixture()
        source['Entry.as'] = ENTRY.replace('CHOICES[FiscalRange.OLD]=OldAdapter;', 'CHOICES[FiscalRange.OLD]=OldAdapter; CHOICES[serverChoice()]=FutureAdapter;')
        with self.assertRaisesRegex(ValueError, '动态映射写入'):
            trace(source)

    def test_changed_tab_iife_is_pending_instead_of_guessing(self):
        scripts = fixture()
        scripts['CurrentConfig.as'] = scripts['CurrentConfig.as'].replace('return Assert.', 'items.push(dynamicTab());return Assert.')
        result = trace(scripts)
        self.assertEqual(result['aliases'], [])
        self.assertEqual(len(result['issues']), 1)
        self.assertIn('页签表静态结构', result['issues'][0]['reason'])

    def test_unconsumed_array_does_not_create_an_activity(self):
        scripts = fixture()
        scripts['CurrentConfig.as'] = scripts['CurrentConfig.as'].replace('public static const TABS:', 'public static const UNUSED:Array=[{"actName":"ghostactivity"}]; public static const TABS:')
        self.assertEqual(trace(scripts)['aliases'], ['currentoffer'])

    def test_tab_time_boundaries_and_other_entry_types_are_explicit(self):
        scripts = fixture()
        scripts['CurrentConfig.as'] = scripts['CurrentConfig.as'].replace('"id":7', '"isOnline":"20311201-20320101","id":7')
        result = trace(scripts)
        self.assertEqual(result['aliases'], [])
        self.assertEqual(result['inactive'][0]['alias'], 'currentoffer')
        self.assertEqual(result['nextSelectionTime'], '2031-12-01T02:00:00+08:00')
        scripts['CurrentConfig.as'] = scripts['CurrentConfig.as'].replace('AQTabDefine.DEFAULT_ACT_TYPE', 'SomeOtherService.TYPE')
        self.assertIn('其他入口类型', trace(scripts)['issues'][0]['reason'])

    def test_loader_wrappers_and_local_literal_bindings(self):
        scripts = fixture()
        scripts['CurrentPanel.as'] = '''package mmo.fixture { public class CurrentPanel {
            public function open():void {
                Bridge.dispatch(7,"viawrapper",{});
                var target:String=null; target="vialocal"; svc.loadAndInitNormalActivity(target,{});
            }
        }}'''
        scripts['Bridge.as'] = '''package mmo.fixture { public class Bridge {
            public static function dispatch(param1:int,param2:String,param3:Object):void {
                svc.loadAndInitNormalActivity(param2,param3);
            }
        }}'''
        self.assertEqual(trace(scripts)['aliases'], ['vialocal', 'viawrapper'])
        scripts['CurrentPanel.as'] = scripts['CurrentPanel.as'].replace('target="vialocal";', 'target="vialocal";target=runtimeAlias();')
        result = trace(scripts)
        self.assertEqual(result['aliases'], ['viawrapper'])
        self.assertTrue(result['issues'])

    def test_alias_failure_distinguishes_official_exclusion_and_unknown(self):
        discovery = {'blocked': ['blocked'], 'entries': {'off': {'online': 'false'}, 'loop': {'link': 'btnNewAct_loop_show'}, 'other': {'link': 'btnGo_scene'}}}
        self.assertEqual(unresolved_alias_status('blocked', discovery), 'blocked')
        self.assertEqual(unresolved_alias_status('off', discovery), 'offline')
        self.assertEqual(unresolved_alias_status('loop', discovery), 'cyclic-entry')
        self.assertEqual(unresolved_alias_status('missing', discovery), 'missing-registration')
        self.assertEqual(unresolved_alias_status('other', discovery), 'unrecognized-entry')

    def test_cache_reuses_only_until_next_selector_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            class Updater:
                scratch = root
                force_components = ()
                service_exports = 0
                def resource(self, name, versions):
                    path = root / (name.rsplit('/', 1)[-1] + '.swf')
                    path.write_bytes(b'fixture')
                    return path, {'resource': name, 'version': versions[name]}
                def export(self, swf, target, cls):
                    target.mkdir(parents=True, exist_ok=True)
                    path = target / (cls.rsplit('.', 1)[-1] + '.as')
                    value = REGISTRY if cls.endswith('ServiceConfig') else TIME if cls == TIME_CLASS else ENUM
                    path.write_text(value, 'utf-8')
                    return path
                def activity_exporter(self, swf, target):
                    self.service_exports += 1
                    return fixture()
            updater = Updater()
            versions = {'library/interfaces': '1', 'library/util': '1', 'future/service': '1'}
            discovery = {'latestRelease': '20311018', 'entries': {}, 'roots': [], 'serviceEntries':
                [{'service': 'FutureMembership', 'method': 'show', 'arguments': [], 'name': 'offer', 'route': 'FutureMembership#show'}]}
            previous = discover_service_roots(updater, versions, copy.deepcopy(discovery), now='2031-10-18T22:00:00')
            self.assertEqual(previous['roots'], ['currentoffer'])
            cached = discover_service_roots(updater, versions, copy.deepcopy(discovery), previous, now='2031-12-20T22:00:00')
            self.assertEqual(updater.service_exports, 1)
            self.assertEqual(cached['roots'], ['currentoffer'])
            changed = discover_service_roots(updater, versions, copy.deepcopy(discovery), cached, now='2032-01-01T02:00:00')
            self.assertEqual(updater.service_exports, 2)
            self.assertEqual(changed['roots'], ['futureoffer'])
            versions['future/service'] = '2'
            fresh = discover_service_roots(updater, versions, copy.deepcopy(discovery), changed, now='2032-01-02T02:00:00')
            self.assertEqual(updater.service_exports, 3)
            # Repeated registry failures keep only the previous confirmed roots
            # and report them as retained, never as successfully scanned.
            resource = updater.resource
            def fail_resource(*args):
                raise OSError('fixture offline')
            updater.resource = fail_resource
            failed = discover_service_roots(updater, versions, copy.deepcopy(discovery), fresh, now='2032-01-03T02:00:00')
            self.assertEqual(failed['roots'], [])
            self.assertEqual(failed['retainedRoots'], ['futureoffer'])
            failed_twice = discover_service_roots(updater, versions, copy.deepcopy(discovery), failed, now='2032-01-04T02:00:00')
            self.assertEqual(failed_twice['retainedRoots'], ['futureoffer'])
            self.assertTrue(failed_twice['pending'])
            updater.resource = resource
            recovered = discover_service_roots(updater, versions, copy.deepcopy(discovery), failed_twice, now='2032-01-05T02:00:00')
            self.assertEqual(recovered['roots'], ['futureoffer'])
            self.assertEqual(recovered['retainedRoots'], [])
            self.assertEqual(recovered['pending'], [])
            urls = []
            def fetch(url, path):
                urls.append(url)
                path.write_bytes(b'fixture')
                return {'url': url}
            updater.fetch = fetch
            versions.pop('future/service')
            with patch('public_icon_updater.loader_default', return_value={'version': '2032010500000000', 'defaultVersion': '2000'}):
                unversioned = discover_service_roots(updater, versions, copy.deepcopy(discovery), recovered, now='2032-01-05T03:00:00')
            self.assertEqual(urls, ['https://aoqi.100bt.com/play/future/service~2000.swf'])
            self.assertEqual(unversioned['pending'], [])
            self.assertEqual(unversioned['roots'], ['futureoffer'])
            unknown = copy.deepcopy(discovery)
            unknown['serviceEntries'][0]['service'] = 'UnregisteredService'
            unknown['serviceEntries'][0]['route'] = 'UnregisteredService#show'
            result = discover_service_roots(updater, versions, unknown, now='2032-01-05T04:00:00')
            self.assertEqual(len(urls), 1)
            self.assertTrue(result['pending'])


if __name__ == '__main__':
    unittest.main()
