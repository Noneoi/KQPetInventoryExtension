#include "recommendation_adapter.h"
#include "../extension/pet_detail_catalog.h"

QList<ActionRecommendation> RecommendationEngine::generate(
    const QString& account, const AccountAssetOverview& overview,
    const QList<ShopExchangeGood>& realGoods,
    const QJsonObject& shopPacket, bool shopPacketKnown,
    const AccountResourceView& resources, const ShopConditionContext& context) {
  // The old entry point freezes runtime lookup data before entering the pure
  // snapshot pipeline. Applications may cache the compiled/prepared values.
  CompiledShopCatalog compiled = CompiledShopCatalog::compile(realGoods);
  QHash<QString, QString> names;
  for (const CompiledShopGood& good : compiled.goods()) {
    for (const ResourceRequirement& requirement : good.requirements) {
      if (names.contains(requirement.resourceKey)) continue;
      const QStringList key = requirement.resourceKey.split(QLatin1Char(':'));
      names.insert(requirement.resourceKey, PetDetailCatalog::instance().materialName(
          key.value(0).toInt(), key.value(1).toInt()));
    }
  }
  compiled = compiled.withMaterialNames(names);
  ShopConditionContext effectiveContext = context;
  effectiveContext.shopPacketKnown = shopPacketKnown && context.shopPacketKnown;
  const PreparedShopConditions prepared =
      PreparedShopConditions::prepare(compiled, shopPacket, resources, effectiveContext);
  const PetMetadataView catalog(PetDetailCatalog::instance().snapshot());
  const ShopPetMetadataSnapshot metadata{catalog.stargodDefinitions(),catalog.astrolabeDefinitions(),catalog.petDefinitions(),
      catalog.sacredStarPlans(),catalog.sacredStagePlans(),catalog.badgeDefinitions()};
  return generatePrepared(account, overview, prepared, metadata);
}
