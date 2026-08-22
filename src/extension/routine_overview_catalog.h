#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVector>

struct RoutineTaskDefinition {
  int id = 0;
  QString name;
  int dayFinish = 0;
  int dayActive = 0;
  int weekFinish = 0;
  int weekDailyMax = 0;
  int weekActive = 0;
};

struct ActivityOverviewDefinition {
  QString key;
  QString name;
  QDate startDate;
  int redPointId = 0;
  QVector<int> redPointIds;
};

class RoutineOverviewCatalog final {
public:
  static RoutineOverviewCatalog& instance();

  const QList<RoutineTaskDefinition>& tasks() const { return tasks_; }
  const QList<ActivityOverviewDefinition>& activities() const { return activities_; }
  const QVector<int>& dayPrizeThresholds() const { return dayPrizeThresholds_; }
  const QVector<int>& weekPrizeThresholds() const { return weekPrizeThresholds_; }
  QDateTime sourceUpdatedAt() const { return sourceUpdatedAt_; }
  QString sourceLabel() const { return sourceLabel_; }

  bool reloadFromDataRoot(const QString& dataRoot, QString* error = nullptr);
  bool updateFromOfficialData(const QString& dataRoot, QString* error = nullptr);

private:
  RoutineOverviewCatalog();
  bool loadObject(const class QJsonObject& root, const QString& source,
                  const QDateTime& updatedAt, QString* error);

  QList<RoutineTaskDefinition> tasks_;
  QList<ActivityOverviewDefinition> activities_;
  QVector<int> dayPrizeThresholds_;
  QVector<int> weekPrizeThresholds_;
  QDateTime sourceUpdatedAt_;
  QString sourceLabel_;
};
