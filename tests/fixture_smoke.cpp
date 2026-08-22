#include "build_info.h"
#include "pet_detail_analyzer.h"
#include "pet_detail_catalog.h"
#include "pet_detail_renderer.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"
#include "target_compatibility_guard.h"

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

QString compact(const QJsonObject& packet) {
  return QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  repository->handlePacket(QStringLiteral("recivedata"), compact(packet));
}

template <typename Controller>
void deliver(Controller* controller, const QJsonObject& packet) {
  controller->handlePacket(QStringLiteral("recivedata"), compact(packet));
}

void deliverPackets(RoutineOverviewController* controller, const QJsonObject& fixture) {
  for (const QJsonValue& value : fixture.value(QStringLiteral("packets")).toArray())
    deliver(controller, value.toObject());
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
  ok &= require(analyzed.instanceId == 900001 && analyzed.raceId == 6506 &&
                    analyzed.battlePower.current == 14547 &&
                    analyzed.battlePower.extreme == 29400 &&
                    analyzed.battlePower.highest == 30750 &&
                    analyzed.battlePower.highestGap == 16203 &&
                    !analyzed.battlePower.componentGaps.isEmpty(),
                "detail analyzer did not produce the expected stable view model");
  ok &= require(analyzed.badges.size() == 2 && analyzed.astrolabe.stars.size() >= 5 &&
                    analyzed.stargods.size() == 3 &&
                    analyzed.relationships.summonRows.size() == 1 &&
                    analyzed.relationships.summonRows.constFirst().pets.constFirst().name ==
                        QStringLiteral("[灵初]超凡无双·超能"),
                "badge, astrolabe, stargod, or relationship analysis was not moved into the view model");
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
                    renderedDetail.contains(QStringLiteral("战斗力分析")) &&
                    renderedDetail.contains(QStringLiteral("</html>")),
                "detail renderer did not compose the analyzed sections into a document");

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
  shop.setSender([](const QString&, const QString&, const QString&) { return true; });
  ok &= require(shop.requestInfo(), "fixture shop refresh did not start");
  deliver(&shop, loadFixture(QStringLiteral("shop/material_real_v1_sanitized.json"), &ok));
  deliver(&shop, loadFixture(QStringLiteral("shop/real_v1_sanitized.json"), &ok));
  ok &= require(shop.hasPacket() && shop.hasMaterialCounts() &&
                    shop.materialCounts().value(QStringLiteral("4:3237")) == 42,
                "real shop and material fixtures were not combined");

  RoutineOverviewController routine(&repository);
  routine.setSender([](const QString&, const QString&, const QString&) { return true; });
  ok &= require(routine.requestRefresh(), "fixture routine refresh did not start");
  deliverPackets(&routine,
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
  deliverPackets(&routine,
                 loadFixture(QStringLiteral("routine/partial_failure_synthetic.json"), &ok));
  ok &= require(routine.dailyPacket().value(QStringLiteral("av")).toInt() == 16 &&
                    routine.activeRedPoints().contains(10026) &&
                    routine.opportunityPackets()
                            .value(QStringLiteral("1008_20220603_swa_0_0"))
                            .toObject()
                            .value(QStringLiteral("ti"))
                            .toInt() == 2 &&
                    routine.opportunityPackets()
                            .value(QStringLiteral("16_24_A"))
                            .toObject()
                            .value(QStringLiteral("sweep"))
                            .toInt() == 4,
                "one failed routine packet discarded siblings or replaced valid cache");

  if (!ok) return 2;
  std::fprintf(stdout, "PASS: sanitized protocol fixtures\n");
  return 0;
}
