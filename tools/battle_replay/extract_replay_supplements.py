"""Statically extract official replay reference tables; never infer battle causes.

All indexes contain zero-based rowId values into their group's ``rows``. A
pair key is exactly ``sourceRaceId_targetRaceId``. Source order and shadowed
rows remain available. Carry indexes require the *verified* result of the
official skin/multiple-form expansion; XML's additional '#' tokens are not
that result. Formation conditions are documentation, not simulated effects.
No ActionScript, description expression, URL, or resource is executed.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import xml.parsers.expat


def sha(data):
    return hashlib.sha256(data).hexdigest()


def integer(value, default=None):
    if value is None or value == '':
        if default is not None:
            return default
        raise ValueError('Missing official integer')
    if isinstance(value, bool) or not re.fullmatch(r'[+-]?\d+', str(value).strip()):
        raise ValueError('Unsupported official integer: ' + str(value))
    return int(value)


def number(value, default=0):
    if value is None or value == '':
        return default
    if isinstance(value, bool):
        raise ValueError('Boolean is not an official numeric field')
    result = float(value)
    if not math.isfinite(result):
        raise ValueError('Nonfinite official number')
    return int(result) if result.is_integer() else result


def ids(value, separator='#'):
    return [str(integer(token)) for token in value.split(separator) if token.strip()]


class StaticLiteralParser:
    """A deliberately small AS literal/constructor grammar, not an evaluator."""
    def __init__(self, text, constructors=()):
        self.text, self.pos = text, 0
        self.constructors = set(constructors)

    def skip(self):
        while True:
            match = re.match(r'(?:\s+|//[^\r\n]*(?:\r?\n|$)|/\*.*?\*/)', self.text[self.pos:], re.S)
            if not match:
                return
            self.pos += match.end()

    def expect(self, token):
        self.skip()
        if not self.text.startswith(token, self.pos):
            raise ValueError(f'Expected {token!r} at {self.pos}')
        self.pos += len(token)

    def sequence(self, closing):
        values = []
        self.skip()
        if self.text.startswith(closing, self.pos):
            self.pos += len(closing)
            return values
        while True:
            values.append(self.value())
            self.skip()
            if self.text.startswith(closing, self.pos):
                self.pos += len(closing)
                return values
            self.expect(',')

    def value(self):
        self.skip()
        start = self.pos
        ch = self.text[self.pos:self.pos + 1]
        if ch == '"':
            value, length = json.JSONDecoder().raw_decode(self.text[self.pos:])
            self.pos += length
            return value
        if ch == '[':
            self.pos += 1
            return self.sequence(']')
        if ch == '{':
            self.pos += 1
            values, offsets = [], []  # Keep duplicate keys and source order.
            self.skip()
            if self.text.startswith('}', self.pos):
                self.pos += 1
                return {'objectEntries': values, 'entryOffsets': offsets}
            while True:
                self.skip()
                offsets.append(self.pos)
                key = self.value()
                if isinstance(key, (dict, list, bool)) or key is None:
                    raise ValueError('Unsupported AS object key')
                self.expect(':')
                values.append([str(key), self.value()])
                self.skip()
                if self.text.startswith('}', self.pos):
                    self.pos += 1
                    return {'objectEntries': values, 'entryOffsets': offsets}
                self.expect(',')
        constructor = re.match(r'new\s+(\w+)\s*\(', self.text[self.pos:])
        if constructor:
            name = constructor[1]
            if name not in self.constructors:
                raise ValueError('Unapproved literal constructor: ' + name)
            self.pos += constructor.end()
            args = self.sequence(')')
            return {'constructor': name, 'arguments': args, 'offset': start,
                    'rawExpression': self.text[start:self.pos]}
        literal = re.match(r'(true|false|null)(?![\w$])', self.text[self.pos:])
        if literal:
            self.pos += literal.end()
            return {'true': True, 'false': False, 'null': None}[literal[1]]
        numeric = re.match(r'-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?', self.text[self.pos:])
        if numeric:
            self.pos += numeric.end()
            return number(numeric[0])
        raise ValueError('Unsupported AS expression at ' + str(self.pos))


def declaration(text, name, constructors):
    matches = list(re.finditer(r'\b' + re.escape(name) + r'\s*:\s*(?:Array|Object)\s*=\s*', text))
    if len(matches) != 1:
        raise ValueError('Missing or ambiguous official declaration: ' + name)
    offset = matches[0].end()
    parser = StaticLiteralParser(text[offset:], constructors)
    value = parser.value()
    parser.expect(';')
    return value, offset


def _compact(text):
    return re.sub(r'\s+', '', text)


def require(text, *fragments):
    compact = _compact(text)
    for fragment in fragments:
        if _compact(fragment) not in compact:
            raise ValueError('Official consumer schema changed: ' + fragment)


def _evidence(path):
    path = Path(path)
    data = path.read_bytes()
    return {'file': str(path), 'sha256': sha(data), 'bytes': len(data)}


def verify_race_expansions(expansions):
    if expansions is None:
        return []
    if not isinstance(expansions, dict) or not isinstance(expansions.get('byRaceId'), dict):
        raise ValueError('Invalid official race-expansion document')
    sources = expansions.get('sourceEvidence')
    if not isinstance(sources, list) or not sources:
        raise ValueError('Official race-expansion sources are required')
    for source in sources:
        if not isinstance(source, dict) or not source.get('file') or not source.get('sha256'):
            raise ValueError('Incomplete official race-expansion source')
        if sha(Path(source['file']).read_bytes()) != source['sha256']:
            raise ValueError('Official race-expansion source hash mismatch: ' + source['file'])
    for race, variants in expansions['byRaceId'].items():
        if str(integer(race)) != race or int(race) <= 0 or not isinstance(variants, list):
            raise ValueError('Invalid race-expansion key or list')
        normalized = [str(integer(value)) for value in variants]
        if any(int(value) <= 0 for value in normalized) or len(set(normalized)) != len(normalized):
            raise ValueError('Invalid or duplicate race-expansion member')
    return sources


def _find(roots, relative, required=True):
    for root in roots:
        if root is None:
            continue
        candidate = Path(root) / relative
        if candidate.is_file():
            return candidate
    if required:
        raise FileNotFoundError('Official source missing: ' + relative)
    return None


def _xml_file(source, name, suffix='xmlClass'):
    files = list((Path(source) / 'xml').glob('*.' + name + '__' + suffix + '.xml'))
    if len(files) != 1:
        raise ValueError('Expected one official XML: ' + name + '/' + suffix)
    return files[0]


def xml_records(text, evidence):
    """Keep every element's fields, exact line and one-based XML path."""
    if re.search(r'<!\s*(?:DOCTYPE|ENTITY)\b', text, re.I):
        raise ValueError('External/entity declarations are not table literals')
    parser = xml.parsers.expat.ParserCreate()
    stack, counts, records = [], [Counter()], []
    lines = text.splitlines()

    def start(tag, attributes):
        counts[-1][tag] += 1
        path = '/'.join([*stack, f'{tag}[{counts[-1][tag]}]'])
        stack.append(f'{tag}[{counts[-1][tag]}]')
        counts.append(Counter())
        line = parser.CurrentLineNumber
        records.append({'tag': tag, 'rawAttributes': dict(attributes),
                        'source': {**evidence, 'line': line, 'path': '/' + path},
                        'rawLine': lines[line - 1] if line <= len(lines) else ''})

    def end(tag):
        stack.pop()
        counts.pop()

    parser.StartElementHandler, parser.EndElementHandler = start, end
    parser.Parse(text, True)
    return records


def _rows(records, parent, tag='s'):
    pattern = re.compile(r'/' + re.escape(parent) + r'\[\d+\]/' + re.escape(tag) + r'\[\d+\]$')
    result = []
    for record in records:
        if record['tag'] == tag and pattern.search(record['source']['path']):
            row = dict(record)
            row['source'] = {**record['source'], 'row': len(result) + 1}
            result.append(row)
    return result


def _display(row, name='', descriptions=None, readings=()):
    row.update(name=name, title=name, descriptions=descriptions or {},
               description='\n'.join(value for value in (descriptions or {}).values() if value),
               readings=[{'label': label, 'value': value} for label, value in readings])
    return row


def _at(text, evidence, needle):
    offset = text.find(needle)
    if offset < 0:
        raise ValueError('Official consumer statement changed: ' + needle)
    return {**evidence, 'line': text.count('\n', 0, offset) + 1, 'statement': needle}


def _generated_label(row, label, references=()):
    row.update(name=label, title=label, generatedLabel=True, officialName='',
               labelReferences=list(references))


def apply_display_labels(result, pets):
    """Presentation labels never claim to be missing official skill names."""
    def names(races):
        values = []
        for race in races:
            identity = pets.get(race, {}).get('identity', {})
            name = identity.get('canonicalName', '')
            label = name or f'精灵 [{race}]'
            if label not in values:
                values.append(label)
        return '、'.join(values)

    for group in ('summon', 'carry'):
        for row in result[group]['rows']:
            source_ids = row['sourceRaceIds'] if group == 'summon' else [row['carrierRaceId']]
            target_ids = row['targetRaceIds'] if group == 'summon' else [row['carriedPetRaceId']]
            refs = [{'table': 'pets', 'id': race, 'field': 'identity.canonicalName'}
                    for race in dict.fromkeys(source_ids + target_ids)]
            _generated_label(row, names(source_ids) + ' → ' + names(target_ids) + ' · 配置配对', refs)
    for row in result['powerFull']['rows']:
        _generated_label(row, names([row['raceId']]) + ' · 满气势表现',
                         [{'table': 'pets', 'id': row['raceId'], 'field': 'identity.canonicalName'}])
        row['readings'].insert(0, {'label': '资料性质', 'value': '官方满气势表现 Buff，不是战报附加状态'})
    for row in result['almighty']['rows']:
        _generated_label(row, names([row['raceId']]) + ' · 全能技资料',
                         [{'table': 'pets', 'id': row['raceId'], 'field': 'identity.canonicalName'}])
    for row in result['titles']['rows']:
        _generated_label(row, f'称号 [{row["titleId"]}] · 表现资料')
    resource_labels = {'avatar': '战斗外观', 'changeAvatar': '换形表现', 'actionMovie': '动作表现',
                       'preDownload': '预载资源', 'background': '场景背景'}
    for group, label in resource_labels.items():
        for row in result['resources'].get(group, {}).get('rows', []):
            identifier = row.get('id', row.get('resource', row.get('scene', '')))
            _generated_label(row, label + (' [' + identifier + ']' if identifier else '') + ' · 配置资料')


def add_hero_description_readings(group, reader_text, reader_evidence, race_expansions=None):
    require(reader_text, 'descriptionArr = skillDefine.description.split("\\n@\\n");',
            'var _loc3_:* = param1.split("^");', 'map[param1.mutilIndex] = param1;',
            'description += desc.replace("@触发@","");')
    group['descriptionReader'] = _at(reader_text, reader_evidence, 'public function getHeroSkillTips(')
    group['descriptionScope'] = 'official skill descriptions only; no independent translated rule or activation evaluation'
    expansions = (race_expansions or {}).get('byRaceId', {})
    for skill_id, row_ids in group['bySkillId'].items():
        shown = [group['rows'][index] for index in row_ids if group['rows'][index]['isShow']]
        has_multi = any(row['mutilIndex'] > 0 for row in shown)
        selected = {str(row['mutilIndex']): row['rowId'] for row in shown}
        for row_id in row_ids:
            row = group['rows'][row_id]
            row['readings'][1]['label'] = '效果对象参数（配置原文）'
            if row['cls'] == 'RaceEffected':
                seed = str(integer((row['clsParams'] or '').split(',')[0]))
                row['effectRaceSeed'] = seed
                row['effectiveClsParamsStatus'] = 'resolved' if seed in expansions else 'unavailable'
                if seed in expansions:
                    row['effectiveRaceIds'] = expansions[seed]
                    row['readings'].append({'label': '官方皮肤及形态兼容种族集合', 'value': expansions[seed]})
            description = row['descriptions'].get('skillDescription', '')
            row['conditionDescriptions'] = []
            row['conditionDescriptionStatus'] = 'whole-skill-description'
            if not has_multi:
                continue
            row['conditionDescriptionStatus'] = 'not-selected-by-show-index'
            if selected.get(str(row['mutilIndex'])) != row_id:
                continue
            row['conditionDescriptionStatus'] = 'unmatched-official-delimiter'
            parts = description.split('\n@\n')
            # Preserve a source-format mismatch instead of substituting <br>
            # for a delimiter that the verified reader does not substitute.
            if '<br>@<br>' in description and '\n@\n' not in description:
                continue
            for part in parts:
                pair = part.split('^')
                if len(pair) >= 2 and pair[0] == str(row['mutilIndex']):
                    row['conditionDescriptions'].append(pair[1].replace('@触发@', '', 1))
            if row['conditionDescriptions']:
                row['conditionDescriptionStatus'] = 'resolved-official-index'
                row['readings'].append({'label': '该档官方说明（不判断触发）',
                                        'value': row['conditionDescriptions']})


def _index_last(group, index, key, row_id, history):
    group.setdefault(history, {}).setdefault(key, []).append(row_id)
    group.setdefault(index, {})[key] = row_id


def parse_summons(main, hidden):
    group = {'rows': [], 'byPair': {}, 'historyByPair': {}, 'bySourceRaceId': {},
             'hiddenSourceRaceIds': [], 'loadOrder': ['main', 'hide'],
             'indexRule': 'exact s × sed IDs; later row wins; hide table is loaded after main'}
    for table, records in [('main', main), ('hide', hidden)]:
        for original in _rows(records, 'pet'):
            attrs = original['rawAttributes']
            row = {**original, 'rowId': len(group['rows']), 'table': table,
                   'sourceRaceIds': ids(attrs.get('s', '')),
                   'targetRaceIds': ids(attrs.get('sed', '')),
                   'effectId': str(integer(attrs.get('effect'), 0)),
                   'isShow': integer(attrs.get('isShow'), 0) == 1,
                   'summonerTeamId': str(integer(attrs.get('team'), 0)),
                   'summonedPetTeamId': str(integer(attrs.get('pt'), 0)),
                   'startTimeRaw': attrs.get('startTime', '')}
            _display(row, descriptions={'description': attrs.get('des', ''),
                                       'skillDescription': attrs.get('sdes', '')},
                     readings=[('召唤者种族', row['sourceRaceIds']), ('召唤对象种族', row['targetRaceIds'])])
            group['rows'].append(row)
            for source in row['sourceRaceIds']:
                group['bySourceRaceId'].setdefault(source, []).append(row['rowId'])
                if table == 'hide' and source not in group['hiddenSourceRaceIds']:
                    group['hiddenSourceRaceIds'].append(source)
                for target in row['targetRaceIds']:
                    _index_last(group, 'byPair', source + '_' + target, row['rowId'], 'historyByPair')
    return group


def parse_carries(records, race_expansions=None):
    group = {'rows': [], 'byPair': {}, 'historyByPair': {}, 'bySourceRaceId': {},
             'byDeclaredSourceRaceId': {}, 'unresolvedExpansion': [],
             'indexRule': 'first c/ced token, then official getAllSkinAndPetRaceIds; later row wins'}
    expansions = (race_expansions or {}).get('byRaceId', {})
    if expansions and not (race_expansions or {}).get('sourceEvidence'):
        raise ValueError('Carry race expansions require explicit official source evidence')
    group['expansionEvidence'] = (race_expansions or {}).get('sourceEvidence', [])
    if not isinstance(expansions, dict) or any(not isinstance(value, list) for value in expansions.values()):
        raise ValueError('Official carry expansions must be arrays keyed by seed race ID')
    for original in _rows(records, 'pet'):
        attrs = original['rawAttributes']
        carrier = str(integer(attrs.get('c', '').split('#')[0]))
        carried = str(integer(attrs.get('ced', '').split('#')[0]))
        source_ids = [str(integer(x)) for x in expansions.get(carrier, [])]
        target_ids = [str(integer(x)) for x in expansions.get(carried, [])]
        if any(int(x) <= 0 for x in source_ids + target_ids):
            raise ValueError('Invalid official carry expansion')
        row = {**original, 'rowId': len(group['rows']), 'carrierRaceId': carrier,
               'carriedPetRaceId': carried, 'sourceRaceIds': source_ids, 'targetRaceIds': target_ids,
               'type': integer(attrs.get('type'), -1), 'battlePowerRate': number(attrs.get('bpr')),
               'gimmick': attrs.get('gimmick', ''),
               'expansionStatus': 'resolved' if carrier in expansions and carried in expansions else 'unresolved'}
        _display(row, descriptions={'skillDescription': attrs.get('sdes', '')},
                 readings=[('配置携带者种族', carrier), ('配置携带对象种族', carried),
                           ('携同类别', {4: '神使', 8: '召唤兽', 16: '不朽书灵'}.get(row['type'], str(row['type'])))])
        group['rows'].append(row)
        group['byDeclaredSourceRaceId'].setdefault(carrier, []).append(row['rowId'])
        if row['expansionStatus'] != 'resolved':
            group['unresolvedExpansion'].append({'rowId': row['rowId'], 'carrierRaceId': carrier,
                                                'carriedPetRaceId': carried,
                                                'reason': 'officialSkinAndMultipleFormExpansionUnavailable'})
            continue
        for source in source_ids:
            group['bySourceRaceId'].setdefault(source, []).append(row['rowId'])
            for target in target_ids:
                _index_last(group, 'byPair', source + '_' + target, row['rowId'], 'historyByPair')
    return group


def parse_almighty(records):
    group = {'rows': [], 'byRaceId': {}, 'historyByRaceId': {}, 'indexRule': 'first ID row wins'}
    for original in _rows(records, 'pet'):
        attrs = original['rawAttributes']
        row = {**original, 'rowId': len(group['rows']), 'raceId': str(integer(attrs['id'])),
               'psychicsCalledPetRaceId': str(integer(attrs['psci']))}
        for field, attribute in [('heroSkillName', 'hn'), ('heroSkillDesc', 'hd'),
                                 ('summonSkillName', 'sn'), ('summonSkillDesc', 'sd'),
                                 ('psychicsSkillName', 'pn'), ('psychicsSkillDesc', 'pd')]:
            row[field] = attrs.get(attribute, '')
        _display(row, descriptions={field: row[field] for field in ('heroSkillDesc', 'summonSkillDesc', 'psychicsSkillDesc')},
                 readings=[('英雄技', row['heroSkillName']), ('召唤技', row['summonSkillName']),
                           ('通灵技', row['psychicsSkillName']), ('通灵召唤种族', row['psychicsCalledPetRaceId'])])
        group['rows'].append(row)
        group['historyByRaceId'].setdefault(row['raceId'], []).append(row['rowId'])
        group['byRaceId'].setdefault(row['raceId'], row['rowId'])
    return group


HERO_FIELDS = ['skillId', 'zhandouli1', 'zhandouli2', 'type', 'cls', 'clsParams',
               'isShow', 'preCls', 'preClsParams', 'preIncludeSummonPet', 'mutilIndex']


def parse_formation_skills(text, evidence, kind, skills=None):
    constructor = 'HeroSkillDefine' if kind == 'hero' else 'PsychicsSkillDefine'
    values, offset = declaration(text, 'defines', [constructor])
    if not isinstance(values, list):
        raise ValueError('Expected official skill definition array')
    group = {'rows': [], 'bySkillId': {}, 'indexRule': 'all matching ID rows in source order',
             'scope': 'formation conditions and description metadata; never battle activation'}
    for value in values:
        if not isinstance(value, dict) or value.get('constructor') != constructor:
            raise ValueError('Unexpected formation skill definition')
        args = value['arguments']
        if not 7 <= len(args) <= 11:
            raise ValueError('Unsupported formation skill constructor arity')
        params = args + [None, None, None, 0][len(args) - 7:]
        fields = dict(zip(HERO_FIELDS, params))
        fields['skillId'] = str(integer(fields['skillId']))
        fields['isShow'] = bool(integer(fields['isShow']))
        for key in ['zhandouli1', 'zhandouli2', 'type', 'mutilIndex']:
            fields[key] = number(fields[key])
        for key in ['cls', 'clsParams', 'preCls', 'preClsParams', 'preIncludeSummonPet']:
            if fields[key] is not None and not isinstance(fields[key], str):
                raise ValueError('Nonliteral official condition: ' + key)
        if kind == 'psychics':
            fields['psychicsCalledPetRaceId'] = str(integer(fields.pop('mutilIndex')))
        row = {**fields, 'rowId': len(group['rows']), 'rawParameters': args,
               'rawExpression': value['rawExpression'],
               'source': {**evidence, 'row': len(group['rows']) + 1,
                          'line': text.count('\n', 0, offset + value['offset']) + 1}}
        skill = (skills or {}).get(row['skillId'], {})
        _display(row, skill.get('name', ''), {'skillDescription': skill.get('description', '')},
                 [('效果对象规则', row['cls']), ('效果对象参数', row['clsParams']),
                  ('前置条件规则', row['preCls']), ('前置条件参数', row['preClsParams']),
                  ('前置条件召唤计入参数', row['preIncludeSummonPet'])])
        row['descriptionReference'] = {'table': 'skills', 'id': row['skillId']}
        group['rows'].append(row)
        group['bySkillId'].setdefault(row['skillId'], []).append(row['rowId'])
    return group


def _nested(value, constructor, fields):
    if not isinstance(value, dict) or value.get('constructor') != constructor or len(value['arguments']) != len(fields):
        raise ValueError('Unsupported nested formation definition')
    return dict(zip(fields, value['arguments']))


def parse_formation_teams(text, evidence):
    allowed = ['FormationTeamBuff', 'FormationTeamBuffEffect', 'FormationTeamBuffFormationEffect']
    value, offset = declaration(text, 'buffs', allowed)
    if not isinstance(value, dict) or 'objectEntries' not in value:
        raise ValueError('Expected official formation team object')
    group = {'rows': [], 'byId': {}, 'historyById': {},
             'namespace': 'formationTeamBuff; not battle Buff IDs', 'indexRule': 'last object key wins'}
    for key, item in value['objectEntries']:
        if not isinstance(item, dict) or item.get('constructor') != 'FormationTeamBuff':
            raise ValueError('Unsupported formation team row')
        args = item['arguments']
        if not 4 <= len(args) <= 6:
            raise ValueError('Unsupported formation team constructor arity')
        params = args + [None, 0][len(args) - 4:]
        identifier, name, pet_array, effects, formation_effects, effect_index = params
        if str(integer(key)) != str(integer(identifier)):
            raise ValueError('Formation team key/ID disagreement')
        if not isinstance(name, str) or not isinstance(pet_array, list) or not isinstance(effects, list):
            raise ValueError('Unsupported formation team field')
        races = [[str(integer(race)) for race in alternatives] for alternatives in pet_array]
        effect_rows = [_nested(x, 'FormationTeamBuffEffect', ['petNum', 'effect']) for x in effects]
        formation_rows = [_nested(x, 'FormationTeamBuffFormationEffect', ['formationId', 'petNum', 'effect'])
                          for x in (formation_effects or [])]
        row = {'rowId': len(group['rows']), 'id': str(integer(identifier)), 'petAlternatives': races,
               'effects': effect_rows, 'formationEffects': formation_rows, 'effectIndex': integer(effect_index),
               'rawParameters': args, 'rawExpression': item['rawExpression'],
               'source': {**evidence, 'row': len(group['rows']) + 1,
                          'line': text.count('\n', 0, offset + item['offset']) + 1}}
        _display(row, name, readings=[('组合精灵种族（每组为备选）', races), ('官方效果档位', effect_rows),
                                     ('阵型条件', formation_rows)])
        group['rows'].append(row)
        _index_last(group, 'byId', row['id'], row['rowId'], 'historyById')
    return group


def parse_resources(documents, consumers=None):
    resources = {}
    for kind, records in documents.items():
        group = {'rows': [], 'byId': {}, 'historyById': {}, 'scope': 'resource metadata only'}
        if kind == 'background':
            group['byScene'] = {}
        elif kind == 'preDownload':
            group['byMode'] = {}
        for record in records:
            if not record['rawAttributes']:
                continue
            attrs, path = record['rawAttributes'], record['source']['path']
            row = {**record, 'rowId': len(group['rows']), 'runtimeEligible': True, 'excludedReason': ''}
            key = None
            if kind in ('avatar', 'actionMovie'):
                key = str(integer(attrs['id']))
                row['id'] = key
                row['runtimeEligible'] = bool(re.search(r'/config\[\d+\]/s\[\d+\]$', path))
            elif kind == 'changeAvatar':
                # The retained official consumer selects background.s, while
                # the current XML contains change.s. Do not silently repair it.
                row['id'] = str(integer(attrs['id']))
                row['runtimeEligible'] = bool(re.search(r'/background\[\d+\]/s\[\d+\]$', path))
                if row['runtimeEligible']:
                    key = row['id']
                else:
                    row['excludedReason'] = 'outsideOfficialBackgroundSelector'
            elif kind == 'preDownload':
                row['resource'] = attrs.get('file', '')
                row['mode'] = 'sequential' if '/seq[' in path else 'common'
                if row['mode'] == 'sequential' or row['resource'] in ('battle/catch_res', 'battle/gainexp_res'):
                    row['runtimeEligible'] = False
                    row['excludedReason'] = 'excludedBattleModeOrRewardResource'
            elif kind == 'background':
                row['scene'] = attrs.get('scene', '')
                row['runtimeEligible'] = '/background[' in path
                if not row['runtimeEligible']:
                    row['excludedReason'] = 'rewardBackgroundMetadataOnly'
            _display(row, readings=[('资源字段', attrs)])
            group['rows'].append(row)
            if kind == 'background' and row['runtimeEligible']:
                group['byScene'].setdefault(row['scene'], []).append(row['rowId'])
            if kind == 'preDownload':
                group['byMode'].setdefault(row['mode'], []).append(row['rowId'])
            if key is not None and row['runtimeEligible']:
                _index_last(group, 'byId', key, row['rowId'], 'historyById')
        group['rawRowCount'] = len(group['rows'])
        group['runtimeEligibleRows'] = sum(row['runtimeEligible'] for row in group['rows'])
        resources[kind] = group
    return resources


def parse_version_rules(version_text, version_evidence, loader_text, loader_evidence):
    require(version_text, 'DEFAULT_VERSION:String = "2000";',
            'for each(_loc4_ in param3.f)', 'versionMap[_loc4_.@n] = _loc4_.@v;',
            'setTempVersion(param2);', 'for each(_loc2_ in param1.u.f)',
            'versionMap[_loc2_.@n] = _loc2_.@v;',
            'if(version.length == 0){return "";}',
            'if(EXTENSION_NAMES.indexOf(param2) != -1)',
            'return _loc3_ + DEFAULT_VERSION;', 'return "~" + version;',
            'var _loc4_:String = getVersion(param1,param2);',
            'param1 = testFileMap[param1];', '_loc5_ = param1 + _loc4_ + "." + param2;')
    require(loader_text, 'this.version = this.versionXML.v;',
            'new URLRequest("versiondata~" + this.version + ".swf")',
            'var _loc4_:XML = new XML(_loc3_["vconfig"]);',
            'VersManager.setVersionXml(this.version,this.versionXML,_loc4_);')
    extensions, _ = declaration(version_text, 'EXTENSION_NAMES', [])
    if not isinstance(extensions, list) or any(not isinstance(value, str) for value in extensions):
        raise ValueError('Invalid official version-extension list')
    return {'status': 'verified-consumer', 'urlTemplate': '{resource}{versionSuffix}.{extension}',
            'relativeTo': 'official loader URL base', 'defaultExtension': 'swf',
            'versionMapExtensions': extensions, 'defaultMappedVersion': '2000',
            'versionMapOrder': ['versiondata.DocClass.vconfig.f', 'start.xml.u.f (last wins; refreshed by setTempVersion)'],
            'branches': [{'when': 'start.v is empty', 'suffix': ''},
                         {'when': 'extension in versionMapExtensions and map has nonempty value', 'suffix': '~{mappedVersion}'},
                         {'when': 'extension in versionMapExtensions and map has no nonempty value', 'suffix': '~2000'},
                         {'when': 'extension not in versionMapExtensions', 'suffix': '~{start.v}'}],
            'zoneOverride': 'resolve version against original resource, then start.u.f rn/zone test override may replace resource path',
            'aqzVersionRule': 'start.v' if 'aqz' not in extensions else 'mapped-or-default',
            'source': _at(version_text, version_evidence, 'private static function getVersion('),
            'consumerEvidence': [_at(version_text, version_evidence, 'public static function setVersionXml('),
                                 _at(version_text, version_evidence, 'public static function setTempVersion('),
                                 _at(version_text, version_evidence, 'public static function getVerURLRequest('),
                                 _at(loader_text, loader_evidence, 'private function onLoadBytesSuccess(')]}


def parse_resource_loading(fighter, background, downloader, version_rules=None):
    """Record reviewed resource-template branches, never enumerate guesses."""
    ft, fi = fighter
    bt, bi = background
    dt, di = downloader
    group = {'rows': [], 'byId': {}, 'scope': 'official resource metadata; no asset availability or battle-cause claim',
             'versionRules': version_rules or {'status': 'unavailable', 'reason': 'verifiedLoaderSourceNotProvided'}}

    def constant(text, evidence, name):
        pattern = r'\bconst\s+' + re.escape(name) + r'\s*:\s*String\s*=\s*("(?:\\.|[^"\\])*")\s*;'
        matches = list(re.finditer(pattern, text))
        if len(matches) != 1:
            raise ValueError('Missing or ambiguous official resource constant: ' + name)
        value = json.loads(matches[0][1])
        return value, {**evidence, 'line': text.count('\n', 0, matches[0].start()) + 1,
                       'statement': matches[0][0]}

    def add(identifier, label, source, constant_name, template_suffix, field, condition, statements,
            *, extension='swf', format_fields=None, eligible=True):
        text, info = source
        prefix, location = constant(text, info, constant_name)
        if format_fields is not None:
            template = prefix
            if template.count('%d') != len(format_fields):
                raise ValueError('Changed official resource format: ' + constant_name)
            for name in format_fields:
                template = template.replace('%d', '{' + name + '}', 1)
        else:
            template = prefix + template_suffix
        row = {'rowId': len(group['rows']), 'id': identifier, 'namespace': prefix.split('/')[0],
               'template': template, 'extension': extension, 'keyField': field,
               'condition': condition, 'runtimeEligible': eligible,
               'excludedReason': '' if eligible else 'excludedOpeningItemOrRewardPresentation',
               'rawConstant': prefix, 'source': location,
               'consumerEvidence': [_at(text, info, statement) for statement in statements],
               'versionRulesPath': '/resources/loading/versionRules'}
        _display(row, descriptions={'consumerRule': condition}, readings=[('资源模板', template), ('官方取值字段', field),
                  ('使用分支', condition), ('文件类型', extension)])
        _generated_label(row, label + ' · 官方资源模板')
        group['rows'].append(row)
        group['byId'][identifier] = row['rowId']

    add('action-swf', '精灵动作', fighter, 'ACTION_FILE_PREFIX', '{faceId}', 'FighterInfo.faceId / transform.curFaceId',
        '动作配置缺失、主动作非 Spine，或任一前景/背景片段非 Spine 时，保留 SWF 动作。',
        ['this.addActionFile(fighterInfo.faceId,actionFileList);', 'this.addActionFile(transform.curFaceId,actionFileList);',
         'this.addToArrayHelper(param2,this.ACTION_FILE_PREFIX + param1);'])
    add('action-aqz', '精灵 Spine 动作', fighter, 'SPINE_ACTION_FILE', '', 'ActionMovieDefine key = faceId',
        'ActionMovieDefine.isSpine 为真。', ['if(_loc4_.isSpine)', 'StringUtil.format(this.SPINE_ACTION_FILE,param1)'],
        extension='aqz', format_fields=['faceId'])
    for side in ('foreground', 'background'):
        add('action-' + side, '动作' + ('前景' if side == 'foreground' else '背景'), fighter,
            'SPINE_' + side.upper() + '_FILE', '', 'faceId + ActionMovieDefine.' + side + '[n][0]',
            '该片段配置的第二项 == 1 时加载 Spine；否则主 SWF 动作已包含这一分支。',
            ['for each(_loc5_ in _loc4_.' + side + ')', '_loc7_ = _loc5_[1] == 1;'],
            extension='aqz', format_fields=['faceId', 'fragmentId'])
    add('skill-swf', '技能效果', fighter, 'EFFECT_FILE_PREFIX', '{skillId}', 'SkillDefine / recorded skill ID',
        '技能不是 Spine 且未切换到简化技能资源；反击及换技沿其记录的技能编号查询。',
        ['this.EFFECT_FILE_PREFIX + transform.normalSkillId'])
    add('skill-aqz', '技能 Spine 效果', fighter, 'SPINE_EFFECT_FILE', '', 'recorded skill ID',
        '对应 SkillDefine.isSpine 为真。', ['StringUtil.format(this.SPINE_EFFECT_FILE,transform.normalSkillId)'],
        extension='aqz', format_fields=['skillId'])
    add('buff-swf', 'Buff 效果', fighter, 'BUFFER_FILE_PREFIX', '{displayId}', 'BufferDefine.displayId',
        '由实际 Buff 编号查询 BufferDefine，再取 displayId；客户端的跳过及简化条件仍适用。',
        ['this.BUFFER_FILE_PREFIX + _loc28_.displayId', 'private function isSkipBuffEffect('])
    add('buff-aqz', 'Buff Spine 效果', fighter, 'SPINE_BUFFER_FILE', '', 'BufferDefine.displayId',
        '对应 BufferDefine.isSpine 为真；编号先通过 BufferDefine 取得 displayId。',
        ['StringUtil.format(this.SPINE_BUFFER_FILE,_loc28_.displayId)'], extension='aqz', format_fields=['displayId'])
    add('full-power', '满气势表现', fighter, 'BUFFER_FILE_PREFIX', '{displayId}',
        'getPowerFullBuffId(faceId) -> BufferDefine.displayId',
        '官方满气势表现 Buff，不是战报附加状态；使用外观 faceId 查询，不回退到 raceId。',
        ['this._config.getPowerFullBuffId(fighterInfo.faceId)', 'pfBuffURL = this.BUFFER_FILE_PREFIX + pwBufferDefine.displayId;'])
    for identifier, const, suffix, ext, fields in [('summon-swf', 'SUMMON_FILE_PREFIX', '{effectId}', 'swf', None),
                                                  ('summon-aqz', 'SPINE_SUMMON_FILE', '', 'aqz', ['effectId'])]:
        add(identifier, '召唤表现', fighter, const, suffix, 'SummonDefine.effect (exact source_originalRaceId × target_originalRaceId)',
            '配对由实际双方 originalRaceId 查询；SWF/Spine 分支看该次召唤的 SkillDefine.isSpine。',
            ['sourceFighter.originalRaceId', 'summonFighter.originalRaceId',
             'this._config.getSummonDefine(sourceRaceId,summonedPetRaceId)'], extension=ext, format_fields=fields)
    add('change-avatar', '换形过渡', fighter, 'CHANGEAVATAR_FILE_PREFIX', '{curFaceId_or__default}', 'transform.curFaceId',
        'curFaceId 在 changeAvatarIds 中才加载具体资源，否则使用 _default。',
        ['if(this._config.changeAvatarIds.indexOf(transform.curFaceId) != -1)',
         'this.CHANGEAVATAR_FILE_PREFIX + "_default"'])
    add('progress-state', '通灵进度表现', fighter, 'BUFFERSTATE_FILE_PREFIX', '{displayId}',
        'player.psychicsBuffData.bufferId -> BufferDefine.displayId',
        '玩家记录存在 psychicsBuffData 时按其 bufferId 查询；资源使用 displayId。',
        ['this._config.getBufferDefineById(_loc25_.bufferId)', 'this.BUFFERSTATE_FILE_PREFIX + _loc27_.displayId'])
    add('pet-title', '精灵称号表现', fighter, 'PETTITLE_FILE_PREFIX', '{titleId}', 'FighterInfo.petTitleData.getTitleIds()',
        '只加载该精灵记录实际携带的 titleId；演示槽位不代表拥有称号。',
        ['titleIds = fighterInfo.petTitleData.getTitleIds();', 'PETTITLE_FILE_PREFIX + param1'])
    for tag, first in [('normal', '1'), ('super', '2')]:
        add('simple-' + tag, '简化技能表现', fighter, 'SIMPLE_EFFECT_FILE_PREFIX',
            '{close_or_remote}/effect' + first + '00000{c_or_r}{attributeSuffix}',
            'SkillDefine type + main RaceTypeDefine.id % 100 + original skill ID',
            '官方简化分支：close/remote 和 c/r 看技能类型；治疗、缺字典、普通属性或属性尾码不足两位，回原技能资源；本条仅首位 ' + first + ' 的非回退分支。',
            ['var _loc2_:int = int(SkillDefine.getRaceIdBySkillId(param1));',
             'if(param1.toString().charAt(0) == "' + first + '")',
             'SkillDefine.getOrigSkillId(param1)'])
    add('dragon-action', '小神龙动作', fighter, 'ACTION_FILE_PREFIX', '{actionResId}', 'BufferExecuteDefine.actionResId',
        '由实际小神龙执行 Buff 查询 BufferExecuteDefine；不将 actionResId 当精灵 raceId。',
        ['this._config.getBufferExecutDefine(_loc30_)', 'this.ACTION_FILE_PREFIX + _loc31_.actionResId'])
    add('dragon-effect', '小神龙效果', fighter, 'EFFECT_FILE_PREFIX', '{skillId_or_lightedSkillId}',
        'BufferExecuteDefine.skillId / lightedSkillId', '小神龙 isGlittery() 分支使用 lightedSkillId，否则使用 skillId。',
        ['if(_loc23_.isGlittery())', 'this.EFFECT_FILE_PREFIX + _loc31_.lightedSkillId', 'this.EFFECT_FILE_PREFIX + _loc31_.skillId'])
    add('scene', '战斗场景', background, 'BACKGROUND_FILE_PREFIX', '{sceneName_or_default}', 'BattleInfo.sceneName',
        '场景名在 backgroundScenes 中时加载该名称，否则 default。',
        ['var _loc4_:String = _loc3_.sceneName;', 'if(_loc1_.backgroundScenes.indexOf(_loc4_) != -1)'])
    add('equipped-background', '背景道具资料', background, 'NEW_BACKGROUND_FILE_PREFIX', '_{serverId}', 'UseItemResult.backgroundResults[].serverId',
        '仅保留官方原文；开战道具展示不属于此次文字回放范围。',
        ['_loc7_ = _loc6_.backgroundResults;', '_loc5_ = _loc9_.serverId;'], eligible=False)
    require(dt, 'if(VersManager.version.length > 0){this.onLoadDefaultComplete(null);return;}',
            'this.DEFAULT_ACTION_FILE', 'this.DEFAULT_EFFECT_FILE', 'DEFAULT_CHANGEAVATAR_FILE')
    group['downloadFailureRule'] = {
        'description': '有版本号时不进入此默认资源分支；无版本号时，失败的动作/技能/换形/场景资源可进入各自默认资源分支。',
        'source': _at(dt, di, 'private function onLoadError('),
        'scope': 'fallback metadata does not prove any concrete asset exists or downloaded successfully'}
    group['rawRowCount'] = len(group['rows'])
    group['runtimeEligibleRows'] = sum(row['runtimeEligible'] for row in group['rows'])
    return group


def parse_power_full(records, pets, dictionary_text, dictionary_evidence):
    require(dictionary_text, 'if(this._ultSuperSkillNo > 0){ return SUPER_JOB_BUFF; } return -1;')
    match = re.search(r'const\s+SUPER_JOB_BUFF\s*:\s*int\s*=\s*(\d+)\s*;', dictionary_text)
    if not match:
        raise ValueError('Missing official SUPER_JOB_BUFF')
    super_buff = str(integer(match[1]))
    group = {'rows': [], 'byRaceId': {}, 'raceIdOverrides': {}, 'rawMappingRows': [],
             'overrideSources': {}, 'overrideHistory': {},
             'defaultBuffId': None, 'superJobBuffId': super_buff,
             'lookupOrder': ['raceIdOverrides', 'dictionaryFullPowerBuffId != -1', 'defaultBuffId'],
             'scope': 'client full-power visual Buff; not a recorded Buff or a skill activation'}
    for record in records:
        if '/pfmap[' not in record['source']['path']:
            continue
        attrs = record['rawAttributes']
        if record['tag'] == 'def':
            group['defaultBuffId'] = str(integer(attrs['buffId']))
            group['defaultSource'] = record['source']
        elif record['tag'] == 'm':
            group['rawMappingRows'].append(record)
            race = str(integer(attrs['raceId']))
            group['raceIdOverrides'][race] = str(integer(attrs['buffId']))
            group['overrideSources'][race] = record['source']
            group['overrideHistory'].setdefault(race, []).append(len(group['rawMappingRows']) - 1)
    if group['defaultBuffId'] is None:
        raise ValueError('Missing default full-power Buff')
    for race, pet in pets.items():
        dictionary_buff = super_buff if integer(pet.get('ultSuperSkillNo'), 0) > 0 else '-1'
        branch = 'explicit-race-map' if race in group['raceIdOverrides'] else 'pet-dictionary' if dictionary_buff != '-1' else 'default'
        effective = group['raceIdOverrides'].get(race, dictionary_buff if dictionary_buff != '-1' else group['defaultBuffId'])
        row = {'rowId': len(group['rows']), 'raceId': race, 'dictionaryFullPowerBuffId': dictionary_buff,
               'buffId': effective, 'branch': branch, 'source': dictionary_evidence,
               'dictionarySource': pet.get('source', {}), 'ultSuperSkillNo': pet.get('ultSuperSkillNo', 0)}
        _display(row, readings=[('满气势视觉 Buff', effective), ('官方查询分支', branch)])
        group['rows'].append(row)
        group['byRaceId'][race] = row['rowId']
    return group


def parse_titles(demo_text, demo_evidence, data_text, service_text):
    require(data_text, 'if(params["title"] is Array)', 'petTitleArr = [params["title"]];',
            'var _loc3_:int = int(param1["titleId"]);', 'data._titleIdMap[_loc3_] = param1;',
            'this._titleIdMap[param1]["content"]', 'this._titleIdMap[param1]["extId"]')
    require(service_text, 'var movieUrl:String = "pettitle/movie/pettitlemovie" + titleId;',
            'var movieCls:String = "mmo.pettitle.movie.TitleMovie" + titleId;')
    values, offset = declaration(demo_text, 'BATTLE_DEMOS', [])
    if not isinstance(values, dict) or 'objectEntries' not in values:
        raise ValueError('Unsupported official title demo declaration')
    group = {'rows': [], 'byId': {}, 'historyById': {}, 'status': 'record-schema-and-demo-references',
             'namespace': 'mmo.interfaces.pettitle.PetTitleData', 'definitionStatus': 'unavailable',
             'reason': 'service provides movies and demo slots, no title name/description definitions',
             'scope': 'demo IDs are not proof of ownership or title activation in another battle',
             'recordSchema': {'field': 'title', 'shape': 'object-or-array', 'idField': 'titleId',
                              'valuesField': 'content', 'extensionField': 'extId',
                              'duplicateRule': 'last recorded titleId wins'},
             'resourceTemplate': 'pettitle/movie/pettitlemovie{titleId}',
             'resourceClassTemplate': 'mmo.pettitle.movie.TitleMovie{titleId}',
             'rawSource': demo_evidence}
    for index, (key, value) in enumerate(values['objectEntries']):
        identifier = str(integer(key))
        if value is not None and (not isinstance(value, dict) or 'objectEntries' not in value):
            raise ValueError('Unsupported official title demo value')
        row = {'rowId': len(group['rows']), 'titleId': identifier, 'declarationKind': 'demo-slot',
               'demoAvailable': value is not None, 'definitionAvailable': False,
               'resource': group['resourceTemplate'].format(titleId=identifier),
               'resourceClass': group['resourceClassTemplate'].format(titleId=identifier),
               'source': {**demo_evidence, 'row': index + 1,
                          'line': demo_text.count('\n', 0, offset + values['entryOffsets'][index]) + 1}}
        _display(row, readings=[('称号记录编号', identifier), ('官方演示配置', '有' if value is not None else '空'),
                               ('官方名称及说明', '当前服务未提供定义')])
        group['rows'].append(row)
        _index_last(group, 'byId', identifier, row['rowId'], 'historyById')
    return group


def attach_title_resource_verification(group, verification, evidence):
    """Attach exact title-ID inspection with fresh byte/hash checks, not names."""
    entries = verification.get('byTitleId')
    if not isinstance(entries, dict):
        raise ValueError('Invalid title resource verification')
    group['resourceVerificationSource'] = evidence
    group['resourceVerificationScope'] = verification.get('scope', '')
    group['resourceVerificationIssues'] = []
    group['reason'] = 'service provides movies and demo slots, no title name/description definitions; independent resource verification incomplete'
    sources = []
    for row in group['rows']:
        row['readings'] = [item for item in row['readings'] if item['label'] not in ('官方名称及说明', '独立资源核验')]
        row['readings'].append({'label': '官方名称及说明', 'value': '主服务未提供定义；独立资源尚未通过本次核验'})
        proof = entries.get(row['titleId'])
        if not isinstance(proof, dict):
            row['resourceVerification'] = {'verificationStatus': 'missing', 'reason': 'titleIdNotInResourceInspection'}
            group['resourceVerificationIssues'].append({'titleId': row['titleId'], 'reason': 'missingInspection'})
            continue
        proof = json.loads(json.dumps(proof))
        row['resourceVerification'] = proof
        issues = []
        if proof.get('resource') != row['resource'] or proof.get('className') != row['resourceClass']:
            issues.append('resourceOrClassDoesNotMatchOfficialTitleId')
        if proof.get('status') != 'downloaded':
            issues.append('downloadNotConfirmed')
        expected_sources = [proof] + proof.get('sourceEvidence', [])
        for entry in expected_sources:
            if not isinstance(entry, dict) or not entry.get('file') or not entry.get('sha256'):
                issues.append('missingSourcePathOrHash')
                continue
            path = Path(entry['file'])
            if not path.is_file():
                issues.append('sourceFileMissing:' + str(path))
                continue
            actual = _evidence(path)
            if actual['sha256'] != entry['sha256'] or ('bytes' in entry and actual['bytes'] != entry['bytes']):
                issues.append('sourceFileHashOrSizeMismatch:' + str(path))
                continue
            sources.append(actual)
        proof['verificationStatus'] = 'failed' if issues else 'verified'
        proof['verificationIssues'] = issues
        if issues:
            group['resourceVerificationIssues'].append({'titleId': row['titleId'], 'issues': issues})
        semantic_status = proof.get('metadataStatus') == 'noSemanticDefinitionInInspectedExports'
        if not issues and semantic_status:
            row['readings'] = [item for item in row['readings'] if item['label'] != '官方名称及说明']
            row['readings'].append({'label': '官方名称及说明',
                                    'value': '已查主服务及对应独立资源；当前可读导出未取得名称说明'})
        row['readings'].append({'label': '独立资源核验', 'value': '已校验文件及导出哈希' if not issues else '来源缺失或校验失败，不能确认'})
    group['unmatchedResourceVerificationIds'] = sorted(set(entries) - set(group['byId']), key=integer)
    group['verifiedResourceCount'] = sum(row.get('resourceVerification', {}).get('verificationStatus') == 'verified'
                                          for row in group['rows'])
    if group['rows'] and group['verifiedResourceCount'] == len(group['rows']):
        group['reason'] = 'service and exact independent resources inspected; current readable exports supply no title name/description definitions'
    return sources


def link_supplements(result, skills, buffs, pets):
    relations, unresolved = [], []
    targets = {'skills': skills, 'buffs': buffs, 'pets': pets}

    def link(group, row, field, target, identifier):
        identifier = str(identifier)
        if not re.fullmatch(r'\d+', identifier) or int(identifier) <= 0:
            return
        relation = {'fromTable': group, 'rowId': row['rowId'], 'field': field,
                    'targetTable': target, 'targetId': identifier,
                    'sourcePath': f'/{group}/rows/{row["rowId"]}/source',
                    'status': 'resolved' if identifier in targets[target] else 'missing',
                    'scope': 'official configuration reference; does not prove a battle cause'}
        relations.append(relation)
        if relation['status'] != 'resolved':
            unresolved.append(relation.copy())

    for group in ('summon', 'carry'):
        for row in result[group]['rows']:
            for field in ('sourceRaceIds', 'targetRaceIds'):
                for race in dict.fromkeys(row[field]):
                    link(group, row, field, 'pets', race)
            if group == 'carry':
                for field in ('carrierRaceId', 'carriedPetRaceId'):
                    link(group, row, field, 'pets', row[field])
    for row in result['almighty']['rows']:
        for field in ('raceId', 'psychicsCalledPetRaceId'):
            link('almighty', row, field, 'pets', row[field])
    for group in ('hero', 'psychics'):
        for row in result[group]['rows']:
            link(group, row, 'skillId', 'skills', row['skillId'])
            if group == 'psychics':
                link(group, row, 'psychicsCalledPetRaceId', 'pets', row['psychicsCalledPetRaceId'])
    for row in result['formationTeams']['rows']:
        for race in dict.fromkeys(race for choices in row['petAlternatives'] for race in choices):
            link('formationTeams', row, 'petAlternatives', 'pets', race)
        # FormationTeamBuff.buffId and battle Buff.id are different namespaces.
    for row in result['powerFull']['rows']:
        link('powerFull', row, 'buffId', 'buffs', row['buffId'])
    unresolved.extend({'fromTable': 'carry', **row} for row in result['carry']['unresolvedExpansion'])
    result['relations'], result['unresolved'] = relations, unresolved


def extract_replay_supplements(source, auxiliary, *, formation_source=None,
                               title_source=None, skills=None, buffs=None, pets=None,
                               race_expansions=None, loader_source=None):
    source, auxiliary = Path(source), Path(auxiliary)
    formation_source = Path(formation_source) if formation_source else auxiliary
    roots = [source, auxiliary, formation_source]
    evidence = {}
    for entry in verify_race_expansions(race_expansions):
        evidence[entry['file']] = entry

    def read(path):
        info = _evidence(path)
        evidence[str(path)] = info
        return Path(path).read_text(encoding='utf-8-sig'), info

    def script(relative):
        return read(_find(roots, relative))

    def document(name, suffix='xmlClass'):
        text, info = read(_xml_file(source, name, suffix))
        return xml_records(text, info)

    summon, _ = script('interfaces/scripts/mmo/interfaces/battleconfig/define/SummonDefine.as')
    require(summon, 'String(params.@s).split("#")', 'String(params.@sed).split("#")',
            'define._summonerRaceId = summonerRaceId;', 'define._summonedPetRaceId = summonedPetRaceId;',
            '_loc2_._effect = int(param1.@effect);', '_loc2_._skillDescription = String(param1.@sdes);')
    consumer, _ = script('battleconfig/scripts/mmo/battleconfig/data/SummonConfigXML.as')
    require(consumer, 'this.parseXMLConfig(this._xmlClass); this.parseHideConfig();',
            'this.parseXMLConfig(this._hideConfigXmlClass);',
            'this._raceId2Define.add(_loc5_.summonerRaceId + "_" + _loc5_.summonedPetRaceId,_loc5_);')
    carry, _ = script('interfaces/scripts/mmo/interfaces/battleconfig/define/CarryDefine.as')
    require(carry, 'parseInt(String(param1.@c).split("#")[0])', 'parseInt(String(param1.@ced).split("#")[0])',
            'PetRaceUtil.getAllSkinAndPetRaceIds([_loc3_])', 'PetRaceUtil.getAllSkinAndPetRaceIds([_loc4_])',
            'ArrayTypeNames:Array = ["神使","召唤兽","不朽书灵"]')
    consumer, _ = script('battleconfig/scripts/mmo/battleconfig/data/CarryConfigXML.as')
    require(consumer, 'this._raceId2Define.add(_loc5_.carrierRaceId + "_" + _loc5_.carriedPetRaceId,_loc5_);')
    consumer, _ = script('battleconfig/scripts/mmo/battleconfig/data/AlmightyConfigXML.as')
    require(consumer, 'if(!this._id2Define.containsKey(_loc4_.id)){this._id2Define.add(_loc4_.id,[_loc4_]);}')
    definition, _ = script('interfaces/scripts/mmo/interfaces/battleconfig/define/AlmightySkillDefine.as')
    require(definition, '_loc2_._id = parseInt(param1.@id);', '_loc2_._psychicsCalledPetRaceId = parseInt(param1.@psci);')
    result = {'schemaVersion': 1,
              'scope': 'official client reference metadata; actual targets, causes and activations require battle records',
              'summon': parse_summons(document('SummonConfigXML'), document('SummonConfigXML', 'hideConfigXmlClass')),
              'carry': parse_carries(document('CarryConfigXML'), race_expansions),
              'almighty': parse_almighty(document('AlmightyConfigXML'))}
    hero_definition, _ = script('interfaces/scripts/mmo/interfaces/formation/HeroSkillDefine.as')
    for field, parameter in zip(['_id', '_zhandouli1', '_zhandouli2', '_type', '_cls', '_clsParams',
                               '_preCls', '_preClsParams', '_preIncludeSummonPet', '_mutilIndex'],
                              [1, 2, 3, 4, 5, 6, 8, 9, 10, 11]):
        require(hero_definition, f'this.{field} = param{parameter};')
    require(hero_definition, 'this._isShow = Boolean(param7);')
    psychics_definition, _ = script('interfaces/scripts/mmo/interfaces/formation/PsychicsSkillDefine.as')
    require(psychics_definition, 'this._nPsychicsCalledPetRaceId = param11;',
            'super(param1,param2,param3,param4,param5,param6,param7,param8,param9,param10);')
    for group, relative in [('hero', 'formation/scripts/mmo/formation/config/HeroSkillConfig.as'),
                            ('psychics', 'formation/scripts/mmo/formation/psychicsskill/PsychicsSkillConfig.as')]:
        text, info = script(relative)
        require(text, 'if(defines[_loc3_].id == param1){_loc2_.push(_loc4_);}')
        result[group] = parse_formation_skills(text, info, group, skills)
    require(hero_definition, 'parseInt(String(this._clsParams).split(",")[0])',
            'PetRaceUtil.getAllSkinAndPetRaceIds([_loc1_]);', 'return _loc2_.join(",");')
    reader, reader_info = script('formation/scripts/mmo/formation/FormationManager.as')
    add_hero_description_readings(result['hero'], reader, reader_info, race_expansions)
    for name, fragments in {
        'FormationTeamBuff': ['this.buffId = param1;', 'this.buffName = param2;', 'this.petArray = param3;',
                             'this.effectArray = param4;', 'this.formationEffectArray = param5;', 'this.effectIndex = param6;'],
        'FormationTeamBuffEffect': ['this.petNum = param1;', 'this.effect = param2;'],
        'FormationTeamBuffFormationEffect': ['this.formationId = param1;', 'this.petNum = param2;', 'this.effect = param3;'],
    }.items():
        text, _ = script('interfaces/scripts/mmo/interfaces/formation/' + name + '.as')
        require(text, *fragments)
    text, info = script('formation/scripts/mmo/formation/config/FormationTeamBuffConfig.as')
    result['formationTeams'] = parse_formation_teams(text, info)
    documents = {}
    for key, name in [('avatar', 'BattleAvatarConfigXML'), ('changeAvatar', 'BattleChangeAvatarConfigXML'),
                      ('actionMovie', 'ActionMovieConfigXML'), ('preDownload', 'PreDownloadConfigXML'),
                      ('background', 'BattleBgConfigXML')]:
        documents[key] = document(name)
        consumer, _ = script('battleconfig/scripts/mmo/battleconfig/data/' + name + '.as')
        if key == 'changeAvatar':
            require(consumer, 'for each(_loc2_ in _loc1_.background.s)')
        elif key == 'avatar':
            require(consumer, 'for each(_loc2_ in _loc1_.s)', 'this._id2define.add(_loc3_.Id,_loc3_);')
        elif key == 'actionMovie':
            require(consumer, 'for each(_loc2_ in _loc1_.s)', 'this._id2define[_loc3_.id] = _loc3_;')
        elif key == 'preDownload':
            require(consumer, 'for each(_loc2_ in _loc1_.common.f)', 'for each(_loc3_ in _loc1_.seq.f)')
        elif key == 'background':
            require(consumer, 'for each(_loc2_ in _loc1_.background.s)', 'this._scenes.push(String(_loc2_.@scene));')
    result['resources'] = parse_resources(documents)
    loader_paths = None
    if loader_source is not None:
        loader_paths = [_find([loader_source], 'scripts/mmo/loader/scheduler/VersManager.as'),
                        _find([loader_source], 'scripts/mmo/loader/LoaderContainer.as')]
    else:
        audit_path = auxiliary / 'cross-resource-audit-20261009/consumer/consumer-audit.json'
        if audit_path.is_file():
            audit_text, _ = read(audit_path)
            sources = json.loads(audit_text)['sources']
            audited = [sources['versionManager'], sources['loaderVersion']]
            if all(Path(item['file']).is_file() for item in audited):
                for item in audited:
                    if _evidence(item['file'])['sha256'] != item['sha256']:
                        raise ValueError('Audited loader source hash mismatch: ' + item['file'])
                loader_paths = [Path(item['file']) for item in audited]
    version_rules = None
    if loader_paths:
        version_text, version_info = read(loader_paths[0])
        loader_text, loader_info = read(loader_paths[1])
        version_rules = parse_version_rules(version_text, version_info, loader_text, loader_info)
    result['resources']['loading'] = parse_resource_loading(
        script('battle/scripts/mmo/battle/command/playbattle/DownloadFighterCommand.as'),
        script('battle/scripts/mmo/battle/command/playbattle/DownloadBackgroundCommand.as'),
        script('battle/scripts/mmo/battle/command/playbattle/RealDownloadCommand.as'), version_rules)
    dictionary, info = script('pet-interface/scripts/mmo/interfaces/pet/data/PetDictionaryDataItem.as')
    consumer, _ = script('battleconfig/scripts/mmo/battleconfig/data/BufferConfigXML.as')
    require(consumer, 'for each(_loc3_ in _loc1_.pfmap.m)',
            '_loc5_ = int(_loc3_.@raceId);', '_loc6_ = int(_loc3_.@buffId);',
            'this._defaultPowerFullBuffId = int(_loc1_.pfmap.def.@buffId);')
    result['powerFull'] = parse_power_full(document('BufferConfigXML'), pets or {}, dictionary, info)
    fighter_text, fighter_info = script('battle/scripts/mmo/battle/fighter/BattleFighter.as')
    require(fighter_text, 'this._battleConfig.getPowerFullBuffId(this._info.faceId)',
            'if(param1 >= this._info.maxPower)', 'this._avatar.addBuff(_loc2_);',
            'this._avatar.removeBuff(_loc2_);')
    result['powerFull'].update(lookupKey='FighterInfo.faceId',
        consumerEvidence=[_at(fighter_text, fighter_info, 'public function set curPower(')],
        lookupScope='byRaceId indexes dictionary keys; caller must use the recorded faceId, never substitute raceId')
    # mmo.config.TitleConfig is the PLAYER title namespace, not PetTitleData.
    # Retain an explicit unavailable state rather than joining coincident IDs.
    result['titles'] = {'rows': [], 'byId': {}, 'status': 'unavailable',
                        'namespace': 'mmo.interfaces.pettitle.PetTitleData',
                        'requiredSources': ['PetTitleData', 'IPetTitleService', 'pettitle definitions'],
                        'reason': 'verifiedPetTitleDefinitionAndConsumerNotProvided'}
    if title_source is not None:
        title_roots = [Path(title_source), Path(title_source).parent]
        demo, demo_info = read(_find(title_roots, 'pettitle/scripts/mmo/pettitle/config/PetTitleBattleDemoConfig.as'))
        data, _ = read(_find(title_roots, 'extra-interfaces/scripts/mmo/interfaces/pettitle/PetTitleData.as'))
        service, _ = read(_find(title_roots, 'pettitle/scripts/mmo/pettitle/PetTitleService.as'))
        result['titles'] = parse_titles(demo, demo_info, data, service)
        title_verification_path = _find(title_roots,
            'identity-reference/pet-title-assets/title-resource-evidence.json', required=False)
        if title_verification_path:
            verification_text, verification_info = read(title_verification_path)
            for checked in attach_title_resource_verification(result['titles'], json.loads(verification_text), verification_info):
                evidence[checked['file']] = checked
    apply_display_labels(result, pets or {})
    link_supplements(result, skills or {}, buffs or {}, pets or {})
    result['sourceEvidence'] = evidence
    result['summary'] = {key + 'Rows': len(result[key]['rows']) for key in
                         ('summon', 'carry', 'almighty', 'hero', 'psychics', 'formationTeams', 'powerFull', 'titles')}
    result['summary'].update(summonPairs=len(result['summon']['byPair']), carryPairs=len(result['carry']['byPair']),
                             heroSkillIds=len(result['hero']['bySkillId']), psychicsSkillIds=len(result['psychics']['bySkillId']),
                             relations=len(result['relations']), unresolved=len(result['unresolved']),
                             resourceLoadingRows=len(result['resources']['loading']['rows']),
                             titleResourcesVerified=result['titles'].get('verifiedResourceCount', 0),
                             sourceFiles=len(evidence),
                             titleDefinitionsAvailable=result['titles'].get('definitionStatus') == 'available')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--auxiliary', type=Path, required=True)
    parser.add_argument('--formation-source', type=Path)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--race-expansions', type=Path)
    parser.add_argument('--title-source', type=Path)
    parser.add_argument('--loader-source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding='utf-8-sig'))
    expansions = json.loads(args.race_expansions.read_text(encoding='utf-8-sig')) if args.race_expansions else None
    result = extract_replay_supplements(args.source, args.auxiliary, formation_source=args.formation_source,
                                       skills=catalog['skills'], buffs=catalog['buffs'], pets=catalog['pets'],
                                       race_expansions=expansions, title_source=args.title_source,
                                       loader_source=args.loader_source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result['summary'], ensure_ascii=False))


if __name__ == '__main__':
    main()
