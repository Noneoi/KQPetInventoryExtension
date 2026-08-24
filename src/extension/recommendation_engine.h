#pragma once

#include "account_resource_view.h"
#include "asset_analysis_types.h"
#include "recommendation_types.h"
#include "shop_exchange_catalog.h"

class RecommendationEngine final {
public:
  static QList<ActionRecommendation> generate(
      const QString& account, const AccountAssetOverview& overview,
      const QList<ShopExchangeGood>& realGoods,
      const QJsonObject& shopPacket, bool shopPacketKnown,
      const AccountResourceView& resources);
  static QList<ActionRecommendation> applyFreshness(
      const QList<ActionRecommendation>& recommendations,
      bool cultivationStale, bool shopStale);
};
