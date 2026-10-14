"""Read opportunity rules and verify their actual official AS consumers.

Nothing here executes ActionScript. Numeric declarations may change; consumer
functions must still match the reviewed contract before the catalog is replaced.
The review inputs (official URL, SWF SHA and original exports) are recorded in
qa/routine-rules-20261014/official-inputs.json. Tests carry unmodified excerpts.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

VERSION = 1
LIMIT = 2**31 - 1
STAR = "mmo.activityext.newact20220602.starwheeladventure."
COMPETITION = "mmo.activityext.newact20240705.nationalcompetition."
FARM = "mmo.activityext.newact20260522.farmv2."
ARENA = "mmo.arenav3."
SPECS = {
    "starRules": ("newactivityext/newact20220602/starwheeladventure/starwheeladventure", "starAdventure", (
        STAR + "config.SWA_Config", STAR + "model.SWA_Model")),
    "competitionRules": ("newactivityext/newact20240705/nationalcompetition/nationalcompetition", "nationalCompetition", (
        COMPETITION + "config.NC_Config", COMPETITION + "model.NC_Model", COMPETITION + "panel.NC_MainPanel")),
    "farmRules": ("newactivityext/newact20260522/farmv2/farmv2", "farm", (
        FARM + "model.FV2Config", FARM + "model.FV2Model", FARM + "model.FV2Client")),
    "arenaRules": ("arenav3/arenav3service", "arena", (
        ARENA + "config.ArenaV3BattleFieldConfig", ARENA + "model.ArenaV3Model",
        ARENA + "model.ArenaV3FieldInfo", ARENA + "defines.ArenaV3BattleFieldDefine")),
}
STATUSES = {
    "starAdventure.dailyDeclaredLimit": "official-declaration-only-not-used-to-derive-account-counts",
    "starAdventure.weeklyLimit": "consumer-verified",
    "nationalCompetition.dailyBase": "consumer-verified",
    "nationalCompetition.weeklyDeclaredLimit": "official-declaration-only-not-an-observed-weekly-cap",
    "farm.inviteBonus": "consumer-verified-storage-and-daily-replenishment-by-invite-count",
    "arena.zao1": "consumer-verified",
    "arena.zao2": "consumer-verified",
}

# Full function signatures and bodies, tokenized with strings preserved. The
# selected functions, not names alone, establish raw-counter/field/formula roles.
# Filled only from the reviewed official exports, never from update candidates.
CONTRACTS = {'starRules': {'mmo.activityext.newact20220602.starwheeladventure.model.SWA_Model': {'parseGetGameInfo': '21af447a17d997960ca070ca54484d38c51469b0bb6f377b5ed1bc8c6a9a15f2',
                                                                                     'todayRestGameTimes': 'ddc7a17cb44f71f6d41949a8d9c38ffa9c061907b9f630da69308286a78717bf',
                                                                                     'weekRestGameTimes': '47a621ffc35856cc92a5593f4ae058cd28bff698ff5517b402e652117d8020b4',
                                                                                     'weekPlayedTimes': '13d6082d84fd5de9cb703bfecf46eabe36fd31456437ef469c6806ba933d678f'}},
 'competitionRules': {'mmo.activityext.newact20240705.nationalcompetition.model.NC_Model': {'init': 'abc464f12fa55c201afde5cc123f59c901276361935ced0d836ca157a0012ce7',
                                                                                            'buyDailyTicketTimes': '70880c7fa0f80b64ad90d903b79f4ac9270288ed76d1fbc5d668c08d7c1f3f31',
                                                                                            'dailyTicketNum': '929d5971dd2931c94e6762e7d40f9b872fb87351ccf69a7b630ac127a2cb05b6',
                                                                                            'weekTicketNum': '13d6f1f802f9c15301109d6812823854134e1bd80d79f99e0c11eceb1f6c3a7c',
                                                                                            'ticketNum': '335ef7855199f029905272d6b016a9a481cdb0360c3c2a1b8f384a00752debcb'},
                      'mmo.activityext.newact20240705.nationalcompetition.panel.NC_MainPanel': {'updatePanel': '7411262e76701c6f3b4b15a94ddbcd8854c587a429164167c0f84e4688c2b669'}},
 'farmRules': {'mmo.activityext.newact20260522.farmv2.model.FV2Config': {'getInviteBonus': '4f9ce93b742ae75b78e0c79f3539b5a15ff9af830376b9236007daeb57577564'},
               'mmo.activityext.newact20260522.farmv2.model.FV2Model': {'parseData': 'bb02ab47865d66c56b1e9b929931601bf8248683c389af242f3559265afe1e54',
                                                                        'refreshTimes': '5be25eee33242b8a8aa128db6f34b798141feaa0d56f5b35186c9ecf9ef91db2',
                                                                        'refreshTimesMax': 'c3cd3434e5ca5f29e3abb0c726c643fdc0521ca396ba50e63435d725d2a2f63a',
                                                                        'pickTimes': '642b2cd055a211a760896cdb46351ff63e870b1da3d65b2f044e6c3eb6b52ffe',
                                                                        'pickTimesMax': 'a18d33ef017cad21ddfbd3f12bd1e9db764b9b081807c32b311461d13e9f8dc9',
                                                                        'inviteCount': '075786396a3f3522c587de0585e4a4eccc569df052287e576d7f9198171b5adc'},
               'mmo.activityext.newact20260522.farmv2.model.FV2Client': {'getInfo': 'b0d4aa8bb9d730990cb81628bb223d9180cdec890b875c75aa467805f636e7cc',
                                                                         '_parseInviteString': '9d9e4430b80d70979135e04c654bd8e4ad56018d86658218e52f7f82b333ce42'}},
 'arenaRules': {'mmo.arenav3.config.ArenaV3BattleFieldConfig': {'getFieldDefineById': '6a2cb48fc167b0df50dfaa713d06f78221c8e31d295cf4e462a583e2ceee9d10'},
                'mmo.arenav3.model.ArenaV3Model': {'setArenaV3Info': '448b0118a052058efc2a72cdb453c43610056f8063a2f35abdab245b2cd8b9fd'},
                'mmo.arenav3.model.ArenaV3FieldInfo': {'parseInfo': '1671e5ebdd96c91093ad57973e516f6eb28008c1dc1d74f9da828509b2a68b38',
                                                       'fieldDefine': '28db76e2198fdad436d63b0d76c228eb3381fe5039b1679d79b755ee85c7eeed',
                                                       'remainChallengeTime': '155b22e31f43dc53c836c4e593db5f8b2ae081f6ba659867db0311d0c6a86755'},
                'mmo.arenav3.defines.ArenaV3BattleFieldDefine': {'createWithObjArr': '3f615780f86b16e8d2228ee4d8563883bfb4280795aa082286b96e7ddd93f2ba',
                                                                 'createWithObj': 'd720a2e567cb5360dc11834eb3fefb8e61faffbda7880cb29561c36a6c26ba9d',
                                                                 'dailyFreeChallengeTime': 'e8182e7b0d0f6cc8aa7d6180ae121d5cef85cdc1c6e79913dc04511f2298e07b',
                                                                 'id': '4ad6a2b1fef6cc0483a567d9e48045a7eda2443f40becadfc0dfd18759f5ddd9'}}}


def fingerprint() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def digest(value) -> str:
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":")).encode()).hexdigest()


def _tokens(text: str) -> list[str]:
    if not isinstance(text, str) or len(text) > 16 * 1024 * 1024:
        raise ValueError("日常额度脚本过大或类型无效，已保留旧目录")
    pattern = re.compile(r'\s+|//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_$][\w$]*|(?:\d+\.\d+|\d+)|.', re.DOTALL)
    return [token for token in pattern.findall(text)
            if not token.isspace() and not token.startswith(("//", "/*"))]


def _balanced(tokens: list[str], start: int) -> tuple[list[str], int]:
    pairs = {"(": ")", "[": "]", "{": "}"}
    if start >= len(tokens) or tokens[start] not in pairs:
        raise ValueError("日常额度脚本结构无法识别，已保留旧目录")
    stack = []
    for index in range(start, len(tokens)):
        token = tokens[index]
        if token in pairs:
            stack.append(pairs[token])
        elif token in pairs.values():
            if not stack or stack.pop() != token:
                raise ValueError("日常额度脚本括号无效，已保留旧目录")
            if not stack:
                return tokens[start:index + 1], index + 1
        if len(stack) > 64:
            break
    raise ValueError("日常额度脚本不完整，已保留旧目录")


def method(text: str, name: str) -> list[str]:
    tokens = _tokens(text)
    starts = [i for i, token in enumerate(tokens) if token == "function" and
              (tokens[i + 1:i + 3] == [name, "("] or tokens[i + 1:i + 4] == ["get", name, "("])]
    if len(starts) != 1:
        raise ValueError("官方日常额度消费者缺失或重复：" + name)
    start = starts[0]
    try:
        body = tokens.index("{", start)
    except ValueError as error:
        raise ValueError("官方日常额度消费者没有实现：" + name) from error
    _, end = _balanced(tokens, body)
    return tokens[start:end]


def declaration(text: str, name: str) -> list[str]:
    tokens = _tokens(text)
    starts = [i for i in range(len(tokens) - 2) if tokens[i:i + 3] == ["const", name, ":"]]
    if len(starts) != 1:
        raise ValueError("官方日常额度声明缺失或重复：" + name)
    start = starts[0]
    try:
        equal = tokens.index("=", start)
        end = tokens.index(";", equal)
    except ValueError as error:
        raise ValueError("官方日常额度声明不完整：" + name) from error
    return tokens[equal + 1:end]


def _integer(tokens: list[str]) -> int:
    if len(tokens) != 1 or not re.fullmatch(r"0|[1-9][0-9]{0,9}", tokens[0]) or int(tokens[0]) > LIMIT:
        raise ValueError("官方日常额度出现动态数值，已保留旧目录")
    return int(tokens[0])


def _split(tokens: list[str], delimiter: str = ",") -> list[list[str]]:
    result, current = [], []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in ("(", "[", "{"):
            group, index = _balanced(tokens, index)
            current += group
            continue
        if token == delimiter:
            result.append(current)
            current = []
        else:
            current.append(token)
        index += 1
    result.append(current)
    return result


def _object(tokens: list[str]) -> dict[str, list[str]]:
    if not tokens or tokens[0] != "{" or _balanced(tokens, 0)[1] != len(tokens):
        raise ValueError("官方日常额度表不是静态对象，已保留旧目录")
    result = {}
    for entry in _split(tokens[1:-1]):
        if len(entry) < 3 or entry[1] != ":" or not entry[0].startswith('"'):
            raise ValueError("官方日常额度表字段结构变化，已保留旧目录")
        key = json.loads(entry[0])
        if key in result:
            raise ValueError("官方日常额度表字段重复，已保留旧目录")
        result[key] = entry[2:]
    return result


def _array(text: str, name: str) -> list[list[str]]:
    tokens = declaration(text, name)
    if not tokens or tokens[0] != "[" or _balanced(tokens, 0)[1] != len(tokens):
        raise ValueError("官方日常额度表结构变化，已保留旧目录")
    return _split(tokens[1:-1])


def _verify_contracts(label: str, scripts: dict[str, str]) -> dict[str, str]:
    result = {}
    for selected, checks in CONTRACTS[label].items():
        for name, expected in checks.items():
            actual = digest(method(scripts[selected], name))
            if actual != expected:
                raise ValueError("官方日常额度消费规则变化，已保留旧目录：" + selected + "." + name)
            result[selected + "." + name] = actual
    return result


def parse_rules(label: str, scripts: dict[str, str]) -> tuple[dict, dict]:
    if label not in SPECS or set(scripts) != set(SPECS[label][2]):
        raise ValueError("官方日常额度来源不完整，已保留旧目录")
    contracts = _verify_contracts(label, scripts)
    config = scripts[SPECS[label][2][0]]
    if label == "starRules":
        rules = {"dailyDeclaredLimit": _integer(declaration(config, "DAILY_GAME_TIMES")),
                 "weeklyLimit": _integer(declaration(config, "WEEKLY_GAME_TIMES"))}
    elif label == "competitionRules":
        # MAX_TICKET has no live remaining-count consumer in this package. Keep
        # the declaration as evidence, never subtract it from account counters.
        rules = {"dailyBase": _integer(declaration(config, "DAILY_MAX_TICKET")),
                 "weeklyDeclaredLimit": _integer(declaration(config, "MAX_TICKET"))}
    elif label == "farmRules":
        rows = []
        for index, value in enumerate(_array(config, "INVITE_BONUS")):
            fields = _object(value)
            if not {"count", "dailyRefresh", "refreshMax", "dailyPick", "pickMax"} <= set(fields):
                raise ValueError("官方农场邀请档位字段缺失，已保留旧目录")
            row = {key: _integer(fields[key]) for key in ("count", "dailyRefresh", "refreshMax", "dailyPick", "pickMax")}
            if row["count"] != index:
                raise ValueError("官方农场邀请档位不连续，已保留旧目录")
            rows.append(row)
        rules = {"inviteBonus": rows}
    else:
        rows = [_object(value) for value in _array(config, "BATTLE_FIELD_CONFIG_ARRAY")]
        # Follow the actual array references and the zao1/zao2 consumer, instead
        # of assuming any unrelated '8' constant means daily challenge chances.
        fields = {}
        for counter, reference in (("zao1", "JING_DIAN_BATTLE_FIELD"), ("zao2", "LUAN_DOU__BATTLE_FIELD")):
            tokens = declaration(config, reference)
            if len(tokens) != 4 or tokens[:2] != ["BATTLE_FIELD_DEFINES", "["] or tokens[-1] != "]":
                raise ValueError("官方竞技场映射变化，已保留旧目录")
            index = _integer(tokens[2:3])
            if index >= len(rows):
                raise ValueError("官方竞技场映射超出配置，已保留旧目录")
            row = rows[index]
            expression = row.get("dailyFreeChallengeTime", [])
            if expression != ["DAILY_FREE_FIGHT_TIME_NORMAL"]:
                raise ValueError("官方竞技场挑战额度公式变化，已保留旧目录")
            getter = method(config, "DAILY_FREE_FIGHT_TIME_NORMAL")
            if getter[:7] != ["function", "get", "DAILY_FREE_FIGHT_TIME_NORMAL", "(", ")", ":", "int"] or getter[7:9] != ["{", "return"] or getter[-2:] != [";", "}"]:
                raise ValueError("官方竞技场挑战额度不再是静态数值，已保留旧目录")
            fields[counter] = {"fieldId": _integer(row["id"]), "dailyBase": _integer(getter[9:-2])}
        if fields["zao2"]["fieldId"] != fields["zao1"]["fieldId"] + 1:
            raise ValueError("官方竞技场账号字段映射变化，已保留旧目录")
        expected_bindings = {
            "BATTLE_FIELD_DEFINES": ["ArenaV3BattleFieldDefine", ".", "createWithObjArr", "(", "BATTLE_FIELD_CONFIG_ARRAY", ")"],
        }
        for name, expected in expected_bindings.items():
            if declaration(config, name) != expected:
                raise ValueError("官方竞技场场地定义绑定变化，已保留旧目录")
        tokens = _tokens(config)
        assignments = [tokens[i:i + 7] for i in range(len(tokens) - 4)
                       if tokens[i:i + 2] == ["JING_YING_BATTLE_FIELDS", "["] and tokens[i + 4] == "="]
        if assignments != [["JING_YING_BATTLE_FIELDS", "[", "0", "]", "=", "JING_DIAN_BATTLE_FIELD", ";"],
                           ["JING_YING_BATTLE_FIELDS", "[", "1", "]", "=", "LUAN_DOU__BATTLE_FIELD", ";"]]:
            raise ValueError("官方竞技场场地顺序变化，已保留旧目录")
        constructors = _object(declaration(scripts[ARENA + "defines.ArenaV3BattleFieldDefine"], "SPECIAL_CONSTRUCTOR"))
        if {"dailyFreeChallengeTime", "id"} & set(constructors):
            raise ValueError("官方竞技场额度字段新增转换，已保留旧目录")
        rules = fields
    if not valid_group(label, rules):
        raise ValueError("官方日常额度值无效，已保留旧目录")
    return rules, contracts


def valid_group(label: str, value: dict) -> bool:
    def valid_int(number):
        return type(number) is int and 0 <= number <= LIMIT
    if not isinstance(value, dict):
        return False
    if label in ("starRules", "competitionRules"):
        keys = {"dailyDeclaredLimit", "weeklyLimit"} if label == "starRules" else {"dailyBase", "weeklyDeclaredLimit"}
        return set(value) == keys and all(valid_int(v) for v in value.values())
    if label == "farmRules":
        rows = value.get("inviteBonus")
        return set(value) == {"inviteBonus"} and isinstance(rows, list) and 1 <= len(rows) <= 100 and all(
            isinstance(row, dict) and set(row) == {"count", "dailyRefresh", "refreshMax", "dailyPick", "pickMax"}
            and row["count"] == i and all(valid_int(v) for v in row.values())
            and row["dailyRefresh"] <= row["refreshMax"] and row["dailyPick"] <= row["pickMax"]
            for i, row in enumerate(rows))
    return set(value) == {"zao1", "zao2"} and all(isinstance(row, dict) and set(row) == {"fieldId", "dailyBase"}
        and all(valid_int(v) for v in row.values()) and row["fieldId"] > 0 for row in value.values()) and value["zao2"]["fieldId"] == value["zao1"]["fieldId"] + 1


def valid_rules(root: dict, resources: dict) -> bool:
    if not isinstance(root, dict) or type(root.get("schema")) is not int or root.get("schema") != VERSION or set(root) != {"schema", *(v[1] for v in SPECS.values())}:
        return False
    for label, (key, field, classes) in SPECS.items():
        source = resources.get(label)
        if not valid_group(label, root[field]) or not isinstance(source, dict):
            return False
        if (source.get("key") != key or not isinstance(source.get("version"), str)
                or not re.fullmatch(r"[0-9]{8,20}", source["version"])
                or not re.fullmatch(r"[0-9a-f]{64}", str(source.get("sha256", "")))
                or source.get("url") != "https://aoqi.100bt.com/play/" + key + "~" + source["version"] + ".swf"
                or source.get("rulesSha256") != digest(root[field])):
            return False
        scripts = source.get("scripts")
        if not isinstance(scripts, dict) or set(scripts) != set(classes) or any(not re.fullmatch(r"[0-9a-f]{64}", str(v)) for v in scripts.values()):
            return False
        expected = {cls + "." + name: sha for cls, checks in CONTRACTS[label].items() for name, sha in checks.items()}
        if source.get("contracts") != expected:
            return False
    return True
