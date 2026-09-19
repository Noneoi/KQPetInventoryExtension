#include "inventory_publisher.h"
#include "inventory_projection.h"
#include "application/pet/pet_repository.h"
#include "application/catalog/pet_detail_catalog.h"
#include "application/pet/pet_detail_preparation_service.h"
#include "application/pet/pet_derivation_cache.h"
#include <QFileInfo>
#include <QTimer>
#include <QThread>
#include <atomic>
#include <algorithm>

InventoryPublisher::InventoryPublisher(PetRepository* repository, InventoryProjection* projection,
                                       QObject* parent)
    : QObject(parent), repository_(repository), projection_(projection) {
  metadata_ = PetDetailCatalog::instance().snapshot();
  connect(repository_, &PetRepository::dataChanged, this, [this] {
    requestPublication();
    if (detailService_) detailDependenciesChanged(QSet<qint64>{});
  });
  connect(repository_, &PetRepository::detailChanged, this, [this](qint64 id) {
    requestPublication(false, id); detailDependenciesChanged({id});
  });
  connect(repository_, &PetRepository::accountSessionChanged, this, [this] {
    facts_.clear(); factFailures_.clear(); detailInterests_.clear(); detailSelections_.clear(); detailRecordVersions_.clear(); detailRequests_.clear(); waitingDetails_.clear();
    requestedDetailReads_.clear(); preparedDetails_.clear(); detailErrors_.clear(); summaryStamps_.clear();
    if (detailService_) detailService_->bindSession(repository_->accountKey(), repository_->sessionGeneration());
    requestPublication();
  });
  connect(repository_, &PetRepository::sessionTrustChanged, this, [this] {
    preparedDetails_.clear();
    if (detailService_) {
      QSet<qint64> selected;
      for (const auto& value : detailSelections_) selected.insert(value.instanceId);
      detailService_->summariesChanged(selected);
    }
    requestPublication();
  });
  connect(repository_, &PetRepository::rawRecordAvailable, this, [this](const RawPetRecordHandle& raw) {
    if (raw && raw->key.account == repository_->accountKey() && raw->key.epoch == repository_->sessionGeneration()) {
      requestPublication(false, raw->key.instanceId);
      const auto changed = changedSelectedRecords();
      if (detailService_ && !changed.isEmpty()) detailService_->summariesChanged(changed);
      provideWaitingDetails();
    }
  });
  connect(repository_, &PetRepository::rawRecordLoadFinished, this,
      [this](const PetRecordKey& key, bool loaded, const QString& error, StorageStatus status) {
    if (loaded || key.account != repository_->accountKey() || key.epoch != repository_->sessionGeneration()) return;
    QList<quint64> rejected;
    for (auto it = waitingDetails_.begin(); it != waitingDetails_.end(); ++it) {
      if (detailSelections_.value(it.value()).instanceId != key.instanceId) continue;
      detailErrors_[it.value()] = error;
      if (status == StorageStatus::QueueFull || status == StorageStatus::Superseded || status == StorageStatus::Cancelled)
        requestedDetailReads_.remove(it.key());
      else rejected.append(it.key());
    }
    requestPublication(false, key.instanceId);
    const QPointer<InventoryPublisher> alive(this);
    for (auto request : rejected) {
      waitingDetails_.remove(request); requestedDetailReads_.remove(request);
      if (detailService_) detailService_->rejectInputs(request, error);
      if (!alive) return;
    }
    if (status == StorageStatus::QueueFull || status == StorageStatus::Superseded || status == StorageStatus::Cancelled)
      QTimer::singleShot(50, this, &InventoryPublisher::provideWaitingDetails);
  });
  connect(repository_, &PetRepository::rawRecordEvicted, this,
      [this](const QString& account, quint64 epoch, qint64 id, quint64) {
    if (account == repository_->accountKey() && epoch == repository_->sessionGeneration()) requestPublication(false, id);
  });
  connect(repository_, &PetRepository::persistenceChanged, this,
      [this](const QString&, const QString& path, quint64, StorageStatus, const QString&) {
    const qint64 id = QFileInfo(path).completeBaseName().toLongLong();
    requestPublication(id <= 0, id);
  });
  connect(repository_, &PetRepository::statusChanged, projection_, &InventoryProjection::statusChanged,
          Qt::QueuedConnection);
  requestPublication();
}

void InventoryPublisher::metadataUpdated(std::shared_ptr<const PetDetailCatalogSnapshot> metadata) {
  Q_ASSERT(thread() == QThread::currentThread());
  if (!metadata || (metadata_ && metadata->revision < metadata_->revision) || metadata == metadata_) return;
  metadata_ = std::move(metadata);
  facts_.clear();
  factFailures_.clear();
  preparedDetails_.clear();
  if (detailService_) detailService_->invalidateMetadata(metadata_->revision, metadata_->contentDigest);
  // Rebuild the projection to drop old metadata leases as well as making old
  // keys unreadable. Outstanding GUI snapshots keep their own tracked leases.
  requestPublication();
}

void InventoryPublisher::factsUpdated(qint64 id, const PetDerivedFactsHandle& facts) {
  Q_ASSERT(thread() == QThread::currentThread());
  if (!facts || facts->key.record.account != repository_->accountKey() ||
      facts->key.record.epoch != repository_->sessionGeneration() || !(facts->key.record == repository_->recordVersion(id).key) ||
      !metadata_ || facts->key.metadataRevision != metadata_->revision || facts->key.metadataDigest != metadata_->contentDigest ||
      repository_->briefFor(id).isEmpty()) return;
  if (facts_.value(id) == facts) return;
  factFailures_.remove(id);
  facts_.insert(id, facts); requestPublication(false, id);
  provideWaitingDetails();
}

void InventoryPublisher::factsFailed(const PetDerivationKey& key, const QString& error) {
  if (!metadata_ || key.record.account != repository_->accountKey() ||
      key.record.epoch != repository_->sessionGeneration() ||
      key.record != repository_->recordVersion(key.record.instanceId).key ||
      key.metadataRevision != metadata_->revision || key.metadataDigest != metadata_->contentDigest) return;
  factFailures_.insert(key.record.instanceId,{key,QStringLiteral("本地分析未完成：%1；请刷新详情或更新数据后重试").arg(error)});
  provideWaitingDetails();
}

quint64 InventoryPublisher::summaryRevision(qint64 id, const QJsonObject& brief) {
  const auto found = summaryStamps_.constFind(id);
  if (found != summaryStamps_.cend() && found->value == brief) return found->revision;
  if (summaryStamps_.size() >= 2048) summaryStamps_.clear();
  const auto revision = ++nextSummaryRevision_;
  summaryStamps_.insert(id, {brief, revision}); return revision;
}
void InventoryPublisher::setDetailService(PetDetailPreparationService* service, PetDerivationCache* derivations) {
  detailService_ = service; derivations_ = derivations;
  if (!service) return;
  service->bindSession(repository_->accountKey(), repository_->sessionGeneration());
  connect(service, &PetDetailPreparationService::inputsNeeded, this, &InventoryPublisher::detailInputsNeeded);
  connect(service, &PetDetailPreparationService::relatedSummariesNeeded, this,
      [this](quint64 request, const QVector<qint64>& ids) {
    QVector<DetailRelatedSummary> values; values.reserve(ids.size());
    for (auto id : ids) {
      QJsonObject brief = repository_->briefFor(id);
      QJsonObject fields;
      for (auto it = brief.constBegin(); it != brief.constEnd(); ++it)
        // The roster location fields say whether this account holds the related
        // pet and where, which its own relation rows report to the reader.
        if (it.key().startsWith("_meta") ||
            QStringList{"id","r","ri","fr","n","customName","lv","zdl","xzdl","_location","_warehouseGroup"}.contains(it.key()))
          fields.insert(it.key(), it.value());
      values.append({id, brief.isEmpty() ? 0 : summaryRevision(id, brief), !brief.isEmpty(), fields});
    }
    if (detailService_) detailService_->provideRelatedSummaries(request, std::move(values));
  });
  connect(service, &PetDetailPreparationService::ready, this,
      [this](int consumer, quint64 request, const PreparedPetDetailHandle& value) {
    if (!value || detailRequests_.value(consumer) != request || !metadata_ || detailSelections_.value(consumer).instanceId != value->identity.instanceId ||
        !(value->version.facts.record == repository_->recordVersion(value->identity.instanceId).key) ||
        value->version.facts.metadataRevision != metadata_->revision || value->version.facts.metadataDigest != metadata_->contentDigest) return;
    preparedDetails_[consumer] = value; detailErrors_.remove(consumer);
    requestPublication(false, value->identity.instanceId);
  });
  connect(service, &PetDetailPreparationService::failed, this,
      [this](int consumer, quint64 request, DetailPreparationStatus status, const QString& error) {
    waitingDetails_.remove(request); requestedDetailReads_.remove(request);
    if (detailRequests_.value(consumer) != request) return;
    if (status == DetailPreparationStatus::Cancelled || status == DetailPreparationStatus::Superseded) return;
    detailErrors_[consumer] = error; requestPublication(false, detailSelections_.value(consumer).instanceId);
  });
}
void InventoryPublisher::selectDetail(int consumer, qint64 id) {
  if (!detailService_ || consumer < 0 || consumer >= kDetailConsumerCount) return;
  const DetailSelection selection{repository_->accountKey(), repository_->sessionGeneration(), id};
  if (id > 0 && detailSelections_.value(consumer) == selection && !detailErrors_.contains(consumer)) return;
  for (auto it = waitingDetails_.begin(); it != waitingDetails_.end();) {
    if (it.value() == consumer) { requestedDetailReads_.remove(it.key()); it = waitingDetails_.erase(it); }
    else ++it;
  }
  preparedDetails_.remove(consumer); detailErrors_.remove(consumer);
  if (id <= 0 || repository_->briefFor(id).isEmpty()) {
    detailSelections_.remove(consumer); detailRecordVersions_.remove(consumer); detailRequests_.remove(consumer);
    detailService_->release(consumer); requestPublication(false); return;
  }
  detailSelections_[consumer] = selection;
  detailRecordVersions_[consumer] = repository_->recordVersion(id).key;
  const auto submitted = detailService_->request(consumer, selection);
  detailRequests_[consumer] = submitted.requestId;
  if (!submitted.accepted) detailErrors_[consumer] = submitted.error;
  requestPublication(false, id);
}
void InventoryPublisher::requestDetailPage(int consumer, DetailSection section, int pageIndex) {
  if (detailService_ && detailSelections_.contains(consumer)) {
    const auto submitted = detailService_->requestPage(consumer, section, pageIndex);
    if (submitted.accepted) detailRequests_[consumer] = submitted.requestId;
    else detailErrors_[consumer] = submitted.error;
  }
}
void InventoryPublisher::detailInputsNeeded(quint64 request, int consumer, const DetailSelection& selection) {
  if (!detailService_ || !(detailSelections_.value(consumer) == selection)) return;
  detailRequests_[consumer] = request;
  for (auto it = waitingDetails_.begin(); it != waitingDetails_.end();) {
    if (it.value() == consumer) { requestedDetailReads_.remove(it.key()); it = waitingDetails_.erase(it); }
    else ++it;
  }
  waitingDetails_[request] = consumer; provideWaitingDetails();
}
void InventoryPublisher::provideWaitingDetails() {
  if (!detailService_ || !metadata_) return;
  const auto waiting = waitingDetails_;
  for (auto it = waiting.cbegin(); it != waiting.cend(); ++it) {
    const auto selection = detailSelections_.value(it.value());
    if (selection.account != repository_->accountKey() || selection.epoch != repository_->sessionGeneration()) continue;
    const auto raw = repository_->rawRecordHandle(selection.instanceId);
    if (!raw) {
      if (!requestedDetailReads_.contains(it.key())) {
        requestedDetailReads_.insert(it.key());
        if (!repository_->requestCachedDetail(selection.instanceId)) {
          requestedDetailReads_.remove(it.key()); QTimer::singleShot(50, this, &InventoryPublisher::provideWaitingDetails);
        }
      }
      continue;
    }
    const PetDerivationKey key{raw->key, metadata_->revision, metadata_->contentDigest, AssetAnalysisVersion::kCurrentAnalysis};
    auto facts = facts_.value(selection.instanceId);
    if ((!facts || !(facts->key == key)) && derivations_) facts = derivations_->lookup(key);
    if (!facts || !(facts->key == key)) {
      const auto failed = factFailures_.constFind(selection.instanceId);
      if (failed != factFailures_.cend() && failed->key == key) {
        const QString error = failed->error;
        waitingDetails_.remove(it.key()); requestedDetailReads_.remove(it.key());
        detailService_->rejectInputs(it.key(),error);
      }
      continue;
    }
    FrozenDetailInputs inputs;
    inputs.raw = raw; inputs.facts = facts; inputs.metadata = metadata_;
    inputs.brief = repository_->briefFor(selection.instanceId);
    inputs.summaryRevision = summaryRevision(selection.instanceId, inputs.brief);
    inputs.sourceVerified = repository_->sessionContext().canPersist();
    waitingDetails_.remove(it.key()); requestedDetailReads_.remove(it.key());
    detailService_->provideInputs(it.key(), std::move(inputs));
  }
}
QSet<qint64> InventoryPublisher::changedSelectedRecords() {
  QSet<qint64> changed;
  for (auto it = detailSelections_.cbegin(); it != detailSelections_.cend(); ++it) {
    const auto key = repository_->recordVersion(it->instanceId).key;
    if (!(detailRecordVersions_.value(it.key()) == key)) {
      detailRecordVersions_[it.key()] = key;
      changed.insert(it->instanceId);
    }
  }
  return changed;
}
void InventoryPublisher::detailDependenciesChanged(const QSet<qint64>& ids) {
  if (!detailService_) return;
  QSet<qint64> changed = ids | changedSelectedRecords();
  if (ids.isEmpty()) {
    for (auto it = summaryStamps_.cbegin(); it != summaryStamps_.cend(); ++it)
      if (repository_->briefFor(it.key()) != it->value) changed.insert(it.key());
    const auto selections = detailSelections_;
    for (auto it = selections.cbegin(); it != selections.cend(); ++it)
      if (repository_->briefFor(it->instanceId).isEmpty()) selectDetail(it.key(), 0);
  }
  if (!changed.isEmpty()) detailService_->summariesChanged(changed);
  provideWaitingDetails();
}

void InventoryPublisher::setDetailInterests(const QSet<qint64>& ids) {
  Q_ASSERT(thread() == QThread::currentThread());
  // One instance per detail consumer at most. A larger set means the GUI and
  // this publisher disagree about how many slots exist, so nothing is pinned.
  if (ids.size() > kDetailConsumerCount) return;
  const auto changed = detailInterests_ | ids;
  detailInterests_ = ids;
  for (auto id : changed) requestPublication(false, id);
  for (auto id : ids) repository_->requestCachedDetail(id);
}

void InventoryPublisher::requestPublication(bool inventoryChanged, qint64 detailId) {
  Q_ASSERT(thread() == QThread::currentThread());
  inventoryChanged_ = inventoryChanged_ || inventoryChanged;
  if (detailId > 0) detailsChanged_.insert(detailId);
  if (scheduled_) return;
  scheduled_ = true;
  QTimer::singleShot(0, this, &InventoryPublisher::publish);
}

void InventoryPublisher::publish() {
  Q_ASSERT(repository_->thread() == QThread::currentThread());
  static std::atomic<quint64> publications{0};
  const bool full = inventoryChanged_ || !last_ || last_->account != repository_->accountKey() ||
                    last_->sessionEpoch != repository_->sessionGeneration();
  auto snapshot = !full && last_ ? std::make_shared<InventoryViewSnapshot>(*last_)
                                : std::make_shared<InventoryViewSnapshot>();
  snapshot->publication = ++publications;
  snapshot->sessionEpoch = repository_->sessionGeneration();
  snapshot->inventoryRevision = repository_->inventoryRevision();
  snapshot->metadata = metadata_;
  snapshot->preparedDetails = preparedDetails_;
  snapshot->detailErrors = detailErrors_;
  snapshot->account = repository_->accountKey();
  snapshot->cachePath = repository_->cachePath();
  snapshot->dataRoot = repository_->dataRoot();
  snapshot->updatedAt = repository_->updatedAt();
  snapshot->authenticated = repository_->isAuthenticated();
  snapshot->sourceVerified = repository_->sessionContext().canPersist();
  snapshot->sessionState = repository_->sessionContext().state;
  snapshot->membershipChanged = full;
  snapshot->changedDetails = std::move(detailsChanged_);
  const auto collect = [this, &snapshot](qint64 id) {
    const auto version = repository_->recordVersion(id);
    if (version.valid()) snapshot->recordVersions.insert(id, version); else snapshot->recordVersions.remove(id);
    const auto facts = facts_.value(id);
    if (facts && facts->key.record == version.key && snapshot->metadata &&
        facts->key.metadataRevision == snapshot->metadata->revision && facts->key.metadataDigest == snapshot->metadata->contentDigest)
      snapshot->facts.insert(id, facts);
    else { snapshot->facts.remove(id); facts_.remove(id); }
    if (detailInterests_.contains(id)) {
      const auto raw = repository_->rawRecordHandle(id);
      if (raw) snapshot->rawDetails.insert(id, raw); else snapshot->rawDetails.remove(id);
    } else snapshot->rawDetails.remove(id);
    // Naked JSON maps are retained only by synthetic legacy projections.
    snapshot->details.remove(id);
    const QDateTime savedAt = repository_->detailSavedAt(id);
    if (savedAt.isValid()) snapshot->detailSavedTimes.insert(id, savedAt);
    else snapshot->detailSavedTimes.remove(id);
  };
  if (full) {
    snapshot->backpack = repository_->backpackBriefs();
    snapshot->warehouse = repository_->warehouseBriefs();
    backpackRows_.clear(); warehouseRows_.clear();
    for (qsizetype row = 0; row < snapshot->backpack.size(); ++row) {
      const qint64 id = snapshot->backpack.at(row).value(QStringLiteral("id")).toVariant().toLongLong();
      backpackRows_.insert(id, int(row)); collect(id);
    }
    for (qsizetype row = 0; row < snapshot->warehouse.size(); ++row) {
      const qint64 id = snapshot->warehouse.at(row).value(QStringLiteral("id")).toVariant().toLongLong();
      warehouseRows_.insert(id, int(row)); collect(id);
    }
    for (auto it = facts_.begin(); it != facts_.end();) {
      if (!backpackRows_.contains(it.key()) && !warehouseRows_.contains(it.key())) it = facts_.erase(it);
      else ++it;
    }
    for (auto it = detailInterests_.begin(); it != detailInterests_.end();) {
      if (!backpackRows_.contains(*it) && !warehouseRows_.contains(*it)) it = detailInterests_.erase(it);
      else ++it;
    }
  } else {
    // Details update only affected records. JSON trees and list ordering are not
    // reparsed/sorted per response; COW container copies are bounded per batch.
    for (qint64 id : snapshot->changedDetails) {
      collect(id);
      if (backpackRows_.contains(id)) snapshot->backpack[backpackRows_.value(id)] = repository_->briefFor(id);
      if (warehouseRows_.contains(id)) snapshot->warehouse[warehouseRows_.value(id)] = repository_->briefFor(id);
    }
  }
  scheduled_ = false;
  inventoryChanged_ = false;
  last_ = snapshot;
  projection_->publish(std::move(snapshot));
}
