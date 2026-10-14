#pragma once
#include "battle_replay_identity_presentation.h"
#include "battle_replay_timeline_events.h"

// A UI projection of recorded actions and life events. A round is a contiguous
// run of the same recorded csn among actions, not a reconstruction of turns.
namespace ReplayTimeline {
struct LifeMarker {
  int step = 0, turn = 0, round = -1, team = -1;
  QString key, name, label, note;
  bool recovery = false;
  int roundIndex = -1, roundTurn = 0;
};

struct RoundGroup {
  int number = -1;
  QList<int> steps;
};

struct TimelineUnit {
  QString key, name, player;
  int team = -1, x = -1, y = -1;
};

struct LifeModel {
  QList<LifeMarker> markers;
  QList<RoundGroup> rounds;
  QList<TimelineUnit> units;
  QList<int> globalTurns, localTurns, roundIndexes, actionSteps;
};

inline LifeModel buildLifeModel(const BattleReplay::Replay& replay, const QJsonObject& catalog) {
  LifeModel model;
  QMap<QString, int> unitIndexes;
  int globalTurn = 0, localTurn = 0, roundIndex = -1;
  for (int index = 0; index < replay.steps.size(); ++index) {
    const auto& step = replay.steps[index];
    // Full fighter keys include team, player and instance identity. Multiple
    // forms, clones and occupants of the same cell remain separate rows.
    for (const auto& fighter : step.state) {
      if (fighter.representer || fighter.team < 0 || fighter.key.isEmpty()) continue;
      const auto existing = unitIndexes.constFind(fighter.key);
      if (existing == unitIndexes.cend()) {
        unitIndexes.insert(fighter.key, int(model.units.size()));
        model.units.append({fighter.key, replayFighterName(fighter, catalog), fighter.player,
          fighter.team, fighter.x, fighter.y});
      } else if (model.units[*existing].name.isEmpty()) {
        // Keep the first observed name and initial position through later
        // transformations. An unresolved original-name label stays explicit.
        model.units[*existing].name = replayFighterName(fighter, catalog);
      }
    }

    const bool action = isAction(step);
    if (action) {
      const int number = step.round < 0 ? -1 : step.round;
      if (model.rounds.isEmpty() || model.rounds.last().number != number) {
        model.rounds.append(RoundGroup{number, {}});
        roundIndex = int(model.rounds.size()) - 1;
        localTurn = 0;
      }
      ++globalTurn;
      ++localTurn;
      model.rounds[roundIndex].steps.append(index);
      model.actionSteps.append(index);
    }
    // Setup/result/non-action steps retain the last observed action position.
    // Before the first action this is 0/0/-1 even when rounds[0] can be previewed.
    model.globalTurns.append(globalTurn);
    model.localTurns.append(localTurn);
    model.roundIndexes.append(roundIndex);
    if (!action) continue;

    // Reuse the exact event expansion used by event navigation. Splitting a
    // command can expose several death/recovery transitions for one fighter;
    // none are coalesced by key, action or round.
    for (const auto& event : events(step)) {
      const bool recovery = event.kind == QStringLiteral("生命恢复");
      if ((!recovery && event.kind != QStringLiteral("生命归零")) ||
          event.moment < 0 || event.moment >= step.moments.size()) continue;
      const auto& state = step.moments[event.moment].state;
      if (!state.contains(event.key)) continue;
      const auto& fighter = state[event.key];
      if (fighter.representer || fighter.team < 0 || fighter.key.isEmpty()) continue;
      const auto before = event.moment > 0 ? step.moments[event.moment - 1].state.value(event.key) : fighter;
      const bool present = before.key == fighter.key && BattleReplay::presentedOnField(fighter) && BattleReplay::presentedOnField(before);
      LifeMarker marker;
      marker.step = index; marker.turn = globalTurn; marker.round = step.round; marker.team = fighter.team;
      marker.roundIndex = roundIndex; marker.roundTurn = localTurn;
      marker.key = fighter.key; marker.name = replayFighterName(fighter, catalog); marker.recovery = recovery;
      marker.label = present ? (recovery ? QStringLiteral("复活") : QStringLiteral("死亡")) : event.kind;
      marker.note = recovery ? QStringLiteral("已记录生命从 0 恢复为正值，不推断触发技能。") : QStringLiteral("已记录生命从正值降至 0。");
      if (!present) marker.note += QStringLiteral("登场状态未确认，仅标生命变化。");
      if (step.moments[event.moment].part == QStringLiteral("calibration")) marker.note += QStringLiteral("来自最终数值记录。");
      if (step.parallelBatch) marker.note += QStringLiteral("同批战线记录，列表顺序不代表并行战线的完成先后。");
      model.markers.append(marker);
    }
  }
  return model;
}
} // namespace ReplayTimeline
