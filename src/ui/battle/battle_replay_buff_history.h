#pragma once
#include "domain/battle_replay.h"

// A history of observations, not a reconstruction of an unrecorded Buff
// mechanism. In particular, a numeric cause does not establish membership or
// identify the original applier, and a zero-change removal remains a record.
namespace ReplayBuffHistory {
inline bool isAddition(const BattleReplay::Effect& effect) {
  if (effect.buffOperation >= 0) return effect.buffOperation == 1;
  // Compatibility for old/manual snapshots; never use a name or config flag.
  bool ok = false;
  return effect.raw.toString().section(QLatin1Char('_'), 0, 0).toInt(&ok) == 1 && ok;
}
struct Entry {
  int step = 0, effectIndex = -1;
  bool initial = false;
  BattleReplay::Effect effect;
};

inline bool matches(const BattleReplay::Effect& effect, const QString& key, const QString& id) {
  return effect.target == key && effect.causeId == id &&
    (effect.cause == QStringLiteral("buff") || effect.kind == QStringLiteral("buff") ||
     effect.kind == QStringLiteral("progress"));
}

inline QList<Entry> recorded(const BattleReplay::Replay& replay, const QString& key, const QString& id,
                            int currentStep, int currentMoment) {
  QList<Entry> entries;
  if (key.isEmpty() || id.isEmpty() || currentStep < 0 || currentStep >= replay.steps.size()) return entries;
  const auto& initial = replay.steps.first().state;
  if (initial.contains(key)) {
    const auto& fighter = initial[key];
    const auto count = fighter.buffs.count(id);
    if (count) {
      Entry entry; entry.initial = true;
      entry.effect.kind = QStringLiteral("buff"); entry.effect.target = key; entry.effect.causeId = id;
      entry.effect.after = count; entry.effect.path = fighter.buffPath;
      entries.append(entry);
    }
    if (fighter.teamProgress.contains(id)) {
      const auto& progress = fighter.teamProgress[id];
      Entry entry; entry.initial = true;
      entry.effect.kind = QStringLiteral("progress"); entry.effect.target = key; entry.effect.causeId = id;
      entry.effect.after = progress.current; entry.effect.path = progress.path; entry.effect.raw = progress.raw;
      entries.append(entry);
    }
  }
  const auto& current = replay.steps[currentStep];
  for (int n = 0; n <= currentStep; ++n) {
    const auto& step = replay.steps[n];
    // A sibling lane has no proven completion order at the current boundary.
    if (n != currentStep && current.parallelBatch && step.batchId == current.batchId) continue;
    for (int i = 0; i < step.effects.size(); ++i) {
      const auto& effect = step.effects[i];
      if (n == currentStep && currentMoment >= 0 && (effect.moment < 0 || effect.moment > currentMoment)) continue;
      if (matches(effect, key, id)) entries.append({n, i, false, effect});
    }
  }
  return entries;
}

inline QStringList observedIds(const BattleReplay::Replay& replay, const QString& key,
                              int currentStep, int currentMoment) {
  QStringList ids;
  if (key.isEmpty() || currentStep < 0 || currentStep >= replay.steps.size()) return ids;
  const auto append = [&](const QString& id) { if (!id.isEmpty() && !ids.contains(id)) ids.append(id); };
  const auto& initial = replay.steps.first().state;
  if (initial.contains(key)) {
    for (const auto& id : initial[key].buffs) append(id);
    for (const auto& progress : initial[key].teamProgress) append(progress.buffId);
  }
  const auto& current = replay.steps[currentStep];
  for (int n = 0; n <= currentStep; ++n) {
    const auto& step = replay.steps[n];
    if (n != currentStep && current.parallelBatch && step.batchId == current.batchId) continue;
    for (const auto& effect : step.effects) {
      if (n == currentStep && currentMoment >= 0 && (effect.moment < 0 || effect.moment > currentMoment)) continue;
      if (matches(effect, key, effect.causeId)) append(effect.causeId);
    }
  }
  return ids;
}
} // namespace ReplayBuffHistory
