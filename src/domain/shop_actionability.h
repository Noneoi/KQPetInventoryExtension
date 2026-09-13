#pragma once

#include "account_resource_view.h"
#include "catalog_types.h"

#include <QJsonObject>
#include <QList>
#include <QStringList>

enum class ShopConditionState { Satisfied, Blocked, Unknown };
enum class ShopConditionFreshness { Unknown, Current, Invalidated };

inline QString shopQuotaValidityKey(int shopId, const QString& periodKey) {
  return QStringLiteral("si%1:%2").arg(shopId).arg(periodKey);
}
inline QString shopQuotaValidityKey(const ShopExchangeGood& good) {
  const auto key = shopQuotaValidityKey(good.shopId,good.limitKey);
  return good.sourceKey.isEmpty() ? key : good.sourceKey + QLatin1Char(':') + key;
}

struct ShopCondition {
  ShopConditionState state = ShopConditionState::Unknown;
  QString reason;
  QString source;
  QDateTime observedAt;
  ShopConditionFreshness freshness = ShopConditionFreshness::Unknown;

  ShopConditionState effectiveState() const {
    return freshness == ShopConditionFreshness::Invalidated
               ? ShopConditionState::Unknown : state;
  }
};

// Callers may supply only already verified expression facts, with their source.
// This API neither interprets catalog strings nor requests new server data.
struct ShopConditionContext {
  bool shopPacketKnown = true;
  QString shopSource;
  QDateTime shopObservedAt;
  ShopConditionFreshness shopFreshness = ShopConditionFreshness::Unknown;
  // Evaluated at capture by the Application freshness boundary. A numeric
  // observation alone does not establish which server quota period it belongs
  // to. Empty entries deliberately do not default to Current.
  QHash<QString, ShopCondition> quotaValidity;
  QString petSource;
  QDateTime petObservedAt;
  ShopConditionFreshness petFreshness = ShopConditionFreshness::Unknown;
  QHash<QString, ShopCondition> verifiedUnlockFacts;
};

struct ResourceRequirement {
  QString resourceKey;
  QString resourceName;
  qint64 required = 0;
  qint64 owned = 0;
  bool ownedKnown = false;
  ShopCondition condition;

  qint64 missing() const {
    return ownedKnown && owned >= 0 && required > owned ? required - owned : 0;
  }
};

struct ShopActionability {
  // Legacy fields remain derived views of the conditions for existing pages.
  bool petEligible = false;
  int remainingCount = -1;
  bool resourcesKnown = false;
  bool resourcesEnough = false;
  QList<ResourceRequirement> requirements;
  QString eligibilityReason;
  QStringList applicableCodes;
  ShopCondition eligibilityCondition;
  ShopCondition costCondition;
  ShopCondition resourceCondition;
  ShopCondition limitCondition;
  ShopCondition unlockCondition;

  bool excluded() const;
  int unknownConditionCount() const;
};
