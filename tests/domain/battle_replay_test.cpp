#include "domain/battle_replay.h"
#include "domain/battle_replay_state.h"
#include "fixtures/battle_replay_fixture.h"
#include "fixtures/empowerment_rule_checks.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <functional>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  int failures = 0;
  const auto check = [&](bool ok, const char* why) { if (!ok) { qCritical() << why; ++failures; } };
  using namespace BattleReplay;
  const auto bytes = battleReplayFixture();
  const auto catalog = battleReplayTestCatalog();
  const auto replay = parse(bytes, catalog);
  check(replay.error.isEmpty(), "valid protocol fixture rejected");
  check(replay.original == bytes, "original bytes changed");
  check(replay.actions == 2 && replay.steps.size() == 6, "empty-damage action or segment lost");
  const auto a = fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  const auto enemy = fighterKey(1, QStringLiteral("tester"), QStringLiteral("7"));
  const auto summoned = fighterKey(0, QStringLiteral("tester"), QStringLiteral("8"));
  const auto team = fighterKey(1, QStringLiteral("tester"), QStringLiteral("@team"));
  check(replay.steps[0].state.size() == 5, "same-cell or same-ID fighters collapsed");
  check(replay.steps[0].state[a].buffs.size() == 2, "repeated initial buffs discarded");
  check(replay.steps[2].state[a].shield == 50, "ashp=0 incorrectly erased shield");
  check(replay.steps[2].state[enemy].hp == 180, "isse=false side effects discarded");
  check(replay.steps[2].state[a].power == 150, "power was incorrectly capped at 100");
  check(replay.steps[2].state[team].lastUnit[QStringLiteral("proa")].toInt() == 2, "team progress assigned to pet");
  check(!replay.steps[3].state[a].shield && !replay.steps[3].state[a].buffsKnown, "unknown special timing presented as a fully known state");
  check(replay.steps[3].state[summoned].presence.contains(QStringLiteral("召唤")), "summon record lost");
  check(!replay.steps[3].state[a].hpPath.isEmpty(), "observation has no provenance");
  check(replay.steps[0].state[a].hp == 100 && replay.steps[3].state[a].lastNumbers["ahp"].toInt() == 90, "later changes mutated earlier snapshots or raw final value lost");
  check(replay.steps[4].state[enemy].presence.contains(QStringLiteral("玩家已移除")), "player removal lost");
  check(replay.steps[5].title.contains(QStringLiteral("未分胜负")), "draw cast to winner zero");
  bool special = false, empty = false, miss = false, xLabel = false, yLabel = false;
  for (const auto& f : replay.fields) {
    special |= f.path.endsWith(QStringLiteral("/unknownPayload/x")) && f.value.toString() == QStringLiteral("preserve");
    empty |= f.path == QStringLiteral("/extra/empty") && f.value.isArray();
    xLabel |= f.path == QStringLiteral("/br/or/team/0/players/0/cts/0/x") && f.label.contains(QStringLiteral("上下"));
    yLabel |= f.path == QStringLiteral("/br/or/team/0/players/0/cts/0/y") && f.label.contains(QStringLiteral("纵深"));
  }
  for (const auto& d : replay.steps[2].details) miss |= d.text.contains(QStringLiteral("命中标记=否"));
  check(special && empty, "unknown or empty raw fields lost"); check(!miss, "missing ishit treated as miss");
  check(xLabel && yLabel, "protocol coordinates incorrectly described as screen x/y");
  check(!replay.issues.isEmpty(), "unknown special event silently accepted");
  check(!parse(QByteArrayLiteral("{bad")).error.isEmpty(), "broken JSON accepted");
  check(!parse(QByteArrayLiteral("{} ")).error.isEmpty(), "non-battle object accepted");
  auto malformed = QJsonDocument::fromJson(bytes).object();
  auto br = malformed[QStringLiteral("br")].toObject();
  auto flow = br[QStringLiteral("fr")].toArray(); auto block = flow[0].toObject(); auto actions = block[QStringLiteral("rl")].toArray();
  auto action = actions[0].toObject(); action[QStringLiteral("cn")] = QJsonObject{{QStringLiteral("ahp"), QStringLiteral("not a number")}};
  actions[0] = action; block[QStringLiteral("rl")] = actions; flow[0] = block; br[QStringLiteral("fr")] = flow; malformed[QStringLiteral("br")] = br;
  const auto invalid = parse(QJsonDocument(malformed).toJson(), catalog);
  check(!invalid.steps[2].state[a].hp && !invalid.issues.isEmpty(), "invalid value silently converted to zero");
  const auto stateBytes = battleReplayStateFixture();
  const auto ordered = parse(stateBytes, catalog);
  const auto& actionStep = ordered.steps[2];
  check(ordered.issues.isEmpty(), "valid ordered fixture produced unresolved records");
  check(actionStep.state[a].shield == -33 && actionStep.state[enemy].shield == 55, "shield ledger/counter gates/clamping differ from official rules");
  check(actionStep.state[a].hp == 77 && actionStep.state[a].maxHp == 150, "after-group value update must follow finalHp calibration");
  check(actionStep.state[a].buffs == QStringList({"101", "99"}), "Buff multiset removed all duplicates or double counted");
  check(actionStep.state[enemy].buffs == QStringList({"100"}), "wait Buff missing from current membership");
  check(actionStep.state[a].initialBuffs == QStringList({"99", "99"}), "initial Buff evidence mutated");
  check(ordered.steps[0].state[fighterKey(0, "tester", "@team")].buffs == QStringList({"77", "88", "88"}), "initial team progress Buff or duplicates omitted");
  const auto checkUnknownMembers = [&](const QMap<QString, Fighter>& state) {
    for (const auto& fighter : state) {
      check(fighter.buffMembers.size() == fighter.buffs.size(), "Buff provenance/member counts diverged");
      for (int i = 0; i < std::min(fighter.buffMembers.size(), fighter.buffs.size()); ++i) {
        const auto& member = fighter.buffMembers[i];
        check(member.id == fighter.buffs[i] && member.source.isEmpty() && member.sourceName.isEmpty() &&
          member.sourceTeam == -1 && !member.path.isEmpty(), "three-field Buff tuple fabricated an applier or lost ordered evidence");
      }
    }
  };
  for (const auto& step : ordered.steps) {
    checkUnknownMembers(step.state);
    for (const auto& moment : step.moments) checkUnknownMembers(moment.state);
    for (const auto& effect : step.effects) if (effect.kind == QStringLiteral("buff"))
      check(effect.source.isEmpty() && effect.buffSource.isEmpty() && effect.buffSourceTeam == -1,
        "Buff effect inferred applier from actor or recipient");
  }
  // Synthetic existing provenance verifies removal follows the first matched
  // member. Neither the action's actor nor the remover becomes its applier;
  // a newly appended three-field tuple still has unknown provenance.
  auto provenanceState = ordered.steps[1].state;
  provenanceState[a].buffMembers = {
    {QStringLiteral("99"), enemy, QStringLiteral("synthetic enemy applier"), QStringLiteral("/synthetic/first"), 1},
    {QStringLiteral("99"), a, QStringLiteral("synthetic allied applier"), QStringLiteral("/synthetic/second"), 0}
  };
  Step provenanceAction; provenanceAction.actor = a; provenanceAction.path = QStringLiteral("/synthetic/action");
  provenanceAction.raw = QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 999999999}, {"ses", 1},
    {"cn", QJsonObject{{"bd_bfs", "0_99_1,1_99_2"}}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
  QStringList provenanceIssues;
  advanceAction(provenanceAction, provenanceState, catalog, provenanceIssues);
  const auto& remaining = provenanceState[a].buffMembers;
  check(provenanceIssues.isEmpty() && provenanceState[a].buffs == QStringList({"99", "99"}) && remaining.size() == 2,
    "synthetic provenance operation changed Buff multiset semantics");
  check(remaining.size() == 2 && remaining[0].source == a && remaining[0].sourceTeam == 0 &&
    remaining[1].source.isEmpty() && remaining[1].sourceTeam == -1, "Buff removal dropped wrong source or attributed unknown addition");
  bool removedFirstSource = false, addedUnknownSource = false;
  for (const auto& effect : provenanceAction.effects) if (effect.kind == QStringLiteral("buff")) {
    removedFirstSource |= effect.delta == -1 && effect.source.isEmpty() && effect.buffSource == enemy &&
      effect.buffSourceTeam == 1 && effect.buffSourcePath == QStringLiteral("/synthetic/first");
    addedUnknownSource |= effect.delta == 1 && effect.source.isEmpty() && effect.buffSource.isEmpty() && effect.buffSourceTeam == -1;
  }
  check(removedFirstSource && addedUnknownSource, "Buff change lost matched-member provenance or fabricated new provenance");
  check(ordered.steps[1].state[a].buffMembers[0].source.isEmpty() && ordered.original == stateBytes,
    "synthetic provenance test mutated imported snapshots or original bytes");
  // Buff IDs enter the official client as integers, including opening
  // membership. Formatting variants must not leave an undeletable copy.
  {
    auto root = QJsonDocument::fromJson(stateBytes).object(); auto battle = root["br"].toObject();
    auto initial = battle["or"].toObject(); auto teams = initial["team"].toArray();
    auto own = teams[0].toObject(); auto players = own["players"].toArray(); auto player = players[0].toObject();
    auto pets = player["cts"].toArray(); auto source = pets[0].toObject(); source["bfs"] = "00099, +099";
    pets[0] = source; player["cts"] = pets; player["pcbfs"] = "0088,+0088";
    auto progress = player["ppbf"].toObject(); progress["pbid"] = "00077"; player["ppbf"] = progress;
    players[0] = player; own["players"] = players; teams[0] = own; initial["team"] = teams;
    battle["or"] = initial; root["br"] = battle;
    const auto normalizedBytes = QJsonDocument(root).toJson(); const auto normalized = parse(normalizedBytes, catalog);
    check(normalized.issues.isEmpty() && normalized.steps.first().state[a].buffs == QStringList({"99", "99"}) &&
      normalized.steps[2].state[a].buffs == actionStep.state[a].buffs,
      "formatted initial Buff IDs did not match integer display operations");
    const auto ownTeam = fighterKey(0, "tester", "@team");
    check(normalized.steps.first().state[ownTeam].buffs == QStringList({"77", "88", "88"}) &&
      normalized.steps.first().state[ownTeam].teamProgress.keys() == QStringList{"77"},
      "initial player Buff/progress IDs were not normalized without losing duplicates");
    check(normalized.original == normalizedBytes && normalized.root == root &&
      normalized.steps.first().state[a].raw["bfs"].toString() == "00099, +099" &&
      normalized.steps.first().state[ownTeam].teamProgress["77"].raw["pbid"].toString() == "00077",
      "Buff identity normalization overwrote original evidence");

    source["bfs"] = "99,,malformed,50000000000,99"; pets[0] = source; player["cts"] = pets;
    player["pcbfs"] = QJsonObject{{"buff", 88}}; progress["pbid"] = QJsonValue(QJsonValue::Null); player["ppbf"] = progress;
    players[0] = player; own["players"] = players; teams[0] = own; initial["team"] = teams;
    battle["or"] = initial; root["br"] = battle; const auto invalidBuffs = parse(QJsonDocument(root).toJson(), catalog);
    check(!invalidBuffs.steps.first().state[a].buffsKnown &&
      invalidBuffs.steps.first().state[a].buffs == QStringList({"99", "99"}) &&
      !invalidBuffs.steps.first().state[ownTeam].buffsKnown &&
      invalidBuffs.steps.first().state[ownTeam].teamProgress.isEmpty() && !invalidBuffs.issues.isEmpty(),
      "malformed initial Buff evidence became an empty known set or a fabricated zero ID");
  }
  // Anonymous, hidden and absent-config Buffs retain their complete recorded
  // operations. A clear+appear is not collapsed into a net-zero refresh, and
  // membership multiplicity is not asserted to be a server-defined layer.
  {
    auto anonymousCatalog = catalog; auto buffs = anonymousCatalog["buffs"].toObject();
    buffs["99"] = QJsonObject{{"nm", ""}, {"hd", 1}, {"buffWaitType", 0}}; anonymousCatalog["buffs"] = buffs;
    auto observed = ordered.steps[1].state;
    observed[a].buffMembers[0] = {"99", enemy, "recorded applier", "/recorded/first", 1};
    Step changes; changes.actor = a; changes.path = "/anonymous-buffs";
    changes.raw = QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 999999999}, {"ses", 1},
      {"cn", QJsonObject{{"bb_hps", "00099_-4_1,900001_2_2"}, {"bb_shps", "99_3_1"}, {"bb_vgs", "900001_7_1"},
        {"bd_bfs", "0_99_1,1_99_1,1_900001_1,0_900002_1"}, {"ad_bfs", "0_900001_2"}}},
      {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
    QStringList anonymousIssues; advanceAction(changes, observed, anonymousCatalog, anonymousIssues);
    QList<Effect> operations; QList<Effect> numbers;
    for (const auto& e : changes.effects) {
      if (e.kind == "buff") operations.append(e);
      if (e.kind == "value" && e.cause == "buff") numbers.append(e);
    }
    check(observed[a].buffsKnown && observed[a].buffs == QStringList({"99", "99"}) &&
      observed[a].hp == 98 && observed[a].shield == 53 && observed[a].power == 107,
      "anonymous/hidden/absent-config Buffs lost membership or numeric changes");
    check(operations.size() == 5 && numbers.size() == 4,
      "same-stage clear/appear or unchanged clear was discarded as a net-zero effect");
    if (operations.size() == 5) {
      check(operations[0].buffOperation == 0 && operations[0].before == 2 && operations[0].after == 1 &&
        operations[0].buffSource == enemy && operations[0].buffSourcePath == "/recorded/first" &&
        operations[1].buffOperation == 1 && operations[1].before == 1 && operations[1].after == 2 &&
        operations[1].buffSource.isEmpty(), "refresh sequence changed or inherited unrecorded Buff provenance");
      check(operations[3].buffOperation == 0 && operations[3].before == 0 && operations[3].after == 0 &&
        operations[3].delta == 0 && !operations[3].changed && operations[3].raw.toString() == "0_900002_1" &&
        operations[3].buffSource.isEmpty(), "unmatched clear lost the recorded operation or fabricated an applier");
      check(operations[2].causeId == "900001" && operations[2].buffWaitType == -1 &&
        operations[4].buffOperation == 0 && operations[4].causeId == "900001",
        "missing Buff configuration suppressed appearance/removal or invented wait policy");
    }
    for (const auto& e : numbers) check(e.source.isEmpty() && e.buffSource.isEmpty() && e.delta.has_value() &&
      !e.raw.isUndefined() && !e.path.isEmpty(), "Buff numerical evidence lost values/path or inferred an applier");
    check(changes.moments.first().state[a].buffs.count("99") == 2 && observed[a].initialBuffs == QStringList({"99", "99"}) &&
      !anonymousIssues.isEmpty(), "missing-config timing was hidden or later Buff state leaked into opening evidence");
  }
  // Playback boundaries are data, not translations of the Chinese captions.
  // The source's changesets 9/20 run with the target's 1/2 at cursor 0/1.
  QStringList playbackBoundaries;
  bool sourceOrdinalGroup = false, targetOrdinalGroup = false, targetFlagsAtSetup = false;
  bool calibrationBeforeObjects = false, objectsAfterCalibration = false;
  for (const auto& moment : actionStep.moments) {
    playbackBoundaries.append(moment.phase + QLatin1Char('/') + moment.part + QLatin1Char('/') + QString::number(moment.groupIndex));
    check(moment.actor == (moment.phase == "counter" ? enemy : a), "moment actor lost original action identity or counter attacker");
    const bool grouped = moment.phase == "before" || moment.phase == "after";
    check(grouped ? moment.groupIndex >= 0 : moment.groupIndex == -1, "moment group index mixed ordinal groups with ungrouped phases");
    for (const auto& effect : moment.effects) {
      if (moment.phase == "before" && moment.part == "values" && moment.groupIndex == 0) {
        sourceOrdinalGroup |= effect.target == a && effect.raw.toString() == "99_10_9";
        targetOrdinalGroup |= effect.target == enemy && effect.raw.toString() == "99_-3_1";
      }
      targetFlagsAtSetup |= effect.kind == "target" && moment.phase == "setup" && moment.part == "initial" && effect.source == moment.actor;
      calibrationBeforeObjects |= effect.cause == "calibration" && effect.target == a && effect.after == 83 &&
        moment.phase == "after" && moment.part == "calibration" && moment.groupIndex == 1;
      objectsAfterCalibration |= effect.cause == "record" && effect.target == a && effect.after == 77 &&
        moment.phase == "after" && moment.part == "objects" && moment.groupIndex == 1;
      if (effect.cause == "counter") check(moment.phase == "counter" && moment.part == "result" && effect.source == moment.actor,
        "counter moment actor does not agree with its directed effects");
    }
  }
  check(playbackBoundaries == QStringList{
    "setup/initial/-1", "before/wait-buff/0", "before/values/0", "before/values/1", "before/power-buff/1",
    "attack/result/-1", "counter/result/-1", "after/values/0", "after/power-buff/0", "after/calibration/0",
    "after/objects/0", "after/values/1", "after/power-buff/1", "after/calibration/1", "after/objects/1", "settle/final/-1"},
    "structured playback metadata changed the existing snapshot order, count or command boundaries");
  check(sourceOrdinalGroup && targetOrdinalGroup && targetFlagsAtSetup,
    "changeset IDs were treated as global playback group indexes or target flags moved into another snapshot");
  check(calibrationBeforeObjects && objectsAfterCalibration, "after-phase calibration/value-reset boundaries lost");
  bool waitFirst = false, pairedGroups = false, attackTotals = false, counter = false, calibration = false, reset = false;
  for (const auto& moment : actionStep.moments) {
    if (moment.title.contains(QStringLiteral("等待型"))) {
      waitFirst |= moment.state[a].shield == 50 && moment.state[enemy].shield == 70 && moment.state[enemy].buffs.contains("100");
      pairedGroups |= moment.note.contains(QStringLiteral("组 9")) && moment.note.contains(QStringLiteral("组 1"));
    }
    if (moment.title == QStringLiteral("技能结算")) attackTotals = moment.state[enemy].hp == 50 && moment.state[a].shield == 53;
    counter |= moment.title == QStringLiteral("反击结算") && moment.state[a].shield == 47 && moment.state[enemy].shield == 54;
    calibration |= moment.title.contains(QStringLiteral("校准")) && moment.state[a].hp == 83;
    reset |= moment.title.contains(QStringLiteral("特殊记录")) && moment.state[a].hp == 77;
  }
  check(waitFirst && pairedGroups, "normal groups globally sorted or waiting Buff applied after numbers");
  check(attackTotals, "target attack sum or source attack delta incorrect");
  check(counter, "counter stage numbers incorrect");
  check(calibration, "final value calibration stage missing");
  check(reset, "value reset stage missing");
  bool directedDamage = false, sourceCost = false, directedCounter = false, unattributedBuff = false;
  bool buffMembership = false, calibrationOnly = false, unchangedCalibration = false, resetUnattributed = false;
  int effectCount = 0;
  for (int i = 0; i < actionStep.moments.size(); ++i) {
    for (const auto& e : actionStep.moments[i].effects) {
      check(e.moment == i && e.target.size() && e.path.startsWith(actionStep.path) && !e.text.isEmpty(),
        "readable effect lost its moment, identity, evidence path or description");
      check(effectCount < actionStep.effects.size() && actionStep.effects[effectCount].text == e.text &&
        actionStep.effects[effectCount].moment == i && actionStep.effects[effectCount].raw == e.raw,
        "action effect summary duplicated, reordered or omitted a moment effect");
      ++effectCount;
      directedDamage |= e.kind == "value" && e.cause == "skill" && e.source == a && e.target == enemy &&
        e.metric == QStringLiteral("生命") && e.before == 200 && e.after == 50 && e.delta == -150 &&
        e.path.endsWith("/trl/0/tn/att_hps") && e.raw.toString() == "-300,150";
      sourceCost |= e.cause == "skill" && e.source == a && e.target == a && e.metric == QStringLiteral("生命") && e.delta == -10;
      directedCounter |= e.cause == "counter" && e.source == enemy && e.target == a &&
        e.metric == QStringLiteral("生命") && e.before == 85 && e.after == 77 && e.delta == -8;
      unattributedBuff |= e.kind == "value" && e.cause == "buff" && e.source.isEmpty() && e.target == a &&
        e.causeId == "99" && e.before == 50 && e.after == 60 && e.delta == 10 && e.text.contains(QStringLiteral("施加者未记录"));
      buffMembership |= e.kind == "buff" && e.source.isEmpty() && e.target == enemy && e.causeId == "100" &&
        e.before == 0 && e.after == 1 && e.delta == 1;
      calibrationOnly |= e.kind == "observation" && e.cause == "calibration" && e.source.isEmpty() &&
        e.target == a && e.metric == QStringLiteral("生命") && e.after == 83;
      unchangedCalibration |= e.cause == "calibration" && e.before == e.after && !e.changed;
      resetUnattributed |= e.cause == "record" && e.source.isEmpty() && e.target == a &&
        e.metric == QStringLiteral("生命上限") && e.before == 100 && e.after == 150;
    }
  }
  check(effectCount == actionStep.effects.size(), "action summary contains extra effects");
  check(directedDamage && sourceCost && directedCounter, "skill/counter relation attributed to the wrong acting fighter");
  check(unattributedBuff && buffMembership, "Buff change lost its identity or fabricated an applier");
  check(calibrationOnly && unchangedCalibration && resetUnattributed, "final/reset values presented as a direct attack");
  bool teamProgress = false, targetWithoutHit = false;
  for (const auto& e : replay.steps[2].effects) {
    teamProgress |= e.kind == "progress" && e.target == team && e.source.isEmpty() && e.before == 0 && e.after == 2 &&
      e.delta == 2 && e.raw.toObject()["mp"].toInt() == 10;
    targetWithoutHit |= e.kind == "target" && e.source == a && e.target == enemy &&
      e.text.contains(QStringLiteral("技能生效：否")) && !e.text.contains(QStringLiteral("命中：否"));
  }
  check(teamProgress && targetWithoutHit, "progress evidence or target flags lost; missing hit status must stay unknown");
  // Replay snapshots are immutable even when visited in reverse, repeatedly.
  for (int pass = 0; pass < 3; ++pass) {
    for (int i = static_cast<int>(actionStep.moments.size()) - 1; i >= 0; --i)
      check(actionStep.moments[i].state.contains(a), "snapshot lost identity during reverse visit");
    check(actionStep.moments.first().state[a].shield == 50 && actionStep.moments.first().state[a].buffs.size() == 2, "later mutation leaked into opening snapshot");
  }
  auto seqCatalog = catalog; auto skills = seqCatalog["skills"].toObject(); auto skill = skills["999999999"].toObject();
  skill["playOrderType"] = "1"; skills["999999999"] = skill; seqCatalog["skills"] = skills;
  const auto sequential = parse(stateBytes, seqCatalog);
  bool emptyGroups = false;
  for (const auto& m : sequential.steps[2].moments) if (m.title.contains(QStringLiteral("等待型")))
    emptyGroups |= m.state[a].shield == 50 && m.state[a].buffs.count("99") == 2 && m.note.contains(QStringLiteral("组 1")) && !m.note.contains(QStringLiteral("组 9"));
  check(emptyGroups && sequential.steps[2].state[a].shield == -33, "sequence fill mode not distinguished from ordinal groups");
  bool sequenceSourceCursor = false, sequenceTargetCursor = false;
  for (const auto& moment : sequential.steps[2].moments) for (const auto& effect : moment.effects) {
    sequenceSourceCursor |= effect.target == a && effect.raw.toString() == "99_10_9" && moment.phase == "before" &&
      moment.part == "values" && moment.groupIndex == 9;
    sequenceTargetCursor |= effect.target == enemy && effect.raw.toString() == "99_-3_1" && moment.phase == "before" &&
      moment.part == "values" && moment.groupIndex == 1;
  }
  check(sequenceSourceCursor && sequenceTargetCursor, "filled sequence groups were renumbered after empty snapshots were skipped");
  auto unknownWaitCatalog = catalog;
  auto unknownWaitBuffs = unknownWaitCatalog["buffs"].toObject(); unknownWaitBuffs.remove("100");
  unknownWaitCatalog["buffs"] = unknownWaitBuffs;
  const auto unknownWait = parse(stateBytes, unknownWaitCatalog);
  bool unverifiedGroupEnd = false;
  for (const auto& moment : unknownWait.steps[2].moments) if (moment.phase == "before" && moment.groupIndex == 0) {
    unverifiedGroupEnd |= moment.part == "group-end";
    check(moment.part != "wait-buff" && moment.part != "values" && moment.part != "power-buff",
      "missing Buff wait configuration fabricated a known internal playback boundary");
  }
  check(unverifiedGroupEnd, "unknown Buff timing lost the explicit group-end boundary");
  const auto alterAction = [&](const std::function<void(QJsonObject&)>& change) {
    auto root = QJsonDocument::fromJson(stateBytes).object(); auto battle = root["br"].toObject(); auto flow = battle["fr"].toArray();
    auto block = flow[0].toObject(); auto list = block["rl"].toArray(); auto action = list[0].toObject(); change(action);
    list[0] = action; block["rl"] = list; flow[0] = block; battle["fr"] = flow; root["br"] = battle;
    return parse(QJsonDocument(root).toJson(), catalog);
  };
  const auto disabled = alterAction([](QJsonObject& action) { action["ses"] = -1; });
  check(disabled.steps[2].state[a].shield == -20 && disabled.steps[2].state[enemy].shield == 70, "disabled attack/counter deltas consumed or Buff effects discarded");
  for (const auto& moment : disabled.steps[2].moments)
    check(moment.phase != "attack" && moment.phase != "counter", "disabled attack fabricated a playback hit or counter phase");
  const auto inactive = alterAction([](QJsonObject& action) { auto ts = action["trl"].toArray(); auto t = ts[0].toObject(); t["isse"] = false; ts[0] = t; action["trl"] = ts; });
  check(inactive.steps[2].state[a].shield == -20 && inactive.steps[2].state[enemy].buffs.contains("100"), "isse=false must skip attack numbers but keep before/after effects");
  for (const auto& e : inactive.steps[2].effects)
    check(e.cause != "skill" && e.cause != "counter", "inactive target fabricated a direct skill or counter relation");
  const auto corrupt = alterAction([](QJsonObject& action) { auto n = action["cn"].toObject(); n["bb_shps"] = "99_broken_9"; action["cn"] = n; });
  check(!corrupt.steps[2].state[a].shield && !corrupt.issues.isEmpty(), "malformed delta presented as known balance");
  const auto legacy = alterAction([](QJsonObject& action) { auto n = action["cn"].toObject(); n["ba_bfs"] = "101"; n["bc_bfs"] = "99"; action["cn"] = n; });
  check(legacy.steps[2].state[a].buffs == actionStep.state[a].buffs && !legacy.issues.isEmpty(), "unconsumed legacy fields double-applied");
  const auto selfTarget = alterAction([](QJsonObject& action) {
    auto ts = action["trl"].toArray();
    ts.append(QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", false},
      {"tn", QJsonObject{{"bb_shps", "99_100_1"}, {"bd_bfs", "1_101_1"}}}});
    action["trl"] = ts;
  });
  check(selfTarget.steps[2].state[a].shield == -33 && selfTarget.steps[2].state[a].buffs.count("101") == 2,
    "same source/target HP-SP must be skipped without dropping its Buff changes");
  const auto missing = parse(stateBytes);
  check(!missing.steps[2].state[a].shield && !missing.steps[2].state[a].buffsKnown && !missing.issues.isEmpty(), "missing official configuration silently guessed");
  check(missing.steps[2].moments.last().phase == "unresolved" && missing.steps[2].moments.last().part == "final" &&
    missing.steps[2].moments.last().actor == a && missing.steps[2].moments.last().groupIndex == -1,
    "unverified internal order mislabeled as a confirmed playback phase");
  const auto battlefield = parse(battleReplayBattlefieldFixture(), battleReplayBattlefieldCatalog());
  const auto clone = fighterKey(0, "tester", "9");
  const auto& transformed = battlefield.steps[2].state[a];
  check(battlefield.issues.isEmpty(), "verified battlefield transitions reported unknown");
  check(transformed.raceId == "9002" && transformed.faceId == "9003" && transformed.maxPower == 150,
    "transform did not update current form and official power reference");
  check(transformed.normalSkill == "12" && transformed.uniqueSkill == "45" && transformed.superUniqueSkill == "31",
    "cu first/fallback semantics or persistent extra skill differs from official setter");
  check(transformed.attributes == QStringList{"4"} && transformed.attributesKnown, "attribute-only update lost or reset form");
  check(!battlefield.steps[2].state[summoned].entered && battlefield.steps[3].state[summoned].entered,
    "summon metadata mistaken for an appearance command, or summon skill ignored");
  bool summonPlayback = false;
  for (const auto& moment : battlefield.steps[3].moments) for (const auto& effect : moment.effects)
    summonPlayback |= effect.target == summoned && effect.kind == "appearance" && moment.phase == "attack" &&
      moment.part == "result" && moment.actor == a && moment.groupIndex == -1;
  check(summonPlayback, "summon appearance lost its distinct skill-result playback phase");
  check(battlefield.steps[2].state[clone].cloneCalled && battlefield.steps[2].state[clone].entered && battlefield.steps[2].state.size() == battlefield.steps[0].state.size(),
    "clone resolved against unconsumed ti/pn instead of hosting player");
  check(battlefield.steps[4].state[a].playerX == 2 && battlefield.steps[4].state[a].playerY == 1 &&
    battlefield.steps[4].state[a].x == 0 && battlefield.steps[4].state[a].y == 0 && !battlefield.steps[4].state[a].playerRemoved,
    "formation shift incorrectly moved a fighter's nine-grid cell");
  check(battlefield.steps[5].state[a].playerRemoved && !battlefield.steps[5].state[fighterKey(0, "reserve-player", "7")].playerRemoved,
    "player removal erased another same-team formation");
  check(battlefield.steps[2].keyChanges.contains(QStringLiteral("生命归零")) && battlefield.steps[2].keyChanges.contains(QStringLiteral("生命恢复")) &&
    battlefield.steps[2].keyChanges.contains(QStringLiteral("登场")) && battlefield.steps[2].keyChanges.contains(QStringLiteral("变身")),
    "within-action key transitions lost when final HP equals initial HP");
  check(battlefield.steps[0].state[a].raceId == "9001" && battlefield.steps[0].state[a].maxPower == 100 &&
    !battlefield.steps[0].state[clone].entered, "later shape/appearance changes mutated initial snapshot");
  bool transformEffect = false, cloneEffect = false, summonEffect = false;
  for (const auto& e : battlefield.steps[2].effects) {
    transformEffect |= e.kind == "transform" && e.target == a && e.source.isEmpty() && e.cause == "record";
    cloneEffect |= e.kind == "appearance" && e.target == clone && e.source.isEmpty() && e.cause == "record";
  }
  for (const auto& e : battlefield.steps[3].effects)
    summonEffect |= e.kind == "appearance" && e.target == summoned && e.source == a && e.cause == "skill";
  check(transformEffect && cloneEffect && summonEffect, "appearance/transform evidence lost or its source was guessed");
  // Cosmetic names and nicknames never override verified species identity.
  auto namedCatalog = battleReplayBattlefieldCatalog();
  auto namedPets = namedCatalog["pets"].toObject();
  for (const auto& id : {QStringLiteral("9001"), QStringLiteral("9002")}) {
    auto pet = namedPets[id].toObject();
    pet["name"] = "same cosmetic name";
    pet["identity"] = QJsonObject{{"canonicalName", id == "9001" ? "original A" : "original B"}, {"status", "skin"}};
    namedPets[id] = pet;
  }
  namedCatalog["pets"] = namedPets;
  auto namedBytes = battleReplayBattlefieldFixture();
  namedBytes.replace("\"name\":\"synthetic source\"", "\"name\":\"cosmetic\",\"customName\":\"nickname\"");
  namedBytes.replace("\"name\":\"synthetic clone\"", "\"name\":\"clone skin\",\"oRaceId\":9002");
  const auto named = parse(namedBytes, namedCatalog);
  check(named.original == namedBytes && named.steps[0].state[a].raw["customName"].toString() == "nickname", "raw names were overwritten");
  check(fighterName(named.steps[0].state[a]) == "original A" && fighterName(named.steps[2].state[a]) == "original B", "skin naming did not follow current form");
  check(named.steps[0].state[clone].name == "original B" && fighterName(named.steps[0].state[clone]) == "original A", "initial copied form lost original pet identity");
  check(named.steps[2].state[a].raceId == "9002" && named.steps[2].state[a].normalSkill == "12", "name normalization changed mechanics identifiers");
  check(named.steps[2].title.startsWith("original A") && named.steps[3].title.startsWith("original B"), "action title used stale form or cosmetic name");
  auto unknownPet = namedPets["9001"].toObject(); unknownPet["identity"] = QJsonObject{{"status", "unresolved"}};
  namedPets["9001"] = unknownPet; namedCatalog["pets"] = namedPets;
  check(petName(namedCatalog, "9001").contains(QStringLiteral("原名待核实")), "unresolved skin silently reused as original name");
  unknownPet.remove("identity"); namedPets["9001"] = unknownPet; namedCatalog["pets"] = namedPets;
  check(petName(namedCatalog, "9001").contains(QStringLiteral("原名待核实")), "legacy display name was treated as verified original name");

  // Presentation identities follow the official client while the logged si
  // remains available. A disabled action still names the attempted skill.
  auto metadataCatalog = battleReplayBattlefieldCatalog();
  auto metadataSkills = metadataCatalog["skills"].toObject();
  metadataSkills["999999997"] = QJsonObject{{"name", "synthetic combo"}, {"action", "3"}, {"attackType", "近身"}};
  metadataCatalog["skills"] = metadataSkills;
  metadataCatalog["execution"] = QJsonObject{{"1", QJsonObject{{"type", "1"}, {"nm", "synthetic enabled"}}},
    {"-1", QJsonObject{{"type", "2"}, {"nm", "synthetic disabled"}}}};
  metadataCatalog["combos"] = QJsonArray{QJsonObject{{"skillId", "999999999"}, {"buffs", "99,101"},
    {"targetSkillIds", "999999998,999999997"}}};
  const auto rewriteAction = [&](const std::function<void(QJsonObject&)>& change, const QJsonObject& definitions) {
    auto root = QJsonDocument::fromJson(stateBytes).object(); auto battle = root["br"].toObject();
    auto flow = battle["fr"].toArray(); auto block = flow[0].toObject(); auto list = block["rl"].toArray();
    auto action = list[0].toObject(); change(action); list[0] = action; block["rl"] = list;
    flow[0] = block; battle["fr"] = flow; root["br"] = battle;
    return parse(QJsonDocument(root).toJson(), definitions);
  };
  auto comboBytes = stateBytes;
  comboBytes.replace("\"bfs\":\"99,99\"", "\"bfs\":\"99,101\"");
  const auto combo = parse(comboBytes, metadataCatalog);
  check(combo.steps[2].loggedSkillId == "999999999" && combo.steps[2].playbackSkillId == "999999997" &&
    combo.steps[2].raw.toObject()["si"].toInt() == 999999999 && combo.steps[2].playbackKind == "normal" &&
    combo.steps[2].executionName == "synthetic enabled", "combo priority, logged skill or playback metadata lost");
  bool comboCause = false;
  for (const auto& e : combo.steps[2].effects) if (e.cause == "skill") comboCause |= e.causeId == "999999997";
  check(comboCause, "combo effects still name the pre-substitution skill");
  const auto disabledMetadata = rewriteAction([](QJsonObject& action) { action["ses"] = -1; }, metadataCatalog);
  check(disabledMetadata.steps[2].playbackKind == "disabled" && disabledMetadata.steps[2].loggedSkillId == "999999999" &&
    disabledMetadata.steps[2].playbackSkillId == "999999998" && disabledMetadata.steps[2].executionName == "synthetic disabled",
    "disabled command lost attempted skill or acquired a normal playback kind");
  check(battlefield.steps[3].playbackKind == "summon" && battlefield.steps[3].playbackSkillId == "999999998",
    "summon command metadata depends on presentation text");
  check(missing.steps[2].playbackKind == "unresolved" && missing.steps[2].loggedSkillId == "999999999" &&
    missing.steps[2].playbackSkillId.isEmpty(), "missing configuration fabricated a confirmed playback skill");

  // The counter's normal skill may have changed during BEFORE records. It is
  // read at the counter boundary and is not passed through combo selection.
  const auto counterMetadata = rewriteAction([](QJsonObject& action) {
    auto targets = action["trl"].toArray(); auto target = targets[0].toObject(); auto nums = target["tn"].toObject();
    auto units = nums["rl"].toArray();
    units.prepend(QJsonObject{{"t", 1}, {"o", 1}, {"changeset", 1}, {"or", 9001}, {"of", 9001},
      {"cr", 9002}, {"cf", 9002}, {"cn", 42}, {"cu", "43"}, {"scu", 44}, {"ca", "2"}});
    nums["rl"] = units; target["tn"] = nums; targets[0] = target; action["trl"] = targets;
  }, battleReplayBattlefieldCatalog());
  check(counterMetadata.steps[2].counterSkillId == "42", "counter metadata used stale or originating skill ID");
  bool counterCause = false;
  for (const auto& e : counterMetadata.steps[2].effects)
    counterCause |= e.cause == "counter" && e.causeId == "42" && e.source == enemy;
  check(counterCause, "counter effect lost current normal skill or its own actor");

  // Null uses exactly the same representer/default rules as absence.
  const auto nullIds = rewriteAction([](QJsonObject& action) {
    action["orrt"] = QJsonValue(QJsonValue::Null);
    action["prl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", QJsonValue(QJsonValue::Null)},
      {"isse", true}, {"ishit", true}, {"tn", QJsonObject{{"bd_bfs", "1_101_1"}}}}};
  }, catalog);
  check(nullIds.steps[2].actor == a && nullIds.steps[2].state[a].shield == actionStep.state[a].shield,
    "orrt:null turned a normal action into a representative action");
  check(nullIds.steps[2].state.size() == ordered.steps[0].state.size() && nullIds.steps[2].state[team].buffs.contains("101"),
    "tci:null created a phantom fighter or lost representative state");
  bool teamTargetRecord = false;
  for (const auto& e : nullIds.steps[2].effects) if (e.path.endsWith("/prl/0")) {
    teamTargetRecord |= e.kind == "team-target" && e.cause == "record" && e.source.isEmpty() &&
      e.target == team && e.raw.toObject()["ishit"].toBool() && !e.changed;
    check(e.kind != "target", "prl became an ordinary attack target");
  }
  check(teamTargetRecord, "prl target evidence was dropped or fabricated an attacking source");

  // Reported t=2 old values define official bubbles, not the ledger's observed
  // starting balance. Both remain independently auditable when they disagree.
  auto resetBytes = stateBytes;
  resetBytes.replace("\"old_curv\":83", "\"old_curv\":50");
  resetBytes.replace("\"old_maxv\":100", "\"old_maxv\":120");
  const auto resetMetadata = parse(resetBytes, catalog);
  bool recordedHpReset = false, recordedMaxReset = false;
  for (const auto& e : resetMetadata.steps[2].effects) if (e.cause == "record" && e.kind == "value") {
    check(!e.before || !e.after || e.delta == *e.after - *e.before, "recorded reset delta broke ledger before/after invariant");
    recordedHpReset |= e.metric == QStringLiteral("生命") && e.before == 83 && e.after == 77 && e.delta == -6 &&
      e.recordedBefore == 50 && e.recordedAfter == 77 && e.recordedDelta == 27;
    recordedMaxReset |= e.metric == QStringLiteral("生命上限") && e.before == 100 && e.after == 150 && e.delta == 50 &&
      e.recordedBefore == 120 && e.recordedAfter == 150 && e.recordedDelta == 30;
  }
  check(recordedHpReset && recordedMaxReset && resetMetadata.steps[2].state[a].hp == 77,
    "t=2 reused observed state instead of recorded old/new bubble values");
  const auto incompleteReset = rewriteAction([](QJsonObject& action) {
    auto nums = action["cn"].toObject(); auto units = nums["rl"].toArray(); auto unit = units[0].toObject();
    unit.remove("old_curv"); units[0] = unit; nums["rl"] = units; action["cn"] = nums;
  }, catalog);
  for (const auto& e : incompleteReset.steps[2].effects) if (e.cause == "record" && e.metric == QStringLiteral("生命"))
    check(!e.recordedBefore && !e.recordedDelta && e.recordedAfter == 77, "missing recorded old value was guessed from current state");

  // Queue inheritance ends at the BattleUnit boundary. Per-team player rounds
  // persist; an explicit empty queue is known, while an absent first queue is not.
  auto queueRoot = QJsonDocument::fromJson(stateBytes).object(); auto queueBattle = queueRoot["br"].toObject();
  const QJsonObject queueBase{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 999999999}, {"ses", 1},
    {"cn", QJsonObject{}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
  auto queueFirst = queueBase; queueFirst["seqa"] = QJsonArray{7, 8}; queueFirst["seqb"] = QJsonArray{7};
  queueFirst["pr"] = 3; queueFirst["fbs"] = true; queueFirst["isor"] = false;
  auto queueSecond = queueBase; queueSecond["cti"] = 1; queueSecond["pr"] = 2; queueSecond["fbs"] = false;
  auto queueThird = queueBase; queueThird["seqa"] = QJsonArray{}; queueThird["pr"] = 0; queueThird["isor"] = true;
  queueBattle["fr"] = QJsonArray{QJsonObject{{"pan", "left-recorded"}, {"pbn", "right-recorded"},
    {"rl", QJsonArray{queueFirst, queueSecond, queueThird}}},
    QJsonObject{{"pbn", ""}, {"rl", QJsonArray{queueBase}}}}; queueRoot["br"] = queueBattle;
  const auto queues = parse(QJsonDocument(queueRoot).toJson(), catalog);
  check(queues.steps[2].knownActionQueues == QSet<int>{0, 1} && queues.steps[2].actionQueues.value(0) == QStringList{"7", "8"},
    "initial explicit action queues were not structured");
  check(queues.steps[3].actionQueues == queues.steps[2].actionQueues && queues.steps[3].playerRounds.value(0) == 3 &&
    queues.steps[3].playerRounds.value(1) == 2, "absent queue failed to inherit or other team's player round was erased");
  check(queues.steps[4].knownActionQueues.contains(0) && queues.steps[4].actionQueues.value(0).isEmpty() &&
    queues.steps[4].actionQueues.value(1) == QStringList{"7"} && queues.steps[4].playerRounds.value(0) == 3,
    "explicit empty queue treated as absent or zero player round erased visible prior round");
  check(queues.steps[6].knownActionQueues.isEmpty() && queues.steps[6].actionQueues.isEmpty() && queues.steps[6].playerRounds.size() == 2,
    "queues inherited across BattleUnit boundary or player rounds reset with the queue");
  check(queues.steps[1].actionQueuePlayers.value(0) == "left-recorded" && queues.steps[2].actionQueuePlayers.value(1) == "right-recorded" &&
    queues.steps[3].actionQueuePlayers == queues.steps[1].actionQueuePlayers && queues.steps[6].actionQueuePlayers.isEmpty(),
    "queue identities were inferred from actor/selection, or inherited across the wrong BattleUnit");
  check(queues.steps[2].fromNormalQueue == true && queues.steps[3].fromNormalQueue == false && !queues.steps[4].fromNormalQueue &&
    queues.steps[2].overRoundLimit == false && !queues.steps[3].overRoundLimit && queues.steps[4].overRoundLimit == true,
    "queue-source/round-limit metadata collapsed missing, true and false");

  // Two simultaneous progress effects retain independent observations and
  // provenance even though the legacy lastUnit is overwritten each time.
  const auto progressMetadata = rewriteAction([](QJsonObject& action) {
    action["cn"] = QJsonObject{}; action["trl"] = QJsonArray{};
    action["prl"] = QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"isse", false},
      {"tn", QJsonObject{{"rl", QJsonArray{
        QJsonObject{{"t", 4}, {"o", 1}, {"changeset", 1}, {"pbid", 77}, {"prob", 2}, {"proa", 4}, {"vc", 2}, {"mp", 10}, {"isse", false}},
        QJsonObject{{"t", 4}, {"o", 1}, {"changeset", 2}, {"pbid", 88}, {"prob", 0}, {"proa", 6}, {"vc", 6}, {"mp", 12}, {"isse", true}},
        QJsonObject{{"t", 4}, {"o", 2}, {"changeset", 3}, {"pbid", 77}, {"prob", 4}, {"proa", 0}, {"vc", -4}, {"mp", 10}, {"isse", true}}
      }}}}}};
  }, catalog);
  const auto ourTeam = fighterKey(0, "tester", "@team");
  const auto initialProgress = progressMetadata.steps[0].state[ourTeam].teamProgress;
  const auto finalProgress = progressMetadata.steps[2].state[ourTeam].teamProgress;
  check(initialProgress.size() == 1 && initialProgress.value("77").current == 2 && initialProgress.value("77").maximum == 10 &&
    !initialProgress.value("77").delta, "initial ppbf lost balance or fabricated a prior change");
  check(finalProgress.size() == 2 && finalProgress.value("77").current == 0 && finalProgress.value("77").previous == 4 &&
    finalProgress.value("77").delta == -4 && finalProgress.value("77").skillEffected == true &&
    finalProgress.value("88").current == 6 && finalProgress.value("88").maximum == 12 &&
    finalProgress.value("88").path.endsWith("/prl/0/tn/rl/1"), "one team's progress effect erased another or lost source evidence");
  check(progressMetadata.steps[2].state[ourTeam].lastUnit["pbid"].toInt() == 77 &&
    progressMetadata.steps[0].state[ourTeam].teamProgress.value("77").current == 2,
    "progress map broke legacy lastUnit or mutated initial snapshots");
  {
    const auto formattedProgress = rewriteAction([](QJsonObject& action) {
      action["cn"] = QJsonObject{}; action["trl"] = QJsonArray{};
      action["prl"] = QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"isse", false},
        {"tn", QJsonObject{{"rl", QJsonArray{
          QJsonObject{{"t", 4}, {"o", 1}, {"changeset", 1}, {"pbid", "00077"}, {"prob", 2}, {"proa", 5}, {"vc", 3}, {"mp", 10}},
          QJsonObject{{"t", 4}, {"o", 1}, {"changeset", 1}, {"pbid", "+077"}, {"prob", 5}, {"proa", 2}, {"vc", -3}, {"mp", 10}},
          QJsonObject{{"t", 4}, {"o", 1}, {"changeset", 1}, {"pbid", QJsonValue(QJsonValue::Null)}, {"prob", 2}, {"proa", 4}, {"vc", 2}, {"mp", 10}}
        }}}}}};
    }, catalog);
    const auto& step = formattedProgress.steps[2]; QList<Effect> events;
    for (const auto& e : step.effects) if (e.kind == "progress") events.append(e);
    check(step.state[ourTeam].teamProgress.size() == 1 && step.state[ourTeam].teamProgress["77"].current == 2 &&
      step.state[ourTeam].teamProgress["77"].previous == 5 && step.state[ourTeam].teamProgress["77"].delta == -3 &&
      step.state[ourTeam].teamProgress["77"].raw["pbid"].toString() == "+077",
      "equivalent progress IDs split the balance, or invalid identity overwrote a known progress observation");
    check(events.size() == 3 && events[0].causeId == "77" && events[1].causeId == "77" && events[2].causeId.isEmpty() &&
      events[0].delta == 3 && events[1].delta == -3 && events[2].delta == 2 &&
      events[0].raw.toObject()["pbid"].toString() == "00077" && !formattedProgress.issues.isEmpty(),
      "progress normalization lost same-stage events, raw IDs or invalid-ID numeric evidence");
  }

  // Representative skill choice uses the first eps dragon, its recorded gene,
  // and the official normal/lighted skill IDs. Unmatched dragons stay unknown.
  auto teamCatalog = catalog;
  teamCatalog["bufferExecution"] = QJsonObject{{"88", QJsonObject{{"actionResId", "200"}, {"skillId", "701"}, {"lightedSkillId", "702"}}},
    {"89", QJsonObject{{"actionResId", "0"}, {"skillId", "703"}}}};
  const auto makeTeamReplay = [&](const QJsonArray& dragons, const QJsonArray& dragonIds, int buffId) {
    auto root = QJsonDocument::fromJson(stateBytes).object(); auto battle = root["br"].toObject();
    auto opening = battle["or"].toObject(); auto teams = opening["team"].toArray(); auto allied = teams[0].toObject();
    auto players = allied["players"].toArray(); auto player = players[0].toObject(); player["drs"] = dragons;
    players[0] = player; allied["players"] = players; teams[0] = allied; opening["team"] = teams; battle["or"] = opening;
    auto action = queueBase; action["orrt"] = 2; action["epbi"] = buffId; action["eps"] = dragonIds;
    action.remove("si"); action.remove("cci");
    battle["fr"] = QJsonArray{QJsonObject{{"rl", QJsonArray{action}}}}; root["br"] = battle;
    return parse(QJsonDocument(root).toJson(), teamCatalog);
  };
  const auto noDragon = makeTeamReplay({}, {}, 88);
  const auto lighted = makeTeamReplay(QJsonArray{QJsonObject{{"di", 5}, {"ge", "aaBc"}}}, QJsonArray{5}, 88);
  const auto ordinaryDragon = makeTeamReplay(QJsonArray{QJsonObject{{"di", 5}, {"ge", "aABc"}}}, QJsonArray{5}, 88);
  const auto unknownDragon = makeTeamReplay({}, QJsonArray{5}, 88);
  const auto simpleTeam = makeTeamReplay({}, {}, 89);
  check(noDragon.steps[2].playbackKind == "team-skill" && noDragon.steps[2].playbackSkillId == "701" &&
    noDragon.steps[2].loggedSkillId.isEmpty() && noDragon.steps[2].actor == ourTeam, "team execution confused epbi or synthetic si with a recorded skill");
  check(lighted.steps[2].playbackSkillId == "702" && ordinaryDragon.steps[2].playbackSkillId == "701",
    "official two-lowercase-a gene predicate was not followed");
  check(unknownDragon.steps[2].playbackKind == "team-skill" && unknownDragon.steps[2].playbackSkillId.isEmpty() &&
    !unknownDragon.issues.isEmpty(), "missing dragon identity silently selected the ordinary team skill");
  check(simpleTeam.steps[2].playbackKind == "team-simple" && simpleTeam.steps[2].playbackSkillId.isEmpty(),
    "simple representative command fabricated a skill attack");

  // Flash waitType 2 starts the next item only after its own wait callback.
  // Types 1/3 can start together; their non-wait tail runs AFTER the outer
  // callback. Independent fighters retain their own local batch numbering.
  auto waitCatalog = catalog; auto waitBuffs = waitCatalog["buffs"].toObject();
  waitBuffs["102"] = QJsonObject{{"nm", "synthetic serial"}, {"buffWaitType", "2"}, {"did", "645"}};
  waitBuffs["103"] = QJsonObject{{"nm", "synthetic force visible"}, {"buffWaitType", "3"}};
  waitBuffs["104"] = QJsonObject{{"nm", "synthetic non-wait"}, {"buffWaitType", "0"}};
  waitBuffs["105"] = QJsonObject{{"nm", "synthetic concurrent"}, {"buffWaitType", "1"}};
  waitCatalog["buffs"] = waitBuffs;
  const auto waitOrdering = rewriteAction([](QJsonObject& action) {
    action["cn"] = QJsonObject{{"bd_bfs", "1_102_1,1_102_1,1_104_1"}, {"bb_hps", "99_-1_1"}};
    action["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", false},
      {"tn", QJsonObject{{"bd_bfs", "1_100_1,1_105_1,1_104_1"}, {"bb_hps", "99_-2_1"}}}}};
    action["prl"] = QJsonArray{};
  }, waitCatalog);
  QList<int> sourceWaitBatches, targetWaitBatches;
  bool serialTailBeforeCallback = false, concurrentTailAfterCallback = false, parallelStart = false;
  int waitMoments = 0, tailMoments = 0;
  for (const auto& moment : waitOrdering.steps[2].moments) {
    if (moment.part == "wait-buff") ++waitMoments;
    if (moment.part == "wait-tail") ++tailMoments;
    bool sourceStarts = false, targetStarts = false;
    for (const auto& e : moment.effects) if (e.kind == "buff") {
      if (e.target == a && e.causeId == "102") {
        sourceWaitBatches.append(e.buffPlaybackBatch);
        check(e.buffWaitType == 2 && !e.afterWaitCallback && moment.part == "wait-buff",
          "serial wait became a concurrent callback tail or H5-only display645 skip");
        sourceStarts |= e.buffPlaybackBatch == 0;
      }
      if (e.target == enemy && (e.causeId == "100" || e.causeId == "105")) {
        targetWaitBatches.append(e.buffPlaybackBatch); targetStarts |= e.buffPlaybackBatch == 0;
      }
      serialTailBeforeCallback |= e.target == a && e.causeId == "104" && e.buffPlaybackBatch == 2 &&
        !e.afterWaitCallback && moment.part == "wait-buff" && moment.state[a].hp == 100;
      concurrentTailAfterCallback |= e.target == enemy && e.causeId == "104" && e.buffPlaybackBatch == 1 &&
        e.afterWaitCallback && moment.part == "wait-tail";
    }
    parallelStart |= sourceStarts && targetStarts;
  }
  check(sourceWaitBatches == QList<int>{0, 1} && targetWaitBatches == QList<int>{0, 0} && parallelStart,
    "same-fighter serial/concurrent waits or independent fighter tracks were collapsed");
  check(waitMoments == 3 && tailMoments == 1 && serialTailBeforeCallback && concurrentTailAfterCallback,
    "Flash wait completion and trailing non-wait boundaries were not retained");
  check(waitOrdering.steps[2].state[a].hp == 99 && waitOrdering.steps[2].state[enemy].hp == 198 &&
    waitOrdering.steps[2].state[a].buffs.count("102") == 2 && waitOrdering.steps[2].state[enemy].buffs.contains("104"),
    "wait presentation lost/repeated Buff operations or changed numerical effects");

  auto hiddenState = ordered.steps[1].state; hiddenState[enemy].hp = 0; updatePresence(hiddenState[enemy]);
  Step forceVisible; forceVisible.actor = a; forceVisible.path = "/synthetic/wait-force-visible";
  forceVisible.raw = QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 999999999}, {"ses", 1},
    {"cn", QJsonObject{}}, {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", false},
      {"tn", QJsonObject{{"bd_bfs", "1_103_1,1_105_1"}}}}}}, {"prl", QJsonArray{}}};
  QStringList forceVisibleIssues; advanceAction(forceVisible, hiddenState, waitCatalog, forceVisibleIssues);
  bool visibleDirective = false;
  for (const auto& e : forceVisible.effects) if (e.kind == "buff" && e.causeId == "103")
    visibleDirective |= e.visibilityCommand == "show-up" && e.buffWaitType == 3 && e.buffPlaybackBatch == 0 && e.source.isEmpty();
  check(visibleDirective && hiddenState[enemy].hp == 0 && hiddenState[enemy].presence.contains(QStringLiteral("生命为 0")) &&
    !forceVisible.keyChanges.contains(QStringLiteral("生命恢复")), "waitType 3 showUp was dropped or fabricated a resurrection");

  // Official BattleUtil.handleOneBuff restores summon layers at the consumed
  // waiting-Buff add, without setting the called-out flag. The real example is
  // 20260918114229-2-win.txt /br/fr/0/rl/0/trl/6/tn/ad_bfs = 1_30098_2;
  // the synthetic IDs below exercise that same existing branch independently.
  auto summonDisplayState = battlefield.steps[0].state;
  const auto summonBefore = summonDisplayState[summoned];
  Step summonDisplay; summonDisplay.actor = a; summonDisplay.path = "/synthetic/summon-display";
  summonDisplay.raw = QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 999999999}, {"ses", 1},
    {"cn", QJsonObject{}}, {"trl", QJsonArray{
      QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 8}, {"isse", false}, {"tn", QJsonObject{{"bd_bfs", "1_100_1"}}}},
      QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 9}, {"isse", false}, {"tn", QJsonObject{{"bd_bfs", "1_100_1"}}}}}}};
  QStringList summonDisplayIssues;
  advanceAction(summonDisplay, summonDisplayState, battleReplayBattlefieldCatalog(), summonDisplayIssues);
  check(!summonBefore.entered && !summonBefore.displayLayersRestored && !presentedOnField(summonBefore) &&
    !summonDisplayState[summoned].entered && summonDisplayState[summoned].displayLayersRestored &&
    presentedOnField(summonDisplayState[summoned]) && summonDisplayState[summoned].hp == summonBefore.hp &&
    !summonDisplayState[summoned].presence.contains(QStringLiteral("待召唤")) &&
    summonDisplayState[summoned].displayLayersPath == "/synthetic/summon-display/trl/0/tn/bd_bfs" &&
    !summonDisplayState[clone].displayLayersRestored && !presentedOnField(summonDisplayState[clone]) &&
    !summonDisplay.keyChanges.contains(QStringLiteral("登场")) && !summonDisplay.keyChanges.contains(QStringLiteral("生命恢复")),
    "official layer restoration stayed pending or fabricated entry/revival/uncalled clone");
  int restoredMoment = -1;
  for (const auto& e : summonDisplay.effects) if (e.target == summoned && e.restoresDisplayLayers) restoredMoment = e.moment;
  check(restoredMoment > 0 && !summonDisplay.moments[restoredMoment - 1].state[summoned].displayLayersRestored &&
    summonDisplay.moments[restoredMoment].state[summoned].displayLayersRestored &&
    !battlefield.steps[0].state[summoned].displayLayersRestored,
    "layer restoration was applied before its actual command or mutated an older snapshot");
  Step removeSummonBuff; removeSummonBuff.actor = a; removeSummonBuff.path = "/synthetic/remove-summon-buff";
  auto removeAction = summonDisplay.raw.toObject(); auto removeTargets = removeAction["trl"].toArray();
  auto removeTarget = removeTargets[0].toObject(); removeTarget["tn"] = QJsonObject{{"bd_bfs", "0_100_1"}};
  removeAction["trl"] = QJsonArray{removeTarget}; removeSummonBuff.raw = removeAction;
  advanceAction(removeSummonBuff, summonDisplayState, battleReplayBattlefieldCatalog(), summonDisplayIssues);
  check(summonDisplayState[summoned].displayLayersRestored && !summonDisplayState[summoned].entered &&
    !summonDisplayState[summoned].buffs.contains("100") && presentedOnField(summonDisplayState[summoned]),
    "a later action/Buff removal reverted a restored summon to pending");
  summonDisplayState[summoned].hp = 0; updatePresence(summonDisplayState[summoned]);
  check(summonDisplayState[summoned].presence.contains(QStringLiteral("生命为 0")),
    "display restoration concealed the recorded zero HP");

  // tbt/brs is the official team container. Select the last member with an or
  // object; keep every member as evidence, without concatenating their states.
  const auto firstMember = QJsonDocument::fromJson(stateBytes).object()["br"].toObject();
  auto selectedMember = firstMember; selectedMember["win"] = 1;
  auto selectedOpening = selectedMember["or"].toObject(); auto selectedTeams = selectedOpening["team"].toArray();
  auto selectedTeam = selectedTeams[0].toObject(); auto selectedPlayers = selectedTeam["players"].toArray();
  auto selectedPlayer = selectedPlayers[0].toObject(); auto selectedFighters = selectedPlayer["cts"].toArray();
  auto selectedFighter = selectedFighters[0].toObject(); selectedFighter["hp"] = 333; selectedFighters[0] = selectedFighter;
  selectedPlayer["cts"] = selectedFighters; selectedPlayers[0] = selectedPlayer; selectedTeam["players"] = selectedPlayers;
  selectedTeams[0] = selectedTeam; selectedOpening["team"] = selectedTeams; selectedMember["or"] = selectedOpening;
  const QJsonArray teamMembers{firstMember, QJsonObject{{"nn", "metadata only"}}, selectedMember,
    QJsonObject{{"or", QJsonValue(QJsonValue::Null)}, {"unparsed", "retained"}}};
  const QJsonObject teamContainer{{"tbt", false}, {"brs", teamMembers}};
  const QJsonObject teamEnvelope{{"outerEvidence", 123}, {"br", teamContainer}};
  const auto teamBytes = QJsonDocument(teamEnvelope).toJson(QJsonDocument::Compact);
  const auto selectedTeamReplay = parse(teamBytes, catalog);
  check(selectedTeamReplay.error.isEmpty() && selectedTeamReplay.containerKind == "team" && selectedTeamReplay.selectedBattleIndex == 2 &&
    selectedTeamReplay.containerBattleCount == 4 && selectedTeamReplay.actions == 1 && selectedTeamReplay.steps.size() == 4,
    "team container was rejected, chose the wrong member, or played brs sequentially");
  check(selectedTeamReplay.original == teamBytes && selectedTeamReplay.root == teamEnvelope &&
    selectedTeamReplay.sha256 == QString::fromLatin1(QCryptographicHash::hash(teamBytes, QCryptographicHash::Sha256).toHex()),
    "team selection changed full-document original evidence or hash");
  check(selectedTeamReplay.battlePath == "/br/brs/2" && selectedTeamReplay.steps[0].path == "/br/brs/2/or" &&
    selectedTeamReplay.steps[2].path == "/br/brs/2/fr/0/rl/0" && selectedTeamReplay.steps[0].state[a].hp == 333 &&
    selectedTeamReplay.steps[0].title.contains(QStringLiteral("第 3/4 项")), "selected member lost original paths or reused another member's state");
  bool unselectedEvidence = false, selectedFieldLabel = false;
  for (const auto& field : selectedTeamReplay.fields) {
    unselectedEvidence |= field.path == "/br/brs/3/unparsed" && field.value.toString() == "retained";
    selectedFieldLabel |= field.path == "/br/brs/2/or/team/0/players/0/cts/0/hp" && field.verified;
  }
  check(unselectedEvidence && selectedFieldLabel, "team members were discarded or known child fields lost their scoped labels");
  const auto directTeamReplay = parse(QJsonDocument(teamContainer).toJson(), catalog);
  check(directTeamReplay.error.isEmpty() && directTeamReplay.battlePath == "/brs/2", "unwrapped team container lost its root path");
  auto brokenSelectedMember = selectedMember; brokenSelectedMember.remove("fr");
  const auto brokenTeam = parse(QJsonDocument(QJsonObject{{"tbt", 1},
    {"brs", QJsonArray{firstMember, brokenSelectedMember}}}).toJson(), catalog);
  check(!brokenTeam.error.isEmpty() && brokenTeam.selectedBattleIndex == 1 && brokenTeam.steps.isEmpty(),
    "invalid official selection silently fell back to an earlier member");
  const auto noPlayableMember = parse(QJsonDocument(QJsonObject{{"tbt", 1},
    {"brs", QJsonArray{QJsonObject{{"nn", "metadata only"}}}}}).toJson(), catalog);
  check(!noPlayableMember.error.isEmpty() && !noPlayableMember.fields.isEmpty(), "empty team container discarded its evidence or fabricated a battle");
  auto nullTeamType = firstMember; nullTeamType["tbt"] = QJsonValue(QJsonValue::Null); nullTeamType["brs"] = QJsonArray{};
  const auto ordinaryWithNullType = parse(QJsonDocument(nullTeamType).toJson(), catalog);
  check(ordinaryWithNullType.error.isEmpty() && ordinaryWithNullType.containerKind.isEmpty() && ordinaryWithNullType.actions == 1,
    "tbt:null incorrectly entered the team container branch");

  // Real captured reports currently have one nonempty line. These deliberately
  // synthetic reports exercise the official multi-line callback barriers; they
  // are not evidence for a particular real battle's cross-line completion time.
  const auto lineAction = [](int side, int change) {
    return QJsonObject{{"cti", side}, {"cpn", "tester"}, {"cci", 7}, {"si", 999999999}, {"ses", 1},
      {"cn", QJsonObject{{"bb_hps", QStringLiteral("99_%1_1").arg(change)}}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
  };
  const auto lineUnit = [](int lane, const QJsonArray& actions) {
    return QJsonObject{{"bfn", lane}, {"isf", !actions.isEmpty()}, {"pan", "tester"}, {"pbn", "tester"},
      {"pr", QJsonObject{{"dl", QJsonArray{}}, {"ml", QJsonArray{}}}}, {"rl", actions}};
  };
  const auto withFlow = [&](const QJsonArray& units) {
    auto report = firstMember; report["fr"] = units;
    return QJsonDocument(QJsonObject{{"br", report}}).toJson(QJsonDocument::Compact);
  };
  const auto byPath = [](const Replay& report, const QString& path) {
    for (const auto& step : report.steps) if (step.path == path) return step;
    return Step{};
  };
  const auto batchEnd = [](const Replay& report, int batch) {
    for (const auto& step : report.steps) if (step.batchId == batch && step.keyChanges.contains(QStringLiteral("同批结算"))) return step;
    return Step{};
  };
  auto l0a0 = lineAction(0, -10); l0a0["seqa"] = QJsonArray{7, 8}; l0a0["seqb"] = QJsonArray{7}; l0a0["pr"] = 2;
  auto l1a0 = lineAction(1, -20); l1a0["seqa"] = QJsonArray{8}; l1a0["seqb"] = QJsonArray{}; l1a0["pr"] = 9;
  const auto multiBytes = withFlow(QJsonArray{
    lineUnit(0, QJsonArray{l0a0, lineAction(0, -5)}),
    lineUnit(1, QJsonArray{l1a0, lineAction(1, -5), lineAction(1, -2)}),
    lineUnit(0, QJsonArray{lineAction(0, -3)})});
  const auto multi = parse(multiBytes, catalog);
  check(multi.error.isEmpty() && multi.issues.isEmpty() && multi.actions == 6,
    "valid independent multi-line commands were rejected or lost");
  QStringList multiOrder;
  for (const auto& step : multi.steps) if (!step.actor.isEmpty()) multiOrder.append(step.path);
  check(multiOrder == QStringList{"/br/fr/0/rl/0", "/br/fr/1/rl/0", "/br/fr/0/rl/1", "/br/fr/1/rl/1", "/br/fr/2/rl/0", "/br/fr/1/rl/2"},
    "parallel actions were flattened by fr instead of official slot batches");
  const auto firstLeft = byPath(multi, "/br/fr/0/rl/0"), firstRight = byPath(multi, "/br/fr/1/rl/0");
  check(firstLeft.parallelBatch && firstRight.parallelBatch && firstLeft.lineIndex == 0 && firstRight.lineIndex == 1 &&
    firstLeft.batchId == 0 && firstRight.batchId == 0 && firstLeft.batchStartState[a].hp == 100 && firstRight.batchStartState[a].hp == 100 &&
    firstLeft.state[a].hp == 90 && firstLeft.state[enemy].hp == 200 && firstRight.state[a].hp == 100 && firstRight.state[enemy].hp == 180,
    "same-batch line snapshots consumed the preceding line's result or lost the shared start");
  const auto firstSettled = batchEnd(multi, 0), lastSettled = batchEnd(multi, 2);
  check(firstSettled.state[a].hp == 90 && firstSettled.state[enemy].hp == 180 && firstSettled.actor.isEmpty() && firstSettled.effects.isEmpty() &&
    firstSettled.parallelConflictKeys.isEmpty() && firstSettled.parallelBatch && firstSettled.batchStartState[a].hp == 100,
    "disjoint line writes were not merged at an explicit non-skill batch end");
  check(lastSettled.state[a].hp == 82 && lastSettled.state[enemy].hp == 173 && multi.steps.last().state[a].hp == 82,
    "the next batch did not begin from the prior merged observation");
  const auto secondLeft = byPath(multi, "/br/fr/0/rl/1"), secondRight = byPath(multi, "/br/fr/1/rl/1");
  const auto replacementLeft = byPath(multi, "/br/fr/2/rl/0");
  check(secondLeft.actionQueues.value(0) == QStringList{"7", "8"} && secondRight.actionQueues.value(0) == QStringList{"8"} &&
    secondRight.knownActionQueues.contains(1) && secondRight.actionQueues.value(1).isEmpty() && replacementLeft.knownActionQueues.isEmpty() &&
    replacementLeft.actionQueues.isEmpty() && replacementLeft.actionQueuePlayers.value(0) == "tester",
    "interleaved actions leaked queues across original BattleUnit boundaries");
  check(secondLeft.playerRounds.value(0) == 2 && !secondLeft.playerRounds.contains(1) && secondRight.playerRounds.value(1) == 9 &&
    !secondRight.playerRounds.contains(0) && replacementLeft.playerRounds.value(0) == 2 && firstSettled.playerRounds.isEmpty() &&
    multi.steps.last().playerRounds.isEmpty(), "line round slots were presented as a serial global counter");
  check(multi.original == multiBytes && multi.root == QJsonDocument::fromJson(multiBytes).object() &&
    multi.sha256 == QString::fromLatin1(QCryptographicHash::hash(multiBytes, QCryptographicHash::Sha256).toHex()),
    "parallel planning changed the original document or digest");

  // A target-only record reads the same fighter another line writes. Do not
  // silently choose the lane order as the outcome, even without two HP deltas.
  auto readingAction = lineAction(1, 0); readingAction["cn"] = QJsonObject{};
  readingAction["trl"] = QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", false}, {"tn", QJsonObject{}}}};
  const auto readConflict = parse(withFlow(QJsonArray{lineUnit(0, QJsonArray{lineAction(0, -10)}), lineUnit(1, QJsonArray{readingAction})}), catalog);
  const auto readSettled = batchEnd(readConflict, 0);
  check(readConflict.error.isEmpty() && !readConflict.issues.isEmpty() && readSettled.parallelConflictKeys == QStringList{a} &&
    !readSettled.state[a].hp && !readSettled.state[a].maxHp && !readSettled.state[a].shield && !readSettled.state[a].buffsKnown &&
    !readSettled.state[a].entryKnown && readSettled.state[enemy].hp == 200,
    "cross-line write/read conflict was accepted as a known final state or invalidated an unrelated unit");
  check(byPath(readConflict, "/br/fr/0/rl/0").state[a].hp == 90 && byPath(readConflict, "/br/fr/1/rl/0").state[a].hp == 100 &&
    byPath(readConflict, "/br/fr/1/rl/0").parallelConflictKeys.contains(a),
    "conflict handling discarded independent line observations or failed to label them");
  auto writingAction = readingAction;
  writingAction["trl"] = QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", true},
    {"tn", QJsonObject{{"att_hps", "-20"}}}}};
  const auto writeConflict = parse(withFlow(QJsonArray{lineUnit(0, QJsonArray{lineAction(0, -10)}), lineUnit(1, QJsonArray{writingAction})}), catalog);
  check(batchEnd(writeConflict, 0).parallelConflictKeys == QStringList{a} && !writeConflict.steps.last().state[a].hp &&
    byPath(writeConflict, "/br/fr/1/rl/0").state[a].hp == 80,
    "parallel writes were summed in arbitrary line order or picked a last writer");

  auto impossibleLane = lineUnit(1, QJsonArray{lineAction(1, -1)}); impossibleLane["isf"] = false;
  const auto impossibleBytes = withFlow(QJsonArray{lineUnit(0, QJsonArray{lineAction(0, -1)}), impossibleLane});
  const auto impossible = parse(impossibleBytes, catalog);
  check(!impossible.error.isEmpty() && impossible.actions == 0 && impossible.original == impossibleBytes &&
    impossible.root == QJsonDocument::fromJson(impossibleBytes).object() && !impossible.fields.isEmpty(),
    "incomplete official flow was silently played serially or lost its raw evidence");

  // Empty bfn 1/2 setup slots are the ordinary captured shape. They must not
  // activate parallel merging or change the existing action's moment ledger.
  auto singleUnit = firstMember["fr"].toArray()[0].toObject(); singleUnit["bfn"] = 0; singleUnit["isf"] = true;
  const auto paddedSingle = parse(withFlow(QJsonArray{singleUnit, lineUnit(1, {}), lineUnit(2, {}), lineUnit(0, {})}), catalog);
  const auto paddedAction = byPath(paddedSingle, "/br/fr/0/rl/0");
  check(paddedSingle.error.isEmpty() && paddedSingle.actions == ordered.actions && paddedSingle.steps.size() == ordered.steps.size() + 3 &&
    !paddedAction.parallelBatch && paddedAction.batchStartState.isEmpty() && paddedAction.batchId == -1 &&
    paddedAction.moments.size() == actionStep.moments.size() && paddedAction.effects.size() == actionStep.effects.size(),
    "empty multi-line slots changed the single-line replay contract");
  for (int i = 0; i < std::min(paddedAction.moments.size(), actionStep.moments.size()); ++i) {
    const auto& actual = paddedAction.moments[i]; const auto& prior = actionStep.moments[i];
    check(actual.title == prior.title && actual.phase == prior.phase && actual.part == prior.part && actual.groupIndex == prior.groupIndex &&
      actual.state[a].hp == prior.state[a].hp && actual.state[a].shield == prior.state[a].shield && actual.state[a].buffs == prior.state[a].buffs &&
      actual.state[enemy].hp == prior.state[enemy].hp && actual.state[enemy].shield == prior.state[enemy].shield &&
      actual.effects.size() == prior.effects.size(), "single-line moments changed after the parallel parser refactor");
  }
  {
    // Ordinary-battle compatibility fixtures. All IDs/configurations below are
    // synthetic; they exercise official client branches without asserting new
    // facts about any actual pet or fabricating missing production data.
    auto compatCatalog = battleReplayBattlefieldCatalog();
    auto compatPets = compatCatalog["pets"].toObject();
    compatPets["9003"] = QJsonObject{{"name", "synthetic form C"}, {"maxPower", 100},
      {"identity", QJsonObject{{"status", "native"}, {"canonicalName", "synthetic form C"}}}};
    compatCatalog["pets"] = compatPets;
    const auto pet = [](int id, int race, int x = 0, int y = 0) {
      return QJsonObject{{"id", id}, {"raceId", race}, {"faceId", race}, {"x", x}, {"y", y},
        {"hp", 100}, {"maxHp", 100}, {"vigour", 100}, {"shp", 0}, {"attr", "1"},
        {"nskill", 100000 + race}, {"uskills", 200000 + race}, {"suskill", 300000 + race}};
    };
    const auto actionFor = [](int actor = 7) {
      return QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", actor}, {"si", 999999999}, {"ses", 1},
        {"cn", QJsonObject{}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
    };
    const auto ordinaryBytes = [](const QJsonArray& own, const QJsonArray& opposing, const QJsonArray& actions) {
      return QJsonDocument(QJsonObject{{"br", QJsonObject{{"win", 0},
        {"or", QJsonObject{{"team", QJsonArray{
          QJsonObject{{"id", 0}, {"players", QJsonArray{QJsonObject{{"name", "tester"}, {"cts", own}}}}},
          QJsonObject{{"id", 1}, {"players", QJsonArray{QJsonObject{{"name", "tester"}, {"cts", opposing}}}}}}}}},
        {"fr", QJsonArray{QJsonObject{{"bfn", 0}, {"isf", true}, {"pan", "tester"}, {"pbn", "tester"}, {"rl", actions}}}}}}}).toJson();
    };
    const auto transform = [](int race) {
      return QJsonObject{{"t", 1}, {"o", 2}, {"changeset", 1}, {"cr", race}, {"cf", race}, {"ca", "1"},
        {"cn", 100000 + race}, {"cu", QString::number(200000 + race)}, {"scu", 300000 + race}};
    };

    auto chaser = pet(9, 9003); chaser["icr"] = true;
    auto firstBody = pet(8, 9002); firstBody["oRaceId"] = 9001;
    auto orphan = pet(10, 9003, 1); orphan["icr"] = true;
    auto transformChaser = actionFor(9); transformChaser["cn"] = QJsonObject{{"rl", QJsonArray{transform(9001)}}};
    auto transformChaserAgain = transformChaser; transformChaserAgain["cn"] = QJsonObject{{"rl", QJsonArray{transform(9003)}}};
    auto transformBody = actionFor(8); transformBody["cn"] = QJsonObject{{"rl", QJsonArray{transform(9001),
      QJsonObject{{"t", 1}, {"o", 2}, {"changeset", 2}, {"cr", 9003}, {"cf", 9003}, {"ca", "1"},
        {"cn", 109003}, {"cu", "209003"}, {"scu", 309003}}}}};
    const auto originBytes = ordinaryBytes(QJsonArray{chaser, firstBody, pet(7, 9001), orphan},
      QJsonArray{pet(7, 9001, 1)}, QJsonArray{transformChaser, transformChaserAgain, transformBody});
    const auto origins = parse(originBytes, compatCatalog);
    const auto chaserKey = fighterKey(0, "tester", "9"), bodyKey = fighterKey(0, "tester", "8");
    const auto firstOrigin = origins.steps.first().state.value(chaserKey);
    check(origins.error.isEmpty() && origins.issues.isEmpty() && firstOrigin.originRaceId == "9002" &&
      firstOrigin.originFighterKey == bodyKey && firstOrigin.originRelation == "chaser" &&
      firstOrigin.originPath.endsWith("/cts/1/raceId"), "chaser origin did not use first same-player non-chaser in input cell order");
    check(firstOrigin.name == "synthetic form C" && firstOrigin.raceId == "9003" &&
      origins.steps.first().state[fighterKey(0, "tester", "10")].originRaceId.isEmpty(),
      "original-body association overwrote own identity, used oRaceId, or crossed team boundaries");
    const auto finalChaser = origins.steps.last().state.value(chaserKey), finalBody = origins.steps.last().state.value(bodyKey);
    check(finalChaser.originRaceId == "9002" && finalChaser.originFighterKey == bodyKey && finalChaser.originRelation == "chaser" &&
      finalChaser.raceId == "9003" && finalBody.originRaceId == "9002" && finalBody.originFighterKey == bodyKey &&
      finalBody.originRelation == "transform" && finalBody.raceId == "9003" && !finalBody.originPath.isEmpty(),
      "multiple transformations overwrote or lost the persistent original-body relationship");
    check(origins.original == originBytes && origins.root == QJsonDocument::fromJson(originBytes).object() &&
      origins.steps.first().state[bodyKey].originRaceId.isEmpty(), "origin association changed original evidence or earlier snapshots");

    auto absentShield = pet(7, 9001); absentShield.remove("shp");
    auto nullShield = pet(8, 9001); nullShield["shp"] = QJsonValue(QJsonValue::Null);
    auto invalidShield = pet(9, 9001); invalidShield["shp"] = "not-a-number";
    auto booleanShield = pet(10, 9001); booleanShield["shp"] = false;
    const auto shieldDefaults = parse(ordinaryBytes(QJsonArray{absentShield, nullShield, invalidShield, booleanShield}, {}, {}), compatCatalog);
    check(shieldDefaults.steps.first().state[a].shield == 0 && !shieldDefaults.steps.first().state[a].raw.contains("shp") &&
      !shieldDefaults.steps.first().state[fighterKey(0, "tester", "8")].shield &&
      !shieldDefaults.steps.first().state[fighterKey(0, "tester", "9")].shield &&
      !shieldDefaults.steps.first().state[fighterKey(0, "tester", "10")].shield,
      "missing initial shield did not default to zero or explicit null/invalid shield was guessed");

    auto lastCatalog = compatCatalog;
    auto lastSkills = lastCatalog["skills"].toObject(); auto lastBase = lastSkills["999999999"].toObject();
    lastBase["last"] = "1"; lastBase["playOrderType"] = "1"; lastSkills["999999999"] = lastBase;
    lastCatalog["skills"] = lastSkills;
    lastCatalog["skillLastEffects"] = QJsonObject{{"999999999", QJsonObject{{"action", "3"}, {"attackType", "远程"},
      {"skillBaseType", "召唤技"}, {"playOrderType", "0"}}}};
    auto lastAction = actionFor(); lastAction["cn"] = QJsonObject{{"bb_shps", "99_1_2"}};
    lastAction["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true},
      {"tn", QJsonObject{{"att_hps", "-10"}}}}};
    auto reserve = pet(7, 9002); reserve["ics"] = true; reserve["is"] = false;
    const auto dedicatedLast = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{reserve}, QJsonArray{lastAction, lastAction}), lastCatalog);
    const auto firstAction = byPath(dedicatedLast, "/br/fr/0/rl/0"), finalAction = byPath(dedicatedLast, "/br/fr/0/rl/1");
    check(dedicatedLast.issues.isEmpty() && !firstAction.isLastRound && firstAction.lastSkillStatus == "not-applicable" &&
      finalAction.isLastRound && finalAction.lastSkillStatus == "applied" && finalAction.playbackKind == "summon" &&
      finalAction.loggedSkillId == "999999999" && finalAction.playbackSkillId == "999999999" &&
      firstAction.state[enemy].hp == 90 && finalAction.state[enemy].hp == 90 && finalAction.state[enemy].entered,
      "last-effect definition did not select the final command or incorrectly changed logged skill/numeric state");
    bool baseSequenceKept = false;
    for (const auto& moment : finalAction.moments) for (const auto& effect : moment.effects)
      baseSequenceKept |= moment.phase == "before" && moment.groupIndex == 2 && effect.raw.toString() == "99_1_2";
    check(baseSequenceKept, "last-effect play order replaced the base definition's earlier fillWithMaxChangeSetId decision");
    auto counterLast = lastAction; auto counterTargets = counterLast["trl"].toArray(); auto counterTarget = counterTargets[0].toObject();
    counterTarget["issb"] = true; counterTarget["isse"] = false; counterTargets[0] = counterTarget; counterLast["trl"] = counterTargets;
    auto missingLastCatalog = lastCatalog; missingLastCatalog.remove("skillLastEffects");
    const auto counterExclusion = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{counterLast}), missingLastCatalog);
    check(counterExclusion.issues.isEmpty() && !byPath(counterExclusion, "/br/fr/0/rl/0").isLastRound &&
      byPath(counterExclusion, "/br/fr/0/rl/0").lastSkillStatus == "not-applicable",
      "any target counter flag failed to suppress last-round playback, including an ineffective target");
    auto disabledLast = lastAction; disabledLast["ses"] = -1;
    const auto disabledExclusion = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{disabledLast}), missingLastCatalog);
    check(disabledExclusion.issues.isEmpty() && byPath(disabledExclusion, "/br/fr/0/rl/0").isLastRound &&
      byPath(disabledExclusion, "/br/fr/0/rl/0").lastSkillStatus == "not-applicable" &&
      byPath(disabledExclusion, "/br/fr/0/rl/0").playbackKind == "disabled", "disabled command incorrectly requires a last-effect definition");
    auto missingLastAction = actionFor(); missingLastAction["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"},
      {"tci", 7}, {"isse", true}, {"tn", QJsonObject{{"att_hps", "-10"}, {"att_shps", "-5"}, {"ahp", 73}, {"avg", 60}}}}};
    const auto missingLastBytes = ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{missingLastAction});
    const auto missingLast = parse(missingLastBytes, missingLastCatalog);
    const auto missingLastStep = byPath(missingLast, "/br/fr/0/rl/0");
    check(missingLast.error.isEmpty() && !missingLast.issues.isEmpty() && missingLastStep.lastSkillStatus == "missing" &&
      missingLastStep.playbackKind == "unresolved" && !missingLastStep.playbackNotes.isEmpty() &&
      missingLastStep.moments.last().phase == "unresolved" && missingLastStep.state[enemy].hp == 73 &&
      missingLastStep.state[enemy].power == 60 && !missingLastStep.state[enemy].shield && missingLastStep.state[a].hp == 100 &&
      missingLast.original == missingLastBytes && missingLast.sha256 == QString::fromLatin1(QCryptographicHash::hash(missingLastBytes, QCryptographicHash::Sha256).toHex()),
      "missing dedicated last config was silently substituted, lost certain final observations, or changed original evidence");

    auto testingCatalog = compatCatalog; auto testingPets = testingCatalog["pets"].toObject(); auto testingPet = testingPets["9001"].toObject();
    testingPet["desc"] = QStringLiteral("内测"); testingPets["9001"] = testingPet; testingCatalog["pets"] = testingPets;
    auto testingSkills = testingCatalog["skills"].toObject();
    testingSkills["109999"] = QJsonObject{{"action", "3"}, {"attackType", "近身"}};
    testingSkills["109001"] = QJsonObject{{"action", "3"}, {"attackType", "近身"}};
    testingSkills["109002"] = QJsonObject{{"action", "3"}, {"attackType", "远程"}};
    testingCatalog["skills"] = testingSkills;
    auto testingAction = actionFor(); testingAction["si"] = 109001;
    const auto testingBytes = ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{testingAction});
    const auto testingReplay = parse(testingBytes, testingCatalog); const auto testingStep = byPath(testingReplay, "/br/fr/0/rl/0");
    check(testingReplay.issues.isEmpty() && testingStep.loggedSkillId == "109001" && testingStep.testingSkillId == "109999" &&
      testingStep.playbackSkillId == "109999" && testingStep.raw.toObject()["si"].toInt() == 109001 &&
      testingStep.state[a].raceId == "9001" && testingStep.state[a].name == "synthetic form A" && testingReplay.original == testingBytes,
      "official testing description failed to map the skill or overwrote logged pet/skill identity");
    auto nonTestingCatalog = testingCatalog; testingPet.remove("desc"); testingPet["name"] = QStringLiteral("内测");
    testingPets["9001"] = testingPet; nonTestingCatalog["pets"] = testingPets;
    const auto nameOnlyReplay = parse(testingBytes, nonTestingCatalog);
    check(byPath(nameOnlyReplay, "/br/fr/0/rl/0").testingSkillId.isEmpty() &&
      byPath(nameOnlyReplay, "/br/fr/0/rl/0").playbackSkillId == "109001", "testing was guessed from a name or absent dictionary description");
    auto comboTestingCatalog = testingCatalog;
    comboTestingCatalog["combos"] = QJsonArray{QJsonObject{{"skillId", "109999"}, {"buffs", "99"}, {"targetSkillIds", "109002"}}};
    auto buffedTestingPet = pet(7, 9001); buffedTestingPet["bfs"] = "99";
    const auto comboTestingReplay = parse(ordinaryBytes(QJsonArray{buffedTestingPet}, {}, QJsonArray{testingAction}), comboTestingCatalog);
    check(byPath(comboTestingReplay, "/br/fr/0/rl/0").testingSkillId == "109999" &&
      byPath(comboTestingReplay, "/br/fr/0/rl/0").playbackSkillId == "109002", "combo lookup ran before the official testing getter replacement");
    auto testingCounterAction = actionFor(); testingCounterAction["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"},
      {"tci", 7}, {"isse", true}, {"issb", true}, {"tn", QJsonObject{{"att_hps", "-1"}, {"sb_hps", "-1"}}}}};
    const auto testingCounter = parse(ordinaryBytes(QJsonArray{pet(7, 9002)}, QJsonArray{pet(7, 9001)}, QJsonArray{testingCounterAction}), testingCatalog);
    check(byPath(testingCounter, "/br/fr/0/rl/0").counterSkillId == "109999" &&
      byPath(testingCounter, "/br/fr/0/rl/0").state[enemy].normalSkill == "109001",
      "counter ignored FighterInfo's testing normal-skill getter or overwrote the stored skill");
    auto testingBodyChaser = pet(8, 9003); testingBodyChaser["icr"] = true;
    const auto testingOrigin = parse(ordinaryBytes(QJsonArray{testingBodyChaser, pet(7, 9001)}, {}, {}), testingCatalog);
    check(testingOrigin.steps.first().state[bodyKey].originRaceId == "9999" && testingOrigin.steps.first().state[a].raceId == "9001",
      "chaser setup did not use official testing race getter independently of original pet identity");
    auto changeTestingForm = testingAction; changeTestingForm["cn"] = QJsonObject{{"rl", QJsonArray{transform(9002)}}};
    auto afterTestingForm = actionFor(); afterTestingForm["si"] = 109002;
    const auto testingFormChanges = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{changeTestingForm, afterTestingForm}), testingCatalog);
    check(testingFormChanges.issues.isEmpty() && testingFormChanges.steps.last().state[a].originRaceId == "9001" &&
      testingFormChanges.steps.last().state[a].originRelation == "transform" &&
      byPath(testingFormChanges, "/br/fr/0/rl/1").testingSkillId.isEmpty() &&
      byPath(testingFormChanges, "/br/fr/0/rl/1").playbackSkillId == "109002",
      "transformation origin used the testing display race or testing mapping stayed attached to a previous form");

    auto emptyHit = actionFor(); auto emptyTarget = QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7},
      {"isse", true}, {"tn", QJsonObject{}}};
    emptyHit["trl"] = QJsonArray{emptyTarget};
    const auto emptyHitReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{emptyHit}), compatCatalog);
    const auto hasAttackBoundary = [](const Step& step) {
      for (const auto& moment : step.moments) if (moment.phase == "attack" && moment.part == "result") return true;
      return false;
    };
    const auto emptyHitStep = byPath(emptyHitReplay, "/br/fr/0/rl/0");
    check(emptyHitReplay.issues.isEmpty() && hasAttackBoundary(emptyHitStep) && emptyHitStep.state[a].hp == 100 &&
      emptyHitStep.state[enemy].hp == 100 && emptyHitStep.effects.size() == 1 && emptyHitStep.effects.first().kind == "target",
      "effective empty-number target lost its client-hint attack boundary or acquired fabricated numeric effects");
    emptyTarget["isse"] = false; emptyHit["trl"] = QJsonArray{emptyTarget};
    const auto ineffectiveEmpty = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{emptyHit}), compatCatalog);
    emptyTarget["isse"] = true; emptyTarget.remove("tn"); emptyHit["trl"] = QJsonArray{emptyTarget};
    const auto missingEmpty = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{emptyHit}), compatCatalog);
    check(!hasAttackBoundary(byPath(ineffectiveEmpty, "/br/fr/0/rl/0")) && !hasAttackBoundary(byPath(missingEmpty, "/br/fr/0/rl/0")),
      "missing tn or ineffective target manufactured an attack-result boundary");

    auto waitCatalog = compatCatalog; auto waitBuffs = waitCatalog["buffs"].toObject();
    waitBuffs["200"] = QJsonObject{{"nm", "synthetic serial wait"}, {"buffWaitType", 2}};
    waitBuffs["201"] = QJsonObject{{"nm", "synthetic force show"}, {"buffWaitType", 3}}; waitCatalog["buffs"] = waitBuffs;
    auto waitAction = actionFor(); waitAction["cn"] = QJsonObject{{"bd_bfs", "1_100_1,1_200_1,1_201_1,1_99_1"}};
    auto zeroHpPet = pet(7, 9001); zeroHpPet["hp"] = 0;
    const auto waitingZero = parse(ordinaryBytes(QJsonArray{zeroHpPet}, {}, QJsonArray{waitAction}), waitCatalog);
    const auto waitingAlive = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{waitAction}), waitCatalog);
    const auto zeroStep = byPath(waitingZero, "/br/fr/0/rl/0"), aliveStep = byPath(waitingAlive, "/br/fr/0/rl/0");
    QMap<QString, QString> conditions; bool forcedShow = false;
    for (const auto& effect : zeroStep.effects) if (effect.kind == "buff") {
      conditions[effect.causeId] = effect.buffWaitCondition;
      forcedShow |= effect.causeId == "201" && effect.visibilityCommand == "show-up";
    }
    check(waitingZero.issues.isEmpty() && conditions["100"] == "visible-or-skip" && conditions["200"] == "visible-or-skip" &&
      conditions["201"] == "force-show" && conditions["99"] == "none" && forcedShow &&
      zeroStep.state[a].hp == 0 && !zeroStep.keyChanges.contains(QStringLiteral("生命恢复")) && !zeroStep.playbackNotes.isEmpty() &&
      zeroStep.moments.size() == aliveStep.moments.size(),
      "wait policy inferred Avatar alpha from HP/hidden Buff, revived a unit, or omitted the explicit visibility condition");

    check(targetFighterId(QJsonValue(QJsonValue::Undefined)) == "@team" &&
      targetFighterId(QJsonValue(QJsonValue::Null)) == "@team" && targetFighterId(QJsonValue(2147483647)) == "@team" &&
      targetFighterId(QJsonValue("2147483647")) == "@team" && targetFighterId(QJsonValue(3275)) == "3275" &&
      targetFighterId(QJsonValue(-1)) == "-1", "official target sentinel was not unified or an unrelated ID was reclassified");
    const auto teamTargetAction = [&](const QJsonValue& id) {
      auto result = actionFor();
      QJsonObject target{{"tti", 1}, {"tpn", "tester"}, {"tn", QJsonObject{{"bd_bfs", "1_99_1"}}}};
      if (!id.isUndefined()) target["tci"] = id;
      result["prl"] = QJsonArray{target}; return result;
    };
    for (const auto& id : QList<QJsonValue>{QJsonValue(QJsonValue::Undefined), QJsonValue(QJsonValue::Null),
        QJsonValue(2147483647), QJsonValue("2147483647")}) {
      const auto report = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)},
        QJsonArray{teamTargetAction(id)}), compatCatalog);
      const auto action = byPath(report, "/br/fr/0/rl/0");
      check(report.issues.isEmpty() && action.unlinkedReferences.isEmpty() &&
        action.state.size() == report.steps.first().state.size() && action.state[team].buffs == QStringList{"99"},
        "null/missing/explicit sentinel did not update the same registered representer");
    }

    // Exact target object from battle-43817036008-s0-seq11554.json, action 14,
    // trl[8] (also repeated in the rematch). This is not a hypothetical pet:
    // no instance 3275 is registered in those reports' initial formations.
    const auto realReference = QJsonDocument::fromJson(R"({"tpn":"teamB","ishit":false,"isbl":false,"tti":1,"iscr":false,"issb":false,"tn":{"avg":0,"ashp":0.0,"ahp":0.0},"tci":3275,"tpi":2,"isse":false,"isIm":false})").object();
    const auto referenceBytes = [&](const QJsonArray& actions) {
      auto root = QJsonDocument::fromJson(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, actions)).object();
      auto battle = root["br"].toObject(); auto opening = battle["or"].toObject(); auto teams = opening["team"].toArray();
      auto opposing = teams[1].toObject(); auto players = opposing["players"].toArray(); auto player = players[0].toObject();
      player["name"] = "teamB"; players[0] = player; opposing["players"] = players; teams[1] = opposing;
      opening["team"] = teams; battle["or"] = opening; root["br"] = battle; return QJsonDocument(root).toJson();
    };
    auto referenceAction = actionFor();
    const QJsonObject registeredTarget{{"tti", 1}, {"tpn", "teamB"}, {"tci", 7}, {"isse", true},
      {"tn", QJsonObject{{"att_hps", "-10"}}}};
    referenceAction["trl"] = QJsonArray{registeredTarget, realReference};
    const auto referenceInput = referenceBytes(QJsonArray{referenceAction, referenceAction});
    const auto referenceReplay = parse(referenceInput, compatCatalog);
    const auto unlinkedKey = fighterKey(1, "teamB", "3275"), registeredKey = fighterKey(1, "teamB", "7");
    check(referenceReplay.error.isEmpty() && referenceReplay.issues.size() == 2 && referenceReplay.original == referenceInput &&
      referenceReplay.sha256 == QString::fromLatin1(QCryptographicHash::hash(referenceInput, QCryptographicHash::Sha256).toHex()) &&
      referenceReplay.root == QJsonDocument::fromJson(referenceInput).object(),
      "real-derived unlinked target changed original evidence or was not diagnosed per occurrence");
    for (const auto& action : referenceReplay.steps) {
      check(!action.state.contains(unlinkedKey) && action.state.size() == referenceReplay.steps.first().state.size(),
        "unlinked instance became a permanent fighter snapshot");
      for (const auto& moment : action.moments)
        check(!moment.state.contains(unlinkedKey), "unlinked instance polluted an internal moment");
      for (const auto& effect : action.effects)
        check(effect.target != unlinkedKey && effect.source != unlinkedKey, "unlinked target acquired an invented relation or state effect");
    }
    for (int actionIndex = 0; actionIndex < 2; ++actionIndex) {
      const auto action = byPath(referenceReplay, QStringLiteral("/br/fr/0/rl/%1").arg(actionIndex));
      check(action.unlinkedReferences.size() == 1 && action.playbackKind == "normal" &&
        action.state[registeredKey].hp == 90 - actionIndex * 10 && action.state[a].hp == 100,
        "inactive unlinked reference invalidated registered fighters or disappeared from a later action");
      if (action.unlinkedReferences.size() != 1) continue;
      const auto& reference = action.unlinkedReferences.first();
      check(reference.kind == "target" && reference.key == unlinkedKey && reference.id == "3275" && reference.team == 1 &&
        reference.player == "teamB" && reference.path == action.path + "/trl/1" && reference.raw == realReference &&
        reference.numbers == realReference["tn"].toObject() && reference.flags.size() == 6 && !reference.flags["isse"] &&
        !reference.reason.isEmpty(), "unlinked reference lost its full target, numbers, explicit flags, identity or path");
    }
    bool keptUnlinkedField = false;
    for (const auto& field : referenceReplay.fields)
      keptUnlinkedField |= field.path == "/br/fr/0/rl/0/trl/1/tn/ahp" && field.value == QJsonValue(0);
    check(keptUnlinkedField, "isolating an unlinked target dropped raw field inspection");
    // Synthetic nonempty numbers on an unregistered target still determine
    // the official after-group count. Its own shield is never reconstructed,
    // but a registered source's absolute calibration must retain that boundary.
    auto groupedReference = realReference; groupedReference["tn"] = QJsonObject{{"ab_shps", "99_10_1"}};
    auto groupedReferenceAction = actionFor(); groupedReferenceAction["cn"] = QJsonObject{{"ahp", 70}};
    groupedReferenceAction["trl"] = QJsonArray{groupedReference};
    const auto groupedReferences = parse(referenceBytes(QJsonArray{groupedReferenceAction}), compatCatalog);
    const auto groupedReferenceStep = byPath(groupedReferences, "/br/fr/0/rl/0");
    check(groupedReferenceStep.state[a].hp == 70 && !groupedReferenceStep.state.contains(unlinkedKey) &&
      groupedReferenceStep.unlinkedReferences.size() == 1 && groupedReferenceStep.effects.size() == 1 &&
      groupedReferenceStep.effects.first().cause == "calibration", "unlinked record isolation changed registered calibration group boundaries");
    auto unlinkedTeamTarget = realReference; unlinkedTeamTarget.remove("isse"); unlinkedTeamTarget["ishit"] = 1;
    auto teamReferenceAction = actionFor(); teamReferenceAction["prl"] = QJsonArray{unlinkedTeamTarget};
    const auto teamReferences = parse(referenceBytes(QJsonArray{teamReferenceAction}), compatCatalog);
    const auto teamReferenceStep = byPath(teamReferences, "/br/fr/0/rl/0");
    check(teamReferenceStep.unlinkedReferences.size() == 1 && teamReferenceStep.unlinkedReferences.first().kind == "team-target" &&
      !teamReferenceStep.unlinkedReferences.first().flags.contains("isse") &&
      !teamReferenceStep.unlinkedReferences.first().flags.contains("ishit") && teamReferenceStep.effects.isEmpty(),
      "unlinked prl was treated as an attack or missing/non-boolean target flags acquired invented values");

    // Direct command reconstruction must be just as strict as parse(),
    // including independent t=3 references and same-player t=5 clone IDs.
    auto directState = referenceReplay.steps.first().state;
    auto unregisteredActor = referenceAction; unregisteredActor["cci"] = 999;
    unregisteredActor["cn"] = QJsonObject{{"att_hps", "-10"}};
    Step directUnknown; directUnknown.actor = fighterKey(0, "tester", "999");
    directUnknown.path = "/synthetic/unregistered-actor"; directUnknown.raw = unregisteredActor;
    QStringList directIssues; advanceAction(directUnknown, directState, compatCatalog, directIssues);
    check(directUnknown.playbackKind == "unresolved" && directUnknown.unlinkedReferences.size() == 2 &&
      !directState.contains(directUnknown.actor) && !directState.contains(unlinkedKey) && !directState[registeredKey].hp &&
      directState.size() == referenceReplay.steps.first().state.size(),
      "direct Engine use created an unregistered actor/target or treated unapplied damage as a known balance");
    auto unknownUnitAction = actionFor(); unknownUnitAction["cn"] = QJsonObject{{"rl", QJsonArray{
      QJsonObject{{"t", 3}, {"o", 1}, {"changeset", 1}, {"ti", 1}, {"pn", "teamB"}, {"ci", 800}},
      QJsonObject{{"t", 5}, {"o", 2}, {"changeset", 1}, {"ci", 801}}}}};
    Step directUnits; directUnits.actor = a; directUnits.path = "/synthetic/unregistered-units"; directUnits.raw = unknownUnitAction;
    directState = referenceReplay.steps.first().state; QStringList unitIssues;
    advanceAction(directUnits, directState, compatCatalog, unitIssues);
    check(directUnits.unlinkedReferences.size() == 2 && directState.size() == referenceReplay.steps.first().state.size() &&
      directState[a].hp == 100 && directUnits.effects.isEmpty(), "unregistered special-record references became summoned or cloned fighters");

    // Four source HP effects cross both boundaries inside ONE attack Moment.
    // Deduplication is local to that moment and preserves first-event order.
    auto pulseAction = actionFor(); pulseAction["cn"] = QJsonObject{{"att_hps", "-100,100,-100,100"}};
    pulseAction["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"tn", QJsonObject{}}}};
    const auto pulseReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{pulseAction, pulseAction}), compatCatalog);
    for (int actionIndex = 0; actionIndex < 2; ++actionIndex) {
      const auto action = byPath(pulseReplay, QStringLiteral("/br/fr/0/rl/%1").arg(actionIndex));
      check(action.state[a].hp == 100 && action.events.size() == 2 &&
        action.events[0].kind == QStringLiteral("生命归零") && action.events[1].kind == QStringLiteral("生命恢复") &&
        action.events[0].key == a && action.events[1].key == a && action.events[0].moment == action.events[1].moment &&
        action.moments[action.events[0].moment].phase == "attack", "net-zero intra-moment HP crossings were lost, reordered or duplicated");
    }
    auto formPulse = actionFor(); formPulse["cn"] = QJsonObject{{"rl", QJsonArray{transform(9002), transform(9001)}}};
    const auto formPulseReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{formPulse}), compatCatalog);
    const auto formPulseStep = byPath(formPulseReplay, "/br/fr/0/rl/0");
    check(formPulseStep.state[a].raceId == "9001" && formPulseStep.events.size() == 1 &&
      formPulseStep.events.first().kind == QStringLiteral("变身") &&
      formPulseStep.moments[formPulseStep.events.first().moment].part == "objects",
      "two same-group transformations returning to the original form disappeared from key events");
    const auto resetUnit = [](int current, int maximum) {
      return QJsonObject{{"t", 2}, {"o", 2}, {"changeset", 1}, {"new_curv", current}, {"new_maxv", maximum}};
    };
    auto resetPulse = actionFor(); resetPulse["cn"] = QJsonObject{{"rl", QJsonArray{
      resetUnit(0, 100), resetUnit(100, 120), resetUnit(100, 100)}}};
    const auto resetPulseReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{resetPulse}), compatCatalog);
    const auto resetPulseStep = byPath(resetPulseReplay, "/br/fr/0/rl/0");
    check(resetPulseStep.state[a].hp == 100 && resetPulseStep.state[a].maxHp == 100 && resetPulseStep.events.size() == 3 &&
      resetPulseStep.events[0].kind == QStringLiteral("生命归零") && resetPulseStep.events[1].kind == QStringLiteral("生命恢复") &&
      resetPulseStep.events[2].kind == QStringLiteral("生命上限变化") && resetPulseStep.events[0].moment == resetPulseStep.events[2].moment,
      "same-group HP resets lost transient crossings/max changes or reordered their effects");
    check(resetPulseStep.state[a].hpPath.endsWith("/rl/2/new_curv") &&
      resetPulseStep.state[a].maxHpPath.endsWith("/rl/2/new_maxv"),
      "HP reset provenance points to initial values rather than the latest explicit reset fields");
    auto invalidReset = resetUnit(100, 100); invalidReset.remove("new_maxv"); invalidReset["new_curv"] = QJsonValue::Null;
    auto invalidResetAction = actionFor(); invalidResetAction["cn"] = QJsonObject{{"rl", QJsonArray{invalidReset}}};
    const auto invalidResetReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{invalidResetAction}), compatCatalog);
    const auto invalidResetFighter = byPath(invalidResetReplay, "/br/fr/0/rl/0").state[a];
    check(!invalidResetFighter.hp && !invalidResetFighter.maxHp && invalidResetFighter.hpPath.endsWith("/rl/0/new_curv") &&
      invalidResetFighter.maxHpPath.endsWith("/rl/0/new_maxv"), "invalid reset loses exact unknown-value provenance");

    auto duplicateHp = actionFor(); duplicateHp["cn"] = QJsonObject{{"bb_hps", "99_-10_1,99_-20_1,99_-5_2"}, {"ahp", 65}};
    const auto duplicateReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002)}, QJsonArray{duplicateHp}), compatCatalog);
    const auto duplicateStep = byPath(duplicateReplay, "/br/fr/0/rl/0");
    check(!duplicateReplay.issues.isEmpty() && duplicateStep.playbackKind == "unresolved" && !duplicateStep.playbackNotes.isEmpty() &&
      duplicateStep.state[a].hp == 65 && duplicateStep.state[enemy].hp == 100 &&
      duplicateStep.raw.toObject()["cn"].toObject()["bb_hps"].toString() == "99_-10_1,99_-20_1,99_-5_2",
      "duplicate HP group IDs were silently deduplicated/replayed or lost the absolute final observation");
    for (const auto& effect : duplicateStep.effects)
      check(effect.cause != "buff", "ambiguous duplicate HP groups were nevertheless applied in a fabricated order");
    auto noFinal = duplicateHp; auto noFinalNumbers = noFinal["cn"].toObject(); noFinalNumbers.remove("ahp"); noFinal["cn"] = noFinalNumbers;
    const auto noFinalReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{noFinal}), compatCatalog);
    check(!byPath(noFinalReplay, "/br/fr/0/rl/0").state[a].hp, "unconfirmed duplicate-group damage left a false known HP balance");
    auto repeatedShield = actionFor(); repeatedShield["cn"] = QJsonObject{{"bb_shps", "99_10_1,99_20_1"}};
    const auto shieldGroups = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, {}, QJsonArray{repeatedShield}), compatCatalog);
    check(shieldGroups.issues.isEmpty() && byPath(shieldGroups, "/br/fr/0/rl/0").state[a].shield == 30,
      "HP-only duplicate-index limitation incorrectly rejected ordinary same-group shield records");

    auto multipleClose = actionFor(); multipleClose["trl"] = QJsonArray{
      QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"tn", QJsonObject{{"att_hps", "-10"}, {"ahp", 90}}}},
      QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 8}, {"isse", true}, {"tn", QJsonObject{{"att_hps", "-20"}, {"ahp", 80}}}}};
    const auto multipleReplay = parse(ordinaryBytes(QJsonArray{pet(7, 9001)}, QJsonArray{pet(7, 9002), pet(8, 9002, 1)},
      QJsonArray{multipleClose}), compatCatalog);
    const auto multipleStep = byPath(multipleReplay, "/br/fr/0/rl/0");
    check(!multipleReplay.issues.isEmpty() && multipleStep.playbackKind == "unresolved" && !hasAttackBoundary(multipleStep) &&
      multipleStep.state[enemy].hp == 90 && multipleStep.state[fighterKey(1, "tester", "8")].hp == 80 &&
      multipleStep.state[a].hp == 100, "unsupported close multi-target command invented attack order or lost final observations");
    auto activeUnlinked = realReference; activeUnlinked["isse"] = true;
    auto activeReferenceAction = actionFor(); activeReferenceAction["cn"] = QJsonObject{{"att_hps", "-10"}, {"ahp", 90}};
    activeReferenceAction["trl"] = QJsonArray{activeUnlinked};
    const auto activeReferenceReplay = parse(referenceBytes(QJsonArray{activeReferenceAction}), compatCatalog);
    const auto activeReferenceStep = byPath(activeReferenceReplay, "/br/fr/0/rl/0");
    check(activeReferenceStep.playbackKind == "unresolved" && !hasAttackBoundary(activeReferenceStep) &&
      activeReferenceStep.state[a].hp == 90 && !activeReferenceStep.state.contains(unlinkedKey),
      "a close command that cannot locate its effective target was presented as a completed attack");
  }
  for (const auto& error : BattleReplayFixture::empowermentRuleFailures()) check(false, error.toUtf8().constData());
  qInfo() << "battle replay protocol checks:" << (failures ? "FAILED" : "PASS");
  return failures ? 1 : 0;
}
