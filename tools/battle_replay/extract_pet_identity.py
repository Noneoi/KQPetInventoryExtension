"""Resolve cosmetic species through verified official consumers, not name text.

groupRaceId also groups chasers/forms, so it is evidence of affiliation only.
It must not substitute for Cpsv3SkinSummary.findOriginalRaceId's exact match.
Skin display forms have a separate, explicit base in IPetSkinMultiForm. That
bridge describes an illustration/appearance, never a battle transformation.
"""
import hashlib
import json
from pathlib import Path
import re


def parse_skin_definitions(text):
    matches = list(re.finditer(r'\bSKIN_DEFINES_SRC\s*:\s*Array\s*=\s*', text))
    if len(matches) != 1:
        raise ValueError('Missing or ambiguous official skin table')
    rows, end = json.JSONDecoder().raw_decode(text[matches[0].end():])
    if not text[matches[0].end() + end:].lstrip().startswith(';'):
        raise ValueError('Skin table is not a literal array')
    return rows


def parse_multiple_form_groups(text):
    match = re.search(r'\bFORM_CONF\s*:\s*Array\s*=\s*\[(.*?)\];', text, re.S)
    if match is None:
        raise ValueError('Missing official battle-form groups')
    body = match.group(1)
    pattern = r'new\s+MultipleFormPetFormDefine\s*\(\s*(\[[\d\s,]*\])\s*\)'
    matches = list(re.finditer(pattern, body))
    if re.sub(pattern, '', body).strip(' \t\r\n,'):
        raise ValueError('Battle-form groups are not literal constructor arrays')
    return [json.loads(match.group(1)) for match in matches]


def build_race_expansions(race_ids, skins, multiple_form_groups):
    """Membership of PetRaceUtil.getAllSkinAndPetRaceIds([id]).

    This is a compatibility set, not a canonical-name resolver. The public
    skin getter uses Cpsv3SkinSummary, not the unused private V2 implementation.
    Return sorted membership rather than inventing the official UI list order.
    """
    by_race = {}
    for skin in skins:
        for pet in skin['availablePets']:
            by_race[int(pet['raceId'])] = skin  # Official dictionary: last write.

    def first_pet(skin, field, value):
        return next((pet for pet in skin['availablePets'] if int(pet[field]) == value), None)

    def switch_skin(race):
        skin = by_race.get(race)
        if skin is None:
            if not any(row.get('fuse') is None and first_pet(row, 'raceIdNeed', race)
                       for row in skins):
                return []
            original = race
        else:
            if skin.get('fuse') is not None:
                return []
            original = int(first_pet(skin, 'raceId', race)['raceIdNeed'])
        # A hidden exact skin entry makes getRaceIds return null, even when
        # other visible skins use the same native species.
        if any(not row.get('show', False) and first_pet(row, 'raceId', race)
               for row in skins):
            return []
        results = []
        for row in skins:
            if row.get('show', False):
                pet = first_pet(row, 'raceIdNeed', original)
                if pet is not None:
                    results.append(int(pet['raceId']))
        # getXChangeableRaceIds only adds visible skins with that exact
        # raceIdNeed, already in the membership above. It can change ordering
        # and duplicates; neither is used for compatibility membership.
        return [original] + results if results else []

    result = {}
    for value in race_ids:
        race = int(value)
        switched = switch_skin(race)
        original = switched[0] if switched else race
        # getDefine returns the first matching group. This is one expansion,
        # not a transitive closure across overlapping groups or later skins.
        forms = next((group for group in multiple_form_groups if original in group), [])
        expanded = list(dict.fromkeys([original] + forms))
        complete = set(expanded)
        for form in expanded:
            complete.update(switch_skin(form))
        result[str(race)] = [str(item) for item in sorted(complete)]
    return result


def resolve_identities(pets, skins, *, include_display_forms=False):
    candidates, evidence, display_forms = {}, {}, {}
    listed_races, required_races = set(), set()
    for row_index, skin in enumerate(skins):
        listed_races.update(str(int(pet['raceId'])) for pet in skin['availablePets'])
        # Official isFusionPoster() returns _fuse != null. The original-race
        # consumer refuses these poster entries even if they list a race ID.
        if skin.get('fuse') is not None:
            continue
        for pet_index, pet in enumerate(skin['availablePets']):
            race, original = str(int(pet['raceId'])), str(int(pet['raceIdNeed']))
            if int(original) <= 0:
                continue
            required_races.add(original)
            candidates.setdefault(race, set()).add(original)
            evidence.setdefault(race, []).append({
                'skinId': skin['id'], 'row': row_index + 1,
                'availablePet': pet_index + 1, 'raceIdNeed': original,
            })
        if include_display_forms:
            forms = skin.get('changeableRaceIds') or []
            if forms:
                base = str(int(forms[0]))
                # The consumer's first entry is the base. It is not the first
                # availablePets row (which can refer to another release).
                for form_index, value in enumerate(forms[1:], 2):
                    race = str(int(value))
                    if int(race) <= 0 or int(base) <= 0:
                        continue
                    display_forms.setdefault(race, []).append({
                        'skinId': skin['id'], 'row': row_index + 1,
                        'formIndex': form_index, 'baseDisplayRaceId': base,
                    })

    def resolve(race, seen):
        if race in seen:
            return '', [], 'cycle'
        target = pets.get(race)
        if target is None:
            return '', [], 'missingDictionaryEntry'
        seen = seen | {race}
        options = candidates.get(race, set())
        if len(options) > 1:
            return '', [], 'ambiguousSkinMapping'
        if options:
            original = next(iter(options))
            base, tail, reason = resolve(original, seen)
            return base, [{'raceId': race, 'originalRaceId': original,
                           'skinRows': evidence[race]}] + tail, reason
        if race in display_forms:
            bases = sorted({row['baseDisplayRaceId'] for row in display_forms[race]}, key=int)
            paths = []
            for display_base in bases:
                base, tail, reason = resolve(display_base, seen)
                if reason:
                    return '', [], reason
                paths.append({'baseDisplayRaceId': display_base,
                              'baseRaceId': base, 'chain': tail})
            originals = {path['baseRaceId'] for path in paths}
            if len(originals) != 1:
                return '', [], 'ambiguousDisplayFormMapping'
            base = next(iter(originals))
            return base, [{'kind': 'skinDisplayForm', 'raceId': race,
                           'originalRaceId': base, 'skinRows': display_forms[race],
                           'basePaths': paths}], ''
        # Cpsv3SkinSummary also accepts an unlisted race used as raceIdNeed.
        # A fusion/invalid skin row remains listed and cannot use this branch.
        if race not in listed_races and race in required_races:
            return race, [], ''
        if '皮肤' in target['sign'].split(','):
            return '', [], 'missingExactSkinMapping'
        return race, [], ''

    result = {}
    for race, pet in pets.items():
        current, chain, reason = resolve(race, set())
        group = pet['groupRaceId']
        result[race] = {
            'status': 'unresolved' if reason else 'skin' if chain else 'native',
            'canonicalName': '' if reason else pets[current]['name'],
            'baseRaceId': '' if reason else current,
            'reason': reason, 'chain': chain,
            'groupRaceId': group,
            'groupName': pets.get(group, {}).get('name', ''),
        }
        if chain and chain[0].get('kind') == 'skinDisplayForm':
            result[race]['mappingType'] = 'skinDisplayForm'
            result[race]['scope'] = 'Official alternate display form; not evidence of an in-battle form change.'
    return result


def extract_pet_identities(pets, skin_source, verified_source):
    if skin_source is None or verified_source is None:
        raise ValueError('Verified skin source is required for canonical pet names')
    root = Path(skin_source) / 'scripts/mmo/changepetskinv3/other'
    sources = {}
    texts = {}
    for name in ('Cpsv3SkinConfig', 'Cpsv3SkinSummary', 'Cpsv3Skin', 'Cpsv3AvailablePet'):
        path = root / (name + '.as')
        data = path.read_bytes()
        texts[name] = data.decode('utf-8-sig')
        sources[name] = {'file': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
    compact = {name: re.sub(r'\s+', '', text) for name, text in texts.items()}
    required = {
        'Cpsv3Skin': ['returnthis._fuse!=null;',
                     'if(_loc2_.raceId==param1){return_loc2_.raceIdNeed;}'],
        'Cpsv3SkinSummary': ['functionfindOriginalRaceId(param1:int):int',
                            'if(_loc2_.isFusionPoster()){return-1;}',
                            '_loc3_=_loc2_.findRaceIdNeed(param1);',
                            'if(Cpsv3SkinSummary.isAnySkinNeedThisRaceId(param1)){returnparam1;}'],
        'Cpsv3AvailablePet': ['getDataItemByRaceId(this._raceIdNeed)'],
    }
    for name, fragments in required.items():
        for fragment in fragments:
            if fragment not in compact[name]:
                raise ValueError('Unverified original-pet consumer: ' + name)
    multi_path = root / 'outer/Cpsv3SkinMultiFormImpl.as'
    include_display_forms = multi_path.exists()
    if include_display_forms:
        data = multi_path.read_bytes()
        multi_text = re.sub(r'\s+', '', data.decode('utf-8-sig'))
        for fragment in (
            'functiongetchangeableRaceIds():Array{returnthis._base.changeableRaceIds;}',
            'functiongetchangeableRaceIdsExcludeBase():Array{returnthis._base.changeableRaceIds.slice(1);}',
            'var_loc2_:Array=this.changeableRaceIds;',
            'return_loc2_[0];',
        ):
            if fragment not in multi_text:
                raise ValueError('Unverified skin display-form consumer')
        sources['Cpsv3SkinMultiFormImpl'] = {'file': str(multi_path), 'sha256': hashlib.sha256(data).hexdigest()}
    provenance = json.loads((verified_source / 'flash-resolved/provenance.json').read_text(encoding='utf-8-sig'))
    package = next(row for row in provenance['files'] if row['resource'] == 'changepetskinv3/changepetskinservice')
    if hashlib.sha256((verified_source / package['file']).read_bytes()).hexdigest() != package['sha256']:
        raise ValueError('Skin package differs from manifest-verified snapshot')
    identities = resolve_identities(pets, parse_skin_definitions(texts['Cpsv3SkinConfig']),
                                    include_display_forms=include_display_forms)
    counts = {status: sum(row['status'] == status for row in identities.values())
              for status in ('native', 'skin', 'unresolved')}
    return {'pets': identities, 'report': {
        'counts': counts, 'package': package, 'sources': sources,
        'displayFormConsumerVerified': include_display_forms,
        'displayFormIdentities': sum(row.get('mappingType') == 'skinDisplayForm' for row in identities.values()),
        'scope': 'Exact availablePets.raceId -> raceIdNeed joins; verified skin display forms follow changeableRaceIds[0] only when all paths agree. groupRaceId and switchable battle-form groups are affiliation only. No name splitting, race arithmetic, era merging, skill replacement or battle-cause inference.',
    }}
