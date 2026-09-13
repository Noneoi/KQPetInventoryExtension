#pragma once
#include "pet_power_types.h"
#include "pet_era.h"
#include <QJsonObject>
#include <QJsonArray>

// Frozen metadata for pure power calculations. Compute never touches a runtime
// catalog singleton, mutable repository, widgets or filesystem.
struct PetPowerMetadata {
  QJsonObject stargods;
  int slotMaxLevel = 0;
  QJsonObject astrolabe;
  QJsonArray jobs;
  PetEra era = PetEra::Unknown;
  bool eraResolved = false;
  QJsonObject stargod(int id) const { return stargods.value(QString::number(id)).toObject(); }
};
PetBattlePowerState calculatePetBattlePower(const QJsonObject& pet, const PetPowerMetadata& metadata);
PetPowerMetadata petPowerMetadataFromCatalog(const QJsonObject& pet, int slotMax,
    const QJsonObject& stargods, const QJsonObject& astrolabe, const QJsonObject& pets);
