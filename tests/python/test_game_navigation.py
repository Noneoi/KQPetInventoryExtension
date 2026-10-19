import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    probe = Path(sys.argv[1]).resolve()
    node = shutil.which('node')
    if not node:
        raise RuntimeError('Navigation JavaScript regression tests require Node.js on PATH (tests only).')

    def query(mode, value):
        run = subprocess.run([str(probe), mode, value], check=True, capture_output=True, timeout=10)
        return json.loads(run.stdout)

    routes = [f'btnNewAct_storeexchangeframework_showMainPanel_{i}' for i in (1, 2, 3, 4, 5, 6, 8)]
    routes += ['btnNewAct_storeexchangeframework_showMainPanel_3_clearAll',
               'btnNewAct_futureactivity20990101_showMainPanel_23',
               'btnService_FuturePass_showMainPanelWithTab_-1']
    catalog = json.loads((Path(__file__).resolve().parents[2] / 'assets/activity-exchange-data.json').read_bytes())
    routes += [shop['navigationLink'] for shop in catalog['shops'] if shop.get('navigationLink')]
    routes = sorted(set(routes))
    cases = []
    for route in routes:
        actual = query('--script', route)
        assert actual['allowed'] and actual['script']
        cases.append({'route': route, 'script': actual['script']})
    for route in ['', 'https://example.com', 'javascript:alert(1)',
                  'btnNewAct_activity_showMainPanel\n',
                  'btnNewAct_activity_showMainPanel\');alert(1);//',
                  'btnService_FuturePass_buyItem_1', 'btnService_FuturePass_sendMessage_1',
                  'btnService_FuturePass_showMainPanel_callback',
                  'btnNewAct_activity_' + 'a' * 257]:
        assert query('--script', route) == {'allowed': False, 'script': ''}, route

    # Only an explicit dispatch receipt is accepted. Empty/legacy/unknown
    # results must not claim a displayed panel or trigger a second submission.
    for value, kind in [('submitted-flash', 0), ('submitted-h5', 0),
                        ('flash-not-ready', 1), ('h5-not-ready', 1), ('missing-game-runtime', 1),
                        ('error:fixture failure', 2), ('', 3), ('opened', 3), ('unexpected', 3)]:
        result = query('--result', value)
        assert result['kind'] == kind and result['message'], (value, result)
        assert '已打开' not in result['message'], result
    run = subprocess.run([node, str(Path(__file__).parents[1] / 'bridge/game_navigation_script_test.js')],
                         input=json.dumps(cases), text=True, capture_output=True, check=True, timeout=20)
    print(run.stdout.strip())


if __name__ == '__main__':
    main()
