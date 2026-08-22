#include "pet_repository.h"

#include "diagnostic_logger.h"
#include "pet_detail_catalog.h"
#include "pet_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
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

QJsonObject readObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object() : QJsonObject{};
}

bool writeObject(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    DiagnosticLogger::error(QStringLiteral("cache"),
                            QStringLiteral("atomic write open failed file=%1 error=%2")
                                .arg(QFileInfo(path).fileName(), file.errorString()));
    return false;
  }
  file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
  if (file.commit()) return true;
  DiagnosticLogger::error(QStringLiteral("cache"),
                          QStringLiteral("atomic write commit failed file=%1 error=%2")
                              .arg(QFileInfo(path).fileName(), file.errorString()));
  return false;
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

QString valueString(const QJsonValue& value) {
  if (value.isString()) return value.toString();
  if (value.isDouble()) return QString::number(value.toDouble(), 'g', 16);
  return {};
}

}  // namespace

PetRepository::PetRepository(QObject* parent) : QObject(parent) {
  const QString overrideRoot = qEnvironmentVariable("KQPET_DATA_ROOT");
  cacheRoot_ = overrideRoot.isEmpty()
                   ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("KQPetData"))
                   : overrideRoot;
  QDir().mkpath(cacheRoot_);
  migrateLegacyCache();
  loadCache();
}

qint64 PetRepository::petId(const QJsonObject& pet) { return petInstanceId(pet); }
QString PetRepository::stringValue(const QJsonValue& value) { return valueString(value); }

QList<QJsonObject> PetRepository::objectsIn(const QJsonValue& value) {
  QList<QJsonObject> result;
  if (value.isObject()) {
    result.append(value.toObject());
  } else if (value.isArray()) {
    for (const QJsonValue& item : value.toArray()) {
      if (item.isObject()) result.append(item.toObject());
      else if (item.isArray()) result.append(objectsIn(item));
    }
  }
  return result;
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
      QStringLiteral("_position"), QStringLiteral("_visualMismatch"),
      QStringLiteral("_metaOriginalName"), QStringLiteral("_metaAttributes"),
      QStringLiteral("_metaJobs"), QStringLiteral("_metaEra"),
      QStringLiteral("_metaRaceId")};
  QJsonObject brief;
  for (const QString& field : fields)
    if (pet.contains(field)) brief.insert(field, pet.value(field));
  return brief;
}

bool PetRepository::validDetail(const QJsonObject& pet) {
  return petId(pet) > 0 && petRaceId(pet) > 0 && pet.contains(QStringLiteral("lv"));
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
  for (QJsonObject& pet : pets) pet = withDeploymentState(pet);
  return pets;
}
QList<QJsonObject> PetRepository::warehousePets() const { return sorted(warehouse_); }

QJsonObject PetRepository::backpackPet(qint64 instanceId) const {
  return withDeploymentState(backpack_.value(instanceId));
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
  const QJsonObject pet = backpack_.value(instanceId);
  if (pet.isEmpty()) return false;
  const QDateTime savedAt = QDateTime::currentDateTime();
  if (!saveDetail(instanceId, pet, savedAt)) return false;
  details_.insert(instanceId, pet);
  detailSavedTimes_.insert(instanceId, savedAt);
  return true;
}

QJsonObject PetRepository::detailFor(qint64 instanceId) const {
  if (backpack_.contains(instanceId)) return backpackPet(instanceId);
  if (warehouse_.contains(instanceId)) return warehouse_.value(instanceId);
  return details_.value(instanceId);
}

QJsonObject PetRepository::warehousePet(qint64 instanceId) const {
  return warehouse_.value(instanceId);
}

bool PetRepository::hasCachedDetail(qint64 instanceId) const {
  return details_.contains(instanceId);
}

QDateTime PetRepository::detailSavedAt(qint64 instanceId) const {
  return detailSavedTimes_.value(instanceId);
}

QList<qint64> PetRepository::warehouseIdsByDetailAge() const {
  QList<qint64> ids = warehouse_.keys();
  std::sort(ids.begin(), ids.end(), [this](qint64 left, qint64 right) {
    const bool leftMissing = !details_.contains(left);
    const bool rightMissing = !details_.contains(right);
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
  warehouseExpectation_ = {};
}

void PetRepository::expectListPart(const QString& command, quint64 requestGeneration,
                                   const QString& account, quint64 sessionGeneration) {
  const RequestExpectation expectation{account, sessionGeneration, requestGeneration, 0, true};
  if (command == QStringLiteral("2_1_10")) backpackExpectation_ = expectation;
  if (command == QStringLiteral("2_1_S")) warehouseExpectation_ = expectation;
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
}

void PetRepository::cancelSequenceUpdate(quint64 requestGeneration) {
  if (sequenceExpectation_.requestGeneration == requestGeneration)
    sequenceExpectation_.active = false;
}

bool PetRepository::expectationMatches(const RequestExpectation& expectation) const {
  return expectation.active && authenticated_ && expectation.account == accountKey_ &&
         expectation.sessionGeneration == sessionGeneration_;
}

bool PetRepository::parseBackpack(const QJsonObject& packet) {
  const QList<QJsonObject> pets = objectsIn(packet.value(QStringLiteral("pl")));
  if (pets.isEmpty() && !packet.contains(QStringLiteral("pl"))) return false;
  QHash<qint64, QPair<int, int>> positions;
  QJsonArray sequences;
  const QJsonValue sequenceValue = packet.value(QStringLiteral("pps"));
  if (sequenceValue.isArray()) sequences = sequenceValue.toArray();
  else if (sequenceValue.isString()) sequences.append(sequenceValue);
  packSequences_.clear();
  for (int pack = 0; pack < sequences.size(); ++pack) {
    const QStringList ids = sequences.at(pack).toString().split(QLatin1Char('#'), Qt::SkipEmptyParts);
    packSequences_.insert(pack, ids);
    for (int position = 0; position < ids.size(); ++position)
      positions.insert(ids.at(position).toLongLong(), qMakePair(pack, position));
  }
  const QJsonValue capacityValue = packet.value(QStringLiteral("ppc"));
  if (!capacityValue.isUndefined() && !capacityValue.isNull()) {
    packCapacities_.clear();
    if (capacityValue.isDouble()) {
      packCapacities_.insert(0, capacityValue.toInt());
    } else if (capacityValue.isArray()) {
      const QJsonArray capacities = capacityValue.toArray();
      for (int pack = 0; pack < capacities.size(); ++pack)
        packCapacities_.insert(pack, capacities.at(pack).toInt());
    } else if (capacityValue.isObject()) {
      const QJsonObject capacities = capacityValue.toObject();
      for (auto iterator = capacities.begin(); iterator != capacities.end(); ++iterator)
        packCapacities_.insert(iterator.key().toInt(), iterator.value().toInt());
    }
  }
  QHash<qint64, QJsonObject> replacement;
  for (QJsonObject pet : pets) {
    const qint64 id = petId(pet);
    if (id <= 0) continue;
    const QJsonObject previous = backpack_.contains(id) ? backpack_.value(id)
                                                        : details_.value(id);
    pet = PetDetailCatalog::instance().enrichMetadata(pet, previous);
    pet.insert(QStringLiteral("_location"), QStringLiteral("backpack"));
    if (positions.contains(id)) {
      pet.insert(QStringLiteral("_packType"), positions.value(id).first);
      pet.insert(QStringLiteral("_position"), positions.value(id).second);
    }
    replacement.insert(id, pet);
  }
  backpack_ = replacement;
  return true;
}

bool PetRepository::parseWarehouse(const QJsonObject& packet) {
  struct Group { const char* key; const char* name; };
  constexpr Group groups[] = {{"ns", "normal"}, {"rb", "goodbye"}, {"es", "elite"}};
  bool recognized = false;
  QHash<qint64, QJsonObject> replacement;
  QList<qint64> mismatches;
  for (const Group& group : groups) {
    const QString key = QString::fromLatin1(group.key);
    if (!packet.contains(key)) continue;
    recognized = true;
    int position = 0;
    for (QJsonObject brief : objectsIn(packet.value(key))) {
      const qint64 id = petId(brief);
      if (id <= 0) { ++position; continue; }
      const QJsonObject previous = details_.contains(id) ? details_.value(id)
                                                         : warehouse_.value(id);
      brief = PetDetailCatalog::instance().enrichMetadata(brief, previous);
      brief.insert(QStringLiteral("_location"), QStringLiteral("warehouse"));
      brief.insert(QStringLiteral("_warehouseGroup"), QString::fromLatin1(group.name));
      brief.insert(QStringLiteral("_position"), position++);
      const bool mismatch = details_.contains(id) &&
                            visualIdentityDiffers(details_.value(id), brief);
      if (mismatch) {
        brief.insert(QStringLiteral("_visualMismatch"), true);
        mismatches.append(id);
      }
      QJsonObject merged = details_.contains(id) ? merge(details_.value(id), brief) : brief;
      copyPowerFields(details_.value(id), &merged);
      replacement.insert(id, merged);
    }
  }
  if (!recognized) return false;
  warehouse_ = replacement;
  for (qint64 id : mismatches) emit visualMismatchDetected(id);
  return true;
}

bool PetRepository::parseDetail(const QJsonObject& packet, quint64 requestGeneration) {
  if (!packet.value(QStringLiteral("p")).isObject()) return false;
  QJsonObject detail = packet.value(QStringLiteral("p")).toObject();
  const qint64 id = petId(detail);
  if (id != detailExpectation_.instanceId) return false;
  detailExpectation_.active = false;
  if (!validDetail(detail)) {
    emit detailResponseRejected(id, requestGeneration,
                                QStringLiteral("详情缺少实例ID、种族ID或等级字段"));
    return false;
  }
  const bool inBackpack = backpack_.contains(id);
  const bool inWarehouse = warehouse_.contains(id);
  if (!inBackpack && !inWarehouse) {
    emit detailResponseRejected(id, requestGeneration,
                                QStringLiteral("该实例已不在当前背包或仓库列表"));
    return false;
  }
  detail = PetDetailCatalog::instance().enrichMetadata(detail, details_.value(id));
  detail.insert(QStringLiteral("_location"),
                inWarehouse ? QStringLiteral("warehouse")
                            : QStringLiteral("backpack"));
  const QDateTime savedAt = QDateTime::currentDateTime();
  if (!saveDetail(id, detail, savedAt)) {
    emit detailResponseRejected(id, requestGeneration,
                                QStringLiteral("详情缓存原子写入失败"));
    return false;
  }
  details_.insert(id, detail);
  detailSavedTimes_.insert(id, savedAt);
  if (!inWarehouse && inBackpack) {
    QJsonObject merged = merge(detail, backpack_.value(id));
    copyPowerFields(detail, &merged);
    merged.insert(QStringLiteral("_location"), QStringLiteral("backpack"));
    backpack_.insert(id, merged);
  } else if (inWarehouse) {
    QJsonObject brief = warehouseBriefForCache(warehouse_.value(id));
    brief.remove(QStringLiteral("_visualMismatch"));
    QJsonObject merged = merge(detail, brief);
    copyPowerFields(detail, &merged);
    warehouse_.insert(id, merged);
  }
  emit detailChanged(id);
  emit detailResponseAccepted(id, requestGeneration);
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
  const int currentId = formationInfo.value(QStringLiteral("cfid")).toInt();
  if (currentId <= 0 || !formationInfo.value(QStringLiteral("fl")).isArray()) return false;

  QHash<QString, QString> positions;
  QHash<int, int> plans;
  const auto addFormations = [this, &positions](const QJsonValue& value) {
    for (const QJsonObject& formation : objectsIn(value)) {
      const int id = formation.value(QStringLiteral("id")).toInt();
      const int plan = formation.value(QStringLiteral("p")).toInt();
      if (id > 0 && formation.value(QStringLiteral("ps")).isString())
        positions.insert(formationKey(id, plan),
                         formation.value(QStringLiteral("ps")).toString());
    }
  };
  addFormations(formationInfo.value(QStringLiteral("fl")));
  for (auto iterator = packet.begin(); iterator != packet.end(); ++iterator) {
    if (!iterator.key().startsWith(QStringLiteral("plan")) || !iterator.value().isObject())
      continue;
    bool validId = false;
    const int id = iterator.key().mid(4).toInt(&validId);
    if (!validId || id <= 0) continue;
    const QJsonObject planObject = iterator.value().toObject();
    plans.insert(id, planObject.value(QStringLiteral("cpid")).toInt());
    addFormations(planObject.value(QStringLiteral("fl")));
  }
  if (positions.isEmpty()) return false;

  formationPositions_ = positions;
  formationPlans_ = plans;
  currentFormationId_ = currentId;
  deployedPetIds_.clear();
  formationKnown_ = false;
  const QString key = currentFormationKey();
  if (!key.isEmpty()) updateDeployedPets(formationPositions_.value(key));
  return true;
}

bool PetRepository::parseFormationPositionChange(const QJsonObject& packet) {
  if (!packet.value(QStringLiteral("fl")).isArray()) return false;
  bool recognized = false;
  for (const QJsonObject& formation : objectsIn(packet.value(QStringLiteral("fl")))) {
    const int id = formation.value(QStringLiteral("id")).toInt();
    const int plan = formation.value(QStringLiteral("p")).toInt();
    if (id <= 0 || !formation.value(QStringLiteral("ps")).isString()) continue;
    formationPositions_.insert(formationKey(id, plan),
                               formation.value(QStringLiteral("ps")).toString());
    recognized = true;
  }
  if (!recognized || currentFormationId_ <= 0) return false;
  const QString key = currentFormationKey();
  if (key.isEmpty()) return false;
  updateDeployedPets(formationPositions_.value(key));
  return true;
}

bool PetRepository::parseFormationChanged(const QJsonObject& packet) {
  if (!packet.value(QStringLiteral("fs")).isObject()) return false;
  const QJsonObject formation = packet.value(QStringLiteral("fs")).toObject();
  const int id = formation.value(QStringLiteral("id")).toInt();
  const int plan = formation.value(QStringLiteral("p")).toInt();
  if (id <= 0 || !formation.value(QStringLiteral("ps")).isString()) return false;
  formationPositions_.insert(formationKey(id, plan),
                             formation.value(QStringLiteral("ps")).toString());
  const QString key = currentFormationKey();
  if (key != formationKey(id, plan)) return false;
  updateDeployedPets(formation.value(QStringLiteral("ps")).toString());
  return true;
}

bool PetRepository::parseFormationSelection(const QJsonObject& packet) {
  if (packet.contains(QStringLiteral("r")) &&
      packet.value(QStringLiteral("r")).toInt() != 1)
    return false;
  const int id = packet.value(QStringLiteral("id")).toInt();
  if (id <= 0) return false;
  const int plan = packet.value(QStringLiteral("p")).toInt();
  currentFormationId_ = id;
  formationPlans_.insert(id, plan);
  deployedPetIds_.clear();
  formationKnown_ = false;
  const QString key = currentFormationKey();
  if (!key.isEmpty()) updateDeployedPets(formationPositions_.value(key));
  return true;
}

void PetRepository::handlePacket(const QString& method, const QString& payload) {
  if (method != QStringLiteral("recivedata")) return;
  QJsonParseError parseError{};
  QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    const int begin = payload.indexOf(QLatin1Char('{'));
    const int end = payload.lastIndexOf(QLatin1Char('}'));
    if (begin < 0 || end <= begin) return;
    document = QJsonDocument::fromJson(payload.mid(begin, end - begin + 1).toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
  }
  const QJsonObject packet = document.object();
  const QString command = packet.value(QStringLiteral("_cmd")).toString();
  if (command == QStringLiteral("21_1")) {
    const QString account = stringValue(packet.value(QStringLiteral("info")).toObject()
                                            .value(QStringLiteral("n")));
    if (!account.isEmpty()) activateAccountSession(account);
    return;
  }

  // Formation state is runtime-only. Official 2_2_10 payload is identified by
  // fis.cfid/fl; _cmd is usually present but not required.
  if (authenticated_ && (command == QStringLiteral("2_2_10") ||
                         packet.value(QStringLiteral("fis")).isObject())) {
    if (parseFormationLoad(packet)) emit dataChanged();
    if (command == QStringLiteral("2_2_10") ||
        packet.value(QStringLiteral("fis")).isObject())
      return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_0")) {
    if (parseFormationPositionChange(packet)) emit dataChanged();
    return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_14")) {
    if ((!packet.contains(QStringLiteral("r")) ||
         packet.value(QStringLiteral("r")).toInt() == 1) &&
        parseFormationPositionChange(packet))
      emit dataChanged();
    return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_1")) {
    if (parseFormationChanged(packet)) emit dataChanged();
    return;
  }
  if (authenticated_ && command == QStringLiteral("2_2_11")) {
    if (parseFormationSelection(packet)) emit dataChanged();
    return;
  }

  if (command == QStringLiteral("2_1_10")) {
    if (!expectationMatches(backpackExpectation_)) {
      DiagnosticLogger::warning(QStringLiteral("response"),
                                QStringLiteral("rejected stale backpack response"));
      return;
    }
    const quint64 generation = backpackExpectation_.requestGeneration;
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
    sequenceExpectation_.active = false;
    if (packet.contains(QStringLiteral("r")) &&
        packet.value(QStringLiteral("r")).toInt() != 1) {
      emit sequenceUpdateRejected(
          generation,
          QStringLiteral("服务器返回失败代码 %1")
              .arg(valueString(packet.value(QStringLiteral("r")))));
    } else {
      // The official 2_1_11 success response may already contain the updated
      // backpack payload. Apply it before notifying the controller so the
      // extension UI updates immediately; the controller still performs a
      // deferred read-only backpack/warehouse verification afterwards.
      if (packet.contains(QStringLiteral("pl")) &&
          packet.contains(QStringLiteral("pps")) && parseBackpack(packet)) {
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
}

void PetRepository::clearExpectations() {
  backpackExpectation_ = {};
  warehouseExpectation_ = {};
  detailExpectation_ = {};
  sequenceExpectation_ = {};
}

void PetRepository::activateAccountSession(const QString& account) {
  if (account.isEmpty()) return;
  const bool changed = account != accountKey_;
  const bool hadFormationState = formationKnown_ || !formationPositions_.isEmpty();
  if (changed && (!backpack_.isEmpty() || !warehouse_.isEmpty())) saveInventory();
  accountKey_ = account;
  ++sessionGeneration_;
  authenticated_ = true;
  clearExpectations();
  formationPositions_.clear();
  formationPlans_.clear();
  deployedPetIds_.clear();
  currentFormationId_ = 0;
  formationKnown_ = false;
  if (changed) loadAccount();
  writeLastAccount();
  if (changed || hadFormationState) emit dataChanged();
  emit accountSessionChanged(accountKey_, sessionGeneration_);
  emit statusChanged(QStringLiteral("已识别账号 %1，已加载该账号本地缓存：%2")
                         .arg(accountKey_, cachePath_));
}

void PetRepository::loadDetails() {
  QDir directory(detailsPath_);
  QList<QPair<qint64, QJsonObject>> migrations;
  for (const QString& fileName : directory.entryList({QStringLiteral("*.json")}, QDir::Files)) {
    const QJsonObject envelope = readObject(directory.filePath(fileName));
    const int schema = envelope.value(QStringLiteral("schema")).toInt();
    if ((schema != 2 && schema != 3) ||
        envelope.value(QStringLiteral("account")).toString() != accountKey_ ||
        !envelope.value(QStringLiteral("pet")).isObject()) continue;
    const QJsonObject storedDetail = envelope.value(QStringLiteral("pet")).toObject();
    const QJsonObject detail = PetDetailCatalog::instance().enrichMetadata(storedDetail);
    const qint64 id = petId(detail);
    const qint64 envelopeId = valueString(envelope.value(QStringLiteral("instanceId"))).toLongLong();
    if (id <= 0 || (envelopeId > 0 && envelopeId != id)) continue;
    details_.insert(id, detail);
    QDateTime savedAt =
        QDateTime::fromString(envelope.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
    if (!savedAt.isValid())
      savedAt = QFileInfo(directory.filePath(fileName)).lastModified();
    if (!savedAt.isValid()) savedAt = QDateTime::currentDateTime();
    detailSavedTimes_.insert(id, savedAt);
    if (schema == 2 || detail != storedDetail) migrations.append(qMakePair(id, detail));
  }
  for (const auto& migration : migrations) saveDetail(migration.first, migration.second);
}

void PetRepository::loadAccount() {
  backpack_.clear();
  warehouse_.clear();
  details_.clear();
  detailSavedTimes_.clear();
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
  const QJsonObject profile = readObject(cachePath_);
  const int schema = profile.value(QStringLiteral("schema")).toInt();
  if ((schema == 2 || schema == 3) &&
      profile.value(QStringLiteral("account")).toString() == accountKey_) {
    for (const QJsonValue& value : profile.value(QStringLiteral("backpack")).toArray()) {
      if (!value.isObject() || petId(value.toObject()) <= 0) continue;
      const QJsonObject pet = PetDetailCatalog::instance().enrichMetadata(value.toObject());
      backpack_.insert(petId(pet), pet);
    }
    for (const QJsonValue& value : profile.value(QStringLiteral("warehouse")).toArray()) {
      if (!value.isObject() || petId(value.toObject()) <= 0) continue;
      const QJsonObject pet = PetDetailCatalog::instance().enrichMetadata(value.toObject());
      warehouse_.insert(petId(pet), pet);
    }
    updatedAt_ = QDateTime::fromString(profile.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
  }
  loadDetails();
  for (auto iterator = warehouse_.begin(); iterator != warehouse_.end(); ++iterator) {
    if (!details_.contains(iterator.key())) continue;
    QJsonObject merged = merge(details_.value(iterator.key()), iterator.value());
    copyPowerFields(details_.value(iterator.key()), &merged);
    iterator.value() = merged;
  }
}

void PetRepository::loadCache() {
  QFile lastAccount(QDir(cacheRoot_).filePath(QStringLiteral("last-account.txt")));
  if (lastAccount.open(QIODevice::ReadOnly)) {
    const QString saved = QString::fromUtf8(lastAccount.readAll()).trimmed();
    if (!saved.isEmpty()) accountKey_ = saved;
  }
  loadAccount();
  authenticated_ = false;
}

void PetRepository::writeLastAccount() const {
  QSaveFile file(QDir(cacheRoot_).filePath(QStringLiteral("last-account.txt")));
  if (!file.open(QIODevice::WriteOnly)) return;
  file.write(accountKey_.toUtf8());
  file.commit();
}

void PetRepository::saveInventory() {
  QJsonArray backpack;
  for (const QJsonObject& pet : backpack_.values()) backpack.append(pet);
  QJsonArray warehouse;
  for (const QJsonObject& pet : warehouse_.values()) warehouse.append(warehouseBriefForCache(pet));
  writeObject(cachePath_, {{QStringLiteral("schema"), 3},
                           {QStringLiteral("account"), accountKey_},
                           {QStringLiteral("savedAt"),
                            (updatedAt_.isValid() ? updatedAt_ : QDateTime::currentDateTime())
                                .toString(Qt::ISODate)},
                           {QStringLiteral("backpack"), backpack},
                           {QStringLiteral("warehouse"), warehouse}});
  writeLastAccount();
}

bool PetRepository::saveDetail(qint64 instanceId, const QJsonObject& detail,
                               const QDateTime& requestedSavedAt) {
  const QJsonObject brief = warehouseBriefForCache(warehouse_.value(instanceId));
  QDateTime savedAt = requestedSavedAt;
  if (!savedAt.isValid()) savedAt = detailSavedTimes_.value(instanceId);
  if (!savedAt.isValid()) savedAt = QDateTime::currentDateTime();
  const QJsonValue obtainedAt = detail.contains(QStringLiteral("gd"))
                                    ? detail.value(QStringLiteral("gd"))
                                    : brief.value(QStringLiteral("gd"));
  return writeObject(QDir(detailsPath_).filePath(QStringLiteral("%1.json").arg(instanceId)),
                     {{QStringLiteral("schema"), 3},
                      {QStringLiteral("account"), accountKey_},
                      {QStringLiteral("instanceId"), QString::number(instanceId)},
                      {QStringLiteral("raceIdAtSave"), petRaceId(detail)},
                      {QStringLiteral("faceIdAtSave"), petFaceId(detail)},
                      {QStringLiteral("displayNameAtSave"), petProtocolName(detail)},
                      {QStringLiteral("obtainedAt"), obtainedAt},
                      {QStringLiteral("savedAt"), savedAt.toString(Qt::ISODate)},
                      {QStringLiteral("imageCacheKey"), petVisualKey(detail)},
                      {QStringLiteral("pet"), detail}});
}

void PetRepository::migrateLegacyCache() {
  const QString lastAccountPath = QDir(cacheRoot_).filePath(QStringLiteral("last-account.txt"));
  if (QFile::exists(lastAccountPath)) return;
  const QString legacyPath = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                                 .filePath(QStringLiteral("KQPetInventory/cache-v1.json"));
  const QJsonObject legacy = readObject(legacyPath);
  if (legacy.value(QStringLiteral("schema")).toInt() != 1) return;
  const QJsonObject profiles = legacy.value(QStringLiteral("profiles")).toObject();
  for (auto iterator = profiles.begin(); iterator != profiles.end(); ++iterator) {
    if (!iterator.value().isObject()) continue;
    const QString account = iterator.key();
    const QJsonObject oldProfile = iterator.value().toObject();
    QJsonArray warehouse;
    for (const QJsonValue& value : oldProfile.value(QStringLiteral("warehouse")).toArray())
      if (value.isObject()) warehouse.append(warehouseBriefForCache(value.toObject()));
    const QString directory = accountDirectory(account);
    writeObject(QDir(directory).filePath(QStringLiteral("inventory.json")),
                {{QStringLiteral("schema"), 3}, {QStringLiteral("account"), account},
                 {QStringLiteral("savedAt"), oldProfile.value(QStringLiteral("savedAt"))},
                 {QStringLiteral("backpack"), oldProfile.value(QStringLiteral("backpack"))},
                 {QStringLiteral("warehouse"), warehouse}});
    const QJsonObject details = oldProfile.value(QStringLiteral("details")).toObject();
    for (auto detail = details.begin(); detail != details.end(); ++detail) {
      const qint64 id = detail.key().toLongLong();
      if (id <= 0 || !detail.value().isObject()) continue;
      const QJsonObject pet = detail.value().toObject();
      writeObject(QDir(directory).filePath(QStringLiteral("details/%1.json").arg(id)),
                  {{QStringLiteral("schema"), 3}, {QStringLiteral("account"), account},
                   {QStringLiteral("instanceId"), QString::number(id)},
                   {QStringLiteral("raceIdAtSave"), petRaceId(pet)},
                   {QStringLiteral("faceIdAtSave"), petFaceId(pet)},
                   {QStringLiteral("displayNameAtSave"), petProtocolName(pet)},
                   {QStringLiteral("obtainedAt"), pet.value(QStringLiteral("gd"))},
                   {QStringLiteral("savedAt"), oldProfile.value(QStringLiteral("savedAt"))},
                   {QStringLiteral("imageCacheKey"), petVisualKey(pet)},
                   {QStringLiteral("pet"), pet}});
    }
  }
  const QString last = legacy.value(QStringLiteral("lastAccount")).toString();
  if (!last.isEmpty()) {
    QSaveFile file(lastAccountPath);
    if (file.open(QIODevice::WriteOnly)) { file.write(last.toUtf8()); file.commit(); }
  }
}
