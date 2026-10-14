#include "domain/battle_replay_dragon.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>

using namespace BattleReplay;

namespace {

QJsonObject syntheticCatalog() {
  return {{"petDragons", QJsonObject{
    {"qualities", QJsonObject{{"1", "C"}, {"2", "B"}, {"3", "A"}, {"4", "S"}}},
    {"definitions", QJsonObject{
      {"1", QJsonObject{{"name", "synthetic dragon one"}, {"skillType", "synthetic display"},
        {"descriptionTemplate", "synthetic [0.02*N]% / [2*N]"}, {"source", QJsonObject{{"path", "synthetic-definition"}, {"line", 7}}}}},
      {"2", QJsonObject{{"name", "synthetic dragon two"}, {"descriptionTemplate", "synthetic [0.0125*N]-[0.025*N]"}}}
    }}
  }}};
}

Fighter player(const QJsonValue& dragons, int team = 0) {
  Fighter owner; owner.team = team; owner.player = QStringLiteral("synthetic-player"); owner.representer = true;
  owner.key = fighterKey(team, owner.player, QStringLiteral("@team"));
  owner.path = QStringLiteral("/br/or/team/%1/players/0").arg(team);
  owner.raw = QJsonObject{{"drs", dragons}}; return owner;
}

Step command(const Fighter& owner, const QJsonValue& ids, int index, const QString& kind = QStringLiteral("team-skill")) {
  Step step; step.actor = owner.key; step.path = QStringLiteral("/br/fr/0/rl/%1").arg(index);
  step.playbackKind = kind; step.playbackSkillId = QStringLiteral("990002");
  step.raw = QJsonObject{{"cti", owner.team}, {"cpn", owner.player}, {"epbi", 900}, {"eps", ids}, {"csn", index + 1}};
  return step;
}

bool states(const DragonRoster& roster, DragonState first, DragonState second) {
  return roster.dragons.size() == 2 && roster.dragons[0].state == first && roster.dragons[1].state == second;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv); int failures = 0;
  const auto check = [&](bool ok, const char* reason) { if (!ok) { qCritical() << reason; ++failures; } };
  // Test identities/names/values are deliberately synthetic. The field meanings,
  // case-sensitive aa test, description formatting and eps[0] follow the client.
  const QJsonArray rawDragons{
    QJsonObject{{"di", 101}, {"dfi", 1}, {"q", 4}, {"nv", 4999}, {"ge", "aaAABB"}, {"dbfi", 900}},
    QJsonObject{{"di", 202}, {"dfi", 2}, {"q", 1}, {"nv", 2000}, {"ge", "AaAABB"}, {"dbfi", 900}}
  };
  const auto owner = player(rawDragons), enemy = player(rawDragons, 1);
  const auto catalog = syntheticCatalog();
  const auto original = QJsonDocument(owner.raw).toJson(QJsonDocument::Compact);
  Replay replay; replay.steps.append(Step{});
  replay.steps.append(command(owner, QJsonArray{101, 202}, 0));
  replay.steps.append(command(enemy, QJsonArray{101}, 1));
  replay.steps.append(command(owner, QJsonArray{}, 2));
  replay.steps.append(command(owner, QJsonArray{202}, 3));
  auto roster = dragonRosterAt(replay, owner, 0, DragonCommandProgress::Complete, catalog);
  check(roster.rosterKnown && roster.issues.isEmpty() && states(roster, DragonState::Ready, DragonState::Ready), "initial dragon roster/status invalid");
  if (roster.dragons.size() != 2) return 1;
  const auto& first = roster.dragons[0]; const auto& second = roster.dragons[1];
  check(first.id == "101" && first.defineId == "1" && first.name == "synthetic dragon one" && first.definitionKnown &&
    first.quality == 4 && first.qualityName == "S" && first.skillValue == 4999 && first.executeBufferId == "900", "dragon fields or official table identity lost");
  check(first.geneRecorded && first.gene == "aaAABB" && first.glittery == true && second.glittery == false && second.qualityName == "C",
    "gene casing or quality mapping differs from recorded data");
  check(first.descriptionComplete && first.description == "synthetic 99% / 9998" && first.descriptionTemplate == "synthetic [0.02*N]% / [2*N]" &&
    second.description == "synthetic 25-50" && first.definitionSource["path"] == "synthetic-definition", "description formatting rounded or lost source/template");
  check(first.path == owner.path + "/drs/0" && first.raw == rawDragons[0] && roster.raw == rawDragons, "dragon raw evidence/path not retained");

  roster = dragonRosterAt(replay, owner, 1, DragonCommandProgress::Before, catalog);
  check(states(roster, DragonState::Ready, DragonState::Ready) && roster.activeId.isEmpty(), "preparation leaked current command completion");
  roster = dragonRosterAt(replay, owner, 1, DragonCommandProgress::Active, catalog);
  check(states(roster, DragonState::Fighting, DragonState::Ready) && roster.activeId == "101" &&
    roster.executeIds == QStringList{"101", "202"} && roster.executeRaw == QJsonArray{101, 202} && roster.executionSkillId == "990002",
    "eps list marked extra dragons, used dbfi instead of instance, or lost recorded IDs");
  check(roster.dragons[0].lastAction == 1 && roster.dragons[0].lastActionPath == replay.steps[1].path &&
    roster.dragons[0].lastSkillId == replay.steps[1].playbackSkillId && roster.dragons[0].lastCompletionAction == -1,
    "dragon execution lost step/skill provenance or leaked future completion");
  roster = dragonRosterAt(replay, owner, 1, DragonCommandProgress::Complete, catalog);
  check(states(roster, DragonState::Finished, DragonState::Ready) && roster.activeId.isEmpty() &&
    roster.dragons[0].lastCompletionAction == 1 && roster.dragons[0].lastCompletionPath == replay.steps[1].path,
    "official completion gate affected wrong dragon or lost callback provenance");
  roster = dragonRosterAt(replay, owner, 2, DragonCommandProgress::Complete, catalog);
  check(states(roster, DragonState::Finished, DragonState::Ready), "enemy's same instance ID affected own dragon");
  roster = dragonRosterAt(replay, owner, 3, DragonCommandProgress::Complete, catalog);
  check(states(roster, DragonState::Finished, DragonState::Ready), "empty eps inferred next dragon from matching execution Buff");
  roster = dragonRosterAt(replay, owner, 4, DragonCommandProgress::Before, catalog);
  check(states(roster, DragonState::Finished, DragonState::Ready), "second dragon preparation lost prior command state");
  roster = dragonRosterAt(replay, owner, 4, DragonCommandProgress::Active, catalog);
  check(states(roster, DragonState::Finished, DragonState::Fighting) && roster.activeId == "202", "explicit next execution did not start named dragon");
  roster = dragonRosterAt(replay, owner, 4, DragonCommandProgress::Complete, catalog);
  check(states(roster, DragonState::Finished, DragonState::Finished), "completed executions not retained");
  check(states(dragonRosterAt(replay, owner, 0, DragonCommandProgress::Complete, catalog), DragonState::Ready, DragonState::Ready) &&
    states(dragonRosterAt(replay, owner, 1, DragonCommandProgress::Before, catalog), DragonState::Ready, DragonState::Ready),
    "backward seek retained future dragon state");
  check(QJsonDocument(owner.raw).toJson(QJsonDocument::Compact) == original, "dragon reconstruction mutated raw player data");

  for (const auto& ids : {QJsonValue(QJsonArray{999, 101}), QJsonValue(QJsonArray{0, 101}), QJsonValue(QJsonArray{-1, 101}),
      QJsonValue(QJsonArray{QJsonValue::Null, 101}), QJsonValue("101")}) {
    Replay invalid; invalid.steps.append(command(owner, ids, 0));
    roster = dragonRosterAt(invalid, owner, 0, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Ready, DragonState::Ready) && !roster.issues.isEmpty() && roster.executeRaw == ids,
      "invalid/unmatched eps fell back to another dragon or lost original value");
  }
  for (const auto& kind : {QStringLiteral("normal"), QStringLiteral("unresolved")}) {
    Replay records; records.steps.append(command(owner, QJsonArray{101}, 0, kind));
    roster = dragonRosterAt(records, owner, 0, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Ready, DragonState::Ready) && !roster.issues.isEmpty(), "non-dragon command fabricated a confirmed execution");
  }
  {
    Replay callbacks;
    callbacks.steps.append(command(owner, QJsonArray{101}, 0));
    auto sameRound = command(owner, QJsonArray{202}, 1);
    auto raw = sameRound.raw.toObject(); raw["csn"] = 1; sameRound.raw = raw;
    callbacks.steps.append(sameRound);
    roster = dragonRosterAt(callbacks, owner, 1, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Fighting) && roster.activeId == "202" &&
      roster.dragons[1].lastAction == 1 && roster.dragons[1].lastCompletionAction == -1,
      "same-round callback bypassed official completion gate");
    callbacks.steps.append(command(owner, QJsonArray{101}, 2, QStringLiteral("team-simple")));
    roster = dragonRosterAt(callbacks, owner, 2, DragonCommandProgress::Active, catalog);
    check(states(roster, DragonState::Finished, DragonState::Fighting) && roster.dragons[0].lastAction == 0,
      "simple command fabricated START or selected its eps instance");
    roster = dragonRosterAt(callbacks, owner, 2, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Finished) && roster.dragons[1].lastCompletionAction == 2 &&
      roster.dragons[1].lastAction == 1 && roster.dragons[1].lastSkillId == sameRound.playbackSkillId,
      "simple FINISHED did not finish the existing slot, or was relabelled a dragon skill");
    callbacks.steps.append(command(owner, QJsonArray{101}, 3, QStringLiteral("team-simple")));
    roster = dragonRosterAt(callbacks, owner, 3, DragonCommandProgress::Complete, catalog);
    check(roster.dragons[1].lastCompletionAction == 2, "finished queue accepted another simple callback");
    check(states(dragonRosterAt(callbacks, owner, 1, DragonCommandProgress::Before, catalog), DragonState::Finished, DragonState::Ready),
      "backward seek retained same-round START or later simple completion");
  }
  {
    Replay callbacks;
    auto simple = command(owner, QJsonArray{202}, 0, QStringLiteral("team-simple"));
    auto raw = simple.raw.toObject(); raw.remove("csn"); simple.raw = raw;
    callbacks.steps.append(simple);
    roster = dragonRosterAt(callbacks, owner, 0, DragonCommandProgress::Active, catalog);
    check(states(roster, DragonState::Ready, DragonState::Ready) && roster.activeId.isEmpty(), "simple branch invented a START hint");
    roster = dragonRosterAt(callbacks, owner, 0, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Ready) && roster.dragons[0].lastAction == -1 &&
      roster.dragons[0].lastSkillId.isEmpty() && roster.dragons[0].lastCompletionAction == 0,
      "simple callback did not use initial slot, or invented an unrecorded dragon attack");
    raw["csn"] = QJsonValue::Null; simple.raw = raw; callbacks.steps.append(simple);
    roster = dragonRosterAt(callbacks, owner, 1, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Ready), "missing/null csn did not share official default-zero gate");
    callbacks.steps.append(command(owner, QJsonArray{202}, 2));
    roster = dragonRosterAt(callbacks, owner, 2, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Finished), "later explicit START failed after a simple completion");
  }
  {
    Replay reverse;
    reverse.steps.append(command(owner, QJsonArray{202, 101}, 0));
    roster = dragonRosterAt(reverse, owner, 0, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Ready, DragonState::Finished) && roster.dragons[0].lastAction == -1,
      "eps[0] did not select second slot independently of dbfi/default slot");
    reverse.steps.append(command(owner, QJsonArray{101}, 1));
    roster = dragonRosterAt(reverse, owner, 1, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Ready) && roster.dragons[1].lastCompletionAction == 0,
      "official next-slot ready reset was replaced by cumulative completion guesses");
    reverse.steps.append(command(owner, QJsonArray{999}, 2));
    reverse.steps.append(command(owner, QJsonArray{202}, 3, QStringLiteral("team-simple")));
    roster = dragonRosterAt(reverse, owner, 3, DragonCommandProgress::Complete, catalog);
    check(states(roster, DragonState::Finished, DragonState::Ready) && !roster.issues.isEmpty(),
      "simple callback guessed a slot after unmatched START");
  }
  Replay parallel; auto parallelCommand = command(owner, QJsonArray{101}, 0); parallelCommand.parallelBatch = true; parallel.steps.append(parallelCommand);
  roster = dragonRosterAt(parallel, owner, 0, DragonCommandProgress::Complete, catalog);
  check(states(roster, DragonState::Ready, DragonState::Ready) && !roster.issues.isEmpty(), "unsupported parallel command was silently serialized");

  auto incomplete = rawDragons[0].toObject(); incomplete["dfi"] = 999; incomplete.remove("ge"); incomplete["q"] = 17;
  roster = dragonRosterAt(replay, player(QJsonArray{incomplete}), -1, DragonCommandProgress::Before, catalog);
  check(roster.dragons[0].name == QStringLiteral("小神龙 [999]") && !roster.dragons[0].definitionKnown &&
    roster.dragons[0].quality == 17 && roster.dragons[0].qualityName == QStringLiteral("未知") &&
    !roster.dragons[0].geneRecorded && !roster.dragons[0].glittery && roster.dragons[0].description.isEmpty(),
    "missing definition/gene/quality invented dragon data");
  incomplete["dfi"] = 1; incomplete.remove("nv"); incomplete["ge"] = 42;
  roster = dragonRosterAt(replay, player(QJsonArray{incomplete}), -1, DragonCommandProgress::Before, catalog);
  check(!roster.dragons[0].skillValue && !roster.dragons[0].descriptionComplete && roster.dragons[0].description.contains('N') &&
    !roster.dragons[0].glittery && !roster.dragons[0].issues.isEmpty(), "missing parameter or malformed gene became zero/ordinary");
  for (const auto& gene : {QStringLiteral("aa"), QStringLiteral("aA"), QStringLiteral("a"), QString()}) {
    incomplete["ge"] = gene;
    roster = dragonRosterAt(replay, player(QJsonArray{incomplete}), -1, DragonCommandProgress::Before, catalog);
    check(roster.dragons[0].geneRecorded && roster.dragons[0].glittery == (gene == "aa"), "gene prefix/case was inferred incorrectly");
  }
  auto unsupportedCatalog = catalog; auto table = unsupportedCatalog["petDragons"].toObject(); auto definitions = table["definitions"].toObject();
  auto definition = definitions["1"].toObject(); definition["descriptionTemplate"] = "synthetic [N+1]"; definitions["1"] = definition;
  table["definitions"] = definitions; unsupportedCatalog["petDragons"] = table;
  roster = dragonRosterAt(replay, owner, -1, DragonCommandProgress::Before, unsupportedCatalog);
  check(!roster.dragons[0].descriptionComplete && roster.dragons[0].description == "synthetic [4999+1]", "unsupported display formula invented a result");
  roster = dragonRosterAt(replay, player(QJsonObject{{"di", 101}}), -1, DragonCommandProgress::Before, catalog);
  check(!roster.rosterKnown && roster.dragons.isEmpty() && !roster.issues.isEmpty(), "non-array roster was silently accepted");
  roster = dragonRosterAt(replay, player(QJsonArray{}), -1, DragonCommandProgress::Before, catalog);
  check(roster.rosterKnown && roster.dragons.isEmpty() && roster.issues.isEmpty(), "known empty roster lost its distinction");
  roster = dragonRosterAt(replay, player(QJsonValue::Null), -1, DragonCommandProgress::Before, catalog);
  check(!roster.rosterKnown && roster.dragons.isEmpty(), "unrecorded roster treated as known empty");

  qInfo() << "dragon reconstruction failures" << failures;
  return failures ? 1 : 0;
}
