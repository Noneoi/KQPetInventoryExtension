// Protocol intake: request expectations, list/detail/formation packet
// parsing, session source evidence and inbound packet dispatch.
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
        emit packetRejected(QStringLiteral("2_1_S"),
            QStringLiteral("duplicate instance across groups: %1 appears again in %2")
                .arg(id).arg(QString::fromLatin1(group.name)));
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
        // The retained group is the one this packet did not carry, so name it:
        // otherwise the report cannot say which observation is being protected.
        emit packetRejected(QStringLiteral("2_1_S"),
            QStringLiteral("instance %1 in %2 conflicts with retained group %3")
                .arg(id).arg(candidate.key(),
                    replacement.value(id).value(QStringLiteral("_warehouseGroup")).toString()));
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
