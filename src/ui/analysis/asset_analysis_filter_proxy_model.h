#pragma once

#include "domain/asset_analysis_types.h"
#include "domain/cultivation_material_inventory.h"
#include "domain/cultivation_material_plan.h"

#include <QSortFilterProxyModel>

class AssetAnalysisFilterProxyModel final : public QSortFilterProxyModel {
  Q_OBJECT

public:
  explicit AssetAnalysisFilterProxyModel(QObject* parent = nullptr);

  void setAssetFilter(PetAssetFilter filter);
  void setQuery(const QString& query);
  void setCultivationFilters(const QString& category, const QString& era, const QString& rating);
  QString materialSummary() const;
  QString materialPlan(const MaterialInventorySnapshot& inventory, const QSet<qint64>& selected = {}, bool onlySelected = false) const;
  CultivationMaterialPlan materialPlanDetails(const MaterialInventorySnapshot& inventory,
      const QSet<qint64>& selected = {}, bool onlySelected = false) const;

protected:
  bool filterAcceptsRow(int sourceRow,
                        const QModelIndex& sourceParent) const override;

private:
  PetAssetFilter filter_ = PetAssetFilter::All;
  QString query_;
  QString category_, era_, rating_;
};
