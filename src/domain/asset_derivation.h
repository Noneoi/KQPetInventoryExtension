#pragma once
#include "asset_analysis_types.h"
struct PetBattlePowerState;
struct ShopPetDerived;

// Value-only cultivation, identity and aggregation rules. All facts/metadata
// are frozen by Application; this class cannot inspect a live repository.
class AssetDerivation final {
public:
  static bool matchesFilter(const PetAssetRecord& pet, PetAssetFilter filter);
  static PetAssetRecord derivePet(const PetAssetRecord& seed, const QJsonObject& stargods,
                                  const QJsonObject& astrolabe = {}, const QJsonObject& pets = {});
  static PetAssetRecord derivePetWithPower(const PetAssetRecord& seed, const PetBattlePowerState& power);
  static void applySacredCultivation(PetAssetRecord* record, const ShopPetDerived& cultivation);
  static void applyCultivation(PetAssetRecord* record, const ShopPetDerived& cultivation);
  static void accumulate(AccountAssetOverview* overview, const PetAssetRecord& pet);
  static QJsonObject identityFields(const QJsonObject& pet);
  static QJsonObject analysisInputFields(const QJsonObject& brief, const QJsonObject& detail);
};
