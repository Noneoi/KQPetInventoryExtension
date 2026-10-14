#pragma once

#include "battle_replay.h"

namespace BattleReplay {

struct EmpowermentEvidence {
  QString source, target, skillId, description;
  // Attribute IDs: the official native dictionary and the observed opening
  // formation respectively. `before` is not an earlier battle-log snapshot.
  QStringList beforeAttributes, afterAttributes;
  QString officialEffect;
};

// Cross-check the initial formation only. This does not assign a meaning to
// raw `ibat`, nor infer an owner for every attribute change or Buff.
QMap<QString, EmpowermentEvidence> identifyEmpowerments(
    const QMap<QString, Fighter>& initialState, const QJsonObject& catalog);

// Independently cross-check an opening SELF_RACE_TYPE result. Only return a
// unique self explanation within the player's known formation configuration;
// an external element skill with the same possible result remains ambiguous.
// The evidence describes an opening formation, never a battle-time cast.
QMap<QString, EmpowermentEvidence> identifySelfTransforms(
    const QMap<QString, Fighter>& initialState, const QJsonObject& catalog);

} // namespace BattleReplay
