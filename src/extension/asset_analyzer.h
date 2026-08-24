#pragma once

#include "asset_analysis_types.h"

class PetRepository;
class ShopExchangeController;
class RoutineOverviewController;

class AssetAnalyzer final {
public:
  AssetAnalyzer(PetRepository* repository,
                ShopExchangeController* shopController,
                RoutineOverviewController* routineController);

  AccountInventorySummary inventorySummary() const;
  InventorySignature inventorySignature() const;
  AccountAssetOverview analyze() const;
  quint64 analysisRunCount() const { return analysisRunCount_; }
  AccountAssetOverview routineSummary() const;
  void updateRoutineSummary(AccountAssetOverview* overview) const;

  static bool matchesFilter(const PetAssetRecord& pet, PetAssetFilter filter);

private:
  PetRepository* repository_ = nullptr;
  ShopExchangeController* shopController_ = nullptr;
  RoutineOverviewController* routineController_ = nullptr;
  mutable quint64 analysisRunCount_ = 0;
};
