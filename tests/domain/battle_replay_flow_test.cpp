#include "domain/battle_replay_flow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

using namespace BattleReplay;

namespace {

QJsonObject segment(int line, int count, const QString& player, bool isFight = true,
                    const QJsonArray& deletions = {}, const QJsonArray& moves = {}) {
  QJsonArray actions;
  for (int i = 0; i < count; ++i)
    actions.append(QJsonObject{{QStringLiteral("record"), player + QString::number(i)}});
  return {{QStringLiteral("bfn"), line}, {QStringLiteral("isf"), isFight},
    {QStringLiteral("pan"), player}, {QStringLiteral("pbn"), player + QStringLiteral("-enemy")},
    {QStringLiteral("rl"), actions},
    {QStringLiteral("pr"), QJsonObject{{QStringLiteral("dl"), deletions}, {QStringLiteral("ml"), moves}}}};
}

QStringList actionsOf(const BattleFlowPlan& plan) {
  QStringList result;
  for (const auto& item : plan.items) if (item.kind == FlowPlanItemKind::Action)
    result.append(QStringLiteral("%1:%2/%3@%4").arg(item.batchId).arg(item.frIndex).arg(item.rlIndex).arg(item.lineIndex));
  return result;
}

QStringList barriersOf(const BattleFlowPlan& plan) {
  QStringList result;
  for (const auto& item : plan.items) if (item.kind == FlowPlanItemKind::SegmentBarrier)
    result.append(QStringLiteral("%1:%2@%3").arg(item.batchId).arg(item.frIndex).arg(item.lineIndex));
  return result;
}

bool exactCoverage(const QJsonArray& flow, const BattleFlowPlan& plan, const QString& base) {
  QMap<QString, int> expected, actual;
  for (int i = 0; i < flow.size(); ++i) {
    const auto path = base + QLatin1Char('/') + QString::number(i);
    expected[path] = 1;
    const auto actions = flow[i].toObject()[QStringLiteral("rl")].toArray();
    for (int j = 0; j < actions.size(); ++j)
      expected[path + QStringLiteral("/rl/") + QString::number(j)] = 1;
  }
  for (const auto* items : {&plan.items, &plan.unscheduled}) for (const auto& item : *items) {
    if (item.frIndex < 0 || item.frIndex >= flow.size()) return false;
    const auto path = base + QLatin1Char('/') + QString::number(item.frIndex);
    if (item.kind == FlowPlanItemKind::SegmentBarrier) {
      if (item.rlIndex != -1 || item.path != path) return false;
    } else if (item.rlIndex < 0 || item.path != path + QStringLiteral("/rl/") + QString::number(item.rlIndex)) return false;
    ++actual[item.path];
  }
  return actual == expected;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  int failures = 0;
  const auto check = [&](bool ok, const char* message) {
    if (!ok) { qCritical() << message; ++failures; }
  };
  const QString base = QStringLiteral("/br/fr");

  // All 502 inspected real reports have one populated lane, two empty lane
  // placeholders, then a same-lane cleanup. The fixtures contain no real data.
  const QJsonArray normal{segment(0, 2, QStringLiteral("A")), segment(1, 0, {}, false),
    segment(2, 0, {}, false), segment(0, 0, {}, false,
      QJsonArray{QJsonObject{{QStringLiteral("player"), QStringLiteral("A")}, {QStringLiteral("teamId"), 0}}})};
  const auto original = QJsonDocument(normal).toJson(QJsonDocument::Compact);
  const auto normalPlan = planBattleFlow(normal);
  check(normalPlan.complete && normalPlan.issues.isEmpty() && normalPlan.unscheduled.isEmpty(), "single-lane report rejected");
  check(normalPlan.batchCount == 2 && actionsOf(normalPlan) == QStringList{QStringLiteral("0:0/0@0"), QStringLiteral("1:0/1@0")},
    "single-lane action order changed");
  check(barriersOf(normalPlan) == QStringList{QStringLiteral("0:0@0"), QStringLiteral("0:1@1"), QStringLiteral("0:2@2"), QStringLiteral("2:3@0")},
    "empty setup or trailing cleanup loaded at the wrong boundary");
  check(exactCoverage(normal, normalPlan, base), "single-lane raw paths lost or duplicated");
  check(QJsonDocument(normal).toJson(QJsonDocument::Compact) == original, "planner modified the input");

  // C blocks the queue head while A occupies lane 0. D must not sneak into
  // free lane 2 before C is admitted. A and B are concurrently started.
  const QJsonArray interleaved{segment(0, 2, QStringLiteral("A")), segment(1, 3, QStringLiteral("B")),
    segment(0, 1, QStringLiteral("C")), segment(2, 1, QStringLiteral("D"))};
  const auto interleavedPlan = planBattleFlow(interleaved);
  check(interleavedPlan.complete && interleavedPlan.batchCount == 3, "multi-lane plan did not complete");
  check(actionsOf(interleavedPlan) == QStringList{QStringLiteral("0:0/0@0"), QStringLiteral("0:1/0@1"),
      QStringLiteral("1:0/1@0"), QStringLiteral("1:1/1@1"), QStringLiteral("2:2/0@0"),
      QStringLiteral("2:1/2@1"), QStringLiteral("2:3/0@2")}, "same-batch interleaving or lane continuation is wrong");
  check(barriersOf(interleavedPlan) == QStringList{QStringLiteral("0:0@0"), QStringLiteral("0:1@1"), QStringLiteral("2:2@0"), QStringLiteral("2:3@2")},
    "a later segment bypassed the blocked queue head");
  check(exactCoverage(interleaved, interleavedPlan, base), "multi-lane actions lost or repeated");

  // C's lane is free after A, but it cannot delete B while B still has rounds.
  const QJsonArray deletions{QJsonObject{{QStringLiteral("player"), QStringLiteral("B-enemy")}, {QStringLiteral("teamId"), 1}}};
  const QJsonArray moves{QJsonObject{{QStringLiteral("player"), QStringLiteral("C")},
    {QStringLiteral("teamId"), 0}, {QStringLiteral("x_to"), 2}, {QStringLiteral("y_to"), 1}}};
  const QJsonArray deletionBarrier{segment(0, 1, QStringLiteral("A")), segment(1, 3, QStringLiteral("B")),
    segment(0, 1, QStringLiteral("C"), true, deletions), segment(2, 1, QStringLiteral("D"), true, {}, moves)};
  const auto deletionPlan = planBattleFlow(deletionBarrier);
  check(deletionPlan.complete && deletionPlan.batchCount == 4, "cross-lane deletion did not wait for the affected player");
  check(actionsOf(deletionPlan) == QStringList{QStringLiteral("0:0/0@0"), QStringLiteral("0:1/0@1"),
      QStringLiteral("1:1/1@1"), QStringLiteral("2:1/2@1"), QStringLiteral("3:2/0@0"), QStringLiteral("3:3/0@2")},
    "deletion barrier cleared before all affected-lane actions ended");
  check(barriersOf(deletionPlan) == QStringList{QStringLiteral("0:0@0"), QStringLiteral("0:1@1"), QStringLiteral("3:2@0"), QStringLiteral("3:3@2")},
    "deletion/shift segment not preserved at the admission barrier");
  check(exactCoverage(deletionBarrier, deletionPlan, base), "blocked deletion lost raw records");

  // handleShiftActions overwrites the callback count on every admission. If
  // another segment is admitted after nonempty ml, exact order depends on the
  // original movement callbacks and cannot be recovered from packet values.
  const QJsonArray overwrittenShiftFlow{segment(0, 2, QStringLiteral("C"), true, {}, moves),
    segment(1, 2, QStringLiteral("D"))};
  const auto overwrittenShiftPlan = planBattleFlow(overwrittenShiftFlow);
  check(!overwrittenShiftPlan.complete && !overwrittenShiftPlan.issues.isEmpty() &&
    exactCoverage(overwrittenShiftFlow, overwrittenShiftPlan, base), "overwritten shift callbacks invented a confirmed batch order");

  // The client removes preceding dl entries before it notices a later active
  // player. Do not move that partial mutation to a fabricated atomic barrier.
  const QJsonArray partialDeletes{QJsonObject{{QStringLiteral("player"), QStringLiteral("finished")}},
    QJsonObject{{QStringLiteral("player"), QStringLiteral("B")}}};
  const QJsonArray partialDeleteFlow{segment(0, 1, QStringLiteral("A")), segment(1, 3, QStringLiteral("B")),
    segment(0, 1, QStringLiteral("C"), true, partialDeletes)};
  const auto partialDeletePlan = planBattleFlow(partialDeleteFlow);
  check(!partialDeletePlan.complete && !partialDeletePlan.issues.isEmpty() &&
    exactCoverage(partialDeleteFlow, partialDeletePlan, base), "partial deletion was silently treated as an atomic barrier");

  // isPlayerInBattle() stops at its first matching slot. A finished earlier
  // slot must not conceal a later active slot with the same player name.
  const QJsonArray repeatedPlayerFlow{segment(0, 1, QStringLiteral("same")), segment(1, 3, QStringLiteral("same")),
    segment(0, 1, QStringLiteral("C"), true,
      QJsonArray{QJsonObject{{QStringLiteral("player"), QStringLiteral("same")}}})};
  const auto repeatedPlayerPlan = planBattleFlow(repeatedPlayerFlow);
  check(!repeatedPlayerPlan.complete && !repeatedPlayerPlan.issues.isEmpty() &&
    exactCoverage(repeatedPlayerFlow, repeatedPlayerPlan, base), "repeated player slots invented a confirmed deletion order");

  const QJsonArray emptySegments{segment(2, 0, {}, false), segment(2, 0, {}, true), segment(0, 0, {}, false)};
  const auto emptyPlan = planBattleFlow(emptySegments);
  check(emptyPlan.complete && emptyPlan.batchCount == 0 && emptyPlan.items.size() == 3 && actionsOf(emptyPlan).isEmpty(),
    "empty segments created a fake action or stalled");
  check(exactCoverage(emptySegments, emptyPlan, base), "empty barriers were discarded");
  const auto noFlow = planBattleFlow({});
  check(noFlow.complete && noFlow.items.isEmpty() && noFlow.batchCount == 0, "empty flow did not finish");

  // BattleUnit's AS int field assigns absent/null bfn to zero. A provided
  // invalid value must not accidentally turn into lane zero through toInt().
  auto missingLane = segment(1, 1, QStringLiteral("missing")); missingLane.remove(QStringLiteral("bfn"));
  auto nullLane = segment(1, 1, QStringLiteral("null")); nullLane[QStringLiteral("bfn")] = QJsonValue(QJsonValue::Null);
  const QJsonArray defaults{missingLane, nullLane};
  const auto defaultPlan = planBattleFlow(defaults, QStringLiteral("/br/brs/2/fr"));
  check(defaultPlan.complete && actionsOf(defaultPlan) == QStringList{QStringLiteral("0:0/0@0"), QStringLiteral("1:1/0@0")},
    "official missing/null lane default not preserved");
  check(exactCoverage(defaults, defaultPlan, QStringLiteral("/br/brs/2/fr")), "team-container original path mapping changed");
  for (const auto& invalid : QJsonArray{-1, 3, 1.5, QStringLiteral("bad"), true, QJsonObject{}}) {
    auto malformed = segment(0, 2, QStringLiteral("bad")); malformed[QStringLiteral("bfn")] = invalid;
    const QJsonArray input{segment(1, 1, QStringLiteral("first")), malformed, segment(2, 1, QStringLiteral("last"))};
    const auto plan = planBattleFlow(input);
    check(!plan.complete && !plan.issues.isEmpty() && plan.items.isEmpty(), "illegal lane published a confirmed schedule");
    check(exactCoverage(input, plan, base), "illegal lane discarded or duplicated an action");
    for (const auto& item : plan.unscheduled) check(item.batchId == -1, "unconfirmed action received a fabricated batch");
  }

  // The official run() does not filter isf=false actions, yet that flag can
  // prevent the completion mask from changing. Keep the records, not a claim
  // that this inconsistent flow has a verified continuation schedule.
  for (int count : {1, 2}) {
    const QJsonArray contradictory{segment(0, count, QStringLiteral("not-fighting"), false), segment(0, 1, QStringLiteral("next"))};
    const auto plan = planBattleFlow(contradictory);
    check(!plan.complete && !plan.issues.isEmpty() && plan.items.isEmpty(), "isf=false with actions was silently repaired");
    check(exactCoverage(contradictory, plan, base), "isf=false caused action records to be dropped");
  }
  auto missingFight = segment(0, 1, QStringLiteral("unknown")); missingFight.remove(QStringLiteral("isf"));
  const QJsonArray unknownFlag{missingFight};
  const auto unknownPlan = planBattleFlow(unknownFlag);
  check(!unknownPlan.complete && exactCoverage(unknownFlag, unknownPlan, base), "missing isf was assumed to prove a complete schedule");

  auto badDelete = segment(0, 1, QStringLiteral("bad-delete"));
  badDelete[QStringLiteral("pr")] = QJsonObject{{QStringLiteral("dl"), QJsonArray{QJsonObject{{QStringLiteral("teamId"), 1}}}}};
  const QJsonArray unknownDelete{badDelete, segment(2, 1, QStringLiteral("following"))};
  const auto badDeletePlan = planBattleFlow(unknownDelete);
  check(!badDeletePlan.complete && !badDeletePlan.issues.isEmpty() && exactCoverage(unknownDelete, badDeletePlan, base),
    "unresolvable deletion barrier was silently ignored");
  const QJsonArray brokenSegment{17, segment(0, 1, QStringLiteral("kept"))};
  const auto brokenPlan = planBattleFlow(brokenSegment, QStringLiteral("/fr"));
  check(!brokenPlan.complete && exactCoverage(brokenSegment, brokenPlan, QStringLiteral("/fr")), "nonobject segment lost the remaining records");

  if (failures == 0) qInfo() << "battle replay flow planner: all checks passed";
  return failures == 0 ? 0 : 1;
}
