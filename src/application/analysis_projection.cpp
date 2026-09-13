#include "analysis_projection.h"
#include <QThread>

AnalysisProjection::AnalysisProjection(QObject* parent) : AnalysisReadView(parent) {}
void AnalysisProjection::publish(std::shared_ptr<const AnalysisViewSnapshot> next) {
  if (!next) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (next->publication <= highestPublication_) return;
  highestPublication_ = next->publication;
  pendingHistory_ = pendingHistory_ || next->historyChanged;
  pending_ = std::move(next);
  if (scheduled_) return;
  scheduled_ = true;
  QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
}
void AnalysisProjection::drain() {
  Q_ASSERT(thread() == QThread::currentThread());
  std::shared_ptr<const AnalysisViewSnapshot> next;
  bool history = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next = std::move(pending_); scheduled_ = false;
    history = pendingHistory_; pendingHistory_ = false;
  }
  if (!next) return;
  const auto previous = snapshot_;
  snapshot_ = std::move(next);
  const bool session = !previous || previous->account != snapshot_->account || previous->sessionEpoch != snapshot_->sessionEpoch;
  const bool analysis = !previous || previous->analysisGeneration != snapshot_->analysisGeneration;
  if (session) emit accountAnalysisChanged();
  emit inventoryCountsChanged();
  if (snapshot_->inventoryStale && (!previous || !previous->inventoryStale)) emit inventoryMembershipChanged();
  for (qint64 id : snapshot_->dirtyPets)
    if (!previous || !previous->dirtyPets.contains(id)) emit petDetailChanged(id);
  if (snapshot_->shopStale && (!previous || !previous->shopStale)) emit shopAnalysisInvalidated();
  emit routineSummaryChanged();
  if (history || session) emit historyChanged();
  emit historyStateChanged();
  emit instanceHistoryChanged();
  if (!previous || previous->running != snapshot_->running) emit analysisRunningChanged(snapshot_->running);
  if (analysis && snapshot_->hasAnalysis) emit analysisCompleted();
}
AccountInventorySummary AnalysisProjection::inventorySummary() const { return snapshot_ ? snapshot_->inventory : AccountInventorySummary{}; }
AccountAssetOverview AnalysisProjection::overview() const { return snapshot_ ? snapshot_->overview : AccountAssetOverview{}; }
AccountAssetOverview AnalysisProjection::routineSummary() const { return snapshot_ ? snapshot_->routine : AccountAssetOverview{}; }
QList<ActionRecommendation> AnalysisProjection::recommendations() const { return snapshot_ ? snapshot_->recommendations : QList<ActionRecommendation>{}; }
bool AnalysisProjection::hasAnalysis() const { return snapshot_ && snapshot_->hasAnalysis; }
QDateTime AnalysisProjection::lastAnalyzedAt() const { return snapshot_ ? snapshot_->analyzedAt : QDateTime{}; }
QSet<qint64> AnalysisProjection::dirtyPetIds() const { return snapshot_ ? snapshot_->dirtyPets : QSet<qint64>{}; }
bool AnalysisProjection::inventoryAnalysisStale() const { return snapshot_ && snapshot_->inventoryStale; }
bool AnalysisProjection::shopAnalysisStale() const { return snapshot_ && snapshot_->shopStale; }
QList<AccountAssetSnapshot> AnalysisProjection::snapshots() const { return snapshot_ ? snapshot_->history : QList<AccountAssetSnapshot>{}; }
bool AnalysisProjection::autoSnapshotEnabled() const { return snapshot_ && snapshot_->autoSnapshot; }
bool AnalysisProjection::autoSnapshotSettingKnown() const { return snapshot_ && snapshot_->autoSnapshotKnown; }
bool AnalysisProjection::snapshotHistoryLoading() const { return snapshot_ && snapshot_->historyLoading; }
bool AnalysisProjection::snapshotHistoryHasOlder() const { return snapshot_ && snapshot_->historyHasOlder; }
QString AnalysisProjection::snapshotHistoryError() const { return snapshot_ ? snapshot_->historyError : QString{}; }
qint64 AnalysisProjection::instanceHistoryId() const { return snapshot_ ? snapshot_->instanceHistoryId : 0; }
bool AnalysisProjection::instanceHistoryLoading() const { return snapshot_ && snapshot_->instanceHistoryLoading; }
QString AnalysisProjection::instanceHistoryError() const { return snapshot_ ? snapshot_->instanceHistoryError : QString{}; }
QList<SnapshotInstanceHistoryEntry> AnalysisProjection::instanceHistory() const { return snapshot_ ? snapshot_->instanceHistory : QList<SnapshotInstanceHistoryEntry>{}; }
