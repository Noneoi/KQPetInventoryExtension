#include "pet_filter_proxy_model.h"

#include "pet_search.h"
#include "pet_table_model.h"

PetFilterProxyModel::PetFilterProxyModel(QObject* parent) : QSortFilterProxyModel(parent) {
  setDynamicSortFilter(true);
  setFilterRole(PetTableModel::FilterCacheRevisionRole);
  setSortRole(PetTableModel::SortCacheRevisionRole);
  sort(0, Qt::AscendingOrder);
}

void PetFilterProxyModel::setSourceModel(QAbstractItemModel* model) {
  petModel_ = qobject_cast<PetTableModel*>(model);
  QSortFilterProxyModel::setSourceModel(model);
}

void PetFilterProxyModel::setFilters(const QString& query, const QString& attribute,
                                     const QString& job, const QString& era) {
  if (query_ == query && attribute_ == attribute && job_ == job && era_ == era) return;
  if (query_ != query) preparedQuery_ = preparePetSearchQuery(query);
  query_ = query;
  attribute_ = attribute;
  job_ = job;
  era_ = era;
  invalidateFilter();
}
void PetFilterProxyModel::setQuery(const QString& query) { setFilters(query, attribute_, job_, era_); }
void PetFilterProxyModel::setAttributeFilter(const QString& value) { setFilters(query_, value, job_, era_); }
void PetFilterProxyModel::setJobFilter(const QString& value) { setFilters(query_, attribute_, value, era_); }
void PetFilterProxyModel::setEraFilter(const QString& value) { setFilters(query_, attribute_, job_, value); }

void PetFilterProxyModel::setSortMode(SortMode mode, bool ascending) {
  if (sortMode_ == mode && ascending_ == ascending) return;
  sortMode_ = mode;
  ascending_ = ascending;
  invalidate();
  sort(0, Qt::AscendingOrder);
}

bool PetFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (!petModel_ || sourceParent.isValid()) return false;
  const auto* row = petModel_->cachedRow(sourceRow);
  if (!row || !petQueryMatches(preparedQuery_, row->search)) return false;
  return (attribute_.isEmpty() || row->attributes.contains(attribute_)) &&
         (job_.isEmpty() || row->jobs.contains(job_)) &&
         (era_.isEmpty() || row->era == era_);
}

bool PetFilterProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  if (!petModel_) return QSortFilterProxyModel::lessThan(left, right);
  const auto* a = petModel_->cachedRow(left.row());
  const auto* b = petModel_->cachedRow(right.row());
  if (!a || !b) return left.row() < right.row();
  const int mode = qBound(0, static_cast<int>(sortMode_), 4);
  const auto& leftKey = a->sortKeys[mode];
  const auto& rightKey = b->sortKeys[mode];
  if (leftKey.known != rightKey.known) return leftKey.known;
  if (leftKey.known && leftKey.value != rightKey.value)
    return ascending_ ? leftKey.value < rightKey.value : leftKey.value > rightKey.value;
  if (a->sortKeys[0].value != b->sortKeys[0].value)
    return a->sortKeys[0].value < b->sortKeys[0].value;
  return a->instanceId < b->instanceId;
}
