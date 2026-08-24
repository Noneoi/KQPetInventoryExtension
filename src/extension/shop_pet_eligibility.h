#pragma once

#include "shop_exchange_catalog.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

enum class ShopPetEligibilityState {
  Usable,
  NotUsable,
  Unknown
};

struct ShopPetEligibility {
  ShopPetEligibilityState state = ShopPetEligibilityState::Unknown;
  QString reason;
  QStringList usefulCodes;
};

ShopPetEligibility analyzeShopPetEligibility(const ShopExchangeGood& good,
                                              const QJsonObject& pet,
                                              bool hasFullDetail);
