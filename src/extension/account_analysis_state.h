#pragma once

#include "asset_analysis_types.h"

#include <QSet>

struct AccountAnalysisState {
  AccountAssetOverview lastValidOverview;
  InventorySignature analyzedSignature;
  QDateTime lastAnalysisAt;
  QSet<qint64> dirtyPetIds;
  bool hasAnalysis = false;
  bool inventoryStale = false;
  bool shopStale = false;
};
