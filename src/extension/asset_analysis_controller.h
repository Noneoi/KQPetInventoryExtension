#pragma once

#include "account_analysis_state.h"
#include "asset_analysis_settings.h"
#include "asset_analyzer.h"
#include "asset_snapshot_store.h"

#include <QHash>
#include <QObject>

class PetRepository;
class ShopExchangeController;
class RoutineOverviewController;

class AssetAnalysisController final : public QObject {
  Q_OBJECT

public:
  AssetAnalysisController(PetRepository* repository,
                          ShopExchangeController* shopController,
                          RoutineOverviewController* routineController,
                          QObject* parent = nullptr);

  AccountInventorySummary inventorySummary() const;
  InventorySignature inventorySignature() const;
  AccountAssetOverview recalculateOverview();
  AccountAssetOverview overview() const;
  AccountAssetOverview routineSummary() const;
  bool hasAnalysis() const;
  QDateTime lastAnalyzedAt() const;
  QSet<qint64> dirtyPetIds() const;
  bool inventoryAnalysisStale() const;
  bool shopAnalysisStale() const;
  quint64 analysisRunCount() const { return analyzer_.analysisRunCount(); }
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
  void inventoryCountsChanged();
  void inventoryMembershipChanged();
  void petDetailChanged(qint64 instanceId);
  void shopAnalysisInvalidated();
  void routineSummaryChanged();
  void accountAnalysisChanged();
  void historyChanged();
  void statusChanged(const QString& status);

private:
  void changeAccount(const QString& account, quint64 generation);
  void handleInventoryChanged();
  void handlePetDetailChanged(qint64 instanceId);
  void handleShopAnalysisInvalidated();
  void handleRoutineSummaryChanged();

  AccountAnalysisState& currentState();
  const AccountAnalysisState* currentStateIfPresent() const;

  PetRepository* repository_ = nullptr;
  ShopExchangeController* shopController_ = nullptr;
  RoutineOverviewController* routineController_ = nullptr;
  AssetAnalyzer analyzer_;
  AssetSnapshotStore snapshotStore_;
  AssetAnalysisSettings settings_;
  QString account_;
  InventorySignature inventorySignature_;
  QHash<QString, AccountAnalysisState> analysisStates_;
  bool autoSnapshotEnabled_ = false;
};
