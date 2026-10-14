"""Download and decode official transform configuration without executing game code."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

from fetch_reference import fetch, saved_flash_versions

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from generate_pet_detail_data import iter_call_args


def parse_transform_skills(source):
    result = {}
    for fields in iter_call_args(source, 'new TransformSkillDefine('):
        if len(fields) != 7:
            raise ValueError('unexpected TransformSkillDefine argument count')
        skill_id, kind = str(int(fields[0])), int(fields[6])
        if kind not in (0, 1, 2) or skill_id in result:
            raise ValueError('unknown transform skill type or duplicate ID')
        effect_arr, auto_arr = json.loads(fields[4]), json.loads(fields[5])
        if not all(isinstance(x, str) and len(x.split(':')) == 4 for x in effect_arr + auto_arr):
            raise ValueError('unexpected TramsformEffectParam encoding')
        result[skill_id] = {
            'name': json.loads(fields[1]), 'description': json.loads(fields[2]),
            'autoEffectDescription': json.loads(fields[3]),
            'effectArr': effect_arr, 'autoEffectArr': auto_arr,
            # Both names carry the original seventh constructor argument (0/1/2).
            # They do not overwrite the unrelated battle skill type.
            'type': kind, 'transformType': kind,
            'source': 'formation/formationservice',
        }
    if not result:
        raise ValueError('no official transform definitions found')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('--java', required=True)
    parser.add_argument('--ffdec', required=True)
    args = parser.parse_args()
    manifest = (args.source / 'start.xml').read_bytes()
    versions = saved_flash_versions(args.source)
    resource = 'formation/formationservice'
    version = versions.get(resource, '2000')
    url = 'https://aoqi.100bt.com/play/' + resource + '~' + version + '.swf'
    data = fetch(url)
    package = args.source / 'formation_formationservice.swf'
    package.write_bytes(data)
    output = args.source / 'formation'
    subprocess.run([args.java, '-Xmx768m', '-jar', args.ffdec,
                    '-selectclass', 'mmo.formation.transformskill.++',
                    '-export', 'script', str(output), str(package)], check=True)
    config = output / 'scripts/mmo/formation/transformskill/TransformSkillConfig.as'
    result = parse_transform_skills(config.read_text(encoding='utf-8-sig'))
    provenance = {'manifestSha256': hashlib.sha256(manifest).hexdigest(),
                  'resource': resource, 'version': version, 'url': url,
                  'sha256': hashlib.sha256(data).hexdigest(), 'entries': len(result),
                  'decompiledFile': config.relative_to(args.source).as_posix(),
                  'decompiledSha256': hashlib.sha256(config.read_bytes()).hexdigest()}
    (args.source / 'transform-skills.json').write_text(
        json.dumps(result, ensure_ascii=False), encoding='utf-8')
    (args.source / 'formation-provenance.json').write_text(
        json.dumps(provenance, ensure_ascii=False, indent=2), encoding='utf-8')
    print('official transform skills:', len(result))


if __name__ == '__main__':
    main()
