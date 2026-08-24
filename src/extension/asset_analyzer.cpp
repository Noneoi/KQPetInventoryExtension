#include "asset_analyzer.h"

#include "pet_detail_analyzer.h"
#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_repository.h"
#include "routine_overview_catalog.h"
#include "routine_overview_controller.h"
#include "shop_exchange_catalog.h"
#include "shop_exchange_controller.h"
#include "shop_pet_eligibility.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace {

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
  const QJsonObject tree = packets.value(QStringLiteral("1008_20190531_gbt_1")).toObject();
  if (tree.value(QStringLiteral("ti")).isDouble()) {
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining += qMax(0, tree.value(QStringLiteral("ti")).toInt());
  }
  const QJsonObject beast = packets.value(QStringLiteral("2_36_1")).toObject();
  if (beast.value(QStringLiteral("t")).isDouble()) {
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining += qMax(0, beast.value(QStringLiteral("t")).toInt());
  }
  const QJsonObject competition = packets.value(QStringLiteral("110_123_0")).toObject();
  if (competition.value(QStringLiteral("rdt")).isDouble()) {
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining +=
        qMax(0, 40 + competition.value(QStringLiteral("rdb")).toInt() -
                    competition.value(QStringLiteral("rdt")).toInt());
  }
  if (competition.value(QStringLiteral("rwwt")).isDouble()) {
    result->weekOpportunityKnown = true;
    result->weekOpportunityRemaining +=
        qMax(0, competition.value(QStringLiteral("rwwt")).toInt());
  }
  const QJsonObject farm = packets.value(QStringLiteral("1008_20260522_nf_0")).toObject();
  if (farm.value(QStringLiteral("pt")).isDouble()) {
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining += qMax(0, farm.value(QStringLiteral("pt")).toInt());
  }
  const QJsonObject arena = packets.value(QStringLiteral("16_24_A")).toObject();
  for (const QString& field : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
    const QJsonObject info = arena.value(field).toObject();
    if (!info.value(QStringLiteral("ct")).isDouble() ||
        !info.value(QStringLiteral("bct")).isDouble())
      continue;
    result->todayOpportunityKnown = true;
    result->todayOpportunityRemaining +=
        qMax(0, 8 - info.value(QStringLiteral("ct")).toInt() +
                    info.value(QStringLiteral("bct")).toInt());
  }
}

}  // namespace

AssetAnalyzer::AssetAnalyzer(PetRepository* repository,
                             ShopExchangeController* shopController,
                             RoutineOverviewController* routineController)
    : repository_(repository), shopController_(shopController),
      routineController_(routineController) {}

AccountInventorySummary AssetAnalyzer::inventorySummary() const {
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
    if (!hasFullDetail(repository_, pet)) ++result.missingDetailPets;
  }
  return result;
}

InventorySignature AssetAnalyzer::inventorySignature() const {
  InventorySignature result;
  if (!repository_) return result;
  result.account = repository_->accountKey();
  QList<QJsonObject> pets = repository_->backpackPets();
  pets.append(repository_->warehousePets());
  for (const QJsonObject& pet : pets) {
    const qint64 instanceId = petInstanceId(pet);
    if (instanceId <= 0 || result.locations.contains(instanceId)) continue;
    result.locations.insert(instanceId, locationText(pet));
  }
  return result;
}

AccountAssetOverview AssetAnalyzer::analyze() const {
  ++analysisRunCount_;
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

AccountAssetOverview AssetAnalyzer::routineSummary() const {
  AccountAssetOverview result;
  if (repository_) result.account = repository_->accountKey();
  analyzeRoutine(routineController_, &result);
  return result;
}

void AssetAnalyzer::updateRoutineSummary(AccountAssetOverview* overview) const {
  if (!overview) return;
  overview->routineDataKnown = false;
  overview->unfinishedDailyTasks = 0;
  overview->unfinishedWeeklyTasks = 0;
  overview->todayOpportunityKnown = false;
  overview->weekOpportunityKnown = false;
  overview->todayOpportunityRemaining = 0;
  overview->weekOpportunityRemaining = 0;
  analyzeRoutine(routineController_, overview);
}

bool AssetAnalyzer::matchesFilter(const PetAssetRecord& pet,
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
