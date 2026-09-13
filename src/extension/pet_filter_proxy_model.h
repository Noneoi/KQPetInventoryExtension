#pragma once

#include <QSortFilterProxyModel>
#include <QString>
#include <QPointer>
#include "pet_search.h"

class PetTableModel;

class PetFilterProxyModel final : public QSortFilterProxyModel {
  Q_OBJECT

public:
  enum class SortMode {
    Default = 0,
    BattlePower = 1,
    ExtremePower = 2,
    CatalogSequence = 3,
    ObtainedAt = 4,
  };

  explicit PetFilterProxyModel(QObject* parent = nullptr);
  void setSourceModel(QAbstractItemModel* model) override;

  void setQuery(const QString& query);
  void setAttributeFilter(const QString& attribute);
  void setJobFilter(const QString& job);
  void setEraFilter(const QString& era);
  void setSortMode(SortMode mode, bool ascending);
  void setFilters(const QString& query, const QString& attribute,
                  const QString& job, const QString& era);

protected:
  bool filterAcceptsRow(int sourceRow,
                        const QModelIndex& sourceParent) const override;
  bool lessThan(const QModelIndex& sourceLeft,
                const QModelIndex& sourceRight) const override;

private:
  QString query_;
  PetSearchQuery preparedQuery_;
  QPointer<PetTableModel> petModel_;
  QString attribute_;
  QString job_;
  QString era_;
  SortMode sortMode_ = SortMode::Default;
  bool ascending_ = true;
};
