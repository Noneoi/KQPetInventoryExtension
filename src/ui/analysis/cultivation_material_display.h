#pragma once

#include "domain/cultivation_material_plan.h"

struct CultivationMaterialCell {
  QString text, html;
};

// Presentation only. The original requirements, eligibility and inventory
// calculations retain every material, including items hidden from this view.
bool showCultivationMaterial(const CultivationMaterialPlanRow& row);
CultivationMaterialCell cultivationMaterialCell(const PetAssetRecord& pet, const QString& category);
