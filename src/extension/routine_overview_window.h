#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QSet>
#include "../domain/catalog_types.h"
#include "../contracts/observation_types.h"
#include <memory>

class QLabel;
class QPushButton;
class QTableWidget;

class RoutineOverviewWindow final : public QDialog {
  Q_OBJECT

public:
  explicit RoutineOverviewWindow(QWidget* parent = nullptr);
  void setData(const QJsonObject& dailyPacket, bool hasDaily,
               const QSet<int>& activeRedPoints, bool hasRedPoints,
               const QJsonObject& opportunityPackets = {});
  void setCatalogSnapshot(std::shared_ptr<const RoutineCatalogSnapshot> catalog);
  void setPeriodValidity(const QHash<QString, ObservationValidity>& validity);
  void setReadOnlyObservations(const QJsonObject& packets);

public slots:
  void resetSessionContext();
  void setStatus(const QString& status);
  void setRunning(bool running);
  void rebuild();

signals:
  void refreshRequested();

private:
  void rebuildTasks();
  void rebuildActivities();
  void rebuildOpportunities();
  QString prizeSummary(bool daily) const;
  ObservationValidity validity(const QString& key) const;
  void updateObservedData();

  QPushButton* refresh_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* dailySummary_ = nullptr;
  QLabel* weeklySummary_ = nullptr;
  QLabel* activityNote_ = nullptr;
  QLabel* opportunityNote_ = nullptr;
  QTableWidget* dailyTable_ = nullptr;
  QTableWidget* weeklyTable_ = nullptr;
  QTableWidget* activityTable_ = nullptr;
  QTableWidget* opportunityTable_ = nullptr;
  QJsonObject dailyPacket_;
  QJsonObject sourceDailyPacket_;
  QSet<int> sourceRedPoints_;
  QJsonObject sourceOpportunityPackets_;
  bool hasSourceDaily_ = false;
  bool hasSourceRedPoints_ = false;
  QJsonObject readOnlyPackets_;
  QSet<int> activeRedPoints_;
  QJsonObject opportunityPackets_;
  bool hasDaily_ = false;
  bool hasRedPoints_ = false;
  std::shared_ptr<const RoutineCatalogSnapshot> catalog_;
  QHash<QString, ObservationValidity> periodValidity_;
};
