#include "asset_analysis_controller.h"
#include "asset_analysis_version.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"

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
  repository->handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

QJsonObject cultivatedPet(qint64 id, int race, const QString& name,
                          int current, bool full) {
  return {{QStringLiteral("id"), id},
          {QStringLiteral("r"), race},
          {QStringLiteral("fr"), race},
          {QStringLiteral("n"), name},
          {QStringLiteral("lv"), 120},
          {QStringLiteral("zdl"), current},
          {QStringLiteral("xzdl"), 10000},
          {QStringLiteral("astrolabebr"), full},
          {QStringLiteral("sgs"),
           full ? QJsonValue(QStringLiteral(
                      "66:8#67:8#70:8#71:8#72:8#73:8#74:8#80:8:80"))
                : QJsonValue(QStringLiteral(
                      "66:8#67:8#70:8#0:8#0:8#0:8#0:8#80:8:80"))},
          {QStringLiteral("sgsp"),
           full ? QJsonValue(QJsonArray{71, 72, 73, 74})
                : QJsonValue(QJsonArray{})},
          {QStringLiteral("czdlv"),
           QJsonObject{{QStringLiteral("lv"),
                        full ? current - 5500 : current - 2615},
                       {QStringLiteral("sgv"), full ? 5200 : 2600},
                       {QStringLiteral("asv"), full ? 150 : 0},
                       {QStringLiteral("bsv"), full ? 100 : 10},
                       {QStringLiteral("sjv"), full ? 50 : 5}}},
          {QStringLiteral("mzdlv"),
           QJsonObject{{QStringLiteral("lv"), 5850},
                       {QStringLiteral("sgv"), 4000},
                       {QStringLiteral("asv"), 0},
                       {QStringLiteral("bsv"), 100},
                       {QStringLiteral("sjv"), 50}}}};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

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
  const AccountInventorySummary inventory = controller.inventorySummary();
  ok &= require(inventory.totalPets == 4 && inventory.backpackPets == 2 &&
                    inventory.normalWarehousePets == 1 &&
                    inventory.eliteWarehousePets == 1,
                "lightweight inventory summary counts are incorrect");
  const AccountAssetOverview overview = controller.recalculateOverview();
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
      ok &= require(pet.currentPower == 5000 && pet.extremePower == 10000 &&
                        pet.highestPower == 11350,
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
  const QList<AccountAssetSnapshot> snapshots = controller.snapshots();
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

  const AccountAssetOverview refreshed = controller.recalculateOverview();
  ok &= require(controller.dirtyPetIds().isEmpty() &&
                    !controller.inventoryAnalysisStale() &&
                    refreshed.totalCurrentPower != overview.totalCurrentPower,
                "manual refresh did not clear dirty state and recompute all pets");

  QMetaObject::invokeMethod(&shop, "infoUpdated", Qt::DirectConnection);
  ok &= require(shopInvalidations == 1 && controller.shopAnalysisStale() &&
                    !controller.inventoryAnalysisStale() &&
                    controller.dirtyPetIds().isEmpty(),
                "shop update affected more than shop analysis freshness");
  controller.recalculateOverview();
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
  controller.recalculateOverview();

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
  controller.recalculateOverview();

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
  controller.recalculateOverview();

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
  ok &= require(controller.autoSnapshotEnabled() && controller.snapshots().size() == 1 &&
                    controller.hasAnalysis() &&
                    controller.overview().account == QStringLiteral("asset-account") &&
                    !controller.shopAnalysisStale(),
                "account-specific analysis and snapshot settings were not restored");

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
  const QList<AccountAssetSnapshot> compatibleSnapshots = controller.snapshots();
  ok &= require(compatibleSnapshots.size() == 2 &&
                    compatibleSnapshots.constFirst().schemaVersion ==
                        AssetAnalysisVersion::kLegacySnapshotSchema &&
                    compatibleSnapshots.constFirst().analysisVersion == 1 &&
                    compatibleSnapshots.constFirst().totalCurrentPower == 12345,
                "legacy snapshot compatibility or future-schema isolation failed");

  AccountAssetSnapshot previous = snapshots.constFirst();
  AccountAssetSnapshot current = previous;
  current.totalCurrentPower += 500;
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

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: account asset overview, diagnostics, and snapshots\n");
  return 0;
}
