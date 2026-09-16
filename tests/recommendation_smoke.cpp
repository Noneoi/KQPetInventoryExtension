#include "account_resource_view.h"
#include "recommendation_engine.h"
#include "shop_actionability.h"
#include "quota_test_support.h"
#include "pet_detail_catalog.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>
#include <algorithm>
#include <limits>
#include <random>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

// A bare FAIL line cannot separate a stale expectation from a ranking or
// quantity defect, so the deciding row is printed on failure.
void reportRows(const char* label, const QList<ActionRecommendation>& rows) {
  std::fprintf(stderr, "  %s: rows=%lld\n", label, static_cast<long long>(rows.size()));
  for (const ActionRecommendation& row : rows) {
    std::fprintf(stderr,
                 "    type=%d key=%s pet=%lld countKnown=%d count=%d remainingGap=%d closes=%d "
                 "exchangeLeft=%d supportedGap=%d coverageKnown=%d\n",
                 static_cast<int>(row.type), qPrintable(row.shopGoodKey),
                 static_cast<long long>(row.petInstanceId), int(row.actionableCountKnown),
                 row.actionableCount, row.remainingGapAfterAction, int(row.closesKnownGap),
                 row.remainingExchangeCount, row.supportedGapCount, int(row.resourceCoverageKnown));
    const auto state = [](const ShopCondition& condition) {
      return static_cast<int>(condition.effectiveState());
    };
    std::fprintf(stderr, "    conditions: eligibility=%d cost=%d resource=%d limit=%d unlock=%d unknown=%d\n",
                 state(row.eligibilityCondition), state(row.costCondition), state(row.resourceCondition),
                 state(row.limitCondition), state(row.unlockCondition), row.unknownConditionCount);
    std::fprintf(stderr, "    reasons: eligibility='%s' cost='%s' resource='%s' limit='%s' unlock='%s'\n",
                 qPrintable(row.eligibilityCondition.reason), qPrintable(row.costCondition.reason),
                 qPrintable(row.resourceCondition.reason), qPrintable(row.limitCondition.reason),
                 qPrintable(row.unlockCondition.reason));
  }
}

ShopExchangeGood good(int itemId, const QString& cost = QStringLiteral("4:100:20"),
                      const QString& code = QStringLiteral("41")) {
  ShopExchangeGood result;
  result.shopId = 1;
  result.itemServerId = itemId;
  result.shopName = QStringLiteral("真实测试商店");
  result.description = QStringLiteral("真实项目 %1").arg(itemId);
  result.limitKey = QStringLiteral("dl");
  result.limitCount = 3;
  result.cost = cost;
  result.enhanceType = code;
  result.raceIds = {7001};
  return result;
}

QJsonObject packet(int itemId, int used) {
  return {{QStringLiteral("si1"),
           QJsonObject{{QStringLiteral("bi%1").arg(itemId),
                        QJsonObject{{QStringLiteral("dl"), used}}}}}};
}

QJsonObject allCounts() {
  QJsonObject items;
  for (int id = 1; id <= 100; ++id)
    items.insert(QStringLiteral("bi%1").arg(id),
                 QJsonObject{{QStringLiteral("dl"), 0}});
  return {{QStringLiteral("si1"), items}};
}
PetAssetRecord pet(qint64 id = 1001, int currentSoul = 10,
                   int completion = 80) {
  PetAssetRecord result;
  result.instanceId = id;
  result.raceId = 7001;
  result.name = QStringLiteral("测试精灵 %1").arg(id);
  result.detailAvailable = true;
  result.improvable = true;
  result.completionPercent = completion;
  result.completionKnown = true;
  result.powerGapKnown = true;
  result.currentPower = 9000;
  result.highestPower = 10000;
  result.soulMissing = currentSoul < 100;
  result.gapKeys = {QStringLiteral("bsv")};
  result.gaps = {QStringLiteral("元魂 +90")};
  const int badgeMaximum = PetDetailCatalog::instance().badge(101).value(QStringLiteral("maxLevel")).toInt();
  result.pet = {{QStringLiteral("id"), id},
                {QStringLiteral("r"), 7001},
                {QStringLiteral("badge"),QStringLiteral("101:%1").arg(currentSoul >= 100 ? badgeMaximum : qMax(0,badgeMaximum-1))},
                {QStringLiteral("czdlv"),
                 QJsonObject{{QStringLiteral("bsv"), currentSoul}}},
                {QStringLiteral("mzdlv"),
                 QJsonObject{{QStringLiteral("bsv"), 100}}}};
  return result;
}

AccountAssetOverview overview(const QList<PetAssetRecord>& pets) {
  AccountAssetOverview result;
  result.account = QStringLiteral("account-a");
  result.pets = pets;
  result.totalPets = pets.size();
  return result;
}

const ResourceRequirement* requirement(
    const ActionRecommendation& recommendation, const QString& key) {
  for (const ResourceRequirement& value : recommendation.requirements)
    if (value.resourceKey == key) return &value;
  return nullptr;
}

QStringList ids(const QList<ActionRecommendation>& recommendations) {
  QStringList result;
  for (const ActionRecommendation& recommendation : recommendations)
    result.append(recommendation.stableId);
  return result;
}

QList<ActionRecommendation> generateWithSyntheticPeriod(
    const QString& account, const AccountAssetOverview& overview,
    const QList<ShopExchangeGood>& goods, const QJsonObject& packet,
    bool packetKnown, const AccountResourceView& resources, const ShopConditionContext& context = {}) {
  return RecommendationEngine::generate(account, overview, goods, packet, packetKnown, resources,
      syntheticQuotaContext(goods, context));
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  bool ok = true;
  const QHash<QString, qint64> enough{{QStringLiteral("4:100"), 100},
                                     {QStringLiteral("4:200"), 50}};
  const AccountResourceView enoughResources(enough, true);
  const AccountAssetOverview base = overview({pet()});

  QList<ActionRecommendation> result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), base, {}, allCounts(), true, enoughResources);
  ok &= require(result.isEmpty(),
                "a missing real shop project produced a fabricated recommendation");

  ShopExchangeGood wrongRace = good(1);
  wrongRace.raceIds = {9999};
  result = generateWithSyntheticPeriod(QStringLiteral("account-a"), base,
                                           {wrongRace}, allCounts(), true,
                                           enoughResources);
  ok &= require(result.isEmpty(), "a pet-ineligible project was recommended");

  result = generateWithSyntheticPeriod(QStringLiteral("account-a"), base,
                                           {good(1)}, allCounts(), true,
                                           enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow &&
                    result.constFirst().goodName == QStringLiteral("真实项目 1"),
                "eligible project with enough resources was not ReadyNow");

  ShopExchangeGood multi = good(2, QStringLiteral("4:100:20#4:200:10"));
  result = generateWithSyntheticPeriod(QStringLiteral("account-a"), base,
                                           {multi}, allCounts(), true,
                                           enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow &&
                    result.constFirst().requirements.size() == 2,
                "multiple satisfied resources were not ReadyNow");

  const AccountResourceView oneMissing(
      {{QStringLiteral("4:100"), 35}, {QStringLiteral("4:200"), 7}}, true);
  result = generateWithSyntheticPeriod(QStringLiteral("account-a"), base,
                                           {multi}, allCounts(), true, oneMissing);
  const ResourceRequirement* missing =
      result.isEmpty() ? nullptr
                       : requirement(result.constFirst(), QStringLiteral("4:200"));
  ok &= require(result.size() == 1 &&
                    result.constFirst().type ==
                        RecommendationType::ResourceMissing &&
                    missing && missing->owned == 7 && missing->required == 10 &&
                    missing->missing() == 3,
                "resource deficit or per-resource missing count is incorrect");

  const AccountResourceView unknownResources(
      {{QStringLiteral("4:100"), 100}}, true);
  result = generateWithSyntheticPeriod(QStringLiteral("account-a"), base,
                                           {multi}, allCounts(), true,
                                           unknownResources);
  const ResourceRequirement* unknown =
      result.isEmpty() ? nullptr
                       : requirement(result.constFirst(), QStringLiteral("4:200"));
  ok &= require(result.size() == 1 &&
                    result.constFirst().type ==
                        RecommendationType::ResourceUnknown &&
                    unknown && !unknown->ownedKnown && unknown->owned == 0,
                "unknown resource was treated as zero or sufficient");

  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), base, {good(1)}, packet(1, 3), true,
      enoughResources);
  ok &= require(result.isEmpty(),
                "zero remaining exchanges produced a ReadyNow recommendation");

  ShopExchangeGood expensive = good(3, QStringLiteral("4:100:120"));
  ShopExchangeGood affordable = good(4, QStringLiteral("4:100:20"));
  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), base, {expensive, affordable}, allCounts(), true,
      enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow &&
                    result.constFirst().shopGoodKey == affordable.stableKey() &&
                    result.constFirst().alternateGoodCount == 1,
                "multiple real projects did not select one best non-duplicate option");

  PetAssetRecord full = pet(1002, 100, 100);
  full.fullyCultivated = true;
  full.improvable = false;
  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), overview({full}), {good(1)}, allCounts(), true,
      enoughResources);
  ok &= require(result.isEmpty(), "fully cultivated pet received a recommendation");

  PetAssetRecord near = pet(1003, 10, 96);
  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), overview({near}), {}, allCounts(), true,
      enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type ==
                        RecommendationType::NearFullCultivation &&
                    result.constFirst().shopGoodKey.isEmpty(),
                "near-full fallback without a real project is incorrect");
  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), overview({near}), {good(1)}, allCounts(), true,
      enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow,
                "ReadyNow pet was duplicated as near-full");

  PetAssetRecord red = pet(1004, 10, 92);
  // The record mirrors what a complete detail yields, and the raw reply must
  // carry the slot sequence and the per-pet stargod backpack: without them the
  // red-star offer cannot be confirmed as a real purchase gap at all.
  red.missingRedStars = 2;
  red.stargodSlotsKnown = true;
  red.pet.insert(QStringLiteral("sgs"), QStringLiteral("0:8#0:8"));
  red.pet.insert(QStringLiteral("sgsp"), QJsonArray{});
  red.gaps = {QStringLiteral("红色星神缺 2")};
  ShopExchangeGood redGood = good(5, QStringLiteral("4:100:20"),
                                  QStringLiteral("34"));
  redGood.provenGapCode = QStringLiteral("34");
  redGood.provenGapUnitsPerExchange = 1;
  const AccountResourceView oneExchange(
      {{QStringLiteral("4:100"), 20}}, true);
  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), overview({red}), {redGood}, allCounts(), true,
      oneExchange);
  const bool oneUnitQuantity = result.size() == 1 && result.constFirst().actionableCountKnown &&
      result.constFirst().actionableCount == 1 && result.constFirst().remainingGapAfterAction == 1;
  if (!oneUnitQuantity) reportRows("proven one-unit exchange", result);
  ok &= require(oneUnitQuantity, "proven one-unit exchange quantity was not calculated correctly");
  redGood.provenGapUnitsPerExchange = 0;
  redGood.provenGapCode.clear();
  result = generateWithSyntheticPeriod(
      QStringLiteral("account-a"), overview({red}), {redGood}, allCounts(), true,
      oneExchange);
  ok &= require(result.size() == 1 &&
                    !result.constFirst().actionableCountKnown,
                "ambiguous exchange semantics guessed a resolvable quantity");

  const QList<ActionRecommendation> preserved = result;
  const QList<ActionRecommendation> cultivationStale =
      RecommendationEngine::applyFreshness(preserved, true, false);
  ok &= require(cultivationStale.size() == preserved.size() &&
                    cultivationStale.constFirst().stale &&
                    cultivationStale.constFirst().cultivationStale,
                "cultivation stale did not preserve and mark old recommendations");
  QList<ActionRecommendation> mixed = preserved;
  ActionRecommendation plainNear;
  plainNear.type = RecommendationType::NearFullCultivation;
  plainNear.stableId = QStringLiteral("near-full:account-a:9");
  mixed.append(plainNear);
  const QList<ActionRecommendation> shopStale =
      RecommendationEngine::applyFreshness(mixed, false, true);
  ok &= require(shopStale.at(0).shopStale && shopStale.at(0).stale &&
                    !shopStale.at(1).shopStale && !shopStale.at(1).stale,
                "shop stale affected non-shop near-full advice");

  const QList<ActionRecommendation> otherAccount =
      generateWithSyntheticPeriod(QStringLiteral("account-b"), base,
                                     {good(1)}, allCounts(), true, enoughResources);
  ok &= require(otherAccount.size() == 1 &&
                    otherAccount.constFirst().stableId.startsWith(
                        QStringLiteral("shop:account-b:")) &&
                    !otherAccount.constFirst().stableId.contains(
                        QStringLiteral("account-a")),
                "recommendation identity leaked across accounts");

  QList<PetAssetRecord> orderingPets;
  for (int index = 0; index < 20; ++index)
    orderingPets.append(pet(2000 + index, 10, 80 + index % 10));
  const AccountAssetOverview orderingOverview = overview(orderingPets);
  const QList<ActionRecommendation> orderA = generateWithSyntheticPeriod(
      QStringLiteral("stable"), orderingOverview, {good(1)}, allCounts(), true,
      enoughResources);
  const QList<ActionRecommendation> orderB = generateWithSyntheticPeriod(
      QStringLiteral("stable"), orderingOverview, {good(1)}, allCounts(), true,
      enoughResources);
  ok &= require(ids(orderA) == ids(orderB) && orderA.size() == 20,
                "same input did not produce stable complete output");

  QList<PetAssetRecord> performancePets;
  performancePets.reserve(2000);
  for (int index = 0; index < 2000; ++index)
    performancePets.append(pet(100000 + index, 10, 80 + index % 20));
  QElapsedTimer timer;
  timer.start();
  const QList<ActionRecommendation> performance =
      generateWithSyntheticPeriod(
          QStringLiteral("performance"), overview(performancePets), {good(1)},
          allCounts(), true, enoughResources);
  ok &= require(performance.size() == 2000 && timer.elapsed() < 3000,
                "2000-pet recommendation performance regressed");

  const auto recommend = [&](const ShopExchangeGood& project,
                             const AccountResourceView& counts,
                             const ShopConditionContext& context = {}) {
    return generateWithSyntheticPeriod(QStringLiteral("audit"), base,
        {project}, allCounts(), true, counts, context);
  };
  const auto isUnknown = [](const QList<ActionRecommendation>& rows) {
    return rows.size() == 1 &&
           rows.constFirst().type == RecommendationType::ConditionUnknown;
  };

  // Formal regressions for all three pre-v2 audit reproductions.
  const ShopExchangeGood repeated = good(1, QStringLiteral("4:100:60#4:100:60"));
  result = recommend(repeated, enoughResources);
  ok &= require(result.size() == 1 &&
                    result.first().type == RecommendationType::ResourceMissing &&
                    result.first().requirements.size() == 1 &&
                    result.first().requirements.first().required == 120 &&
                    result.first().requirements.first().missing() == 20 &&
                    result.first().resourceCoverageMillionths == 833333,
                "duplicate resource costs were not aggregated before checking funds");
  result = recommend(good(1, QStringLiteral("4:100:20#not-a-cost")), enoughResources);
  ok &= require(isUnknown(result) && result.first().requirements.isEmpty() &&
                    !result.first().resourceCoverageKnown,
                "a partially malformed cost was accepted as a smaller complete cost");
  ShopExchangeGood locked = good(1);
  locked.unlock = QStringLiteral("EBFLevel$4");
  result = recommend(locked, enoughResources);
  ok &= require(isUnknown(result) &&
                    result.first().unlockCondition.state == ShopConditionState::Unknown,
                "an unverified unlock expression was ignored or guessed");

  for (const QString& cost : {QString{}, QStringLiteral(" "),
      QStringLiteral("4:100:0"), QStringLiteral("4:100:-1"),
      QStringLiteral("4:100:1.5"), QStringLiteral("4:100:1e2"),
      QStringLiteral("4:100:20:"), QStringLiteral("4:100:20#"),
      QStringLiteral("4:100:20##4:200:1"), QStringLiteral("0:100:20"),
      QStringLiteral("4:0:20"), QStringLiteral("2147483648:100:20"),
      QStringLiteral("4:100:9223372036854775808"),
      QStringLiteral("4:100:9223372036854775807#4:100:1")}) {
    result = recommend(good(1, cost), enoughResources);
    ok &= require(isUnknown(result) && !result.first().resourceCoverageKnown,
                  "invalid cost or checked-add overflow became known / free");
  }
  result = recommend(good(1, QStringLiteral("4:100:60|4:100:60")), enoughResources);
  ok &= require(result.size() == 1 && result.first().requirements.size() == 1 &&
                    result.first().requirements.first().required == 120,
                "supported alternate separator bypassed duplicate aggregation");

  const qint64 maxCount = std::numeric_limits<qint64>::max();
  result = recommend(good(1, QStringLiteral("4:100:9223372036854775807")),
      AccountResourceView({{QStringLiteral("4:100"), maxCount - 1}}, true));
  ok &= require(result.size() == 1 &&
                    result.first().type == RecommendationType::ResourceMissing &&
                    result.first().resourceCoverageMillionths == 999999 &&
                    result.first().requirements.first().missing() == 1,
                "maximum cost arithmetic overflowed or coverage rounded up to one");
  result = recommend(good(1),
      AccountResourceView({{QStringLiteral("4:100"), -1}}, true));
  ok &= require(isUnknown(result), "negative resource balance was trusted");

  const AccountResourceView mixedBalances({{QStringLiteral("4:100"), 0}}, true);
  result = recommend(multi, mixedBalances);
  ok &= require(isUnknown(result) && result.first().requirements.first().missing() == 20 &&
                    result.first().requirements.first().condition.state ==
                        ShopConditionState::Blocked,
                "mixed known shortage and unknown balance lost a condition or got the wrong class");
  const QDateTime observation = QDateTime::fromString(
      QStringLiteral("2026-09-09T00:00:00Z"), Qt::ISODate);
  ShopConditionContext verified;
  verified.verifiedUnlockFacts.insert(locked.unlock,
      {ShopConditionState::Satisfied, QStringLiteral("测试规则已验证"),
       QStringLiteral("synthetic-verified-rule"), observation,
       ShopConditionFreshness::Current});
  result = recommend(locked, enoughResources, verified);
  ok &= require(result.size() == 1 && result.first().type == RecommendationType::ReadyNow,
                "a supplied verified passing unlock fact did not satisfy the condition");
  verified.verifiedUnlockFacts[locked.unlock].state = ShopConditionState::Blocked;
  result = recommend(locked, unknownResources, verified);
  ok &= require(result.isEmpty(), "a known locked project remained a candidate");
  verified.verifiedUnlockFacts[locked.unlock].freshness = ShopConditionFreshness::Invalidated;
  result = recommend(locked, enoughResources, verified);
  ok &= require(isUnknown(result), "an expired unlock fact was still authoritative");

  result = generateWithSyntheticPeriod(QStringLiteral("audit"), base, {good(1)},
      {}, true, enoughResources);
  ok &= require(isUnknown(result) && result.first().remainingExchangeCount == -1,
                "missing limit field was treated as zero used exchanges");
  for (const QJsonValue& count : {QJsonValue(-1), QJsonValue(0.5),
       QJsonValue(QStringLiteral("bad")), QJsonValue(QStringLiteral("1.0")),
       QJsonValue(2147483648.0), QJsonValue(QJsonValue::Null), QJsonValue(true)}) {
    const QJsonObject invalidPacket{{QStringLiteral("si1"),
        QJsonObject{{QStringLiteral("bi1"), QJsonObject{{QStringLiteral("dl"), count}}}}}};
    ok &= require(ShopExchangeCatalog::remainingCount(invalidPacket, good(1)) == -1,
                  "malformed, fractional or overflowing use count was accepted");
  }
  ShopConditionContext staleLimits;
  staleLimits.shopFreshness = ShopConditionFreshness::Invalidated;
  result = recommend(good(1), enoughResources, staleLimits);
  ok &= require(isUnknown(result), "invalidated period limits became ReadyNow");
  result = generateWithSyntheticPeriod(QStringLiteral("audit"), base, {good(1)},
      allCounts(), false, enoughResources);
  ok &= require(isUnknown(result), "unknown shop source suppressed the real pending candidate");

  ShopExchangeGood free = good(1, {});
  free.provenFree = true;
  free.provenUnlimited = true;
  result = generateWithSyntheticPeriod(QStringLiteral("audit"), base, {free},
      {}, false, AccountResourceView{});
  ok &= require(result.size() == 1 && result.first().type == RecommendationType::ReadyNow,
                "explicit verified free and unlimited contracts were not recognized");

  result = generateWithSyntheticPeriod(QStringLiteral("audit"), base, {multi},
      packet(2, 3), true, unknownResources);
  ok &= require(result.isEmpty(), "known exhaustion did not exclude mixed unknown conditions");
  const AccountResourceView staleResources(enough, true, observation,
      QStringLiteral("synthetic-material-observation"), true);
  result = recommend(good(1), staleResources);
  ok &= require(isUnknown(result) && !result.first().resourceCoverageKnown &&
                    result.first().requirements.first().condition.observedAt == observation,
                "invalidated resources were treated as current or lost observation evidence");
  ShopConditionContext stalePet;
  stalePet.petFreshness = ShopConditionFreshness::Invalidated;
  result = recommend(good(1), enoughResources, stalePet);
  ok &= require(isUnknown(result) && result.first().supportedGapCount == 0 &&
                    !result.first().completionKnown && !result.first().powerGapKnown,
                "invalidated pet eligibility remained actionable or participated in known ranking");

  for (const QJsonValue& currentSoul : {QJsonValue(QJsonValue::Null),
       QJsonValue(QStringLiteral("bad")),
       QJsonValue(-1), QJsonValue(0.5), QJsonValue(true)}) {
    PetAssetRecord malformed = pet();
    malformed.pet.insert(QStringLiteral("czdlv"),
        QJsonObject{{QStringLiteral("bsv"), currentSoul}});
    malformed.pet.insert(QStringLiteral("badge"),currentSoul);
    result = generateWithSyntheticPeriod(QStringLiteral("audit"), overview({malformed}),
        {good(1)}, allCounts(), true, enoughResources);
    ok &= require(isUnknown(result), "invalid badge cultivation value became a zero / useful gap");
  }

  redGood.provenGapCode = QStringLiteral("34");
  redGood.provenGapUnitsPerExchange = std::numeric_limits<int>::max();
  result = generateWithSyntheticPeriod(QStringLiteral("audit"), overview({red}),
      {redGood}, allCounts(), true, enoughResources);
  ok &= require(result.size() == 1 && !result.first().actionableCountKnown,
                "unsupported large exchange-unit declaration was trusted or overflowed");
  redGood.provenGapUnitsPerExchange = 1;
  ShopExchangeGood wide = good(6, QStringLiteral("4:100:20"), QStringLiteral("34-41"));
  result = generateWithSyntheticPeriod(QStringLiteral("audit"), overview({red}),
      {wide, redGood}, allCounts(), true, enoughResources);
  const bool provenClosureRanked = result.size() == 1 && result.first().shopGoodKey == redGood.stableKey() &&
      result.first().closesKnownGap && result.first().actionableCount == 2;
  if (!provenClosureRanked) reportRows("proven gap closure ranking", result);
  ok &= require(provenClosureRanked, "proven gap closure did not rank before broader unquantified coverage");

  const auto readyBeforeStale = recommend(good(1), enoughResources);
  const auto invalidatedReady = RecommendationEngine::applyFreshness(readyBeforeStale, false, true);
  ok &= require(isUnknown(invalidatedReady) && !invalidatedReady.first().actionableCountKnown &&
                    !invalidatedReady.first().resourceCoverageKnown &&
                    invalidatedReady.first().remainingExchangeCount == -1 &&
                    readyBeforeStale.first().type == RecommendationType::ReadyNow,
                "stale application kept ReadyNow or mutated the saved current result");

  // Comparing raw units from different materials produces the wrong winner.
  const ShopExchangeGood nearlyFunded = good(11, QStringLiteral("4:100:1000"));
  const ShopExchangeGood barelyFunded = good(12, QStringLiteral("4:200:2"));
  result = generateWithSyntheticPeriod(QStringLiteral("ranking"), base,
      {barelyFunded, nearlyFunded}, allCounts(), true,
      AccountResourceView({{QStringLiteral("4:100"), 900},
                           {QStringLiteral("4:200"), 1}}, true));
  ok &= require(result.size() == 1 && result.first().shopGoodKey == nearlyFunded.stableKey() &&
                    result.first().resourceCoverageMillionths == 900000,
                "resource ranking compared incomparable raw material deficits");

  QList<PetAssetRecord> numericOrder{pet(10), pet(2)};
  result = generateWithSyntheticPeriod(QStringLiteral("ranking"), overview(numericOrder),
      {good(1)}, allCounts(), true, enoughResources);
  ok &= require(result.size() == 2 && result.first().petInstanceId == 2,
                "numeric pet identity tie-break used lexicographic text order");
  PetAssetRecord unknownCompletion = pet(1, 10, 99);
  unknownCompletion.completionKnown = false;
  unknownCompletion.powerGapKnown = false;
  result = generateWithSyntheticPeriod(QStringLiteral("ranking"),
      overview({unknownCompletion, pet(2, 10, 10)}), {good(1)}, allCounts(), true,
      enoughResources);
  ok &= require(result.size() == 2 && result.first().petInstanceId == 2,
                "unknown completion participated as a known high sorting value");
  result = generateWithSyntheticPeriod(QStringLiteral("ranking"),
      overview({unknownCompletion}), {}, allCounts(), true, enoughResources);
  // Unknown completion keeps the evidenced local suggestion and must never turn
  // into a near-full claim with a confirmed completion percentage.
  const bool unknownCompletionRow = result.size() == 1 &&
      result.constFirst().type == RecommendationType::LocalCultivation &&
      !result.constFirst().completionKnown && !result.constFirst().powerGapKnown;
  if (!unknownCompletionRow) reportRows("unknown completion", result);
  ok &= require(unknownCompletionRow, "unknown completion generated a near-full claim");

  QList<ShopExchangeGood> shuffledGoods{good(1), good(2), good(3)};
  const auto beforeShuffle = generateWithSyntheticPeriod(QStringLiteral("shuffle"),
      overview(orderingPets), shuffledGoods, allCounts(), true, enoughResources);
  std::mt19937 random(20260909);
  std::shuffle(orderingPets.begin(), orderingPets.end(), random);
  std::shuffle(shuffledGoods.begin(), shuffledGoods.end(), random);
  const auto afterShuffle = generateWithSyntheticPeriod(QStringLiteral("shuffle"),
      overview(orderingPets), shuffledGoods, allCounts(), true, enoughResources);
  ok &= require(ids(beforeShuffle) == ids(afterShuffle),
                "input permutation changed best projects or final recommendation order");
  QList<ActionRecommendation> orderCases;
  for (int index = 0; index < 80; ++index) {
    ActionRecommendation value;
    value.type = static_cast<RecommendationType>(index % 4);
    value.petInstanceId = index % 9;
    value.shopGoodKey = QString::number(index % 3);
    value.completionKnown = index % 2;
    value.completionPercent = index % 101;
    value.powerGapKnown = index % 3;
    value.highestPower = index * 30;
    value.currentPower = index * 10;
    value.closesKnownGap = index % 3 == 0;
    value.supportedGapCount = index % 5;
    value.unknownConditionCount = index % 4;
    value.resourceCoverageKnown = index % 2;
    value.resourceCoverageMillionths = index * 10000;
    orderCases.append(value);
  }
  for (const auto& a : orderCases) {
    ok &= require(!RecommendationEngine::less(a, a), "ranking was not irreflexive");
    for (const auto& b : orderCases) {
      ok &= require(!(RecommendationEngine::less(a, b) && RecommendationEngine::less(b, a)),
                    "ranking was not asymmetric");
      for (const auto& c : orderCases) {
        if (RecommendationEngine::less(a, b) && RecommendationEngine::less(b, c))
          ok &= require(RecommendationEngine::less(a, c), "ranking was not transitive");
        if (!RecommendationEngine::less(a, b) && !RecommendationEngine::less(b, a) &&
            !RecommendationEngine::less(b, c) && !RecommendationEngine::less(c, b))
          ok &= require(!RecommendationEngine::less(a, c) && !RecommendationEngine::less(c, a),
                        "ranking equivalence was not transitive");
      }
    }
  }

  // Structural corruption is rejected as one catalog; valid items in the same
  // document must not hide an invalid sibling or erase a known unlock rule.
  QFile embedded(QStringLiteral(":/kqpet/shop-exchange-data.json"));
  ok &= require(embedded.open(QIODevice::ReadOnly), "embedded catalog test input unavailable");
  const QJsonObject originalCatalog = QJsonDocument::fromJson(embedded.readAll()).object();
  ShopExchangeCatalog& liveCatalog = ShopExchangeCatalog::instance();
  const auto originalGoods = liveCatalog.onlineGoods(QDate(2026, 9, 9));
  const auto tryCatalog = [&](const QJsonObject& object) {
    QString error;
    return static_cast<bool>(ShopExchangeCatalog::prepare(object, QStringLiteral("fixture"), {}, &error));
  };
  for (int corruption = 0; corruption < 3; ++corruption) {
    QJsonObject broken = originalCatalog;
    QJsonArray shops = broken.value(QStringLiteral("shops")).toArray();
    QJsonObject firstShop = shops.first().toObject();
    QJsonArray goods = firstShop.value(QStringLiteral("goods")).toArray();
    if (corruption == 0) goods.append(QJsonValue(42));
    else {
      QJsonObject firstGood = goods.first().toObject();
      if (corruption == 1) firstGood.insert(QStringLiteral("unlock"), QJsonArray{4});
      else firstGood.insert(QStringLiteral("raceIds"), QJsonArray{7001, 1.5});
      goods[0] = firstGood;
    }
    firstShop.insert(QStringLiteral("goods"), goods);
    shops[0] = firstShop;
    broken.insert(QStringLiteral("shops"), shops);
    ok &= require(!tryCatalog(broken) &&
                      liveCatalog.onlineGoods(QDate(2026, 9, 9)).size() == originalGoods.size(),
                  "a partially corrupt external catalog replaced the last-known-good snapshot");
  }

  // A pet with an owned-star action keeps both its shop row and its local
  // cultivation row, so the real upper bound is one row per pet plus one extra
  // row for each of those pets. The container accounting and the budget check
  // must cover that bound instead of assuming exactly one row per pet.
  {
    QList<PetAssetRecord> twins;
    for (qint64 id = 1; id <= 4; ++id) {
      PetAssetRecord record = pet(id, 10, 80);
      // One empty ordinary slot plus one owned red star produces the
      // "已有星神待装备/调整" action, which is the only path that adds a second
      // row next to the shop row for the same pet.
      record.pet.insert(QStringLiteral("sgs"), QStringLiteral("0:8"));
      record.pet.insert(QStringLiteral("sgsp"), QJsonArray{66});
      record.pet.insert(QStringLiteral("stargodSlotMaxLevel"), 8);
      twins.append(record);
    }
    const QList<ShopExchangeGood> goods{good(1), good(2)};
    const auto prepared = prepareWithSyntheticPeriod(CompiledShopCatalog::compile(goods),
        allCounts(), AccountResourceView(enough, true));
    const PetMetadataView catalogView(PetDetailCatalog::instance().snapshot());
    const ShopPetMetadataSnapshot metadata{catalogView.stargodDefinitions(),
        catalogView.astrolabeDefinitions(), catalogView.petDefinitions(),
        catalogView.sacredStarPlans(), catalogView.sacredStagePlans(),
        catalogView.badgeDefinitions()};
    const auto runSession = [&](quint64 budget, QList<ActionRecommendation>* rows,
                                AccountAssetOverview* taken, RecommendationSliceStats* timing,
                                RecommendationSession::Status* status) {
      AccountAssetOverview input = overview(twins);
      RecommendationSession session(QStringLiteral("account-a"), input, prepared, metadata, nullptr, true);
      while (session.step(nullptr, 16, 4096, budget) == RecommendationSession::Status::Running) {}
      *status = session.status();
      *rows = session.takeResults();
      *taken = session.takeOverview();
      *timing = session.sliceStats();
    };
    QList<ActionRecommendation> rows;
    AccountAssetOverview taken;
    RecommendationSliceStats timing;
    RecommendationSession::Status status = RecommendationSession::Status::Running;
    runSession(8ULL * 1024 * 1024, &rows, &taken, &timing, &status);
    int shopRows = 0, localRows = 0;
    for (const auto& row : rows) {
      if (row.shopGoodKey.isEmpty()) ++localRows; else ++shopRows;
    }
    if (!(status == RecommendationSession::Status::Complete && rows.size() == 8 &&
          shopRows == 4 && localRows == 4 && taken.pets.size() == 4)) {
      std::fprintf(stderr,
          "  two-row batch: status=%d rows=%lld shop=%d local=%d overviewPets=%lld "
          "capacityBytes=%llu chargedBytes=%llu rowHeap=%llu overviewHeap=%llu\n",
          static_cast<int>(status), static_cast<long long>(rows.size()), shopRows, localRows,
          static_cast<long long>(taken.pets.size()), timing.resultRowCapacityBytes,
          timing.resultChargedBytes, timing.resultRowHeapBytes, timing.overviewHeapBytes);
    }
    ok &= require(status == RecommendationSession::Status::Complete && rows.size() == 8 &&
                      shopRows == 4 && localRows == 4 && taken.pets.size() == 4,
                  "two rows per pet were not published with all P overview rows kept");
    const quint64 rowBytes = quint64(rows.size()) * sizeof(ActionRecommendation);
    // The documented breakdown splits the overview charge into its own capacity
    // and heap parts, so the identity includes both.
    ok &= require(timing.resultRowCapacityBytes >= rowBytes &&
                      timing.resultChargedBytes == timing.resultRowHeapBytes +
                          timing.resultRowCapacityBytes + timing.overviewHeapBytes +
                          timing.overviewCapacityBytes,
                  "the shared result accounting did not follow the real row count or container capacity");
    // A budget that only fits one row per pet must reject the task outright
    // instead of publishing a half materialized result.
    QList<ActionRecommendation> smallRows;
    AccountAssetOverview smallTaken;
    RecommendationSliceStats smallTiming;
    RecommendationSession::Status smallStatus = RecommendationSession::Status::Running;
    runSession(quint64(sizeof(ActionRecommendation)) + 4096, &smallRows, &smallTaken,
               &smallTiming, &smallStatus);
    ok &= require(smallStatus == RecommendationSession::Status::ResultBudgetExceeded &&
                      smallRows.isEmpty(),
                  "a two-row-per-pet result published rows after exceeding the result budget");
  }

  if (!ok) return 1;
  std::fprintf(stdout,
               "PASS: real-project recommendation, resources, stale state, ordering, performance\n");
  return 0;
}
