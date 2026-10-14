"""Keep lookup coverage distinct from membership and source attribution."""
import json
from pathlib import Path
import tempfile
import unittest

from audit_table_coverage import audit


class LogReferenceTests(unittest.TestCase):
    def test_hero_preload_legacy_and_real_negative_buff(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            samples = root / 'logs'
            samples.mkdir()
            catalog = root / 'catalog.json'
            catalog.write_text(json.dumps({'skills': {'31': {'name': 'hero', 'description': 'text'}},
                                            'buffs': {'-2': {'nm': 'maximum HP'}, '8': {'nm': ''},
                                                      '9': {'nm': 'legacy'}, '10': {'nm': 'dragon'},
                                                      '11': {'nm': 'item'}}}), encoding='utf-8')
            data = {'br': {'abs': '8', 'or': {'team': [{'players': [{'hs': 31,
                    'drs': [{'dbfi': 10}], 'cts': [{'bfs': '-2,-1'}]}]}]},
                    'ir': {'buffrs': [{'i': 999, 'c': {'tl': [{'bs': '11'}]}}]},
                    'fr': [{'rl': [{'si': 0, 'cn': {'ba_bfs': '9,777'}}]}]}}
            (samples / 'synthetic.json').write_text(json.dumps(data), encoding='utf-8')
            result = audit(samples, catalog)
            self.assertEqual(result['skills']['entries']['31']['roles'], {'hero_skill': 1})
            buffs = result['buffs']['entries']
            self.assertEqual(buffs['-2']['status'], 'named')
            self.assertEqual(buffs['8']['status'], 'officially_unnamed')
            self.assertEqual(buffs['8']['roles'], {'resource_preload': 1})
            self.assertEqual(buffs['9']['roles'], {'legacy_unconsumed_field': 1})
            self.assertEqual(buffs['10']['roles'], {'dragon_configuration': 1})
            self.assertEqual(buffs['11']['roles'], {'item_result': 1})
            self.assertNotIn('999', buffs)
            self.assertEqual(buffs['777']['status'], 'missing_definition')
            self.assertEqual(result['nonPositiveMarkers'], {'buffs:-1': 1, 'skills:0': 1})
            self.assertFalse(result['invalidReferences'])


if __name__ == '__main__':
    unittest.main()
