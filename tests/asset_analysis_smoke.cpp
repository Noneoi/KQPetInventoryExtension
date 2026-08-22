#include "asset_analysis_controller.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

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
          {QStringLiteral("sgs"), QStringLiteral("1:1#2:1#3:1")},
          {QStringLiteral("czdlv"),
           QJsonObject{{QStringLiteral("sgv"), full ? 1200 : 0},
                       {QStringLiteral("asv"), full ? 150 : 0},
                       {QStringLiteral("bsv"), full ? 100 : 10},
                       {QStringLiteral("sjv"), full ? 50 : 5}}},
          {QStringLiteral("mzdlv"),
           QJsonObject{{QStringLiteral("sgv"), 0},
                       {QStringLiteral("asv"), full ? 0 : 100},
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
  const AccountAssetOverview overview = controller.overview();
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

  ok &= require(controller.recordSnapshot(), "snapshot could not be recorded");
  const QList<AccountAssetSnapshot> snapshots = controller.snapshots();
  ok &= require(snapshots.size() == 1 && snapshots.constFirst().totalPets == 4 &&
                    snapshots.constFirst().pets.size() == 4,
                "snapshot did not preserve lightweight account assets");
  controller.setAutoSnapshotEnabled(true);
  bool analysisInvalidated = false;
  QObject::connect(&controller, &AssetAnalysisController::analysisInvalidated,
                   &application, [&analysisInvalidated]() {
                     analysisInvalidated = true;
                   });
  repository.expectDetail(2002, 3, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
           {QStringLiteral("p"),
            cultivatedPet(2002, 7102, QStringLiteral("精英待培养"), 7100, false)}});
  ok &= require(analysisInvalidated,
                "detail update did not mark manual analysis as stale");
  ok &= require(controller.snapshots().size() == 1,
                "background detail update unexpectedly wrote a snapshot");

  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("other-account")}}}});
  ok &= require(!controller.autoSnapshotEnabled() && controller.snapshots().isEmpty(),
                "snapshot history or settings leaked into another account");
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("asset-account")}}}});
  ok &= require(controller.autoSnapshotEnabled() && controller.snapshots().size() == 1,
                "account-specific snapshot settings were not restored");

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
