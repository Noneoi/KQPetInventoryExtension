#include "asset_snapshot_store.h"

#include "diagnostic_logger.h"
#include "pet_repository.h"
#include "packet_contract.h"
#include "storage_service.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <limits>

#include <algorithm>

namespace {

QJsonObject snapshotPetObject(const AssetSnapshotPet& pet) {
  return {{QStringLiteral("instanceId"), QString::number(pet.instanceId)},
          {QStringLiteral("raceId"), pet.raceId},
          {QStringLiteral("name"), pet.name},
          {QStringLiteral("currentPower"), pet.currentPower},
          {QStringLiteral("highestPower"), pet.highestPower},
          {QStringLiteral("completionPercent"), pet.completionPercent},
          {QStringLiteral("fullyCultivated"), pet.fullyCultivated},
          {QStringLiteral("redStarComplete"), pet.redStarComplete},
          {QStringLiteral("astrolabeBreakthrough"), pet.astrolabeBreakthrough},
          {QStringLiteral("currentPowerKnown"), pet.currentPowerKnown},
          {QStringLiteral("cultivationKnown"), pet.cultivationKnown},
          {QStringLiteral("redStarKnown"), pet.redStarKnown},
          {QStringLiteral("astrolabeKnown"), pet.astrolabeKnown}};
}

bool optionalInteger(const QJsonObject& object, const QString& key, int* result,
                     int maximum = std::numeric_limits<int>::max()) {
  if (!object.contains(key)) return true;
  qint64 value = 0;
  if (!PacketContracts::checkedInteger(object.value(key), &value, 0, maximum)) return false;
  *result = static_cast<int>(value);
  return true;
}

bool optionalBoolean(const QJsonObject& object, const QString& key, bool* result) {
  if (!object.contains(key)) return true;
  const QJsonValue value = object.value(key);
  if (!value.isBool()) return false;
  *result = value.toBool();
  return true;
}

bool parseSnapshotPet(const QJsonObject& object, AssetSnapshotPet* pet) {
  if (!PacketContracts::checkedInteger(object.value(QStringLiteral("instanceId")), &pet->instanceId, 1) ||
      !optionalInteger(object, QStringLiteral("raceId"), &pet->raceId) ||
      !optionalInteger(object, QStringLiteral("currentPower"), &pet->currentPower) ||
      !optionalInteger(object, QStringLiteral("highestPower"), &pet->highestPower) ||
      !optionalInteger(object, QStringLiteral("completionPercent"), &pet->completionPercent, 100) ||
      !optionalBoolean(object, QStringLiteral("fullyCultivated"), &pet->fullyCultivated) ||
      !optionalBoolean(object, QStringLiteral("redStarComplete"), &pet->redStarComplete) ||
      !optionalBoolean(object, QStringLiteral("astrolabeBreakthrough"), &pet->astrolabeBreakthrough) ||
      !optionalBoolean(object, QStringLiteral("currentPowerKnown"), &pet->currentPowerKnown) ||
      !optionalBoolean(object, QStringLiteral("cultivationKnown"), &pet->cultivationKnown) ||
      !optionalBoolean(object, QStringLiteral("redStarKnown"), &pet->redStarKnown) ||
      !optionalBoolean(object, QStringLiteral("astrolabeKnown"), &pet->astrolabeKnown)) return false;
  if (object.contains(QStringLiteral("name")) && !object.value(QStringLiteral("name")).isString()) return false;
  pet->name = object.value(QStringLiteral("name")).toString();
  // Legacy optional known fields stay false. A new known=true assertion is
  // accepted only when the corresponding value was actually present/typed.
  if ((pet->currentPowerKnown && !object.contains(QStringLiteral("currentPower"))) ||
      (pet->cultivationKnown && !object.contains(QStringLiteral("fullyCultivated"))) ||
      (pet->redStarKnown && !object.contains(QStringLiteral("redStarComplete"))) ||
      (pet->astrolabeKnown && !object.contains(QStringLiteral("astrolabeBreakthrough")))) return false;
  return true;
}

bool parseSnapshot(const QJsonObject& object, int schema, AccountAssetSnapshot* snapshot) {
  snapshot->schemaVersion = schema;
  snapshot->analysisVersion = schema == AssetAnalysisVersion::kLegacySnapshotSchema ? 1 : 0;
  if (!optionalInteger(object, QStringLiteral("analysisVersion"), &snapshot->analysisVersion) ||
      snapshot->analysisVersion <= 0) return false;
  snapshot->account = object.value(QStringLiteral("account")).toString();
  snapshot->createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
  if (snapshot->account.isEmpty() || !snapshot->createdAt.isValid() ||
      !object.value(QStringLiteral("pets")).isArray()) return false;
  if (schema == AssetAnalysisVersion::kCurrentSnapshotSchema &&
      !object.value(QStringLiteral("summary")).isObject()) return false;
  const QJsonObject summary = schema == AssetAnalysisVersion::kCurrentSnapshotSchema
                                  ? object.value(QStringLiteral("summary")).toObject() : object;
  qint64 total = 0, cultivated = 0;
  if (!PacketContracts::checkedInteger(summary.value(QStringLiteral("totalPets")), &total, 0,
                                       std::numeric_limits<int>::max()) ||
      !PacketContracts::checkedInteger(summary.value(QStringLiteral("fullyCultivatedPets")), &cultivated, 0, total) ||
      !PacketContracts::checkedInteger(summary.value(QStringLiteral("totalCurrentPower")),
                                       &snapshot->totalCurrentPower, 0)) return false;
  snapshot->totalPets = static_cast<int>(total);
  snapshot->fullyCultivatedPets = static_cast<int>(cultivated);
  if (!optionalBoolean(summary, QStringLiteral("totalCurrentPowerKnown"), &snapshot->totalCurrentPowerKnown)) return false;
  QSet<qint64> seen;
  for (const QJsonValue& value : object.value(QStringLiteral("pets")).toArray()) {
    AssetSnapshotPet pet;
    if (!value.isObject() || !parseSnapshotPet(value.toObject(), &pet) || seen.contains(pet.instanceId)) return false;
    seen.insert(pet.instanceId);
    snapshot->pets.append(pet);
  }
  snapshot->petsComplete = snapshot->pets.size() == snapshot->totalPets;
  if (snapshot->totalCurrentPowerKnown) {
    qint64 measured = 0;
    if (!snapshot->petsComplete) return false;
    for (const auto& pet : snapshot->pets) {
      if (!pet.currentPowerKnown) return false;
      measured += pet.currentPower; // int-count x nonnegative int is within qint64.
    }
    if (measured != snapshot->totalCurrentPower) return false;
  }
  return true;
}

}  // namespace

namespace {
qint64 snapshotBytes(const AccountAssetSnapshot& snapshot) {
  qint64 bytes = sizeof(AccountAssetSnapshot) + snapshot.pets.capacity() * qint64(sizeof(AssetSnapshotPet));
  bytes += (snapshot.account.capacity() + snapshot.storageKey.capacity()) * qint64(sizeof(QChar));
  for (const AssetSnapshotPet& pet : snapshot.pets) bytes += pet.name.capacity() * qint64(sizeof(QChar));
  return bytes;
}

bool earlierSnapshot(const AccountAssetSnapshot& left, const AccountAssetSnapshot& right) {
  if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
  return left.storageKey < right.storageKey;
}
SnapshotInstanceHistoryEntry instanceEntry(const AccountAssetSnapshot& snapshot, qint64 id) {
  SnapshotInstanceHistoryEntry entry;
  entry.storageKey = snapshot.storageKey;
  entry.createdAt = snapshot.createdAt;
  entry.schemaVersion = snapshot.schemaVersion;
  entry.analysisVersion = snapshot.analysisVersion;
  entry.membershipKnown = snapshot.petsComplete;
  for (const AssetSnapshotPet& pet : snapshot.pets) if (pet.instanceId == id) {
    entry.pet = pet;
    entry.present = true;
    entry.membershipKnown = true;
    break;
  }
  return entry;
}

void putInstanceEntry(QList<SnapshotInstanceHistoryEntry>* entries, const SnapshotInstanceHistoryEntry& entry) {
  const auto old = std::find_if(entries->begin(), entries->end(), [&](const auto& value) {
    return value.storageKey == entry.storageKey;
  });
  if (old != entries->end()) entries->erase(old);
  entries->append(entry);
  std::sort(entries->begin(), entries->end(), [](const auto& left, const auto& right) {
    return left.createdAt == right.createdAt ? left.storageKey < right.storageKey : left.createdAt < right.createdAt;
  });
}

}  // namespace

AssetSnapshotStore::AssetSnapshotStore(PetRepository* repository, QObject* parent,
                                       const SnapshotStorageLimits& limits)
    : QObject(parent), repository_(repository), storage_(repository ? repository->storageService() : nullptr),
      limits_(limits), pumpTimer_(this) {
  limits_.maximumSummaries = std::max(1, limits_.maximumSummaries);
  limits_.maximumFullSnapshots = std::max(1, limits_.maximumFullSnapshots);
  limits_.maximumFullCacheBytes = std::max<qint64>(1, limits_.maximumFullCacheBytes);
  limits_.maximumSnapshotBytes = std::clamp<qint64>(limits_.maximumSnapshotBytes, 1, 16 * 1024 * 1024);
  limits_.maximumConcurrentReads = std::clamp(limits_.maximumConcurrentReads, 1, 2);
  pumpTimer_.setSingleShot(true);
  connect(&pumpTimer_, &QTimer::timeout, this, &AssetSnapshotStore::pump);
  if (!repository_ || !storage_) return;
  connect(storage_, &StorageService::completed, this, &AssetSnapshotStore::receive);
  connect(repository_, &PetRepository::accountSessionChanged, this, &AssetSnapshotStore::switchAccount);
  account_ = repository_->accountKey();
  epoch_ = repository_->sessionGeneration();
  requestHistory(account_);
}

AssetSnapshotStore::~AssetSnapshotStore() {
  pumpTimer_.stop();
  if (storage_) {
    storage_->cancelReads(historyContext_);
    storage_->cancelReads(instanceContext_);
    if (!scanCursor_.isEmpty()) storage_->cancelScan(scanCursor_);
  }
}

StorageContext AssetSnapshotStore::newReadContext() const {
  if (!storage_ || !repository_ || !repository_->storageContext()) return {};
  return storage_->createAccountContext(repository_->accountKey(),
      QFileInfo(repository_->storageContext()->directory()).fileName());
}

void AssetSnapshotStore::switchAccount(const QString& account, quint64 epoch) {
  Q_UNUSED(epoch);
  selectedInstance_ = 0;
  selectedFullKey_.clear();
  requestHistory(account);
}

StorageSubmission AssetSnapshotStore::write(const QString& account, const AccountAssetOverview& overview,
                                            QString* admissionStatus) {
  StorageSubmission rejected;
  const auto deny = [&](const QString& error) {
    rejected.error = error;
    if (admissionStatus) *admissionStatus = error;
    return rejected;
  };
  if (!repository_ || !storage_ || account.isEmpty() || account != repository_->accountKey() || overview.account != account)
    return deny(QStringLiteral("快照账号与当前读取账号不一致"));
  if (!repository_->sessionContext().canPersist() || !overview.sourceVerified ||
      overview.inputSessionEpoch != repository_->sessionGeneration() ||
      overview.inventoryRevision != repository_->inventoryRevision())
    return deny(QStringLiteral("分析来源未核实或输入版本已变化，未提交快照保存"));
  if (overview.totalPets <= 0 || overview.totalPets != overview.pets.size() || overview.analysisVersion <= 0 ||
      overview.fullyCultivatedPets < 0 || overview.fullyCultivatedPets > overview.totalPets || overview.totalCurrentPower < 0)
    return deny(QStringLiteral("快照汇总与实例数据无效，旧快照保持不变"));
  AccountAssetSnapshot snapshot;
  snapshot.schemaVersion = AssetAnalysisVersion::kCurrentSnapshotSchema;
  snapshot.analysisVersion = overview.analysisVersion;
  snapshot.account = account;
  snapshot.createdAt = QDateTime::currentDateTimeUtc();
  snapshot.totalPets = overview.totalPets;
  snapshot.fullyCultivatedPets = overview.fullyCultivatedPets;
  snapshot.totalCurrentPower = overview.totalCurrentPower;
  snapshot.petsComplete = true;
  snapshot.storageKey = QStringLiteral("snapshots/%1.json").arg(snapshot.createdAt.toLocalTime().date().toString(QStringLiteral("yyyy-MM-dd")));
  QJsonArray pets;
  QSet<qint64> seen;
  bool allPowerKnown = true;
  qint64 measuredPower = 0;
  for (const PetAssetRecord& record : overview.pets) {
    if (record.instanceId <= 0 || seen.contains(record.instanceId) || record.raceId < 0 ||
        record.currentPower < 0 || record.highestPower < 0 || record.completionPercent < 0 || record.completionPercent > 100)
      return deny(QStringLiteral("快照包含无效或重复实例，旧快照保持不变"));
    seen.insert(record.instanceId);
    AssetSnapshotPet pet;
    pet.instanceId = record.instanceId;
    pet.raceId = record.raceId;
    pet.name = record.name;
    pet.currentPower = record.currentPower;
    pet.highestPower = record.highestPower;
    pet.completionPercent = record.completionPercent;
    pet.fullyCultivated = record.fullyCultivated;
    pet.currentPowerKnown = record.currentPowerKnown;
    allPowerKnown &= record.currentPowerKnown;
    if (record.currentPowerKnown) measuredPower += record.currentPower;
    pet.cultivationKnown = record.cultivationKnown;
    pet.redStarKnown = record.redStarKnown;
    pet.astrolabeKnown = record.astrolabeKnown;
    pet.redStarComplete = record.redStarKnown && !record.redStarMissing;
    pet.astrolabeBreakthrough = record.astrolabeKnown && !record.astrolabeMissing;
    snapshot.pets.append(pet);
    pets.append(snapshotPetObject(pet));
  }
  snapshot.totalCurrentPowerKnown = allPowerKnown && measuredPower == snapshot.totalCurrentPower;
  const QJsonObject object{{QStringLiteral("schema"), snapshot.schemaVersion},
      {QStringLiteral("analysisVersion"), snapshot.analysisVersion}, {QStringLiteral("account"), account},
      {QStringLiteral("createdAt"), snapshot.createdAt.toString(Qt::ISODate)},
      {QStringLiteral("summary"), QJsonObject{{QStringLiteral("totalPets"), snapshot.totalPets},
          {QStringLiteral("fullyCultivatedPets"), snapshot.fullyCultivatedPets},
          {QStringLiteral("totalCurrentPowerKnown"), snapshot.totalCurrentPowerKnown},
          {QStringLiteral("totalCurrentPower"), QString::number(snapshot.totalCurrentPower)}}},
      {QStringLiteral("pets"), pets}};
  PendingWrite pending;
  pending.account = account;
  pending.key = snapshot.storageKey;
  pending.epoch = overview.inputSessionEpoch;
  pending.inputRevision = overview.inventoryRevision;
  pending.storeRevision = ++nextStoreRevision_;
  pending.snapshot = snapshot;
  const StorageSubmission admission = storage_->submitJsonWrite(
      {repository_->storageContext(), pending.key, pending.storeRevision, object, limits_.maximumSnapshotBytes, false});
  if (admission.accepted) {
    pendingWrites_.insert(admission.taskId, std::move(pending));
    pathRevisions_.insert(snapshot.storageKey, nextStoreRevision_);
    if (admissionStatus) *admissionStatus = QStringLiteral("快照已排队，等待磁盘提交");
  } else if (admissionStatus) *admissionStatus = admission.error;
  QMetaObject::invokeMethod(this, [this, admission, account, epoch = overview.inputSessionEpoch, key = snapshot.storageKey] {
    if (!admission.accepted || pendingWrites_.contains(admission.taskId))
      emit writeStateChanged(admission.taskId, account, epoch, key, admission.status, admission.error);
  }, Qt::QueuedConnection);
  return admission;
}

QList<AccountAssetSnapshot> AssetSnapshotStore::cachedHistory(const QString& account) const {
  if (account != account_) return {};
  QList<AccountAssetSnapshot> result = summaries_;
  for (AccountAssetSnapshot& snapshot : result) {
    const auto full = fullCache_.constFind(snapshot.storageKey);
    if (full != fullCache_.constEnd()) snapshot = full.value().snapshot;
  }
  return result;
}

QList<SnapshotInstanceHistoryEntry> AssetSnapshotStore::cachedInstanceHistory(const QString& account, qint64 instanceId) const {
  return account == account_ && instanceId == selectedInstance_ ? instanceHistory_ : QList<SnapshotInstanceHistoryEntry>{};
}

int AssetSnapshotStore::pendingTaskCount() const {
  return static_cast<int>(pendingReads_.size() + pendingWrites_.size() + queuedReads_.size() +
                          historyNames_.size() + instanceKeys_.size()) + (scanWanted_ && !scanInFlight_ ? 1 : 0);
}

SnapshotCacheStats AssetSnapshotStore::cacheStats() const {
  return {static_cast<int>(summaries_.size()), static_cast<int>(fullCache_.size()), fullCacheBytes_,
          static_cast<int>(instanceHistory_.size()), pendingTaskCount(), historyHasOlder_};
}

void AssetSnapshotStore::requestHistory(const QString& account, const QDateTime& before) {
  if (!repository_ || !storage_ || account != repository_->accountKey()) return;
  if (historyLoading_ && account == account_ && epoch_ == repository_->sessionGeneration() && before_ == before) return;
  if (historyContext_) storage_->cancelReads(historyContext_);
  if (instanceContext_) storage_->cancelReads(instanceContext_);
  if (!scanCursor_.isEmpty()) storage_->cancelScan(scanCursor_);
  account_ = account;
  epoch_ = repository_->sessionGeneration();
  ++historyGeneration_;
  ++instanceGeneration_;
  before_ = before;
  queuedReads_.clear();
  historyNames_.clear();
  instanceKeys_.clear();
  summaries_.clear();
  fullCache_.clear();
  requestedFullKeys_.clear();
  fullCacheBytes_ = 0;
  historyHasOlder_ = false;
  historyError_.clear();
  instanceError_.clear();
  instanceHistory_.clear();
  scanCursor_.clear();
  scanWanted_ = true;
  scanInFlight_ = false;
  historyLoading_ = true;
  instanceLoading_ = selectedInstance_ > 0;
  instanceWaitingForHistory_ = instanceLoading_;
  historyContext_ = newReadContext();
  instanceContext_ = {};
  emit historyChanged(account_, epoch_);
  emit historyLoadingChanged(account_, epoch_, true, {});
  pumpTimer_.start(0);
}

void AssetSnapshotStore::requestSnapshotDetails(const QString& account, const QString& key) {
  if (account != account_ || !repository_ || epoch_ != repository_->sessionGeneration()) return;
  const auto summary = std::find_if(summaries_.cbegin(), summaries_.cend(), [&](const AccountAssetSnapshot& value) {
    return value.storageKey == key;
  });
  if (summary == summaries_.cend()) {
    emit snapshotDetailsChanged(account_, epoch_, key, false, QStringLiteral("记录已不在当前历史索引中"));
    return;
  }
  selectedFullKey_ = key;
  if (fullCache_.contains(key)) {
    fullCache_[key].touch = ++cacheTouch_;
    emit snapshotDetailsChanged(account_, epoch_, key, true, {});
    return;
  }
  if (requestedFullKeys_.contains(key)) return;
  requestedFullKeys_.insert(key);
  for (auto read = pendingReads_.begin(); read != pendingReads_.end(); ++read) {
    if (read.value().key == key && readCurrent(read.value())) {
      storage_->prioritizeRead(read.key());
      return;
    }
  }
  const auto queued = std::find_if(queuedReads_.begin(), queuedReads_.end(), [&](const PendingRead& read) { return read.key == key; });
  if (queued != queuedReads_.end()) {
    PendingRead read = *queued;
    queuedReads_.erase(queued);
    queuedReads_.push_front(std::move(read));
  } else {
    PendingRead read;
    read.kind = ReadKind::Full;
    read.context = historyContext_;
    read.account = account_;
    read.epoch = epoch_;
    read.generation = historyGeneration_;
    read.key = key;
    read.recordRevision = pathRevisions_.value(key);
    queuedReads_.push_front(read);
  }
  pumpTimer_.start(0);
}

void AssetSnapshotStore::requestInstanceHistory(const QString& account, qint64 instanceId) {
  if (!repository_ || !storage_ || account != repository_->accountKey() || instanceId <= 0) return;
  if (account != account_ || epoch_ != repository_->sessionGeneration()) requestHistory(account);
  if (selectedInstance_ == instanceId && (instanceLoading_ || !instanceHistory_.isEmpty())) return;
  if (instanceContext_) storage_->cancelReads(instanceContext_);
  ++instanceGeneration_;
  selectedInstance_ = instanceId;
  instanceError_.clear();
  instanceHistory_.clear();
  instanceKeys_.clear();
  queuedReads_.erase(std::remove_if(queuedReads_.begin(), queuedReads_.end(), [](const PendingRead& read) {
    return read.kind == ReadKind::Instance;
  }), queuedReads_.end());
  instanceContext_ = newReadContext();
  instanceLoading_ = true;
  instanceWaitingForHistory_ = historyLoading_;
  if (!instanceWaitingForHistory_) startInstanceReads();
  emit instanceHistoryChanged(account_, epoch_, instanceId, true, {});
  pumpTimer_.start(0);
}

void AssetSnapshotStore::startInstanceReads() {
  instanceWaitingForHistory_ = false;
  if (!instanceContext_) instanceContext_ = newReadContext();
  instanceKeys_.clear();
  for (const AccountAssetSnapshot& summary : summaries_) instanceKeys_.append(summary.storageKey);
}

bool AssetSnapshotStore::readCurrent(const PendingRead& read) const {
  if (!repository_ || read.account != account_ || read.epoch != epoch_ ||
      account_ != repository_->accountKey() || epoch_ != repository_->sessionGeneration()) return false;
  return read.generation == (read.kind == ReadKind::Instance ? instanceGeneration_ : historyGeneration_);
}

void AssetSnapshotStore::pump() {
  if (!storage_ || !repository_) return;
  int scheduled = 0;
  while (pendingReads_.size() < limits_.maximumConcurrentReads && scheduled < limits_.maximumConcurrentReads) {
    if (queuedReads_.empty() && !instanceKeys_.isEmpty()) {
      PendingRead read;
      read.kind = ReadKind::Instance;
      read.context = instanceContext_;
      read.account = account_;
      read.epoch = epoch_;
      read.generation = instanceGeneration_;
      read.instanceId = selectedInstance_;
      read.key = instanceKeys_.takeFirst();
      read.recordRevision = pathRevisions_.value(read.key);
      queuedReads_.push_back(read);
    }
    if (queuedReads_.empty() && !historyNames_.isEmpty()) {
      PendingRead read;
      read.kind = ReadKind::Summary;
      read.context = historyContext_;
      read.account = account_;
      read.epoch = epoch_;
      read.generation = historyGeneration_;
      read.key = historyNames_.takeFirst();
      read.recordRevision = pathRevisions_.value(read.key);
      queuedReads_.push_back(read);
    }
    if (queuedReads_.empty() && scanWanted_ && !scanInFlight_ && historyNames_.isEmpty()) {
      PendingRead read;
      read.kind = ReadKind::Scan;
      read.context = historyContext_;
      read.account = account_;
      read.epoch = epoch_;
      read.generation = historyGeneration_;
      read.cursor = scanCursor_;
      queuedReads_.push_back(read);
    }
    if (queuedReads_.empty()) break;
    const PendingRead read = queuedReads_.front();
    if (!readCurrent(read)) { queuedReads_.pop_front(); continue; }
    const StorageSubmission admission = read.kind == ReadKind::Scan
        ? storage_->submitScan({read.context, QStringLiteral("snapshots"), {QStringLiteral("*.json")}, 0, read.cursor, 64})
        : storage_->submitRead({read.context, read.key, read.recordRevision, limits_.maximumSnapshotBytes,
                               read.kind == ReadKind::Full || read.kind == ReadKind::Instance || requestedFullKeys_.contains(read.key)});
    if (!admission.accepted && admission.status == StorageStatus::QueueFull) { pumpTimer_.start(5); break; }
    queuedReads_.pop_front();
    if (!admission.accepted) {
      if (read.kind == ReadKind::Scan) scanWanted_ = false;
      historyError_ = admission.error;
      emit historyLoadingChanged(account_, epoch_, historyLoading_, historyError_);
      if (requestedFullKeys_.remove(read.key) || read.kind == ReadKind::Full)
        emit snapshotDetailsChanged(account_, epoch_, read.key, false, admission.error);
      if (read.kind == ReadKind::Instance) {
        instanceError_ = admission.error;
        emit instanceHistoryChanged(account_, epoch_, selectedInstance_, true, instanceError_);
      }
      continue;
    }
    pendingReads_.insert(admission.taskId, read);
    if (read.kind == ReadKind::Scan) scanInFlight_ = true;
    ++scheduled;
  }
  finishLoadingIfDone();
}

void AssetSnapshotStore::trimSummaries() {
  while (summaries_.size() > limits_.maximumSummaries) {
    const QString key = summaries_.takeFirst().storageKey;
    const auto full = fullCache_.find(key);
    if (full != fullCache_.end()) { fullCacheBytes_ -= full.value().bytes; fullCache_.erase(full); }
    historyHasOlder_ = true;
  }
}

void AssetSnapshotStore::cacheFull(const QString& key, const AccountAssetSnapshot& snapshot) {
  const qint64 bytes = snapshotBytes(snapshot);
  if (fullCache_.contains(key)) {
    fullCacheBytes_ -= fullCache_.value(key).bytes;
    fullCache_.remove(key);
  }
  if (bytes > limits_.maximumFullCacheBytes) return;
  fullCache_.insert(key, {snapshot, bytes, ++cacheTouch_});
  fullCacheBytes_ += bytes;
  while (fullCache_.size() > limits_.maximumFullSnapshots || fullCacheBytes_ > limits_.maximumFullCacheBytes) {
    auto oldest = fullCache_.end();
    for (auto item = fullCache_.begin(); item != fullCache_.end(); ++item) {
      if (item.key() == selectedFullKey_ && fullCache_.size() > 1) continue;
      if (oldest == fullCache_.end() || item.value().touch < oldest.value().touch) oldest = item;
    }
    if (oldest == fullCache_.end()) break;
    fullCacheBytes_ -= oldest.value().bytes;
    fullCache_.erase(oldest);
  }
}

void AssetSnapshotStore::acceptSnapshot(const QString& key, AccountAssetSnapshot snapshot, bool wantedFull) {
  snapshot.storageKey = key;
  if (before_.isValid() && snapshot.createdAt >= before_) return;
  const auto existing = std::find_if(summaries_.begin(), summaries_.end(), [&](const AccountAssetSnapshot& value) {
    return value.storageKey == key;
  });
  if (existing != summaries_.end()) summaries_.erase(existing);
  AccountAssetSnapshot summary = snapshot;
  summary.pets.clear();
  summary.petsComplete = false;
  const auto position = std::lower_bound(summaries_.begin(), summaries_.end(), summary, earlierSnapshot);
  summaries_.insert(position, summary);
  trimSummaries();
  const auto retained = std::find_if(summaries_.cbegin(), summaries_.cend(), [&](const AccountAssetSnapshot& value) {
    return value.storageKey == key;
  });
  if (retained != summaries_.cend()) {
    const qsizetype index = std::distance(summaries_.cbegin(), retained);
    if (wantedFull || index >= summaries_.size() - 2) cacheFull(key, snapshot);
  }
}

void AssetSnapshotStore::receive(const StorageResult& result) {
  const auto write = pendingWrites_.find(result.taskId);
  if (write != pendingWrites_.end()) {
    const PendingWrite pending = write.value();
    pendingWrites_.erase(write);
    if (pending.account == account_ && pending.epoch == epoch_ && repository_ &&
        epoch_ == repository_->sessionGeneration() && account_ == repository_->accountKey() &&
        pathRevisions_.value(pending.key) == pending.storeRevision && result.status == StorageStatus::Saved) {
      acceptSnapshot(pending.key, pending.snapshot, true);
      if (selectedInstance_ > 0) {
        putInstanceEntry(&instanceHistory_, instanceEntry(pending.snapshot, selectedInstance_));
        emit instanceHistoryChanged(account_, epoch_, selectedInstance_, instanceLoading_, instanceError_);
      }
      emit historyChanged(account_, epoch_);
    }
    QMetaObject::invokeMethod(this, [this, task = result.taskId, account = pending.account, epoch = pending.epoch,
                                    key = pending.key, status = result.status, error = result.error] {
      emit writeStateChanged(task, account, epoch, key, status, error);
    }, Qt::QueuedConnection);
    emit writeFinished(result.taskId, pending.account, pending.epoch, pending.inputRevision,
                       pending.snapshot.analysisVersion, result.status, result.error);
    return;
  }
  const auto found = pendingReads_.find(result.taskId);
  if (found == pendingReads_.end()) return;
  const PendingRead read = found.value();
  pendingReads_.erase(found);
  if (!readCurrent(read)) {
    if (storage_ && !result.scanCursor.isEmpty()) storage_->cancelScan(result.scanCursor);
    pumpTimer_.start(0);
    return;
  }
  if (read.kind == ReadKind::Scan) {
    scanInFlight_ = false;
    if (result.status == StorageStatus::QueueFull) { pumpTimer_.start(5); return; }
    scanWanted_ = result.status == StorageStatus::Scanned && result.hasMore;
    scanCursor_ = result.scanCursor;
    if (result.status == StorageStatus::Scanned) historyNames_.append(result.relativeNames);
    else if (result.status != StorageStatus::NotFound && result.status != StorageStatus::Cancelled) {
      historyError_ = result.error;
      emit historyLoadingChanged(account_, epoch_, true, historyError_);
    }
  } else {
    AccountAssetSnapshot snapshot;
    const QJsonObject object = result.status == StorageStatus::Loaded ? QJsonDocument::fromJson(result.content).object() : QJsonObject{};
    qint64 schema = 0;
    const bool valid = result.status == StorageStatus::Loaded &&
        PacketContracts::checkedInteger(object.value(QStringLiteral("schema")), &schema) &&
        (schema == AssetAnalysisVersion::kLegacySnapshotSchema || schema == AssetAnalysisVersion::kCurrentSnapshotSchema) &&
        object.value(QStringLiteral("account")).toString() == account_ &&
        parseSnapshot(object, static_cast<int>(schema), &snapshot);
    const bool versionMatches = pathRevisions_.value(read.key) == read.recordRevision;
    if (valid && versionMatches) {
      snapshot.storageKey = read.key;
      if (read.kind == ReadKind::Instance) {
        putInstanceEntry(&instanceHistory_, instanceEntry(snapshot, read.instanceId));
        emit instanceHistoryChanged(account_, epoch_, selectedInstance_, true, {});
        if (requestedFullKeys_.remove(read.key)) {
          acceptSnapshot(read.key, snapshot, true);
          emit historyChanged(account_, epoch_);
          emit snapshotDetailsChanged(account_, epoch_, read.key, fullCache_.contains(read.key), {});
        }
      } else {
        const bool explicitlyWanted = requestedFullKeys_.remove(read.key) || read.kind == ReadKind::Full;
        acceptSnapshot(read.key, std::move(snapshot), explicitlyWanted);
        emit historyChanged(account_, epoch_);
        if (explicitlyWanted) emit snapshotDetailsChanged(account_, epoch_, read.key, fullCache_.contains(read.key),
            fullCache_.contains(read.key) ? QString{} : QStringLiteral("完整快照超过缓存预算"));
      }
    } else if (read.kind == ReadKind::Instance) {
      const auto summary = std::find_if(summaries_.cbegin(), summaries_.cend(), [&](const auto& value) { return value.storageKey == read.key; });
      const bool alreadyPresent = std::any_of(instanceHistory_.cbegin(), instanceHistory_.cend(), [&](const auto& value) { return value.storageKey == read.key; });
      if (!alreadyPresent && summary != summaries_.cend()) {
        SnapshotInstanceHistoryEntry unknown;
        unknown.storageKey = read.key;
        unknown.createdAt = summary->createdAt;
        unknown.schemaVersion = summary->schemaVersion;
        unknown.analysisVersion = summary->analysisVersion;
        putInstanceEntry(&instanceHistory_, unknown);
      }
      instanceError_ = QStringLiteral("部分日期的个体历史无法确认，保留未知状态");
      emit instanceHistoryChanged(account_, epoch_, selectedInstance_, true, instanceError_);
      if (requestedFullKeys_.remove(read.key))
        emit snapshotDetailsChanged(account_, epoch_, read.key, false, instanceError_);
    } else if (requestedFullKeys_.remove(read.key) || read.kind == ReadKind::Full) {
      emit snapshotDetailsChanged(account_, epoch_, read.key, false,
          versionMatches ? QStringLiteral("快照读取或结构校验失败") : QStringLiteral("该记录已有更新版本"));
    } else if (!valid && result.status != StorageStatus::NotFound && result.status != StorageStatus::Cancelled) {
      historyError_ = QStringLiteral("部分历史文件无效或不可读，原文件保持不变");
      emit historyLoadingChanged(account_, epoch_, historyLoading_, historyError_);
      if (read.kind == ReadKind::Instance) instanceError_ = historyError_;
    }
  }
  finishLoadingIfDone();
  pumpTimer_.start(0);
}

void AssetSnapshotStore::finishLoadingIfDone() {
  bool historyPending = scanWanted_ || scanInFlight_ || !historyNames_.isEmpty();
  bool instancePending = instanceWaitingForHistory_ || !instanceKeys_.isEmpty();
  for (auto read = pendingReads_.cbegin(); read != pendingReads_.cend(); ++read) if (readCurrent(read.value())) {
    historyPending |= read.value().kind == ReadKind::Scan || read.value().kind == ReadKind::Summary;
    instancePending |= read.value().kind == ReadKind::Instance;
  }
  for (const PendingRead& read : queuedReads_) if (readCurrent(read)) {
    historyPending |= read.kind == ReadKind::Scan || read.kind == ReadKind::Summary;
    instancePending |= read.kind == ReadKind::Instance;
  }
  if (historyLoading_ && !historyPending) {
    historyLoading_ = false;
    emit historyLoadingChanged(account_, epoch_, false, historyError_);
    if (instanceWaitingForHistory_) { startInstanceReads(); pumpTimer_.start(0); instancePending = !instanceKeys_.isEmpty(); }
  }
  if (instanceLoading_ && !instancePending) {
    instanceLoading_ = false;
    emit instanceHistoryChanged(account_, epoch_, selectedInstance_, false, instanceError_);
  }
}
