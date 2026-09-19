#pragma once

#include "domain/asset_analysis_types.h"

#include <QSortFilterProxyModel>

class AssetAnalysisFilterProxyModel final : public QSortFilterProxyModel {
  Q_OBJECT

public:
  explicit AssetAnalysisFilterProxyModel(QObject* parent = nullptr);

  void setAssetFilter(PetAssetFilter filter);
  void setQuery(const QString& query);

protected:
  bool filterAcceptsRow(int sourceRow,
                        const QModelIndex& sourceParent) const override;

private:
  PetAssetFilter filter_ = PetAssetFilter::All;
  QString query_;
};
