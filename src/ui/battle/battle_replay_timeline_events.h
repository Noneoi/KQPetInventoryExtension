#pragma once
#include "battle_replay_meter_sequence.h"

// Shared by exact-event navigation and the compact life timeline. These are
// observations already parsed from the report, not inferred battle mechanics.
namespace ReplayTimeline {
struct EventPosition { QString kind, key; int moment = -1, segment = -1; };

inline bool isAction(const BattleReplay::Step& step) {
  return !step.moments.isEmpty() && step.moments.first().phase == QStringLiteral("setup") &&
    step.moments.first().part == QStringLiteral("initial");
}

inline QList<EventPosition> events(const BattleReplay::Step& step) {
  QList<EventPosition> result;
  QSet<int> expanded;
  for (const auto& event : step.events) {
    if (event.kind == QStringLiteral("生命归零") || event.kind == QStringLiteral("生命恢复")) {
      if (expanded.contains(event.moment)) continue;
      const auto slices = ReplayMeterSequence::split(step, event.moment);
      if (!slices.isEmpty()) {
        expanded.insert(event.moment);
        for (int segment = 0; segment < slices.size(); ++segment) {
          const auto& slice = slices[segment];
          for (const int index : slice.effects) {
            const auto& effect = step.effects[index];
            if (effect.metric != QStringLiteral("生命") || !effect.before || !effect.after) continue;
            const auto& before = slice.beforeState.value(effect.target);
            const auto& after = slice.state.value(effect.target);
            if (before.representer || !before.hp || !after.hp) continue;
            if (*before.hp > 0 && *after.hp == 0)
              result.append({QStringLiteral("生命归零"), effect.target, event.moment, segment});
            else if (*before.hp == 0 && *after.hp > 0)
              result.append({QStringLiteral("生命恢复"), effect.target, event.moment, segment});
          }
        }
        continue;
      }
    }
    result.append({event.kind, event.key, event.moment, -1});
  }
  return result;
}
} // namespace ReplayTimeline
