#include "pet_detail_catalog.h"

#include "pet_identity.h"

#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {

QList<int> idsOf(const QString& text) {
  QList<int> ids;
  for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
    ids.append(part.toInt());
  return ids;
}

bool hasAny(const QList<int>& ids, std::initializer_list<int> candidates) {
  for (int candidate : candidates) {
    if (ids.contains(candidate))
      return true;
  }
  return false;
}

QString fusionName(const QList<int>& ids) {
  if (hasAny(ids, {26, 36}) && hasAny(ids, {29, 39}))
    return QStringLiteral("召唤英雄");
  if (hasAny(ids, {19, 20}) && hasAny(ids, {26, 36}))
    return QStringLiteral("元素召唤");
  if (hasAny(ids, {19, 20}) && hasAny(ids, {29, 39}))
    return QStringLiteral("元素英雄");
  if (hasAny(ids, {19, 20}) && hasAny(ids, {28, 38}))
    return QStringLiteral("元素通灵");
  if (hasAny(ids, {17, 18}) && hasAny(ids, {28, 38}))
    return QStringLiteral("赋能通灵");
  if (hasAny(ids, {17, 18}) && hasAny(ids, {26, 36}))
    return QStringLiteral("赋能召唤");
  if (hasAny(ids, {42, 43}) && hasAny(ids, {28, 38}))
    return QStringLiteral("幻元通灵");
  return {};
}

}  // namespace

const PetDetailCatalog& PetDetailCatalog::instance() {
  static const PetDetailCatalog catalog;
  return catalog;
}

PetDetailCatalog::PetDetailCatalog() {
  QFile file(QStringLiteral(":/kqpet/pet-detail-data.json"));
  if (!file.open(QIODevice::ReadOnly))
    return;
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return;
  root_ = document.object();
  // The catalog is data rather than code.  A newer data package can be placed
  // beside the client without rebuilding the DLL.  This is intentionally an
  // overlay: sections and pet rows present in the external file replace the
  // embedded release snapshot, while all other rows stay available.
  QString externalPath = QString::fromUtf8(qgetenv("KQPET_CATALOG_PATH"));
  if (externalPath.isEmpty()) {
    externalPath = QDir(QCoreApplication::applicationDirPath())
                       .filePath(QStringLiteral("KQPetData/catalog/pet-detail-data.json"));
  }
  QFile external(externalPath);
  if (external.open(QIODevice::ReadOnly)) {
    QJsonParseError externalError{};
    const QJsonDocument externalDocument =
        QJsonDocument::fromJson(external.readAll(), &externalError);
    if (externalError.error == QJsonParseError::NoError && externalDocument.isObject()) {
      const QJsonObject overlay = externalDocument.object();
      for (auto section = overlay.begin(); section != overlay.end(); ++section) {
        if (section.value().isObject() && root_.value(section.key()).isObject()) {
          QJsonObject merged = root_.value(section.key()).toObject();
          const QJsonObject additions = section.value().toObject();
          for (auto item = additions.begin(); item != additions.end(); ++item)
            merged.insert(item.key(), item.value());
          root_.insert(section.key(), merged);
        } else {
          root_.insert(section.key(), section.value());
        }
      }
    }
  }
  loaded_ = !root_.value(QStringLiteral("pets")).toObject().isEmpty();
  indexPets();
}

QString PetDetailCatalog::eraPrefix(const QString& name) {
  static const QRegularExpression pattern(QStringLiteral("^[\\[【]([^\\]】]+)[\\]】]"));
  const QRegularExpressionMatch match = pattern.match(name.trimmed());
  return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString PetDetailCatalog::coreName(const QString& name) {
  QString trimmed = name.trimmed();
  static const QRegularExpression era(QStringLiteral("^[\\[【][^\\]】]+[\\]】]"));
  trimmed.remove(era);
  trimmed = trimmed.trimmed();
  const int separator = trimmed.lastIndexOf(QChar(0x00B7));
  if (separator >= 0)
    return trimmed.mid(separator + 1).trimmed();
  return trimmed;
}

int PetDetailCatalog::liveRaceId(const QJsonObject& pet) {
  return petRaceId(pet);
}

void PetDetailCatalog::indexPets() {
  coreToRace_.clear();
  eraCoreToRace_.clear();
  const QJsonObject pets = root_.value(QStringLiteral("pets")).toObject();
  auto better = [](int candidate, int current) {
    if (current <= 0) return true;
    const bool candidateNormal = candidate > 0 && candidate < 20000;
    const bool currentNormal = current > 0 && current < 20000;
    if (candidateNormal != currentNormal) return candidateNormal;
    return candidate > current;
  };
  for (auto iterator = pets.begin(); iterator != pets.end(); ++iterator) {
    const int raceId = iterator.key().toInt();
    if (raceId <= 0 || !iterator.value().isObject()) continue;
    const QString name = iterator.value().toObject().value(QStringLiteral("name")).toString();
    const QString core = coreName(name);
    const QString era = eraPrefix(name);
    if (core.isEmpty()) continue;
    if (better(raceId, coreToRace_.value(core)))
      coreToRace_.insert(core, raceId);
    if (!era.isEmpty()) {
      const QString eraKey = era + QLatin1Char('/') + core;
      if (better(raceId, eraCoreToRace_.value(eraKey)))
        eraCoreToRace_.insert(eraKey, raceId);
    }
  }
}

QJsonObject PetDetailCatalog::familyMetadata(const QString& liveName) const {
  const QString core = coreName(liveName);
  if (core.isEmpty()) return {};
  const QString era = eraPrefix(liveName);
  if (!era.isEmpty()) {
    const int eraRace = eraCoreToRace_.value(era + QLatin1Char('/') + core);
    if (eraRace > 0) return pet(eraRace);
  }
  const int familyRace = coreToRace_.value(core);
  return familyRace > 0 ? pet(familyRace) : QJsonObject{};
}

QJsonObject PetDetailCatalog::metadataFor(const QJsonObject& petObject) const {
  const QJsonObject direct = pet(liveRaceId(petObject));
  if (!direct.isEmpty()) return direct;
  const QString snapshotAttributes =
      petObject.value(QStringLiteral("_metaAttributes")).toString();
  const QString snapshotJobs = petObject.value(QStringLiteral("_metaJobs")).toString();
  const QString snapshotName =
      petObject.value(QStringLiteral("_metaOriginalName")).toString();
  if (!snapshotName.isEmpty() || !snapshotAttributes.isEmpty() || !snapshotJobs.isEmpty()) {
    return {{QStringLiteral("name"), snapshotName},
            {QStringLiteral("attributes"), snapshotAttributes},
            {QStringLiteral("jobs"), snapshotJobs}};
  }
  return familyMetadata(petObject.value(QStringLiteral("n")).toString());
}

QString PetDetailCatalog::resolvedOriginalName(const QJsonObject& petObject) const {
  const int raceId = liveRaceId(petObject);
  if (!pet(raceId).isEmpty()) {
    const QString original = originalName(raceId);
    if (!original.isEmpty()) return original;
  }
  const QString snapshot =
      petObject.value(QStringLiteral("_metaOriginalName")).toString().trimmed();
  if (!snapshot.isEmpty()) return snapshot;
  const QString live = petObject.value(QStringLiteral("n")).toString().trimmed();
  return live.isEmpty() ? petName(raceId) : live;
}

QString PetDetailCatalog::resolvedAttributes(const QJsonObject& petObject) const {
  return attributes(metadataFor(petObject).value(QStringLiteral("attributes")).toString());
}

QString PetDetailCatalog::resolvedJobs(const QJsonObject& petObject) const {
  const QJsonObject direct = pet(liveRaceId(petObject));
  if (!direct.isEmpty())
    return jobs(direct.value(QStringLiteral("jobs")).toString());
  const QString snapshot = petObject.value(QStringLiteral("_metaJobs")).toString();
  if (!snapshot.isEmpty()) return jobs(snapshot);
  const QString runtimeJob = petObject.value(QStringLiteral("rt")).toVariant().toString();
  if (!runtimeJob.isEmpty() && runtimeJob != QStringLiteral("0"))
    return jobs(runtimeJob);
  return jobs(familyMetadata(petObject.value(QStringLiteral("n")).toString())
                  .value(QStringLiteral("jobs"))
                  .toString());
}

QString PetDetailCatalog::resolvedEra(const QJsonObject& petObject) const {
  const QString snapshot = petObject.value(QStringLiteral("_metaEra")).toString().trimmed();
  if (!snapshot.isEmpty()) return snapshot;
  const QStringList names = {resolvedOriginalName(petObject),
                             petName(liveRaceId(petObject)),
                             petObject.value(QStringLiteral("n")).toString()};
  for (const QString& name : names) {
    const QString era = eraPrefix(name);
    if (!era.isEmpty()) return era;
  }
  return QStringLiteral("其它");
}

QJsonObject PetDetailCatalog::enrichMetadata(const QJsonObject& petObject,
                                             const QJsonObject& previous) const {
  QJsonObject result = petObject;
  QJsonObject metadata = pet(liveRaceId(petObject));
  int metadataRace = liveRaceId(petObject);
  if (metadata.isEmpty()) {
    const QStringList snapshotKeys = {
        QStringLiteral("_metaOriginalName"), QStringLiteral("_metaAttributes"),
        QStringLiteral("_metaJobs"), QStringLiteral("_metaEra"),
        QStringLiteral("_metaRaceId")};
    bool copied = false;
    for (const QString& key : snapshotKeys) {
      if (previous.contains(key)) {
        result.insert(key, previous.value(key));
        copied = true;
      }
    }
    if (copied) return result;
    metadata = familyMetadata(petObject.value(QStringLiteral("n")).toString());
    metadataRace = 0;
  }

  QString original;
  if (metadataRace > 0)
    original = originalName(metadataRace);
  if (original.isEmpty()) original = metadata.value(QStringLiteral("name")).toString();
  if (original.isEmpty()) original = petObject.value(QStringLiteral("n")).toString();
  const QString attributesSequence = metadata.value(QStringLiteral("attributes")).toString();
  QString jobsSequence = metadata.value(QStringLiteral("jobs")).toString();
  if (jobsSequence.isEmpty())
    jobsSequence = petObject.value(QStringLiteral("rt")).toVariant().toString();
  QString era = eraPrefix(original);
  if (era.isEmpty()) era = eraPrefix(petObject.value(QStringLiteral("n")).toString());

  if (!original.isEmpty()) result.insert(QStringLiteral("_metaOriginalName"), original);
  if (!attributesSequence.isEmpty())
    result.insert(QStringLiteral("_metaAttributes"), attributesSequence);
  if (!jobsSequence.isEmpty() && jobsSequence != QStringLiteral("0"))
    result.insert(QStringLiteral("_metaJobs"), jobsSequence);
  if (!era.isEmpty()) result.insert(QStringLiteral("_metaEra"), era);
  if (metadataRace > 0) {
    int canonicalRace = metadataRace;
    QSet<int> visited;
    while (canonicalRace > 0 && !visited.contains(canonicalRace)) {
      visited.insert(canonicalRace);
      const int parentRace =
          pet(canonicalRace).value(QStringLiteral("groupRaceId")).toInt();
      if (parentRace <= 0 || pet(parentRace).isEmpty()) break;
      canonicalRace = parentRace;
    }
    result.insert(QStringLiteral("_metaRaceId"), canonicalRace);
  }
  return result;
}

QJsonObject PetDetailCatalog::item(const char* section, int defineId) const {
  return root_.value(QString::fromLatin1(section))
      .toObject()
      .value(QString::number(defineId))
      .toObject();
}

QJsonObject PetDetailCatalog::pet(int raceId) const { return item("pets", raceId); }

QString PetDetailCatalog::petName(int raceId) const {
  return pet(raceId).value(QStringLiteral("name")).toString();
}

QString PetDetailCatalog::originalName(int raceId) const {
  QSet<int> visited;
  QJsonObject current = pet(raceId);
  QString name = current.value(QStringLiteral("name")).toString();
  int groupRaceId = current.value(QStringLiteral("groupRaceId")).toInt();
  while (groupRaceId > 0 && !visited.contains(groupRaceId)) {
    visited.insert(groupRaceId);
    const QJsonObject base = pet(groupRaceId);
    if (base.isEmpty())
      break;
    name = base.value(QStringLiteral("name")).toString(name);
    groupRaceId = base.value(QStringLiteral("groupRaceId")).toInt();
  }
  return name;
}

QString PetDetailCatalog::attributes(const QString& sequence) const {
  const QJsonObject names = root_.value(QStringLiteral("attributes")).toObject();
  QStringList result;
  for (const QString& part : sequence.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString id = part.trimmed();
    result.append(names.value(id).toString(QStringLiteral("属性 %1").arg(id)));
  }
  result.removeDuplicates();
  return result.isEmpty() ? QStringLiteral("—") : result.join(QStringLiteral(" + "));
}

QString PetDetailCatalog::jobs(const QString& sequence) const {
  const QJsonObject names = root_.value(QStringLiteral("jobs")).toObject();
  QStringList groups;
  for (const QString& group : sequence.split(QLatin1Char('-'), Qt::SkipEmptyParts)) {
    const QList<int> ids = idsOf(group);
    QString name = fusionName(ids);
    if (name.isEmpty()) {
      QStringList parts;
      for (int id : ids)
        parts.append(names.value(QString::number(id)).toString(QStringLiteral("职业 %1").arg(id)));
      parts.removeDuplicates();
      name = parts.join(QStringLiteral(" + "));
    }
    if (!name.isEmpty())
      groups.append(name);
  }
  return groups.isEmpty() ? QStringLiteral("—") : groups.join(QStringLiteral(" / "));
}

QString PetDetailCatalog::mappedName(const char* section, int defineId,
                                     const QString& fallbackPrefix) const {
  const QString name = item(section, defineId).value(QStringLiteral("name")).toString();
  return name.isEmpty() ? QStringLiteral("%1 %2").arg(fallbackPrefix).arg(defineId) : name;
}

QString PetDetailCatalog::badgeName(int defineId) const {
  return mappedName("badges", defineId, QStringLiteral("元魂"));
}

QJsonObject PetDetailCatalog::badge(int defineId) const {
  return item("badges", defineId);
}

QString PetDetailCatalog::sacredEquipmentName(int defineId) const {
  return mappedName("sacredEquipment", defineId, QStringLiteral("神源兽"));
}

QString PetDetailCatalog::astrolabeName(int defineId) const {
  return mappedName("astrolabe", defineId, QStringLiteral("星灵"));
}

QJsonObject PetDetailCatalog::astrolabe(int defineId) const {
  return item("astrolabe", defineId);
}

QJsonObject PetDetailCatalog::stargod(int defineId) const { return item("stargods", defineId); }

QString PetDetailCatalog::itemName(int itemId) const {
  return mappedName("items", itemId, QStringLiteral("道具"));
}

QString PetDetailCatalog::moneyName(int moneyId) const {
  return mappedName("money", moneyId, QStringLiteral("货币"));
}

QString PetDetailCatalog::materialName(int type, int materialId) const {
  if (type == 4) return itemName(materialId);
  if (type == 8) return moneyName(materialId);
  if (type == 134 && materialId == 1) return QStringLiteral("个人贡献币");
  if (type == 134 && materialId == 2) return QStringLiteral("联盟资金");
  return QStringLiteral("材料%1:%2").arg(type).arg(materialId);
}

QString PetDetailCatalog::materialCostText(int type, int materialId, int count) const {
  return QStringLiteral("%1 ×%2").arg(materialName(type, materialId)).arg(count);
}

int PetDetailCatalog::sacredMaxStar(int planId) {
  switch (planId) {
    case 1:
      return 8;
    case 2:
      return 10;
    case 3:
      return 9;
    default:
      return 0;
  }
}

int PetDetailCatalog::sacredMaxStage(int planId) {
  if (planId < 1 || planId > 24)
    return 0;
  return 5 + ((planId - 1) % 3);
}
