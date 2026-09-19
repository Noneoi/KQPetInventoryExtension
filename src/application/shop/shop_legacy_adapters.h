#pragma once
#include "domain/shop_actionability.h"
#include "domain/shop_pet_eligibility.h"
#include "application/catalog/shop_exchange_catalog.h"

// Application adapters freeze global metadata for the old call signatures.
// Pure calculation headers do not declare or depend on these adapters.
ShopActionability evaluateShopActionability(
    const ShopExchangeGood& good, const QJsonObject& pet,
    const QJsonObject& shopPacket, const AccountResourceView& resources,
    const ShopConditionContext& context = {});
ShopPetEligibility analyzeShopPetEligibility(
    const ShopExchangeGood& good, const QJsonObject& pet, bool hasFullDetail);
