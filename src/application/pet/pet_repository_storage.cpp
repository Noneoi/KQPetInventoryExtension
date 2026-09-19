// Account persistence: account activation, asynchronous cache reads,
// queued writes and storage results, and the one-time legacy cache migration.
// Part of the pet_repository implementation; see pet_repository.cpp for the rest.

#include "pet_repository.h"
#include "pet_repository_internal.h"

#include "diagnostics/diagnostic_logger.h"
#include "application/catalog/pet_detail_catalog.h"
#include "domain/pet_identity.h"
#include "protocol/packet_contract.h"
#include "storage/storage_service.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <climits>

using PetRepositoryInternal::copyPowerFields;

namespace {

QString safeAccountName(const QString& account) {
  static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]+$"));
  if (safe.match(account).hasMatch()) return account;
  return QString::fromLatin1(
             QCryptographicHash::hash(account.toUtf8(), QCryptographicHash::Sha256).toHex())
      .left(24);
}

}  // namespace

QString PetRepository::accountDirectory(const QString& account) const {
  return QDir(cacheRoot_).filePath(QStringLiteral("accounts/%1").arg(safeAccountName(account)));
}

void PetRepository::setAccountPaths() {
  const QString directory = accountDirectory(accountKey_);
  cachePath_ = QDir(directory).filePath(QStringLiteral("inventory.json"));
  detailsPath_ = QDir(directory).filePath(QStringLiteral("details"));
  storageContext_ = storage_ ? storage_->createAccountContext(accountKey_, safeAccountName(accountKey_)) : StorageContext{};
}

void PetRepository::clearExpectations() {
  backpackExpectation_ = {};
  warehouseExpectation_ = {};
  detailExpectation_ = {};
  sequenceExpectation_ = {};
}

void PetRepository::activateAccountSession(const QString& account) {
  if (account.isEmpty()) return;
  accountKey_ = account;
  ++sessionGeneration_;
  session_.account = account;
  session_.epoch = sessionGeneration_;
  const bool verified = session_.source.sameSource(currentEnvelope_.source) &&
                        session_.source.account == account;
  session_.state = verified ? SessionConnectionState::Active : SessionConnectionState::Uncertain;
  session_.uncertaintyReason = verified ? QString{} : QStringLiteral("仅观察到账号，连接来源尚未核实");
  // The login packet belongs to the epoch it establishes, after its capture
  // credential has already been checked against the preceding epoch.
  currentEnvelope_.capturedSessionEpoch = sessionGeneration_;
  authenticated_ = true;
  clearExpectations();
  formationPositions_.clear();
  formationPlans_.clear();
  deployedPetIds_.clear();
  currentFormationId_ = 0;
  formationKnown_ = false;
  backpackObservation_ = {};
  warehouseObservations_.clear();
  // Every new local epoch starts from historical disk data. This discards old
  // weak observations and raw-record epoch keys as well as old expectations;
  // a repeated weak login cannot promote them to the new reading round.
  loadAccount();
  writeLastAccount();
  emit dataChanged();
  emit accountSessionChanged(accountKey_, sessionGeneration_);
  emit sessionTrustChanged(session_.state, session_.uncertaintyReason);
  emit statusChanged((verified ? QStringLiteral("账号 %1 来源已核实，正在异步读取本地缓存：%2")
                                : QStringLiteral("已观察账号 %1，来源尚未核实；正在读取本地缓存：%2"))
                         .arg(accountKey_, cachePath_));
}

void PetRepository::loadDetails() {
  detailScanWanted_ = true;
  detailScanInFlight_ = false;
  detailScanCursor_.clear();
  cachePumpTimer_->start(0);
  publishCacheLoading();
}

void PetRepository::loadAccount() {
  if (storage_) {
    storage_->cancelReads(storageContext_);
    if (!detailScanCursor_.isEmpty()) storage_->cancelScan(detailScanCursor_);
  }
  ++cacheLoadGeneration_;
  ++inventoryRevision_;
  ++listRevision_;
  queuedReads_.erase(std::remove_if(queuedReads_.begin(), queuedReads_.end(), [](const PendingRead& pending) {
    return pending.kind == ReadKind::Inventory || pending.kind == ReadKind::Detail || pending.kind == ReadKind::Directory;
  }), queuedReads_.end());
  pendingDetailReads_.clear();
  queuedDetailReads_.clear();
  pendingDetailNames_.clear();
  detailScanCursor_.clear();
  detailScanWanted_ = false;
  detailScanInFlight_ = false;
  backpack_.clear();
  warehouse_.clear();
  rawRecords_->clearScope(); detailStates_.clear(); detailIdentities_.clear(); savedMemoryRevisions_.clear(); rawReadRetryAfter_ = 0;
  detailSavedTimes_.clear();
  detailRevisions_.clear();
  savedDetailRevisions_.clear();
  packSequences_.clear();
  packCapacities_.clear();
  formationPositions_.clear();
  formationPlans_.clear();
  deployedPetIds_.clear();
  currentFormationId_ = 0;
  formationKnown_ = false;
  clearExpectations();
  onlineData_ = false;
  updatedAt_ = {};
  setAccountPaths();
  PendingRead pending;
  pending.kind = ReadKind::Inventory;
  pending.context = storageContext_;
  pending.account = accountKey_;
  pending.epoch = sessionGeneration_;
  pending.loadGeneration = cacheLoadGeneration_;
  pending.inventoryRevision = listRevision_;
  pending.relativePath = QStringLiteral("inventory.json");
  pending.maximumBytes = 16 * 1024 * 1024;
  pending.selected = true;
  queueCacheRead(pending);
}

void PetRepository::loadCache() {
  PendingRead pending;
  pending.kind = ReadKind::LastAccount;
  pending.context = sharedStorageContext_;
  pending.account = accountKey_;
  pending.epoch = sessionGeneration_;
  pending.relativePath = QStringLiteral("last-account.txt");
  pending.maximumBytes = 4096;
  pending.selected = true;
  queueCacheRead(pending);
}

bool PetRepository::cacheReadCurrent(const PendingRead& pending) const {
  if (session_.state == SessionConnectionState::Closing) return false;
  if (pending.kind == ReadKind::Legacy || pending.kind == ReadKind::MigrationMarker) return true;
  if (pending.kind == ReadKind::LastAccount)
    return sessionGeneration_ == 0 && !authenticated_ && session_.state == SessionConnectionState::Disconnected;
  return pending.account == accountKey_ && pending.epoch == sessionGeneration_ &&
         pending.loadGeneration == cacheLoadGeneration_;
}

bool PetRepository::queueCacheRead(PendingRead pending) {
  if (!storage_ || !pending.context || session_.state == SessionConnectionState::Closing) return false;
  if (queuedReads_.size() >= 256) {
    publishCacheLoading(QStringLiteral("缓存读取队列已满，请稍后重试选中详情"));
    return false;
  }
  if (pending.selected) queuedReads_.push_front(std::move(pending));
  else queuedReads_.push_back(std::move(pending));
  cachePumpTimer_->start(0);
  publishCacheLoading();
  return true;
}

bool PetRepository::requestCachedDetail(qint64 instanceId) {
  if (instanceId <= 0 || !storage_ || !storageContext_) return false;
  const PetRecordKey requested{accountKey_, sessionGeneration_, instanceId, detailMemoryRevision(instanceId)};
  const auto currentVersion = recordVersion(instanceId);
  if (rawRecordResident(instanceId) && (currentVersion.complete || currentVersion.persisted)) {
    QTimer::singleShot(0, this, [this, requested] {
      if (requested.account == accountKey_ && requested.epoch == sessionGeneration_) {
        const bool unchanged = detailMemoryRevision(requested.instanceId) == requested.detailMemoryRevision;
        const bool available = unchanged && rawRecordResident(requested.instanceId);
        emit rawRecordLoadFinished(requested, available,
            available ? QString{} : QStringLiteral("读取确认前详情版本或驻留状态已变化"),
            available ? StorageStatus::Loaded : StorageStatus::Superseded);
      }
    });
    return true;
  }
  if (pendingDetailReads_.contains(instanceId)) {
    const auto task = pendingDetailReads_.value(instanceId);
    if (pendingReads_.value(task).detailMemoryRevision == requested.detailMemoryRevision) {
      // An explicit lookup keeps its intent even if this member leaves the
      // current roster while the already accepted read is in flight.
      pendingReads_[task].selected = true;
      storage_->prioritizeRead(task); return true;
    }
  }
  if (queuedDetailReads_.contains(instanceId)) {
    const auto found = std::find_if(queuedReads_.begin(), queuedReads_.end(), [instanceId](const PendingRead& pending) {
      return pending.kind == ReadKind::Detail && pending.instanceId == instanceId;
    });
    if (found != queuedReads_.end()) {
      PendingRead selected = *found;
      selected.selected = true;
      selected.detailMemoryRevision = requested.detailMemoryRevision;
      selected.recordRevision = detailRevisions_.value(instanceId);
      queuedReads_.erase(found);
      queuedReads_.push_front(std::move(selected));
    }
    return true;
  }
  PendingRead pending;
  pending.kind = ReadKind::Detail;
  pending.context = storageContext_;
  pending.account = accountKey_;
  pending.epoch = sessionGeneration_;
  pending.loadGeneration = cacheLoadGeneration_;
  pending.inventoryRevision = inventoryRevision_;
  pending.recordRevision = detailRevisions_.value(instanceId);
  pending.detailMemoryRevision = requested.detailMemoryRevision;
  pending.instanceId = instanceId;
  pending.relativePath = QStringLiteral("details/%1.json").arg(instanceId);
  pending.selected = true;
  pending.maximumBytes = 16 * 1024 * 1024;
  if (!queueCacheRead(pending)) {
    QTimer::singleShot(0, this, [this, requested] {
      emit rawRecordLoadFinished(requested, false, QStringLiteral("详情读取队列拒绝任务"), StorageStatus::QueueFull);
    });
    return false;
  }
  queuedDetailReads_.insert(instanceId);
  return true;
}

void PetRepository::pumpCacheReads() {
  if (rawReadRetryAfter_ > rawCacheClock_.elapsed()) {
    cachePumpTimer_->start(int(rawReadRetryAfter_ - rawCacheClock_.elapsed())); return;
  }
  if (!storage_ || session_.state == SessionConnectionState::Closing) {
    queuedReads_.clear();
    pendingDetailNames_.clear();
    detailScanWanted_ = false;
    migrationActive_ = false;
    publishCacheLoading();
    return;
  }
  int scheduled = 0;
  while (pendingReads_.size() < 8 && scheduled < 8) {
    if (queuedReads_.empty() && !pendingDetailNames_.isEmpty()) {
      const QString path = pendingDetailNames_.takeFirst();
      qint64 id = 0;
      if (!PacketContracts::checkedInteger(QFileInfo(path).completeBaseName(), &id, 1) ||
          (!backpack_.contains(id) && !warehouse_.contains(id)) ||
          hasCachedDetail(id) || rawRecordResident(id) || queuedDetailReads_.contains(id) || pendingDetailReads_.contains(id)) continue;
      PendingRead detail;
      detail.kind = ReadKind::Detail;
      detail.context = storageContext_;
      detail.account = accountKey_;
      detail.epoch = sessionGeneration_;
      detail.loadGeneration = cacheLoadGeneration_;
      detail.inventoryRevision = inventoryRevision_;
      detail.recordRevision = detailRevisions_.value(id);
      detail.detailMemoryRevision = detailMemoryRevision(id);
      detail.maximumBytes = 16 * 1024 * 1024;
      detail.instanceId = id;
      detail.relativePath = path;
      queuedReads_.push_back(detail);
      queuedDetailReads_.insert(id);
    }
    if (queuedReads_.empty() && detailScanWanted_ && !detailScanInFlight_ && pendingDetailNames_.isEmpty()) {
      PendingRead scan;
      scan.kind = ReadKind::Directory;
      scan.context = storageContext_;
      scan.account = accountKey_;
      scan.epoch = sessionGeneration_;
      scan.loadGeneration = cacheLoadGeneration_;
      scan.inventoryRevision = inventoryRevision_;
      scan.relativePath = QStringLiteral("details");
      scan.scanCursor = detailScanCursor_;
      queuedReads_.push_back(scan);
    }
    if (queuedReads_.empty()) break;
    const PendingRead pending = queuedReads_.front();
    if (!cacheReadCurrent(pending) || (pending.kind == ReadKind::Detail && !pending.selected &&
        !backpack_.contains(pending.instanceId) && !warehouse_.contains(pending.instanceId))) {
      queuedReads_.pop_front();
      queuedDetailReads_.remove(pending.instanceId);
      continue;
    }
    StorageSubmission admission;
    if (pending.kind == ReadKind::Directory) {
      admission = storage_->submitScan({pending.context, pending.relativePath,
          {QStringLiteral("*.json")}, pending.recordRevision, pending.scanCursor, 64});
    } else {
      admission = storage_->submitRead({pending.context, pending.relativePath, pending.recordRevision,
                                        pending.maximumBytes, pending.selected});
    }
    if (!admission.accepted && admission.status == StorageStatus::QueueFull) {
      cachePumpTimer_->start(5);
      break;
    }
    queuedReads_.pop_front();
    queuedDetailReads_.remove(pending.instanceId);
    if (!admission.accepted) {
      if (pending.kind == ReadKind::Directory) { detailScanWanted_ = false; detailScanInFlight_ = false; }
      if (pending.kind == ReadKind::Detail)
        emit rawRecordLoadFinished({pending.account, pending.epoch, pending.instanceId, pending.detailMemoryRevision},
                                  false, admission.error, admission.status);
      publishCacheLoading(admission.error);
      continue;
    }
    pendingReads_.insert(admission.taskId, pending);
    if (pending.kind == ReadKind::Detail) pendingDetailReads_.insert(pending.instanceId, admission.taskId);
    if (pending.kind == ReadKind::Directory) detailScanInFlight_ = true;
    ++scheduled;
  }
  pumpMigration();
  publishCacheLoading();
}

void PetRepository::applyInventoryCache(const QJsonObject& object, const PendingRead& pending, const QByteArray& digest) {
  if (!cacheReadCurrent(pending) || listRevision_ != pending.inventoryRevision) return;
  qint64 schema = 0;
  if (!PacketContracts::checkedInteger(object.value(QStringLiteral("schema")), &schema) ||
      (schema != 2 && schema != 3 && schema != 4) || object.value(QStringLiteral("account")).toString() != pending.account ||
      (schema == 4 && object.value(QStringLiteral("trust")).toString() != QStringLiteral("verified-observation") &&
       object.value(QStringLiteral("trust")).toString() != QStringLiteral("read-only-observation"))) return;
  const bool observedOnly = schema == 4 && object.value(QStringLiteral("trust")).toString() == QStringLiteral("read-only-observation");
  const auto backpack = PacketContracts::decodePetList(object.value(QStringLiteral("backpack")));
  const auto warehouse = PacketContracts::decodePetList(object.value(QStringLiteral("warehouse")));
  if (!backpack.valid() || !warehouse.valid()) {
    publishCacheLoading(QStringLiteral("本地列表缓存结构无效，未覆盖当前内存")); return;
  }
  QHash<qint64, QJsonObject> nextBackpack, nextWarehouse;
  QList<RawPetRecordInput> legacyBackpack;
  const QDateTime savedAt = QDateTime::fromString(object.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
  for (auto pet : backpack.pets) {
    const qint64 id = petId(pet);
    if (observedOnly) pet.insert(QStringLiteral("_unverifiedObservation"), true);
    pet = PetDetailCatalog::instance().enrichMetadata(pet, detailIdentities_.value(id));
    pet.insert(QStringLiteral("_location"), QStringLiteral("backpack"));
    const auto brief = inventoryBrief(pet, true); nextBackpack.insert(id, brief);
    // Old schema 3 inventory files held full backpack bodies. Keep them once
    // in the raw cache; new inventory writes contain summaries only. They stay
    // protected until a verified session saves an independent detail file.
    if (pet != brief) {
      RawPetRecordInput input;
      input.key = {accountKey_, sessionGeneration_, id, ++nextDetailMemoryRevision_}; input.object = pet; input.brief = brief;
      QJsonObject checked = pet;
      input.complete = pet.value(QStringLiteral("czdlv")).isObject() && pet.value(QStringLiteral("mzdlv")).isObject() &&
          PacketContracts::normalizePet(&checked, true);
      input.sourceKnown = !pet.value(QStringLiteral("_unverifiedObservation")).toBool();
      input.contentDigest = digest; input.observedAt = savedAt;
      legacyBackpack.append(std::move(input));
    }
  }
  for (auto pet : warehouse.pets) {
    const qint64 id = petId(pet);
    if (observedOnly) pet.insert(QStringLiteral("_unverifiedObservation"), true);
    pet = PetDetailCatalog::instance().enrichMetadata(pet, detailIdentities_.value(id));
    pet.insert(QStringLiteral("_location"), QStringLiteral("warehouse"));
    copyPowerFields(detailIdentities_.value(id), &pet);
    nextWarehouse.insert(id, inventoryBrief(pet, false));
  }
  if (!admitRawRecords(legacyBackpack, false)) {
    publishCacheLoading(QStringLiteral("旧背包详情超过原文缓存预算，列表未覆盖当前内存")); return;
  }
  backpack_ = std::move(nextBackpack); warehouse_ = std::move(nextWarehouse);
  for (auto row = backpack_.begin(); row != backpack_.end(); ++row)
    if (!detailStates_.contains(row.key())) reviseRawBrief(row.key(), row.value(), !row.value().value(QStringLiteral("_unverifiedObservation")).toBool());
  for (auto row = warehouse_.begin(); row != warehouse_.end(); ++row)
    reviseRawBrief(row.key(), row.value(), !row.value().value(QStringLiteral("_unverifiedObservation")).toBool());
  updatedAt_ = savedAt; ++inventoryRevision_; ++listRevision_;
  emit dataChanged();
  if (!cacheReadCurrent(pending)) return;
  for (const auto& input : legacyBackpack) announceRaw(input.key.instanceId);
}

void PetRepository::applyDetailCache(const QJsonObject& object, const PendingRead& pending,
                                    const QDateTime& fileTime, const QByteArray& digest) {
  const PetRecordKey requested{pending.account, pending.epoch, pending.instanceId, pending.detailMemoryRevision};
  const auto finish = [this, requested](bool loaded, const QString& error, StorageStatus status = StorageStatus::ReadFailed) {
    emit rawRecordLoadFinished(requested, loaded, error, loaded ? StorageStatus::Loaded : status);
  };
  if (!cacheReadCurrent(pending) || detailMemoryRevision(pending.instanceId) != pending.detailMemoryRevision ||
      detailRevisions_.value(pending.instanceId) != pending.recordRevision) {
    finish(false, QStringLiteral("读取期间账号或详情版本已变化，旧磁盘结果未覆盖新观察"), StorageStatus::Superseded); return;
  }
  if (!pending.selected && !backpack_.contains(pending.instanceId) && !warehouse_.contains(pending.instanceId)) {
    finish(false, QStringLiteral("实例已离开当前库存，停止后台预读；历史文件仍可显式读取"), StorageStatus::Superseded); return;
  }
  const auto residentVersion = recordVersion(pending.instanceId);
  if (rawRecordResident(pending.instanceId) && (residentVersion.complete || residentVersion.persisted)) { finish(true, {}); return; }
  qint64 schema = 0, envelopeId = 0;
  QJsonObject detail = object.value(QStringLiteral("pet")).toObject();
  if (!PacketContracts::checkedInteger(object.value(QStringLiteral("schema")), &schema) ||
      (schema != 2 && schema != 3 && schema != 4) || object.value(QStringLiteral("account")).toString() != pending.account ||
      (schema == 4 && object.value(QStringLiteral("trust")).toString() != QStringLiteral("verified-observation") &&
       object.value(QStringLiteral("trust")).toString() != QStringLiteral("read-only-observation")) ||
      !PacketContracts::normalizePet(&detail, true) || petId(detail) != pending.instanceId || digest.size() != 32 ||
      (object.contains(QStringLiteral("complete")) && !object.value(QStringLiteral("complete")).isBool()) ||
      (object.contains(QStringLiteral("instanceId")) &&
       (!PacketContracts::checkedInteger(object.value(QStringLiteral("instanceId")), &envelopeId, 1) || envelopeId != pending.instanceId))) {
    finish(false, QStringLiteral("本地详情结构、账号或实例不匹配")); return;
  }
  const auto previous = detailStates_.value(pending.instanceId);
  const bool knownOriginal = previous.complete || previous.persisted;
  if (knownOriginal && (!previous.persisted || previous.contentDigest != digest)) {
    finish(false, QStringLiteral("本地文件已变化，不能用不同内容覆盖已知详情版本")); return;
  }
  // Everything a consumer can read before this reload publishes: the roster
  // copy, the merged detail view every reader and the move policy use, and the
  // record's completeness/source evidence. A reload that reproduces all of them
  // has observed nothing new.
  const bool rosterPresent = warehouse_.contains(pending.instanceId) ||
      backpack_.contains(pending.instanceId);
  const QJsonObject rosterBriefBefore = warehouse_.contains(pending.instanceId)
      ? warehouse_.value(pending.instanceId) : backpack_.value(pending.instanceId);
  const QJsonObject mergedBefore = mergedRecordView(pending.instanceId);
  if (schema == 4 && object.value(QStringLiteral("trust")).toString() == QStringLiteral("read-only-observation"))
    detail.insert(QStringLiteral("_unverifiedObservation"), true);
  detail = PetDetailCatalog::instance().enrichMetadata(detail);
  QDateTime savedAt = QDateTime::fromString(object.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
  if (!savedAt.isValid()) savedAt = fileTime;
  QDateTime observedAt = QDateTime::fromString(object.value(QStringLiteral("observedAt")).toString(), Qt::ISODate);
  if (!observedAt.isValid()) observedAt = savedAt;
  QJsonObject brief = recordBrief(pending.instanceId);
  if (!onlineData_ && (!updatedAt_.isValid() || observedAt >= updatedAt_)) {
    const auto roster = brief;
    brief = merge(brief, inventoryBrief(detail, backpack_.contains(pending.instanceId)));
    for (const QString& field : {QStringLiteral("_location"), QStringLiteral("_warehouseGroup"),
         QStringLiteral("_packType"), QStringLiteral("_position")})
      if (roster.contains(field)) brief.insert(field, roster.value(field));
  }
  const bool mismatch = !brief.isEmpty() && visualIdentityDiffers(detail, brief);
  // Explicit history reads have no live list summary. Freeze their complete
  // scalar identity from the validated original so facts can be derived and
  // release the raw cache's underived pin, without inventing roster membership.
  if (brief.isEmpty()) brief = inventoryBrief(detail, false);
  if (mismatch) brief.insert(QStringLiteral("_visualMismatch"), true);
  copyPowerFields(detail, &brief);
  brief = inventoryBrief(brief, backpack_.contains(pending.instanceId));
  RawPetRecordInput input;
  input.object = detail; input.brief = brief; input.persisted = true; input.contentDigest = digest;
  input.complete = object.value(QStringLiteral("complete")).toBool(true);
  input.sourceKnown = !detail.value(QStringLiteral("_unverifiedObservation")).toBool() &&
      !brief.value(QStringLiteral("_unverifiedObservation")).toBool();
  // The record key identifies every input of the derivation seed. A reload that
  // changes completeness, source evidence or a calculation-relevant field must
  // advance it instead of publishing new inputs under the previous version,
  // which would make the derivation cache reject the pet as "one derivation key
  // was reused with different calculation seed" and keep facts derived from the
  // older inputs.
  const bool seedRelevantChange = !knownOriginal || previous.complete != input.complete ||
      previous.sourceKnown != input.sourceKnown || calculationOverlayDiffers(previous.brief, brief);
  input.key = seedRelevantChange
      ? PetRecordKey{accountKey_, sessionGeneration_, pending.instanceId, ++nextDetailMemoryRevision_}
      : previous.key;
  input.observedAt = observedAt;
  input.rawProjectionOnly = calculationProjectionMatchesRaw(detail, brief);
  if (!admitRawRecords({input}, false)) {
    rawReadRetryAfter_ = rawCacheClock_.elapsed() + 50;
    if (cacheReadCurrent(pending) && queueCacheRead(pending)) queuedDetailReads_.insert(pending.instanceId);
    else finish(false, QStringLiteral("原文缓存和读取等待队列均已满"), StorageStatus::QueueFull);
    return;
  }
  const qint64 id = pending.instanceId;
  const quint64 revision = ++nextStoreRevision_;
  detailRevisions_.insert(id, revision); savedDetailRevisions_.insert(id, revision);
  savedMemoryRevisions_.insert(id, input.key.detailMemoryRevision); detailSavedTimes_.insert(id, savedAt);
  if (warehouse_.contains(id)) warehouse_.insert(id, brief);
  else if (backpack_.contains(id)) backpack_.insert(id, brief);
  // This reload can land while a move intent is still being journaled (or while
  // an analysis input is being stamped). Bumping the fact revision for a
  // publication that changes nothing any consumer can read would abort that
  // confirmed write as "list or session changed" without any change: keep the
  // bump for genuinely new facts, drop it for an exact republication.
  if (!rosterPresent || rosterBriefBefore != brief || mergedBefore != mergedRecordView(id) ||
      previous.complete != input.complete || previous.sourceKnown != input.sourceKnown)
    ++inventoryRevision_;
  announceRaw(id);
  if (!cacheReadCurrent(pending)) { finish(false, QStringLiteral("详情发布期间会话已变化"), StorageStatus::Superseded); return; }
  emit detailChanged(id);
  if (mismatch && cacheReadCurrent(pending)) emit visualMismatchDetected(id);
  finish(true, {});
}
void PetRepository::handleCacheRead(const StorageResult& result, const PendingRead& pending) {
  if (!cacheReadCurrent(pending)) {
    if (!result.scanCursor.isEmpty() && storage_) storage_->cancelScan(result.scanCursor);
    if (pending.kind == ReadKind::Detail)
      emit rawRecordLoadFinished({pending.account, pending.epoch, pending.instanceId, pending.detailMemoryRevision}, false,
                                QStringLiteral("详情读取所属会话已结束"), StorageStatus::Superseded);
    return;
  }
  if (pending.kind == ReadKind::LastAccount) {
    if (result.status == StorageStatus::Loaded) {
      const QString saved = QString::fromUtf8(result.content).trimmed();
      if (!saved.isEmpty() && saved.size() <= 1024) accountKey_ = saved;
    }
    const QString selected = accountKey_;
    loadAccount();
    if (sessionGeneration_ == 0 && accountKey_ == selected)
      emit accountSessionChanged(accountKey_, sessionGeneration_);
    migrateLegacyCache();
    return;
  }
  if (pending.kind == ReadKind::Directory) {
    detailScanInFlight_ = false;
    if (result.status == StorageStatus::QueueFull) {
      detailScanWanted_ = true;
      cachePumpTimer_->start(5);
      return;
    }
    if (result.status == StorageStatus::Scanned) {
      pendingDetailNames_.append(result.relativeNames);
      detailScanCursor_ = result.scanCursor;
      detailScanWanted_ = result.hasMore;
    } else {
      detailScanWanted_ = false;
      detailScanCursor_.clear();
      if (result.status != StorageStatus::NotFound && result.status != StorageStatus::Cancelled)
        publishCacheLoading(QStringLiteral("详情缓存目录读取失败：%1").arg(result.error));
    }
    return;
  }
  if (pending.kind == ReadKind::Inventory) {
    if (result.status == StorageStatus::Loaded) {
      QElapsedTimer decode; decode.start();
      const QJsonDocument document = QJsonDocument::fromJson(result.content);
      ioMetrics_.inventoryJsonDecodeNanoseconds += decode.nsecsElapsed();
      if (document.isObject()) applyInventoryCache(document.object(), pending, result.contentDigest);
      else publishCacheLoading(QStringLiteral("本地列表缓存不是有效JSON对象，未覆盖当前内存"));
    } else if (result.status != StorageStatus::NotFound && result.status != StorageStatus::Cancelled) {
      publishCacheLoading(QStringLiteral("本地列表缓存读取失败：%1").arg(result.error));
    }
    if (cacheReadCurrent(pending)) loadDetails();
    return;
  }
  if (pending.kind == ReadKind::Detail) {
    if (!pending.selected && !backpack_.contains(pending.instanceId) && !warehouse_.contains(pending.instanceId)) {
      emit rawRecordLoadFinished({pending.account,pending.epoch,pending.instanceId,pending.detailMemoryRevision},false,
          QStringLiteral("实例已离开当前库存，历史详情未自动载入"),StorageStatus::Superseded);
      return;
    }
    if (result.status == StorageStatus::Loaded) {
      QElapsedTimer decode; decode.start();
      const QJsonDocument document = QJsonDocument::fromJson(result.content);
      ioMetrics_.rawJsonDecodeNanoseconds += decode.nsecsElapsed();
      ++ioMetrics_.rawJsonDecodeCalls;
      if (document.isObject()) {
        QElapsedTimer apply; apply.start();
        const QPointer<PetRepository> alive(this);
        applyDetailCache(document.object(), pending, result.modifiedAt, result.contentDigest);
        if (!alive) return;
        ioMetrics_.rawApplyNanoseconds += apply.nsecsElapsed();
      }
      else {
        const QString error = QStringLiteral("本地详情缓存不是有效JSON对象，未覆盖当前详情");
        publishCacheLoading(error);
        emit rawRecordLoadFinished({pending.account, pending.epoch, pending.instanceId, pending.detailMemoryRevision}, false, error);
      }
    } else {
      const QString error = result.error.isEmpty() ? QStringLiteral("本地详情不存在或读取已取消") : result.error;
      if (result.status != StorageStatus::NotFound && result.status != StorageStatus::Cancelled)
        publishCacheLoading(QStringLiteral("本地详情缓存读取失败：%1").arg(error));
      emit rawRecordLoadFinished({pending.account, pending.epoch, pending.instanceId, pending.detailMemoryRevision}, false, error, result.status);
    }
    return;
  }
  if (pending.kind == ReadKind::MigrationMarker) {
    if (result.status == StorageStatus::Loaded) {
      const QJsonObject marker = QJsonDocument::fromJson(result.content).object();
      qint64 schema = 0;
      const QString digest = marker.value(QStringLiteral("sourceDigest")).toString();
      static const QRegularExpression digestPattern(QStringLiteral("^[a-f0-9]{64}$"));
      if (PacketContracts::checkedInteger(marker.value(QStringLiteral("schema")), &schema) && schema == 1 &&
          marker.value(QStringLiteral("completed")).isBool() && marker.value(QStringLiteral("completed")).toBool() &&
          digestPattern.match(digest).hasMatch()) migrationCompletedDigest_ = digest;
    }
    PendingRead legacy;
    legacy.kind = ReadKind::Legacy;
    legacy.context = legacyReadContext_;
    legacy.relativePath = QStringLiteral("cache-v1.json");
    legacy.maximumBytes = 16 * 1024 * 1024;
    queueCacheRead(legacy);
    return;
  }
  if (pending.kind == ReadKind::Legacy && result.status == StorageStatus::Loaded) {
    const QJsonObject legacy = QJsonDocument::fromJson(result.content).object();
    const QString digest = QString::fromLatin1(result.contentDigest.toHex());
    qint64 schema = 0;
    if (!PacketContracts::checkedInteger(legacy.value(QStringLiteral("schema")), &schema) || schema != 1 ||
        !legacy.value(QStringLiteral("profiles")).isObject() || digest == migrationCompletedDigest_) return;
    if (legacy.contains(QStringLiteral("lastAccount")) && !legacy.value(QStringLiteral("lastAccount")).isString()) {
      publishCacheLoading(QStringLiteral("旧缓存账号索引无效，源文件保留且未写完成标记"));
      return;
    }
    migrationProfiles_ = legacy.value(QStringLiteral("profiles")).toObject();
    migrationAccounts_ = migrationProfiles_.keys();
    migrationAccountIndex_ = 0;
    migrationDetailIndex_ = -1;
    migrationDetailKeys_.clear();
    migrationDigest_ = digest;
    migrationLastAccount_ = legacy.value(QStringLiteral("lastAccount")).toString();
    migrationActive_ = true;
    migrationWriteInFlight_ = false;
    migrationIndexPending_ = !migrationLastAccount_.isEmpty();
    migrationMarkerPending_ = false;
  }
}

quint64 PetRepository::queueWrite(const StorageContext& context, const QString& relativePath,
                                  const QByteArray& bytes, PendingWrite pending, bool required, bool onlyIfMissing) {
  pending.relativePath = relativePath;
  pending.revision = ++nextStoreRevision_;
  StorageSubmission admission;
  if (storage_) admission = storage_->submitWrite({context, relativePath, pending.revision, bytes, required, onlyIfMissing});
  else { admission.status = StorageStatus::Closing; admission.error = QStringLiteral("storage is unavailable"); }
  return trackWrite(admission, pending);
}

quint64 PetRepository::queueJsonWrite(const StorageContext& context, const QString& relativePath,
                                      const QJsonObject& object, PendingWrite pending, bool required, bool onlyIfMissing) {
  pending.relativePath = relativePath;
  pending.revision = ++nextStoreRevision_;
  StorageSubmission admission;
  if (storage_) admission = storage_->submitJsonWrite({context, relativePath, pending.revision,
                                                      object, 16 * 1024 * 1024, required, onlyIfMissing});
  else { admission.status = StorageStatus::Closing; admission.error = QStringLiteral("storage is unavailable"); }
  return trackWrite(admission, pending);
}

quint64 PetRepository::trackWrite(const StorageSubmission& admission, const PendingWrite& pending) {
  lastWriteAdmissionStatus_ = admission.status;
  lastWriteAdmissionError_ = admission.error;
  if (!admission.accepted) {
    const QString account = pending.account;
    const quint64 epoch = pending.epoch;
    QMetaObject::invokeMethod(this, [this, pending, admission, account, epoch] {
      if (account == accountKey_ && epoch == sessionGeneration_)
        emit persistenceChanged(account, pending.relativePath, pending.revision,
                                admission.status, admission.error, pending.epoch);
    }, Qt::QueuedConnection);
    return 0;
  }
  if (pending.kind == WriteKind::Detail || pending.kind == WriteKind::Preservation)
    detailRevisions_.insert(pending.instanceId, pending.revision);
  pendingWrites_.insert(admission.taskId, pending);
  QMetaObject::invokeMethod(this, [this, taskId = admission.taskId, pending] {
    if (pendingWrites_.contains(taskId) && pending.account == accountKey_ &&
        pending.epoch == sessionGeneration_)
      emit persistenceChanged(pending.account, pending.relativePath, pending.revision, StorageStatus::Queued, {}, pending.epoch);
  }, Qt::QueuedConnection);
  return admission.taskId;
}

void PetRepository::handleStorageResult(const StorageResult& result) {
  const auto read = pendingReads_.find(result.taskId);
  if (read != pendingReads_.end()) {
    const PendingRead pending = read.value();
    pendingReads_.erase(read);
    if (pending.kind == ReadKind::Detail && pendingDetailReads_.value(pending.instanceId) == result.taskId)
      pendingDetailReads_.remove(pending.instanceId);
    handleCacheRead(result, pending);
    cachePumpTimer_->start(0);
    publishCacheLoading();
    return;
  }
  const auto iterator = pendingWrites_.find(result.taskId);
  if (iterator == pendingWrites_.end()) return;
  const PendingWrite pending = iterator.value();
  pendingWrites_.erase(iterator);
  if (pending.kind == WriteKind::Migration || pending.kind == WriteKind::MigrationIndex ||
      pending.kind == WriteKind::MigrationMarker) {
    migrationWriteFinished(pending, result);
    cachePumpTimer_->start(0);
  }
  const auto current = [this, &pending] {
    return pending.account == accountKey_ && pending.epoch == sessionGeneration_;
  };
  if (!current()) return;  // Old accepted writes still commit to their frozen old-account path.
  const bool detail = pending.kind == WriteKind::Detail || pending.kind == WriteKind::Preservation;
  bool saved = result.status == StorageStatus::Saved;
  QString persistenceError = result.error;
  StorageStatus persistenceStatus = result.status;
  if (detail && saved && result.contentDigest.size() != 32) {
    saved = false; persistenceStatus = StorageStatus::WriteFailed;
    persistenceError = QStringLiteral("保存返回缺少最终内容摘要，详情保存状态未获确认");
  }
  const bool latestDetail = !detail || (detailRevisions_.value(pending.instanceId) == pending.revision &&
      detailMemoryRevision(pending.instanceId) == pending.recordKey.detailMemoryRevision);
  if (detail && saved) {
    auto currentRaw = rawRecords_->acquire(accountKey_, sessionGeneration_, pending.instanceId);
    const bool samePayload = currentRaw && pending.rawRecord && currentRaw->payload == pending.rawRecord->payload;
    if (samePayload || latestDetail) {
      const auto confirmed = currentRaw ? rawRecords_->saved(currentRaw->key, result.contentDigest) : RawPetRecordHandle{};
      if (currentRaw && !confirmed) {
        saved = false; persistenceStatus = StorageStatus::WriteFailed;
        persistenceError = QStringLiteral("原文版本未能绑定保存摘要，详情保存状态未获确认");
      } else {
        auto& state = detailStates_[pending.instanceId];
        state.persisted = true; state.contentDigest = result.contentDigest;
        savedMemoryRevisions_.insert(pending.instanceId, pending.recordKey.detailMemoryRevision);
        announceRaw(pending.instanceId);
        if (!current()) return;
      }
    }
  }
  if (detail && saved && latestDetail) {
    savedDetailRevisions_.insert(pending.instanceId, pending.revision);
    detailSavedTimes_.insert(pending.instanceId, pending.savedAt);
  }
  // A partial backpack observation may arrive before the first local read.
  // Its only-if-missing write cannot replace an older full original. Load
  // that original now while keeping the newly observed compact list fields.
  if (detail && latestDetail && result.status == StorageStatus::Superseded &&
      pending.rawRecord && !pending.rawRecord->complete)
    requestCachedDetail(pending.instanceId);
  emit persistenceChanged(pending.account, pending.relativePath, pending.revision, persistenceStatus, persistenceError, pending.epoch);
  if (!current()) return;
  QString reason = persistenceError;
  if (!latestDetail) reason = QStringLiteral("该详情已有更新版本，旧保存结果不再代表当前详情");
  else if (!saved && reason.isEmpty()) reason = QStringLiteral("详情未持久化：任务被更新替代或存储已关闭");
  if (pending.kind == WriteKind::Preservation) {
    emit detailPreservationFinished(result.taskId, pending.instanceId, saved && latestDetail, reason);
  } else if (pending.kind == WriteKind::Detail && pending.requestGeneration != 0) {
    if (saved && latestDetail) emit detailResponseAccepted(pending.instanceId, pending.requestGeneration);
    else emit detailResponseRejected(pending.instanceId, pending.requestGeneration, reason);
  }
}

void PetRepository::writeLastAccount() {
  if (!canCacheAccountObservation()) return;
  PendingWrite pending;
  pending.kind = WriteKind::LastAccount;
  pending.account = accountKey_;
  pending.epoch = sessionGeneration_;
  queueWrite(sharedStorageContext_, QStringLiteral("last-account.txt"), accountKey_.toUtf8(), pending);
}

void PetRepository::saveInventory() {
  if (!canCacheAccountObservation()) return;
  QJsonArray backpack;
  for (const QJsonObject& pet : backpack_.values()) backpack.append(pet);
  QJsonArray warehouse;
  for (const QJsonObject& pet : warehouse_.values()) warehouse.append(warehouseBriefForCache(pet));
  const QJsonObject object{{QStringLiteral("schema"), 4},
                           {QStringLiteral("trust"), currentPacketCanPersist() ? QStringLiteral("verified-observation") : QStringLiteral("read-only-observation")},
                           {QStringLiteral("account"), accountKey_},
                           {QStringLiteral("savedAt"),
                            (updatedAt_.isValid() ? updatedAt_ : QDateTime::currentDateTime()).toString(Qt::ISODateWithMs)},
                           {QStringLiteral("backpack"), backpack}, {QStringLiteral("warehouse"), warehouse}};
  PendingWrite pending;
  pending.kind = WriteKind::Inventory;
  pending.account = accountKey_;
  pending.epoch = sessionGeneration_;
  queueJsonWrite(storageContext_, QStringLiteral("inventory.json"), object, pending);
  writeLastAccount();
}

quint64 PetRepository::saveDetail(qint64 instanceId, const QJsonObject& detail,
                                 const QDateTime& requestedSavedAt, quint64 requestGeneration,
                                 bool preservation) {
  if (preservation ? !session_.canPersist() : !canCacheAccountObservation()) return 0;
  auto raw = rawRecords_->acquire(accountKey_, sessionGeneration_, instanceId);
  if (!raw || raw->object() != detail) {
    lastWriteAdmissionStatus_ = StorageStatus::InvalidRequest;
    lastWriteAdmissionError_ = QStringLiteral("详情原文已经变化或不驻留，拒绝保存合成/旧版本数据");
    return 0;
  }
  QDateTime savedAt = requestedSavedAt;
  if (!savedAt.isValid()) savedAt = detailSavedTimes_.value(instanceId);
  if (!savedAt.isValid()) savedAt = QDateTime::currentDateTime();
  const auto brief = recordBrief(instanceId);
  const QJsonValue obtainedAt = detail.contains(QStringLiteral("gd")) ? detail.value(QStringLiteral("gd")) : brief.value(QStringLiteral("gd"));
  const QJsonObject object{{QStringLiteral("schema"), 4}, {QStringLiteral("account"), accountKey_},
      {QStringLiteral("trust"), raw->sourceKnown ? QStringLiteral("verified-observation") : QStringLiteral("read-only-observation")},
      {QStringLiteral("instanceId"), QString::number(instanceId)}, {QStringLiteral("complete"), raw->complete},
      {QStringLiteral("raceIdAtSave"), petRaceId(detail)}, {QStringLiteral("faceIdAtSave"), petFaceId(detail)},
      {QStringLiteral("displayNameAtSave"), petProtocolName(detail)}, {QStringLiteral("obtainedAt"), obtainedAt},
      {QStringLiteral("savedAt"), savedAt.toString(Qt::ISODateWithMs)}, {QStringLiteral("imageCacheKey"), petVisualKey(detail)},
      {QStringLiteral("observedAt"), raw->observedAt.toString(Qt::ISODateWithMs)},
      {QStringLiteral("pet"), raw->object()}};
  PendingWrite pending;
  pending.kind = preservation ? WriteKind::Preservation : WriteKind::Detail;
  pending.account = accountKey_; pending.epoch = sessionGeneration_; pending.instanceId = instanceId;
  pending.requestGeneration = requestGeneration; pending.receiveSequence = currentEnvelope_.receiveSequence;
  pending.savedAt = savedAt; pending.recordKey = raw->key; pending.rawRecord = raw;
  // Storage jobs hold JSON values; the matching pending record also holds the
  // raw lease until the IO completion is consumed, including account switches.
  detailRevisions_.insert(instanceId, nextStoreRevision_ + 1);
  return queueJsonWrite(storageContext_, QStringLiteral("details/%1.json").arg(instanceId), object, pending, true,
                        !raw->complete && !preservation);
}
void PetRepository::migrateLegacyCache() {
  if (migrationProbeStarted_ || !storage_) return;
  migrationProbeStarted_ = true;
  legacyReadContext_ = storage_->createReadOnlyContext(legacyDataRoot_);
  PendingRead marker;
  marker.kind = ReadKind::MigrationMarker;
  marker.context = sharedStorageContext_;
  marker.relativePath = QStringLiteral("migration-v1-completed.json");
  marker.maximumBytes = 4096;
  queueCacheRead(marker);
}

void PetRepository::pumpMigration() {
  if (!migrationActive_ || migrationWriteInFlight_ || !storage_) return;
  PendingWrite pending;
  pending.epoch = sessionGeneration_;
  QString path;
  QJsonObject object;
  StorageContext context;
  bool conditional = true;
  if (migrationAccountIndex_ < migrationAccounts_.size()) {
    const QString account = migrationAccounts_.at(migrationAccountIndex_);
    const QJsonObject profile = migrationProfiles_.value(account).toObject();
    if (profile.contains(QStringLiteral("details")) && !profile.value(QStringLiteral("details")).isObject()) {
      migrationActive_ = false;
      publishCacheLoading(QStringLiteral("旧详情集合无效，源文件保留且未记录完成标记"));
      return;
    }
    const QJsonObject details = profile.value(QStringLiteral("details")).toObject();
    if (!migrationContext_) {
      migrationContext_ = storage_->createAccountContext(account, safeAccountName(account));
      migrationDetailKeys_ = details.keys();
    }
    context = migrationContext_;
    pending.kind = WriteKind::Migration;
    pending.account = account;
    if (migrationDetailIndex_ < 0) {
      const DecodedPetList backpack = PacketContracts::decodePetList(profile.value(QStringLiteral("backpack")));
      const DecodedPetList warehouse = PacketContracts::decodePetList(profile.value(QStringLiteral("warehouse")));
      if (!backpack.valid() || !warehouse.valid()) {
        migrationActive_ = false;
        publishCacheLoading(QStringLiteral("旧缓存迁移结构无效，源文件保留且未记录完成标记"));
        return;
      }
      QJsonArray briefPets;
      for (const QJsonObject& pet : warehouse.pets) briefPets.append(warehouseBriefForCache(pet));
      path = QStringLiteral("inventory.json");
      object = {{QStringLiteral("schema"), 3}, {QStringLiteral("account"), account},
                {QStringLiteral("savedAt"), profile.value(QStringLiteral("savedAt"))},
                {QStringLiteral("backpack"), profile.value(QStringLiteral("backpack"))},
                {QStringLiteral("warehouse"), briefPets}};
    } else if (migrationDetailIndex_ < migrationDetailKeys_.size()) {
      qint64 id = 0;
      const QString key = migrationDetailKeys_.at(migrationDetailIndex_);
      QJsonObject pet = details.value(key).toObject();
      if (!PacketContracts::checkedInteger(key, &id, 1) || !PacketContracts::normalizePet(&pet, true) || petId(pet) != id) {
        migrationActive_ = false;
        publishCacheLoading(QStringLiteral("旧详情迁移结构无效，源文件保留且未记录完成标记"));
        return;
      }
      path = QStringLiteral("details/%1.json").arg(id);
      object = {{QStringLiteral("schema"), 3}, {QStringLiteral("account"), account},
                {QStringLiteral("instanceId"), QString::number(id)}, {QStringLiteral("pet"), pet},
                {QStringLiteral("raceIdAtSave"), petRaceId(pet)}, {QStringLiteral("faceIdAtSave"), petFaceId(pet)},
                {QStringLiteral("displayNameAtSave"), petProtocolName(pet)},
                {QStringLiteral("obtainedAt"), pet.value(QStringLiteral("gd"))},
                {QStringLiteral("savedAt"), profile.value(QStringLiteral("savedAt"))},
                {QStringLiteral("imageCacheKey"), petVisualKey(pet)}};
    } else {
      ++migrationAccountIndex_;
      migrationDetailIndex_ = -1;
      migrationDetailKeys_.clear();
      migrationContext_.reset();
      cachePumpTimer_->start(0);
      return;
    }
  } else {
    context = sharedStorageContext_;
    pending.account = migrationLastAccount_;
    if (migrationIndexPending_) {
      pending.kind = WriteKind::MigrationIndex;
      const quint64 task = queueWrite(context, QStringLiteral("last-account.txt"),
                                     migrationLastAccount_.toUtf8(), pending, false, true);
      if (task) migrationWriteInFlight_ = true;
      else if (lastWriteAdmissionStatus_ == StorageStatus::QueueFull) cachePumpTimer_->start(10);
      else {
        migrationActive_ = false;
        publishCacheLoading(QStringLiteral("旧缓存迁移未入队：%1；未记录完成标记").arg(lastWriteAdmissionError_));
      }
      return;
    }
    pending.kind = WriteKind::MigrationMarker;
    conditional = false;
    path = QStringLiteral("migration-v1-completed.json");
    object = {{QStringLiteral("schema"), 1}, {QStringLiteral("sourceDigest"), migrationDigest_},
              {QStringLiteral("completed"), true}};
  }
  const quint64 task = queueJsonWrite(context, path, object, pending, false, conditional);
  if (task) migrationWriteInFlight_ = true;
  else if (lastWriteAdmissionStatus_ == StorageStatus::QueueFull) cachePumpTimer_->start(10);
  else {
    migrationActive_ = false;
    publishCacheLoading(QStringLiteral("旧缓存迁移未入队：%1；未记录完成标记").arg(lastWriteAdmissionError_));
  }
}

void PetRepository::migrationWriteFinished(const PendingWrite& pending, const StorageResult& result) {
  migrationWriteInFlight_ = false;
  if (result.status != StorageStatus::Saved && result.status != StorageStatus::Superseded) {
    migrationActive_ = false;
    migrationContext_.reset();
    publishCacheLoading(QStringLiteral("旧缓存迁移未完成：%1；源文件保留，下次启动继续尝试").arg(result.error));
    return;
  }
  if (pending.kind == WriteKind::Migration) {
    ++migrationDetailIndex_;
  } else if (pending.kind == WriteKind::MigrationIndex) {
    migrationIndexPending_ = false;
    migrationMarkerPending_ = true;
  } else if (pending.kind == WriteKind::MigrationMarker) {
    migrationActive_ = false;
    migrationCompletedDigest_ = migrationDigest_;
    migrationProfiles_ = {};
    migrationAccounts_.clear();
    migrationContext_.reset();
    if (sessionGeneration_ == 0 && !authenticated_ && session_.state == SessionConnectionState::Disconnected &&
        accountKey_ == QStringLiteral("default") &&
        !migrationLastAccount_.isEmpty()) {
      const QString selected = migrationLastAccount_;
      accountKey_ = selected;
      loadAccount();
      if (sessionGeneration_ == 0 && accountKey_ == selected && !authenticated_)
        emit accountSessionChanged(accountKey_, sessionGeneration_);
    }
  }
  publishCacheLoading();
}
