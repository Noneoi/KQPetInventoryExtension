#pragma once

#include "account_resource_view.h"
#include "shop_exchange_catalog.h"

#include <QJsonObject>
#include <QList>
#include <QStringList>

struct ResourceRequirement {
  QString resourceKey;
  QString resourceName;
  qint64 required = 0;
  qint64 owned = 0;
  bool ownedKnown = false;

  qint64 missing() const {
    return ownedKnown ? qMax<qint64>(0, required - owned) : 0;
  }
};

struct ShopActionability {
  bool petEligible = false;
  int remainingCount = 0;
  bool resourcesKnown = false;
  bool resourcesEnough = false;
  QList<ResourceRequirement> requirements;
  QString eligibilityReason;
  QStringList applicableCodes;
};

ShopActionability evaluateShopActionability(
    const ShopExchangeGood& good, const QJsonObject& pet,
    const QJsonObject& shopPacket, const AccountResourceView& resources);
