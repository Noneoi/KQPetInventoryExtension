#include "asset_analysis_filter_proxy_model.h"

#include "asset_analysis_model.h"
#include "domain/asset_derivation.h"
#include "domain/cultivation_material_plan.h"

AssetAnalysisFilterProxyModel::AssetAnalysisFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
  setSortRole(AssetAnalysisModel::SortRole);
  setDynamicSortFilter(true);
}

void AssetAnalysisFilterProxyModel::setAssetFilter(PetAssetFilter filter) {
  if (filter_ == filter) return;
  filter_ = filter;
  invalidateRowsFilter();
}

void AssetAnalysisFilterProxyModel::setQuery(const QString& query) {
  const QString normalized = query.trimmed();
  if (query_ == normalized) return;
  query_ = normalized;
  invalidateRowsFilter();
}

bool AssetAnalysisFilterProxyModel::filterAcceptsRow(
    int sourceRow, const QModelIndex&) const {
  const auto* model = qobject_cast<const AssetAnalysisModel*>(sourceModel());
  const PetAssetRecord* pet = model ? model->petAt(sourceRow) : nullptr;
  if (!pet || !AssetDerivation::matchesFilter(*pet, filter_)) return false;
  if (!AssetDerivation::matchesCultivationCategory(*pet, category_) ||
      (!era_.isEmpty() && pet->pet.value(QStringLiteral("_metaEra")).toString() != era_) ||
      (!rating_.isEmpty() && pet->pet.value(QStringLiteral("_metaRating")).toString() != rating_)) return false;
  return query_.isEmpty() || pet->name.contains(query_, Qt::CaseInsensitive) ||
         QString::number(pet->instanceId).contains(query_);
}

void AssetAnalysisFilterProxyModel::setCultivationFilters(const QString& category, const QString& era, const QString& rating) {
  if (category_ == category && era_ == era && rating_ == rating) return;
  category_ = category; era_ = era; rating_ = rating; invalidateRowsFilter();
}

QString AssetAnalysisFilterProxyModel::materialSummary() const {
  QList<const PetAssetRecord*> selected;
  const auto* model = qobject_cast<const AssetAnalysisModel*>(sourceModel());
  if (model) for (int row = 0; row < rowCount(); ++row) selected.append(model->petAt(mapToSource(index(row,0)).row()));
  return AssetDerivation::cultivationMaterialSummary(selected,category_,true);
}

QString AssetAnalysisFilterProxyModel::materialPlan(const MaterialInventorySnapshot& inventory, const QSet<qint64>& ids, bool onlySelected) const {
  const auto plan = materialPlanDetails(inventory,ids,onlySelected);
  if (onlySelected && plan.petCount == 0) return QStringLiteral("请在下方列表选择要优先培养的精灵（可按 Ctrl 或 Shift 多选）");
  return cultivationMaterialPlanSummary(plan);
}

CultivationMaterialPlan AssetAnalysisFilterProxyModel::materialPlanDetails(const MaterialInventorySnapshot& inventory, const QSet<qint64>& ids, bool onlySelected) const {
  QList<const PetAssetRecord*> selected;
  const auto* model = qobject_cast<const AssetAnalysisModel*>(sourceModel());
  if (model) for (int row = 0; row < rowCount(); ++row) {
    const auto* pet = model->petAt(mapToSource(index(row,0)).row());
    if (pet && (!onlySelected || ids.contains(pet->instanceId))) selected.append(pet);
  }
  return buildCultivationMaterialPlan(selected,category_,inventory);
}
