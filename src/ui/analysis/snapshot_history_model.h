#pragma once

#include "domain/asset_analysis_types.h"

#include <QAbstractTableModel>

class SnapshotHistoryModel final : public QAbstractTableModel {
  Q_OBJECT

public:
  explicit SnapshotHistoryModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;
  void setSnapshots(const QList<AccountAssetSnapshot>& snapshots);

private:
  QList<AccountAssetSnapshot> snapshots_;
};
