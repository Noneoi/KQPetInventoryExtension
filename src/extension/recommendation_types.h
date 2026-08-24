#pragma once

#include "shop_actionability.h"

#include <QList>
#include <QStringList>

enum class RecommendationType {
  ReadyNow,
  ResourceMissing,
  ResourceUnknown,
  NearFullCultivation
};

struct ActionRecommendation {
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
  qint64 totalMissingResources = 0;
  bool actionableCountKnown = false;
  int actionableCount = 0;
  int remainingGapAfterAction = 0;
  bool stale = false;
  bool cultivationStale = false;
  bool shopStale = false;
};
