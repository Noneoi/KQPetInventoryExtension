#pragma once

#include "domain/battle_replay.h"
#include <algorithm>
#include <tuple>

namespace ReplayMeterSequence {
// These are readable portions of one recorded command, not recovered Flash
// frame times. They preserve local value-list order and never split a target's
// summed attack result into invented animation hits.
struct Slice {
  QMap<QString, BattleReplay::Fighter> beforeState, state;
  QList<int> effects;
  QSet<int> completed;
};

namespace Detail {
inline int metricIndex(const QString& metric) {
  if (metric == QStringLiteral("生命")) return 0;
  if (metric == QStringLiteral("护盾")) return 1;
  if (metric == QStringLiteral("气势")) return 2;
  return -1;
}

inline std::optional<qint64>& value(BattleReplay::Fighter& fighter, int metric) {
  return metric == 0 ? fighter.hp : metric == 1 ? fighter.shield : fighter.power;
}

inline bool sameNonNumeric(const BattleReplay::Fighter& a, const BattleReplay::Fighter& b) {
  // A pure-value command may change balances, their provenance and HP-derived
  // presence only. In particular, never pull a future form or Buff membership
  // backwards while making the numeric portion easier to read.
  if (std::tie(a.key, a.name, a.player, a.id, a.path, a.team, a.x, a.y, a.representer,
        a.raceId, a.faceId, a.formName, a.normalSkill, a.uniqueSkill, a.superUniqueSkill,
        a.originRaceId, a.originFighterKey, a.originRelation, a.originPath, a.attributes,
        a.formPath, a.attributesPath, a.skillsPath, a.presencePath, a.attributesKnown,
        a.summoned, a.clone, a.cloneCalled, a.entered, a.entryKnown, a.chaser, a.playerRemoved,
        a.playerX, a.playerY, a.maxHp, a.maxPower, a.maxHpPath, a.buffs, a.initialBuffs,
        a.buffsKnown, a.buffPath, a.raw, a.lastNumbers, a.lastUnit) !=
      std::tie(b.key, b.name, b.player, b.id, b.path, b.team, b.x, b.y, b.representer,
        b.raceId, b.faceId, b.formName, b.normalSkill, b.uniqueSkill, b.superUniqueSkill,
        b.originRaceId, b.originFighterKey, b.originRelation, b.originPath, b.attributes,
        b.formPath, b.attributesPath, b.skillsPath, b.presencePath, b.attributesKnown,
        b.summoned, b.clone, b.cloneCalled, b.entered, b.entryKnown, b.chaser, b.playerRemoved,
        b.playerX, b.playerY, b.maxHp, b.maxPower, b.maxHpPath, b.buffs, b.initialBuffs,
        b.buffsKnown, b.buffPath, b.raw, b.lastNumbers, b.lastUnit)) return false;
  if (a.buffMembers.size() != b.buffMembers.size() || a.teamProgress.keys() != b.teamProgress.keys()) return false;
  for (int i = 0; i < a.buffMembers.size(); ++i) {
    const auto& x = a.buffMembers[i]; const auto& y = b.buffMembers[i];
    if (std::tie(x.id, x.source, x.sourceName, x.path, x.sourceTeam) !=
        std::tie(y.id, y.source, y.sourceName, y.path, y.sourceTeam)) return false;
  }
  for (auto it = a.teamProgress.cbegin(); it != a.teamProgress.cend(); ++it) {
    const auto& x = it.value(); const auto y = b.teamProgress.value(it.key());
    if (std::tie(x.buffId, x.path, x.current, x.maximum, x.previous, x.delta, x.skillEffected, x.raw) !=
        std::tie(y.buffId, y.path, y.current, y.maximum, y.previous, y.delta, y.skillEffected, y.raw)) return false;
  }
  return true;
}

inline bool sameNumbers(const BattleReplay::Fighter& a, const BattleReplay::Fighter& b) {
  return std::tie(a.hp, a.shield, a.power, a.maxHp, a.maxPower, a.hpPath, a.shieldPath, a.powerPath, a.presence) ==
    std::tie(b.hp, b.shield, b.power, b.maxHp, b.maxPower, b.hpPath, b.shieldPath, b.powerPath, b.presence);
}

inline bool apply(QMap<QString, BattleReplay::Fighter>& state, const BattleReplay::Effect& effect) {
  if (!state.contains(effect.target) || !effect.before || !effect.after) return false;
  const int metric = metricIndex(effect.metric); if (metric < 0) return false;
  auto& fighter = state[effect.target]; auto& balance = value(fighter, metric);
  if (balance != effect.before) return false;
  balance = effect.after;
  if (metric == 0) { fighter.hpPath = effect.path; BattleReplay::updatePresence(fighter); }
  else if (metric == 1) fighter.shieldPath = effect.path;
  else fighter.powerPath = effect.path;
  return true;
}
} // namespace Detail

inline QList<Slice> split(const BattleReplay::Step& step, int momentIndex) {
  using namespace BattleReplay;
  if (momentIndex <= 0 || momentIndex >= step.moments.size()) return {};
  const auto& moment = step.moments[momentIndex];
  const auto& beginning = step.moments[momentIndex - 1].state;
  const bool attack = (moment.phase == QStringLiteral("attack") || moment.phase == QStringLiteral("counter")) &&
    moment.part == QStringLiteral("result");
  const bool grouped = (moment.phase == QStringLiteral("before") || moment.phase == QStringLiteral("after")) &&
    (moment.part == QStringLiteral("values") || moment.part == QStringLiteral("power-buff"));
  if ((!attack && !grouped) || moment.effects.isEmpty() || beginning.keys() != moment.state.keys()) return {};
  if (attack && (moment.actor.isEmpty() || !beginning.contains(moment.actor))) return {};
  for (auto it = beginning.cbegin(); it != beginning.cend(); ++it)
    if (!Detail::sameNonNumeric(it.value(), moment.state.value(it.key()))) return {};

  QList<int> indices;
  for (int i = 0; i < step.effects.size(); ++i) if (step.effects[i].moment == momentIndex) indices.append(i);
  if (indices.size() != moment.effects.size()) return {};
  const QString prefix = attack ? (moment.phase == QStringLiteral("counter") ? QStringLiteral("sb_") : QStringLiteral("att_")) :
    (moment.phase == QStringLiteral("before") ? QStringLiteral("bb_") : QStringLiteral("ab_"));
  const QStringList suffixes{QStringLiteral("hps"), QStringLiteral("shps"), QStringLiteral("vgs")};
  QMap<QString, QMap<int, QList<int>>> lists;
  QMap<QString, QMap<int, QString>> paths;
  QList<int> targets;
  bool repeated = false;
  for (int ordinal = 0; ordinal < indices.size(); ++ordinal) {
    const int index = indices[ordinal]; const auto& effect = step.effects[index];
    const auto& local = moment.effects[ordinal]; const int metric = Detail::metricIndex(effect.metric);
    if (std::tie(effect.target, effect.metric, effect.path, effect.kind, effect.source, effect.cause,
          effect.before, effect.after, effect.delta, effect.raw) !=
        std::tie(local.target, local.metric, local.path, local.kind, local.source, local.cause,
          local.before, local.after, local.delta, local.raw)) return {};
    if (effect.kind != QStringLiteral("value") || metric < 0 || !effect.before || !effect.after || effect.raw.isObject() ||
        effect.recordedBefore || effect.recordedAfter || effect.recordedDelta ||
        !beginning.contains(effect.target) || beginning.value(effect.target).representer ||
        !effect.path.endsWith(QLatin1Char('/') + prefix + suffixes[metric])) return {};
    if (grouped && (effect.cause != QStringLiteral("buff") ||
        (moment.part == QStringLiteral("values") ? metric == 2 : metric != 2))) return {};
    if (attack && (effect.source != moment.actor ||
        (moment.phase == QStringLiteral("counter") ? effect.cause != QStringLiteral("counter") :
          effect.cause != QStringLiteral("skill") && effect.cause != QStringLiteral("team")))) return {};
    // Two callbacks for the same target/metric are not one proven local list,
    // even when a serialized ledger happens to have adjoining balances.
    auto& path = paths[effect.target][metric];
    if (!path.isEmpty() && path != effect.path) return {};
    path = effect.path;
    if (attack && effect.target != moment.actor) { targets.append(index); continue; }
    // The normal source is cn; a counter source is the original trl entry.
    // Self-target results from another callback may not pose as source costs.
    if (attack && (moment.phase == QStringLiteral("attack") ?
        effect.path != step.path + QStringLiteral("/cn/") + prefix + suffixes[metric] :
        !effect.path.startsWith(step.path + QStringLiteral("/trl/")) ||
          !effect.path.endsWith(QStringLiteral("/tn/") + prefix + suffixes[metric]))) return {};
    auto& list = lists[effect.target][metric]; list.append(index);
    repeated |= list.size() > 1;
  }
  if (!repeated) return {};
  // The domain already combined a target's attack array. A repeated target
  // aggregate is not evidence for extra hits and must not be split here.
  QSet<QString> targetFields;
  for (const int index : targets) {
    const auto& effect = step.effects[index];
    const auto field = effect.target + QChar(0x1f) + effect.metric;
    if (targetFields.contains(field)) return {};
    targetFields.insert(field);
  }

  QMap<QString, QList<int>> queues;
  int count = 0;
  for (auto it = lists.cbegin(); it != lists.cend(); ++it) {
    // Official source lists and Before/After HP-SP lists consume all HP first,
    // then shield, then power. The existing Moment keeps the power barrier.
    auto& queue = queues[it.key()];
    for (int metric = 0; metric < 3; ++metric) queue.append(it.value().value(metric));
    count = std::max(count, static_cast<int>(queue.size()));
  }
  if (count < 2) return {};
  QList<Slice> result;
  auto state = beginning; QSet<int> completed;
  for (int ordinal = 0; ordinal < count; ++ordinal) {
    Slice slice; slice.beforeState = state;
    // Different recipients proceed alongside each other. No ordering of
    // their animation callbacks is inferred from the map or effect index.
    for (const auto& queue : queues) if (ordinal < queue.size()) slice.effects.append(queue[ordinal]);
    // playSourceSkillNumResult applies the whole source list before target
    // balances. Keep each target total atomic and land all targets together.
    if (attack && ordinal + 1 == count) slice.effects.append(targets);
    for (const int index : slice.effects) {
      if (!Detail::apply(state, step.effects[index])) return {};
      completed.insert(index);
    }
    slice.state = state; slice.completed = completed; result.append(slice);
  }
  if (completed.size() != indices.size()) return {};
  for (auto it = state.cbegin(); it != state.cend(); ++it)
    if (!Detail::sameNumbers(it.value(), moment.state.value(it.key()))) return {};
  // Only after every recorded value has been consumed can the original
  // command snapshot be exposed. Static metadata was checked above.
  result.last().state = moment.state;
  return result;
}
} // namespace ReplayMeterSequence
