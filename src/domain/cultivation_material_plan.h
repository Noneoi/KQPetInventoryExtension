#pragma once
#include "asset_analysis_types.h"
#include "cultivation_material_inventory.h"

struct CultivationMaterialContributor {
  qint64 instanceId = 0;
  QString name;
  qint64 count = 0;
  bool distinguishInstance = false;
};

struct CultivationMaterialPlanRow {
  int type = 0, id = 0, extra = 0;
  QString category, name, scope;
  qint64 required = 0;
  bool complete = true;
  std::optional<qint64> owned, missing;
  QList<CultivationMaterialContributor> pets;
};

struct CultivationMaterialPlan {
  QString category;
  QList<CultivationMaterialPlanRow> materials;
  qsizetype petCount = 0;
  qsizetype unknownPetCount = 0;
  QDateTime observedAt;
};

QString cultivationMaterialCategoryName(const QString& category);
CultivationMaterialPlan buildCultivationMaterialPlan(const QList<const PetAssetRecord*>& pets,
    const QString& category, const MaterialInventorySnapshot& inventory);
QString cultivationMaterialPlanSummary(const CultivationMaterialPlan& plan);
QString cultivationMaterialPlanSummary(const QList<const PetAssetRecord*>& pets,
    const QString& category, const MaterialInventorySnapshot& inventory);
