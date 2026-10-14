#pragma once
#include "ui/battle/battle_replay_cell.h"
#include <QJsonArray>

namespace ReplayActionMeters {
struct Window {
  // UI reading groups refine only proven local number-list order. They do
  // not change the domain's official callback boundaries or invent frames.
  QMap<int, int> groups;
  int current = -1, first = 0;
};

inline bool visibleChange(const BattleReplay::Effect& effect) {
  const auto delta = effect.recordedDelta ? effect.recordedDelta : effect.delta;
  return delta ? *delta != 0 : effect.changed && effect.before != effect.after;
}

// A target's critical flag belongs to that target's direct attack result. It
// cannot label a Buff tick, self cost, healing, or a later counterattack.
inline bool criticalDamage(const BattleReplay::Step& step, const BattleReplay::Effect& effect) {
  const auto delta = effect.recordedDelta ? effect.recordedDelta : effect.delta;
  if (!delta || *delta >= 0 || (effect.metric != QStringLiteral("生命") && effect.metric != QStringLiteral("护盾")) ||
      effect.source != step.actor || (effect.cause != QStringLiteral("skill") && effect.cause != QStringLiteral("team"))) return false;
  if (step.playbackKind != QStringLiteral("normal") && step.playbackKind != QStringLiteral("team-skill")) return false;
  for (const auto& target : step.effects) {
    if (target.kind != QStringLiteral("target") || target.target != effect.target ||
        !effect.path.startsWith(target.path + QStringLiteral("/tn/att_"))) continue;
    const auto flags = target.raw.toObject();
    return flags[QStringLiteral("isse")].toBool() && flags[QStringLiteral("iscr")].toBool() &&
      !(flags[QStringLiteral("ishit")].isBool() && !flags[QStringLiteral("ishit")].toBool()) && !flags[QStringLiteral("isIm")].toBool();
  }
  return false;
}

inline void populate(const BattleReplay::Step& step, const QMap<QString, BattleReplay::Fighter>& beginning,
                     const QMap<QString, BattleReplay::Fighter>& visible, const QJsonObject& catalog,
                     int throughMoment, bool pending, QMap<QString, ReplayUnitOverview>& overview,
                     const Window* window = nullptr) {
  for (const auto& fighter : visible) {
    auto& item = overview[fighter.key]; const auto start = beginning.value(fighter.key);
    item.hasActionMeterContext = true; item.actionMetersPending = pending;
    item.activeMeterEvents.emplace();
    item.actionHpBefore = start.hp; item.actionMaxHpBefore = start.maxHp;
    item.actionShieldBefore = start.shield; item.actionPowerBefore = start.power; item.actionMaxPowerBefore = start.maxPower;
  }
  int firstGroup = window ? window->first : 0;
  if (!window) {
    auto actor = step.actor;
    for (int m = 0; m <= throughMoment && m < step.moments.size(); ++m) {
      const auto& next = step.moments[m].actor;
      if (!next.isEmpty() && next != actor) { firstGroup = m; actor = next; }
    }
  }
  QMap<QString, QMap<QString, int>> latest;
  // Rebuild exact history and the latest per-meter group independently. A
  // zero confirmation does not remove a still-readable change. Rewinding is
  // deterministic and never needs a wall-clock timer or a synthetic balance.
  for (int i = 0; i < step.effects.size(); ++i) {
    const auto& effect = step.effects[i];
    if (!visible.contains(effect.target) || effect.moment < 0 || effect.moment > throughMoment ||
        (effect.metric != QStringLiteral("生命") && effect.metric != QStringLiteral("护盾") &&
         effect.metric != QStringLiteral("气势") && effect.metric != QStringLiteral("生命上限"))) continue;
    if (!effect.recordedDelta && !effect.delta && !effect.before && !effect.after) continue;
    const int group = window ? window->groups.value(i, -1) : effect.moment;
    if (window && group > window->current) continue;
    ReplayUnitEvent event; event.index = i; event.metric = effect.metric;
    const auto delta = effect.recordedDelta ? effect.recordedDelta : effect.delta;
    // Keep a meaningful explicit zero, but do not manufacture notices for
    // unchanged final confirmations at the end of every action.
    if (!effect.changed && !effect.recordedDelta && !effect.delta) continue;
    event.valueText = delta ? (*delta > 0 ? QStringLiteral("+") : QString()) + QString::number(*delta)
      : (effect.before ? QString::number(*effect.before) : QStringLiteral("?")) + QStringLiteral(" → ") +
        (effect.after ? QString::number(*effect.after) : QStringLiteral("?"));
    if (effect.metric == QStringLiteral("生命上限")) {
      event.metric = QStringLiteral("生命"); event.valueText.prepend(QStringLiteral("上限 "));
    } else {
      event.observedBefore = effect.before; event.observedAfter = effect.after;
    }
    if (effect.cause == QStringLiteral("calibration")) event.valueText.prepend(QStringLiteral("校准 "));
    event.critical = criticalDamage(step, effect);
    event.color = event.metric == QStringLiteral("生命") ? QStringLiteral("#c7465b") :
      event.metric == QStringLiteral("护盾") ? QStringLiteral("#246fc2") : QStringLiteral("#ba751d");
    event.label = event.metric + QLatin1Char(' ') + event.valueText;
    event.detail = effect.text + QStringLiteral("\n记录：") + effect.path;
    if (event.critical) event.detail += QStringLiteral("\n对应目标的直接伤害记录标记为暴击。");
    if (effect.metric == QStringLiteral("生命") && effect.cause == QStringLiteral("buff")) {
      const auto mark = catalog[QStringLiteral("buffs")].toObject()[effect.causeId].toObject()[QStringLiteral("bhs")].toString();
      if (!mark.isEmpty() && catalog[QStringLiteral("clientPresentation")].toObject()[QStringLiteral("buffHarmSources")].toArray().contains(mark))
        event.harmLabel = mark;
    }
    auto& item = overview[effect.target];
    item.actionMeterEvents.append(event);
    if (!pending || group < firstGroup || !visibleChange(effect)) continue;
    auto& last = latest[effect.target];
    if (!last.contains(event.metric) || group > last.value(event.metric)) {
      last[event.metric] = group;
      auto& active = *item.activeMeterEvents;
      for (int n = active.size(); n-- > 0;) if (active[n].metric == event.metric) active.removeAt(n);
    }
    if (group == last.value(event.metric)) item.activeMeterEvents->append(event);
  }
}
} // namespace ReplayActionMeters
