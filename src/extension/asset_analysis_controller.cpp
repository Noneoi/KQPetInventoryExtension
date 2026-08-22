#include "asset_analysis_controller.h"

#include "diagnostic_logger.h"
#include "pet_detail_analyzer.h"
#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_repository.h"
#include "routine_overview_catalog.h"
#include "routine_overview_controller.h"
#include "shop_exchange_catalog.h"
#include "shop_exchange_controller.h"
#include "shop_pet_eligibility.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

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

QString displayName(const QJsonObject& pet) {
  QString name = pet.value(QStringLiteral("customName")).toString().trimmed();
  if (name.isEmpty()) name = pet.value(QStringLiteral("n")).toString().trimmed();
  if (name.isEmpty()) name = PetDetailCatalog::instance().resolvedOriginalName(pet);
  return name.isEmpty() ? QStringLiteral("未知精灵") : name;
}

QString locationText(const QJsonObject& pet) {
  if (pet.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack"))
    return QStringLiteral("背包");
  if (pet.value(QStringLiteral("_warehouseGroup")).toString() == QStringLiteral("elite"))
    return QStringLiteral("精英仓库");
  return QStringLiteral("普通仓库");
}

bool hasFullDetail(const PetRepository* repository, const QJsonObject& pet) {
  if (!repository) return false;
  if (pet.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack"))
    return pet.value(QStringLiteral("czdlv")).isObject() &&
           pet.value(QStringLiteral("mzdlv")).isObject();
  return repository->hasCachedDetail(petInstanceId(pet));
}

bool hasGap(const PetBattlePowerState& state, const QString& key) {
  for (const PetBattlePowerGap& gap : state.componentGaps)
    if (gap.key == key && gap.gap > 0) return true;
  return false;
}

bool raceAllowed(const ShopExchangeGood& good, const QJsonObject& pet) {
  if (good.raceIds.isEmpty()) return false;
  const int raceId = petRaceId(pet);
  const int metadataRaceId = pet.value(QStringLiteral("_metaRaceId")).toInt();
  return good.raceIds.contains(raceId) ||
         (metadataRaceId > 0 && good.raceIds.contains(metadataRaceId));
}

bool shopCanImprove(const QJsonObject& pet, bool detailAvailable,
                    const QJsonObject& packet, bool hasPacket) {
  if (!hasPacket || !detailAvailable) return false;
  for (const ShopExchangeGood& good : ShopExchangeCatalog::instance().onlineGoods()) {
    if (!raceAllowed(good, pet)) continue;
    if (ShopExchangeCatalog::remainingCount(packet, good) <= 0) continue;
    if (analyzeShopPetEligibility(good, pet, true).state ==
        ShopPetEligibilityState::Usable)
      return true;
  }
  return false;
}

void analyzeRoutine(const RoutineOverviewController* controller,
                    AccountAssetOverview* result) {
  if (!controller || !result) return;
  const QJsonObject daily = controller->dailyPacket();
  result->routineDataKnown = controller->hasDailyPacket() ||
                             !controller->opportunityPackets().isEmpty();
  if (controller->hasDailyPacket()) {
    const QJsonArray day = daily.value(QStringLiteral("ti")).toArray();
    const QJsonArray week = daily.value(QStringLiteral("wti")).toArray();
    int dayIndex = 0;
    int weekIndex = 0;
    for (const RoutineTaskDefinition& task : RoutineOverviewCatalog::instance().tasks()) {
      if (task.dayFinish > 0) {
        const int progress = dayIndex < day.size() ? day.at(dayIndex).toInt() : 0;
        if (progress < task.dayFinish) ++result->unfinishedDailyTasks;
        ++dayIndex;
      }
      if (task.weekFinish > 0) {
        const int progress = weekIndex < week.size() ? week.at(weekIndex).toInt() : 0;
        if (progress < task.weekFinish) ++result->unfinishedWeeklyTasks;
        ++weekIndex;
      }
    }
  }

  const QJsonObject packets = controller->opportunityPackets();
  const QJsonObject star = packets.value(QStringLiteral("1008_20220603_swa_0_0")).toObject();
  if (star.value(QStringLiteral("ti")).isDouble()) {
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining += qMax(0, star.value(QStringLiteral("ti")).toInt());
  }
  if (star.value(QStringLiteral("wgt")).isDouble()) {
    result->weekOpportunityKnown = true;
    result->weekOpportunityRemaining += qMax(0, 6 - star.value(QStringLiteral("wgt")).toInt());
  }
  const QJsonObject arena = packets.value(QStringLiteral("16_24_A")).toObject();
  if (arena.value(QStringLiteral("sweep")).isDouble()) {
    int challenges = 0;
    for (const QString& key : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
      const QJsonObject field = arena.value(key).toObject();
      if (field.value(QStringLiteral("curz")).toInt() > 0)
        challenges += qMax(0, field.value(QStringLiteral("ct")).toInt());
    }
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining += qMax(0, 8 - challenges);
    result->todayOpportunityRemaining +=
        qMax(0, 8 - arena.value(QStringLiteral("sweep")).toInt());
  }
  const QJsonObject fusion = packets.value(QStringLiteral("100_13_0")).toObject();
  if (fusion.value(QStringLiteral("pt")).isDouble()) {
    result->weekOpportunityKnown = true;
    result->weekOpportunityRemaining += qMax(0, 3 - fusion.value(QStringLiteral("pt")).toInt());
  }
  const QJsonObject feed = packets.value(QStringLiteral("100_2_0")).toObject();
  if (feed.value(QStringLiteral("rfc")).isDouble()) {
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining += qMax(0, feed.value(QStringLiteral("rfc")).toInt());
  }
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

}  // namespace

AssetAnalysisController::AssetAnalysisController(
    PetRepository* repository, ShopExchangeController* shopController,
    RoutineOverviewController* routineController, QObject* parent)
    : QObject(parent), repository_(repository), shopController_(shopController),
      routineController_(routineController) {
  if (!repository_) return;
  account_ = repository_->accountKey();
  loadSettings();
  connect(repository_, &PetRepository::dataChanged, this, [this]() {
    emit inventoryChanged();
    emit analysisInvalidated();
  });
  connect(repository_, &PetRepository::detailChanged, this,
          [this](qint64) { emit analysisInvalidated(); });
  connect(repository_, &PetRepository::accountSessionChanged, this,
          &AssetAnalysisController::changeAccount);
  if (shopController_)
    connect(shopController_, &ShopExchangeController::infoUpdated, this,
            &AssetAnalysisController::analysisInvalidated);
  if (shopController_)
    connect(shopController_, &ShopExchangeController::catalogUpdated, this,
            &AssetAnalysisController::analysisInvalidated);
  if (routineController_)
    connect(routineController_, &RoutineOverviewController::dataUpdated, this,
            &AssetAnalysisController::analysisInvalidated);
  if (routineController_)
    connect(routineController_, &RoutineOverviewController::catalogUpdated, this,
            &AssetAnalysisController::analysisInvalidated);
}

AccountInventorySummary AssetAnalysisController::inventorySummary() const {
  AccountInventorySummary result;
  if (!repository_) return result;
  result.account = repository_->accountKey();
  result.inventoryUpdatedAt = repository_->updatedAt();
  QList<QJsonObject> pets = repository_->backpackPets();
  pets.append(repository_->warehousePets());
  QSet<qint64> seen;
  for (const QJsonObject& pet : pets) {
    const qint64 instanceId = petInstanceId(pet);
    if (instanceId <= 0 || seen.contains(instanceId)) continue;
    seen.insert(instanceId);
    ++result.totalPets;
    const QString location = locationText(pet);
    if (location == QStringLiteral("背包")) ++result.backpackPets;
    else if (location == QStringLiteral("精英仓库")) ++result.eliteWarehousePets;
    else ++result.normalWarehousePets;
  }
  return result;
}

AccountAssetOverview AssetAnalysisController::overview() const {
  AccountAssetOverview result;
  if (!repository_) return result;
  result.account = repository_->accountKey();
  result.inventoryUpdatedAt = repository_->updatedAt();
  result.shopDataKnown = shopController_ && shopController_->hasPacket();
  const QJsonObject shopPacket = shopController_ ? shopController_->packet() : QJsonObject{};

  QList<QJsonObject> pets = repository_->backpackPets();
  pets.append(repository_->warehousePets());
  QSet<qint64> seen;
  for (const QJsonObject& brief : pets) {
    const qint64 instanceId = petInstanceId(brief);
    if (instanceId <= 0 || seen.contains(instanceId)) continue;
    seen.insert(instanceId);
    QJsonObject pet = repository_->detailFor(instanceId);
    for (auto iterator = brief.begin(); iterator != brief.end(); ++iterator)
      pet.insert(iterator.key(), iterator.value());

    PetAssetRecord record;
    record.instanceId = instanceId;
    record.raceId = petRaceId(pet);
    record.name = displayName(pet);
    record.location = locationText(pet);
    record.pet = pet;
    record.detailAvailable = hasFullDetail(repository_, pet);
    if (record.detailAvailable) {
      const PetBattlePowerState power = PetDetailAnalyzer::analyzeBattlePower(pet);
      record.currentPower = power.current;
      record.extremePower = power.extreme;
      record.highestPower = power.highest;
      record.completionPercent = power.highest > 0
                                     ? qBound(0, qRound(power.current * 100.0 / power.highest), 100)
                                     : 0;
      record.fullyCultivated = power.isHighest;
      record.redStarMissing = power.hasExtreme && power.missingRedBonus > 0;
      record.astrolabeMissing = !power.breakthrough || hasGap(power, QStringLiteral("asv"));
      record.sacredMissing = hasGap(power, QStringLiteral("sjv"));
      record.soulMissing = hasGap(power, QStringLiteral("bsv"));
      for (const PetBattlePowerGap& gap : power.componentGaps)
        record.gaps.append(QStringLiteral("%1 +%2").arg(gap.label).arg(gap.gap));
      if (record.redStarMissing) record.gaps.append(QStringLiteral("红星未满"));
      if (record.astrolabeMissing && !hasGap(power, QStringLiteral("asv")))
        record.gaps.append(QStringLiteral("星轮未突破"));
      record.improvable = !record.fullyCultivated &&
                          (power.highestGap > 0 || !record.gaps.isEmpty());
      record.shopImprovable = shopCanImprove(pet, true, shopPacket,
                                             result.shopDataKnown);
    }

    ++result.totalPets;
    if (record.location == QStringLiteral("背包")) ++result.backpackPets;
    else if (record.location == QStringLiteral("精英仓库")) ++result.eliteWarehousePets;
    else ++result.normalWarehousePets;
    if (!record.detailAvailable) ++result.missingDetailPets;
    if (record.fullyCultivated) ++result.fullyCultivatedPets;
    if (record.improvable) ++result.improvablePets;
    if (record.redStarMissing) ++result.redStarMissingPets;
    if (record.astrolabeMissing) ++result.astrolabeMissingPets;
    if (record.sacredMissing) ++result.sacredMissingPets;
    if (record.soulMissing) ++result.soulMissingPets;
    if (record.shopImprovable) ++result.shopImprovablePets;
    result.totalCurrentPower += record.currentPower;
    result.pets.append(record);
  }
  std::sort(result.pets.begin(), result.pets.end(),
            [](const PetAssetRecord& left, const PetAssetRecord& right) {
              if (left.completionPercent != right.completionPercent)
                return left.completionPercent < right.completionPercent;
              const int order = QString::localeAwareCompare(left.name, right.name);
              return order == 0 ? left.instanceId < right.instanceId : order < 0;
            });
  analyzeRoutine(routineController_, &result);
  return result;
}

bool AssetAnalysisController::matchesFilter(const PetAssetRecord& pet,
                                            PetAssetFilter filter) {
  switch (filter) {
    case PetAssetFilter::All: return true;
    case PetAssetFilter::Backpack: return pet.location == QStringLiteral("背包");
    case PetAssetFilter::NormalWarehouse:
      return pet.location == QStringLiteral("普通仓库");
    case PetAssetFilter::EliteWarehouse:
      return pet.location == QStringLiteral("精英仓库");
    case PetAssetFilter::FullyCultivated: return pet.fullyCultivated;
    case PetAssetFilter::Improvable: return pet.improvable;
    case PetAssetFilter::MissingDetail: return !pet.detailAvailable;
    case PetAssetFilter::RedStarMissing: return pet.redStarMissing;
    case PetAssetFilter::AstrolabeMissing: return pet.astrolabeMissing;
    case PetAssetFilter::SacredMissing: return pet.sacredMissing;
    case PetAssetFilter::SoulMissing: return pet.soulMissing;
    case PetAssetFilter::ShopImprovable: return pet.shopImprovable;
  }
  return true;
}

QString AssetAnalysisController::accountDirectory() const {
  return repository_ ? QFileInfo(repository_->cachePath()).absolutePath() : QString();
}

QString AssetAnalysisController::settingsPath() const {
  return QDir(accountDirectory()).filePath(QStringLiteral("asset-analysis.json"));
}

QString AssetAnalysisController::snapshotsDirectory() const {
  return QDir(accountDirectory()).filePath(QStringLiteral("snapshots"));
}

void AssetAnalysisController::changeAccount(const QString& account, quint64) {
  account_ = account;
  loadSettings();
  emit inventoryChanged();
  emit analysisInvalidated();
  emit historyChanged();
}

void AssetAnalysisController::loadSettings() {
  autoSnapshotEnabled_ = false;
  const QJsonObject object = readObject(settingsPath());
  if (object.value(QStringLiteral("account")).toString() == account_)
    autoSnapshotEnabled_ = object.value(QStringLiteral("autoSnapshot")).toBool();
}

void AssetAnalysisController::saveSettings() const {
  if (account_.isEmpty()) return;
  writeObject(settingsPath(), {{QStringLiteral("schema"), 1},
                               {QStringLiteral("account"), account_},
                               {QStringLiteral("autoSnapshot"), autoSnapshotEnabled_}});
}

void AssetAnalysisController::setAutoSnapshotEnabled(bool enabled) {
  if (autoSnapshotEnabled_ == enabled) return;
  autoSnapshotEnabled_ = enabled;
  saveSettings();
  emit statusChanged(
      enabled ? QStringLiteral("已启用：每次手动刷新资产分析后同步更新当日快照")
              : QStringLiteral("已关闭：手动刷新分析后同步更新快照"));
}

bool AssetAnalysisController::recordSnapshot() {
  if (!repository_ || account_.isEmpty()) return false;
  return recordSnapshotFromOverview(overview());
}

bool AssetAnalysisController::recordSnapshotFromOverview(
    const AccountAssetOverview& current) {
  if (!repository_ || account_.isEmpty() || current.account != account_) return false;
  if (current.totalPets <= 0) {
    emit statusChanged(QStringLiteral("当前账号没有可记录的精灵资产缓存"));
    return false;
  }
  AccountAssetSnapshot snapshot;
  snapshot.account = account_;
  snapshot.createdAt = QDateTime::currentDateTime();
  snapshot.totalPets = current.totalPets;
  snapshot.fullyCultivatedPets = current.fullyCultivatedPets;
  snapshot.totalCurrentPower = current.totalCurrentPower;
  QJsonArray pets;
  for (const PetAssetRecord& record : current.pets) {
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
    pet.astrolabeBreakthrough =
        record.detailAvailable && !record.astrolabeMissing;
    snapshot.pets.append(pet);
    pets.append(snapshotPetObject(pet));
  }
  const QString path = QDir(snapshotsDirectory())
                           .filePath(snapshot.createdAt.date().toString(
                               QStringLiteral("yyyy-MM-dd.json")));
  const bool saved = writeObject(
      path, {{QStringLiteral("schema"), 1},
             {QStringLiteral("account"), snapshot.account},
             {QStringLiteral("createdAt"), snapshot.createdAt.toString(Qt::ISODate)},
             {QStringLiteral("totalPets"), snapshot.totalPets},
             {QStringLiteral("fullyCultivatedPets"), snapshot.fullyCultivatedPets},
             {QStringLiteral("totalCurrentPower"),
              QString::number(snapshot.totalCurrentPower)},
             {QStringLiteral("pets"), pets}});
  if (!saved) {
    DiagnosticLogger::error(QStringLiteral("snapshot"),
                            QStringLiteral("snapshot atomic write failed"));
    emit statusChanged(QStringLiteral("历史快照写入失败，旧快照保持不变"));
    return false;
  }
  DiagnosticLogger::info(QStringLiteral("snapshot"),
                         QStringLiteral("recorded date=%1 pets=%2")
                             .arg(snapshot.createdAt.date().toString(Qt::ISODate))
                             .arg(snapshot.totalPets));
  emit historyChanged();
  emit statusChanged(QStringLiteral("已记录 %1 的轻量资产快照（同一天再次记录会原子更新）")
                         .arg(snapshot.createdAt.date().toString(QStringLiteral("yyyy-MM-dd"))));
  return true;
}

QList<AccountAssetSnapshot> AssetAnalysisController::snapshots() const {
  QList<AccountAssetSnapshot> result;
  QDir directory(snapshotsDirectory());
  for (const QString& fileName : directory.entryList(
           {QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
    const QJsonObject object = readObject(directory.filePath(fileName));
    if (object.value(QStringLiteral("schema")).toInt() != 1 ||
        object.value(QStringLiteral("account")).toString() != account_)
      continue;
    AccountAssetSnapshot snapshot;
    snapshot.account = account_;
    snapshot.createdAt = QDateTime::fromString(
        object.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
    if (!snapshot.createdAt.isValid()) continue;
    snapshot.totalPets = object.value(QStringLiteral("totalPets")).toInt();
    snapshot.fullyCultivatedPets =
        object.value(QStringLiteral("fullyCultivatedPets")).toInt();
    snapshot.totalCurrentPower =
        object.value(QStringLiteral("totalCurrentPower")).toVariant().toLongLong();
    for (const QJsonValue& value : object.value(QStringLiteral("pets")).toArray())
      if (value.isObject()) snapshot.pets.append(parseSnapshotPet(value.toObject()));
    result.append(snapshot);
  }
  std::sort(result.begin(), result.end(),
            [](const AccountAssetSnapshot& left,
               const AccountAssetSnapshot& right) {
              return left.createdAt < right.createdAt;
            });
  return result;
}

AssetSnapshotDelta AssetAnalysisController::compareSnapshots(
    const AccountAssetSnapshot& current,
    const AccountAssetSnapshot& previous) {
  AssetSnapshotDelta result;
  QHash<qint64, AssetSnapshotPet> oldPets;
  for (const AssetSnapshotPet& pet : previous.pets)
    oldPets.insert(pet.instanceId, pet);
  for (const AssetSnapshotPet& pet : current.pets) {
    if (!oldPets.contains(pet.instanceId)) {
      ++result.newPets;
      continue;
    }
    const AssetSnapshotPet old = oldPets.value(pet.instanceId);
    if (!old.fullyCultivated && pet.fullyCultivated)
      ++result.newlyFullyCultivated;
    if (!old.redStarComplete && pet.redStarComplete)
      ++result.newlyRedStarComplete;
    if (!old.astrolabeBreakthrough && pet.astrolabeBreakthrough)
      ++result.newlyAstrolabeBreakthrough;
  }
  result.totalPowerChange = current.totalCurrentPower - previous.totalCurrentPower;
  return result;
}
