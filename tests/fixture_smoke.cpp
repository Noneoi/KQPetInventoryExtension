#include "build_info.h"
#include "pet_detail_analyzer.h"
#include "pet_detail_catalog.h"
#include "pet_detail_renderer.h"
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

void reportBattlePower(const PetDetailViewModel& model) {
  const PetBattlePowerState& power = model.battlePower;
  std::fprintf(stderr,
               "  detail view model: instance=%lld race=%d level=%d current=%d extreme=%d hasHighest=%d "
               "highest=%d currentStargodPower=%d equippedStargodPower=%d slots=%d slotsKnown=%d "
               "equippedStars=%d availableStars=%d missingStars=%d backpackKnown=%d stargodFull=%d "
               "gaps=%d reasons=%s\n",
               static_cast<long long>(model.instanceId), model.raceId, model.level, power.current,
               power.extreme, int(power.hasHighest), power.highest, power.currentStargodPower,
               power.equippedStargodPower, power.stargodSlots, int(power.stargodSlotsKnown),
               power.equippedStars, power.availableStars, power.missingStars,
               int(power.stargodBackpackKnown), int(power.stargodFull),
               int(power.componentGaps.size()),
               qPrintable(power.unknownReasons.join(QStringLiteral("；"))));
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
  const PetDetailViewModel analyzed =
      PetDetailAnalyzer::analyze(repository.detailFor(900001), &repository);
  const bool stableViewModel =
      analyzed.instanceId == 900001 && analyzed.raceId == 6506 &&
      analyzed.battlePower.extreme == 29400 &&
      analyzed.battlePower.stargodSlots == 7 && analyzed.battlePower.equippedStars == 2 &&
      analyzed.battlePower.missingStars == 5 && !analyzed.battlePower.stargodFull &&
      !analyzed.battlePower.componentGaps.isEmpty() &&
      // This frozen sample predates two confirmed protocol fields: five of the
      // eleven components and the per-pet stargod backpack. A partial object
      // must not be summed into a confident total.
      !analyzed.battlePower.hasCurrent && analyzed.battlePower.current == 0 &&
      !analyzed.battlePower.hasHighest && analyzed.battlePower.highest == 0;
  if (!stableViewModel) reportBattlePower(analyzed);
  ok &= require(stableViewModel, "the partial frozen reply produced a fabricated total or lost its known parts");
  const PetDetailViewModel completeAnalyzed =
      PetDetailAnalyzer::analyze(completeReply(repository.detailFor(900001)), &repository);
  const bool completeTotals = completeAnalyzed.battlePower.hasCurrent &&
      completeAnalyzed.battlePower.current == 14547 && completeAnalyzed.battlePower.extreme == 29400 &&
      completeAnalyzed.battlePower.hasHighest && completeAnalyzed.battlePower.highest == 30750;
  if (!completeTotals) reportBattlePower(completeAnalyzed);
  ok &= require(completeTotals, "a complete reply of the same observation lost the known totals");
  ok &= require(analyzed.badges.size() == 2 && analyzed.astrolabe.stars.size() >= 5 &&
                    analyzed.stargods.size() == 3 &&
                    analyzed.relationships.summonRows.size() == 1 &&
                    analyzed.relationships.summonRows.constFirst().pets.constFirst().name ==
                        QStringLiteral("[灵初]超凡无双·超能"),
                "badge, astrolabe, stargod, or relationship analysis was not moved into the view model");
  QJsonObject astrolabeSemanticsPet = repository.detailFor(900001);
  astrolabeSemanticsPet.insert(
      QStringLiteral("astrolabe"),
      QStringLiteral("350:0:1#351:1:0#422:0:0"));
  const PetDetailViewModel astrolabeSemantics =
      PetDetailAnalyzer::analyze(astrolabeSemanticsPet, &repository);
  ok &= require(astrolabeSemantics.astrolabe.stars.size() == 3 &&
                    !astrolabeSemantics.astrolabe.stars.at(0).activated &&
                    astrolabeSemantics.astrolabe.stars.at(0).selected &&
                    astrolabeSemantics.astrolabe.stars.at(1).activated &&
                    !astrolabeSemantics.astrolabe.stars.at(1).selected &&
                    astrolabeSemantics.astrolabe.activatedCount == 1 &&
                    astrolabeSemantics.astrolabe.selectedCount == 1,
                "astrolabe activated and equipped fields were conflated");
  QJsonObject cultivatedPet = repository.detailFor(900001);
  cultivatedPet.insert(QStringLiteral("gt"), 6);
  cultivatedPet.insert(QStringLiteral("ip"), QStringLiteral("100#200"));
  cultivatedPet.insert(QStringLiteral("gps"), QStringLiteral("2#3"));
  cultivatedPet.insert(QStringLiteral("shenjue"), QStringLiteral("1034#2#6|9:6"));
  const PetDetailViewModel cultivated =
      PetDetailAnalyzer::analyze(cultivatedPet, &repository);
  ok &= require(cultivated.talent.levelName == QStringLiteral("超凡入圣") &&
                    cultivated.talent.normalLines.size() == 1 &&
                    cultivated.talent.doubleEnergyLines.size() == 1 &&
                    cultivated.sacred.equipped && cultivated.sacred.star == 9 &&
                    cultivated.sacred.stage == 6,
                "talent or sacred-beast analysis did not produce semantic state");
  ok &= require(PetDetailRenderer::text(QStringLiteral("<x>")) ==
                    QStringLiteral("&lt;x&gt;") &&
                    PetDetailRenderer::document(QStringLiteral("H"), QStringLiteral("B"))
                        .contains(QStringLiteral("<body>HB</body>")),
                "detail renderer did not escape values or compose the document");
  const QString renderedDetail = PetDetailRenderer::render(analyzed);
  ok &= require(renderedDetail.contains(QStringLiteral("基础信息")) &&
                    renderedDetail.contains(QStringLiteral("召唤关系")) &&
                    renderedDetail.contains(QStringLiteral("精灵星神背包")) &&
                    renderedDetail.contains(QStringLiteral("详情未返回背包数据")) &&
                    renderedDetail.contains(QStringLiteral("战斗力分析")) &&
                    renderedDetail.contains(QStringLiteral("</html>")),
                "detail renderer did not compose the analyzed sections into a document");

  PetDetailViewModel gapModel;
  gapModel.available = true;
  gapModel.instanceId = 42;
  gapModel.name = QStringLiteral("差距文案测试");
  gapModel.battlePower.hasCurrent = true;
  gapModel.battlePower.hasExtreme = true;
  gapModel.battlePower.hasHighest = true;
  gapModel.battlePower.serverCurrent = 10000;
  gapModel.battlePower.current = 11000;
  gapModel.battlePower.extreme = 12000;
  gapModel.battlePower.highest = 13000;
  gapModel.battlePower.highestGap = 2000;
  gapModel.battlePower.currentLocallyCalculated = true;
  gapModel.battlePower.stargodSlotsKnown = true;
  gapModel.battlePower.stargodSlots = 4;
  gapModel.battlePower.availableStars = 4;
  gapModel.battlePower.goldStars = 4;
  gapModel.battlePower.stargodLevelsFull = true;
  // The renderer reads the per-component rows, not the legacy componentGaps
  // list, so the model below uses the current contract.
  gapModel.battlePower.components = {
      {QStringLiteral("lv"), QStringLiteral("等级与基础成长"), {}, 900, 1000, 1000, 100,
       true, true, true, true, true},
      {QStringLiteral("bsv"), QStringLiteral("元魂"), {}, 100, 200, 200, 100,
       true, true, true, true, true},
      {QStringLiteral("asv"), QStringLiteral("天迹星轮"), {}, 50, 100, 100, 50,
       true, true, true, true, true},
      {QStringLiteral("sjv"), QStringLiteral("神源兽"), {}, 200, 400, 400, 200,
       true, true, true, true, true}};
  gapModel.battlePower.componentGaps = {
      {QStringLiteral("lv"), QStringLiteral("等级与基础成长"), 900, 1000, 100},
      {QStringLiteral("bsv"), QStringLiteral("元魂"), 100, 200, 100},
      {QStringLiteral("asv"), QStringLiteral("天迹星轮"), 50, 100, 50},
      {QStringLiteral("sjv"), QStringLiteral("神源兽"), 200, 400, 200}};
  gapModel.badges.append({QStringLiteral("神攻"), 1,
                          QStringLiteral("专属元魂"), false});
  gapModel.sacred.equipped = true;
  gapModel.sacred.star = 7;
  gapModel.sacred.maxStar = 9;
  gapModel.sacred.stage = 4;
  gapModel.sacred.maxStage = 6;
  gapModel.astrolabe.stars = {
      {QStringLiteral("已装备但未点亮"), {}, false, false, true},
      {QStringLiteral("已点亮但未装备"), {}, false, true, false},
      {QStringLiteral("专属节点"), {QStringLiteral("应显示材料")}, true, false, false},
      {QStringLiteral("专属已点亮"), {QStringLiteral("不应显示材料")}, true, true, false}};
  gapModel.battlePower.breakthroughApplicable = true;
  gapModel.battlePower.breakthroughApplicabilityKnown = true;
  gapModel.battlePower.breakthroughKnown = true;
  gapModel.battlePower.astrolabeApplicabilityKnown = true;
  gapModel.battlePower.astrolabeApplicable = true;
  gapModel.astrolabe.activatedCount = 2;
  gapModel.astrolabe.selectedCount = 1;
  const QString renderedGaps = PetDetailRenderer::render(gapModel);
  // The confirmed presentation is per component ("当前 / 官方极限分项 / 至高分项 /
  // 尚缺") and per astrolabe node; the older aggregate phrasings no longer exist.
  const bool gapDescriptions =
      renderedGaps.contains(QStringLiteral(
          "当前 900 / 官方极限分项 1000 / 至高分项 1000 / 尚缺 100")) &&
      renderedGaps.contains(QStringLiteral("专属元魂 · 未觉醒")) &&
      renderedGaps.contains(QStringLiteral("7/9 星（未满星）")) &&
      renderedGaps.contains(QStringLiteral("4/6 阶（未满阶），还差 2 阶")) &&
      renderedGaps.contains(QStringLiteral("font-weight:700'>已装备但未点亮（未点亮）</span>")) &&
      renderedGaps.contains(QStringLiteral("<span>已点亮但未装备</span>")) &&
      renderedGaps.contains(QStringLiteral("专属节点 <span class='muted'>（点亮需 应显示材料）</span>（未点亮）")) &&
      renderedGaps.contains(QStringLiteral(">未突破</span>")) &&
      !renderedGaps.contains(QStringLiteral("不应显示材料")) &&
      !renderedGaps.contains(QStringLiteral("已点亮、已装备")) &&
      !renderedGaps.contains(QStringLiteral("已点亮数量")) &&
      !renderedGaps.contains(QStringLiteral("已装备数量")) &&
      !renderedGaps.contains(QStringLiteral("暂未突破"));
  if (!gapDescriptions) {
    std::fputs("  rendered gap document:\n", stderr);
    std::fputs(qPrintable(renderedGaps), stderr);
    std::fputc('\n', stderr);
  }
  ok &= require(gapDescriptions, "ordinary/highest power gap descriptions lost semantic detail");

  QJsonObject backpackStargodPet = completeReply(repository.detailFor(900001));
  backpackStargodPet.insert(
      QStringLiteral("sgsp"), QJsonArray{66, 67, 70, 77, 89, 80});
  const PetDetailViewModel backpackStargods =
      PetDetailAnalyzer::analyze(backpackStargodPet, &repository);
  // Owned ordinary stars are the equipped ones plus the per-pet backpack, and
  // one star per type: 9(6), 18(5), 66(4), 67(7), 70(5 duplicated), 77(22),
  // 89(28) give six usable types for seven slots. Star values follow the slot
  // level, which is 1 in this frozen observation, and the changeable slot uses
  // the best owned movable star (80, red) instead of the equipped gold one.
  const bool backpackCounted = backpackStargods.stargodBackpack.size() == 6 &&
      backpackStargods.battlePower.stargodBackpackKnown &&
      backpackStargods.battlePower.backpackStars == 5 &&
      backpackStargods.battlePower.availableStars == 6 &&
      !backpackStargods.battlePower.stargodFull &&
      backpackStargods.battlePower.missingStars == 1 &&
      backpackStargods.battlePower.bestOrdinaryStargodPower == 740 &&
      backpackStargods.battlePower.changeableStargodPower == 140 &&
      backpackStargods.battlePower.currentStargodPower == 880 &&
      backpackStargods.battlePower.highest == 30750 &&
      !PetDetailRenderer::render(backpackStargods).contains(QStringLiteral("数量已满足满战力"));
  if (!backpackCounted) reportPower("per-pet stargod backpack", backpackStargods.battlePower);
  ok &= require(backpackCounted,
                "per-pet stargod backpack was not displayed or counted without changeable stars");

  QJsonObject sixSlotPet = backpackStargodPet;
  sixSlotPet.insert(
      QStringLiteral("sgs"),
      QStringLiteral("66:8#67:8#0:8#0:8#0:8#0:8#79:8:79"));
  sixSlotPet.insert(QStringLiteral("sgsp"), QJsonArray{66, 67, 70, 80});
  const PetBattlePowerState sixSlotPower =
      PetDetailAnalyzer::analyzeBattlePower(sixSlotPet);
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
      PetDetailAnalyzer::analyzeBattlePower(threeSlotPet);
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
      PetDetailAnalyzer::analyzeBattlePower(locallyCalculatedHighest);
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
      PetDetailAnalyzer::analyzeBattlePower(backpackRedReplacement);
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
