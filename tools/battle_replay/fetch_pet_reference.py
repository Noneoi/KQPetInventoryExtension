"""Fetch the manifest-pinned official dictionaries used by FighterInfo.maxPower.

JPEXS reads package bytecode; game code is never executed. Keep package and
decompiler-output hashes so the generated replay catalog can be reproduced.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
import xml.etree.ElementTree as ET

from fetch_reference import fetch, saved_flash_versions

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from generate_pet_detail_data import iter_create_args


def parse_dictionary(source, resource):
    pets = {}
    for fields in iter_create_args(source):
        # PetDictionaryDataItem.create: param9 attributes; param50 transformSkillNo.
        # Preserve native attributes separately from the battle's initial attr.
        attributes = json.loads(fields[8])
        # Some official placeholders are literally "??"; preserve them as unknown
        # rather than converting to an invented numeric attribute.
        native_attributes = [item.strip() for item in attributes.split(',') if item.strip()]
        race = str(int(fields[0]))
        pets[race] = {'name': json.loads(fields[1]),
                      'maxPower': 150 if int(fields[16]) > 0 else 100,
                      'ultSuperSkillNo': int(fields[16]),
                      'nativeAttributes': native_attributes,
                      'transformSkillNo': int(fields[49]), 'source': resource}
    return pets


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('--java', required=True)
    parser.add_argument('--ffdec', required=True)
    args = parser.parse_args()
    manifest = (args.source / 'start.xml').read_bytes()
    versions = saved_flash_versions(args.source)
    provenance = {'retrievedUtc': datetime.now(timezone.utc).isoformat(),
                  'manifestSha256': hashlib.sha256(manifest).hexdigest(), 'packages': []}
    pets = {}
    for resource, cls in (
        ('pet/petdictionarydata', 'mmo.pet.petdictionarydata.PetDictionaryDataContents'),
        ('pet/petdictionarydataupdate', 'mmo.pet.petdictionarydata.PetDictionaryDataContentsUpdate'),
    ):
        version = versions.get(resource, '2000')
        url = 'https://aoqi.100bt.com/play/' + resource + '~' + version + '.swf'
        package = args.source / (resource.replace('/', '_') + '.swf')
        data = fetch(url)
        package.write_bytes(data)
        output = args.source / resource.rsplit('/', 1)[-1]
        subprocess.run([args.java, '-Xmx768m', '-jar', args.ffdec, '-selectclass', cls,
                        '-export', 'script', str(output), str(package)], check=True)
        source_file = output / 'scripts' / (cls.replace('.', '/') + '.as')
        source = source_file.read_text(encoding='utf-8-sig')
        entries = parse_dictionary(source, resource)
        count = len(entries)
        pets.update(entries)
        if not count:
            raise ValueError('dictionary contained no parsed entries: ' + resource)
        provenance['packages'].append({'resource': resource, 'version': version, 'url': url,
            'sha256': hashlib.sha256(data).hexdigest(), 'entries': count,
            'decompiledFile': source_file.relative_to(args.source).as_posix(),
            'decompiledSha256': hashlib.sha256(source_file.read_bytes()).hexdigest()})
    (args.source / 'pets.json').write_text(json.dumps(pets, ensure_ascii=False), encoding='utf-8')
    (args.source / 'pet-provenance.json').write_text(json.dumps(provenance, ensure_ascii=False, indent=2), encoding='utf-8')
    print('official pet entries:', len(pets))


if __name__ == '__main__':
    main()
