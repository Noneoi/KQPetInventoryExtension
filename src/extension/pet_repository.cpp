#include "pet_repository.h"

#include "diagnostic_logger.h"
#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "packet_contract.h"
#include "storage_service.h"

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

namespace {

QString displayName(const QJsonObject& pet) {
  const QString custom = pet.value(QStringLiteral("customName")).toString();
  if (!custom.isEmpty()) return custom;
  const QString name = pet.value(QStringLiteral("n")).toString();
  if (!name.isEmpty()) return name;
  return QStringLiteral("精灵 %1").arg(petRaceId(pet));
}

QString safeAccountName(const QString& account) {
  static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]+$"));
  if (safe.match(account).hasMatch()) return account;
  return QString::fromLatin1(
             QCryptographicHash::hash(account.toUtf8(), QCryptographicHash::Sha256).toHex())
      .left(24);
}

void copyPowerFields(const QJsonObject& source, QJsonObject* destination) {
  static const QStringList fields = {
      QStringLiteral("zdl"), QStringLiteral("xzdl"), QStringLiteral("czdlv"),
      QStringLiteral("mzdlv"), QStringLiteral("czdlvs"), QStringLiteral("mzdlvs")};
  for (const QString& field : fields) {
    destination->remove(field);
    if (source.contains(field)) destination->insert(field, source.value(field));
  }
}

bool strictFormation(const QJsonObject& formation, int* id, int* plan, QString* positions) {
  qint64 checkedId = 0;
  qint64 checkedPlan = 0;
  if (!PacketContracts::checkedInteger(formation.value(QStringLiteral("id")), &checkedId, 1, INT_MAX) ||
      !PacketContracts::checkedInteger(formation.value(QStringLiteral("p")), &checkedPlan, 0, INT_MAX) ||
      !formation.value(QStringLiteral("ps")).isString()) return false;
  const QString encoded = formation.value(QStringLiteral("ps")).toString();
  QSet<qint64> seen;
  if (!encoded.isEmpty()) for (const QString& part : encoded.split(QLatin1Char('#'), Qt::KeepEmptyParts)) {
    qint64 instance = 0;
    if (!PacketContracts::checkedInteger(part, &instance, -1)) return false;
    if (instance > 0) {
      if (seen.contains(instance)) return false;
      seen.insert(instance);
    }
  }
  *id = static_cast<int>(checkedId);
  *plan = static_cast<int>(checkedPlan);
  *positions = encoded;
  return true;
}

bool optionalSuccess(const QJsonObject& packet) {
  if (!packet.contains(QStringLiteral("r"))) return true;
  qint64 status = 0;
  return PacketContracts::checkedInteger(packet.value(QStringLiteral("r")), &status) && status == 1;
}

QString observedLoginAccount(const QJsonObject& packet) {
  const QJsonValue value = packet.value(QStringLiteral("info")).toObject().value(QStringLiteral("n"));
  if (value.isString()) return value.toString();
  qint64 integer = 0;
  return PacketContracts::checkedInteger(value, &integer, 0) ? QString::number(integer) : QString{};
}

}  // namespace

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

QList<QJsonObject> PetRepository::sorted(const QHash<qint64, QJsonObject>& source) const {
  QList<QJsonObject> values = source.values();
  std::sort(values.begin(), values.end(), [](const QJsonObject& left, const QJsonObject& right) {
    const int nameOrder = QString::localeAwareCompare(displayName(left), displayName(right));
    return nameOrder == 0 ? petId(left) < petId(right) : nameOrder < 0;
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

bool PetRepository::preserveBackpackDetail(qint64 instanceId) {
  // A bool cannot wait for asynchronous durable storage. Keep this legacy
  // query truthful; movement callers must use preserveBackpackDetailAsync.
  return listObservationsAuthoritativeForWrite() && isDetailPersisted(instanceId) &&
         rawRecordResident(instanceId);
}

quint64 PetRepository::preserveBackpackDetailAsync(qint64 instanceId) {
  if (!listObservationsAuthoritativeForWrite()) return 0;
  const auto raw = rawRecordHandle(instanceId);
  if (!backpack_.contains(instanceId) || !raw || !raw->payload || !raw->sourceKnown) return 0;
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

void PetRepository::beginListRefresh(quint64 requestGeneration, const QString& account,
                                     quint64 sessionGeneration) {
  const RequestExpectation expectation{account, sessionGeneration, requestGeneration, 0, true};
  backpackExpectation_ = expectation;
  backpackExpectation_.minimumReceiveSequence = lastInboundSequence_;
  warehouseExpectation_ = {};
  backpackObservation_.ordered = false;
  for (auto it = warehouseObservations_.begin(); it != warehouseObservations_.end(); ++it)
    it.value().ordered = false;
}

void PetRepository::expectListPart(const QString& command, quint64 requestGeneration,
                                   const QString& account, quint64 sessionGeneration) {
  const RequestExpectation expectation{account, sessionGeneration, requestGeneration, 0, true};
  if (command == QStringLiteral("2_1_10")) {
    backpackExpectation_ = expectation;
    backpackExpectation_.minimumReceiveSequence = lastInboundSequence_;
  }
  if (command == QStringLiteral("2_1_S")) {
    warehouseExpectation_ = expectation;
    warehouseExpectation_.minimumReceiveSequence = lastInboundSequence_;
  }
}

void PetRepository::cancelListPart(const QString& command, quint64 requestGeneration) {
  RequestExpectation* expectation = command == QStringLiteral("2_1_10")
                                        ? &backpackExpectation_
                                        : command == QStringLiteral("2_1_S")
                                              ? &warehouseExpectation_ : nullptr;
  if (expectation && expectation->requestGeneration == requestGeneration)
    expectation->active = false;
}

void PetRepository::expectDetail(qint64 instanceId, quint64 requestGeneration,
                                 const QString& account, quint64 sessionGeneration) {
  detailExpectation_ = {account, sessionGeneration, requestGeneration, instanceId,
                        instanceId > 0};
  detailExpectation_.minimumReceiveSequence = lastInboundSequence_;
}

void PetRepository::cancelDetailRequest(qint64 instanceId, quint64 requestGeneration) {
  if (detailExpectation_.instanceId == instanceId &&
      detailExpectation_.requestGeneration == requestGeneration)
    detailExpectation_.active = false;
}

void PetRepository::expectSequenceUpdate(quint64 requestGeneration,
                                         const QString& account,
                                         quint64 sessionGeneration) {
  sequenceExpectation_ = {account, sessionGeneration, requestGeneration, 0, true};
  sequenceExpectation_.minimumReceiveSequence = lastInboundSequence_;
}

void PetRepository::cancelSequenceUpdate(quint64 requestGeneration) {
  if (sequenceExpectation_.requestGeneration == requestGeneration)
    sequenceExpectation_.active = false;
}

bool PetRepository::expectationMatches(const RequestExpectation& expectation) const {
  return expectation.active && authenticated_ && expectation.account == accountKey_ &&
         expectation.sessionGeneration == sessionGeneration_ &&
         (!processingEnvelope_ || currentEnvelope_.receiveSequence > expectation.minimumReceiveSequence);
}

bool PetRepository::parseBackpack(const QJsonObject& packet) {
  const DecodedBackpack candidate = PacketContracts::decodeBackpack(packet);
  if (!candidate.valid) {
    emit packetRejected(QStringLiteral("2_1_10"), candidate.error);
    return false;
  }
  QHash<qint64, QPair<int, int>> positions;
  for (auto it = candidate.sequences.begin(); it != candidate.sequences.end(); ++it)
    for (int position = 0; position < it.value().size(); ++position)
      positions.insert(it.value().at(position).toLongLong(), qMakePair(it.key(), position));
  QHash<qint64, QJsonObject> replacement;
  QList<RawPetRecordInput> rawInputs;
  QList<qint64> retainedComplete;
  for (QJsonObject pet : candidate.list.pets) {
    const qint64 id = petId(pet);
    const QJsonObject previous = backpack_.contains(id) ? backpack_.value(id) : detailIdentities_.value(id);
    pet = PetDetailCatalog::instance().enrichMetadata(pet, previous);
    pet.insert(QStringLiteral("_location"), QStringLiteral("backpack"));
    pet.insert(QStringLiteral("_unverifiedObservation"), !currentPacketCanPersist());
    if (positions.contains(id)) {
      pet.insert(QStringLiteral("_packType"), positions.value(id).first);
      pet.insert(QStringLiteral("_position"), positions.value(id).second);
    }
    QJsonObject brief = inventoryBrief(pet, true);
    replacement.insert(id, brief);
    QJsonObject checked = pet;
    const bool complete = pet.value(QStringLiteral("czdlv")).isObject() && pet.value(QStringLiteral("mzdlv")).isObject();
    if (!complete && detailStates_.value(id).complete) {
      if (visualIdentityDiffers(detailIdentities_.value(id), brief)) {
        brief.insert(QStringLiteral("_visualMismatch"), true);
        replacement.insert(id, brief);
      }
      const auto old = rawRecords_->acquire(accountKey_, sessionGeneration_, id);
      for (auto field = pet.begin(); field != pet.end(); ++field) {
        if (brief.contains(field.key()) && brief.value(field.key()) == field.value()) continue;
        if (!old || old->object().value(field.key()) != field.value()) {
          const QString reason = QStringLiteral("部分背包观察含无法无损合并的新原文字段，保留旧完整详情并等待完整观察");
          emit packetRejected(QStringLiteral("2_1_10"), reason);
          markSessionUncertain(reason); return false;
        }
      }
      retainedComplete.append(id); continue;
    }
    RawPetRecordInput raw;
    raw.key = {accountKey_, sessionGeneration_, id, ++nextDetailMemoryRevision_};
    raw.object = pet; raw.brief = brief; raw.complete = complete && PacketContracts::normalizePet(&checked, true);
    raw.sourceKnown = currentPacketCanPersist(); raw.observedAt = QDateTime::currentDateTimeUtc();
    rawInputs.append(std::move(raw));
  }
  // No mutable state is touched until list, sequence and capacities pass.
  if (!admitRawRecords(rawInputs, true)) return false;
  backpack_ = replacement;
  for (qint64 id : retainedComplete)
    reviseRawBrief(id, backpack_.value(id), currentPacketCanPersist() && detailStates_.value(id).sourceKnown);
  packSequences_ = candidate.sequences;
  // Missing capacity is unknown for this observation, never inherited as a
  // fresh write-preflight fact from an older server snapshot.
  packCapacities_ = candidate.capacities;
  ++inventoryRevision_;
  ++listRevision_;
  backpackObservation_ = currentObservation(candidate.sequencesPresent &&
                                            candidate.capacitiesPresent &&
                                            candidate.capacities.contains(0));
  const auto account = accountKey_; const auto epoch = sessionGeneration_;
  for (const auto& input : rawInputs) {
    QJsonObject normalized = input.object;
    if (canCacheAccountObservation() && PacketContracts::normalizePet(&normalized, true))
      saveDetail(input.key.instanceId, input.object, input.observedAt);
    if (account != accountKey_ || epoch != sessionGeneration_) return false;
    announceRaw(input.key.instanceId);
    if (account != accountKey_ || epoch != sessionGeneration_) return false;
  }
  for (qint64 id : retainedComplete) {
    announceRaw(id);
    if (account != accountKey_ || epoch != sessionGeneration_) return false;
  }
  return true;
}

bool PetRepository::parseWarehouse(const QJsonObject& packet) {
  struct Group { const char* key; const char* name; };
  constexpr Group groups[] = {{"ns", "normal"}, {"rb", "goodbye"}, {"es", "elite"}};
  QHash<QString, DecodedPetList> candidates;
  QSet<qint64> incomingIds;
  bool invalid = false;
  for (const Group& group : groups) {
    const QString key = QString::fromLatin1(group.key);
    if (!packet.contains(key)) continue;
    DecodedPetList candidate = PacketContracts::decodePetList(packet.value(key));
    if (!candidate.valid()) {
      invalid = true;
      warehouseObservations_[QString::fromLatin1(group.name)].ordered = false;
      emit packetRejected(QStringLiteral("2_1_S"), key + QStringLiteral(": ") + candidate.error);
      continue;
    }
    for (const QJsonObject& pet : candidate.pets) {
      const qint64 id = petId(pet);
      if (incomingIds.contains(id)) {
        emit packetRejected(QStringLiteral("2_1_S"), QStringLiteral("duplicate instance across groups"));
        return false;
      }
      incomingIds.insert(id);
    }
    candidates.insert(QString::fromLatin1(group.name), candidate);
  }
  if (candidates.isEmpty()) return false;
  QHash<qint64, QJsonObject> replacement = candidates.size() == 3
                                             ? QHash<qint64, QJsonObject>{} : warehouse_;
  for (auto it = replacement.begin(); it != replacement.end();) {
    const QString oldGroup = it.value().value(QStringLiteral("_warehouseGroup")).toString();
    if (candidates.contains(oldGroup)) it = replacement.erase(it);
    else ++it;
  }
  QList<qint64> mismatches;
  for (auto candidate = candidates.begin(); candidate != candidates.end(); ++candidate) {
    int position = 0;
    for (QJsonObject brief : candidate.value().pets) {
      const qint64 id = petId(brief);
      // A retained omitted/invalid group containing this instance makes the
      // partial packet ambiguous; do not silently delete either observation.
      if (replacement.contains(id)) {
        emit packetRejected(QStringLiteral("2_1_S"), QStringLiteral("instance conflicts with retained group"));
        return false;
      }
      const QJsonObject previous = hasCachedDetail(id) ? detailIdentities_.value(id) : warehouse_.value(id);
      brief = PetDetailCatalog::instance().enrichMetadata(brief, previous);
      brief.insert(QStringLiteral("_location"), QStringLiteral("warehouse"));
      brief.insert(QStringLiteral("_unverifiedObservation"), !currentPacketCanPersist());
      brief.insert(QStringLiteral("_warehouseGroup"), candidate.key());
      brief.insert(QStringLiteral("_position"), position++);
      if (hasCachedDetail(id) && visualIdentityDiffers(detailIdentities_.value(id), brief)) {
        brief.insert(QStringLiteral("_visualMismatch"), true);
        mismatches.append(id);
      }
      copyPowerFields(detailIdentities_.value(id), &brief);
      replacement.insert(id, inventoryBrief(brief, false));
    }
  }
  warehouse_ = replacement;
  for (const auto id : incomingIds) {
    const auto prior = detailStates_.value(id);
    reviseRawBrief(id, warehouse_.value(id), currentPacketCanPersist() && (!prior.complete || prior.sourceKnown));
  }
  ++inventoryRevision_;
  ++listRevision_;
  for (auto candidate = candidates.begin(); candidate != candidates.end(); ++candidate)
    warehouseObservations_.insert(candidate.key(), currentObservation(true));
  if (invalid || candidates.size() != 3) {
    // Partial read observations stay useful, but cannot finish a complete
    // write preflight or a post-write authoritative verification.
    for (auto it = warehouseObservations_.begin(); it != warehouseObservations_.end(); ++it)
      it.value().ordered = false;
  }
  for (qint64 id : mismatches) emit visualMismatchDetected(id);
  const auto account = accountKey_; const auto epoch = sessionGeneration_;
  for (qint64 id : incomingIds) {
    announceRaw(id);
    if (account != accountKey_ || epoch != sessionGeneration_) break;
  }
  return true;
}

bool PetRepository::parseDetail(const QJsonObject& packet, quint64 requestGeneration) {
  const auto rejectMalformed = [this, requestGeneration, &packet](const QString& reason) {
    const qint64 expectedId = detailExpectation_.instanceId;
    detailExpectation_.active = false;
    DiagnosticLogger::event({QStringLiteral("detail_response_invalid"), QStringLiteral("response"),
        requestGeneration, QStringLiteral("decode"),
        QStringLiteral("expectedInstance=%1 pType=%2 idType=%3; %4").arg(expectedId)
            .arg(static_cast<int>(packet.value(QStringLiteral("p")).type()))
            .arg(static_cast<int>(packet.value(QStringLiteral("p")).toObject().value(QStringLiteral("id")).type()))
            .arg(reason),
        QStringLiteral("inspect detail response fields")});
    emit detailResponseRejected(expectedId, requestGeneration, reason);
    return false;
  };
  if (!packet.value(QStringLiteral("p")).isObject())
    return rejectMalformed(QStringLiteral("详情返回的数据对象（p）缺失或类型无效"));
  QJsonObject detail = packet.value(QStringLiteral("p")).toObject();
  const qint64 id = petId(detail);
  if (id <= 0)
    return rejectMalformed(QStringLiteral("详情返回的实例 ID 缺失或无效"));
  if (id != detailExpectation_.instanceId) {
    DiagnosticLogger::event({QStringLiteral("detail_response_other_instance"), QStringLiteral("response"),
        requestGeneration, QStringLiteral("match"),
        QStringLiteral("expectedInstance=%1 actualInstance=%2; continuing to wait for the requested instance")
            .arg(detailExpectation_.instanceId).arg(id), QStringLiteral("retain current request")});
    return false;
  }
  detailExpectation_.active = false;
  if (!PacketContracts::normalizePet(&detail, true)) {
    emit detailResponseRejected(id, requestGeneration, QStringLiteral("详情缺少实例ID、种族ID或等级字段"));
    return false;
  }
  const bool inBackpack = backpack_.contains(id), inWarehouse = warehouse_.contains(id);
  if (!inBackpack && !inWarehouse) {
    emit detailResponseRejected(id, requestGeneration, QStringLiteral("该实例已不在当前背包或仓库列表"));
    return false;
  }
  const auto account = accountKey_; const auto epoch = sessionGeneration_;
  const bool mayPersist = currentPacketCanPersist();
  detail = PetDetailCatalog::instance().enrichMetadata(detail, detailIdentities_.value(id));
  detail.insert(QStringLiteral("_location"), inWarehouse ? QStringLiteral("warehouse") : QStringLiteral("backpack"));
  detail.insert(QStringLiteral("_unverifiedObservation"), !mayPersist);
  QJsonObject oldBrief = inWarehouse ? warehouse_.value(id) : backpack_.value(id);
  oldBrief.remove(QStringLiteral("_visualMismatch"));
  QJsonObject projected = merge(oldBrief, detail);
  projected.insert(QStringLiteral("_unverifiedObservation"), !mayPersist);
  copyPowerFields(detail, &projected);
  const QJsonObject brief = inventoryBrief(projected, !inWarehouse);
  const auto previousRaw = rawRecords_->acquire(account, epoch, id);
  const bool factsChanged = !previousRaw || !previousRaw->complete || previousRaw->object() != detail ||
      calculationOverlayDiffers(previousRaw->brief, brief) || previousRaw->sourceKnown != mayPersist;
  RawPetRecordInput input;
  input.key = factsChanged ? PetRecordKey{account, epoch, id, ++nextDetailMemoryRevision_} : previousRaw->key;
  input.object = detail; input.brief = brief;
  input.sourceKnown = mayPersist; input.complete = true; input.observedAt = QDateTime::currentDateTimeUtc();
  input.rawProjectionOnly = calculationProjectionMatchesRaw(detail, brief);
  if (factsChanged && !admitRawRecords({input}, true)) {
    if (account == accountKey_ && epoch == sessionGeneration_) {
      emit detailPersistenceQueued(id, requestGeneration, 0);
      emit detailResponseRejected(id, requestGeneration, QStringLiteral("原始详情缓存容量不足，未接受或保存新详情"));
    }
    return false;
  }
  if (!factsChanged) {
    rawRecords_->revise(input.key, input.key, brief, mayPersist, input.rawProjectionOnly, input.observedAt);
    detailStates_[id].brief = brief; detailStates_[id].observedAt = input.observedAt;
  }
  if (inWarehouse) warehouse_.insert(id, brief); else backpack_.insert(id, brief);
  if (factsChanged) ++inventoryRevision_;
  const quint64 taskId = saveDetail(id, detail, input.observedAt, requestGeneration);
  if (account != accountKey_ || epoch != sessionGeneration_) return true;
  announceRaw(id);
  if (account != accountKey_ || epoch != sessionGeneration_) return true;
  emit detailPersistenceQueued(id, requestGeneration, taskId);
  if (account != accountKey_ || epoch != sessionGeneration_) return true;
  emit detailChanged(id);
  if (account != accountKey_ || epoch != sessionGeneration_) return true;
  emit detailObserved(id, PacketCorrelationStrength::EntityCorrelated, currentEnvelope_.receiveSequence, false);
  if (!taskId) emit detailResponseRejected(id, requestGeneration,
      QStringLiteral("详情保存队列拒绝任务，内存已更新但尚未保存"));
  return true;
}
QString PetRepository::formationKey(int id, int plan) const {
  return QStringLiteral("%1:%2").arg(id).arg(plan);
}

QString PetRepository::currentFormationKey() const {
  if (currentFormationId_ <= 0) return {};
  int formationId = currentFormationId_;
  int plan = formationPlans_.value(formationId, 0);
  QString key = formationKey(formationId, plan);
  if (formationPositions_.contains(key)) return key;
  key = formationKey(formationId, 0);
  if (formationPositions_.contains(key)) return key;

  // The current official diverse-formation alias maps formation 16 to the
  // plan-based formation 17. Keep the fallback structural so a future
  // adjacent alias can still be recognized without depending on pet data.
  if (formationPlans_.contains(formationId + 1)) {
    key = formationKey(formationId + 1, formationPlans_.value(formationId + 1));
    if (formationPositions_.contains(key)) return key;
  }
  return {};
}

void PetRepository::updateDeployedPets(const QString& positions) {
  QSet<qint64> replacement;
  for (const QString& value : positions.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
    const qint64 id = value.toLongLong();
    if (id > 0) replacement.insert(id);
  }
  deployedPetIds_ = replacement;
  formationKnown_ = true;
}

QJsonObject PetRepository::withDeploymentState(const QJsonObject& pet) const {
  if (pet.isEmpty() || !formationKnown_) return pet;
  QJsonObject result = pet;
  result.insert(QStringLiteral("_inFormation"), deployedPetIds_.contains(petId(pet)));
  return result;
}

bool PetRepository::parseFormationLoad(const QJsonObject& packet) {
  if (!packet.value(QStringLiteral("fis")).isObject()) return false;
  const QJsonObject formationInfo = packet.value(QStringLiteral("fis")).toObject();
  qint64 currentId = 0;
  if (!PacketContracts::checkedInteger(formationInfo.value(QStringLiteral("cfid")), &currentId, 1, INT_MAX) ||
      !formationInfo.value(QStringLiteral("fl")).isArray()) return false;
  QHash<QString, QString> positions;
  QHash<int, int> plans;
  const auto addFormations = [this, &positions](const QJsonValue& value) {
    if (!value.isArray()) return false;
    for (const QJsonValue& entry : value.toArray()) {
      if (!entry.isObject()) return false;
      int id = 0, plan = 0;
      QString encoded;
      if (!strictFormation(entry.toObject(), &id, &plan, &encoded)) return false;
      const QString key = formationKey(id, plan);
      if (positions.contains(key)) return false;
      positions.insert(key, encoded);
    }
    return true;
  };
  if (!addFormations(formationInfo.value(QStringLiteral("fl")))) return false;
  for (auto it = packet.begin(); it != packet.end(); ++it) {
    if (!it.key().startsWith(QStringLiteral("plan"))) continue;
    qint64 id = 0, plan = 0;
    if (!PacketContracts::checkedInteger(it.key().mid(4), &id, 1, INT_MAX) || !it.value().isObject()) return false;
    const QJsonObject planObject = it.value().toObject();
    if (!PacketContracts::checkedInteger(planObject.value(QStringLiteral("cpid")), &plan, 0, INT_MAX) ||
        !addFormations(planObject.value(QStringLiteral("fl")))) return false;
    plans.insert(static_cast<int>(id), static_cast<int>(plan));
  }
  if (positions.isEmpty()) return false;
  formationPositions_ = positions;
  formationPlans_ = plans;
  currentFormationId_ = static_cast<int>(currentId);
  deployedPetIds_.clear();
  formationKnown_ = false;
  const QString key = currentFormationKey();
  if (!key.isEmpty()) updateDeployedPets(formationPositions_.value(key));
  return true;
}

bool PetRepository::parseFormationPositionChange(const QJsonObject& packet) {
  if (!packet.value(QStringLiteral("fl")).isArray() || currentFormationId_ <= 0) return false;
  QHash<QString, QString> replacement = formationPositions_;
  QSet<QString> seen;
  for (const QJsonValue& entry : packet.value(QStringLiteral("fl")).toArray()) {
    if (!entry.isObject()) return false;
    int id = 0, plan = 0;
    QString positions;
    if (!strictFormation(entry.toObject(), &id, &plan, &positions)) return false;
    const QString key = formationKey(id, plan);
    if (seen.contains(key)) return false;
    seen.insert(key);
    replacement.insert(key, positions);
  }
  if (seen.isEmpty()) return false;
  formationPositions_ = replacement;
  const QString key = currentFormationKey();
  if (key.isEmpty()) { formationKnown_ = false; deployedPetIds_.clear(); return true; }
  updateDeployedPets(formationPositions_.value(key));
  return true;
}

bool PetRepository::parseFormationChanged(const QJsonObject& packet) {
  if (!packet.value(QStringLiteral("fs")).isObject()) return false;
  int id = 0, plan = 0;
  QString positions;
  if (!strictFormation(packet.value(QStringLiteral("fs")).toObject(), &id, &plan, &positions)) return false;
  formationPositions_.insert(formationKey(id, plan), positions);
  if (currentFormationKey() == formationKey(id, plan)) updateDeployedPets(positions);
  return true;
}

bool PetRepository::parseFormationSelection(const QJsonObject& packet) {
  if (!optionalSuccess(packet)) return false;
  qint64 id = 0, plan = 0;
  if (!PacketContracts::checkedInteger(packet.value(QStringLiteral("id")), &id, 1, INT_MAX) ||
      !PacketContracts::checkedInteger(packet.value(QStringLiteral("p")), &plan, 0, INT_MAX)) return false;
  currentFormationId_ = static_cast<int>(id);
  formationPlans_.insert(static_cast<int>(id), static_cast<int>(plan));
  deployedPetIds_.clear();
  formationKnown_ = false;
  const QString key = currentFormationKey();
  if (!key.isEmpty()) updateDeployedPets(formationPositions_.value(key));
  return true;
}

void PetRepository::setSessionSourceEvidence(const SessionSourceEvidence& source) {
  if (session_.state == SessionConnectionState::Closing) return;
  if (!source.verified()) {
    markSessionUncertain(QStringLiteral("来源证据缺失或无效"));
    return;
  }
  if (session_.source.sameSource(source) && session_.source.orderingVerified == source.orderingVerified) return;
  weakReadContinuity_ = false;
  session_.source = source;
  session_.state = SessionConnectionState::Authenticating;
  session_.uncertaintyReason.clear();
  authenticated_ = false;
  onlineData_ = false;
  clearExpectations();
  backpackObservation_ = {};
  warehouseObservations_.clear();
  emit sessionTrustChanged(session_.state, QStringLiteral("等待已验证来源的登录边界"));
}

void PetRepository::setConnectionState(SessionConnectionState state, const QString& reason) {
  if (session_.state == SessionConnectionState::Closing && state != SessionConnectionState::Closing) return;
  if (state == SessionConnectionState::Active) return;  // Only a verified login activates a source.
  weakReadContinuity_ = false;
  session_.state = state;
  if (state == SessionConnectionState::Closing) {
    cachePumpTimer_->stop();
    if (storage_) {
      storage_->cancelReads(storageContext_);
      storage_->cancelReads(sharedStorageContext_);
      storage_->cancelReads(legacyReadContext_);
      if (!detailScanCursor_.isEmpty()) storage_->cancelScan(detailScanCursor_);
    }
    queuedReads_.clear();
    pendingDetailNames_.clear();
    detailScanWanted_ = false;
    migrationActive_ = false;
    migrationProfiles_ = {};
    migrationContext_.reset();
  }
  session_.uncertaintyReason = reason;
  authenticated_ = false;
  onlineData_ = false;
  clearExpectations();
  backpackObservation_.ordered = false;
  for (auto it = warehouseObservations_.begin(); it != warehouseObservations_.end(); ++it)
    it.value().ordered = false;
  formationKnown_ = false;
  emit sessionTrustChanged(state, reason);
  emit dataChanged();
}

void PetRepository::markSessionUncertain(const QString& reason) {
  setConnectionState(SessionConnectionState::Uncertain, reason);
}

InventoryObservation PetRepository::currentObservation(bool complete) const {
  InventoryObservation observation;
  observation.revision = inventoryRevision_;
  observation.receiveSequence = currentEnvelope_.receiveSequence;
  observation.sessionEpoch = sessionGeneration_;
  observation.observedAt = QDateTime::currentDateTime();
  observation.complete = complete;
  observation.sourceVerified = currentPacketCanPersist();
  observation.ordered = observation.sourceVerified && session_.source.orderingVerified &&
                        currentEnvelope_.source.orderingVerified &&
                        currentEnvelope_.orderedObservation &&
                        !currentEnvelope_.orderEvidenceToken.isEmpty() &&
                        currentEnvelope_.receiveSequence != 0;
  return observation;
}

bool PetRepository::listObservationsAuthoritativeForWrite() const {
  const auto authoritative = [this](const InventoryObservation& observation) {
    return observation.complete && observation.sourceVerified && observation.ordered &&
           observation.sessionEpoch == sessionGeneration_;
  };
  if (!session_.canPersist() || !authoritative(backpackObservation_)) return false;
  quint64 warehouseSequence = 0;
  for (const QString& group : {QStringLiteral("normal"), QStringLiteral("elite"), QStringLiteral("goodbye")}) {
    const InventoryObservation observation = warehouseObservations_.value(group);
    if (!authoritative(observation)) return false;
    if (warehouseSequence && warehouseSequence != observation.receiveSequence) return false;
    warehouseSequence = observation.receiveSequence;
  }
  for (auto it = backpack_.cbegin(); it != backpack_.cend(); ++it)
    if (warehouse_.contains(it.key())) return false;
  return warehouseSequence > backpackObservation_.receiveSequence;
}

bool PetRepository::currentPacketCanPersist() const {
  return processingEnvelope_ && session_.accepts(currentEnvelope_);
}

bool PetRepository::canCacheAccountObservation() const {
  // Persistence is local bookkeeping. The source/order gates used for game
  // writes remain unchanged, including after these records are reloaded.
  return authenticated_ && !accountKey_.isEmpty() && session_.account == accountKey_ &&
      sessionGeneration_ != 0 && session_.epoch == sessionGeneration_ &&
      (session_.state == SessionConnectionState::Active ||
       (session_.state == SessionConnectionState::Uncertain && weakReadContinuity_));
}

void PetRepository::handlePacket(const QString& method, const QString& payload) {
  InboundEnvelope envelope;
  envelope.receiveSequence = lastInboundSequence_ + 1;
  envelope.capturedSessionEpoch = sessionGeneration_;
  envelope.method = method;
  envelope.payload = payload;
  handleEnvelope(envelope);
}

void PetRepository::handleEnvelope(const InboundEnvelope& envelope) {
  if (session_.state == SessionConnectionState::Closing) return;
  if (envelope.receiveSequence == 0 || envelope.receiveSequence <= lastInboundSequence_) return;
  QJsonObject packet;
  QString error;
  if (!PacketContracts::decodeObject(envelope.method, envelope.payload, &packet, &error)) {
    if (expectationMatches(detailExpectation_)) {
      DiagnosticLogger::event({QStringLiteral("detail_input_ignored"), QStringLiteral("response"),
          detailExpectation_.requestGeneration, QStringLiteral("decode"),
          QStringLiteral("expectedInstance=%1 method=%2 payloadCharacters=%3 reason=%4")
              .arg(detailExpectation_.instanceId).arg(envelope.method)
              .arg(envelope.payload.size()).arg(error.isEmpty() ? QStringLiteral("unsupported input channel") : error),
          QStringLiteral("inspect input channel; raw payload is not logged")});
    }
    if (!error.isEmpty()) emit packetRejected({}, error);
    return;
  }
  lastInboundSequence_ = envelope.receiveSequence;
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  const bool login = command == QStringLiteral("21_1");
  const QString loginAccount = login ? observedLoginAccount(packet) : QString{};
  if (login && loginAccount.isEmpty()) {
    emit packetRejected(command, QStringLiteral("login account is missing or invalid"));
    return;
  }
  // Same-account login notifications may occur more than once.
  // They may renew a local read epoch only while the original weak stream is
  // still usable. This does not establish a host/source/order identity, and
  // cannot recover from disconnect, overflow, a conflicting account or a
  // previously established verified source.
  const bool weakLoginAllowed = login && weakReadContinuity_ && !session_.source.verified() &&
      ((sessionGeneration_ == 0 && session_.state == SessionConnectionState::Disconnected) ||
       (authenticated_ && session_.state == SessionConnectionState::Uncertain && loginAccount == accountKey_));
  if (envelope.source.verified()) {
    if (!session_.source.sameSource(envelope.source) ||
        envelope.capturedSessionEpoch != sessionGeneration_) {
      emit packetRejected(command, QStringLiteral("captured source/session no longer matches"));
      return;
    }
    if (login && session_.state != SessionConnectionState::Authenticating) {
      markSessionUncertain(QStringLiteral("重认证缺少新的已验证来源边界"));
      emit packetRejected(command, QStringLiteral("login cannot reactivate an old source boundary"));
      return;
    }
    if (!login && !session_.accepts(envelope)) return;
  } else if (session_.source.verified() || (login && !weakLoginAllowed)) {
    markSessionUncertain(QStringLiteral("无法核实入站包来源；保留本地视图，请通过已验证的新来源边界恢复"));
    emit packetRejected(command, QStringLiteral("unknown source cannot update an identified account"));
    return;
  }
  const InboundEnvelope previousEnvelope = currentEnvelope_;
  const bool previousProcessing = processingEnvelope_;
  currentEnvelope_ = envelope;
  processingEnvelope_ = true;
  handleDecodedPacket(packet);
  emit packetObserved(packet, currentEnvelope_);
  currentEnvelope_ = previousEnvelope;
  processingEnvelope_ = previousProcessing;
}

void PetRepository::handleDecodedPacket(const QJsonObject& packet) {
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  if (command == QStringLiteral("21_1")) {
    const QString account = observedLoginAccount(packet);
    if (account.isEmpty()) return;
    if (currentEnvelope_.source.verified() && currentEnvelope_.source.account != account) {
      markSessionUncertain(QStringLiteral("登录账号与来源证据不一致"));
      return;
    }
    activateAccountSession(account);
    return;
  }

  const auto formationChanged = [this](const auto& parse) {
    const QSet<qint64> previous = deployedPetIds_;
    const bool known = formationKnown_;
    if (!parse()) return;
    if (previous != deployedPetIds_ || known != formationKnown_) ++inventoryRevision_;
    emit dataChanged();
  };
  // Formation state is runtime-only. Official 2_2_10 payload is identified by
  // fis.cfid/fl; _cmd is usually present but not required.
  if (authenticated_ && (command == QStringLiteral("2_2_10") ||
                         packet.value(QStringLiteral("fis")).isObject())) {
    formationChanged([&] { return parseFormationLoad(packet); });
    if (command == QStringLiteral("2_2_10") ||
        packet.value(QStringLiteral("fis")).isObject())
      return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_0")) {
    formationChanged([&] { return parseFormationPositionChange(packet); });
    return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_14")) {
    formationChanged([&] { return optionalSuccess(packet) && parseFormationPositionChange(packet); });
    return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_1")) {
    formationChanged([&] { return parseFormationChanged(packet); });
    return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_11")) {
    formationChanged([&] { return parseFormationSelection(packet); });
    return;
  }

  if (command == QStringLiteral("2_1_10")) {
    if (!expectationMatches(backpackExpectation_)) {
      DiagnosticLogger::warning(QStringLiteral("response"),
                                QStringLiteral("rejected stale backpack response"));
      return;
    }
    const quint64 generation = backpackExpectation_.requestGeneration;
    if (!optionalSuccess(packet)) {
      emit packetRejected(command, QStringLiteral("list status is invalid or unsuccessful"));
      return;
    }
    if (!parseBackpack(packet)) return;
    backpackExpectation_.active = false;
    onlineData_ = true;
    updatedAt_ = QDateTime::currentDateTime();
    saveInventory();
    emit dataChanged();
    emit listResponseAccepted(command, generation);
    return;
  }
  if (command == QStringLiteral("2_1_S")) {
    if (!expectationMatches(warehouseExpectation_)) {
      DiagnosticLogger::warning(QStringLiteral("response"),
                                QStringLiteral("rejected stale warehouse response"));
      return;
    }
    const quint64 generation = warehouseExpectation_.requestGeneration;
    if (!optionalSuccess(packet)) {
      emit packetRejected(command, QStringLiteral("list status is invalid or unsuccessful"));
      return;
    }
    if (!parseWarehouse(packet)) return;
    warehouseExpectation_.active = false;
    onlineData_ = true;
    updatedAt_ = QDateTime::currentDateTime();
    saveInventory();
    emit dataChanged();
    emit listResponseAccepted(command, generation);
    return;
  }
  if (command == QStringLiteral("2_1_R")) {
    if (!expectationMatches(detailExpectation_)) {
      DiagnosticLogger::warning(QStringLiteral("response"),
                                QStringLiteral("rejected stale detail response"));
      return;
    }
    const quint64 generation = detailExpectation_.requestGeneration;
    if (!optionalSuccess(packet)) {
      const qint64 instanceId = detailExpectation_.instanceId;
      detailExpectation_.active = false;
      emit detailResponseRejected(instanceId, generation, QStringLiteral("详情状态无效或服务器拒绝"));
      return;
    }
    parseDetail(packet, generation);
    return;
  }
  if (command == QStringLiteral("2_1_11")) {
    if (!expectationMatches(sequenceExpectation_)) {
      DiagnosticLogger::warning(QStringLiteral("response"),
                                QStringLiteral("rejected stale move-write response"));
      return;
    }
    const quint64 generation = sequenceExpectation_.requestGeneration;
    qint64 status = 0;
    const bool statusPresent = packet.contains(QStringLiteral("r"));
    if (statusPresent && !PacketContracts::checkedInteger(packet.value(QStringLiteral("r")), &status)) {
      emit packetRejected(command, QStringLiteral("invalid sequence status type"));
      return;
    }
    if (!currentObservation(false).ordered) {
      emit packetRejected(command, QStringLiteral("sequence acknowledgement lacks verified source/order evidence"));
      return;
    }
    if (!statusPresent) {
      const DecodedBackpack decoded = PacketContracts::decodeBackpack(packet);
      if (!decoded.valid || !decoded.sequencesPresent) {
        emit packetRejected(command, QStringLiteral("sequence acknowledgement has neither status nor valid list"));
        return;
      }
    }
    sequenceExpectation_.active = false;
    if (statusPresent && status != 1) {
      emit sequenceUpdateRejected(generation,
          QStringLiteral("服务器返回失败代码 %1").arg(status));
    } else {
      // A command observation is only an acknowledgement. The controller
      // still needs an ordered, source-verified post-write inventory check.
      if (packet.contains(QStringLiteral("pl")) && packet.contains(QStringLiteral("pps")) &&
          parseBackpack(packet)) {
        onlineData_ = true;
        updatedAt_ = QDateTime::currentDateTime();
        saveInventory();
        emit dataChanged();
      }
      emit sequenceUpdateAccepted(generation);
    }
    return;
  }
}

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
QJsonObject PetRepository::residentDetail(qint64 id) const {
  const auto raw = rawRecords_->acquire(accountKey_, sessionGeneration_, id);
  return raw ? raw->object() : QJsonObject{};
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
