#pragma once

#include "../domain/catalog_types.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>


class RoutineOverviewCatalog final {
public:
  static RoutineOverviewCatalog& instance();

  std::shared_ptr<const RoutineCatalogSnapshot> snapshot() const { return std::atomic_load(&snapshot_); }
  QList<RoutineTaskDefinition> tasks() const { return snapshot()->tasks; }
  QList<ActivityOverviewDefinition> activities() const { return snapshot()->activities; }
  QVector<int> dayPrizeThresholds() const { return snapshot()->dayPrizeThresholds; }
  QVector<int> weekPrizeThresholds() const { return snapshot()->weekPrizeThresholds; }
  QDateTime sourceUpdatedAt() const { return snapshot()->sourceUpdatedAt; }
  QString sourceLabel() const { return snapshot()->sourceLabel; }

  static std::shared_ptr<const RoutineCatalogSnapshot> prepare(
      const QJsonObject& root, const QString& source, const QDateTime& updatedAt,
      QString* error = nullptr);
  static QJsonObject parseOfficialTexts(QString taskText, const QString& hudText,
                                       const QString& redText, QString* error = nullptr);

private:
  RoutineOverviewCatalog();
  friend class CatalogIoService;
  void publish(std::shared_ptr<const RoutineCatalogSnapshot> value) { std::atomic_store(&snapshot_, std::move(value)); }

  std::shared_ptr<const RoutineCatalogSnapshot> snapshot_ = std::make_shared<RoutineCatalogSnapshot>();
};
