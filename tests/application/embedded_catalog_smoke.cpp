#include "application/catalog/pet_detail_catalog.h"
#include "application/catalog/pet_skill_catalog.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "application/catalog/routine_overview_catalog.h"
#include "domain/pet_relation_options.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); QTemporaryDir empty;
  qputenv("KQPET_DATA_ROOT",empty.path().toUtf8());
  const auto pets = PetDetailCatalog::instance().snapshot();
  const auto skills = PetSkillCatalog::instance().snapshot();
  const auto shops = ShopExchangeCatalog::instance().snapshot();
  const auto routines = RoutineOverviewCatalog::instance().snapshot();
  bool ok = pets && pets->loaded && pets->sourceUpdatedAt.isValid() &&
      skills && skills->loaded && skills->sourceUpdatedAt == pets->sourceUpdatedAt &&
      shops && shops->loaded && shops->sourceUpdatedAt == pets->sourceUpdatedAt &&
      routines && routines->sourceUpdatedAt == pets->sourceUpdatedAt && !routines->tasks.isEmpty() && !routines->activities.isEmpty();
  if (skills) for (const QString& kind : {QStringLiteral("summon"),QStringLiteral("carry")}) {
    const auto group = skills->root.value("relationRules").toObject().value(kind).toObject();
    const auto sources = group.value("bySourceRaceId").toObject();
    ok &= group.value("available").toBool() && !sources.isEmpty();
    for (auto it = sources.begin(); it != sources.end(); ++it)
      ok &= petRelationCandidateOptions(skills->root,it.key().toInt(),kind == "carry").available;
  }
  if (!ok) std::fprintf(stderr,"FAIL: clean offline installation lacks coherent embedded public catalogs or relation rules\n");
  return ok ? 0 : 1;
}
