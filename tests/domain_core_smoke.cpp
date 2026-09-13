#include "asset_derivation.h"
#include "asset_snapshot_comparator.h"
#include "checked_json_numbers.h"
#include "pet_identity.h"
#include "pet_move_policy.h"
#include "pet_power_calculator.h"
#include "recommendation_engine.h"
#include "shop_limit_facts.h"
#include "pet_analysis_facts.h"
#include "quota_test_support.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <atomic>
#include <cstdio>
#include <limits>

// Deliberately no QCoreApplication, qrc, repository, catalog holder, storage,
// worker or widget. CMake additionally forces every Domain object into this link.
namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
bool numeric() {
  qint64 result = 0;
  bool ok = check(DomainNumeric::checkedInteger(QStringLiteral("9223372036854775807"), &result) &&
      result == std::numeric_limits<qint64>::max(), "exact maximum integer string");
  ok &= check(DomainNumeric::checkedInteger(QStringLiteral("-9223372036854775808"), &result) &&
      result == std::numeric_limits<qint64>::min(), "exact minimum integer string");
  ok &= check(!DomainNumeric::checkedInteger(1.5, &result) &&
      !DomainNumeric::checkedInteger(9007199254740992.0, &result) &&
      !DomainNumeric::checkedInteger(true, &result) &&
      !DomainNumeric::checkedInteger(QStringLiteral(" 1"), &result), "numeric precision and lexical refusals");
  ok &= check(!DomainNumeric::checkedAdd(std::numeric_limits<qint64>::max(), 1, &result) &&
      !DomainNumeric::checkedMultiply(std::numeric_limits<qint64>::min(), -1, &result) &&
      DomainNumeric::checkedMultiply(-7, 9, &result) && result == -63, "checked arithmetic retains overflow semantics");
  return ok;
}
ShopExchangeGood good() {
  ShopExchangeGood value;
  value.shopId = 1; value.itemServerId = 2; value.limitKey = QStringLiteral("dl"); value.limitCount = 3;
  value.cost = QStringLiteral("4:100:60#4:100:60"); value.enhanceType = QStringLiteral("41");
  value.raceIds = {7001}; value.shelfDate = QDate(2020, 1, 1);
  return value;
}
QJsonObject packet() {
  return {{QStringLiteral("si1"), QJsonObject{{QStringLiteral("bi2"), QJsonObject{{QStringLiteral("dl"), 1}}}}}};
}
PetAssetRecord pet() {
  PetAssetRecord value;
  value.instanceId = 1001; value.raceId = 7001; value.name = QStringLiteral("fixture");
  value.detailAvailable = true; value.improvable = true; value.completionKnown = true;
  value.powerGapKnown = true; value.completionPercent = 80; value.currentPower = 9000; value.highestPower = 10000;
  value.soulMissing = true; value.gapKeys = {QStringLiteral("bsv")};
  value.pet = {{QStringLiteral("id"), QStringLiteral("1001")}, {QStringLiteral("r"), 7001},
      {QStringLiteral("badge"),QStringLiteral("101:1")},
      {QStringLiteral("czdlv"), QJsonObject{{QStringLiteral("bsv"), 10}}},
      {QStringLiteral("mzdlv"), QJsonObject{{QStringLiteral("bsv"), 100}}}};
  return value;
}
bool pipeline() {
  ShopExchangeGood item = good();
  bool ok = check(item.isOnlineOn(QDate(2026, 9, 12)) && !item.isOnlineOn(QDate(2019, 1, 1)) &&
      item.itemKey() == QStringLiteral("bi2"), "catalog date is supplied explicitly");
  auto equivalent = item; equivalent.raceIds = {7002, 7001, 7001};
  auto reordered = equivalent; reordered.raceIds = {7001, 7002};
  ok &= check(equivalent.stableKey() == reordered.stableKey(), "stable identity canonicalizes duplicate/order of races");
  AlgorithmPipelineStats stats;
  const auto compiled = CompiledShopCatalog::compile({item}, {}, &stats);
  item.cost = QStringLiteral("4:100:1");
  ok &= check(compiled.goods().size() == 1 && compiled.goods().first().requirements.size() == 1 &&
      compiled.goods().first().requirements.first().required == 120, "duplicate costs aggregate and captured catalog is immutable");
  ok &= check(stats.goodsCompiled == 1 && stats.costExpressionsParsed == 1 && stats.costFragmentsParsed == 2,
      "compilation counters remain one per declared stage");
  ok &= check(shopRemainingCount(packet(), item) == 2 && shopRemainingCount({}, item) == -1,
      "quota absence is unknown");
  auto conflict = packet().value(QStringLiteral("si1")).toObject();
  conflict.insert(QStringLiteral("b2"), QJsonObject{{QStringLiteral("dl"), 2}});
  ok &= check(shopRemainingCount({{QStringLiteral("si1"), conflict}}, item) == -1,
      "conflicting modern and legacy quota objects remain unknown");
  const auto prepared = prepareWithSyntheticPeriod(compiled, packet(),
      AccountResourceView({{QStringLiteral("4:100"), 100}}, true), {}, &stats);
  ok &= check(!prepared.goods().first().account.resourcesEnough &&
      prepared.goods().first().account.resourceCondition.state == ShopConditionState::Blocked,
      "aggregated cost cannot appear affordable");
  AccountAssetOverview overview; overview.account = QStringLiteral("domain-fixture"); overview.pets = {pet()};
  ShopPetMetadataSnapshot metadata;
  metadata.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}}};
  const auto result = PreparedRecommendationEngine::generatePrepared(overview.account, overview, prepared, metadata, &stats);
  ok &= check(result.size() == 1 && result.first().type == RecommendationType::ResourceMissing,
      "pure recommendation retains resource-missing classification");
  item = good(); item.cost = QStringLiteral("4:100:20#not-a-cost");
  const auto malformed = CompiledShopCatalog::compile({item});
  ok &= check(malformed.goods().first().costCondition.state == ShopConditionState::Unknown,
      "malformed cost fragment does not create a partial affordable cost");
  std::atomic_bool cancelled{true};
  RecommendationSession session(overview.account, overview, prepared, {});
  ok &= check(session.step(&cancelled) == RecommendationSession::Status::Cancelled,
      "value-only session observes cancellation before doing work");
  const auto stale = PreparedRecommendationEngine::applyFreshness(result, false, true);
  ok &= check(stale.size() == result.size() && stale.first().type != RecommendationType::ReadyNow,
      "freshness invalidation never manufactures ReadyNow");
  return ok;
}
bool derivationAndSnapshots() {
  PetAssetRecord seed; seed.instanceId = 1;
  const auto derived = AssetDerivation::derivePet(seed, {});
  bool ok = check(derived.instanceId == 1 && !derived.currentPowerKnown &&
      AssetDerivation::matchesFilter(derived, PetAssetFilter::All), "missing detail remains unknown in pure derivation");
  const auto identity = AssetDerivation::identityFields({{QStringLiteral("id"), QStringLiteral("1")},
      {QStringLiteral("rawCultivation"), QJsonArray{1, 2, 3}}});
  ok &= check(identity.contains(QStringLiteral("id")) && !identity.contains(QStringLiteral("rawCultivation")),
      "identity projection excludes raw cultivation");
  AccountAssetSnapshot before, after;
  before.account = after.account = QStringLiteral("A");
  before.analysisVersion = after.analysisVersion = AssetAnalysisVersion::kCurrentAnalysis;
  AssetSnapshotPet old; old.instanceId = 1; old.currentPower = 100; old.currentPowerKnown = true; old.cultivationKnown = true;
  auto current = old; current.currentPower = 120; current.fullyCultivated = true;
  before.pets = {old}; after.pets = {current};
  auto delta = AssetSnapshotComparator::compare(after, before);
  ok &= check(delta.accountComparable && delta.cultivationComparable && delta.newlyFullyCultivated == 1 &&
      delta.powerChangeKnown && delta.totalPowerChange == 20, "known comparable snapshot change");
  before.pets[0].cultivationKnown = false;
  delta = AssetSnapshotComparator::compare(after, before);
  ok &= check(delta.newlyFullyCultivated == 0, "unknown old cultivation is not an achievement");
  after.account = QStringLiteral("B");
  ok &= check(!AssetSnapshotComparator::compare(after, before).accountComparable,
      "cross-account snapshots cannot be compared");
  ok &= check(PetMovePolicy::replace({1, 2, 3}, 2, 8) == QList<qint64>({1, 8, 3}) &&
      !PetMovePolicy::restriction({{QStringLiteral("isRentPet"), true}}).isEmpty(), "move sequence and restriction remain pure rules");
  return ok;
}

bool compactFacts() {
  bool ok = true;
  auto seed = pet();
  seed.observationVerified = true;
  seed.pet.insert(QStringLiteral("sppl"),QJsonObject{{QStringLiteral("id"),9000}});
  QJsonArray inventory;
  for (int index = 0; index < 4096; ++index) inventory.append(66);
  seed.pet.insert(QStringLiteral("sgsp"),inventory);
  AlgorithmPipelineStats preparation;
  auto facts = derivePetAnalysisFacts(seed,{},&preparation);
  ok &= check(facts && preparation.rawPowerCalculations == 1 && preparation.petsDerived == 1,
              "fact preparation runs shared raw rules exactly once");
  if (!facts) return false;
  ok &= check(!facts->asset.pet.contains(QStringLiteral("sgsp")) && !facts->asset.pet.contains(QStringLiteral("sppl")) &&
      facts->battlePower.backpackStars == 4096 && petAnalysisFactsRetainedBytes(*facts) < 16384,
      "compact facts must retain derived count without raw inventory/relations");
  facts->asset.memoryRetention = std::make_shared<int>(1);
  ok &= check(validatePetAnalysisFacts(*facts),"Application lease is not part of semantic fact validation");
  const auto object = petAnalysisFactsToJson(*facts);
  const auto restored = petAnalysisFactsFromJson(QJsonDocument::fromJson(QJsonDocument(object).toJson()).object());
  ok &= check(restored && !restored->asset.memoryRetention && restored->asset.instanceId == seed.instanceId &&
      restored->battlePower.backpackStars == 4096 && restored->battlePower.componentGaps.size() == facts->battlePower.componentGaps.size() &&
      petAnalysisFactsToJson(*restored) == object,"strict compact index roundtrip changed derived facts or retained a lease");
  auto wrong = seed; wrong.instanceId += 1;
  ok &= check(!derivePetAnalysisFacts(wrong,{}),"fact factory accepted a raw/summary ID mismatch");
  auto maximumId = seed;
  maximumId.instanceId = std::numeric_limits<qint64>::max();
  maximumId.pet.insert(QStringLiteral("id"),QString::number(maximumId.instanceId));
  const auto exact = derivePetAnalysisFacts(maximumId,{});
  const auto exactRestored = exact ? petAnalysisFactsFromJson(petAnalysisFactsToJson(*exact)) : std::nullopt;
  ok &= check(exactRestored && exactRestored->asset.instanceId == std::numeric_limits<qint64>::max(),
              "compact facts lost exact maximum instance-ID precision");
  auto bad = object;
  auto asset = bad.value(QStringLiteral("asset")).toObject();
  asset.insert(QStringLiteral("currentPower"),true); bad.insert(QStringLiteral("asset"),asset);
  ok &= check(!petAnalysisFactsFromJson(bad),"compact index accepted the wrong field type");
  bad = object; asset = bad.value(QStringLiteral("asset")).toObject();
  auto identity = asset.value(QStringLiteral("identity")).toObject();
  identity.insert(QStringLiteral("sgsp"),QJsonArray{}); asset.insert(QStringLiteral("identity"),identity);
  bad.insert(QStringLiteral("asset"),asset);
  ok &= check(!petAnalysisFactsFromJson(bad),"compact index accepted raw cultivation JSON");
  PetAssetRecord missing; missing.instanceId = 2; missing.name = QStringLiteral("unknown instance");
  auto unknown = derivePetAnalysisFacts(missing,{});
  ok &= check(unknown && !unknown->asset.detailAvailable && !unknown->asset.currentPowerKnown &&
      unknown->asset.instanceId == 2,"missing detail was dropped or turned into known zero");
  if (!unknown) return false;
  const auto prepared = prepareWithSyntheticPeriod(CompiledShopCatalog::compile({good()}),packet(),
      AccountResourceView({{QStringLiteral("4:100"),1000}},true));
  AccountAssetOverview header; header.account = QStringLiteral("facts-domain"); header.totalPets = 2;
  QList<PetAnalysisFacts> values{*facts,*unknown};
  AlgorithmPipelineStats reused;
  RecommendationSession session(header.account,header,prepared,{},&reused,false,&values);
  while (session.step() == RecommendationSession::Status::Running) {}
  ok &= check(session.status() == RecommendationSession::Status::Complete && session.takeOverview().pets.size() == 2 &&
      reused.preparedFactsReused == 2 && reused.petsDerived == 0 && reused.rawPowerCalculations == 0,
      "prepared path dropped unknown P or reported reused facts as parsed");
  AccountAssetOverview partialPower;
  PetAssetRecord observedZero; observedZero.currentPowerKnown = true; observedZero.currentPower = 0;
  AssetDerivation::accumulate(&partialPower, observedZero);
  ok &= check(partialPower.totalCurrentPowerKnown && partialPower.totalCurrentPower == 0,
      "an explicitly observed zero must remain a known value");
  AssetDerivation::accumulate(&partialPower, unknown->asset);
  ok &= check(!partialPower.totalCurrentPowerKnown && partialPower.totalCurrentPower == 0,
      "unknown individual power must not manufacture a known account total of zero");
  values[1] = values[0];
  AlgorithmPipelineStats rejected;
  RecommendationSession duplicate(header.account,header,prepared,{},&rejected,false,&values);
  while (duplicate.step() == RecommendationSession::Status::Running) {}
  ok &= check(duplicate.status() == RecommendationSession::Status::InvalidInput && duplicate.takeOverview().pets.isEmpty() &&
      rejected.candidatePairsVisited == 0,"duplicate fact IDs reached candidate evaluation or publication");
  return ok;
}
}
int main() {
  const bool ok = numeric() && pipeline() && derivationAndSnapshots() && compactFacts();
  if (ok) std::puts("PASS: QtCore-only Domain numbers, frozen catalog, quota, recommendation, cultivation, snapshots and move rules");
  return ok ? 0 : 1;
}
