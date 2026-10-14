"""Extract the official Flash pet-dragon display definitions, never effects.

The caller owns catalog writes. This extractor verifies the freshly exported
scripts against their extraction manifest and the current interfaces package.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def _read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def _sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_pet_dragons(source, auxiliary=None):
    source, auxiliary = Path(source), Path(auxiliary or source)
    snapshot = auxiliary / 'dragon-official'
    manifest = _read(snapshot / 'extraction.json')
    packages = _read(source / 'provenance.json')['packages']
    package = next(p for p in packages if p['resource'] == 'library/interfaces')
    swf = auxiliary / 'library_interfaces.swf'
    if manifest['packageSha256'] != package['sha256'] or _sha(swf) != package['sha256']:
        raise ValueError('pet-dragon extraction does not match current library/interfaces')
    scripts = snapshot / 'interfaces/scripts/mmo/interfaces/petdragon'
    texts, sources = {}, {}
    for filename in ('PetDragonDefines.as', 'PetDragonDefine.as', 'PetDragonGene.as', 'PetDragonLevel.as'):
        path = scripts / filename
        digest = _sha(path)
        if manifest['scripts'].get(filename) != digest:
            raise ValueError(f'{filename} differs from the recorded interfaces extraction')
        texts[filename] = path.read_text(encoding='utf-8-sig')
        sources[filename] = {
            'resource': 'library/interfaces', 'version': str(package['version']),
            'packageSha256': package['sha256'], 'sha256': digest,
            'path': 'mmo/interfaces/petdragon/' + filename,
        }
    literal = r'"(?:[^"\\]|\\.)*"'
    constructor = re.compile(
        rf'"(?P<key>\d+)"\s*:\s*new PetDragonDefine\(\s*(?P<id>\d+)\s*,\s*'
        rf'(?P<name>{literal})\s*,\s*(?P<color>{literal})\s*,\s*'
        rf'(?P<skillType>{literal})\s*,\s*(?P<descriptionTemplate>{literal})\s*\)')
    text = texts['PetDragonDefines.as']
    definitions = {}
    for match in constructor.finditer(text):
        key = match['key']
        if key != match['id'] or key in definitions:
            raise ValueError('inconsistent or duplicate pet-dragon definition ID')
        row = {name: json.loads(match[name]) for name in ('name', 'color', 'skillType', 'descriptionTemplate')}
        row.update(id=key, source={**sources['PetDragonDefines.as'], 'line': text[:match.start()].count('\n') + 1})
        definitions[key] = row
    if not definitions or len(definitions) != text.count('new PetDragonDefine('):
        raise ValueError('unsupported PetDragonDefines constructor format')
    qualities = {value: name for name, value in re.findall(
        r'public static const ([A-Z]+):int\s*=\s*(\d+)\s*;', texts['PetDragonLevel.as'])}
    if not qualities:
        raise ValueError('missing official pet-dragon quality constants')
    gene = texts['PetDragonGene.as']
    prefix = re.search(r'return this\.geneArray\[0\] == "(.)" && this\.geneArray\[1\] == "(.)";', gene)
    if not prefix:
        raise ValueError('unsupported official pet-dragon glitter rule')
    return {
        'schemaVersion': 1, 'definitions': definitions, 'qualities': qualities,
        'qualitySource': sources['PetDragonLevel.as'],
        'geneRule': {'glitteryPrefix': prefix[1] + prefix[2], 'caseSensitive': True, 'source': sources['PetDragonGene.as']},
        'sources': list(sources.values()),
        'descriptionRule': 'FightPetDragonInfo.getSkillDescription: replace N with recorded nv; bracket arithmetic is truncated with int().',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--auxiliary', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = extract_pet_dragons(args.source, args.auxiliary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'definitions': len(result['definitions']), 'qualities': result['qualities'], 'output': str(args.output)}, ensure_ascii=False))
