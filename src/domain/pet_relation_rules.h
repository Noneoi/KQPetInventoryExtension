#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

enum class PetRelationKind { Summon, Carry };

struct PetRelationFamily {
  int originalRaceId = 0;
  QString originalName;
  QVector<int> raceIds;
  int carryType = 0;
  // Official selection screens omit non-owned hidden summon definitions and
  // skin-only rows. Exact pair targets above remain available for owned pets.
  bool listedWithoutOwnership = true;
};

struct PetRelationOptions {
  // False means the required public projection is absent or malformed, not
  // that the account owns no suitable pets. A valid empty rule set is true.
  bool available = false;
  QVector<PetRelationFamily> families;
};

// Pure forward configuration query. raceIds are exact official pair targets;
// no account ownership, face ID, nickname or matching-name inference occurs.
// Unproven identity mappings remain separate target IDs with an empty name.
PetRelationOptions petRelationOptions(const QJsonObject& replayRoot, int raceId, PetRelationKind kind);
