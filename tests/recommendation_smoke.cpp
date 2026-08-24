#include "account_resource_view.h"
#include "recommendation_engine.h"
#include "shop_actionability.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

ShopExchangeGood good(int itemId, const QString& cost = QStringLiteral("4:100:20"),
                      const QString& code = QStringLiteral("41")) {
  ShopExchangeGood result;
  result.shopId = 1;
  result.itemServerId = itemId;
  result.shopName = QStringLiteral("真实测试商店");
  result.description = QStringLiteral("真实项目 %1").arg(itemId);
  result.limitKey = QStringLiteral("d");
  result.limitCount = 3;
  result.cost = cost;
  result.enhanceType = code;
  result.raceIds = {7001};
  return result;
}

QJsonObject packet(int itemId, int used) {
  return {{QStringLiteral("si1"),
           QJsonObject{{QStringLiteral("bi%1").arg(itemId),
                        QJsonObject{{QStringLiteral("d"), used}}}}}};
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
  result.currentPower = 9000;
  result.highestPower = 10000;
  result.soulMissing = currentSoul < 100;
  result.gapKeys = {QStringLiteral("bsv")};
  result.gaps = {QStringLiteral("元魂 +90")};
  result.pet = {{QStringLiteral("id"), id},
                {QStringLiteral("r"), 7001},
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

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  bool ok = true;
  const QHash<QString, qint64> enough{{QStringLiteral("4:100"), 100},
                                     {QStringLiteral("4:200"), 50}};
  const AccountResourceView enoughResources(enough, true);
  const AccountAssetOverview base = overview({pet()});

  QList<ActionRecommendation> result = RecommendationEngine::generate(
      QStringLiteral("account-a"), base, {}, {}, true, enoughResources);
  ok &= require(result.isEmpty(),
                "a missing real shop project produced a fabricated recommendation");

  ShopExchangeGood wrongRace = good(1);
  wrongRace.raceIds = {9999};
  result = RecommendationEngine::generate(QStringLiteral("account-a"), base,
                                           {wrongRace}, {}, true,
                                           enoughResources);
  ok &= require(result.isEmpty(), "a pet-ineligible project was recommended");

  result = RecommendationEngine::generate(QStringLiteral("account-a"), base,
                                           {good(1)}, {}, true,
                                           enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow &&
                    result.constFirst().goodName == QStringLiteral("真实项目 1"),
                "eligible project with enough resources was not ReadyNow");

  ShopExchangeGood multi = good(2, QStringLiteral("4:100:20#4:200:10"));
  result = RecommendationEngine::generate(QStringLiteral("account-a"), base,
                                           {multi}, {}, true,
                                           enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow &&
                    result.constFirst().requirements.size() == 2,
                "multiple satisfied resources were not ReadyNow");

  const AccountResourceView oneMissing(
      {{QStringLiteral("4:100"), 35}, {QStringLiteral("4:200"), 7}}, true);
  result = RecommendationEngine::generate(QStringLiteral("account-a"), base,
                                           {multi}, {}, true, oneMissing);
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
  result = RecommendationEngine::generate(QStringLiteral("account-a"), base,
                                           {multi}, {}, true,
                                           unknownResources);
  const ResourceRequirement* unknown =
      result.isEmpty() ? nullptr
                       : requirement(result.constFirst(), QStringLiteral("4:200"));
  ok &= require(result.size() == 1 &&
                    result.constFirst().type ==
                        RecommendationType::ResourceUnknown &&
                    unknown && !unknown->ownedKnown && unknown->owned == 0,
                "unknown resource was treated as zero or sufficient");

  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), base, {good(1)}, packet(1, 3), true,
      enoughResources);
  ok &= require(result.isEmpty(),
                "zero remaining exchanges produced a ReadyNow recommendation");

  ShopExchangeGood expensive = good(3, QStringLiteral("4:100:120"));
  ShopExchangeGood affordable = good(4, QStringLiteral("4:100:20"));
  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), base, {expensive, affordable}, {}, true,
      enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow &&
                    result.constFirst().shopGoodKey == affordable.stableKey() &&
                    result.constFirst().alternateGoodCount == 1,
                "multiple real projects did not select one best non-duplicate option");

  PetAssetRecord full = pet(1002, 100, 100);
  full.fullyCultivated = true;
  full.improvable = false;
  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), overview({full}), {good(1)}, {}, true,
      enoughResources);
  ok &= require(result.isEmpty(), "fully cultivated pet received a recommendation");

  PetAssetRecord near = pet(1003, 10, 96);
  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), overview({near}), {}, {}, true,
      enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type ==
                        RecommendationType::NearFullCultivation &&
                    result.constFirst().shopGoodKey.isEmpty(),
                "near-full fallback without a real project is incorrect");
  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), overview({near}), {good(1)}, {}, true,
      enoughResources);
  ok &= require(result.size() == 1 &&
                    result.constFirst().type == RecommendationType::ReadyNow,
                "ReadyNow pet was duplicated as near-full");

  PetAssetRecord red = pet(1004, 10, 92);
  red.pet.insert(QStringLiteral("sgs"), QStringLiteral("0:8#80:8:80"));
  red.missingRedStars = 2;
  red.gaps = {QStringLiteral("红色星神缺 2")};
  ShopExchangeGood redGood = good(5, QStringLiteral("4:100:20"),
                                  QStringLiteral("34"));
  redGood.provenGapCode = QStringLiteral("34");
  redGood.provenGapUnitsPerExchange = 1;
  const AccountResourceView oneExchange(
      {{QStringLiteral("4:100"), 20}}, true);
  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), overview({red}), {redGood}, {}, true,
      oneExchange);
  ok &= require(result.size() == 1 && result.constFirst().actionableCountKnown &&
                    result.constFirst().actionableCount == 1 &&
                    result.constFirst().remainingGapAfterAction == 1,
                "proven one-unit exchange quantity was not calculated correctly");
  redGood.provenGapUnitsPerExchange = 0;
  redGood.provenGapCode.clear();
  result = RecommendationEngine::generate(
      QStringLiteral("account-a"), overview({red}), {redGood}, {}, true,
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
      RecommendationEngine::generate(QStringLiteral("account-b"), base,
                                     {good(1)}, {}, true, enoughResources);
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
  const QList<ActionRecommendation> orderA = RecommendationEngine::generate(
      QStringLiteral("stable"), orderingOverview, {good(1)}, {}, true,
      enoughResources);
  const QList<ActionRecommendation> orderB = RecommendationEngine::generate(
      QStringLiteral("stable"), orderingOverview, {good(1)}, {}, true,
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
      RecommendationEngine::generate(
          QStringLiteral("performance"), overview(performancePets), {good(1)},
          {}, true, enoughResources);
  ok &= require(performance.size() == 2000 && timer.elapsed() < 3000,
                "2000-pet recommendation performance regressed");

  if (!ok) return 1;
  std::fprintf(stdout,
               "PASS: real-project recommendation, resources, stale state, ordering, performance\n");
  return 0;
}
