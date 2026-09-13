#include "pet_detail_catalog.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonObject>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  bool ok = true;
  ok &= require(catalog.isLoaded(), "embedded catalog did not load");
  const QJsonObject fateWheel{{QStringLiteral("r"),7529},
      {QStringLiteral("n"),QStringLiteral("[灵初]轮转·命运之轮")},{QStringLiteral("rt"),26},
      {QStringLiteral("_metaAttributes"),QStringLiteral("24")},{QStringLiteral("_metaJobs"),QStringLiteral("26")}};
  ok &= require(catalog.resolvedOriginalName(fateWheel) == QStringLiteral("[灵初]轮转·命运之轮") &&
      catalog.resolvedAttributes(fateWheel) == QStringLiteral("神灵") &&
      catalog.resolvedJobs(fateWheel) == QStringLiteral("神攻") &&
      catalog.pet(7529).value(QStringLiteral("stargodSlotMaxLevel")).toInt() == 8,
      "current official 7529 identity was missing or stale observation metadata overrode it");
  ok &= require(catalog.petName(7135) == QStringLiteral("逆时空·湮灭神女"),
                "skin name lookup failed");
  ok &= require(catalog.originalName(7135) == QStringLiteral("[灵初]湮灭守望·龙尊"),
                "original name lookup failed");
  ok &= require(catalog.attributes(QStringLiteral("24")) == QStringLiteral("神暗"),
                "attribute lookup failed");
  ok &= require(catalog.jobs(QStringLiteral("22")) == QStringLiteral("神攻"),
                "job lookup failed");
  ok &= require(catalog.badgeName(611) == QStringLiteral("神攻·夯实基础"),
                "badge lookup failed");
  ok &= require(catalog.sacredEquipmentName(1038) == QStringLiteral("神·时空圣龙源兽"),
                "sacred equipment lookup failed");
  ok &= require(catalog.astrolabeName(350) == QStringLiteral("星灵·暴击"),
                "astrolabe lookup failed");
  ok &= require(catalog.astrolabe(422).value(QStringLiteral("exclusive")).toBool() &&
                    !catalog.astrolabe(422).value(QStringLiteral("lightUpCost")).toString().isEmpty(),
                "exclusive astrolabe cost lookup failed");
  ok &= require(catalog.itemName(2079) == QStringLiteral("时空精华"),
                "exclusive essence item name lookup failed");
  ok &= require(catalog.moneyName(33) == QStringLiteral("金豆"),
                "astrolabe money name lookup failed");
  ok &= require(catalog.materialCostText(4, 2079, 160) == QStringLiteral("时空精华 ×160") &&
                    catalog.materialCostText(8, 33, 8000) == QStringLiteral("金豆 ×8000"),
                "exclusive star cost text did not use official material names");
  ok &= require(catalog.stargod(78).value(QStringLiteral("quality")).toInt() == 6,
                "red stargod quality failed");
  ok &= require(catalog.stargod(79).value(QStringLiteral("changeable")).toBool(),
                "changeable stargod lookup failed");
  ok &= require(catalog.sacredMaxStar(1) == 8 &&
                    catalog.sacredMaxStage(6) == 7,
                "sacred equipment max-level lookup failed");
  const QJsonObject qiankun{
      {QStringLiteral("r"), 7516},
      {QStringLiteral("n"), QStringLiteral("[灵初]五行御玄·乾坤")},
      {QStringLiteral("rt"), 26}};
  ok &= require(catalog.resolvedEra(qiankun) == QStringLiteral("灵初"),
                "missing-dictionary era must come from live name prefix");
  ok &= require(catalog.resolvedJobs(qiankun) == QStringLiteral("元素师 / 神速"),
                "current official dual job was replaced by unrelated live rt");
  ok &= require(catalog.resolvedOriginalName(qiankun) ==
                    QStringLiteral("[灵初]五行御玄·乾坤"),
                "missing-dictionary original name must use live name");
  ok &= require(catalog.resolvedAttributes(qiankun) == QStringLiteral("神灵"),
                "current official attribute lookup failed");
  const QJsonObject futureEvolution{{QStringLiteral("r"),990002},
      {QStringLiteral("n"),QStringLiteral("[未来]轮转·命运之轮")},{QStringLiteral("rt"),26}};
  const auto futureEnriched = catalog.enrichMetadata(futureEvolution);
  ok &= require(catalog.resolvedJobs(futureEvolution) == QStringLiteral("—") &&
      catalog.resolvedAttributes(futureEvolution) == QStringLiteral("—") &&
      !futureEnriched.contains(QStringLiteral("_metaJobs")) && !futureEnriched.contains(QStringLiteral("_metaAttributes")),
      "unlisted new evolution guessed its job from rt or borrowed an older same-name attribute");
  const QJsonObject futureSkin{
      {QStringLiteral("r"), 990001},
      {QStringLiteral("n"), QStringLiteral("未来皮肤名")},
      {QStringLiteral("rt"), 22},
      {QStringLiteral("_metaOriginalName"), QStringLiteral("[灵初]原始精灵名")},
      {QStringLiteral("_metaAttributes"), QStringLiteral("26")},
      {QStringLiteral("_metaJobs"), QStringLiteral("22")},
      {QStringLiteral("_metaEra"), QStringLiteral("灵初")},
      {QStringLiteral("_metaRaceId"), 7516}};
  ok &= require(catalog.resolvedOriginalName(futureSkin) ==
                        QStringLiteral("[灵初]原始精灵名") &&
                    catalog.resolvedAttributes(futureSkin) == QStringLiteral("神灵") &&
                    catalog.resolvedJobs(futureSkin) == QStringLiteral("神攻") &&
                    catalog.resolvedEra(futureSkin) == QStringLiteral("灵初"),
                "unknown future skin must inherit the stable instance metadata snapshot");
  if (!ok)
    return 1;
  std::fprintf(stdout, "PASS: embedded pet-detail catalog\n");
  return 0;
}
