#pragma once

#include "account_analysis_state.h"
#include "asset_analysis_settings.h"
#include "asset_analyzer.h"
#include "asset_snapshot_store.h"
#include "analysis_read_view.h"
#include "analysis_worker.h"
#include "analysis_environment.h"
#include "../contracts/pet_derivation_types.h"

#include <QHash>
#include <QObject>

class PetRepository;
class ShopExchangeController;
class RoutineOverviewController;
class PetDerivationCache;
class QTimer;

class AssetAnalysisController final : public AnalysisReadView {
  Q_OBJECT

public:
  AssetAnalysisController(PetRepository* repository,
                          ShopExchangeController* shopController,
                          RoutineOverviewController* routineController,
                          QObject* parent = nullptr,
                          AnalysisEnvironment environment = {});
  ~AssetAnalysisController() override;

  AccountInventorySummary inventorySummary() const;
  InventorySignature inventorySignature() const;
  AccountAssetOverview recalculateOverview();
  AccountAssetOverview overview() const;
  AccountAssetOverview routineSummary() const;
  QList<ActionRecommendation> recommendations() const;
  bool hasAnalysis() const;
  QDateTime lastAnalyzedAt() const;
  QSet<qint64> dirtyPetIds() const;
  bool inventoryAnalysisStale() const;
  bool shopAnalysisStale() const;
  quint64 analysisRunCount() const { return analysisRunCount_; }
  quint64 preparationReadAdmissionRetries() const { return preparationReadAdmissionRetries_; }
  bool analysisRunning() const override { return analysisQueued_; }
  AnalysisMemoryUsage analysisMemoryUsage() const { return worker_.memoryUsage(); }
  AnalysisJobKey currentAnalysisJob() const { return currentJob_; }
  std::shared_ptr<const AnalysisWorkResult> retainedAnalysisResult() const;
  bool postPriorityCompute(std::function<void()> job) { return worker_.postPriority(std::move(job)); }
  void setCompatibilityIdentity(const QString& build, const QString& profile, bool verified);
  void metadataChanged();
  void checkInputFreshness();
  void setDerivationCache(PetDerivationCache* cache);
  bool shutdownAnalysis(int maximumWaitMilliseconds = 2000);
  bool autoSnapshotSettingKnown() const override { return settings_.known(account_) && !settings_.pending(account_); }
  int persistencePendingTaskCount() const { return snapshotStore_.pendingTaskCount() + settings_.pendingTaskCount(); }
  int pendingWriteCount() const { return snapshotStore_.pendingWriteCount() + settings_.pendingWriteCount(); }
  bool snapshotHistoryLoading() const override { return snapshotStore_.historyLoading(); }
  bool snapshotHistoryHasOlder() const override { return snapshotStore_.cacheStats().hasOlderHistory; }
  QString snapshotHistoryError() const override { return snapshotHistoryError_; }
  qint64 instanceHistoryId() const override { return instanceHistoryId_; }
  bool instanceHistoryLoading() const override { return snapshotStore_.instanceHistoryLoading(); }
  QString instanceHistoryError() const override { return instanceHistoryError_; }
  QList<SnapshotInstanceHistoryEntry> instanceHistory() const override {
    return snapshotStore_.cachedInstanceHistory(account_, instanceHistoryId_);
  }
  QList<AccountAssetSnapshot> snapshots() const;
  bool autoSnapshotEnabled() const { return autoSnapshotEnabled_; }
  bool recordSnapshotFromOverview(const AccountAssetOverview& current);
  static bool matchesFilter(const PetAssetRecord& pet, PetAssetFilter filter);
  static AssetSnapshotDelta compareSnapshots(const AccountAssetSnapshot& current,
                                             const AccountAssetSnapshot& previous);

public slots:
  void requestAnalysis() override;
  void cancelAnalysis() override;
  void requestSnapshotHistory(const QDateTime& before = {}) override;
  void requestSnapshotDetails(const QString& key) override;
  void requestInstanceHistory(qint64 instanceId) override;
  void requestSnapshot() override { recordSnapshot(); }
  void setAutoSnapshotEnabled(bool enabled);
  bool recordSnapshot();

signals:
  void analysisJobFinished(const AnalysisJobFinished& finished);
  void derivedFactsChanged(qint64 id, const PetDerivedFactsHandle& facts);
  void derivedFactsFailed(const PetDerivationKey& key, const QString& error);
  void persistenceChanged(const QString& account, quint64 epoch, const QString& record,
                          quint64 revision, StorageStatus status, const QString& error);
  // Frozen input is complete; useful for measuring capture separately from
  // Compute. Any later mutation leaves this exact job's versions unchanged.
  void analysisInputCaptured(quint64 jobId);

private:
  void changeAccount(const QString& account, quint64 generation);
  void handleInventoryChanged();
  void handlePetDetailChanged(qint64 instanceId);
  void handleShopAnalysisInvalidated();
  void handleRoutineSummaryChanged();
  AnalysisVersionStamp versions() const;
  AnalysisJobKey nextJobKey();
  std::shared_ptr<const AnalysisWorkInput> capture(const AnalysisJobKey& key);
  bool jobCurrent(const AnalysisJobKey& key) const;
  void publishResult(std::shared_ptr<const AnalysisWorkResult> result);
  void finishJob(const AnalysisJobFinished& finished);
  void trimAccountStates();
  void pumpDerivations();
  void queueDerivation(qint64 id);
  void acceptDerivedFacts(const PetDerivationKey& key, const PetDerivedFactsHandle& facts);
  void failPreparation(const QString& reason);
  QList<qint64> currentIds() const;
  PetDerivationKey derivationKey(qint64 id) const;
  void refreshPreparationMembers();
  QDate businessDate() const;
  std::shared_ptr<const PetDetailCatalogSnapshot> petMetadataSnapshot() const;
  std::shared_ptr<const ShopCatalogSnapshot> shopCatalogSnapshot() const;

  AccountAnalysisState& currentState();
  const AccountAnalysisState* currentStateIfPresent() const;

  PetRepository* repository_ = nullptr;
  ShopExchangeController* shopController_ = nullptr;
  RoutineOverviewController* routineController_ = nullptr;
  AnalysisEnvironment environment_;
  AssetAnalyzer analyzer_;
  AssetSnapshotStore snapshotStore_;
  AssetAnalysisSettings settings_;
  AnalysisWorker worker_;
  QPointer<PetDerivationCache> derivations_;
  QMetaObject::Connection rawAvailableConnection_;
  QMetaObject::Connection rawLoadConnection_;
  QTimer* derivationPump_ = nullptr;
  QSet<qint64> derivationBacklog_;
  QSet<qint64> readyRawDerivations_;
  QSet<qint64> preparationIds_;
  QSet<qint64> awaitingFacts_;
  QSet<qint64> localReadChecked_;
  QHash<qint64, PetRecordKey> localReadPending_;
  QHash<qint64, PetRecordKey> derivationVersions_;
  QHash<qint64, PetDerivedFactsHandle> preparedHandles_;
  bool preparingFacts_ = false;
  bool preparationStarted_ = false;
  qint64 preparationStartedAt_ = 0;
  quint64 preparationReadAdmissionRetries_ = 0;
  QString account_;
  InventorySignature inventorySignature_;
  QHash<QString, AccountAnalysisState> analysisStates_;
  QStringList accountRecency_;
  AnalysisJobKey currentJob_;
  quint64 nextJobId_ = 0;
  quint64 analysisRunCount_ = 0;
  quint64 detailRevision_ = 0;
  quint64 shopRevision_ = 0;
  quint64 routineRevision_ = 0;
  quint64 trustRevision_ = 0;
  QString buildIdentity_;
  QString profileIdentity_;
  bool compatibilityVerified_ = false;
  bool closing_ = false;
  bool autoSnapshotEnabled_ = false;
  bool analysisQueued_ = false;
  QString snapshotHistoryError_;
  QString instanceHistoryError_;
  qint64 instanceHistoryId_ = 0;
};
