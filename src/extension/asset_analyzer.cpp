#include "asset_analyzer.h"

#include "pet_power_calculator.h"
#include "packet_contract.h"
#include "recommendation_engine.h"
#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_repository.h"
#include "routine_overview_catalog.h"
#include "routine_overview_controller.h"
#include "shop_exchange_catalog.h"
#include "shop_exchange_controller.h"
#include "shop_pet_eligibility.h"
#include "../domain/pet_metadata_view.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <limits>

namespace {

QString displayName(const QJsonObject& pet, const PetMetadataView& metadata) {
  QString name = pet.value(QStringLiteral("customName")).toString().trimmed();
  if (name.isEmpty()) name = pet.value(QStringLiteral("n")).toString().trimmed();
  if (name.isEmpty()) name = metadata.resolvedOriginalName(pet);
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
  return repository->recordVersion(petInstanceId(pet)).complete &&
      !pet.value(QStringLiteral("_visualMismatch")).toBool();
}

void analyzeRoutine(const RoutineOverviewController* controller,
                    AccountAssetOverview* result) {
  if (!controller || !result) return;
  const QJsonObject daily = controller->dailyPacket();
  const auto validity = controller->periodValiditySnapshot();
  const auto current = [&validity](const QString& key) { return validity.value(key).current(); };
  result->routineDataKnown = controller->hasDailyPacket() ||
                             !controller->opportunityPackets().isEmpty();
  if (controller->hasDailyPacket()) {
    const QJsonArray day = daily.value(QStringLiteral("ti")).toArray();
    const QJsonArray week = daily.value(QStringLiteral("wti")).toArray();
    int dayIndex = 0, weekIndex = 0, unfinishedDay = 0, unfinishedWeek = 0;
    bool dayKnown = current(QStringLiteral("ti:daily")), weekKnown = current(QStringLiteral("wti:weekly"));
    for (const RoutineTaskDefinition& task : RoutineOverviewCatalog::instance().snapshot()->tasks) {
      qint64 progress = 0;
      if (task.dayFinish > 0) {
        if (dayIndex >= day.size() || !PacketContracts::checkedInteger(day.at(dayIndex), &progress, 0, INT_MAX)) dayKnown = false;
        else if (progress < task.dayFinish) ++unfinishedDay;
        ++dayIndex;
      }
      if (task.weekFinish > 0) {
        if (weekIndex >= week.size() || !PacketContracts::checkedInteger(week.at(weekIndex), &progress, 0, INT_MAX)) weekKnown = false;
        else if (progress < task.weekFinish) ++unfinishedWeek;
        ++weekIndex;
      }
    }
    result->dailyTasksKnown = dayKnown;
    result->weeklyTasksKnown = weekKnown;
    if (dayKnown) result->unfinishedDailyTasks = unfinishedDay;
    if (weekKnown) result->unfinishedWeeklyTasks = unfinishedWeek;
  }

  const QJsonObject packets = controller->opportunityPackets();
  bool dayOverflow = false, weekOverflow = false;
  auto add = [&](qint64 amount, bool weekly) {
    auto& total = weekly ? result->weekOpportunityRemaining : result->todayOpportunityRemaining;
    auto& known = weekly ? result->weekOpportunityKnown : result->todayOpportunityKnown;
    auto& overflow = weekly ? weekOverflow : dayOverflow;
    if (overflow) return;
    const qint64 next = qint64(total) + qMax<qint64>(0, amount);
    if (next > INT_MAX) { total = 0; known = false; overflow = true; }
    else { total = static_cast<int>(next); known = true; }
  };
  auto number = [](const QJsonObject& object, const char* field, qint64* value) {
    return PacketContracts::checkedInteger(object.value(QLatin1String(field)), value, 0, INT_MAX);
  };
  qint64 value = 0, bonus = 0;
  const auto star = packets.value(QStringLiteral("1008_20220603_swa_0_0")).toObject();
  if (current(QStringLiteral("1008_20220603_swa_0_0:activity"))) {
    if (number(star, "ti", &value)) add(value, false);
    if (number(star, "wgt", &value)) add(6 - value, true);
  }
  for (const auto& pair : QList<QPair<QString, const char*>>{
       {QStringLiteral("1008_20190531_gbt_1"), "ti"}, {QStringLiteral("2_36_1"), "t"},
       {QStringLiteral("1008_20260522_nf_0"), "pt"}})
    if (current(pair.first + QStringLiteral(":activity")) && number(packets.value(pair.first).toObject(), pair.second, &value)) add(value, false);
  const auto competition = packets.value(QStringLiteral("110_123_0")).toObject();
  if (current(QStringLiteral("110_123_0:activity"))) {
    if (number(competition, "rdt", &value) && number(competition, "rdb", &bonus)) add(40 + bonus - value, false);
    if (number(competition, "rwwt", &value)) add(value, true);
  }
  const auto arena = packets.value(QStringLiteral("16_24_A")).toObject();
  for (const auto& field : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
    const auto info = arena.value(field).toObject();
    if (current(QStringLiteral("16_24_A:activity")) && number(info, "ct", &value) && number(info, "bct", &bonus)) add(8 - value + bonus, false);
  }

}

}  // namespace

AssetAnalyzer::AssetAnalyzer(PetRepository* repository,
                             ShopExchangeController* shopController,
                             RoutineOverviewController* routineController)
    : repository_(repository), shopController_(shopController),
      routineController_(routineController) {}

PetAssetRecord AssetAnalyzer::summarySeed(const QJsonObject& brief, bool complete,
                                         bool sourceKnown, const PetMetadataView& metadata) {
  PetAssetRecord seed;
  seed.instanceId = petInstanceId(brief); seed.raceId = petRaceId(brief);
  seed.name = displayName(brief, metadata); seed.location = locationText(brief);
  seed.pet = AssetDerivation::identityFields(brief);
  seed.detailAvailable = complete && !brief.value(QStringLiteral("_visualMismatch")).toBool();
  seed.observationVerified = sourceKnown && !brief.value(QStringLiteral("_unverifiedObservation")).toBool();
  qint64 level = 0;
  if (PacketContracts::checkedInteger(metadata.metadataFor(brief).value(QStringLiteral("stargodSlotMaxLevel")),
                                      &level, 1, std::numeric_limits<int>::max())) seed.metadataSlotMaxLevel = int(level);
  return seed;
}

AccountInventorySummary AssetAnalyzer::inventorySummary() const {
  AccountInventorySummary result;
  if (!repository_) return result;
  result.account = repository_->accountKey();
  result.inventoryUpdatedAt = repository_->updatedAt();
  for (const auto instanceId : repository_->currentInstanceIds()) {
    const auto pet = repository_->briefFor(instanceId);
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
  for (const auto instanceId : repository_->currentInstanceIds()) {
    const auto pet = repository_->briefFor(instanceId);
    result.locations.insert(instanceId, locationText(pet));
  }
  return result;
}

AccountAssetOverview AssetAnalyzer::captureInput(std::shared_ptr<const PetDetailCatalogSnapshot> snapshot) const {
  const PetMetadataView metadata(snapshot ? std::move(snapshot) : PetDetailCatalog::instance().snapshot());
  AccountAssetOverview input;
  if (!repository_) return input;
  input.account = repository_->accountKey();
  input.inputSessionEpoch = repository_->sessionGeneration();
  input.inventoryRevision = repository_->inventoryRevision();
  input.sourceVerified = repository_->sessionContext().canPersist();
  input.inventoryUpdatedAt = repository_->updatedAt();
  input.shopDataKnown = shopController_ && shopController_->hasPacket();
  QList<QJsonObject> briefs = repository_->backpackBriefs();
  briefs.append(repository_->warehouseBriefs());
  input.pets.reserve(briefs.size());
  QSet<qint64> seen;
  for (const QJsonObject& brief : briefs) {
    const qint64 id = petInstanceId(brief);
    if (id <= 0 || seen.contains(id)) continue;
    seen.insert(id);
    const QJsonObject detail = repository_->detailFor(id);
    QJsonObject required = AssetDerivation::analysisInputFields(brief, detail);
    PetAssetRecord pet;
    pet.instanceId = id; pet.raceId = petRaceId(required);
    pet.name = displayName(required, metadata); pet.location = locationText(required);
    pet.detailAvailable = hasFullDetail(repository_, required);
    pet.observationVerified = input.sourceVerified &&
        !brief.value(QStringLiteral("_unverifiedObservation")).toBool() &&
        !detail.value(QStringLiteral("_unverifiedObservation")).toBool();
    qint64 level = 0;
    if (PacketContracts::checkedInteger(metadata.metadataFor(required)
          .value(QStringLiteral("stargodSlotMaxLevel")), &level, 1, std::numeric_limits<int>::max()))
      pet.metadataSlotMaxLevel = static_cast<int>(level);
    pet.pet = required;
    input.pets.append(std::move(pet));
  }
  input.sourceVerified = input.sourceVerified && std::all_of(input.pets.cbegin(), input.pets.cend(),
      [](const PetAssetRecord& pet) { return pet.observationVerified; });
  // The small routine summary is a frozen input value, independent of the
  // expensive per-pet cultivation and product-pair computation below.
  analyzeRoutine(routineController_, &input);
  return input;
}

AccountAssetOverview AssetAnalyzer::analyze() const {
  ++analysisRunCount_;
  AccountAssetOverview input = captureInput();
  const auto catalog = CompiledShopCatalog::compile(ShopExchangeCatalog::instance().onlineGoods());
  const AccountResourceView resources(shopController_ ? shopController_->materialCounts() : QHash<QString, qint64>{},
      input.sourceVerified && shopController_ && shopController_->hasMaterialCounts(), {}, {}, !input.sourceVerified);
  ShopConditionContext context;
  context.shopPacketKnown = input.shopDataKnown;
  context.petFreshness = ShopConditionFreshness::Current;
  context.shopFreshness = input.sourceVerified ? ShopConditionFreshness::Current : ShopConditionFreshness::Invalidated;
  if (shopController_) context.quotaValidity = shopController_->quotaValiditySnapshot();
  const auto prepared = PreparedShopConditions::prepare(catalog,
      shopController_ ? shopController_->packet() : QJsonObject{}, resources, context);
  const PetMetadataView metadata(PetDetailCatalog::instance().snapshot());
  RecommendationSession calculation(input.account, input, prepared,
      {metadata.stargodDefinitions(), metadata.astrolabeDefinitions(), metadata.petDefinitions(),
       metadata.sacredStarPlans(), metadata.sacredStagePlans(), metadata.badgeDefinitions()}, nullptr, true);
  while (calculation.step() == RecommendationSession::Status::Running) {}
  return calculation.takeOverview();
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
  overview->dailyTasksKnown = false;
  overview->weeklyTasksKnown = false;
  overview->unfinishedDailyTasks = 0;
  overview->unfinishedWeeklyTasks = 0;
  overview->todayOpportunityKnown = false;
  overview->weekOpportunityKnown = false;
  overview->todayOpportunityRemaining = 0;
  overview->weekOpportunityRemaining = 0;
  analyzeRoutine(routineController_, overview);
}
