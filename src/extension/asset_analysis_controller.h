#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>

class PetRepository;
class ShopExchangeController;
class RoutineOverviewController;

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
  bool detailAvailable = false;
  bool fullyCultivated = false;
  bool improvable = false;
  bool redStarMissing = false;
  bool astrolabeMissing = false;
  bool sacredMissing = false;
  bool soulMissing = false;
  bool shopImprovable = false;
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
};

struct AccountAssetOverview {
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

class AssetAnalysisController final : public QObject {
  Q_OBJECT

public:
  AssetAnalysisController(PetRepository* repository,
                          ShopExchangeController* shopController,
                          RoutineOverviewController* routineController,
                          QObject* parent = nullptr);

  AccountInventorySummary inventorySummary() const;
  AccountAssetOverview overview() const;
  QList<AccountAssetSnapshot> snapshots() const;
  bool autoSnapshotEnabled() const { return autoSnapshotEnabled_; }
  bool recordSnapshotFromOverview(const AccountAssetOverview& current);
  static bool matchesFilter(const PetAssetRecord& pet, PetAssetFilter filter);
  static AssetSnapshotDelta compareSnapshots(const AccountAssetSnapshot& current,
                                             const AccountAssetSnapshot& previous);

public slots:
  void setAutoSnapshotEnabled(bool enabled);
  bool recordSnapshot();

signals:
  void inventoryChanged();
  void analysisInvalidated();
  void historyChanged();
  void statusChanged(const QString& status);

private:
  QString accountDirectory() const;
  QString settingsPath() const;
  QString snapshotsDirectory() const;
  void changeAccount(const QString& account, quint64 generation);
  void loadSettings();
  void saveSettings() const;

  PetRepository* repository_ = nullptr;
  ShopExchangeController* shopController_ = nullptr;
  RoutineOverviewController* routineController_ = nullptr;
  QString account_;
  bool autoSnapshotEnabled_ = false;
};
