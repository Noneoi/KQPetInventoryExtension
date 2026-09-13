#pragma once
#include "../domain/recommendation_engine.h"
#include "../extension/shop_exchange_catalog.h"

// Compatibility entry point for callers which still capture current catalog
// metadata here. The worker uses PreparedRecommendationEngine directly.
class RecommendationEngine final {
public:
  static QList<ActionRecommendation> generate(
      const QString& account, const AccountAssetOverview& overview,
      const QList<ShopExchangeGood>& realGoods,
      const QJsonObject& shopPacket, bool shopPacketKnown,
      const AccountResourceView& resources,
      const ShopConditionContext& context = {});
  static QList<ActionRecommendation> generatePrepared(
      const QString& account, const AccountAssetOverview& overview,
      const PreparedShopConditions& prepared, const ShopPetMetadataSnapshot& metadata,
      AlgorithmPipelineStats* stats = nullptr) {
    return PreparedRecommendationEngine::generatePrepared(account, overview, prepared, metadata, stats);
  }
  static bool less(const ActionRecommendation& left, const ActionRecommendation& right) {
    return PreparedRecommendationEngine::less(left, right);
  }
  static QList<ActionRecommendation> applyFreshness(
      const QList<ActionRecommendation>& recommendations, bool cultivationStale, bool shopStale) {
    return PreparedRecommendationEngine::applyFreshness(recommendations, cultivationStale, shopStale);
  }
};
