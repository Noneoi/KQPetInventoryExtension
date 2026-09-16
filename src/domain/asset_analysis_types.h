#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QStringList>
#include <memory>

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
  ShopImprovable,
  StargodEquipNeeded,
  StargodUpgradeNeeded,
  ChangeableMissing,
  AnalysisIncomplete
};

struct PetAssetRecord {
  // Declared first so reverse destruction frees all payload before its lease.
  std::shared_ptr<void> memoryRetention;
  qint64 instanceId = 0;
  int raceId = 0;
  int metadataSlotMaxLevel = 0;
  bool observationVerified = false;
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
  bool currentPowerKnown = false;
  bool extremePowerKnown = false;
  bool highestPowerKnown = false;
  bool completionKnown = false;
  bool powerGapKnown = false;
  bool cultivationKnown = false;
  bool redStarKnown = false;
  bool astrolabeKnown = false;
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

// A period total is only meaningful together with how complete its sources are.
// A confirmed subtotal from part of the sources is never the period total.
enum class RoutineCompleteness { Unknown, Partial, Complete, Overflow };

struct RoutineOpportunitySummary {
  RoutineCompleteness completeness = RoutineCompleteness::Unknown;
  int total = 0;              // confirmed sources only; 0 when Overflow
  int confirmedSources = 0;
  int expectedSources = 0;    // sources this analysis covers for the account
  QStringList pendingSources; // covered sources without a confirmed current value
  QDateTime observedAt;       // newest observation among the confirmed sources
};

struct AccountAssetOverview {
  std::shared_ptr<void> memoryRetention;
  int analysisVersion = AssetAnalysisVersion::kCurrentAnalysis;
  quint64 inputSessionEpoch = 0;
  quint64 inventoryRevision = 0;
  bool sourceVerified = false;
  bool inputStale = false;
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
  bool dailyTasksKnown = false;
  bool weeklyTasksKnown = false;
  int unfinishedDailyTasks = 0;
  int unfinishedWeeklyTasks = 0;
  // "Known" now means the period total is complete: every covered source was
  // confirmed for its current period. A partial subtotal keeps this false and
  // is reported through the summaries below instead.
  bool todayOpportunityKnown = false;
  bool weekOpportunityKnown = false;
  int todayOpportunityRemaining = 0;
  int weekOpportunityRemaining = 0;
  RoutineOpportunitySummary todayOpportunities;
  RoutineOpportunitySummary weekOpportunities;
  qint64 totalCurrentPower = 0;
  bool totalCurrentPowerKnown = false;
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
  bool currentPowerKnown = false;
  bool cultivationKnown = false;
  bool redStarKnown = false;
  bool astrolabeKnown = false;
};

struct AccountAssetSnapshot {
  int schemaVersion = 0;
  int analysisVersion = 0;
  QString account;
  QDateTime createdAt;
  int totalPets = 0;
  int fullyCultivatedPets = 0;
  qint64 totalCurrentPower = 0;
  bool totalCurrentPowerKnown = false;
  bool petsComplete = true;
  QString storageKey;
  QList<AssetSnapshotPet> pets;
};

struct SnapshotInstanceHistoryEntry {
  QString storageKey;
  QDateTime createdAt;
  int schemaVersion = 0;
  int analysisVersion = 0;
  bool membershipKnown = false;
  bool present = false;
  AssetSnapshotPet pet;
};

struct AssetSnapshotDelta {
  int newPets = 0;
  int removedPets = 0;
  int newlyFullyCultivated = 0;
  int newlyRedStarComplete = 0;
  int newlyAstrolabeBreakthrough = 0;
  qint64 totalPowerChange = 0;
  bool accountComparable = false;
  bool cultivationComparable = false;
  bool powerChangeKnown = false;
};

Q_DECLARE_METATYPE(SnapshotInstanceHistoryEntry)
