#include "asset_snapshot_store.h"

#include "diagnostic_logger.h"
#include "pet_repository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace {

QJsonObject readObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  return document.isObject() ? document.object() : QJsonObject{};
}

bool writeObject(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
  return file.commit();
}

QJsonObject snapshotPetObject(const AssetSnapshotPet& pet) {
  return {{QStringLiteral("instanceId"), QString::number(pet.instanceId)},
          {QStringLiteral("raceId"), pet.raceId},
          {QStringLiteral("name"), pet.name},
          {QStringLiteral("currentPower"), pet.currentPower},
          {QStringLiteral("highestPower"), pet.highestPower},
          {QStringLiteral("completionPercent"), pet.completionPercent},
          {QStringLiteral("fullyCultivated"), pet.fullyCultivated},
          {QStringLiteral("redStarComplete"), pet.redStarComplete},
          {QStringLiteral("astrolabeBreakthrough"), pet.astrolabeBreakthrough}};
}

AssetSnapshotPet parseSnapshotPet(const QJsonObject& object) {
  AssetSnapshotPet pet;
  pet.instanceId = object.value(QStringLiteral("instanceId")).toVariant().toLongLong();
  pet.raceId = object.value(QStringLiteral("raceId")).toInt();
  pet.name = object.value(QStringLiteral("name")).toString();
  pet.currentPower = object.value(QStringLiteral("currentPower")).toInt();
  pet.highestPower = object.value(QStringLiteral("highestPower")).toInt();
  pet.completionPercent = object.value(QStringLiteral("completionPercent")).toInt();
  pet.fullyCultivated = object.value(QStringLiteral("fullyCultivated")).toBool();
  pet.redStarComplete = object.value(QStringLiteral("redStarComplete")).toBool();
  pet.astrolabeBreakthrough =
      object.value(QStringLiteral("astrolabeBreakthrough")).toBool();
  return pet;
}

AccountAssetSnapshot parseSnapshot(const QJsonObject& object, int schema) {
  AccountAssetSnapshot snapshot;
  snapshot.schemaVersion = schema;
  snapshot.analysisVersion =
      schema == AssetAnalysisVersion::kLegacySnapshotSchema
          ? object.value(QStringLiteral("analysisVersion"))
                .toInt(AssetAnalysisVersion::kCurrentAnalysis)
          : object.value(QStringLiteral("analysisVersion")).toInt();
  snapshot.account = object.value(QStringLiteral("account")).toString();
  snapshot.createdAt = QDateTime::fromString(
      object.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
  const QJsonObject summary =
      schema == AssetAnalysisVersion::kCurrentSnapshotSchema
          ? object.value(QStringLiteral("summary")).toObject()
          : object;
  snapshot.totalPets = summary.value(QStringLiteral("totalPets")).toInt();
  snapshot.fullyCultivatedPets =
      summary.value(QStringLiteral("fullyCultivatedPets")).toInt();
  snapshot.totalCurrentPower =
      summary.value(QStringLiteral("totalCurrentPower")).toVariant().toLongLong();
  for (const QJsonValue& value : object.value(QStringLiteral("pets")).toArray())
    if (value.isObject()) snapshot.pets.append(parseSnapshotPet(value.toObject()));
  return snapshot;
}

}  // namespace

AssetSnapshotStore::AssetSnapshotStore(PetRepository* repository)
    : repository_(repository) {}

QString AssetSnapshotStore::snapshotsDirectory() const {
  return repository_
             ? QDir(QFileInfo(repository_->cachePath()).absolutePath())
                   .filePath(QStringLiteral("snapshots"))
             : QString{};
}

bool AssetSnapshotStore::write(const QString& account,
                               const AccountAssetOverview& overview,
                               QString* status) const {
  if (!repository_ || account.isEmpty() || overview.account != account) return false;
  if (overview.totalPets <= 0) {
    if (status) *status = QStringLiteral("当前账号没有可记录的精灵资产缓存");
    return false;
  }
  AccountAssetSnapshot snapshot;
  snapshot.schemaVersion = AssetAnalysisVersion::kCurrentSnapshotSchema;
  snapshot.analysisVersion = overview.analysisVersion;
  snapshot.account = account;
  snapshot.createdAt = QDateTime::currentDateTime();
  snapshot.totalPets = overview.totalPets;
  snapshot.fullyCultivatedPets = overview.fullyCultivatedPets;
  snapshot.totalCurrentPower = overview.totalCurrentPower;
  QJsonArray pets;
  for (const PetAssetRecord& record : overview.pets) {
    AssetSnapshotPet pet;
    pet.instanceId = record.instanceId;
    pet.raceId = record.raceId;
    pet.name = record.name;
    pet.currentPower = record.currentPower;
    pet.highestPower = record.highestPower;
    pet.completionPercent = record.completionPercent;
    pet.fullyCultivated = record.fullyCultivated;
    pet.redStarComplete = record.detailAvailable &&
                          record.pet.contains(QStringLiteral("xzdl")) &&
                          !record.redStarMissing;
    pet.astrolabeBreakthrough = record.detailAvailable && !record.astrolabeMissing;
    snapshot.pets.append(pet);
    pets.append(snapshotPetObject(pet));
  }
  const QString path = QDir(snapshotsDirectory())
                           .filePath(snapshot.createdAt.date().toString(
                               QStringLiteral("yyyy-MM-dd.json")));
  const bool saved = writeObject(
      path, {{QStringLiteral("schema"), snapshot.schemaVersion},
             {QStringLiteral("analysisVersion"), snapshot.analysisVersion},
             {QStringLiteral("account"), snapshot.account},
             {QStringLiteral("createdAt"), snapshot.createdAt.toString(Qt::ISODate)},
             {QStringLiteral("summary"),
              QJsonObject{{QStringLiteral("totalPets"), snapshot.totalPets},
                          {QStringLiteral("fullyCultivatedPets"),
                           snapshot.fullyCultivatedPets},
                          {QStringLiteral("totalCurrentPower"),
                           QString::number(snapshot.totalCurrentPower)}}},
             {QStringLiteral("pets"), pets}});
  if (!saved) {
    DiagnosticLogger::error(QStringLiteral("snapshot"),
                            QStringLiteral("snapshot atomic write failed"));
    if (status) *status = QStringLiteral("历史快照写入失败，旧快照保持不变");
    return false;
  }
  DiagnosticLogger::info(QStringLiteral("snapshot"),
                         QStringLiteral("recorded date=%1 pets=%2")
                             .arg(snapshot.createdAt.date().toString(Qt::ISODate))
                             .arg(snapshot.totalPets));
  if (status)
    *status = QStringLiteral("已记录 %1 的轻量资产快照（同一天再次记录会原子更新）")
                  .arg(snapshot.createdAt.date().toString(QStringLiteral("yyyy-MM-dd")));
  return true;
}

QList<AccountAssetSnapshot> AssetSnapshotStore::readAll(
    const QString& account) const {
  QList<AccountAssetSnapshot> result;
  QDir directory(snapshotsDirectory());
  for (const QString& fileName : directory.entryList(
           {QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
    const QJsonObject object = readObject(directory.filePath(fileName));
    const int schema = object.value(QStringLiteral("schema")).toInt();
    if ((schema != AssetAnalysisVersion::kLegacySnapshotSchema &&
         schema != AssetAnalysisVersion::kCurrentSnapshotSchema) ||
        object.value(QStringLiteral("account")).toString() != account)
      continue;
    AccountAssetSnapshot snapshot = parseSnapshot(object, schema);
    if (!snapshot.createdAt.isValid()) continue;
    result.append(snapshot);
  }
  std::sort(result.begin(), result.end(),
            [](const AccountAssetSnapshot& left,
               const AccountAssetSnapshot& right) {
              return left.createdAt < right.createdAt;
            });
  return result;
}
