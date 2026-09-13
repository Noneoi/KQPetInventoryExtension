#pragma once

#include "pet_power_types.h"
#include <QJsonObject>
#include <QList>
#include <QString>

// Counts are the total remaining consumption, before any account inventory is
// applied. A missing official cost is unknown, never an implicit zero.
struct PetCultivationMaterial {
  int type = 0, id = 0;
  // Official four-part costs carry a level/quality selector (source beasts use 1).
  int extra = 0;
  QString name;
  int count = 0;
  bool known = false;
  QString scope = QStringLiteral("account");
};

struct PetCultivationRequirement {
  QString key, category, name, status;
  QList<PetCultivationMaterial> materials;
  // State/target knowledge is independent of each material's count knowledge.
  bool known = false;
};

struct PetCultivationRequirements {
  QList<PetCultivationRequirement> items;
  bool completeKnown = false;
  bool complete = false;
};

// Pure calculation over the same frozen raw detail, catalog and power result.
// Completed categories are omitted. Stargod acquisition already accounts for
// this pet's equipped stars and backpack; it must not subtract account stock.
PetCultivationRequirements calculatePetCultivationRequirements(
    const QJsonObject& pet, const QJsonObject& metadataRoot,
    const PetBattlePowerState& power);
