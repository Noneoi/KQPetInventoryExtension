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
    int moveRequestTimeoutMs = 10000;
  };

  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;
  using FlashInvoker = std::function<bool(const QString&, const QString&)>;

  explicit PetRefreshController(PetRepository* repository, QObject* parent = nullptr);
  void setSender(Sender sender);
  void setFlashInvoker(FlashInvoker invoker);
  void setTimings(const Timings& timings);
  Timings timings() const { return timings_; }

  bool listRefreshRunning() const { return listRunning_; }
  bool detailBatchRunning() const { return batchRunning_; }
  bool detailBatchPaused() const { return batchPaused_; }
  bool moveRunning() const;
  void publishState();

public slots:
  void requestManualListRefresh();
  void startWarehouseDetailRefresh();
  void pauseWarehouseDetailRefresh();
  void resumeWarehouseDetailRefresh();
  void cancelWarehouseDetailRefresh();
  void requestSingleDetail(qint64 instanceId);
  void requestMoveToWarehouse(qint64 instanceId);
  void requestMoveToBackpack(qint64 instanceId);
  void chooseMoveReplacement(qint64 outgoingInstanceId);
  void cancelMove();
  void requestFormationLoad();

signals:
  void statusChanged(const QString& status);
  void listRefreshRunningChanged(bool running);
  void detailProgressChanged(bool running, bool paused, int completed, int total,
                             int succeeded, int failed, qint64 currentInstanceId,
                             int estimatedSeconds);
  void commandSent(const QString& command, qint64 instanceId,
                   quint64 requestGeneration);
  void detailRequestFinished(qint64 instanceId, bool succeeded,
                             const QString& reason);
  void moveRunningChanged(bool running);
  void replacementRequired(qint64 incomingInstanceId,
                           const QList<qint64>& eligibleBackpackIds);
  void moveFinished(bool succeeded, const QString& message);

private slots:
  void onAccountSessionChanged(const QString& account, quint64 sessionGeneration);
  void onListResponseAccepted(const QString& command, quint64 requestGeneration);
  void onDetailResponseAccepted(qint64 instanceId, quint64 requestGeneration);
  void onDetailResponseRejected(qint64 instanceId, quint64 requestGeneration,
                                const QString& reason);
  void onSequenceUpdateAccepted(quint64 requestGeneration);
  void onSequenceUpdateRejected(quint64 requestGeneration,
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
  enum class MoveKind { None, ToWarehouse, ToBackpack };
  enum class MovePhase {
    Idle,
    WaitingForIdle,
    Preflight,
    WaitingReplacement,
    AwaitingWrite,
    Verification
  };
  void beginMove(MoveKind kind, qint64 instanceId);
  void startMovePreflight();
  void continueMoveAfterPreflight(bool listsSucceeded);
  void sendMoveSequence(const QList<qint64>& sequence);
  void deferMoveVerification(quint64 requestGeneration,
                             const QString& status);
  void startMoveVerification(const QString& status);
  void finishMoveVerification(bool listsSucceeded);
  void finishMove(bool succeeded, const QString& message);
  void restoreAfterMove();
  QList<qint64> eligibleReplacementIds() const;
  bool removeQueuedDetail(qint64 instanceId);
  bool send(const QString& command, const QString& parameters,
            qint64 instanceId, quint64 requestGeneration);

  PetRepository* repository_ = nullptr;
  Sender sender_;
  FlashInvoker flashInvoker_;
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
  QTimer moveTimeoutTimer_;

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

  MoveKind moveKind_ = MoveKind::None;
  MovePhase movePhase_ = MovePhase::Idle;
  qint64 moveInstanceId_ = 0;
  qint64 moveReplacementId_ = 0;
  quint64 moveRequestGeneration_ = 0;
  QString moveAccount_;
  quint64 moveSessionGeneration_ = 0;
  QList<qint64> moveTargetSequence_;
  bool batchWasRunningBeforeMove_ = false;
  bool batchWasPausedBeforeMove_ = false;
  bool moveWriteRejected_ = false;
  QString moveWriteFailureReason_;
};
