#include "quota_test_support.h"
#include "compiled_shop_catalog.h"
#include "prepared_shop_conditions.h"
#include "../src/application/recommendation_adapter.h"
#include "pet_detail_catalog.h"
#include "../src/domain/shop_limit_facts.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cstdio>
#include <random>

namespace {

bool require(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}

ShopPetMetadataSnapshot syntheticMetadata() {
  ShopPetMetadataSnapshot value;
  value.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}}};
  value.astrolabe = {{QStringLiteral("350"),QJsonObject{{QStringLiteral("exclusive"),false},{QStringLiteral("isTBD"),false}}}};
  return value;
}

ShopExchangeGood syntheticGood(int index, int race, int itemCount = 3) {
  ShopExchangeGood good;
  good.shopId = 1;
  good.itemServerId = index + 1;
  good.limitKey = QStringLiteral("dl");
  good.limitCount = 3;
  good.description = QStringLiteral("synthetic project %1").arg(index);
  good.enhanceType = QStringLiteral("41-62-84");
  good.raceIds = {race};
  QStringList cost;
  for (int item = 0; item < itemCount; ++item)
    cost.append(QStringLiteral("4:%1:%2").arg(100 + item).arg(20 + item));
  good.cost = cost.join(QLatin1Char('#'));
  return good;
}

PetAssetRecord syntheticPet(int index, int race, bool hasDetail = true) {
  PetAssetRecord pet;
  pet.instanceId = 100000 + index;
  pet.raceId = race;
  pet.detailAvailable = hasDetail;
  pet.improvable = true;
  pet.completionPercent = 80;
  pet.completionKnown = true;
  pet.powerGapKnown = true;
  pet.currentPower = 8000;
  pet.highestPower = 10000;
  pet.stargodSlotsKnown = true;
  pet.missingRedStars = 1;
  QJsonObject current, maximum;
  for (const QString& key : {QStringLiteral("lv"), QStringLiteral("sgv"),
       QStringLiteral("bsv"), QStringLiteral("iv"), QStringLiteral("asv")}) {
    current.insert(key, 10);
    maximum.insert(key, 100);
  }
  pet.pet = {{QStringLiteral("id"), pet.instanceId}, {QStringLiteral("r"), race},
      {QStringLiteral("czdlv"), current}, {QStringLiteral("mzdlv"), maximum},
      {QStringLiteral("sgs"), QStringLiteral("0:8")},
      {QStringLiteral("sgsp"), QJsonArray{}},
      {QStringLiteral("badge"), QStringLiteral("101:5#201:0")},
      {QStringLiteral("astrolabe"), QStringLiteral("350:0:0")},
      {QStringLiteral("shenjue"), QStringLiteral("1034#2#6|1:1")}};
  return pet;
}

AccountAssetOverview overview(const QList<PetAssetRecord>& pets) {
  AccountAssetOverview result;
  result.account = QStringLiteral("pipeline-synthetic");
  result.pets = pets;
  result.totalPets = pets.size();
  return result;
}

QJsonObject packetFor(const QList<ShopExchangeGood>& goods) {
  QJsonObject packet;
  for (const ShopExchangeGood& good : goods) {
    // Activity-local numeric IDs are not standard siN/biM quota identities.
    // Synthesizing them here can overwrite an unrelated standard shop item.
    if (!good.sourceKey.isEmpty()) continue;
    const QString shopKey = QStringLiteral("si%1").arg(good.shopId);
    QJsonObject shop = packet.value(shopKey).toObject();
    shop.insert(good.itemKey(), QJsonObject{{good.limitKey, 0}});
    packet.insert(shopKey, shop);
  }
  return packet;
}

AccountResourceView resourcesFor(const CompiledShopCatalog& catalog) {
  QHash<QString, qint64> counts;
  for (const CompiledShopGood& good : catalog.goods())
    for (const ResourceRequirement& requirement : good.requirements)
      counts.insert(requirement.resourceKey, qMax<qint64>(1000000, requirement.required));
  return AccountResourceView(counts, true);
}

QStringList identities(const QList<ActionRecommendation>& rows) {
  QStringList result;
  for (const ActionRecommendation& row : rows)
    result.append(row.stableId + QLatin1Char('|') +
                  QString::number(static_cast<int>(row.type)) + QLatin1Char('|') +
                  QString::number(row.alternateGoodCount));
  return result;
}

qint64 matchingPairs(const QList<PetAssetRecord>& pets,
                     const QList<ShopExchangeGood>& goods) {
  qint64 count = 0;
  for (const PetAssetRecord& pet : pets) {
    if (!pet.detailAvailable || pet.fullyCultivated || !pet.improvable) continue;
    const int race = pet.pet.value(QStringLiteral("r")).toInt();
    const int original = pet.pet.value(QStringLiteral("_metaRaceId")).toInt();
    for (const ShopExchangeGood& good : goods)
      count += race <= 0 || good.raceIds.contains(race) ||
               (original > 0 && good.raceIds.contains(original));
  }
  return count;
}

void report(const QString& workload, qsizetype petCount, qsizetype goodsCount,
            const AlgorithmPipelineStats& stats, qint64 elapsed) {
  const QJsonObject result{{QStringLiteral("workload"), workload},
      {QStringLiteral("pets"), petCount}, {QStringLiteral("goods"), goodsCount},
      {QStringLiteral("costParses"), stats.costExpressionsParsed},
      {QStringLiteral("conditionPreparations"), stats.accountConditionsPrepared},
      {QStringLiteral("petDerivations"), stats.petsDerived},
      {QStringLiteral("H"), stats.candidatePairsVisited},
      {QStringLiteral("candidateComponents"), stats.candidateComponentsChecked},
      {QStringLiteral("materializedRows"), stats.finalRowsMaterialized},
      {QStringLiteral("maximumLiveCandidates"), stats.maximumLiveCandidates},
      {QStringLiteral("elapsedMsInformational"), elapsed}};
  std::fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}

bool syntheticWorkload(const QString& label, int petCount, int goodsCount,
                       int matchesPerPet, int detailPercent, int itemCount = 3,
                       int componentCount = 3) {
  QList<ShopExchangeGood> goods;
  const int groups = matchesPerPet > 0 ? goodsCount / matchesPerPet : goodsCount;
  const QStringList codes{QStringLiteral("41"), QStringLiteral("62"), QStringLiteral("84"),
      QStringLiteral("11"), QStringLiteral("31"), QStringLiteral("34"),
      QStringLiteral("44"), QStringLiteral("91"), QStringLiteral("92"), QStringLiteral("999")};
  for (int index = 0; index < goodsCount; ++index) {
    ShopExchangeGood good = syntheticGood(index, 7000 + (matchesPerPet > 0
        ? index / matchesPerPet : index), itemCount);
    good.enhanceType = codes.mid(0, componentCount).join(QLatin1Char('-'));
    goods.append(good);
  }
  QList<PetAssetRecord> pets;
  for (int index = 0; index < petCount; ++index)
    pets.append(syntheticPet(index, matchesPerPet == 0 ? 900000 : 7000 + index % groups,
                            index % 100 < detailPercent));
  AlgorithmPipelineStats stats;
  QElapsedTimer elapsed;
  elapsed.start();
  const CompiledShopCatalog compiled = CompiledShopCatalog::compile(goods, {}, &stats);
  const PreparedShopConditions prepared = prepareWithSyntheticPeriod(
      compiled, packetFor(goods), resourcesFor(compiled), {}, &stats);
  const auto rows = RecommendationEngine::generatePrepared(QStringLiteral("synthetic"),
      overview(pets), prepared, syntheticMetadata(), &stats);
  const qint64 activePets = petCount / 100 * detailPercent;
  const qint64 expectedH = activePets * matchesPerPet;
  bool ok = require(stats.goodsCompiled == goodsCount &&
      stats.costExpressionsParsed == goodsCount &&
      stats.costFragmentsParsed == qint64(goodsCount) * itemCount &&
      stats.enhancementExpressionsParsed == goodsCount &&
      stats.enhancementComponentsCompiled == qint64(goodsCount) * componentCount &&
      stats.accountConditionsPrepared == goodsCount &&
      stats.resourceBalancesChecked == qint64(goodsCount) * itemCount,
      "catalog parsing or account condition preparation scaled with pet count");
  ok &= require(stats.petsDerived == activePets &&
      stats.cultivationComponentsDerived == activePets * kShopCultivationComponentCount &&
      stats.candidatePairsVisited == expectedH &&
      stats.candidateComponentsChecked == expectedH * componentCount,
      "pet derivation or candidate checks did not follow P + H");
  ok &= require(stats.finalRowsMaterialized == rows.size() &&
      rows.size() == (matchesPerPet == 0 ? 0 : activePets) &&
      stats.maximumLiveCandidates <= 2,
      "candidate table / display text grew with H instead of selected results");
  if (!rows.isEmpty())
    ok &= require(rows.first().type == RecommendationType::ReadyNow &&
        rows.first().alternateGoodCount == matchesPerPet - 1,
        "indexed candidates changed the best category or alternate count");
  report(label, petCount, goodsCount, stats, elapsed.elapsed());
  const auto cachedRows = RecommendationEngine::generatePrepared(QStringLiteral("synthetic"),
      overview(pets), prepared, syntheticMetadata(), &stats);
  ok &= require(identities(rows) == identities(cachedRows) &&
      stats.costExpressionsParsed == goodsCount &&
      stats.accountConditionsPrepared == goodsCount &&
      stats.petsDerived == activePets * 2 &&
      stats.candidatePairsVisited == expectedH * 2,
      "reusing prepared snapshots reparsed catalog / balances or changed output");
  return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  bool ok = true;
  const auto standardQuotaGood = syntheticGood(0,7000);
  auto activityQuotaGood = standardQuotaGood;
  activityQuotaGood.sourceKey = QStringLiteral("synthetic/activity/overlapping-numeric-ids");
  activityQuotaGood.limitKey.clear();
  const auto standardQuotaPacket = packetFor({standardQuotaGood});
  const auto mixedQuotaPacket = packetFor({standardQuotaGood,activityQuotaGood});
  ok &= require(mixedQuotaPacket == standardQuotaPacket &&
      shopRemainingCount(mixedQuotaPacket,standardQuotaGood) == standardQuotaGood.limitCount,
      "activity-local IDs polluted the frozen standard-shop quota fixture");
  for (int coverage : {0, 50, 100}) {
    ok &= syntheticWorkload(QStringLiteral("zero-%1").arg(coverage), 400, 200, 0, coverage);
    ok &= syntheticWorkload(QStringLiteral("sparse5-%1").arg(coverage), 400, 200, 5, coverage);
    ok &= syntheticWorkload(QStringLiteral("full-%1").arg(coverage), 400, 200, 200, coverage);
  }
  ok &= syntheticWorkload(QStringLiteral("normal10"), 400, 200, 10, 100);
  ok &= syntheticWorkload(QStringLiteral("one-cost"), 200, 200, 5, 100, 1);
  ok &= syntheticWorkload(QStringLiteral("ten-costs"), 200, 200, 5, 100, 10);
  ok &= syntheticWorkload(QStringLiteral("one-component"), 200, 200, 5, 100, 3, 1);
  ok &= syntheticWorkload(QStringLiteral("ten-components"), 200, 200, 5, 100, 3, 10);

  QList<ShopExchangeGood> goods{syntheticGood(0, 7000), syntheticGood(1, 7001),
                               syntheticGood(2, 7002)};
  goods[0].raceIds = {7000, 7000, 7001};
  PetAssetRecord skin = syntheticPet(0, 900001);
  skin.pet.insert(QStringLiteral("_metaRaceId"), 7000);
  PetAssetRecord both = syntheticPet(1, 7001);
  both.pet.insert(QStringLiteral("_metaRaceId"), 7000);
  const QList<PetAssetRecord> skins{skin, both};
  AlgorithmPipelineStats skinStats;
  const CompiledShopCatalog compiled = CompiledShopCatalog::compile(goods, {}, &skinStats);
  ok &= require(skinStats.raceAssociationsIndexed == 4,
                "duplicate race references produced duplicate index entries");
  const PreparedShopConditions prepared = prepareWithSyntheticPeriod(
      compiled, packetFor(goods), resourcesFor(compiled), {}, &skinStats);
  QList<ShopExchangeGood> mutableGoods = goods;
  mutableGoods[0].cost = QStringLiteral("not-a-cost");
  mutableGoods[0].raceIds.clear();
  ok &= require(compiled.goods().first().good.cost == goods.first().cost &&
      compiled.goodsForRace(7000).size() == 1 &&
      prepared.goods().first().account.resourcesEnough,
      "compiled/prepared snapshot changed after input catalog mutation");
  const auto skinRows = RecommendationEngine::generatePrepared(QStringLiteral("skins"),
      overview(skins), prepared, {}, &skinStats);
  ok &= require(skinRows.size() == 2 && skinStats.candidatePairsVisited == 3 &&
      matchingPairs(skins, goods) == skinStats.candidatePairsVisited,
      "rare skin metadata race was lost or dual-race union counted a good twice");

  PetAssetRecord unknownRace = syntheticPet(2, 0);
  AlgorithmPipelineStats unknownStats;
  const auto unknownRows = RecommendationEngine::generatePrepared(QStringLiteral("unknown"),
      overview({unknownRace}), prepared, {}, &unknownStats);
  ok &= require(unknownRows.size() == 1 &&
      unknownRows.first().type == RecommendationType::ConditionUnknown &&
      unknownRows.first().alternateGoodCount == 2 &&
      unknownStats.candidatePairsVisited == 3,
      "unknown race was silently eliminated by an index that cannot prove non-matches");

  ShopConditionContext invalidated;
  invalidated.petFreshness = ShopConditionFreshness::Invalidated;
  const auto stalePrepared = prepareWithSyntheticPeriod(
      compiled, packetFor(goods), resourcesFor(compiled), invalidated);
  AlgorithmPipelineStats staleStats;
  const auto staleRows = RecommendationEngine::generatePrepared(QStringLiteral("stale"),
      overview({skin}), stalePrepared, {}, &staleStats);
  ok &= require(staleRows.size() == 1 && staleRows.first().alternateGoodCount == 2 &&
      staleStats.candidatePairsVisited == 3,
      "invalidated identity gained false exclusion authority from the index");

  // Metadata is a frozen value: no runtime singleton is read by derivation.
  QJsonObject mutableDefinitions{{QStringLiteral("9"),
      QJsonObject{{QStringLiteral("quality"), 5}, {QStringLiteral("changeable"), false},
          {QStringLiteral("type"), 1}, {QStringLiteral("limitJobs"), QJsonArray{}},
          {QStringLiteral("battlePower"), QJsonObject{{QStringLiteral("1"), 100}}}}}};
  const ShopPetMetadataSnapshot frozenMetadata{mutableDefinitions};
  auto changedStar = mutableDefinitions.value(QStringLiteral("9")).toObject();
  changedStar.insert(QStringLiteral("quality"), 6);
  mutableDefinitions.insert(QStringLiteral("9"), changedStar);
  QJsonObject starPet{{QStringLiteral("r"), 7000},
                      {QStringLiteral("stargodSlotMaxLevel"), 1},
                      {QStringLiteral("sgsp"), QJsonArray{}},
                      {QStringLiteral("sgs"), QStringLiteral("9:1")}};
  const auto starRule = compileShopPetRule(QStringLiteral("34"));
  ok &= require(evaluateShopPetRule(starRule, deriveShopPet(starPet, true, frozenMetadata)).state ==
                    ShopPetEligibilityState::Usable &&
      evaluateShopPetRule(starRule, deriveShopPet(starPet, true,
          ShopPetMetadataSnapshot{mutableDefinitions})).state == ShopPetEligibilityState::NotUsable,
      "frozen metadata was overwritten or derivation consulted the runtime singleton");

  std::mt19937 random(20260909);
  const auto beforeShuffle = RecommendationEngine::generatePrepared(QStringLiteral("order"),
      overview(skins), prepared, {});
  std::shuffle(goods.begin(), goods.end(), random);
  QList<PetAssetRecord> shuffledPets = skins;
  std::shuffle(shuffledPets.begin(), shuffledPets.end(), random);
  const auto reorderedCatalog = CompiledShopCatalog::compile(goods);
  const auto reordered = prepareWithSyntheticPeriod(reorderedCatalog,
      packetFor(goods), resourcesFor(reorderedCatalog));
  const auto afterShuffle = RecommendationEngine::generatePrepared(QStringLiteral("order"),
      overview(shuffledPets), reordered, {});
  ok &= require(identities(beforeShuffle) == identities(afterShuffle),
                "catalog / pet permutation changed indexed selection or final ordering");

  const QList<ShopExchangeGood> realGoods =
      ShopExchangeCatalog::instance().onlineGoods(QDate(2026, 9, 9));
  ok &= require(!realGoods.isEmpty(), "actual embedded shop catalog was unavailable");
  QList<PetAssetRecord> actualPets;
  for (int index = 0; index < 2000 && !realGoods.isEmpty(); ++index) {
    const ShopExchangeGood& project = realGoods[index % realGoods.size()];
    const int race = project.raceIds[index % project.raceIds.size()];
    PetAssetRecord pet = syntheticPet(index, index % 2 == 0 ? race : 900000 + index);
    if (index % 2) pet.pet.insert(QStringLiteral("_metaRaceId"), race);
    actualPets.append(pet);
  }
  AlgorithmPipelineStats actualStats;
  QElapsedTimer actualElapsed;
  actualElapsed.start();
  const auto actualCatalog = CompiledShopCatalog::compile(realGoods, {}, &actualStats);
  const auto actualPacket = packetFor(realGoods);
  const auto actualResources = resourcesFor(actualCatalog);
  const auto actualPrepared = prepareWithSyntheticPeriod(actualCatalog,
      actualPacket, actualResources, {}, &actualStats);
  const ShopPetMetadataSnapshot actualMetadata{PetDetailCatalog::instance().stargodDefinitions(),
      PetDetailCatalog::instance().astrolabeDefinitions(), PetDetailCatalog::instance().petDefinitions(),
      PetDetailCatalog::instance().sacredStarPlans(), PetDetailCatalog::instance().sacredStagePlans(), PetDetailCatalog::instance().badgeDefinitions()};
  const auto actualRows = RecommendationEngine::generatePrepared(QStringLiteral("actual-catalog"),
      overview(actualPets), actualPrepared, actualMetadata, &actualStats);
  ok &= require(actualStats.candidatePairsVisited == matchingPairs(actualPets, realGoods) &&
      actualStats.costExpressionsParsed == realGoods.size() &&
      actualStats.accountConditionsPrepared == realGoods.size() &&
      actualStats.petsDerived == actualPets.size() && actualStats.maximumLiveCandidates <= 2 &&
      actualStats.finalRowsMaterialized == actualRows.size(),
      "actual catalog / skin workload lost matches or reparsed per candidate");
  report(QStringLiteral("actual-catalog-synthetic-pets"), actualPets.size(), realGoods.size(),
         actualStats, actualElapsed.elapsed());

  // Check a small cross-product reference by selecting each real project
  // independently, then ranking its eligible result with the same frozen pet
  // and account facts. Only the candidate catalog changes in this oracle.
  const QList<PetAssetRecord> referencePets = actualPets.mid(0, 12);
  QList<ActionRecommendation> expected;
  for (const PetAssetRecord& pet : referencePets) {
    QList<ActionRecommendation> candidates;
    for (const ShopExchangeGood& good : realGoods) {
      const auto oneCatalog = CompiledShopCatalog::compile({good});
      const auto onePrepared = PreparedShopConditions::prepare(oneCatalog,
          actualPacket, actualResources, actualPrepared.context());
      auto rows = RecommendationEngine::generatePrepared(QStringLiteral("reference"),
          overview({pet}), onePrepared, actualMetadata);
      if (!rows.isEmpty()) candidates.append(rows.first());
    }
    if (!candidates.isEmpty()) {
      std::sort(candidates.begin(), candidates.end(), RecommendationEngine::less);
      candidates[0].alternateGoodCount = candidates.size() - 1;
      expected.append(candidates.first());
    }
  }
  std::sort(expected.begin(), expected.end(), RecommendationEngine::less);
  const auto indexed = RecommendationEngine::generatePrepared(QStringLiteral("reference"),
      overview(referencePets), actualPrepared, actualMetadata);
  ok &= require(identities(expected) == identities(indexed),
                "indexed actual catalog differs from a full independent-project enumeration");

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: pure shop snapshots, exact work counts, indexed H, skins, immutable inputs\n");
  return 0;
}
