import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import public_skill_updater as skills
from public_update_support import Operations, canonical, sha, atomic_bytes

class SkillUpdateTests(unittest.TestCase):
    def test_same_release_rechecks_actual_resources_and_same_bytes_skip_parse(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); output = root / 'catalog'; output.mkdir()
            counts = {'build': 0, 'fetch': 0}; content = [b'first']
            def resource(name):
                counts['fetch'] += 1
                path = root / (sha(name.encode()) + '.data'); path.write_bytes(content[0])
                return path, {'sha256': sha(content[0]), 'version': '20261001', 'url': name}
            def build(*args):
                counts['build'] += 1
                return {'schema': 1, 'source': args[1], 'pets': {str(n): {} for n in range(1, 9001)},
                        'skills': {str(n): {'name': content[0].decode()} for n in range(1, 20001)},
                        'entries': {str(n): {} for n in range(101)},
                        'relationRules': {'schema': 1, 'summon': {'available': True, 'bySourceRaceId': {}},
                                          'carry': {'available': True, 'bySourceRaceId': {}}}}
            target = output / 'pet-skill-data.json'
            updater = types.SimpleNamespace(scratch=root, catalog=output,
                sources=types.SimpleNamespace(h5=lambda: {'version': '20261001'}, h5_resource=resource, operations=Operations()),
                current=lambda _: json.loads(target.read_bytes()) if target.exists() else {})
            with patch.object(skills, 'build_catalog', side_effect=build), patch.object(skills, 'safe_extract'):
                self.assertTrue(skills.update_skills(updater, lambda path, value: atomic_bytes(path, canonical(value))))
                self.assertFalse(skills.update_skills(updater, lambda path, value: atomic_bytes(path, canonical(value))))
                self.assertEqual(counts, {'fetch': 2 * (len(skills.ARCHIVES) + len(skills.DIRECT)), 'build': 1})
                content[0] = b'same version repaired'
                self.assertTrue(skills.update_skills(updater, lambda path, value: atomic_bytes(path, canonical(value))))
                self.assertEqual(counts, {'fetch': 3 * (len(skills.ARCHIVES) + len(skills.DIRECT)), 'build': 2})
                previous = target.read_bytes()
                content[0] = b'missing relation config'
                complete_build = build
                def incomplete_build(*args):
                    value = complete_build(*args)
                    value['relationRules']['carry']['available'] = False
                    return value
                with patch.object(skills, 'build_catalog', side_effect=incomplete_build):
                    with self.assertRaisesRegex(ValueError, '召唤或携同关系配置不完整'):
                        skills.update_skills(updater, lambda path, value: atomic_bytes(path, canonical(value)))
                self.assertEqual(target.read_bytes(), previous)

if __name__ == '__main__': unittest.main()
