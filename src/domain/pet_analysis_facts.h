#pragma once

#include "asset_analysis_types.h"
#include "pet_power_types.h"
#include "shop_pet_eligibility.h"
#include <optional>

// Only scalar identity JSON and bounded derived components are retained.
// Account/epoch/revision/metadata-digest admission is owned by Application.
struct PetAnalysisFacts {
  PetAssetRecord asset;
  ShopPetDerived eligibility;
  PetBattlePowerState battlePower;
  int analysisVersion = AssetAnalysisVersion::kCurrentAnalysis;
};

// A missing detail is valid Unknown input when its summary has a positive ID.
// Contradictory IDs/races are rejected rather than repaired or associated with
// another instance. The factory never retains raw JSON or metadata in output.
std::optional<PetAnalysisFacts> derivePetAnalysisFacts(
    const PetAssetRecord& seed, const ShopPetMetadataSnapshot& metadata,
    AlgorithmPipelineStats* stats = nullptr, QString* error = nullptr);

bool validatePetAnalysisFacts(const PetAnalysisFacts& facts, QString* error = nullptr);
// Conservative retained allocation charge including QList capacity and scalar
// JSON/string storage, shared by the Application LRU and Worker input meter.
quint64 petAnalysisFactsRetainedBytes(const PetAnalysisFacts& facts);

QJsonObject petAnalysisFactsToJson(const PetAnalysisFacts& facts);
std::optional<PetAnalysisFacts> petAnalysisFactsFromJson(const QJsonObject& object, QString* error = nullptr);

Q_DECLARE_METATYPE(PetAnalysisFacts)
