#include "asset_analysis_controller.h"

#include "asset_snapshot_comparator.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"

AssetAnalysisController::AssetAnalysisController(
    PetRepository* repository, ShopExchangeController* shopController,
    RoutineOverviewController* routineController, QObject* parent)
    : QObject(parent), repository_(repository), shopController_(shopController),
      routineController_(routineController),
      analyzer_(repository, shopController, routineController),
      snapshotStore_(repository), settings_(repository) {
  if (!repository_) return;
  account_ = repository_->accountKey();
  inventorySignature_ = analyzer_.inventorySignature();
  autoSnapshotEnabled_ = settings_.loadAutoSnapshot(account_);
  connect(repository_, &PetRepository::dataChanged, this,
          &AssetAnalysisController::handleInventoryChanged);
  connect(repository_, &PetRepository::detailChanged, this,
          &AssetAnalysisController::handlePetDetailChanged);
  connect(repository_, &PetRepository::accountSessionChanged, this,
          &AssetAnalysisController::changeAccount);
  if (shopController_) {
    connect(shopController_, &ShopExchangeController::infoUpdated, this,
            &AssetAnalysisController::handleShopAnalysisInvalidated);
    connect(shopController_, &ShopExchangeController::catalogUpdated, this,
            &AssetAnalysisController::handleShopAnalysisInvalidated);
  }
  if (routineController_) {
    connect(routineController_, &RoutineOverviewController::dataUpdated, this,
            &AssetAnalysisController::handleRoutineSummaryChanged);
    connect(routineController_, &RoutineOverviewController::catalogUpdated, this,
            &AssetAnalysisController::handleRoutineSummaryChanged);
  }
}

AccountInventorySummary AssetAnalysisController::inventorySummary() const {
  return analyzer_.inventorySummary();
}

InventorySignature AssetAnalysisController::inventorySignature() const {
  return analyzer_.inventorySignature();
}

AccountAnalysisState& AssetAnalysisController::currentState() {
  return analysisStates_[account_];
}

const AccountAnalysisState* AssetAnalysisController::currentStateIfPresent() const {
  const auto iterator = analysisStates_.constFind(account_);
  return iterator == analysisStates_.constEnd() ? nullptr : &iterator.value();
}

AccountAssetOverview AssetAnalysisController::recalculateOverview() {
  AccountAnalysisState& state = currentState();
  state.lastValidOverview = analyzer_.analyze();
  state.analyzedSignature = analyzer_.inventorySignature();
  state.lastAnalysisAt = QDateTime::currentDateTime();
  state.dirtyPetIds.clear();
  state.hasAnalysis = true;
  state.inventoryStale = false;
  state.shopStale = false;
  inventorySignature_ = state.analyzedSignature;
  return state.lastValidOverview;
}

AccountAssetOverview AssetAnalysisController::overview() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state && state->hasAnalysis ? state->lastValidOverview
                                     : AccountAssetOverview{};
}

AccountAssetOverview AssetAnalysisController::routineSummary() const {
  return analyzer_.routineSummary();
}

bool AssetAnalysisController::hasAnalysis() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state && state->hasAnalysis;
}

QDateTime AssetAnalysisController::lastAnalyzedAt() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state ? state->lastAnalysisAt : QDateTime{};
}

QSet<qint64> AssetAnalysisController::dirtyPetIds() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state ? state->dirtyPetIds : QSet<qint64>{};
}

bool AssetAnalysisController::inventoryAnalysisStale() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state && state->inventoryStale;
}

bool AssetAnalysisController::shopAnalysisStale() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state && state->shopStale;
}

bool AssetAnalysisController::matchesFilter(const PetAssetRecord& pet,
                                            PetAssetFilter filter) {
  return AssetAnalyzer::matchesFilter(pet, filter);
}

void AssetAnalysisController::changeAccount(const QString& account, quint64) {
  account_ = account;
  inventorySignature_ = analyzer_.inventorySignature();
  AccountAnalysisState& state = currentState();
  if (state.hasAnalysis && state.analyzedSignature != inventorySignature_)
    state.inventoryStale = true;
  autoSnapshotEnabled_ = settings_.loadAutoSnapshot(account_);
  emit accountAnalysisChanged();
  emit inventoryCountsChanged();
  emit routineSummaryChanged();
  emit historyChanged();
}

void AssetAnalysisController::handleInventoryChanged() {
  if (!repository_ || repository_->accountKey() != account_) return;
  const InventorySignature next = analyzer_.inventorySignature();
  const bool membershipChanged = next != inventorySignature_;
  inventorySignature_ = next;
  emit inventoryCountsChanged();
  if (!membershipChanged) return;
  AccountAnalysisState& state = currentState();
  if (state.hasAnalysis) state.inventoryStale = true;
  emit inventoryMembershipChanged();
}

void AssetAnalysisController::handlePetDetailChanged(qint64 instanceId) {
  if (instanceId <= 0 || !repository_ || repository_->accountKey() != account_) return;
  AccountAnalysisState& state = currentState();
  if (state.dirtyPetIds.contains(instanceId)) return;
  state.dirtyPetIds.insert(instanceId);
  emit petDetailChanged(instanceId);
}

void AssetAnalysisController::handleShopAnalysisInvalidated() {
  if (!repository_ || repository_->accountKey() != account_) return;
  AccountAnalysisState& state = currentState();
  if (state.hasAnalysis) state.shopStale = true;
  emit shopAnalysisInvalidated();
}

void AssetAnalysisController::handleRoutineSummaryChanged() {
  if (!repository_ || repository_->accountKey() != account_) return;
  AccountAnalysisState& state = currentState();
  if (state.hasAnalysis)
    analyzer_.updateRoutineSummary(&state.lastValidOverview);
  emit routineSummaryChanged();
}

void AssetAnalysisController::setAutoSnapshotEnabled(bool enabled) {
  if (autoSnapshotEnabled_ == enabled) return;
  autoSnapshotEnabled_ = enabled;
  settings_.saveAutoSnapshot(account_, enabled);
  emit statusChanged(
      enabled ? QStringLiteral("已启用：每次手动刷新资产分析后同步更新当日快照")
              : QStringLiteral("已关闭：手动刷新分析后同步更新快照"));
}

bool AssetAnalysisController::recordSnapshot() {
  if (!repository_ || account_.isEmpty()) return false;
  const AccountAnalysisState* state = currentStateIfPresent();
  if (!state || !state->hasAnalysis) {
    emit statusChanged(QStringLiteral(
        "尚未分析，请先点击“重新计算养成分析（仅本地）”"));
    return false;
  }
  if (state->inventoryStale || !state->dirtyPetIds.isEmpty()) {
    emit statusChanged(QStringLiteral(
        "资产或详情已有变化，请先重新计算养成分析后再记录快照"));
    return false;
  }
  return recordSnapshotFromOverview(state->lastValidOverview);
}

bool AssetAnalysisController::recordSnapshotFromOverview(
    const AccountAssetOverview& current) {
  QString status;
  const bool saved = snapshotStore_.write(account_, current, &status);
  if (!status.isEmpty()) emit statusChanged(status);
  if (saved) emit historyChanged();
  return saved;
}

QList<AccountAssetSnapshot> AssetAnalysisController::snapshots() const {
  return snapshotStore_.readAll(account_);
}

AssetSnapshotDelta AssetAnalysisController::compareSnapshots(
    const AccountAssetSnapshot& current,
    const AccountAssetSnapshot& previous) {
  return AssetSnapshotComparator::compare(current, previous);
}
