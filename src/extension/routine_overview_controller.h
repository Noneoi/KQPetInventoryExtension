#pragma once

#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>

class PetRepository;
class QTimer;

class RoutineOverviewController final : public QObject {
  Q_OBJECT

public:
  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;

  explicit RoutineOverviewController(PetRepository* repository, QObject* parent = nullptr);
  void setSender(Sender sender);
  bool requestRefresh();
  QJsonObject dailyPacket() const { return dailyPacket_; }
  QSet<int> activeRedPoints() const { return activeRedPoints_; }
  QJsonObject opportunityPackets() const { return opportunityPackets_; }
  bool hasDailyPacket() const { return hasDailyPacket_; }
  bool hasRedPointPacket() const { return hasRedPointPacket_; }

public slots:
  void handlePacket(const QString& method, const QString& payload);

signals:
  void statusChanged(const QString& status);
  void runningChanged(bool running);
  void dataUpdated();
  void catalogUpdated();

private:
  void changeSession(const QString& account, quint64 generation);
  void completeRequest(const QString& command, bool updated,
                       const QString& warning = {});
  void finish(bool publish, const QString& status);
  void loadCache();
  void saveCache() const;

  PetRepository* repository_ = nullptr;
  Sender sender_;
  QTimer* timeout_ = nullptr;
  QJsonObject dailyPacket_;
  QSet<int> activeRedPoints_;
  QJsonObject opportunityPackets_;
  bool hasDailyPacket_ = false;
  bool hasRedPointPacket_ = false;
  bool running_ = false;
  bool anyUpdated_ = false;
  QSet<QString> pendingCommands_;
  QHash<QString, QString> requestLabels_;
  QStringList requestWarnings_;
  QString account_;
  QString requestAccount_;
  quint64 sessionGeneration_ = 0;
  quint64 requestSessionGeneration_ = 0;
};
