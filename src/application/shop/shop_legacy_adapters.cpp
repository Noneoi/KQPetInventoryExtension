#include "shop_legacy_adapters.h"
#include "domain/compiled_shop_catalog.h"
#include "domain/prepared_shop_conditions.h"
#include "application/catalog/pet_detail_catalog.h"

ShopActionability evaluateShopActionability(
    const ShopExchangeGood& good, const QJsonObject& pet,
    const QJsonObject& shopPacket, const AccountResourceView& resources,
    const ShopConditionContext& context) {
  // Thin legacy adapter; all condition semantics live in the prepared pipeline.
  CompiledShopCatalog compiled = CompiledShopCatalog::compile({good});
  QHash<QString, QString> names;
  for (const ResourceRequirement& requirement : compiled.goods().first().requirements) {
    const QStringList key = requirement.resourceKey.split(QLatin1Char(':'));
    names.insert(requirement.resourceKey, PetDetailCatalog::instance().materialName(
        key.value(0).toInt(), key.value(1).toInt()));
  }
  compiled = compiled.withMaterialNames(names);
  const PreparedShopConditions prepared =
      PreparedShopConditions::prepare(compiled, shopPacket, resources, context);
  const PetMetadataView catalog(PetDetailCatalog::instance().snapshot());
  const ShopPetMetadataSnapshot metadata{catalog.stargodDefinitions(),catalog.astrolabeDefinitions(),catalog.petDefinitions(),
      catalog.sacredStarPlans(),catalog.sacredStagePlans(),catalog.badgeDefinitions()};
  const ShopPetDerived derived = deriveShopPet(pet, true, metadata);
  return describePreparedShopActionability(compiled.goods().first(),
      prepared.goods().first(), derived, context);
}

ShopPetEligibility analyzeShopPetEligibility(const ShopExchangeGood& good,
                                              const QJsonObject& pet,
                                              bool hasFullDetail) {
  // Compatibility adapter for existing pages. The pure derivation API above
  // receives this frozen metadata explicitly and never accesses a singleton.
  const PetMetadataView catalog(PetDetailCatalog::instance().snapshot());
  const ShopPetMetadataSnapshot metadata{catalog.stargodDefinitions(),catalog.astrolabeDefinitions(),catalog.petDefinitions(),
      catalog.sacredStarPlans(),catalog.sacredStagePlans(),catalog.badgeDefinitions()};
  return describeShopPetRule(compileShopPetRule(good.enhanceType),
                             deriveShopPet(pet, hasFullDetail, metadata));
}
