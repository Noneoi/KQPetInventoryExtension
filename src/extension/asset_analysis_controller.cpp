#include "asset_analysis_controller.h"

#include "asset_snapshot_comparator.h"
#include "pet_repository.h"
#include "pet_detail_catalog.h"
#include "routine_overview_catalog.h"
#include <QPointer>
#include "account_resource_view.h"
#include "recommendation_engine.h"
#include "routine_overview_controller.h"
#include "shop_exchange_catalog.h"
#include "shop_exchange_controller.h"
#include <QTimer>
#include <QElapsedTimer>
#include "pet_derivation_cache.h"
#include "../domain/pet_identity.h"
#include "../domain/pet_metadata_view.h"

AssetAnalysisController::AssetAnalysisController(
    PetRepository* repository, ShopExchangeController* shopController,
    RoutineOverviewController* routineController, QObject* parent, AnalysisEnvironment environment)
    : AnalysisReadView(parent), repository_(repository), shopController_(shopController),
      routineController_(routineController), environment_(std::move(environment)),
      analyzer_(repository, shopController, routineController),
      snapshotStore_(repository), settings_(repository), worker_({}, this) {
  if (!repository_) return;
  derivationPump_ = new QTimer(this);
  derivationPump_->setSingleShot(true);
  connect(derivationPump_, &QTimer::timeout, this, &AssetAnalysisController::pumpDerivations);
  account_ = repository_->accountKey();
  inventorySignature_ = analyzer_.inventorySignature();
  settings_.requestLoad(account_);
  snapshotStore_.requestHistory(account_);
  connect(repository_, &PetRepository::dataChanged, this,
          &AssetAnalysisController::handleInventoryChanged);
  connect(repository_, &PetRepository::detailChanged, this,
          &AssetAnalysisController::handlePetDetailChanged);
  connect(repository_, &PetRepository::accountSessionChanged, this,
          &AssetAnalysisController::changeAccount);
  connect(repository_, &PetRepository::sessionTrustChanged, this,
          [this](SessionConnectionState, const QString&) {
    ++trustRevision_;
    AccountAnalysisState& current = currentState();
    if (current.hasAnalysis) { current.inventoryStale = true; current.shopStale = true; }
    emit shopAnalysisInvalidated();
  });
  worker_.setInputFactory([this](const AnalysisJobKey& key) { return capture(key); });
  worker_.setPublicationGuard([this](const AnalysisJobKey& key) { return jobCurrent(key); });
  worker_.setResultHandler([this](std::shared_ptr<const AnalysisWorkResult> result) { publishResult(std::move(result)); });
  worker_.setFinishedHandler([this](const AnalysisJobFinished& finished) { finishJob(finished); });
  connect(&settings_, &AssetAnalysisSettings::stateChanged, this,
      [this](const QString& account, quint64 epoch, bool known, bool enabled, bool pending, const QString& error) {
    if (!repository_ || account != account_ || epoch != repository_->sessionGeneration()) return;
    autoSnapshotEnabled_ = known && !pending && enabled;
    QPointer<AssetAnalysisController> alive(this);
    emit accountAnalysisChanged();
    if (!alive) return;
    emit historyStateChanged();
    if (alive && !error.isEmpty()) emit statusChanged(error);
  });
  connect(&settings_, &AssetAnalysisSettings::writeFinished, this,
      [this](quint64, const QString& account, quint64 epoch, bool enabled, StorageStatus status, const QString& error) {
    if (!repository_ || account != account_ || epoch != repository_->sessionGeneration() || status == StorageStatus::Superseded) return;
    emit statusChanged(status == StorageStatus::Saved
        ? (enabled ? QStringLiteral("已保存：手动分析完成后更新快照") : QStringLiteral("已保存：关闭分析后自动快照"))
        : QStringLiteral("快照设置未保存：%1").arg(error));
  });
  connect(&settings_, &AssetAnalysisSettings::writeFinished, this,
      [this](quint64 task, const QString& account, quint64 epoch, bool, StorageStatus status, const QString& error) {
    emit persistenceChanged(account, epoch, QStringLiteral("asset-analysis.json"), task, status, error);
  });
  connect(&snapshotStore_, &AssetSnapshotStore::writeStateChanged, this,
      [this](quint64 task, const QString& account, quint64 epoch, const QString& key, StorageStatus status, const QString& error) {
    emit persistenceChanged(account, epoch, key, task, status, error);
  });
  connect(&snapshotStore_, &AssetSnapshotStore::historyChanged, this,
      [this](const QString& account, quint64 epoch) {
    if (repository_ && account == account_ && epoch == repository_->sessionGeneration()) emit historyChanged();
  });
  connect(&snapshotStore_, &AssetSnapshotStore::historyLoadingChanged, this,
      [this](const QString& account, quint64 epoch, bool, const QString& error) {
    if (!repository_ || account != account_ || epoch != repository_->sessionGeneration()) return;
    const bool reportError = !error.isEmpty() && error != snapshotHistoryError_;
    snapshotHistoryError_ = error;
    QPointer<AssetAnalysisController> alive(this);
    emit historyStateChanged();
    if (alive && reportError) emit statusChanged(error);
  });
  connect(&snapshotStore_, &AssetSnapshotStore::instanceHistoryChanged, this,
      [this](const QString& account, quint64 epoch, qint64 instanceId, bool, const QString& error) {
    if (!repository_ || account != account_ || epoch != repository_->sessionGeneration() || instanceId != instanceHistoryId_) return;
    const bool reportError = !error.isEmpty() && error != instanceHistoryError_;
    instanceHistoryError_ = error;
    QPointer<AssetAnalysisController> alive(this);
    emit instanceHistoryChanged();
    if (alive && reportError) emit statusChanged(error);
  });
  connect(&snapshotStore_, &AssetSnapshotStore::snapshotDetailsChanged, this,
      [this](const QString& account, quint64 epoch, const QString&, bool available, const QString& error) {
    if (!repository_ || account != account_ || epoch != repository_->sessionGeneration()) return;
    if (!available && !error.isEmpty()) snapshotHistoryError_ = error;
    QPointer<AssetAnalysisController> alive(this);
    emit historyChanged();
    if (!alive) return;
    emit historyStateChanged();
    if (alive && !error.isEmpty()) emit statusChanged(error);
  });
  connect(&snapshotStore_, &AssetSnapshotStore::writeFinished, this,
      [this](quint64, const QString& account, quint64 epoch, quint64, int, StorageStatus status, const QString& error) {
    if (!repository_ || account != account_ || epoch != repository_->sessionGeneration() || status == StorageStatus::Superseded) return;
    emit statusChanged(status == StorageStatus::Saved ? QStringLiteral("资产快照已保存")
                                                     : QStringLiteral("资产快照未保存：%1").arg(error));
  });
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

void AssetAnalysisController::setDerivationCache(PetDerivationCache* cache) {
  if (derivations_ == cache || !repository_) return;
  QPointer<AssetAnalysisController> alive(this);
  cancelAnalysis();
  if (!alive) return;
  disconnect(rawAvailableConnection_); disconnect(rawLoadConnection_);
  if (derivations_) disconnect(derivations_, nullptr, this, nullptr);
  derivationBacklog_.clear(); localReadPending_.clear(); localReadChecked_.clear();
  readyRawDerivations_.clear();
  derivationVersions_.clear();
  derivations_ = cache;
  if (!cache) return;
  cache->bindSession(account_, repository_->sessionGeneration());
  rawAvailableConnection_ = connect(repository_, &PetRepository::rawRecordAvailable, this, [this](const RawPetRecordHandle& raw) {
    if (raw && raw->key.account == account_ && raw->key.epoch == repository_->sessionGeneration())
      queueDerivation(raw->key.instanceId);
  });
  rawLoadConnection_ = connect(repository_, &PetRepository::rawRecordLoadFinished, this,
      [this](const PetRecordKey& requested, bool loaded, const QString& error, StorageStatus status) {
    if (!derivations_ || requested.account != account_ || requested.epoch != repository_->sessionGeneration()) return;
    const qint64 id = requested.instanceId;
    const auto pending = localReadPending_.constFind(id);
    if (pending != localReadPending_.constEnd()) {
      if (!(pending.value() == requested)) return;
      localReadPending_.erase(pending);
    }
    const auto version = repository_->recordVersion(id);
    if (!loaded && !(requested == version.key)) { queueDerivation(id); return; }
    if (!loaded && (status == StorageStatus::QueueFull || status == StorageStatus::Cancelled || status == StorageStatus::Superseded)) {
      ++preparationReadAdmissionRetries_;
      queueDerivation(id); return;
    }
    if (!loaded && version.complete && !repository_->rawRecordResident(id) && !derivations_->lookup(derivationKey(id))) {
      if (preparingFacts_ && preparationIds_.contains(id))
        failPreparation(QStringLiteral("实例 %1 的本地原文不可用：%2；请刷新详情后重试，已保留上次分析").arg(id).arg(error));
      return; // No infinite background re-read of a lost durable record.
    }
    if (!loaded && status != StorageStatus::NotFound) {
      if (preparingFacts_ && preparationIds_.contains(id))
        failPreparation(QStringLiteral("实例 %1 的本地详情未能读取：%2；已保留上次分析").arg(id).arg(error));
      return;
    }
    localReadChecked_.insert(id);
    queueDerivation(id);
  });
  connect(cache, &PetDerivationCache::ready, this,
      [this](quint64, const PetDerivationKey& key, const PetDerivedFactsHandle& facts) { acceptDerivedFacts(key, facts); });
  connect(cache, &PetDerivationCache::failed, this,
      [this](quint64, const PetDerivationKey& key, PetDerivationStatus status, const QString& error) {
    if (key.record.account != account_ || key.record.epoch != repository_->sessionGeneration()) return;
    if (key == derivationKey(key.record.instanceId) && status != PetDerivationStatus::Cancelled &&
        status != PetDerivationStatus::Superseded) emit derivedFactsFailed(key,error);
    if (key == derivationKey(key.record.instanceId) && preparingFacts_ && preparationIds_.contains(key.record.instanceId) &&
        status != PetDerivationStatus::Cancelled && status != PetDerivationStatus::Superseded)
      failPreparation(QStringLiteral("实例 %1 派生未完成：%2；已保留上次分析").arg(key.record.instanceId).arg(error));
  });
  connect(cache, &PetDerivationCache::stateChanged, this, [this] {
    if (!closing_ && derivationPump_ && !derivationPump_->isActive()) derivationPump_->start(1);
  });
  for (auto id : currentIds()) queueDerivation(id);
}

QList<qint64> AssetAnalysisController::currentIds() const {
  return repository_ ? repository_->currentInstanceIds() : QList<qint64>{};
}

QDate AssetAnalysisController::businessDate() const {
  return environment_.businessDate ? environment_.businessDate() : currentCatalogBusinessDate();
}
std::shared_ptr<const PetDetailCatalogSnapshot> AssetAnalysisController::petMetadataSnapshot() const {
  return environment_.petMetadataSnapshot ? environment_.petMetadataSnapshot() : PetDetailCatalog::instance().snapshot();
}
std::shared_ptr<const ShopCatalogSnapshot> AssetAnalysisController::shopCatalogSnapshot() const {
  return environment_.shopCatalogSnapshot ? environment_.shopCatalogSnapshot() : ShopExchangeCatalog::instance().snapshot();
}
std::shared_ptr<const AnalysisWorkResult> AssetAnalysisController::retainedAnalysisResult() const {
  const auto* state = currentStateIfPresent();
  return state ? state->retainedResult : std::shared_ptr<const AnalysisWorkResult>{};
}
PetDerivationKey AssetAnalysisController::derivationKey(qint64 id) const {
  const auto metadata = petMetadataSnapshot();
  return {repository_->recordVersion(id).key, metadata ? metadata->revision : 0,
      metadata ? metadata->contentDigest : QByteArray{}, AssetAnalysisVersion::kCurrentAnalysis};
}

void AssetAnalysisController::queueDerivation(qint64 id) {
  if (closing_ || !derivations_ || id <= 0) return;
  const auto key = repository_->recordVersion(id).key;
  const auto previous = derivationVersions_.constFind(id);
  const bool superseded = previous != derivationVersions_.constEnd() && !(previous.value() == key);
  derivationVersions_.insert(id, key);
  if (superseded) {
    QPointer<AssetAnalysisController> alive(this);
    derivations_->invalidateRecord(key);
    if (!alive || closing_ || !derivations_) return;
  }
  derivationBacklog_.insert(id);
  if (repository_->rawRecordResident(id)) readyRawDerivations_.insert(id);
  if (!derivationPump_->isActive()) derivationPump_->start(0);
}

void AssetAnalysisController::acceptDerivedFacts(const PetDerivationKey& key, const PetDerivedFactsHandle& facts) {
  if (closing_ || !facts || !derivations_ || key.record.account != account_ ||
      key.record.epoch != repository_->sessionGeneration() || !(key == derivationKey(key.record.instanceId))) return;
  repository_->markRecordDerived(key.record);
  if (preparingFacts_ && preparationIds_.contains(key.record.instanceId)) {
    // A background summary-only calculation can finish while the manual job
    // is still checking the original file. It must not satisfy that pending
    // read or make the manual capture publish Unknown prematurely.
    if (repository_->recordVersion(key.record.instanceId).complete || localReadChecked_.contains(key.record.instanceId)) {
      preparedHandles_.insert(key.record.instanceId, facts);
      awaitingFacts_.remove(key.record.instanceId);
    }
  }
  QPointer<AssetAnalysisController> alive(this);
  emit derivedFactsChanged(key.record.instanceId, facts);
  if (!alive) return;
  if (preparingFacts_ && !derivationPump_->isActive()) derivationPump_->start(0);
}

void AssetAnalysisController::failPreparation(const QString& reason) {
  if (!preparingFacts_) return;
  AnalysisJobFinished finished;
  finished.key = currentJob_; finished.outcome = AnalysisJobOutcome::InputRejected;
  finished.phase = AnalysisJobPhase::Preparation; finished.error = reason;
  preparingFacts_ = false; preparationStarted_ = false; analysisQueued_ = false;
  preparationIds_.clear(); awaitingFacts_.clear(); preparedHandles_.clear();
  QPointer<AssetAnalysisController> alive(this);
  emit analysisRunningChanged(false);
  if (alive) emit statusChanged(reason);
  if (alive) emit analysisJobFinished(finished);
}

void AssetAnalysisController::refreshPreparationMembers() {
  const auto ids = currentIds();
  QSet<qint64> next(ids.begin(), ids.end());
  for (auto id : preparationIds_ - next) { awaitingFacts_.remove(id); preparedHandles_.remove(id); }
  for (auto id : next) {
    const auto existing = preparedHandles_.value(id);
    if (!existing || !(existing->key == derivationKey(id)) ||
        (!repository_->recordVersion(id).complete && !localReadChecked_.contains(id))) {
      preparedHandles_.remove(id); awaitingFacts_.insert(id); derivationBacklog_.insert(id);
      if (repository_->rawRecordResident(id)) readyRawDerivations_.insert(id);
    }
  }
  preparationIds_ = std::move(next);
}

void AssetAnalysisController::pumpDerivations() {
  if (closing_ || !derivations_ || !repository_) return;
  // No new manual capture/retainer set until an old Worker releases its input.
  if (preparingFacts_ && worker_.busy()) { derivationPump_->start(5); return; }
  if (preparingFacts_ && !preparationStarted_) {
    preparationStarted_ = true; preparationStartedAt_ = transportMonotonicMs();
    refreshPreparationMembers();
  }
  if (preparingFacts_ && transportMonotonicMs() - preparationStartedAt_ > 120000) {
    failPreparation(QStringLiteral("本地详情准备超时，已保留上次分析；请检查缓存读取与后台任务状态"));
    return;
  }
  QElapsedTimer slice; slice.start();
  int units = 0;
  while (!derivationBacklog_.isEmpty() && units++ < 32 && slice.elapsed() < 8) {
    // Loaded originals must get their derivation before trying more disk
    // admissions. Otherwise a full read queue can repeatedly pick the same
    // blocked ID and starve the record whose underived pin blocks that queue.
    const qint64 id = readyRawDerivations_.isEmpty() ? *derivationBacklog_.begin() : *readyRawDerivations_.begin();
    readyRawDerivations_.remove(id); derivationBacklog_.remove(id);
    const auto version = repository_->recordVersion(id);
    if (!version.valid() || version.key.account != account_ || version.key.epoch != repository_->sessionGeneration()) continue;
    const bool needed = preparingFacts_ && preparationIds_.contains(id);
    if (needed && !version.complete && !localReadChecked_.contains(id)) {
      if (!localReadPending_.contains(id)) {
        localReadPending_.insert(id, version.key);
        if (!repository_->requestCachedDetail(id)) {
          localReadPending_.remove(id);
          if (!repository_->storageService() || !repository_->storageContext()) {
            failPreparation(QStringLiteral("本地详情读取上下文不可用，已保留上次分析")); return;
          }
          // Rejected admission says nothing about file existence/completeness.
          // Keep this ID awaited until an actual read terminal result arrives.
          ++preparationReadAdmissionRetries_;
          derivationBacklog_.insert(id); derivationPump_->start(5); break;
        }
      }
      continue;
    }
    const auto key = derivationKey(id);
    const auto raw = repository_->rawRecordHandle(id);
    if (!raw) {
      if (const auto found = derivations_->lookup(key)) { acceptDerivedFacts(key, found); continue; }
      if (!localReadPending_.contains(id)) {
        localReadPending_.insert(id, version.key);
        if (!repository_->requestCachedDetail(id)) {
          localReadPending_.remove(id);
          if (!repository_->storageService() || !repository_->storageContext()) {
            if (needed) { failPreparation(QStringLiteral("实例 %1 的详情读取上下文不可用，已保留上次分析").arg(id)); return; }
            continue;
          }
          ++preparationReadAdmissionRetries_;
          derivationBacklog_.insert(id); derivationPump_->start(5); break;
        }
      }
      continue;
    }
    const auto metadata = petMetadataSnapshot();
    if (!metadata) {
      if (needed) { failPreparation(QStringLiteral("精灵元数据快照不可用，已保留上次分析")); return; }
      continue;
    }
    auto brief = repository_->briefFor(id);
    if (brief.isEmpty()) brief = raw->brief;
    PetDerivationRequest request;
    request.raw = raw;
    request.seed = AssetAnalyzer::summarySeed(brief, raw->complete, raw->sourceKnown, PetMetadataView(metadata));
    const PetMetadataView view(metadata);
    request.metadata = {view.stargodDefinitions(), view.astrolabeDefinitions(), view.petDefinitions(),
        view.sacredStarPlans(), view.sacredStagePlans(), view.badgeDefinitions()};
    request.metadataRevision = metadata->revision; request.metadataDigest = metadata->contentDigest;
    request.storageContext = repository_->storageContext();
    // Route resident hits through request too: an IO Saved completion may have
    // supplied the durable digest since this same memory version was derived.
    // CacheHit binds/persists the disposable index without another calculation.
    const auto accepted = derivations_->request(request);
    if (accepted.facts) acceptDerivedFacts(key, accepted.facts);
    else if (!accepted.accepted) {
      if (accepted.status == PetDerivationStatus::QueueFull || accepted.status == PetDerivationStatus::ComputeUnavailable) {
        queueDerivation(id); derivationPump_->start(5); break;
      }
      emit derivedFactsFailed(key,accepted.error);
      if (needed) { failPreparation(QStringLiteral("实例 %1 派生准备失败：%2").arg(id).arg(accepted.error)); return; }
    }
  }
  if (preparingFacts_ && awaitingFacts_.isEmpty()) {
    // Reconcile membership/record changes once at the freeze boundary. Cache
    // reads may legitimately have taught Core new facts during preparation.
    refreshPreparationMembers();
    if (awaitingFacts_.isEmpty()) {
      checkInputFreshness(); currentJob_.versions = versions();
      preparingFacts_ = false; preparationStarted_ = false;
      worker_.submit(currentJob_);
    }
  }
  if ((!derivationBacklog_.isEmpty() || preparingFacts_) && !derivationPump_->isActive()) derivationPump_->start(5);
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

AssetAnalysisController::~AssetAnalysisController() { shutdownAnalysis(2000); }

void AssetAnalysisController::setCompatibilityIdentity(const QString& build, const QString& profile, bool verified) {
  const bool accepted = verified && !build.isEmpty() && !profile.isEmpty();
  if (buildIdentity_ == build && profileIdentity_ == profile && compatibilityVerified_ == accepted) return;
  buildIdentity_ = build; profileIdentity_ = profile; compatibilityVerified_ = accepted;
  ++trustRevision_;
  auto& state = currentState();
  if (state.hasAnalysis) { state.inventoryStale = true; state.shopStale = true; }
  emit shopAnalysisInvalidated();
}

AnalysisVersionStamp AssetAnalysisController::versions() const {
  AnalysisVersionStamp stamp;
  stamp.inventory = repository_ ? repository_->inventoryRevision() : 0;
  stamp.details = detailRevision_;
  const auto catalog = shopCatalogSnapshot();
  const auto metadata = petMetadataSnapshot();
  stamp.catalog = catalog ? catalog->revision : 0;
  stamp.resources = shopRevision_; stamp.limits = shopRevision_;
  stamp.routines = routineRevision_;
  stamp.routineCatalog = RoutineOverviewCatalog::instance().snapshot()->revision;
  stamp.catalogDay = static_cast<quint64>(businessDate().toJulianDay());
  stamp.trust = trustRevision_;
  stamp.metadata = metadata ? metadata->revision : 0;
  stamp.quotaFreshness = shopController_ ? shopController_->freshnessRevision() : 0;
  stamp.routineFreshness = routineController_ ? routineController_->freshnessRevision() : 0;
  stamp.build = buildIdentity_; stamp.profile = profileIdentity_;
  return stamp;
}

void AssetAnalysisController::metadataChanged() {
  if (closing_) return;
  ++trustRevision_;
  auto& state = currentState();
  if (state.hasAnalysis) { state.inventoryStale = true; state.shopStale = true; }
  if (derivations_) {
    const auto metadata = petMetadataSnapshot();
    derivations_->invalidateMetadata(metadata ? metadata->revision : 0, metadata ? metadata->contentDigest : QByteArray{});
    preparedHandles_.clear();
    if (preparingFacts_) awaitingFacts_ = preparationIds_;
    for (auto id : currentIds()) queueDerivation(id);
  }
  emit inventoryMembershipChanged();
  emit shopAnalysisInvalidated();
}

void AssetAnalysisController::checkInputFreshness() {
  if (shopController_) shopController_->checkFreshness();
  if (routineController_) routineController_->checkFreshness();
}

AnalysisJobKey AssetAnalysisController::nextJobKey() {
  return {++nextJobId_, account_, repository_ ? repository_->sessionGeneration() : 0, versions()};
}

bool AssetAnalysisController::jobCurrent(const AnalysisJobKey& key) const {
  return !closing_ && analysisQueued_ && repository_ && key.jobId == currentJob_.jobId &&
      key.account == account_ && key.account == repository_->accountKey() &&
      key.epoch == repository_->sessionGeneration();
}

std::shared_ptr<const AnalysisWorkInput> AssetAnalysisController::capture(const AnalysisJobKey& key) {
  checkInputFreshness();
  if (!jobCurrent(key) || !(key.versions == versions())) return {};
  const auto petMetadata = petMetadataSnapshot();
  const auto catalog = shopCatalogSnapshot();
  const auto date = businessDate();
  if (!petMetadata || !catalog || !date.isValid()) return {};
  auto input = std::make_shared<AnalysisWorkInput>();
  input->key = key; input->deriveOverview = !derivations_;
  if (derivations_) {
    input->usePreparedFacts = true;
    input->overview = analyzer_.routineSummary();
    input->overview.account = account_;
    input->overview.inputSessionEpoch = repository_->sessionGeneration();
    input->overview.inventoryRevision = repository_->inventoryRevision();
    input->overview.inventoryUpdatedAt = repository_->updatedAt();
    input->overview.sourceVerified = repository_->sessionContext().canPersist();
    input->overview.shopDataKnown = shopController_ && shopController_->hasObservedPacket();
    const auto ids = currentIds();
    input->overview.totalPets = ids.size(); input->overview.pets.clear();
    input->preparedFacts.reserve(ids.size());
    const PetMetadataView metadata(petMetadata);
    for (auto id : ids) {
      const auto expected = derivationKey(id);
      auto handle = preparedHandles_.value(id);
      if (!handle || !(handle->key == expected)) handle = derivations_->lookup(expected);
      if (!handle) return {};
      auto facts = handle->facts;
      const auto version = repository_->recordVersion(id);
      const auto seed = AssetAnalyzer::summarySeed(repository_->briefFor(id), version.complete, version.sourceKnown, metadata);
      // These display fields are deliberately independent of cultivation cache
      // keys. Moves/renames must not require parsing the original star inventory.
      facts.asset.name = seed.name; facts.asset.location = seed.location; facts.asset.pet = seed.pet;
      facts.asset.observationVerified = facts.asset.observationVerified && seed.observationVerified;
      input->overview.sourceVerified &= facts.asset.observationVerified;
      input->preparedFacts.append(std::move(facts));
    }
  } else input->overview = analyzer_.captureInput(petMetadata);
  input->overview.sourceVerified = input->overview.sourceVerified && compatibilityVerified_;
  input->catalogSnapshot = catalog;
  input->catalogDate = date;
  input->materialDefinitions = PetMetadataView(petMetadata).materialDefinitions();
  const bool trusted = input->overview.sourceVerified;
  const auto source = repository_->sessionContext().source.evidenceReference;
  input->resourceCounts = shopController_ ? shopController_->materialCounts() : QHash<QString, qint64>{};
  input->resourceCountsKnown = trusted && shopController_ && shopController_->hasMaterialCounts();
  input->sourceInvalidated = !trusted;
  ShopConditionContext context;
  context.shopPacketKnown = shopController_ && shopController_->hasPacket();
  context.shopSource = source; context.petSource = source;
  context.petObservedAt = input->overview.inventoryUpdatedAt;
  // Frozen local details are valid cultivation input even when they do not
  // authorize game writes. Version admission below handles stale detail data.
  context.petFreshness = ShopConditionFreshness::Current;
  context.shopFreshness = trusted ? ShopConditionFreshness::Current : ShopConditionFreshness::Invalidated;
  if (shopController_) context.quotaValidity = shopController_->quotaValiditySnapshot();
  input->conditionContext = context;
  input->shopPacket = shopController_ ? shopController_->packet() : QJsonObject{};
  const PetMetadataView metadata(petMetadata);
  input->metadata = {metadata.stargodDefinitions(), metadata.astrolabeDefinitions(), metadata.petDefinitions(),
      metadata.sacredStarPlans(), metadata.sacredStagePlans(), metadata.badgeDefinitions()};
  // A GUI catalog update can occur during capture; never label mixed input as
  // the previous version. Only the descriptor is retried, not a calculation.
  if (!(key.versions == versions())) return {};
  preparedHandles_.clear(); preparationIds_.clear(); awaitingFacts_.clear();
  ++analysisRunCount_;
  emit analysisInputCaptured(key.jobId);
  return input;
}

void AssetAnalysisController::trimAccountStates() {
  accountRecency_.removeAll(account_); accountRecency_.append(account_);
  while (accountRecency_.size() > 2) analysisStates_.remove(accountRecency_.takeFirst());
}

void AssetAnalysisController::publishResult(std::shared_ptr<const AnalysisWorkResult> result) {
  checkInputFreshness();
  if (!result || !jobCurrent(result->key)) return;
  const auto now = versions();
  const bool sameVersions = result->key.versions == now;
  const bool trusted = result->overview.sourceVerified && compatibilityVerified_ && repository_->sessionContext().canPersist();
  auto& state = currentState();
  state.lastValidOverview = result->overview;
  state.lastValidOverview.inputStale = !sameVersions;
  state.lastValidRecommendations = result->recommendations;
  state.retainedResult = result;
  state.analyzedSignature = {};
  state.analyzedSignature.account = result->key.account;
  for (const auto& pet : result->overview.pets) state.analyzedSignature.locations.insert(pet.instanceId, pet.location);
  state.lastAnalysisAt = QDateTime::currentDateTime();
  if (result->key.versions.details == now.details) state.dirtyPetIds.clear();
  state.hasAnalysis = true;
  state.inventoryStale = result->key.versions.inventory != now.inventory ||
      result->key.versions.details != now.details || result->key.versions.metadata != now.metadata ||
      result->key.versions.analysis != now.analysis;
  state.shopStale = !trusted || result->key.versions.catalog != now.catalog || result->key.versions.catalogDay != now.catalogDay ||
      result->key.versions.resources != now.resources || result->key.versions.limits != now.limits ||
      result->key.versions.quotaFreshness != now.quotaFreshness ||
      result->key.versions.trust != now.trust;
  trimAccountStates();
  // Snapshot admission occurs before public completion callbacks can start a
  // different operation. Saved is reported exclusively by Storage completion.
  if (sameVersions && trusted && settings_.known(account_) && !settings_.pending(account_) &&
      settings_.loadAutoSnapshot(account_)) snapshotStore_.write(account_, result->overview);
  emit analysisCompleted();
}

void AssetAnalysisController::finishJob(const AnalysisJobFinished& finished) {
  if (!jobCurrent(finished.key)) { emit analysisJobFinished(finished); return; }
  if (finished.outcome == AnalysisJobOutcome::InputRejected && !(finished.key.versions == versions())) {
    currentJob_ = nextJobKey();
    if (derivations_) { preparingFacts_ = true; preparationStarted_ = false; derivationPump_->start(0); }
    else worker_.submit(currentJob_);
    emit analysisJobFinished(finished);
    return;
  }
  analysisQueued_ = false;
  preparingFacts_ = false; preparationStarted_ = false; preparedHandles_.clear();
  QPointer<AssetAnalysisController> alive(this);
  emit analysisRunningChanged(false);
  if (!alive) return;
  if (!closing_ && currentJob_.jobId == finished.key.jobId && !analysisQueued_ &&
      finished.outcome != AnalysisJobOutcome::Published && finished.outcome != AnalysisJobOutcome::Cancelled)
    emit statusChanged(QStringLiteral("分析未完成（状态 %1），已保留上次结果").arg(static_cast<int>(finished.outcome)));
  if (alive) emit analysisJobFinished(finished);
}

void AssetAnalysisController::requestAnalysis() {
  if (closing_ || !repository_) return;
  checkInputFreshness();
  const bool wasRunning = analysisQueued_;
  AnalysisJobFinished superseded;
  const bool reportSuperseded = wasRunning && (preparingFacts_ || !worker_.busy());
  if (reportSuperseded) {
    superseded.key = currentJob_; superseded.outcome = AnalysisJobOutcome::Superseded;
    superseded.phase = preparingFacts_ ? AnalysisJobPhase::Preparation : AnalysisJobPhase::Queued;
  }
  currentJob_ = nextJobKey(); analysisQueued_ = true;
  if (derivations_) {
    worker_.cancel();
    preparingFacts_ = true; preparationStarted_ = false;
    preparationIds_.clear(); awaitingFacts_.clear(); preparedHandles_.clear(); localReadChecked_.clear();
    derivationPump_->start(0);
  } else worker_.submit(currentJob_);
  QPointer<AssetAnalysisController> alive(this);
  if (!wasRunning) emit analysisRunningChanged(true);
  if (alive && reportSuperseded) emit analysisJobFinished(superseded);
}

void AssetAnalysisController::cancelAnalysis() {
  if (!analysisQueued_) return;
  AnalysisJobFinished finished;
  finished.key = currentJob_; finished.outcome = AnalysisJobOutcome::Cancelled;
  finished.phase = preparingFacts_ ? AnalysisJobPhase::Preparation : AnalysisJobPhase::Queued;
  const bool noComputeForThisRequest = preparingFacts_ || !worker_.busy();
  analysisQueued_ = false; ++nextJobId_; currentJob_ = {};
  preparingFacts_ = false; preparationStarted_ = false;
  preparationIds_.clear(); awaitingFacts_.clear(); preparedHandles_.clear();
  worker_.cancel();
  QPointer<AssetAnalysisController> alive(this);
  emit analysisRunningChanged(false);
  // Active Compute supplies its own real cancellation latency through finishJob.
  if (alive && noComputeForThisRequest) emit analysisJobFinished(finished);
}

bool AssetAnalysisController::shutdownAnalysis(int maximumWaitMilliseconds) {
  closing_ = true; analysisQueued_ = false; currentJob_ = {};
  preparingFacts_ = false; preparedHandles_.clear(); derivationBacklog_.clear();
  readyRawDerivations_.clear();
  if (derivationPump_) derivationPump_->stop();
  if (derivations_) derivations_->shutdown();
  return worker_.shutdown(maximumWaitMilliseconds);
}

AccountAssetOverview AssetAnalysisController::overview() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  return state && state->hasAnalysis ? state->lastValidOverview
                                     : AccountAssetOverview{};
}

AccountAssetOverview AssetAnalysisController::routineSummary() const {
  return analyzer_.routineSummary();
}

QList<ActionRecommendation> AssetAnalysisController::recommendations() const {
  const AccountAnalysisState* state = currentStateIfPresent();
  if (!state || !state->hasAnalysis) return {};
  const bool cultivationStale = state->inventoryStale ||
                                !state->dirtyPetIds.isEmpty();
  bool unavailable = false;
  auto result = worker_.freshnessProjection(state->lastValidRecommendations,
      cultivationStale, state->shopStale, &unavailable);
  auto* self = const_cast<AssetAnalysisController*>(this);
  constexpr auto propertyName = "recommendationProjectionUnavailable";
  if (self->property(propertyName).toBool() != unavailable) {
    const QPointer<AssetAnalysisController> alive(self);
    const auto account = account_;
    const auto epoch = repository_->sessionGeneration();
    self->setProperty(propertyName, unavailable);
    if (alive) QMetaObject::invokeMethod(self, [alive, unavailable, account, epoch] {
      if (!alive || alive->account_ != account || alive->repository_->sessionGeneration() != epoch ||
          alive->property("recommendationProjectionUnavailable").toBool() != unavailable) return;
      emit alive->statusChanged(!unavailable ? QStringLiteral("建议显示已恢复")
          : alive->analysisRunning() ? QStringLiteral("建议显示暂不可用，正在计算；上次分析已保留")
                                     : QStringLiteral("建议显示达到内存上限，上次分析已保留"));
    }, Qt::QueuedConnection);
  }
  return result;
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
  const bool wasRunning = analysisQueued_;
  analysisQueued_ = false; currentJob_ = {}; worker_.cancel();
  account_ = account;
  preparingFacts_ = false; preparationStarted_ = false;
  preparedHandles_.clear(); preparationIds_.clear(); awaitingFacts_.clear(); derivationBacklog_.clear();
  readyRawDerivations_.clear();
  localReadChecked_.clear(); localReadPending_.clear();
  derivationVersions_.clear();
  if (derivations_) derivations_->bindSession(account_, repository_->sessionGeneration());
  snapshotHistoryError_.clear();
  instanceHistoryError_.clear();
  instanceHistoryId_ = 0;
  ++detailRevision_; ++shopRevision_; ++routineRevision_; ++trustRevision_;
  inventorySignature_ = analyzer_.inventorySignature();
  auto& state = currentState();
  if (state.hasAnalysis) { state.inventoryStale = true; state.shopStale = true; }
  trimAccountStates();
  autoSnapshotEnabled_ = false;
  settings_.requestLoad(account_); snapshotStore_.requestHistory(account_);
  QPointer<AssetAnalysisController> alive(this);
  setProperty("recommendationProjectionUnavailable", false);
  if (!alive) return;
  if (wasRunning) emit analysisRunningChanged(false);
  if (!alive) return;
  emit accountAnalysisChanged();
  if (!alive) return;
  emit inventoryCountsChanged();
  if (!alive) return;
  emit routineSummaryChanged();
  if (!alive) return;
  emit historyChanged();
  if (!alive) return;
  emit historyStateChanged();
  if (!alive) return;
  emit instanceHistoryChanged();
}

void AssetAnalysisController::handleInventoryChanged() {
  if (!repository_ || repository_->accountKey() != account_) return;
  const InventorySignature next = analyzer_.inventorySignature();
  const bool membershipChanged = next != inventorySignature_;
  inventorySignature_ = next;
  if (derivations_ && membershipChanged) {
    for (auto id : currentIds()) queueDerivation(id);
    if (preparingFacts_ && preparationStarted_) refreshPreparationMembers();
  }
  if (membershipChanged) {
    AccountAnalysisState& state = currentState();
    if (state.hasAnalysis) state.inventoryStale = true;
  }
  const QString expectedAccount = account_;
  const quint64 expectedEpoch = repository_->sessionGeneration();
  QPointer<AssetAnalysisController> alive(this);
  emit inventoryCountsChanged();
  if (!alive || !membershipChanged || account_ != expectedAccount ||
      repository_->sessionGeneration() != expectedEpoch) return;
  emit inventoryMembershipChanged();
}

void AssetAnalysisController::handlePetDetailChanged(qint64 instanceId) {
  if (instanceId <= 0 || !repository_ || repository_->accountKey() != account_) return;
  ++detailRevision_;
  if (derivations_) {
    queueDerivation(instanceId);
    if (preparingFacts_ && preparationIds_.contains(instanceId)) {
      const auto found = preparedHandles_.value(instanceId);
      if (!found || !(found->key == derivationKey(instanceId))) {
        preparedHandles_.remove(instanceId); awaitingFacts_.insert(instanceId);
      }
    }
  }
  AccountAnalysisState& state = currentState();
  if (state.dirtyPetIds.contains(instanceId)) return;
  state.dirtyPetIds.insert(instanceId);
  emit petDetailChanged(instanceId);
}

void AssetAnalysisController::handleShopAnalysisInvalidated() {
  if (!repository_ || repository_->accountKey() != account_) return;
  ++shopRevision_;
  AccountAnalysisState& state = currentState();
  if (state.hasAnalysis) state.shopStale = true;
  emit shopAnalysisInvalidated();
}

void AssetAnalysisController::handleRoutineSummaryChanged() {
  if (!repository_ || repository_->accountKey() != account_) return;
  ++routineRevision_;
  AccountAnalysisState& state = currentState();
  if (state.hasAnalysis)
    analyzer_.updateRoutineSummary(&state.lastValidOverview);
  emit routineSummaryChanged();
}

void AssetAnalysisController::setAutoSnapshotEnabled(bool enabled) {
  if (closing_ || !repository_) return;
  const auto account = account_;
  const auto epoch = repository_->sessionGeneration();
  const auto submission = settings_.saveAutoSnapshot(account_, enabled);
  emit persistenceChanged(account, epoch, QStringLiteral("asset-analysis.json"), submission.taskId,
                           submission.status, submission.error);
  if (!submission.accepted) emit statusChanged(QStringLiteral("快照设置未进入保存队列"));
}

bool AssetAnalysisController::recordSnapshot() {
  if (closing_ || !repository_ || account_.isEmpty()) return false;
  checkInputFreshness();
  const AccountAnalysisState* state = currentStateIfPresent();
  if (!state || !state->hasAnalysis) {
    emit statusChanged(QStringLiteral(
        "尚未分析，请先点击“重新计算养成分析（仅本地）”"));
    return false;
  }
  if (state->inventoryStale || state->shopStale || !state->dirtyPetIds.isEmpty()) {
    emit statusChanged(QStringLiteral(
        "资产或详情已有变化，请先重新计算养成分析后再记录快照"));
    return false;
  }
  return recordSnapshotFromOverview(state->lastValidOverview);
}

bool AssetAnalysisController::recordSnapshotFromOverview(
    const AccountAssetOverview& current) {
  if (closing_ || !compatibilityVerified_ || current.inputStale) return false;
  QString status;
  const auto submission = snapshotStore_.write(account_, current, &status);
  if (!status.isEmpty()) emit statusChanged(status);
  return submission.accepted;
}

void AssetAnalysisController::requestSnapshotHistory(const QDateTime& before) {
  if (closing_ || !repository_) return;
  snapshotStore_.requestHistory(account_, before);
}

void AssetAnalysisController::requestSnapshotDetails(const QString& key) {
  if (closing_ || !repository_) return;
  // The store accepts only a key issued by the current summary index.
  snapshotStore_.requestSnapshotDetails(account_, key);
}

void AssetAnalysisController::requestInstanceHistory(qint64 instanceId) {
  if (closing_ || !repository_ || instanceId <= 0) return;
  if (instanceHistoryId_ == instanceId &&
      (snapshotStore_.instanceHistoryLoading() || !snapshotStore_.cachedInstanceHistory(account_, instanceId).isEmpty())) return;
  instanceHistoryId_ = instanceId;
  instanceHistoryError_.clear();
  snapshotStore_.requestInstanceHistory(account_, instanceId);
}

QList<AccountAssetSnapshot> AssetAnalysisController::snapshots() const {
  return snapshotStore_.cachedHistory(account_);
}

AssetSnapshotDelta AssetAnalysisController::compareSnapshots(
    const AccountAssetSnapshot& current,
    const AccountAssetSnapshot& previous) {
  return AssetSnapshotComparator::compare(current, previous);
}
