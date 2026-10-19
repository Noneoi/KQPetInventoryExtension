// Run the exact production-generated function body in QCefView's result
// wrapper. No network, browser state or game account is used by this fixture.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const cases = JSON.parse(fs.readFileSync(0, 'utf8'));
let count = 0;
for (const {route, script} of cases) {
  function execute({flash, h5, document = {}, body = script} = {}) {
    const calls = [];
    const helper = {doAuto(value) {assert.equal(this, helper); calls.push(['h5', value]);}};
    if (flash === 'ready' || flash === 'throw') {
      const plugin = {gotogetpet(value, mode) {
        assert.equal(this, plugin);
        calls.push(['flash', value, mode]);
        if (flash === 'throw') throw new Error('fixture failure');
        // ExternalInterface's void return is deliberately undefined.
      }};
      document.myFlash = plugin;
    } else if (flash === 'loading') document.myFlash = {};
    const page = {document};
    page.window = page;
    if (h5) page.AQLib = h5 === 'ready' ? {ClickEffectHelper: helper} : {};
    let result;
    page.__cefview_report_js_result__ = (context, value) => {
      assert.equal(context, 'navigation-test'); result = value;
    };
    vm.runInNewContext('window.__cefview_report_js_result__("navigation-test", function(){' + body + '\n}());', page);
    count++;
    return {result, calls};
  }
  assert.deepEqual(execute({flash:'ready'}), {result:'submitted-flash', calls:[['flash',route,-1]]});
  assert.deepEqual(execute({h5:'ready'}), {result:'submitted-h5', calls:[['h5',route]]});
  assert.deepEqual(execute({flash:'ready',h5:'ready'}), {result:'submitted-flash',calls:[['flash',route,-1]]});
  assert.deepEqual(execute({flash:'loading',h5:'ready'}), {result:'flash-not-ready',calls:[]});
  assert.deepEqual(execute({h5:'loading'}), {result:'h5-not-ready',calls:[]});
  assert.deepEqual(execute(), {result:'missing-game-runtime',calls:[]});
  assert.deepEqual(execute({flash:'throw',h5:'ready'}), {result:'error:fixture failure',calls:[['flash',route,-1]]});
  const document = {};
  Object.defineProperty(document, 'myFlash', {get(){throw new Error('plugin unavailable');}});
  assert.deepEqual(execute({document,h5:'ready'}), {result:'error:plugin unavailable',calls:[]});
  // Reproduce the old bug: the inner return is lost without an outer return.
  assert.deepEqual(execute({flash:'ready',body:script.replace(/^return /,'')}), {result:undefined,calls:[['flash',route,-1]]});
}
console.log(JSON.stringify({passed:count,routes:cases.length}));
