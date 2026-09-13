#include "shop_limit_facts.h"
#include "activity_shop_observation.h"
#include <cmath>
#include <limits>

namespace {
bool checkedCount(const QJsonValue& value, int* result) {
  if (value.isDouble()) {
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 ||
        number > std::numeric_limits<int>::max() || std::floor(number) != number)
      return false;
    *result = static_cast<int>(number);
    return true;
  }
  if (!value.isString() || value.toString().isEmpty()) return false;
  int number = 0;
  for (const QChar character : value.toString()) {
    if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
    const int digit = character.unicode() - '0';
    if (number > (std::numeric_limits<int>::max() - digit) / 10) return false;
    number = number * 10 + digit;
  }
  *result = number;
  return true;
}

}

QJsonObject shopItemObject(const QJsonObject& packet,
                                            const ShopExchangeGood& good) {
  if (!good.sourceKey.isEmpty()) return {};
  const QString shopKey = QStringLiteral("si%1").arg(good.shopId);
  QJsonObject shop;
  bool found = false;
  QList<QJsonObject> roots{packet};
  for (const QString& wrapper : {QStringLiteral("data"), QStringLiteral("p"),
                                 QStringLiteral("params")}) {
    if (packet.value(wrapper).isObject()) roots.append(packet.value(wrapper).toObject());
  }
  for (const QJsonObject& root : roots) {
    if (!root.contains(shopKey)) continue;
    if (!root.value(shopKey).isObject()) return {};
    const QJsonObject candidate = root.value(shopKey).toObject();
    if (found && shop != candidate) return {};
    shop = candidate;
    found = true;
  }
  const QJsonValue modern = shop.value(good.itemKey());
  const QJsonValue legacy = shop.value(QStringLiteral("b%1").arg(good.itemServerId));
  if (!modern.isUndefined() && !modern.isObject()) return {};
  if (!legacy.isUndefined() && !legacy.isObject()) return {};
  if (modern.isObject() && legacy.isObject() && modern != legacy) return {};
  return modern.isObject() ? modern.toObject() : legacy.toObject();
}

int shopUsedCount(const QJsonObject& packet, const ShopExchangeGood& good) {
  if (!good.sourceKey.isEmpty()) return observeActivityShopGood(good,packet).used;
  if (good.provenUnlimited) return 0;
  static const QStringList knownLimitKeys{QStringLiteral("dl"), QStringLiteral("wl"),
      QStringLiteral("ml"), QStringLiteral("pl"), QStringLiteral("tl")};
  if (!knownLimitKeys.contains(good.limitKey) || good.limitCount <= 0) return -1;
  const QJsonValue value = shopItemObject(packet, good).value(good.limitKey);
  int count = 0;
  return checkedCount(value, &count) ? count : -1;
}

int shopRemainingCount(const QJsonObject& packet,
                                        const ShopExchangeGood& good) {
  if (!good.sourceKey.isEmpty()) return observeActivityShopGood(good,packet).remaining;
  if (good.provenUnlimited) return std::numeric_limits<int>::max();
  const int used = shopUsedCount(packet, good);
  return used < 0 ? -1 : qMax(0, good.limitCount - used);
}
