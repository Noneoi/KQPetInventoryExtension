#pragma once

#include "domain/asset_analysis_types.h"
#include "domain/recommendation_types.h"

#include <QSet>

struct AnalysisWorkResult;

struct AccountAnalysisState {
  AccountAssetOverview lastValidOverview;
  QList<ActionRecommendation> lastValidRecommendations;
  InventorySignature analyzedSignature;
  QDateTime lastAnalysisAt;
  QSet<qint64> dirtyPetIds;
  bool hasAnalysis = false;
  bool inventoryStale = false;
  bool shopStale = false;
  std::shared_ptr<const AnalysisWorkResult> retainedResult;
};
