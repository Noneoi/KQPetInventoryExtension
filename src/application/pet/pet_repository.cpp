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
  // namespace

PetRepository::PetRepository(QObject* parent, StorageService* storage,
                             const QString& legacyDataRoot, const PetRecordCacheLimits& rawLimits)
    : InventoryReadView(parent), rawRecords_(std::make_unique<PetRecordCache>(rawLimits)) {
  qRegisterMetaType<RawPetRecordHandle>(); qRegisterMetaType<PetRecordKey>();
  rawCacheClock_.start();
  const QString overrideRoot = qEnvironmentVariable("KQPET_DATA_ROOT");
  cacheRoot_ = overrideRoot.isEmpty()
                   ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("KQPetData"))
                   : overrideRoot;
  if (storage) cacheRoot_ = storage->dataRoot();
  storage_ = storage ? storage : new StorageService(cacheRoot_, {}, {}, this);
  sharedStorageContext_ = storage_->createSharedContext();
  legacyDataRoot_ = !legacyDataRoot.isEmpty() ? legacyDataRoot
      : !overrideRoot.isEmpty() ? QDir(cacheRoot_).filePath(QStringLiteral("legacy-import"))
      : QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("KQPetInventory"));
  connect(storage_, &StorageService::completed, this, &PetRepository::handleStorageResult);
  cachePumpTimer_ = new QTimer(this);
  cachePumpTimer_->setSingleShot(true);
  connect(cachePumpTimer_, &QTimer::timeout, this, &PetRepository::pumpCacheReads);
  setAccountPaths();
  loadCache();
  migrateLegacyCache();
}

PetRepository::~PetRepository() {
  cachePumpTimer_->stop();
  if (!storage_) return;
  storage_->cancelReads(storageContext_);
  storage_->cancelReads(sharedStorageContext_);
  storage_->cancelReads(legacyReadContext_);
  if (!detailScanCursor_.isEmpty()) storage_->cancelScan(detailScanCursor_);
}

StorageService* PetRepository::storageService() const { return storage_.data(); }

int PetRepository::pendingReadCount() const {
  return static_cast<int>(pendingReads_.size() + queuedReads_.size() + pendingDetailNames_.size()) +
         (detailScanWanted_ && !detailScanInFlight_ ? 1 : 0);
}

bool PetRepository::cacheLoading() const {
  return pendingReadCount() != 0 || migrationActive_;
}

void PetRepository::publishCacheLoading(const QString& error) {
  const bool loading = cacheLoading();
  if (loading != lastPublishedCacheLoading_ || !error.isEmpty()) {
    lastPublishedCacheLoading_ = loading;
    emit cacheLoadingChanged(accountKey_, loading, error);
  }
}

qint64 PetRepository::petId(const QJsonObject& pet) {
  qint64 id = 0;
  return PacketContracts::checkedInteger(pet.value(QStringLiteral("id")), &id, 1) ? id : 0;
}
QJsonObject PetRepository::merge(const QJsonObject& base, const QJsonObject& overlay) {
  QJsonObject result = base;
  for (auto iterator = overlay.begin(); iterator != overlay.end(); ++iterator)
    result.insert(iterator.key(), iterator.value());
  return result;
}

QJsonObject PetRepository::warehouseBriefForCache(const QJsonObject& pet) {
  static const QStringList fields = {
      QStringLiteral("id"), QStringLiteral("r"), QStringLiteral("ri"),
      QStringLiteral("n"), QStringLiteral("customName"), QStringLiteral("lv"),
      QStringLiteral("fr"), QStringLiteral("g"), QStringLiteral("gd"),
      QStringLiteral("pr"), QStringLiteral("sdpi"), QStringLiteral("srpi"),
      QStringLiteral("srri"), QStringLiteral("sepi"), QStringLiteral("cepi"),
      QStringLiteral("cps"), QStringLiteral("lss"), QStringLiteral("badge"),
      QStringLiteral("_location"), QStringLiteral("_warehouseGroup"),
      QStringLiteral("_position"), QStringLiteral("_visualMismatch"), QStringLiteral("_unverifiedObservation"),
      QStringLiteral("_metaOriginalName"), QStringLiteral("_metaAttributes"),
      QStringLiteral("_metaJobs"), QStringLiteral("_metaEra"),
      QStringLiteral("_metaRaceId")};
  QJsonObject brief;
  for (const QString& field : fields)
    if (pet.contains(field)) brief.insert(field, pet.value(field));
  return brief;
}

bool PetRepository::visualIdentityDiffers(const QJsonObject& detail,
                                          const QJsonObject& brief) {
  const int oldRace = petRaceId(detail);
  const int newRace = petRaceId(brief);
  if (oldRace > 0 && newRace > 0 && oldRace != newRace) return true;
  const int oldFace = petFaceId(detail);
  const int newFace = petFaceId(brief);
  if (oldFace > 0 && newFace > 0 && oldFace != newFace) return true;
  const QString oldName = petProtocolName(detail);
  const QString newName = petProtocolName(brief);
  return !oldName.isEmpty() && !newName.isEmpty() && oldName != newName;
}

// Only a stable, reproducible order is needed here: every view re-sorts by its
// own key, so a locale-aware name collation over the whole account would be
// thrown away on each publication.
QList<QJsonObject> PetRepository::sorted(const QHash<qint64, QJsonObject>& source) const {
  QList<QJsonObject> values = source.values();
  std::sort(values.begin(), values.end(), [](const QJsonObject& left, const QJsonObject& right) {
    return petId(left) < petId(right);
  });
  return values;
}

QList<QJsonObject> PetRepository::backpackPets() const {
  QList<QJsonObject> pets = sorted(backpack_);
  for (QJsonObject& pet : pets) pet = detailFor(petId(pet));
  return pets;
}
QList<QJsonObject> PetRepository::warehousePets() const {
  QList<QJsonObject> pets = sorted(warehouse_);
  for (auto& pet : pets) pet = detailFor(petId(pet));
  return pets;
}
QList<QJsonObject> PetRepository::backpackBriefs() const {
  auto pets = sorted(backpack_); for (auto& pet : pets) pet = withDeploymentState(pet); return pets;
}
QList<QJsonObject> PetRepository::warehouseBriefs() const { return sorted(warehouse_); }
QList<qint64> PetRepository::currentInstanceIds() const {
  QSet<qint64> ids;
  ids.reserve(backpack_.size() + warehouse_.size());
  for (auto it = backpack_.cbegin(); it != backpack_.cend(); ++it) ids.insert(it.key());
  for (auto it = warehouse_.cbegin(); it != warehouse_.cend(); ++it) ids.insert(it.key());
  return ids.values();
}

QJsonObject PetRepository::backpackPet(qint64 instanceId) const {
  return backpack_.contains(instanceId) ? detailFor(instanceId) : QJsonObject{};
}

bool PetRepository::isDeployed(qint64 instanceId) const {
  return formationKnown_ && deployedPetIds_.contains(instanceId);
}

QList<qint64> PetRepository::backpackIds(int packType) const {
  if (packSequences_.contains(packType)) {
    QList<qint64> ids;
    for (const QString& value : packSequences_.value(packType)) {
      const qint64 id = value.toLongLong();
      if (id > 0) ids.append(id);
    }
    return ids;
  }
  QList<QPair<int, qint64>> positioned;
  for (auto iterator = backpack_.cbegin(); iterator != backpack_.cend(); ++iterator) {
    const QJsonObject& pet = iterator.value();
    if (pet.value(QStringLiteral("_packType")).toInt(-1) != packType) continue;
    positioned.append({pet.value(QStringLiteral("_position")).toInt(INT_MAX),
                       iterator.key()});
  }
  std::sort(positioned.begin(), positioned.end());
  QList<qint64> ids;
  for (const auto& item : positioned) ids.append(item.second);
  return ids;
}

int PetRepository::backpackCapacity(int packType) const {
  return packCapacities_.value(packType, 0);
}

quint64 PetRepository::preserveBackpackDetailAsync(qint64 instanceId) {
  if (!listObservationsAuthoritativeForWrite()) return 0;
  const auto raw = rawRecordHandle(instanceId);
  // A read-continuity move preserves the record just observed in this stream;
  // the saved file still records it as a read-only observation.
  if (!backpack_.contains(instanceId) || !raw || !raw->payload ||
      (!raw->sourceKnown && !readContinuityWriteAllowed())) return 0;
  // Preserve the last actual original, not a synthesized combination of a
  // partial current list with older cultivation fields.
  QJsonObject pet = raw->object();
  if (!PacketContracts::normalizePet(&pet, true)) return 0;
  return saveDetail(instanceId, pet, QDateTime::currentDateTime(), 0, true);
}

QJsonObject PetRepository::mergeRecordView(const RawPetRecordHandle& raw, qint64 instanceId) const {
  const QJsonObject brief = recordBrief(instanceId);
  if (!raw) return brief;
  QJsonObject result = merge(raw->object(), brief);
  copyPowerFields(raw->object(), &result);
  return backpack_.contains(instanceId) ? withDeploymentState(result) : result;
}
QJsonObject PetRepository::mergedRecordView(qint64 instanceId) const {
  return mergeRecordView(rawRecords_->acquire(accountKey_, sessionGeneration_, instanceId), instanceId);
}
QJsonObject PetRepository::detailFor(qint64 instanceId) const {
  const auto raw = rawRecords_->acquire(accountKey_, sessionGeneration_, instanceId);
  if (raw) rawRecords_->noteUntrackedExport();
  return mergeRecordView(raw, instanceId);
}

QJsonObject PetRepository::warehousePet(qint64 instanceId) const {
  return warehouse_.contains(instanceId) ? detailFor(instanceId) : QJsonObject{};
}

bool PetRepository::hasCachedDetail(qint64 instanceId) const {
  const auto state = detailStates_.value(instanceId);
  return state.complete || state.persisted;
}

bool PetRepository::isDetailPersisted(qint64 instanceId) const {
  return hasCachedDetail(instanceId) && detailMemoryRevision(instanceId) != 0 &&
         savedMemoryRevisions_.value(instanceId) == detailMemoryRevision(instanceId);
}

QDateTime PetRepository::detailSavedAt(qint64 instanceId) const {
  return detailSavedTimes_.value(instanceId);
}

QList<qint64> PetRepository::warehouseIdsByDetailAge() const {
  QList<qint64> ids = warehouse_.keys();
  std::sort(ids.begin(), ids.end(), [this](qint64 left, qint64 right) {
    const bool leftMissing = !hasCachedDetail(left);
    const bool rightMissing = !hasCachedDetail(right);
    if (leftMissing != rightMissing) return leftMissing;
    const QDateTime leftTime = detailSavedTimes_.value(left);
    const QDateTime rightTime = detailSavedTimes_.value(right);
    if (leftTime != rightTime)
      return !leftTime.isValid() || (rightTime.isValid() && leftTime < rightTime);
    return left < right;
  });
  return ids;
}

QJsonObject PetRepository::inventoryBrief(const QJsonObject& pet, bool backpack) {
  static const QStringList fields{
      "id", "r", "ri", "n", "customName", "lv", "fr", "g", "gd", "pr", "rt",
      "sdpi", "srpi", "srri", "sepi", "cepi", "zdl", "xzdl", "_location", "_warehouseGroup",
      "_position", "_packType", "_visualMismatch", "_metaOriginalName", "_metaAttributes",
      "_metaJobs", "_metaEra", "_metaRaceId", "_unverifiedObservation", "stargodSlotMaxLevel"};
  QJsonObject brief;
  for (const auto& field : fields) {
    const auto value = pet.value(field);
    if (!value.isUndefined() && !value.isArray() && !value.isObject()) brief.insert(field, value);
  }
  for (const QString& field : {QStringLiteral("isRentPet"), QStringLiteral("_inFormation"),
       QStringLiteral("inFormation"), QStringLiteral("isInFormation"), QStringLiteral("inTeam"),
       QStringLiteral("isTeamPet"), QStringLiteral("isFollowPet")}) {
    auto value = pet.value(field);
    if (value.isArray()) value = !value.toArray().isEmpty();
    else if (value.isObject()) value = !value.toObject().isEmpty();
    if (!value.isUndefined()) brief.insert(field, value);
  }
  if (backpack) brief.insert(QStringLiteral("_location"), QStringLiteral("backpack"));
  return brief;
}
bool PetRepository::calculationOverlayDiffers(const QJsonObject& before, const QJsonObject& after) {
  if (petRaceId(before) != petRaceId(after)) return true;
  for (const QString& field : {QStringLiteral("fr"),
       QStringLiteral("n"), QStringLiteral("lv"), QStringLiteral("zdl"), QStringLiteral("xzdl"),
       QStringLiteral("_metaRaceId"), QStringLiteral("_visualMismatch"), QStringLiteral("_unverifiedObservation"),
       QStringLiteral("stargodSlotMaxLevel")})
    if (before.value(field) != after.value(field)) return true;
  return false;
}
bool PetRepository::calculationProjectionMatchesRaw(const QJsonObject& raw, const QJsonObject& brief) {
  if (brief.value(QStringLiteral("_visualMismatch")).toBool()) return false;
  const int observedRace = petRaceId(brief), rawRace = petRaceId(raw);
  if (observedRace > 0 && observedRace != rawRace) return false;
  const auto sameNumber = [](const QJsonValue& left, const QJsonValue& right) {
    qint64 a = 0, b = 0;
    return PacketContracts::checkedInteger(left, &a) && PacketContracts::checkedInteger(right, &b) && a == b;
  };
  for (const QString& field : {QStringLiteral("lv"), QStringLiteral("zdl"), QStringLiteral("xzdl"),
                               QStringLiteral("stargodSlotMaxLevel")})
    if (brief.contains(field) && !sameNumber(brief.value(field), raw.value(field))) return false;
  const int rawMetadataRace = raw.value(QStringLiteral("_metaRaceId")).toInt(rawRace);
  const int observedMetadataRace = brief.value(QStringLiteral("_metaRaceId")).toInt(rawMetadataRace);
  if (observedMetadataRace != rawMetadataRace) return false;
  // Display/position names and r-vs-ri representation do not change pure
  // cultivation. Any future non-scalar calculation overlay must be checked
  // here before it is added to the compact summary contract.
  return true;
}
QJsonObject PetRepository::recordBrief(qint64 id) const {
  return backpack_.contains(id) ? withDeploymentState(backpack_.value(id)) : warehouse_.value(id);
}
RawPetRecordHandle PetRepository::rawRecordHandle(qint64 id) const {
  if (const auto raw = rawRecords_->acquire(accountKey_, sessionGeneration_, id)) return raw;
  const auto version = detailStates_.constFind(id);
  if (version == detailStates_.cend() || version->complete) return {};
  auto summary = std::make_shared<RawPetRecord>();
  summary->key = version->key; summary->sourceKnown = version->sourceKnown;
  summary->brief = recordBrief(id); summary->observedAt = version->observedAt;
  return summary;
}
bool PetRepository::rawRecordResident(qint64 id) const { return rawRecords_->contains(accountKey_, sessionGeneration_, id); }
quint64 PetRepository::detailMemoryRevision(qint64 id) const { return detailStates_.value(id).key.detailMemoryRevision; }
PetRecordVersion PetRepository::recordVersion(qint64 id) const {
  auto version = detailStates_.value(id); version.resident = rawRecordResident(id); return version;
}
PetRecordCacheStats PetRepository::rawCacheStats() const { return rawRecords_->stats(); }
bool PetRepository::markRecordDerived(const PetRecordKey& key) {
  auto found = detailStates_.find(key.instanceId);
  if (found == detailStates_.end() || found->key != key) return false;
  found->derived = true; rawRecords_->markDerived(key); rawReadRetryAfter_ = 0;
  cachePumpTimer_->start(0); return true;
}
void PetRepository::announceRaw(qint64 id) {
  if (const auto raw = rawRecordHandle(id)) emit rawRecordAvailable(raw);
}
bool PetRepository::admitRawRecords(const QList<RawPetRecordInput>& inputs, bool network) {
  const auto account = accountKey_; const auto epoch = sessionGeneration_;
  for (const auto& input : inputs)
    if (input.key.account != account || input.key.epoch != epoch) return false;
  auto admission = rawRecords_->reserve(inputs);
  for (const auto& key : admission.evicted) emit rawRecordEvicted(key.account, key.epoch, key.instanceId, key.detailMemoryRevision);
  if (account != accountKey_ || epoch != sessionGeneration_) return false;
  if (!admission.accepted) {
    emit rawCachePressure(admission.error);
    if (network && account == accountKey_ && epoch == sessionGeneration_)
      markSessionUncertain(QStringLiteral("原始详情缓存容量不足，未接受不完整的新事实：%1").arg(admission.error));
    return false;
  }
  rawRecords_->commit(admission.records);
  for (const auto& raw : admission.records) {
    PetRecordVersion state;
    state.key = raw->key; state.contentDigest = raw->contentDigest; state.complete = raw->complete; state.brief = raw->brief;
    state.persisted = raw->persisted; state.resident = true; state.sourceKnown = raw->sourceKnown;
    state.rawProjectionOnly = raw->rawProjectionOnly; state.observedAt = raw->observedAt;
    detailStates_.insert(raw->key.instanceId, state);
    detailIdentities_.insert(raw->key.instanceId, inventoryBrief(raw->object(), false));
  }
  return true;
}
void PetRepository::reviseRawBrief(qint64 id, const QJsonObject& brief, bool sourceKnown) {
  auto found = detailStates_.find(id);
  if (found == detailStates_.end()) {
    PetRecordVersion value; value.key = {accountKey_, sessionGeneration_, id, ++nextDetailMemoryRevision_};
    value.sourceKnown = sourceKnown; value.brief = brief; value.observedAt = QDateTime::currentDateTimeUtc(); detailStates_.insert(id, value);
    return;
  }
  const auto current = rawRecords_->acquire(accountKey_, sessionGeneration_, id);
  const QJsonObject previous = found->brief;
  PetRecordKey next = found->key;
  if (calculationOverlayDiffers(previous, brief) || found->sourceKnown != sourceKnown) {
    next.detailMemoryRevision = ++nextDetailMemoryRevision_; found->derived = false;
  }
  bool rawOnly = found->rawProjectionOnly && !calculationOverlayDiffers(previous, brief);
  if (current) {
    rawOnly = calculationProjectionMatchesRaw(current->object(), brief);
    rawRecords_->revise(found->key, next, brief, sourceKnown, rawOnly);
  }
  found->key = next; found->sourceKnown = sourceKnown; found->rawProjectionOnly = rawOnly; found->brief = brief;
}
