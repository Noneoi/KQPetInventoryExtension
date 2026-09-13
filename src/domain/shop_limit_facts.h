#pragma once
#include "catalog_types.h"

// Existing strict quota interpretation, independent of a live catalog holder.
QJsonObject shopItemObject(const QJsonObject& packet, const ShopExchangeGood& good);
int shopUsedCount(const QJsonObject& packet, const ShopExchangeGood& good);
int shopRemainingCount(const QJsonObject& packet, const ShopExchangeGood& good);
