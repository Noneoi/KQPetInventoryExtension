#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

class PetRepository;

class PetRefreshController final : public QObject {
  Q_OBJECT

public:
  struct Timings {
    int automaticIntervalMs = 60000;
    int listRequestGapMs = 1000;
    int listTimeoutMs = 10000;
    int detailRequestGapMs = 1000;
    int detailBatchRestMs = 2000;
    int detailTimeoutMs = 8000;
    int detailBatchSize = 12;
    int detailMaxRetries = 1;
  };

  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;

  explicit PetRefreshController(PetRepository* repository, QObject* parent = nullptr);
  void setSender(Sender sender);
  void setTimings(const Timings& timings);
  Timings timings() const { return timings_; }

  bool listRefreshRunning() const { return listRunning_; }
  bool detailBatchRunning() const { return batchRunning_; }
  bool detailBatchPaused() const { return batchPaused_; }
  void publishState();

public slots:
  void requestManualListRefresh();
  void startWarehouseDetailRefresh();
  void pauseWarehouseDetailRefresh();
  void resumeWarehouseDetailRefresh();
  void cancelWarehouseDetailRefresh();
  void requestSingleDetail(qint64 instanceId);

signals:
  void statusChanged(const QString& status);
  void listRefreshRunningChanged(bool running);
  void detailProgressChanged(bool running, bool paused, int completed, int total,
                             int succeeded, int failed, qint64 currentInstanceId,
                             int estimatedSeconds);
  void commandSent(const QString& command, qint64 instanceId,
                   quint64 requestGeneration);

private slots:
  void onAccountSessionChanged(const QString& account, quint64 sessionGeneration);
  void onListResponseAccepted(const QString& command, quint64 requestGeneration);
  void onDetailResponseAccepted(qint64 instanceId, quint64 requestGeneration);
  void onDetailResponseRejected(qint64 instanceId, quint64 requestGeneration,
                                const QString& reason);

private:
  enum class PendingList { None, Automatic, Manual };

  void startListRefresh(bool manual);
  void sendWarehouseListRequest();
  void finishListPart(const QString& command, bool succeeded, const QString& reason = {});
  void maybeFinishListRefresh();
  void scheduleAutomaticRefresh();
  void loadTimings();
  void saveTimings() const;
  void resetForAccount(const QString& account, quint64 sessionGeneration);

  void scheduleNextDetail(int delayMs = -1);
  void sendNextDetail();
  void sendCurrentDetailAttempt();
  void retryOrFinishCurrent(const QString& reason);
  void finishCurrentDetail(bool succeeded, const QString& reason = {});
  void finishDetailBatchIfDone();
  void emitDetailProgress();
  void clearDetailState();
  bool send(const QString& command, const QString& parameters,
            qint64 instanceId, quint64 requestGeneration);

  PetRepository* repository_ = nullptr;
  Sender sender_;
  Timings timings_;
  QString settingsPath_;
  QString account_;
  quint64 sessionGeneration_ = 0;
  quint64 nextRequestGeneration_ = 0;

  QTimer automaticTimer_;
  QTimer listGapTimer_;
  QTimer backpackTimeoutTimer_;
  QTimer warehouseTimeoutTimer_;
  QTimer detailTimer_;
  QTimer detailTimeoutTimer_;

  bool listRunning_ = false;
  bool listManual_ = false;
  bool backpackDone_ = false;
  bool warehouseDone_ = false;
  bool backpackSucceeded_ = false;
  bool warehouseSucceeded_ = false;
  bool warehouseSent_ = false;
  quint64 listRequestGeneration_ = 0;
  PendingList pendingList_ = PendingList::None;

  QList<qint64> priorityQueue_;
  QList<qint64> batchQueue_;
  QSet<qint64> queuedIds_;
  QSet<qint64> batchIds_;
  bool batchRunning_ = false;
  bool batchPaused_ = false;
  int batchTotal_ = 0;
  int batchCompleted_ = 0;
  int batchSucceeded_ = 0;
  int batchFailed_ = 0;
  qint64 currentDetailId_ = 0;
  quint64 currentDetailGeneration_ = 0;
  int currentDetailRetries_ = 0;
};
