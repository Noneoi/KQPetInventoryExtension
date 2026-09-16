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
#include <functional>
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
  // One entry per independent contribution. A source is validated on its own:
  // a valid field in one packet says nothing about that packet's other fields.
  // A group this analysis never covered is not counted as zero, and it does not
  // keep the period permanently partial either.
  struct Source {
    QString label;
    QString group;
    bool weekly = false;
    bool expected = false;
    bool confirmed = false;
    qint64 amount = 0;
    QDateTime observedAt;
    QString pendingReason;
  };
  QList<Source> sources;
  auto evaluate = [&](const QString& label, const QString& group, bool weekly,
                      const std::function<bool(qint64*)>& compute) {
    Source entry;
    entry.label = label;
    entry.group = group;
    entry.weekly = weekly;
    entry.expected = controller->hasRecordedState(group);
    if (entry.expected) {
      const PacketFieldState state = controller->fieldState(group);
      if (state != PacketFieldState::Value && state != PacketFieldState::Empty)
        entry.pendingReason = state == PacketFieldState::Invalid ? QStringLiteral("来源数据无效")
                                                                 : QStringLiteral("来源未返回");
      else if (!current(group + QStringLiteral(":activity")))
        entry.pendingReason = QStringLiteral("周期未确认");
      else if (state == PacketFieldState::Empty) {
        entry.amount = 0;  // Explicitly empty is a confirmed zero, not missing.
        entry.confirmed = true;
        entry.observedAt = controller->observedAt(group);
      } else {
        qint64 value = 0;
        if (compute(&value)) {
          entry.amount = value;
          entry.confirmed = true;
          entry.observedAt = controller->observedAt(group);
        } else {
          entry.pendingReason = QStringLiteral("来源数据无效");
        }
      }
    }
    sources.append(entry);
  };
  auto number = [](const QJsonObject& object, const char* field, qint64* value) {
    return PacketContracts::checkedInteger(object.value(QLatin1String(field)), value, 0, INT_MAX);
  };
  qint64 value = 0;
  const auto star = packets.value(QStringLiteral("1008_20220603_swa_0_0")).toObject();
  evaluate(QStringLiteral("星轮探险（今日）"), QStringLiteral("1008_20220603_swa_0_0"), false,
      [&](qint64* result) { return number(star, "ti", result); });
  evaluate(QStringLiteral("星轮探险（本周）"), QStringLiteral("1008_20220603_swa_0_0"), true,
      [&](qint64* result) {
        if (!number(star, "wgt", result)) return false;
        *result = qMax<qint64>(0, 6 - *result);
        return true;
      });
  const auto tree = packets.value(QStringLiteral("1008_20190531_gbt_1")).toObject();
  evaluate(QStringLiteral("缤纷树（今日）"), QStringLiteral("1008_20190531_gbt_1"), false,
      [&](qint64* result) { return number(tree, "ti", result); });
  const auto beast = packets.value(QStringLiteral("2_36_1")).toObject();
  evaluate(QStringLiteral("源兽之门（今日）"), QStringLiteral("2_36_1"), false,
      [&](qint64* result) { return number(beast, "t", result); });
  const auto farm = packets.value(QStringLiteral("1008_20260522_nf_0")).toObject();
  evaluate(QStringLiteral("最新版农场（今日）"), QStringLiteral("1008_20260522_nf_0"), false,
      [&](qint64* result) { return number(farm, "pt", result); });
  const auto competition = packets.value(QStringLiteral("110_123_0")).toObject();
  evaluate(QStringLiteral("全民斗技（今日）"), QStringLiteral("110_123_0"), false,
      [&](qint64* result) {
        qint64 used = 0, bought = 0;
        if (!number(competition, "rdt", &used) || !number(competition, "rdb", &bought)) return false;
        *result = qMax<qint64>(0, 40 + bought - used);
        return true;
      });
  evaluate(QStringLiteral("全民斗技（本周）"), QStringLiteral("110_123_0"), true,
      [&](qint64* result) { return number(competition, "rwwt", result); });
  // The arena counters are never actively queried, so each is part of the
  // expectation only while the game itself returns that group.
  const auto arena = packets.value(QStringLiteral("16_24_A")).toObject();
  for (const auto& field : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
    const auto info = arena.value(field).toObject();
    evaluate(field == QStringLiteral("zao1") ? QStringLiteral("经典竞技场（今日）")
                                             : QStringLiteral("传奇竞技场（今日）"),
        QStringLiteral("16_24_A:") + field, false, [&](qint64* result) {
          qint64 used = 0, bought = 0;
          if (!number(info, "ct", &used) || !number(info, "bct", &bought)) return false;
          *result = qMax<qint64>(0, 8 + bought - used);
          return true;
        });
  }

  const auto summarize = [&sources](bool weekly, RoutineOpportunitySummary* summary) {
    qint64 total = 0;
    bool overflow = false;
    for (const Source& entry : sources) {
      if (entry.weekly != weekly || !entry.expected) continue;
      ++summary->expectedSources;
      if (!entry.confirmed) {
        summary->pendingSources.append(entry.pendingReason.isEmpty()
            ? entry.label : entry.label + QStringLiteral("：") + entry.pendingReason);
        continue;
      }
      ++summary->confirmedSources;
      const qint64 next = total + entry.amount;
      if (next > INT_MAX || next < 0) overflow = true;
      else total = next;
      if (!summary->observedAt.isValid() || summary->observedAt < entry.observedAt)
        summary->observedAt = entry.observedAt;
    }
    // An overflowed sum is neither a total nor a reliable lower bound.
    summary->total = overflow ? 0 : static_cast<int>(total);
    summary->completeness = overflow ? RoutineCompleteness::Overflow
        : summary->expectedSources == 0 || summary->confirmedSources == 0
            ? RoutineCompleteness::Unknown
            : summary->confirmedSources == summary->expectedSources
                ? RoutineCompleteness::Complete : RoutineCompleteness::Partial;
    if (summary->pendingSources.isEmpty()) summary->pendingSources.clear();
  };
  summarize(false, &result->todayOpportunities);
  summarize(true, &result->weekOpportunities);
  result->todayOpportunityRemaining = result->todayOpportunities.total;
  result->weekOpportunityRemaining = result->weekOpportunities.total;
  result->todayOpportunityKnown =
      result->todayOpportunities.completeness == RoutineCompleteness::Complete;
  result->weekOpportunityKnown =
      result->weekOpportunities.completeness == RoutineCompleteness::Complete;
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
  overview->todayOpportunities = {};
  overview->weekOpportunities = {};
  analyzeRoutine(routineController_, overview);
}
