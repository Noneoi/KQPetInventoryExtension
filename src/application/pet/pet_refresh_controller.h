#pragma once

#include "move_operation.h"
#include "contracts/refresh_timings.h"
#include "storage/storage_types.h"
#include "protocol/protocol_transport.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

class PetRepository;
class ControllerCacheStorage;

class PetRefreshController final : public QObject {
  Q_OBJECT

public:
  using Timings = RefreshTimings;

  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;
  using FlashInvoker = std::function<bool(const QString&, const QString&)>;
  using WriteFlashInvoker = std::function<SubmissionOutcome(const QString&, const QString&)>;

  explicit PetRefreshController(PetRepository* repository, QObject* parent = nullptr);
  ~PetRefreshController() override;
  void setSender(Sender sender);
  void setFlashInvoker(FlashInvoker invoker);
  void setWriteFlashInvoker(WriteFlashInvoker invoker) { writeFlashInvoker_ = std::move(invoker); }
  void setAsyncSender(AsyncSender sender) { asyncSender_ = std::move(sender); }
  MoveOutcome lastMoveOutcome() const { return lastMoveOutcome_; }
  quint64 currentMoveTaskId() const { return moveTaskId_; }
  void setTimings(const Timings& timings);
  Timings timings() const { return timings_; }
  bool timingsKnown() const { return timingsKnown_; }
  bool timingsPending() const;
  int pendingSettingsCount() const;
  int pendingSettingsWriteCount() const;
  QString timingsStorageError() const;

  bool listRefreshRunning() const { return listRunning_; }
  bool detailBatchRunning() const { return batchRunning_; }
  bool detailBatchPaused() const { return batchPaused_; }
  bool moveRunning() const;
  void publishState();

public slots:
  void requestManualListRefresh();
  void startWarehouseDetailRefresh();
  void startWarehouseDetailRefreshForIds(const QList<qint64>& instanceIds);
  void pauseWarehouseDetailRefresh();
  void resumeWarehouseDetailRefresh();
  void cancelWarehouseDetailRefresh();
  void requestSingleDetail(qint64 instanceId);
  void requestMoveToWarehouse(qint64 instanceId);
  void requestMoveToBackpack(qint64 instanceId);
  void chooseMoveReplacement(qint64 outgoingInstanceId);
  void cancelMove();
  void requestFormationLoad();
  void handleSendReceipt(const SendReceipt& receipt);

signals:
  void statusChanged(const QString& status);
  void timingsChanged();
  void timingsPersistenceChanged(quint64 revision, StorageStatus status, const QString& error);
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
  void replacementSelectionRequired(quint64 moveTaskId, const QString& account, quint64 epoch,
                                     qint64 incomingInstanceId, const QList<qint64>& eligibleBackpackIds);
  void moveFinished(bool succeeded, const QString& message);
  void moveOutcomeChanged(const QString& operationId, const QString& account,
                          MoveOutcome outcome, const QString& message);
  void movePersistenceChanged(const QString& operationId, const QString& account,
                              quint64 epoch, StorageStatus status, const QString& error);

private slots:
  void onAccountSessionChanged(const QString& account, quint64 sessionGeneration);
  void onListResponseAccepted(const QString& command, quint64 requestGeneration);
  void onDetailResponseAccepted(qint64 instanceId, quint64 requestGeneration);
  void onDetailResponseRejected(qint64 instanceId, quint64 requestGeneration,
                                const QString& reason);
  void onSequenceUpdateAccepted(quint64 requestGeneration);
  void onSequenceUpdateRejected(quint64 requestGeneration,
                                const QString& reason);
  void onDetailPreservationFinished(quint64 taskId, qint64 instanceId,
                                    bool saved, const QString& reason);
  void onOperationStorageCompleted(const StorageResult& result);

private:
  enum class PendingList { None, Manual };

  void startListRefresh(bool manual);
  void sendWarehouseListRequest();
  void finishListPart(const QString& command, bool succeeded, const QString& reason = {});
  void maybeFinishListRefresh();
  void loadTimings();
  void saveTimings();
  QJsonObject timingsObject() const;
  void resetForAccount(const QString& account, quint64 sessionGeneration);

  void scheduleNextDetail(int delayMs = -1);
  void sendNextDetail();
  void sendCurrentDetailAttempt();
  enum class DetailFailure { Other, ResponseTimeout };
  void retryOrFinishCurrent(const QString& reason, DetailFailure failure = DetailFailure::Other);
  void finishCurrentDetail(bool succeeded, const QString& reason = {},
                           DetailFailure failure = DetailFailure::Other);
  void finishDetailBatchIfDone();
  void emitDetailProgress();
  void clearDetailState();
  enum class MoveKind { None, ToWarehouse, ToBackpack };
  enum class MovePhase {
    Idle,
    WaitingForIdle,
    Preflight,
    WaitingReplacement,
    WaitingForDetailPersistence,
    WaitingForIntent,
    WaitingDispatch,
    AwaitingWrite,
    Verification
  };
  void beginMove(MoveKind kind, qint64 instanceId);
  void startMovePreflight();
  void continueMoveAfterPreflight(bool listsSucceeded);
  void sendMoveSequence(const QList<qint64>& sequence);
  void preserveMoveDetail(qint64 instanceId, const QList<qint64>& sequence);
  void submitPreparedMove();
  void trackOperationWrite(const StorageSubmission& admission, bool intent);
  bool recordMoveOutcome(MoveOutcome outcome, const QString& reason = {});
  enum class DispatchKind { Backpack, Warehouse, Detail, Formation, MoveFlash, MoveSocket };
  struct PendingDispatch {
    OutboundIntent intent;
    DispatchKind kind = DispatchKind::Formation;
    quint64 generation = 0;
    QString operationId;
  };
  bool queueDispatch(const QString& command, const QString& parameters, qint64 instanceId,
                     quint64 generation, DispatchKind kind, qint64 deadline = 0);
  void expirePendingDispatches();
  void revokePendingDispatches();
  void finishMoveDispatch(const PendingDispatch& pending, const SendReceipt& receipt);
  void deferMoveVerification(quint64 requestGeneration,
                             const QString& status);
  void startMoveVerification(const QString& status);
  bool scheduleMoveVerificationRetry(const QString& reason);
  void finishMoveVerification(bool listsSucceeded);
  void finishMove(bool succeeded, const QString& message);
  void finishMove(MoveOutcome outcome, const QString& message);
  bool movePreflightStillValid() const;
  void restoreAfterMove();
  QList<qint64> eligibleReplacementIds() const;
  void publishReplacementSelection(const QList<qint64>& eligibleBackpackIds);
  bool removeQueuedDetail(qint64 instanceId);
  bool send(const QString& command, const QString& parameters,
            qint64 instanceId, quint64 requestGeneration);

  PetRepository* repository_ = nullptr;
  Sender sender_;
  FlashInvoker flashInvoker_;
  WriteFlashInvoker writeFlashInvoker_;
  AsyncSender asyncSender_;
  QHash<quint64, PendingDispatch> pendingDispatches_;
  SendPermit moveSendPermit_;
  std::unique_ptr<MoveOperationJournal> moveJournal_;
  struct PendingOperationWrite {
    QString operationId;
    QString account;
    QString path;
    quint64 epoch = 0;
    quint64 revision = 0;
    bool intent = false;
    StorageContext context;
  };
  QHash<quint64, PendingOperationWrite> operationWrites_;
  StorageContext moveStorageContext_;
  quint64 moveDetailPersistenceTaskId_ = 0;
  qint64 moveDetailPersistenceInstanceId_ = 0;
  quint64 moveIntentTaskId_ = 0;
  MoveOutcome moveOutcome_ = MoveOutcome::NotSent;
  MoveOutcome lastMoveOutcome_ = MoveOutcome::NotSent;
  quint64 movePreflightRevision_ = 0;
  int movePreflightAttempts_ = 0;
  int moveVerificationAttempts_ = 0;
  bool terminalNotificationGuard_ = false;
  Timings timings_;
  ControllerCacheStorage* timingsStorage_ = nullptr;
  bool timingsKnown_ = false;
  bool timingsEdited_ = false;
  quint64 timingsSaveRevision_ = 0;
  QString account_;
  quint64 sessionGeneration_ = 0;
  quint64 nextRequestGeneration_ = 0;

  QTimer listGapTimer_;
  QTimer backpackTimeoutTimer_;
  QTimer warehouseTimeoutTimer_;
  QTimer detailTimer_;
  QTimer detailTimeoutTimer_;
  QTimer moveTimeoutTimer_;
  QTimer dispatchReceiptTimer_;

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
  int consecutiveBatchTimeouts_ = 0;
  qint64 currentDetailId_ = 0;
  quint64 currentDetailGeneration_ = 0;
  int currentDetailRetries_ = 0;
  bool currentDetailFromBatch_ = false;
  bool currentDetailFailuresOnlyTimeouts_ = true;
  bool currentDetailAwaitingStorage_ = false;

  MoveKind moveKind_ = MoveKind::None;
  quint64 moveTaskId_ = 0;
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
  bool moveResponseBeforeReceipt_ = false;
  QString moveWriteFailureReason_;
};
