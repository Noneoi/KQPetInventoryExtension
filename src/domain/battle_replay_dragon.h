#pragma once

#include "domain/battle_replay.h"

namespace BattleReplay {

enum class DragonCommandProgress { Before, Active, Complete };
enum class DragonState { Ready, Fighting, Finished, Unknown };

// A dragon belongs to a player, not to a fighter's nine-cell position. IDs and
// description parameters come from that player's drs record; names come only
// from the extracted official PetDragonDefines table.
struct DragonInfo {
  QString key, id, defineId, path, name;
  int slot = -1;
  std::optional<qint64> quality, skillValue;
  QString qualityName, gene, executeBufferId;
  bool geneRecorded = false, definitionKnown = false;
  std::optional<bool> glittery;
  QString skillType, descriptionTemplate, description;
  bool descriptionComplete = false;
  QJsonObject definitionSource;
  QJsonValue raw;
  QStringList issues;
  DragonState state = DragonState::Unknown;
  int lastAction = -1;
  QString lastActionPath, lastSkillId;
  // A simple representer command emits FINISHED without START. Keep its
  // queue completion separate from evidence that this dragon used a skill.
  int lastCompletionAction = -1;
  QString lastCompletionPath;
};

struct DragonRoster {
  bool rosterKnown = false;
  QString ownerKey, path;
  QJsonValue raw;
  QList<DragonInfo> dragons;
  QStringList issues;
  // The official queue uses only eps[0]. Keep all IDs/raw values for inspection;
  // remaining entries never mark additional dragons as having taken a turn.
  QStringList executeIds;
  QJsonValue executeRaw;
  QString executePath, activeId, executionSkillId;
};

QString dragonStateKey(DragonState state);
QString dragonStateText(DragonState state);

// Pure reconstruction permits arbitrary seeking without retaining future state.
// START selects eps[0]; FINISHED advances the official two-slot queue only
// once per csn, including the simple-command callback that has no START.
// The caller supplies the current boundary; Before never exposes its result.
// This model deliberately excludes parallel/team-battle scheduling.
DragonRoster dragonRosterAt(const Replay& replay, const Fighter& representer,
                           int stepIndex, DragonCommandProgress progress,
                           const QJsonObject& officialCatalog = {});

} // namespace BattleReplay
