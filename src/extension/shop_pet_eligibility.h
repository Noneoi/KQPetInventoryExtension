#pragma once

#include "shop_exchange_catalog.h"

#include <QJsonObject>
#include <QString>

enum class ShopPetEligibilityState {
  Usable,
  NotUsable,
  Unknown
};

struct ShopPetEligibility {
  ShopPetEligibilityState state = ShopPetEligibilityState::Unknown;
  QString reason;
};

ShopPetEligibility analyzeShopPetEligibility(const ShopExchangeGood& good,
                                              const QJsonObject& pet,
                                              bool hasFullDetail);

