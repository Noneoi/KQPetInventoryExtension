#pragma once
#include "domain/prepared_shop_conditions.h"

// These pure Domain fixtures explicitly assume the supplied observation was
// captured within this synthetic server period. Production must use the
// ObservationFreshness boundary; there is no default-valid runtime context.
inline ShopConditionContext syntheticQuotaContext(const QList<ShopExchangeGood>& goods,
                                                   ShopConditionContext context = {}) {
  for (const auto& good : goods) {
    if (good.provenUnlimited) continue;
    const QString key = shopQuotaValidityKey(good.shopId, good.limitKey);
    if (!context.quotaValidity.contains(key)) context.quotaValidity.insert(key,
        {ShopConditionState::Satisfied,
         QStringLiteral("synthetic period [2026-09-09T00:00Z,2026-09-10T00:00Z); captured 12:00Z"),
         QStringLiteral("tests/quota_test_support.h explicit synthetic period evidence"),
         QDateTime(QDate(2026, 9, 9), QTime(12, 0), Qt::UTC), ShopConditionFreshness::Current});
  }
  return context;
}

inline PreparedShopConditions prepareWithSyntheticPeriod(const CompiledShopCatalog& catalog,
    const QJsonObject& packet, const AccountResourceView& resources,
    ShopConditionContext context = {}, AlgorithmPipelineStats* stats = nullptr) {
  QList<ShopExchangeGood> goods;
  for (const auto& good : catalog.goods()) goods.append(good.good);
  return PreparedShopConditions::prepare(catalog, packet, resources, syntheticQuotaContext(goods, context), stats);
}
