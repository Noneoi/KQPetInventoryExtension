#include "asset_analysis_filter_proxy_model.h"

#include "asset_analysis_model.h"
#include "asset_analyzer.h"

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
  if (!pet || !AssetAnalyzer::matchesFilter(*pet, filter_)) return false;
  return query_.isEmpty() || pet->name.contains(query_, Qt::CaseInsensitive) ||
         QString::number(pet->instanceId).contains(query_);
}
