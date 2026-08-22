#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QSet>

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

public slots:
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
  QSet<int> activeRedPoints_;
  QJsonObject opportunityPackets_;
  bool hasDaily_ = false;
  bool hasRedPoints_ = false;
};
