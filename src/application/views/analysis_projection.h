#pragma once
#include "analysis_read_view.h"
#include <memory>
#include <mutex>

struct AnalysisViewSnapshot {
  quint64 publication = 0;
  quint64 sessionEpoch = 0;
  quint64 analysisGeneration = 0;
  QString account;
  AccountInventorySummary inventory;
  AccountAssetOverview overview;
  AccountAssetOverview routine;
  QList<ActionRecommendation> recommendations;
  QList<AccountAssetSnapshot> history;
  QList<SnapshotInstanceHistoryEntry> instanceHistory;
  qint64 instanceHistoryId = 0;
  bool historyLoading = false;
  bool historyHasOlder = false;
  QString historyError;
  bool instanceHistoryLoading = false;
  QString instanceHistoryError;
  bool autoSnapshotKnown = false;
  QDateTime analyzedAt;
  QSet<qint64> dirtyPets;
  bool hasAnalysis = false;
  bool inventoryStale = false;
  bool shopStale = false;
  bool autoSnapshot = false;
  bool running = false;
  bool historyChanged = false;
};

class AnalysisProjection final : public AnalysisReadView {
  Q_OBJECT
public:
  explicit AnalysisProjection(QObject* parent = nullptr);
  void publish(std::shared_ptr<const AnalysisViewSnapshot> snapshot);
  QString accountKey() const { return snapshot_ ? snapshot_->account : QString{}; }
  quint64 sessionEpoch() const { return snapshot_ ? snapshot_->sessionEpoch : 0; }
  AccountInventorySummary inventorySummary() const override;
  AccountAssetOverview overview() const override;
  AccountAssetOverview routineSummary() const override;
  QList<ActionRecommendation> recommendations() const override;
  bool hasAnalysis() const override;
  bool analysisRunning() const override { return snapshot_ && snapshot_->running; }
  QDateTime lastAnalyzedAt() const override;
  QSet<qint64> dirtyPetIds() const override;
  bool inventoryAnalysisStale() const override;
  bool shopAnalysisStale() const override;
  QList<AccountAssetSnapshot> snapshots() const override;
  bool autoSnapshotEnabled() const override;
  bool autoSnapshotSettingKnown() const override;
  bool snapshotHistoryLoading() const override;
  bool snapshotHistoryHasOlder() const override;
  QString snapshotHistoryError() const override;
  qint64 instanceHistoryId() const override;
  bool instanceHistoryLoading() const override;
  QString instanceHistoryError() const override;
  QList<SnapshotInstanceHistoryEntry> instanceHistory() const override;
public slots:
  void requestAnalysis() override { emit analysisRequested(); }
  void cancelAnalysis() override { emit analysisCancellationRequested(); }
  void requestSnapshot() override { emit snapshotRequested(); }
  void setAutoSnapshotEnabled(bool enabled) override { emit autoSnapshotChangeRequested(enabled); }
  void requestSnapshotHistory(const QDateTime& before = {}) override { emit historyRequested(before); }
  void requestSnapshotDetails(const QString& key) override { emit snapshotDetailsRequested(key); }
  void requestInstanceHistory(qint64 id) override { emit instanceHistoryRequested(id); }
signals:
  void analysisRequested();
  void analysisCancellationRequested();
  void snapshotRequested();
  void autoSnapshotChangeRequested(bool enabled);
  void historyRequested(const QDateTime& before);
  void snapshotDetailsRequested(const QString& key);
  void instanceHistoryRequested(qint64 id);
private:
  void drain();
  std::shared_ptr<const AnalysisViewSnapshot> snapshot_;
  std::mutex mutex_;
  std::shared_ptr<const AnalysisViewSnapshot> pending_;
  quint64 highestPublication_ = 0;
  bool scheduled_ = false;
  bool pendingHistory_ = false;
};
