#pragma once
#include "domain/asset_analysis_types.h"
#include "domain/recommendation_types.h"
#include <QObject>
#include <QSet>

class AnalysisReadView : public QObject {
  Q_OBJECT
public:
  explicit AnalysisReadView(QObject* parent = nullptr) : QObject(parent) {}
  virtual AccountInventorySummary inventorySummary() const = 0;
  virtual AccountAssetOverview overview() const = 0;
  virtual AccountAssetOverview routineSummary() const = 0;
  virtual QList<ActionRecommendation> recommendations() const = 0;
  virtual bool hasAnalysis() const = 0;
  virtual bool analysisRunning() const = 0;
  virtual QDateTime lastAnalyzedAt() const = 0;
  virtual QSet<qint64> dirtyPetIds() const = 0;
  virtual bool inventoryAnalysisStale() const = 0;
  virtual bool shopAnalysisStale() const = 0;
  virtual QList<AccountAssetSnapshot> snapshots() const = 0;
  virtual bool autoSnapshotEnabled() const = 0;
  virtual bool autoSnapshotSettingKnown() const = 0;
  virtual bool snapshotHistoryLoading() const = 0;
  virtual bool snapshotHistoryHasOlder() const = 0;
  virtual QString snapshotHistoryError() const = 0;
  virtual qint64 instanceHistoryId() const = 0;
  virtual bool instanceHistoryLoading() const = 0;
  virtual QString instanceHistoryError() const = 0;
  virtual QList<SnapshotInstanceHistoryEntry> instanceHistory() const = 0;
public slots:
  virtual void requestAnalysis() = 0;
  virtual void cancelAnalysis() = 0;
  virtual void requestSnapshot() = 0;
  virtual void setAutoSnapshotEnabled(bool enabled) = 0;
  virtual void requestSnapshotHistory(const QDateTime& before = {}) = 0;
  virtual void requestSnapshotDetails(const QString& key) = 0;
  virtual void requestInstanceHistory(qint64 instanceId) = 0;
signals:
  void inventoryCountsChanged();
  void inventoryMembershipChanged();
  void petDetailChanged(qint64 instanceId);
  void shopAnalysisInvalidated();
  void routineSummaryChanged();
  void accountAnalysisChanged();
  void historyChanged();
  void historyStateChanged();
  void instanceHistoryChanged();
  void statusChanged(const QString& status);
  void analysisCompleted();
  void analysisRunningChanged(bool running);
};
