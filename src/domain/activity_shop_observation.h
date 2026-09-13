#pragma once
#include "catalog_types.h"

struct ActivityShopCost {
  QString key, name;
  qint64 required = 0, owned = 0;
  bool ownedKnown = false, current = false;
};
struct ActivityShopObservation {
  int used = -1, remaining = -1;
  bool quotaKnown = false, historical = false;
  QString standardCost;
  bool priceKnown = false, priceCurrent = false;
  bool applicabilityKnown = true, applicable = true;
  QList<ActivityShopCost> costs;
  QDateTime observedAt;
};

QString activityRequestSignature(const QJsonObject& request);
QJsonValue activityObservedValue(const ShopExchangeGood& good, const QJsonObject& descriptor,
    const QJsonObject& packet, bool* historical = nullptr, bool* verified = nullptr,
    QDateTime* observedAt = nullptr);
ActivityShopObservation observeActivityShopGood(const ShopExchangeGood& good, const QJsonObject& packet);
