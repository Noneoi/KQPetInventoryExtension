#pragma once

#include "shop_actionability.h"

#include <QList>
#include <QStringList>
#include <memory>

enum class RecommendationType {
  ReadyNow,
  ResourceMissing,
  ConditionUnknown,
  ResourceUnknown = ConditionUnknown,  // Source compatibility for old pages.
  NearFullCultivation,
  LocalCultivation
};

struct ActionRecommendation {
  std::shared_ptr<void> memoryRetention;
  QString stableId;
  RecommendationType type = RecommendationType::NearFullCultivation;
  qint64 petInstanceId = 0;
  QString petName;
  QString title;
  QStringList gaps;
  QString shopGoodKey;
  QString shopName;
  QString goodName;
  QList<ResourceRequirement> requirements;
  int remainingExchangeCount = 0;
  int completionPercent = 0;
  int currentPower = 0;
  int highestPower = 0;
  int supportedGapCount = 0;
  int alternateGoodCount = 0;
  ShopCondition eligibilityCondition;
  ShopCondition costCondition;
  ShopCondition resourceCondition;
  ShopCondition limitCondition;
  ShopCondition unlockCondition;
  int unknownConditionCount = 0;
  bool resourceCoverageKnown = false;
  int resourceCoverageMillionths = 0;
  bool closesKnownGap = false;
  bool completionKnown = false;
  bool powerGapKnown = false;
  bool actionableCountKnown = false;
  int actionableCount = 0;
  int remainingGapAfterAction = 0;
  bool stale = false;
  bool cultivationStale = false;
  bool shopStale = false;
};
