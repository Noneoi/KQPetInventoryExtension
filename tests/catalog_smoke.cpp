#include "pet_detail_catalog.h"

#include <QCoreApplication>
#include <QDebug>

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
  ok &= require(catalog.stargod(78).value(QStringLiteral("quality")).toInt() == 6,
                "red stargod quality failed");
  ok &= require(catalog.stargod(79).value(QStringLiteral("changeable")).toBool(),
                "changeable stargod lookup failed");
  ok &= require(PetDetailCatalog::sacredMaxStar(1) == 8 &&
                    PetDetailCatalog::sacredMaxStage(6) == 7,
                "sacred equipment max-level lookup failed");
  if (!ok)
    return 1;
  std::fprintf(stdout, "PASS: embedded pet-detail catalog\n");
  return 0;
}
