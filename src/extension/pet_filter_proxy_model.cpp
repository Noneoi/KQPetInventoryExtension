#include "pet_filter_proxy_model.h"

#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_search.h"
#include "pet_table_model.h"

#include <QDateTime>
#include <QJsonObject>

#include <climits>

namespace {

QStringList categoryParts(const QString& text) {
  QStringList result;
  for (QString part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    part = part.trimmed();
    if (!part.isEmpty()) result.append(part);
  }
  if (result.isEmpty() && !text.trimmed().isEmpty()) result.append(text.trimmed());
  return result;
}

qint64 numericKey(const QJsonObject& pet, PetFilterProxyModel::SortMode mode,
                  bool* valid) {
  *valid = true;
  switch (mode) {
    case PetFilterProxyModel::SortMode::BattlePower:
      *valid = pet.contains(QStringLiteral("zdl"));
      return pet.value(QStringLiteral("zdl")).toVariant().toLongLong();
    case PetFilterProxyModel::SortMode::ExtremePower:
      *valid = pet.contains(QStringLiteral("xzdl"));
      return pet.value(QStringLiteral("xzdl")).toVariant().toLongLong();
    case PetFilterProxyModel::SortMode::CatalogSequence:
      *valid = petRaceId(pet) > 0;
      return petRaceId(pet);
    case PetFilterProxyModel::SortMode::ObtainedAt: {
      const QJsonValue value = pet.value(QStringLiteral("gd"));
      qint64 timestamp = value.toVariant().toLongLong();
      if (timestamp <= 0 && value.isString())
        timestamp = QDateTime::fromString(value.toString(), Qt::ISODate)
                        .toMSecsSinceEpoch();
      *valid = timestamp > 0;
      return timestamp;
    }
    case PetFilterProxyModel::SortMode::Default:
      return pet.value(QStringLiteral("_position")).toInt(INT_MAX);
  }
  return 0;
}

}  // namespace

PetFilterProxyModel::PetFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
  setDynamicSortFilter(true);
  sort(0, Qt::AscendingOrder);
}

void PetFilterProxyModel::setQuery(const QString& query) {
  if (query_ == query) return;
  query_ = query;
  invalidateFilter();
}

void PetFilterProxyModel::setAttributeFilter(const QString& attribute) {
  if (attribute_ == attribute) return;
  attribute_ = attribute;
  invalidateFilter();
}

void PetFilterProxyModel::setJobFilter(const QString& job) {
  if (job_ == job) return;
  job_ = job;
  invalidateFilter();
}

void PetFilterProxyModel::setEraFilter(const QString& era) {
  if (era_ == era) return;
  era_ = era;
  invalidateFilter();
}

void PetFilterProxyModel::setSortMode(SortMode mode, bool ascending) {
  if (sortMode_ == mode && ascending_ == ascending) return;
  sortMode_ = mode;
  ascending_ = ascending;
  invalidate();
  sort(0, Qt::AscendingOrder);
}

bool PetFilterProxyModel::filterAcceptsRow(int sourceRow,
                                           const QModelIndex& sourceParent) const {
  const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
  const QJsonObject pet = index.data(PetTableModel::PetObjectRole).toJsonObject();
  if (pet.isEmpty()) return false;
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const int raceId = petRaceId(pet);
  QString name = pet.value(QStringLiteral("customName")).toString();
  if (name.isEmpty()) name = pet.value(QStringLiteral("n")).toString();
  const QStringList names = {name, pet.value(QStringLiteral("n")).toString(),
                             catalog.petName(raceId),
                             catalog.resolvedOriginalName(pet)};
  const QStringList identifiers = {QString::number(petInstanceId(pet)),
                                   QString::number(raceId)};
  if (!petQueryMatches(query_, names, identifiers)) return false;
  if (!attribute_.isEmpty() &&
      !categoryParts(catalog.resolvedAttributes(pet)).contains(attribute_))
    return false;
  if (!job_.isEmpty() && !categoryParts(catalog.resolvedJobs(pet)).contains(job_))
    return false;
  return era_.isEmpty() || catalog.resolvedEra(pet) == era_;
}

bool PetFilterProxyModel::lessThan(const QModelIndex& sourceLeft,
                                   const QModelIndex& sourceRight) const {
  const QJsonObject left = sourceLeft.data(PetTableModel::PetObjectRole).toJsonObject();
  const QJsonObject right = sourceRight.data(PetTableModel::PetObjectRole).toJsonObject();
  bool leftValid = false;
  bool rightValid = false;
  const qint64 leftKey = numericKey(left, sortMode_, &leftValid);
  const qint64 rightKey = numericKey(right, sortMode_, &rightValid);
  if (leftValid != rightValid) return leftValid;
  if (leftValid && leftKey != rightKey)
    return ascending_ ? leftKey < rightKey : leftKey > rightKey;
  const int leftPosition = left.value(QStringLiteral("_position")).toInt(INT_MAX);
  const int rightPosition = right.value(QStringLiteral("_position")).toInt(INT_MAX);
  if (leftPosition != rightPosition) return leftPosition < rightPosition;
  return petInstanceId(left) < petInstanceId(right);
}
