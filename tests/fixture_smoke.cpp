#include "build_info.h"
#include "pet_detail_catalog.h"
#include "pet_power_calculator.h"
#include "pet_repository.h"
#include "protocol_test_support.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"
#include "target_compatibility_guard.h"
#include "target_profile_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

// Prints the actual analysis state next to a failing expectation. A bare FAIL
// line cannot distinguish a stale fixture expectation from a derivation bug.
void reportPower(const char* label, const PetBattlePowerState& power) {
  std::fprintf(stderr,
               "  %s: current=%d currentKnown=%d extreme=%d highest=%d highestKnown=%d slots=%d "
               "equipped=%d available=%d missing=%d backpackStars=%d currentStargod=%d "
               "bestOrdinary=%d changeable=%d target=%d full=%d isHighest=%d reasons=%s\n",
               label, power.current, int(power.hasCurrent), power.extreme, power.highest,
               int(power.hasHighest), power.stargodSlots, power.equippedStars, power.availableStars,
               power.missingStars, power.backpackStars, power.currentStargodPower,
               power.bestOrdinaryStargodPower, power.changeableStargodPower, power.targetStargodPower,
               int(power.stargodFull), int(power.isHighest),
               qPrintable(power.unknownReasons.join(QStringLiteral("；"))));
}

// The battle-power calculator's own frozen metadata view, so a fixture test
// exercises the same derivation the application performs.
PetBattlePowerState analyzeBattlePower(const QJsonObject& pet) {
  const auto& catalog = PetDetailCatalog::instance();
  return calculatePetBattlePower(pet, petPowerMetadataFromCatalog(pet,
      catalog.metadataFor(pet).value(QStringLiteral("stargodSlotMaxLevel")).toInt(),
      catalog.stargodDefinitions(), catalog.astrolabeDefinitions(), catalog.petDefinitions()));
}

// The confirmed protocol reply carries all eleven power components and uses an
// explicit zero for an inactive system (docs/精灵字段语义映射.md). Synthetic
// fixtures complete their component objects through this helper so they
// describe a real reply instead of a partial object that cannot establish a
// known total. It never invents a non-zero value.
QJsonObject completeComponents(QJsonObject parts) {
  for (const char* key : {"lv", "iv", "pl", "sgv", "ep", "gsv", "lav", "lsv", "bsv", "asv", "sjv"})
    if (!parts.contains(QLatin1String(key))) parts.insert(QLatin1String(key), 0);
  return parts;
}

QJsonObject completeReply(QJsonObject pet) {
  pet.insert(QStringLiteral("czdlv"),
             completeComponents(pet.value(QStringLiteral("czdlv")).toObject()));
  pet.insert(QStringLiteral("mzdlv"),
             completeComponents(pet.value(QStringLiteral("mzdlv")).toObject()));
  if (!pet.contains(QStringLiteral("sgsp"))) pet.insert(QStringLiteral("sgsp"), QJsonArray{});
  return pet;
}

QJsonObject loadFixture(const QString& relativePath, bool* ok) {
  const QString path = QDir(QStringLiteral(KQPET_FIXTURE_ROOT)).filePath(relativePath);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "FAIL: cannot open fixture: %s\n", qPrintable(path));
    *ok = false;
    return {};
  }

  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    std::fprintf(stderr, "FAIL: invalid fixture %s: %s\n", qPrintable(path),
                 qPrintable(error.errorString()));
    *ok = false;
    return {};
  }
  return document.object();
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
  if (!waitForRepositoryIdle(repository))
    qFatal("repository storage did not complete within the test budget");
}

void deliverPackets(PetRepository* repository, const QJsonObject& fixture) {
  for (const QJsonValue& value : fixture.value(QStringLiteral("packets")).toArray())
    deliver(repository, value.toObject());
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir dataRoot;
  bool ok = require(dataRoot.isValid(), "temporary data root unavailable");
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());

  const TargetCompatibilityReport nonTargetReport = TargetCompatibilityGuard::evaluate();
  ok &= require(!nonTargetReport.supported &&
                    QString::fromStdWString(nonTargetReport.extensionVersion) ==
                        BuildInfo::version() &&
                    QString::fromStdWString(nonTargetReport.format())
                        .contains(QStringLiteral("Result: UNSUPPORTED")),
                "non-KQPro process did not fail closed or report generated version");
  const TargetProfile* knownProfile =
      TargetProfileRegistry::find(L"KQProV1.1.3.exe", L"1.1.3");
  ok &= require(knownProfile && knownProfile->id == L"kqpro-1.1.3-x64" &&
                    knownProfile->dispatch.trampolinePolicy ==
                        TrampolinePolicy::ExactRelocationFreePrologue &&
                    TargetProfileRegistry::find(L"KQProV1.1.4.exe", L"1.1.4") &&
                    !TargetProfileRegistry::find(L"KQProV1.1.5.exe", L"1.1.5"),
                "target profile registry did not match known version or fail closed");

  PetRepository repository;
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("fixture-account")}}}});
  ok &= require(repository.isAuthenticated(), "fixture account login was not accepted");

  const QString account = repository.accountKey();
  const quint64 session = repository.sessionGeneration();
  const QJsonObject backpack =
      loadFixture(QStringLiteral("backpack/real_v1_sanitized.json"), &ok);
  const QJsonObject warehouse =
      loadFixture(QStringLiteral("warehouse/real_v1_sanitized.json"), &ok);

  repository.beginListRefresh(10, account, session);
  deliver(&repository, backpack);
  repository.expectListPart(QStringLiteral("2_1_S"), 10, account, session);
  deliver(&repository, warehouse);
  ok &= require(repository.backpackPets().size() == 1 &&
                    repository.warehousePets().size() == 2 &&
                    repository.backpackCapacity() == 56,
                "real inventory fixtures did not reach the repository");
  ok &= require(repository.backpackPet(900001)
                        .value(QStringLiteral("rt"))
                        .isString() &&
                    repository.backpackPet(900001)
                        .value(QStringLiteral("sppl"))
                        .isObject(),
                "string type or relationship pet was not preserved");

  const QJsonObject detail =
      loadFixture(QStringLiteral("detail/real_relationship_v1_sanitized.json"), &ok);
  repository.expectDetail(900001, 11, account, session);
  deliver(&repository, detail);
  ok &= require(repository.hasCachedDetail(900001) &&
                    repository.detailFor(900001).value(QStringLiteral("lv")).toInt() == 120 &&
                    repository.detailFor(900001)
                        .value(QStringLiteral("sppl"))
                        .toObject()
                        .value(QStringLiteral("id"))
                        .toInteger() == 900002,
                "real relationship detail fixture was not parsed and cached");
  const PetBattlePowerState analyzed = analyzeBattlePower(repository.detailFor(900001));
  const bool stablePower =
      analyzed.extreme == 29400 &&
      analyzed.stargodSlots == 7 && analyzed.equippedStars == 2 &&
      analyzed.missingStars == 5 && !analyzed.stargodFull &&
      !analyzed.componentGaps.isEmpty() &&
      // This frozen sample predates two confirmed protocol fields: five of the
      // eleven components and the per-pet stargod backpack. A partial object
      // must not be summed into a confident total.
      !analyzed.hasCurrent && analyzed.current == 0 &&
      !analyzed.hasHighest && analyzed.highest == 0;
  if (!stablePower) reportPower("partial frozen reply", analyzed);
  ok &= require(stablePower, "the partial frozen reply produced a fabricated total or lost its known parts");
  const PetBattlePowerState completeAnalyzed =
      analyzeBattlePower(completeReply(repository.detailFor(900001)));
  const bool completeTotals = completeAnalyzed.hasCurrent &&
      completeAnalyzed.current == 14547 && completeAnalyzed.extreme == 29400 &&
      completeAnalyzed.hasHighest && completeAnalyzed.highest == 30750;
  if (!completeTotals) reportPower("complete reply", completeAnalyzed);
  ok &= require(completeTotals, "a complete reply of the same observation lost the known totals");

  QJsonObject backpackStargodPet = completeReply(repository.detailFor(900001));
  backpackStargodPet.insert(
      QStringLiteral("sgsp"), QJsonArray{66, 67, 70, 77, 89, 80});
  const PetBattlePowerState backpackStargods = analyzeBattlePower(backpackStargodPet);
  // Owned ordinary stars are the equipped ones plus the per-pet backpack, and
  // one star per type: 9(6), 18(5), 66(4), 67(7), 70(5 duplicated), 77(22),
  // 89(28) give six usable types for seven slots. Star values follow the slot
  // level, which is 1 in this frozen observation, and the changeable slot uses
  // the best owned movable star (80, red) instead of the equipped gold one.
  const bool backpackCounted = backpackStargods.stargodBackpackKnown &&
      backpackStargods.backpackStars == 5 &&
      backpackStargods.availableStars == 6 &&
      !backpackStargods.stargodFull &&
      backpackStargods.missingStars == 1 &&
      backpackStargods.bestOrdinaryStargodPower == 740 &&
      backpackStargods.changeableStargodPower == 140 &&
      backpackStargods.currentStargodPower == 880 &&
      backpackStargods.highest == 30750;
  if (!backpackCounted) reportPower("per-pet stargod backpack", backpackStargods);
  ok &= require(backpackCounted,
                "per-pet stargod backpack was not counted without changeable stars");

  QJsonObject sixSlotPet = backpackStargodPet;
  sixSlotPet.insert(
      QStringLiteral("sgs"),
      QStringLiteral("66:8#67:8#0:8#0:8#0:8#0:8#79:8:79"));
  sixSlotPet.insert(QStringLiteral("sgsp"), QJsonArray{66, 67, 70, 80});
  const PetBattlePowerState sixSlotPower =
      analyzeBattlePower(sixSlotPet);
  // Six ordinary slots against three owned ordinary types (66, 67, 70); the
  // changeable star 80 must not consume an ordinary slot type.
  const bool sixSlots = sixSlotPower.stargodSlots == 6 && sixSlotPower.equippedStars == 2 &&
      sixSlotPower.backpackStars == 3 && !sixSlotPower.stargodFull &&
      sixSlotPower.missingStars == 3 && sixSlotPower.hasHighest && sixSlotPower.highest == 30100;
  if (!sixSlots) reportPower("six-slot stargod", sixSlotPower);
  ok &= require(sixSlots, "six-slot stargod capacity was not derived from the actual slot sequence");

  QJsonObject threeSlotPet = sixSlotPet;
  threeSlotPet.insert(QStringLiteral("sgs"), QStringLiteral("66:8#0:8#0:8"));
  threeSlotPet.insert(QStringLiteral("sgsp"), QJsonArray{66, 80});
  const PetBattlePowerState threeSlotPower =
      analyzeBattlePower(threeSlotPet);
  // Only star 66 is an owned ordinary star here; the changeable star 80 is
  // excluded from the ordinary type quota, so two of the three slots remain.
  const bool threeSlots = threeSlotPower.stargodSlots == 3 && threeSlotPower.availableStars == 1 &&
      threeSlotPower.missingStars == 2 && !threeSlotPower.stargodFull;
  if (!threeSlots) reportPower("three-slot stargod", threeSlotPower);
  ok &= require(threeSlots, "three-slot stargod capacity or changeable-star exclusion regressed");

  const PetDetailCatalog& detailCatalog = PetDetailCatalog::instance();
  ok &= require(detailCatalog.stargod(4)
                            .value(QStringLiteral("battlePower"))
                            .toObject()
                            .value(QStringLiteral("8"))
                            .toInt() == 90 &&
                    detailCatalog.stargod(7)
                            .value(QStringLiteral("battlePower"))
                            .toObject()
                            .value(QStringLiteral("8"))
                            .toInt() == 180 &&
                    detailCatalog.stargod(18)
                            .value(QStringLiteral("battlePower"))
                            .toObject()
                            .value(QStringLiteral("8"))
                            .toInt() == 260 &&
                    detailCatalog.stargod(51)
                            .value(QStringLiteral("battlePower"))
                            .toObject()
                            .value(QStringLiteral("8"))
                            .toInt() == 500 &&
                    detailCatalog.stargod(66)
                            .value(QStringLiteral("battlePower"))
                            .toObject()
                            .value(QStringLiteral("8"))
                            .toInt() == 650,
                "official green/blue/purple/gold/red stargod power table regressed");

  QJsonObject locallyCalculatedHighest = completeReply(QJsonObject{
      {QStringLiteral("ri"), 6506},
      {QStringLiteral("zdl"), 99999},
      {QStringLiteral("xzdl"), 99999},
      {QStringLiteral("astrolabebr"), true},
      // Three lit and selected outer nodes at 190 each (frozen catalog), so the
      // 灵初 breakthrough bonus of 150 applies to the reachable total while the
      // official extreme stays at the node sum alone.
      {QStringLiteral("astrolabe"),
       QStringLiteral("350:1:1#351:1:1#352:1:1")},
      {QStringLiteral("sgs"),
       QStringLiteral("66:8#67:8#70:8#71:8#72:8#73:8#74:8#80:8:80")},
      {QStringLiteral("sgsp"), QJsonArray{4, 51}},
      {QStringLiteral("czdlv"),
       QJsonObject{{QStringLiteral("lv"), 25400},
                   {QStringLiteral("sgv"), 1},
                   {QStringLiteral("asv"), 720}}},
      {QStringLiteral("mzdlv"),
       QJsonObject{{QStringLiteral("lv"), 25400},
                   {QStringLiteral("sgv"), 9999},
                   {QStringLiteral("asv"), 570}}}});
  const PetBattlePowerState localHighestPower =
      analyzeBattlePower(locallyCalculatedHighest);
  // 25400 non-stargod + 720 reachable astrolabe + 5200 stargod = 31320, and the
  // server zdl/xzdl/sgv replies (99999/99999/9999) are ignored.
  const bool localHighest = localHighestPower.current == 31320 &&
      localHighestPower.highest == 31320 && localHighestPower.targetStargodPower == 5200 &&
      localHighestPower.currentLocallyCalculated && localHighestPower.isHighest;
  if (!localHighest) reportPower("locally calculated highest", localHighestPower);
  ok &= require(localHighest, "highest power still depended on server zdl or server stargod power");

  QJsonObject backpackRedReplacement = locallyCalculatedHighest;
  backpackRedReplacement.insert(QStringLiteral("zdl"), 30360);
  backpackRedReplacement.insert(
      QStringLiteral("sgs"),
      QStringLiteral("66:8#67:8#70:8#71:8#72:8#73:8#18:8#80:8:80"));
  backpackRedReplacement.insert(QStringLiteral("sgsp"), QJsonArray{74});
  backpackRedReplacement.insert(
      QStringLiteral("czdlv"),
      completeComponents(QJsonObject{{QStringLiteral("lv"), 25400},
                                     {QStringLiteral("sgv"), 4810},
                                     {QStringLiteral("asv"), 720}}));
  const PetBattlePowerState backpackRedPower =
      analyzeBattlePower(backpackRedReplacement);
  const bool redReplacement = backpackRedPower.serverCurrent == 30360 &&
      backpackRedPower.equippedStargodPower == 4810 && backpackRedPower.currentStargodPower == 5200 &&
      backpackRedPower.current == 31320 && backpackRedPower.redStars == 7 &&
      backpackRedPower.missingRedStars == 0 && backpackRedPower.isHighest;
  if (!redReplacement) reportPower("backpack red replacement", backpackRedPower);
  ok &= require(redReplacement,
                "red stargods in the per-pet backpack did not replace the equipped purple stargod");

  bool rejectedMissingDetail = false;
  QObject::connect(&repository, &PetRepository::detailResponseRejected, &application,
                   [&](qint64 id, quint64 generation, const QString&) {
                     if (id == 900001 && generation == 12) rejectedMissingDetail = true;
                   });
  repository.expectDetail(900001, 12, account, session);
  deliver(&repository,
          loadFixture(QStringLiteral("detail/missing_level_synthetic.json"), &ok));
  ok &= require(rejectedMissingDetail &&
                    repository.detailFor(900001).value(QStringLiteral("lv")).toInt() == 120,
                "invalid detail overwrote the previous valid atomic cache");

  const QJsonObject dualJob =
      loadFixture(QStringLiteral("detail/dual_job_type_change_synthetic.json"), &ok);
  const QJsonObject dualPet = dualJob.value(QStringLiteral("p")).toObject();
  ok &= require(dualPet.value(QStringLiteral("lv")).isString() &&
                    dualPet.value(QStringLiteral("sgs")).isNull() &&
                    PetDetailCatalog::instance()
                        .jobs(dualPet.value(QStringLiteral("rt")).toString())
                        .contains(QStringLiteral(" / ")),
                "type-changing/null/dual-profession edge fixture was not preserved");

  // Establish a valid cached detail for the warehouse brief before changing
  // its visual race.  A list-to-list difference alone must not pretend that a
  // stale detail cache exists.
  repository.expectDetail(910002, 13, account, session);
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
           {QStringLiteral("p"),
            QJsonObject{{QStringLiteral("id"), 910002},
                        {QStringLiteral("r"), 6686},
                        {QStringLiteral("fr"), 6686},
                        {QStringLiteral("n"), QStringLiteral("皮肤变化基线")},
                        {QStringLiteral("lv"), 120}}}});
  qint64 visualMismatch = 0;
  QObject::connect(&repository, &PetRepository::visualMismatchDetected, &application,
                   [&](qint64 id) { visualMismatch = id; });
  repository.beginListRefresh(14, account, session);
  repository.expectListPart(QStringLiteral("2_1_S"), 14, account, session);
  deliver(&repository,
          loadFixture(QStringLiteral("warehouse/skin_change_synthetic.json"), &ok));
  ok &= require(visualMismatch == 910002 &&
                    repository.warehousePet(910002)
                        .value(QStringLiteral("_visualMismatch"))
                        .toBool(),
                "skin identity change did not invalidate stale detail state");

  deliver(&repository,
          loadFixture(QStringLiteral("formation/real_v1_sanitized.json"), &ok));
  ok &= require(repository.formationKnown() && repository.isDeployed(900001),
                "real formation alias fixture was not resolved");

  ShopExchangeController shop(&repository);
  QObject::connect(&repository, &PetRepository::packetObserved, &shop,
                   [&shop](const QJsonObject& packet, const InboundEnvelope& envelope) {
                     shop.handleDecodedEnvelope(envelope, packet);
                   });
  shop.setSender([](const QString&, const QString&, const QString&) { return true; });
  ok &= require(shop.requestInfo(), "fixture shop refresh did not start");
  deliver(&repository, loadFixture(QStringLiteral("shop/material_real_v1_sanitized.json"), &ok));
  deliver(&repository, loadFixture(QStringLiteral("shop/real_v1_sanitized.json"), &ok));
  ok &= require(shop.hasPacket() && shop.hasMaterialCounts() &&
                    shop.materialCounts().value(QStringLiteral("4:3237")) == 42,
                "real shop and material fixtures were not combined");

  RoutineOverviewController routine(&repository);
  QObject::connect(&repository, &PetRepository::packetObserved, &routine,
                   [&routine](const QJsonObject& packet, const InboundEnvelope& envelope) {
                     routine.handleDecodedEnvelope(envelope, packet);
                   });
  routine.setSender([](const QString&, const QString&, const QString&) { return true; });
  ok &= require(routine.requestRefresh(), "fixture routine refresh did not start");
  deliverPackets(&repository,
                 loadFixture(QStringLiteral("routine/real_v1_sanitized.json"), &ok));
  ok &= require(routine.hasDailyPacket() && routine.hasRedPointPacket() &&
                    routine.activeRedPoints().contains(10026) &&
                    routine.opportunityPackets()
                            .value(QStringLiteral("1008_20220603_swa_0_0"))
                            .toObject()
                            .value(QStringLiteral("ti"))
                            .toInt() == 2,
                "real routine fixtures were not aggregated");

  ok &= require(routine.requestRefresh(), "partial-failure routine refresh did not start");
  deliverPackets(&repository,
                 loadFixture(QStringLiteral("routine/partial_failure_synthetic.json"), &ok));
  ok &= require(routine.dailyPacket().value(QStringLiteral("av")).toInt() == 16 &&
                    routine.activeRedPoints().contains(10026) &&
                    routine.cachedOpportunityPackets()
                            .value(QStringLiteral("1008_20220603_swa_0_0"))
                            .toObject()
                            .value(QStringLiteral("ti"))
                            .toInt() == 2 &&
                    !routine.opportunityPackets().contains(QStringLiteral("1008_20220603_swa_0_0")) &&
                    routine.opportunityPackets().contains(QStringLiteral("16_24_A")) &&
                    !routine.opportunityPackets().contains(QStringLiteral("100_13_0")) &&
                    !routine.opportunityPackets().contains(QStringLiteral("100_2_0")),
                "one failed routine packet discarded siblings or replaced valid cache");

  if (!ok) return 2;
  std::fprintf(stdout, "PASS: sanitized protocol fixtures\n");
  return 0;
}
