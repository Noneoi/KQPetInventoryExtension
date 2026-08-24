#pragma once

#include "asset_analysis_types.h"

#include <QAbstractTableModel>

class AssetAnalysisModel final : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Role { SortRole = Qt::UserRole, InstanceIdRole };
  enum Column { Pet = 0, Location, CurrentPower, MaximumPower, Completion,
                Gaps, Shop, ColumnCount };

  explicit AssetAnalysisModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;

  void setOverview(const AccountAssetOverview& overview);
  void clear();
  const PetAssetRecord* petAt(int row) const;
  bool shopDataKnown() const { return shopDataKnown_; }

private:
  QList<PetAssetRecord> pets_;
  bool shopDataKnown_ = false;
};
