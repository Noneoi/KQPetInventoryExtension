#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

struct PetRelationCandidateFamily {
  int baseRaceId = 0;
  int targetType = 0;
  QString originalName;
  QVector<int> targetRaceIds;
  bool listedWithoutOwnership = false;
};

struct PetRelationCandidateOptions {
  bool available = false;
  QVector<PetRelationCandidateFamily> families;
};

// Only the final official pair projection is queried. Account arrays, display
// names, face IDs and inferred evolution families are not eligibility inputs.
PetRelationCandidateOptions petRelationCandidateOptions(const QJsonObject& skillRoot,
                                                       int sourceRaceId, bool carry);
