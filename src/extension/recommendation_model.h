#pragma once

#include "recommendation_types.h"

#include <QAbstractTableModel>

class RecommendationModel final : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column {
    Status = 0,
    Pet,
    Gap,
    Project,
    Resources,
    Remaining,
    Conclusion,
    ViewPet,
    ViewShop,
    ColumnCount
  };
  enum Role {
    PetInstanceIdRole = Qt::UserRole + 1,
    ShopGoodKeyRole,
    StableIdRole
  };

  explicit RecommendationModel(QObject* parent = nullptr);
  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  void setRecommendations(const QList<ActionRecommendation>& recommendations);

private:
  QList<ActionRecommendation> recommendations_;
};
