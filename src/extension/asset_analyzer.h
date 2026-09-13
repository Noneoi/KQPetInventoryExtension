#pragma once

#include "asset_analysis_types.h"
#include "../domain/asset_derivation.h"

class PetRepository;
class ShopExchangeController;
class RoutineOverviewController;
class PetMetadataView;
struct PetDetailCatalogSnapshot;

class AssetAnalyzer final {
public:
  AssetAnalyzer(PetRepository* repository,
                ShopExchangeController* shopController,
                RoutineOverviewController* routineController);

  AccountInventorySummary inventorySummary() const;
  InventorySignature inventorySignature() const;
  AccountAssetOverview analyze() const;
  // Core capture only: identity/location, required cultivation fields and
  // frozen metadata. No power/cultivation/product-pair analysis occurs here.
  AccountAssetOverview captureInput(std::shared_ptr<const PetDetailCatalogSnapshot> metadata = {}) const;
  quint64 analysisRunCount() const { return analysisRunCount_; }
  AccountAssetOverview routineSummary() const;
  void updateRoutineSummary(AccountAssetOverview* overview) const;
  static PetAssetRecord summarySeed(const QJsonObject& brief, bool complete,
                                    bool sourceKnown, const PetMetadataView& metadata);

  static bool matchesFilter(const PetAssetRecord& pet, PetAssetFilter filter) { return AssetDerivation::matchesFilter(pet, filter); }
  static PetAssetRecord derivePet(const PetAssetRecord& seed, const QJsonObject& stargods) { return AssetDerivation::derivePet(seed, stargods); }
  static void accumulate(AccountAssetOverview* overview, const PetAssetRecord& pet) { AssetDerivation::accumulate(overview, pet); }
  static QJsonObject identityFields(const QJsonObject& pet) { return AssetDerivation::identityFields(pet); }

private:
  PetRepository* repository_ = nullptr;
  ShopExchangeController* shopController_ = nullptr;
  RoutineOverviewController* routineController_ = nullptr;
  mutable quint64 analysisRunCount_ = 0;
};
