#include "analysis_publisher.h"
#include "analysis_projection.h"
#include "asset_analysis_controller.h"
#include "pet_repository.h"
#include <QTimer>
#include <atomic>

AnalysisPublisher::AnalysisPublisher(PetRepository* repository, AssetAnalysisController* controller,
                                     AnalysisProjection* projection, QObject* parent)
    : QObject(parent), repository_(repository), controller_(controller), projection_(projection) {
  connect(controller_, &AssetAnalysisController::inventoryCountsChanged, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::inventoryMembershipChanged, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::petDetailChanged, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::shopAnalysisInvalidated, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::routineSummaryChanged, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::accountAnalysisChanged, this, [this] { schedule(true); });
  connect(controller_, &AssetAnalysisController::historyChanged, this, [this] { schedule(true); });
  connect(controller_, &AssetAnalysisController::historyStateChanged, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::instanceHistoryChanged, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::analysisCompleted, this, [this] { schedule(); });
  connect(controller_, &AssetAnalysisController::analysisRunningChanged, this, [this](bool running) {
    running_ = running; schedule();
  });
  connect(controller_, &AssetAnalysisController::statusChanged, projection_, &AnalysisProjection::statusChanged,
          Qt::QueuedConnection);
  // User actions route through ApplicationRuntime's GUI facade, which freezes
  // the clicked projection account/epoch and checks them again on Core.
  schedule(true);
}
void AnalysisPublisher::schedule(bool history) {
  historyDirty_ = historyDirty_ || history;
  if (scheduled_) return;
  scheduled_ = true;
  QTimer::singleShot(0, this, &AnalysisPublisher::publish);
}
void AnalysisPublisher::publish() {
  static std::atomic<quint64> publications{0};
  auto snapshot = std::make_shared<AnalysisViewSnapshot>();
  snapshot->publication = ++publications;
  snapshot->account = repository_->accountKey();
  snapshot->sessionEpoch = repository_->sessionGeneration();
  snapshot->analysisGeneration = controller_->analysisRunCount();
  snapshot->inventory = controller_->inventorySummary();
  snapshot->overview = controller_->overview();
  snapshot->routine = controller_->routineSummary();
  snapshot->recommendations = controller_->recommendations();
  snapshot->analyzedAt = controller_->lastAnalyzedAt();
  snapshot->dirtyPets = controller_->dirtyPetIds();
  snapshot->hasAnalysis = controller_->hasAnalysis();
  snapshot->inventoryStale = controller_->inventoryAnalysisStale();
  snapshot->shopStale = controller_->shopAnalysisStale();
  snapshot->autoSnapshot = controller_->autoSnapshotEnabled();
  snapshot->autoSnapshotKnown = controller_->autoSnapshotSettingKnown();
  snapshot->historyLoading = controller_->snapshotHistoryLoading();
  snapshot->historyHasOlder = controller_->snapshotHistoryHasOlder();
  snapshot->historyError = controller_->snapshotHistoryError();
  snapshot->instanceHistoryId = controller_->instanceHistoryId();
  snapshot->instanceHistoryLoading = controller_->instanceHistoryLoading();
  snapshot->instanceHistoryError = controller_->instanceHistoryError();
  snapshot->instanceHistory = controller_->instanceHistory();
  snapshot->running = running_;
  snapshot->historyChanged = historyDirty_;
  snapshot->history = historyDirty_ || !last_ ? controller_->snapshots() : last_->history;
  historyDirty_ = false; scheduled_ = false;
  last_ = snapshot;
  projection_->publish(std::move(snapshot));
}
