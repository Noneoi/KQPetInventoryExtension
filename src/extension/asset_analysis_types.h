#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QStringList>

#include "asset_analysis_version.h"

enum class PetAssetFilter {
  All = 0,
  Backpack,
  NormalWarehouse,
  EliteWarehouse,
  FullyCultivated,
  Improvable,
  MissingDetail,
  RedStarMissing,
  AstrolabeMissing,
  SacredMissing,
  SoulMissing,
  ShopImprovable
};

struct PetAssetRecord {
  qint64 instanceId = 0;
  int raceId = 0;
  QString name;
  QString location;
  int currentPower = 0;
  int extremePower = 0;
  int highestPower = 0;
  int completionPercent = 0;
  int highestPowerGap = 0;
  int missingRedStars = 0;
  int stargodLevelMissingSlots = 0;
  bool detailAvailable = false;
  bool fullyCultivated = false;
  bool improvable = false;
  bool redStarMissing = false;
  bool astrolabeMissing = false;
  bool sacredMissing = false;
  bool soulMissing = false;
  bool shopImprovable = false;
  bool stargodSlotsKnown = false;
  bool hasChangeableSlot = false;
  bool changeableRed = false;
  QStringList gapKeys;
  QStringList gaps;
  QJsonObject pet;
};

struct AccountInventorySummary {
  QString account;
  QDateTime inventoryUpdatedAt;
  int totalPets = 0;
  int backpackPets = 0;
  int normalWarehousePets = 0;
  int eliteWarehousePets = 0;
  int missingDetailPets = 0;
};

struct InventorySignature {
  QString account;
  QHash<qint64, QString> locations;

  bool operator==(const InventorySignature& other) const {
    return account == other.account && locations == other.locations;
  }
  bool operator!=(const InventorySignature& other) const {
    return !(*this == other);
  }
};

struct AccountAssetOverview {
  int analysisVersion = AssetAnalysisVersion::kCurrentAnalysis;
  QString account;
  QDateTime inventoryUpdatedAt;
  int totalPets = 0;
  int backpackPets = 0;
  int normalWarehousePets = 0;
  int eliteWarehousePets = 0;
  int fullyCultivatedPets = 0;
  int improvablePets = 0;
  int missingDetailPets = 0;
  int redStarMissingPets = 0;
  int astrolabeMissingPets = 0;
  int sacredMissingPets = 0;
  int soulMissingPets = 0;
  int shopImprovablePets = 0;
  bool shopDataKnown = false;
  bool routineDataKnown = false;
  int unfinishedDailyTasks = 0;
  int unfinishedWeeklyTasks = 0;
  bool todayOpportunityKnown = false;
  bool weekOpportunityKnown = false;
  int todayOpportunityRemaining = 0;
  int weekOpportunityRemaining = 0;
  qint64 totalCurrentPower = 0;
  QList<PetAssetRecord> pets;
};

struct AssetSnapshotPet {
  qint64 instanceId = 0;
  int raceId = 0;
  QString name;
  int currentPower = 0;
  int highestPower = 0;
  int completionPercent = 0;
  bool fullyCultivated = false;
  bool redStarComplete = false;
  bool astrolabeBreakthrough = false;
};

struct AccountAssetSnapshot {
  int schemaVersion = 0;
  int analysisVersion = 0;
  QString account;
  QDateTime createdAt;
  int totalPets = 0;
  int fullyCultivatedPets = 0;
  qint64 totalCurrentPower = 0;
  QList<AssetSnapshotPet> pets;
};

struct AssetSnapshotDelta {
  int newPets = 0;
  int newlyFullyCultivated = 0;
  int newlyRedStarComplete = 0;
  int newlyAstrolabeBreakthrough = 0;
  qint64 totalPowerChange = 0;
};
