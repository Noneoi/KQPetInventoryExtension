#include "support/protocol_test_support.h"
#include "application/analysis/asset_analysis_controller.h"
#include "domain/asset_analysis_version.h"
#include "application/analysis/asset_snapshot_store.h"
#include "application/pet/pet_repository.h"
#include "domain/pet_power_calculator.h"
#include "application/catalog/pet_detail_catalog.h"
#include <QElapsedTimer>
#include <QThread>
#include <functional>
#include "application/routine/routine_overview_controller.h"
#include "application/shop/shop_exchange_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>


namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
  if (!waitForRepositoryIdle(repository))
    std::fprintf(stderr, "FAIL: asset fixture did not finish its account/cache I/O\n");
}

bool waitUntil(const std::function<bool()>& predicate, int timeout = 10000) {
  QElapsedTimer timer; timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (predicate()) return true;
    QThread::msleep(1);
  } while (timer.elapsed() < timeout);
  return false;
}
AccountAssetOverview analyze(AssetAnalysisController& controller) {
  controller.requestAnalysis();
  if (!waitUntil([&] { return !controller.analysisRunning(); }))
    std::fprintf(stderr, "FAIL: asynchronous full analysis timed out\n");
  return controller.overview();
}
bool storageIdle(AssetAnalysisController& controller) {
  return waitUntil([&] { return controller.persistencePendingTaskCount() == 0 && !controller.snapshotHistoryLoading(); });
}
QList<AccountAssetSnapshot> history(AssetAnalysisController& controller, bool reload = false) {
  if (reload) controller.requestSnapshotHistory();
  storageIdle(controller);
  auto summaries = controller.snapshots();
  for (const auto& summary : summaries) if (!summary.petsComplete) {
    controller.requestSnapshotDetails(summary.storageKey);
    storageIdle(controller);
  }
  return controller.snapshots();
}

QJsonObject cultivatedPet(qint64 id, int race, const QString& name,
                          int current, bool full) {
  QJsonObject currentParts, maximumParts;
  for (const auto& key : {QStringLiteral("lv"), QStringLiteral("iv"), QStringLiteral("pl"),
       QStringLiteral("sgv"), QStringLiteral("ep"), QStringLiteral("gsv"), QStringLiteral("lav"),
       QStringLiteral("lsv"), QStringLiteral("bsv"), QStringLiteral("asv"), QStringLiteral("sjv")}) {
    currentParts.insert(key, 0); maximumParts.insert(key, 0);
  }
  currentParts.insert(QStringLiteral("lv"), full ? current - 5500 : current - 2615);
  currentParts.insert(QStringLiteral("sgv"), full ? 5200 : 2600);
  currentParts.insert(QStringLiteral("bsv"), full ? 100 : 10);
  currentParts.insert(QStringLiteral("sjv"), full ? 50 : 5);
  maximumParts.insert(QStringLiteral("lv"), 5850);
  maximumParts.insert(QStringLiteral("sgv"), 4000);
  maximumParts.insert(QStringLiteral("bsv"), 100);
  maximumParts.insert(QStringLiteral("sjv"), 50);
  maximumParts.insert(QStringLiteral("asv"), full ? 0 : 570);
  return {{QStringLiteral("id"), id},
          {QStringLiteral("r"), race},
          {QStringLiteral("fr"), race},
          {QStringLiteral("n"), name},
          {QStringLiteral("lv"), 120},
          {QStringLiteral("zdl"), current},
          {QStringLiteral("xzdl"), full ? 10000 : 10570},
          {QStringLiteral("astrolabebr"), full},
          {QStringLiteral("badge"), full ? QStringLiteral("101:5#201:1") : QStringLiteral("101:1#201:0")},
          {QStringLiteral("astrolabe"), full ? QString{} : QStringLiteral("350:0:0#351:0:0#352:0:0")},
          {QStringLiteral("sgs"),
           full ? QJsonValue(QStringLiteral(
                      "66:8#67:8#70:8#68:8#72:8#73:8#74:8#80:8:80"))
                : QJsonValue(QStringLiteral(
                      "66:8#67:8#70:8#0:8#0:8#0:8#0:8#80:8:80"))},
          {QStringLiteral("sgsp"),
           full ? QJsonValue(QJsonArray{68, 72, 73, 74})
                : QJsonValue(QJsonArray{})},
          {QStringLiteral("czdlv"), currentParts},
          {QStringLiteral("mzdlv"), maximumParts}};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  const QJsonObject frozenStargods = PetDetailCatalog::instance().stargodDefinitions();
  const QJsonObject frozenAstrolabe = PetDetailCatalog::instance().astrolabeDefinitions();
  const QJsonObject frozenPets = PetDetailCatalog::instance().petDefinitions();
  const ShopPetMetadataSnapshot frozenMetadata{frozenStargods, frozenAstrolabe, frozenPets,
      PetDetailCatalog::instance().sacredStarPlans(), PetDetailCatalog::instance().sacredStagePlans(), PetDetailCatalog::instance().badgeDefinitions()};
  const auto completePet = cultivatedPet(70001, 7001, QStringLiteral("纯快照"), 11350, true);
  const auto powerMetadata = petPowerMetadataFromCatalog(completePet, 8, frozenStargods, frozenAstrolabe, frozenPets);
  const auto purePower = calculatePetBattlePower(completePet, powerMetadata);
  ok &= require(purePower.hasCurrent && purePower.current == 11200 && purePower.hasHighest &&
                    purePower.highest == 11200 && purePower.isHighest && purePower.serverCurrent == 11350 &&
                    purePower.extreme == 10000 && !purePower.astrolabeApplicable && purePower.astrolabeTargetBonus == 0,
                "local supreme did not distinguish server observation, official extreme, and non-applicable astrolabe");
  auto malformedPower = completePet;
  malformedPower.insert(QStringLiteral("zdl"), 11350.5);
  const auto fractionalServer = calculatePetBattlePower(malformedPower, powerMetadata);
  ok &= require(!fractionalServer.hasServerCurrent && fractionalServer.currentLocallyCalculated && fractionalServer.current == 11200,
                "fractional server power was truncated or invalidated independent valid local components");
  auto invalidComponents = completePet.value(QStringLiteral("czdlv")).toObject();
  invalidComponents.insert(QStringLiteral("lv"), 0.5);
  malformedPower = completePet; malformedPower.insert(QStringLiteral("czdlv"), invalidComponents);
  ok &= require(!calculatePetBattlePower(malformedPower, powerMetadata).currentLocallyCalculated &&
                    !calculatePetBattlePower(malformedPower, powerMetadata).isHighest,
                "invalid component acquired a full cultivation result");
  invalidComponents = completePet.value(QStringLiteral("mzdlv")).toObject();
  invalidComponents.insert(QStringLiteral("lv"), INT_MAX);
  malformedPower = completePet; malformedPower.insert(QStringLiteral("mzdlv"), invalidComponents);
  ok &= require(!calculatePetBattlePower(malformedPower, powerMetadata).hasHighest,
                "overflowed highest power was accepted");
  malformedPower = completePet;
  auto missingComponent = completePet.value(QStringLiteral("mzdlv")).toObject();
  missingComponent.remove(QStringLiteral("bsv"));
  malformedPower.insert(QStringLiteral("mzdlv"), missingComponent);
  ok &= require(!calculatePetBattlePower(malformedPower, powerMetadata).hasHighest &&
                    !calculatePetBattlePower(malformedPower, powerMetadata).currentLocallyCalculated,
                "one-sided missing component was promoted to an observed zero");
  auto unknownMaximum = powerMetadata; unknownMaximum.slotMaxLevel = 0;
  ok &= require(!calculatePetBattlePower(completePet, unknownMaximum).hasHighest,
                "observed level was promoted to an unknown maximum");
  auto zeroDefinitions = frozenStargods;
  auto zeroStar = zeroDefinitions.value(QStringLiteral("80")).toObject();
  zeroStar.insert(QStringLiteral("battlePower"), QJsonObject{{QStringLiteral("8"), 0}});
  zeroDefinitions.insert(QStringLiteral("80"), zeroStar);
  auto invalidStarMetadata = powerMetadata; invalidStarMetadata.stargods = zeroDefinitions;
  ok &= require(!calculatePetBattlePower(completePet, invalidStarMetadata).hasHighest,
                "zero star metadata manufactured a fulfilled highest target");
  malformedPower = completePet;
  malformedPower.insert(QStringLiteral("sgs"), QStringLiteral("0:8:9#0:8#0:8"));
  malformedPower.insert(QStringLiteral("sgsp"), QJsonArray{});
  ok &= require(calculatePetBattlePower(malformedPower, powerMetadata).equippedStargodPower == 0,
                "empty ordinary slot borrowed the source star as equipped");
  PetAssetRecord seed;
  seed.instanceId = 70002; seed.raceId = 7001; seed.name = QStringLiteral("纯输入原名");
  seed.location = QStringLiteral("背包"); seed.detailAvailable = true; seed.metadataSlotMaxLevel = 8;
  seed.observationVerified = true;
  seed.pet = cultivatedPet(70002, 7001, seed.name, 5000, false);
  seed.pet.insert(QStringLiteral("_metaOriginalName"), QStringLiteral("检索原名"));
  seed.gaps = {QStringLiteral("obsolete")}; seed.shopImprovable = true;
  auto derivedOnce = AssetDerivation::derivePet(seed, frozenStargods, frozenAstrolabe, frozenPets);
  auto derivedTwice = AssetDerivation::derivePet(derivedOnce, frozenStargods, frozenAstrolabe, frozenPets);
  ok &= require(derivedOnce.gaps == derivedTwice.gaps && !derivedOnce.gaps.contains(QStringLiteral("obsolete")) &&
                    !derivedOnce.shopImprovable, "reused pure seed retained previous cultivation state");
  ShopExchangeGood exhaustedGood;
  exhaustedGood.shopId = 1; exhaustedGood.itemServerId = 1; exhaustedGood.raceIds = {7001};
  exhaustedGood.enhanceType = QStringLiteral("41"); exhaustedGood.provenFree = true;
  exhaustedGood.limitCount = 1; exhaustedGood.limitKey = QStringLiteral("dl");
  ShopConditionContext pureContext;
  pureContext.petFreshness = ShopConditionFreshness::Current; pureContext.shopFreshness = ShopConditionFreshness::Current;
  pureContext.quotaValidity.insert(QStringLiteral("si1:dl"),
      {ShopConditionState::Satisfied, QStringLiteral("explicit synthetic period"), QStringLiteral("isolated asset fixture"),
       QDateTime(QDate(2026, 9, 9), QTime(12, 0), Qt::UTC), ShopConditionFreshness::Current});
  const auto pureConditions = PreparedShopConditions::prepare(CompiledShopCatalog::compile({exhaustedGood}),
      {{QStringLiteral("si1"), QJsonObject{{QStringLiteral("b1"), QJsonObject{{QStringLiteral("dl"), 1}}}}}},
      AccountResourceView({}, true), pureContext);
  AccountAssetOverview pureSeed; pureSeed.account = QStringLiteral("pure"); pureSeed.pets = {seed};
  AlgorithmPipelineStats pureStats;
  RecommendationSession pureSession(pureSeed.account, pureSeed, pureConditions, frozenMetadata, &pureStats, true);
  while (pureSession.step() == RecommendationSession::Status::Running) {}
  const auto pureOverview = pureSession.takeOverview();
  ok &= require(pureStats.petsDerived == 1 && pureStats.candidatePairsVisited == 1 &&
                    pureOverview.pets.size() == 1 && pureOverview.pets.first().shopImprovable &&
                    pureSession.takeResults().isEmpty(), "full overview and recommendations did not share one H traversal");
  ok &= require(!pureOverview.pets.first().pet.contains(QStringLiteral("czdlv")) &&
                    pureOverview.pets.first().pet.value(QStringLiteral("_metaOriginalName")).toString() == QStringLiteral("检索原名"),
                "result retained raw cultivation payload or lost searchable identity");
  std::atomic_bool cancelled{true};
  RecommendationSession cancelledSession(pureSeed.account, pureSeed, pureConditions, frozenMetadata, nullptr, true);
  ok &= require(cancelledSession.step(&cancelled) == RecommendationSession::Status::Cancelled &&
                    cancelledSession.takeOverview().pets.isEmpty(), "cancelled full analysis published a partial overview");

  PetRepository repository;
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("asset-account")}}}});
  const QString account = repository.accountKey();
  const quint64 session = repository.sessionGeneration();
  repository.beginListRefresh(1, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{cultivatedPet(1001, 7001, QStringLiteral("满培养"), 11350, true),
                       cultivatedPet(1002, 7002, QStringLiteral("待培养"), 5000, false)}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("1001#1002")}},
           {QStringLiteral("ppc"), 12}});
  repository.expectListPart(QStringLiteral("2_1_S"), 1, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
           {QStringLiteral("ns"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 2001},
                                   {QStringLiteral("ri"), 7101},
                                   {QStringLiteral("n"), QStringLiteral("缺详情")},
                                   {QStringLiteral("lv"), 120}}}},
           {QStringLiteral("es"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 2002},
                                   {QStringLiteral("ri"), 7102},
                                   {QStringLiteral("n"), QStringLiteral("精英待培养")},
                                   {QStringLiteral("lv"), 120}}}},
           {QStringLiteral("rb"), QJsonArray{}}});
  repository.expectDetail(2002, 2, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
           {QStringLiteral("p"),
            cultivatedPet(2002, 7102, QStringLiteral("精英待培养"), 7000, false)}});

  ShopExchangeController shop(&repository);
  RoutineOverviewController routine(&repository);
  AssetAnalysisController controller(&repository, &shop, &routine);
  controller.setCompatibilityIdentity(QStringLiteral("fixture-build"), QStringLiteral("fixture-profile"), true);
  storageIdle(controller);
  const AccountInventorySummary inventory = controller.inventorySummary();
  ok &= require(inventory.totalPets == 4 && inventory.backpackPets == 2 &&
                    inventory.normalWarehousePets == 1 &&
                    inventory.eliteWarehousePets == 1,
                "lightweight inventory summary counts are incorrect");
  const AccountAssetOverview overview = analyze(controller);
  ok &= require(overview.analysisVersion == AssetAnalysisVersion::kCurrentAnalysis,
                "analysis result version is missing");
  ok &= require(overview.totalPets == 4 && overview.backpackPets == 2 &&
                    overview.normalWarehousePets == 1 &&
                    overview.eliteWarehousePets == 1,
                "asset location counts are incorrect");
  ok &= require(overview.fullyCultivatedPets == 1 &&
                    overview.improvablePets == 2 &&
                    overview.missingDetailPets == 1,
                "cultivation summary counts are incorrect");
  ok &= require(overview.redStarMissingPets == 2 &&
                    overview.astrolabeMissingPets == 2 &&
                    overview.sacredMissingPets == 2 &&
                    overview.soulMissingPets == 2,
                "component gap counts are incorrect");
  bool foundImprovable = false;
  for (const PetAssetRecord& pet : overview.pets) {
    if (pet.instanceId == 1002) {
      foundImprovable = AssetAnalysisController::matchesFilter(
          pet, PetAssetFilter::Improvable);
      ok &= require(pet.currentPower == 5000 && pet.extremePower == 10570 &&
                        pet.highestPower == 11920,
                    "diagnostic power columns are incorrect");
    }
  }
  ok &= require(foundImprovable,
                "diagnostic filtering did not expose improvable pets");

  ok &= require(controller.hasAnalysis() && controller.dirtyPetIds().isEmpty() &&
                    !controller.inventoryAnalysisStale() &&
                    !controller.shopAnalysisStale(),
                "fresh manual analysis did not initialize clean state");
  ok &= require(controller.recordSnapshot(), "snapshot could not be recorded");
  const QList<AccountAssetSnapshot> snapshots = history(controller);
  ok &= require(snapshots.size() == 1 &&
                    snapshots.constFirst().schemaVersion ==
                        AssetAnalysisVersion::kCurrentSnapshotSchema &&
                    snapshots.constFirst().analysisVersion ==
                        AssetAnalysisVersion::kCurrentAnalysis &&
                    snapshots.constFirst().totalPets == 4 &&
                    snapshots.constFirst().pets.size() == 4,
                "versioned snapshot did not preserve lightweight account assets");
  const QDir snapshotDirectory(
      QDir(QFileInfo(repository.cachePath()).absolutePath())
          .filePath(QStringLiteral("snapshots")));
  const QStringList currentSnapshotFiles = snapshotDirectory.entryList(
      {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  QFile currentSnapshotFile(snapshotDirectory.filePath(currentSnapshotFiles.value(0)));
  ok &= require(currentSnapshotFile.open(QIODevice::ReadOnly),
                "current snapshot file could not be inspected");
  const QJsonObject currentSnapshotObject =
      QJsonDocument::fromJson(currentSnapshotFile.readAll()).object();
  ok &= require(currentSnapshotObject.value(QStringLiteral("schema")).toInt() ==
                        AssetAnalysisVersion::kCurrentSnapshotSchema &&
                    currentSnapshotObject
                            .value(QStringLiteral("analysisVersion"))
                            .toInt() == AssetAnalysisVersion::kCurrentAnalysis &&
                    currentSnapshotObject.value(QStringLiteral("summary")).isObject() &&
                    currentSnapshotObject.value(QStringLiteral("pets")).isArray(),
                "snapshot JSON does not use the versioned summary schema");
  controller.setAutoSnapshotEnabled(true);
  ok &= require(storageIdle(controller) && controller.autoSnapshotEnabled(), "auto-snapshot setting was not Saved");
  int membershipChanges = 0;
  int countChanges = 0;
  int detailChanges = 0;
  int shopInvalidations = 0;
  int routineChanges = 0;
  QObject::connect(&controller, &AssetAnalysisController::inventoryMembershipChanged,
                   &application, [&membershipChanges]() { ++membershipChanges; });
  QObject::connect(&controller, &AssetAnalysisController::inventoryCountsChanged,
                   &application, [&countChanges]() { ++countChanges; });
  QObject::connect(&controller, &AssetAnalysisController::petDetailChanged,
                   &application, [&detailChanges](qint64) { ++detailChanges; });
  QObject::connect(&controller, &AssetAnalysisController::shopAnalysisInvalidated,
                   &application, [&shopInvalidations]() { ++shopInvalidations; });
  QObject::connect(&controller, &AssetAnalysisController::routineSummaryChanged,
                   &application, [&routineChanges]() { ++routineChanges; });

  const QDateTime previousInventoryTime = controller.inventorySummary().inventoryUpdatedAt;
  repository.beginListRefresh(3, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{cultivatedPet(1001, 7001, QStringLiteral("满培养"), 11350, true),
                       cultivatedPet(1002, 7002, QStringLiteral("待培养"), 5000, false)}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("1001#1002")}},
           {QStringLiteral("ppc"), 12}});
  repository.expectListPart(QStringLiteral("2_1_S"), 3, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
           {QStringLiteral("ns"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 2001},
                                   {QStringLiteral("ri"), 7101},
                                   {QStringLiteral("n"), QStringLiteral("缺详情")},
                                   {QStringLiteral("lv"), 120}}}},
           {QStringLiteral("es"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 2002},
                                   {QStringLiteral("ri"), 7102},
                                   {QStringLiteral("n"), QStringLiteral("精英待培养")},
                                   {QStringLiteral("lv"), 120}}}},
           {QStringLiteral("rb"), QJsonArray{}}});
  ok &= require(membershipChanges == 0 && !controller.inventoryAnalysisStale(),
                "unchanged list refresh incorrectly expired analysis");
  ok &= require(countChanges == 2 &&
                    controller.inventorySummary().inventoryUpdatedAt >= previousInventoryTime &&
                    controller.inventorySummary().missingDetailPets == 1,
                "list refresh did not update lightweight counts and cache time");

  repository.expectDetail(2002, 3, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
           {QStringLiteral("p"),
            cultivatedPet(2002, 7102, QStringLiteral("精英待培养"), 7100, false)}});
  ok &= require(controller.dirtyPetIds().size() == 1 &&
                    controller.dirtyPetIds().contains(2002) && detailChanges == 1,
                "detail update did not add exactly one dirty instance");
  ok &= require(controller.overview().totalCurrentPower == overview.totalCurrentPower &&
                    controller.overview().pets.size() == overview.pets.size(),
                "detail update unexpectedly rebuilt or cleared the last overview");
  repository.expectDetail(2002, 4, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
           {QStringLiteral("p"),
            cultivatedPet(2002, 7102, QStringLiteral("精英待培养"), 7200, false)}});
  ok &= require(controller.dirtyPetIds().size() == 1 && detailChanges == 1,
                "repeated detail update was not de-duplicated");
  repository.expectDetail(1002, 5, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
           {QStringLiteral("p"),
            cultivatedPet(1002, 7002, QStringLiteral("待培养"), 5100, false)}});
  ok &= require(controller.dirtyPetIds().size() == 2 && detailChanges == 2,
                "multiple detail updates did not accumulate dirty IDs");
  ok &= require(controller.snapshots().size() == 1,
                "background detail update unexpectedly wrote a snapshot");

  int completedJobs = 0;
  const auto completedConnection = QObject::connect(&controller, &AssetAnalysisController::analysisCompleted,
      &application, [&] { ++completedJobs; });
  const auto staleCapture = QObject::connect(&controller, &AssetAnalysisController::analysisInputCaptured,
      &application, [&](quint64) { repository.detailChanged(2002); QMetaObject::invokeMethod(&shop, "infoUpdated", Qt::DirectConnection); });
  analyze(controller);
  QObject::disconnect(staleCapture);
  ok &= require(completedJobs == 1 && controller.inventoryAnalysisStale() && controller.shopAnalysisStale() &&
                    controller.dirtyPetIds().contains(2002), "same-account changed input was published falsely fresh");
  const auto lastPublished = controller.lastAnalyzedAt();
  const auto cancelCapture = QObject::connect(&controller, &AssetAnalysisController::analysisInputCaptured,
      &application, [&](quint64) { controller.cancelAnalysis(); });
  analyze(controller);
  QObject::disconnect(cancelCapture);
  ok &= require(completedJobs == 1 && controller.lastAnalyzedAt() == lastPublished &&
                    controller.dirtyPetIds().contains(2002), "cancelled frozen job replaced analysis or cleared dirty facts");
  QObject::disconnect(completedConnection);
  shopInvalidations = 0;

  const AccountAssetOverview refreshed = analyze(controller);
  ok &= require(controller.dirtyPetIds().isEmpty() &&
                    !controller.inventoryAnalysisStale() &&
                    refreshed.totalCurrentPower != overview.totalCurrentPower,
                "manual refresh did not clear dirty state and recompute all pets");

  QMetaObject::invokeMethod(&shop, "infoUpdated", Qt::DirectConnection);
  ok &= require(shopInvalidations == 1 && controller.shopAnalysisStale() &&
                    !controller.inventoryAnalysisStale() &&
                    controller.dirtyPetIds().isEmpty(),
                "shop update affected more than shop analysis freshness");
  analyze(controller);
  QMetaObject::invokeMethod(&routine, "dataUpdated", Qt::DirectConnection);
  ok &= require(routineChanges == 1 && !controller.inventoryAnalysisStale() &&
                    !controller.shopAnalysisStale() && controller.dirtyPetIds().isEmpty(),
                "routine update incorrectly expired cultivation analysis");

  repository.beginListRefresh(6, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{cultivatedPet(1001, 7001, QStringLiteral("满培养"), 11350, true),
                       cultivatedPet(1002, 7002, QStringLiteral("待培养"), 5100, false),
                       cultivatedPet(3001, 7003, QStringLiteral("新增精灵"), 4000, false)}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("1001#1002#3001")}},
           {QStringLiteral("ppc"), 12}});
  ok &= require(controller.inventoryAnalysisStale() && controller.hasAnalysis() &&
                    controller.overview().pets.size() == 4,
                "new pet did not stale membership while preserving old analysis");
  analyze(controller);

  repository.beginListRefresh(7, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{cultivatedPet(1001, 7001, QStringLiteral("满培养"), 11350, true),
                       cultivatedPet(1002, 7002, QStringLiteral("待培养"), 5100, false)}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("1001#1002")}},
           {QStringLiteral("ppc"), 12}});
  ok &= require(controller.inventoryAnalysisStale(),
                "deleted pet did not stale membership analysis");
  analyze(controller);

  repository.beginListRefresh(8, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{cultivatedPet(1001, 7001, QStringLiteral("满培养"), 11350, true)}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("1001")}},
           {QStringLiteral("ppc"), 12}});
  repository.expectListPart(QStringLiteral("2_1_S"), 8, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
           {QStringLiteral("ns"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 2001},
                                   {QStringLiteral("ri"), 7101},
                                   {QStringLiteral("n"), QStringLiteral("缺详情")},
                                   {QStringLiteral("lv"), 120}},
                       QJsonObject{{QStringLiteral("id"), 1002},
                                   {QStringLiteral("ri"), 7002},
                                   {QStringLiteral("n"), QStringLiteral("待培养")},
                                   {QStringLiteral("lv"), 120}}}},
           {QStringLiteral("es"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 2002},
                                   {QStringLiteral("ri"), 7102},
                                   {QStringLiteral("n"), QStringLiteral("精英待培养")},
                                   {QStringLiteral("lv"), 120}}}},
           {QStringLiteral("rb"), QJsonArray{}}});
  ok &= require(controller.inventoryAnalysisStale(),
                "pet location move did not stale membership analysis");
  ok &= require(controller.snapshots().size() == 1,
                "background membership changes unexpectedly wrote a snapshot");
  analyze(controller);

  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("other-account")}}}});
  ok &= require(!controller.autoSnapshotEnabled() && controller.snapshots().isEmpty() &&
                    !controller.hasAnalysis() && controller.dirtyPetIds().isEmpty() &&
                    controller.overview().account.isEmpty(),
                "analysis, dirty IDs, snapshot history, or settings leaked accounts");
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("asset-account")}}}});
  storageIdle(controller);
  ok &= require(controller.autoSnapshotEnabled() && controller.snapshots().size() == 1 &&
                    controller.hasAnalysis() &&
                    controller.overview().account == QStringLiteral("asset-account") &&
                    controller.shopAnalysisStale() && controller.inventoryAnalysisStale(),
                "account analysis/settings were not restored or old-session analysis was falsely current");

  QFile legacySnapshot(snapshotDirectory.filePath(QStringLiteral("2000-01-01.json")));
  ok &= require(legacySnapshot.open(QIODevice::WriteOnly),
                "legacy snapshot fixture could not be created");
  legacySnapshot.write(QJsonDocument(
                           QJsonObject{{QStringLiteral("schema"), 1},
                                       {QStringLiteral("account"), account},
                                       {QStringLiteral("createdAt"),
                                        QStringLiteral("2000-01-01T12:00:00")},
                                       {QStringLiteral("totalPets"), 3},
                                       {QStringLiteral("fullyCultivatedPets"), 1},
                                       {QStringLiteral("totalCurrentPower"),
                                        QStringLiteral("12345")},
                                       {QStringLiteral("pets"), QJsonArray{}}})
                           .toJson(QJsonDocument::Compact));
  legacySnapshot.close();
  QFile futureSnapshot(snapshotDirectory.filePath(QStringLiteral("2099-01-01.json")));
  ok &= require(futureSnapshot.open(QIODevice::WriteOnly),
                "future snapshot fixture could not be created");
  futureSnapshot.write(QJsonDocument(
                           QJsonObject{{QStringLiteral("schema"), 999},
                                       {QStringLiteral("account"), account},
                                       {QStringLiteral("createdAt"),
                                        QStringLiteral("2099-01-01T12:00:00")}})
                           .toJson(QJsonDocument::Compact));
  futureSnapshot.close();
  const QList<AccountAssetSnapshot> compatibleSnapshots = history(controller, true);
  bool foundLegacy = false;
  for (const auto& snapshot : compatibleSnapshots)
    if (snapshot.schemaVersion == AssetAnalysisVersion::kLegacySnapshotSchema &&
        snapshot.analysisVersion == 1 && snapshot.totalCurrentPower == 12345) foundLegacy = true;
  ok &= require(compatibleSnapshots.size() == 2 && foundLegacy,
                "legacy snapshot compatibility or future-schema isolation failed");

  // A malformed field cannot acquire authority from a separately true known
  // flag, and duplicate/unsafe instance IDs cannot define a comparison set.
  const QString malformedPath = snapshotDirectory.filePath(QStringLiteral("2001-01-01.json"));
  const auto rejectsMalformedSnapshot = [&](const QJsonObject& malformed) {
    QFile file(malformedPath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(malformed).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) return false;
    file.close();
    return history(controller, true).size() == 2;
  };
  const QJsonArray baselinePets = currentSnapshotObject.value(QStringLiteral("pets")).toArray();
  for (const auto& field : QList<QPair<QString, QJsonValue>>{
           {QStringLiteral("currentPower"), QStringLiteral("bad")},
           {QStringLiteral("currentPower"), 123.5},
           {QStringLiteral("fullyCultivated"), QJsonValue::Null},
           {QStringLiteral("redStarKnown"), QStringLiteral("true")},
           {QStringLiteral("instanceId"), 0},
           {QStringLiteral("instanceId"), 9007199254740992.0}}) {
    QJsonObject malformed = currentSnapshotObject;
    QJsonArray pets = baselinePets;
    QJsonObject invalidPet = pets.first().toObject();
    invalidPet.insert(field.first, field.second);
    invalidPet.insert(QStringLiteral("currentPowerKnown"), true);
    invalidPet.insert(QStringLiteral("cultivationKnown"), true);
    pets.replace(0, invalidPet);
    malformed.insert(QStringLiteral("pets"), pets);
    ok &= require(rejectsMalformedSnapshot(malformed),
                  "invalid snapshot field was silently converted into a known value");
  }
  QJsonObject duplicateSnapshot = currentSnapshotObject;
  QJsonArray duplicatePets = baselinePets;
  duplicatePets.append(duplicatePets.first());
  duplicateSnapshot.insert(QStringLiteral("pets"), duplicatePets);
  ok &= require(rejectsMalformedSnapshot(duplicateSnapshot),
                "duplicate snapshot instance ID was accepted");

  AssetSnapshotStore store(&repository);
  AccountAssetOverview trustedInput = analyze(controller);
  if (!require(trustedInput.pets.size() >= 2, "snapshot fault fixture has not loaded its account records")) return 1;
  AccountAssetOverview unverifiedInput = trustedInput;
  unverifiedInput.sourceVerified = false;
  ok &= require(!store.write(account, unverifiedInput).accepted,
                "a weak analysis borrowed a later trusted session to persist");
  AccountAssetOverview oldEpochInput = trustedInput;
  ++oldEpochInput.inputSessionEpoch;
  ok &= require(!store.write(account, oldEpochInput).accepted,
                "snapshot ignored its frozen input session");
  AccountAssetOverview oldRevisionInput = trustedInput;
  ++oldRevisionInput.inventoryRevision;
  ok &= require(!store.write(account, oldRevisionInput).accepted,
                "snapshot ignored its frozen inventory revision");
  AccountAssetOverview duplicateInput = trustedInput;
  duplicateInput.pets[1].instanceId = duplicateInput.pets.first().instanceId;
  ok &= require(!store.write(account, duplicateInput).accepted,
                "invalid snapshot write replaced the previous daily snapshot");

  AccountAssetSnapshot previous = snapshots.constFirst();
  for (AssetSnapshotPet& pet : previous.pets) {
    pet.currentPowerKnown = true;
    pet.cultivationKnown = true;
    pet.redStarKnown = true;
    pet.astrolabeKnown = true;
  }
  AccountAssetSnapshot current = previous;
  current.totalCurrentPower += 500;
  current.pets[1].currentPower += 500;
  AssetSnapshotPet newPet;
  newPet.instanceId = 3001;
  current.pets.append(newPet);
  current.pets[1].fullyCultivated = true;
  current.pets[1].redStarComplete = true;
  current.pets[1].astrolabeBreakthrough = true;
  const AssetSnapshotDelta delta =
      AssetAnalysisController::compareSnapshots(current, previous);
  ok &= require(delta.newPets == 1 && delta.newlyFullyCultivated == 1 &&
                    delta.newlyRedStarComplete == 1 &&
                    delta.newlyAstrolabeBreakthrough == 1 &&
                    delta.totalPowerChange == 500,
                "snapshot delta calculation is incorrect");
  AccountAssetSnapshot otherAccount = current;
  otherAccount.account = QStringLiteral("another-account");
  const auto unrelated = AssetAnalysisController::compareSnapshots(otherAccount, previous);
  ok &= require(!unrelated.accountComparable && unrelated.newPets == 0 &&
                    unrelated.totalPowerChange == 0,
                "cross-account snapshots must not compare");
  AccountAssetSnapshot oldAlgorithm = previous;
  --oldAlgorithm.analysisVersion;
  const auto changedRules = AssetAnalysisController::compareSnapshots(current, oldAlgorithm);
  ok &= require(!changedRules.cultivationComparable && changedRules.newPets == 1 &&
                    changedRules.newlyFullyCultivated == 0 && changedRules.newlyRedStarComplete == 0 &&
                    changedRules.newlyAstrolabeBreakthrough == 0 && changedRules.totalPowerChange == 500,
                "algorithm upgrade was counted as new cultivation");
  previous.pets[1].cultivationKnown = false;
  previous.pets[1].redStarKnown = false;
  previous.pets[1].astrolabeKnown = false;
  const auto unknownBefore = AssetAnalysisController::compareSnapshots(current, previous);
  ok &= require(unknownBefore.newlyFullyCultivated == 0 && unknownBefore.newlyRedStarComplete == 0 &&
                    unknownBefore.newlyAstrolabeBreakthrough == 0,
                "unknown old state was treated as an observed unfinished pet");

  AccountAssetSnapshot duplicatePrevious = previous;
  duplicatePrevious.pets.append(duplicatePrevious.pets.first());
  const auto duplicateDelta = AssetAnalysisController::compareSnapshots(current, duplicatePrevious);
  ok &= require(!duplicateDelta.accountComparable && duplicateDelta.newPets == 0 &&
                    duplicateDelta.totalPowerChange == 0,
                "duplicate previous instance made comparison depend on input order");
  AccountAssetSnapshot duplicateCurrent = current;
  duplicateCurrent.pets.append(duplicateCurrent.pets.first());
  ok &= require(!AssetAnalysisController::compareSnapshots(duplicateCurrent, previous).accountComparable,
                "duplicate current instance was silently discarded during comparison");

  AssetAnalysisController unknownProfile(&repository, &shop, &routine);
  const auto unknownProfileOverview = analyze(unknownProfile);
  bool unknownBecameReady = false;
  for (const auto& recommendation : unknownProfile.recommendations())
    unknownBecameReady |= recommendation.type == RecommendationType::ReadyNow;
  bool hasReadonlyPower = false;
  for (const auto& pet : unknownProfileOverview.pets) hasReadonlyPower |= pet.currentPowerKnown;
  ok &= require(unknownProfile.hasAnalysis() && !unknownProfileOverview.sourceVerified &&
                    !unknownProfile.inventoryAnalysisStale() && unknownProfile.shopAnalysisStale() &&
                    unknownProfileOverview.pets.size() == controller.overview().pets.size() && hasReadonlyPower &&
                    !unknownBecameReady && !unknownProfile.recordSnapshot(),
                "readonly local analysis was suppressed or gained permission for exchange/snapshot writes");
  const auto memory = controller.analysisMemoryUsage();
  ok &= require(memory.snapshotsCaptured > 0 && memory.computeSlices > 0 &&
                    memory.inputChargedBytes == 0 && memory.resultChargedBytes > 0 &&
                    !controller.overview().pets.isEmpty() && controller.overview().pets.first().memoryRetention,
                "full Compute result lost its retained-memory lease or input release barrier");
  const auto runsBeforeClosing = unknownProfile.analysisRunCount();
  ok &= require(unknownProfile.shutdownAnalysis(2000), "idle Compute thread did not shut down");
  unknownProfile.requestAnalysis();
  QCoreApplication::processEvents();
  ok &= require(!unknownProfile.analysisRunning() && unknownProfile.analysisRunCount() == runsBeforeClosing,
                "Closing admitted new analysis work");

  repository.markSessionUncertain(QStringLiteral("synthetic source loss"));
  ok &= require(!store.write(account, trustedInput).accepted,
                "lost session trust still allowed a snapshot write");
  qputenv("KQPET_DATA_ROOT", QDir(temporary.path()).filePath(QStringLiteral("weak-snapshot")).toUtf8());
  PetRepository weak;
  weak.handlePacket(QStringLiteral("recivedata"),
                    QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"weak-snapshot\"}}"));
  weak.beginListRefresh(1, weak.accountKey(), weak.sessionGeneration());
  weak.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(QJsonObject{
      {QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
      {QStringLiteral("pl"), QJsonArray{cultivatedPet(71, 7001, QStringLiteral("weak"), 11350, true)}},
      {QStringLiteral("pps"), QJsonArray{QStringLiteral("71")}},
      {QStringLiteral("ppc"), 12}}).toJson(QJsonDocument::Compact)));
  AssetAnalysisController weakAnalysis(&weak, nullptr, nullptr);
  analyze(weakAnalysis);
  ok &= require(!weakAnalysis.recordSnapshot() && weakAnalysis.snapshots().isEmpty(),
                "weak network observation escaped the persistence gate via asset snapshots");

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: account asset overview, diagnostics, and snapshots\n");
  return 0;
}
