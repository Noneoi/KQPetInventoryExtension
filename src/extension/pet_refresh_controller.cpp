#include "pet_refresh_controller.h"

#include "diagnostic_logger.h"
#include "controller_cache_storage.h"
#include "pet_move_policy.h"
#include "packet_contract.h"
#include "pet_repository.h"
#include "storage_service.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QtGlobal>

#include <utility>
#include <limits>

PetRefreshController::PetRefreshController(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {
  loadTimings();
  dispatchReceiptTimer_.setInterval(10);
  connect(&dispatchReceiptTimer_, &QTimer::timeout, this, &PetRefreshController::expirePendingDispatches);
  connect(repository_, &PetRepository::sessionTrustChanged, this,
          [this](SessionConnectionState state, const QString&) {
    if (state != SessionConnectionState::Active)
      resetForAccount(repository_->accountKey(), repository_->sessionGeneration());
  });
  for (QTimer* timer : {&automaticTimer_, &listGapTimer_, &backpackTimeoutTimer_,
                        &warehouseTimeoutTimer_, &detailTimer_, &detailTimeoutTimer_,
                        &moveTimeoutTimer_}) {
    timer->setSingleShot(true);
  }

  connect(&listGapTimer_, &QTimer::timeout, this,
          &PetRefreshController::sendWarehouseListRequest);
  connect(&backpackTimeoutTimer_, &QTimer::timeout, this, [this]() {
    DiagnosticLogger::warning(QStringLiteral("timeout"),
                              QStringLiteral("backpack request timed out generation=%1")
                                  .arg(listRequestGeneration_));
    repository_->cancelListPart(QStringLiteral("2_1_10"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_10"), false, QStringLiteral("背包请求超时"));
  });
  connect(&warehouseTimeoutTimer_, &QTimer::timeout, this, [this]() {
    DiagnosticLogger::warning(QStringLiteral("timeout"),
                              QStringLiteral("warehouse request timed out generation=%1")
                                  .arg(listRequestGeneration_));
    repository_->cancelListPart(QStringLiteral("2_1_S"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_S"), false, QStringLiteral("仓库请求超时"));
  });
  connect(&detailTimer_, &QTimer::timeout, this, [this]() {
    if (currentDetailId_ > 0)
      sendCurrentDetailAttempt();
    else
      sendNextDetail();
  });
  connect(&detailTimeoutTimer_, &QTimer::timeout, this, [this]() {
    if (currentDetailAwaitingStorage_) return;
    DiagnosticLogger::warning(
        QStringLiteral("timeout"),
        QStringLiteral("detail request timed out instance=%1 generation=%2")
            .arg(currentDetailId_).arg(currentDetailGeneration_));
    repository_->cancelDetailRequest(currentDetailId_, currentDetailGeneration_);
    retryOrFinishCurrent(QStringLiteral("详情请求超时"), DetailFailure::ResponseTimeout);
  });
  connect(&moveTimeoutTimer_, &QTimer::timeout, this, [this]() {
    if (movePhase_ == MovePhase::WaitingForDetailPersistence || movePhase_ == MovePhase::WaitingForIntent) {
      finishMove(MoveOutcome::NotSent,
                 QStringLiteral("等待移动前必要数据保存超时，未提交写请求；已入队存储会有序收尾。"));
      return;
    }
    if (movePhase_ != MovePhase::AwaitingWrite) return;
    DiagnosticLogger::warning(
        QStringLiteral("timeout"),
        QStringLiteral("move write timed out; no retry generation=%1")
            .arg(moveRequestGeneration_));
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
    startMoveVerification(
        QStringLiteral("移动请求响应超时；不会重复写入，正在读取服务器最终状态……"));
  });

  connect(repository_, &PetRepository::accountSessionChanged, this,
          &PetRefreshController::onAccountSessionChanged);
  connect(repository_, &PetRepository::listResponseAccepted, this,
          &PetRefreshController::onListResponseAccepted);
  connect(repository_, &PetRepository::detailResponseAccepted, this,
          &PetRefreshController::onDetailResponseAccepted);
  connect(repository_, &PetRepository::detailResponseRejected, this,
          &PetRefreshController::onDetailResponseRejected);
  connect(repository_, &PetRepository::detailPersistenceQueued, this,
          [this](qint64 instanceId, quint64 generation, quint64) {
    if (instanceId != currentDetailId_ || generation != currentDetailGeneration_) return;
    currentDetailAwaitingStorage_ = true;
    detailTimeoutTimer_.stop();
    detailTimer_.stop();
  });
  connect(repository_, &PetRepository::detailPreservationFinished, this,
          &PetRefreshController::onDetailPreservationFinished);
  connect(repository_->storageService(), &StorageService::completed, this,
          &PetRefreshController::onOperationStorageCompleted);
  connect(repository_, &PetRepository::sequenceUpdateAccepted, this,
          &PetRefreshController::onSequenceUpdateAccepted);
  connect(repository_, &PetRepository::sequenceUpdateRejected, this,
          &PetRefreshController::onSequenceUpdateRejected);

  if (repository_->isAuthenticated())
    resetForAccount(repository_->accountKey(), repository_->sessionGeneration());
}

bool PetRefreshController::moveRunning() const {
  return movePhase_ != MovePhase::Idle;
}

PetRefreshController::~PetRefreshController() {
  if (movePhase_ == MovePhase::WaitingDispatch && moveSendPermit_.revoke() == SubmissionOutcome::Unknown)
    moveOutcome_ = MoveOutcome::Unknown;
  revokePendingDispatches();
  if (moveJournal_)
    moveJournal_->record(moveOutcome_ == MoveOutcome::NotSent ? MoveOutcome::NotSent : MoveOutcome::Unknown,
                         QStringLiteral("controller closing; accepted storage drains on its frozen context"));
}

void PetRefreshController::setSender(Sender sender) { sender_ = std::move(sender); }

void PetRefreshController::setFlashInvoker(FlashInvoker invoker) {
  flashInvoker_ = std::move(invoker);
}

void PetRefreshController::requestFormationLoad() {
  if (!repository_->isAuthenticated()) return;
  send(QStringLiteral("2_2_10"), QStringLiteral("{}"), 0, 0);
}

void PetRefreshController::publishState() {
  emit listRefreshRunningChanged(listRunning_);
  emitDetailProgress();
  emit moveRunningChanged(moveRunning());
}

void PetRefreshController::setTimings(const Timings& timings) {
  timingsEdited_ = true;
  timingsKnown_ = true;
  timings_ = timings;
  timings_.automaticIntervalMs = qMax(1, timings_.automaticIntervalMs);
  timings_.listRequestGapMs = qMax(0, timings_.listRequestGapMs);
  timings_.listTimeoutMs = qMax(1, timings_.listTimeoutMs);
  timings_.detailRequestGapMs = qMax(0, timings_.detailRequestGapMs);
  timings_.detailBatchRestMs = qMax(0, timings_.detailBatchRestMs);
  timings_.detailTimeoutMs = qMax(1, timings_.detailTimeoutMs);
  timings_.detailBatchSize = qMax(1, timings_.detailBatchSize);
  timings_.detailMaxRetries = qMax(0, timings_.detailMaxRetries);
  timings_.moveRequestTimeoutMs = qMax(1, timings_.moveRequestTimeoutMs);
  saveTimings();
  if (repository_->isAuthenticated() && !listRunning_ && !batchRunning_)
    scheduleAutomaticRefresh();
  emit statusChanged(QStringLiteral("刷新参数已在内存应用，正在保存；仅在手动刷新或点选精灵时查询最新数据。"));
}

bool PetRefreshController::timingsPending() const { return timingsStorage_ && timingsStorage_->pendingCount() > 0; }
int PetRefreshController::pendingSettingsCount() const { return timingsStorage_ ? timingsStorage_->pendingCount() : 0; }
int PetRefreshController::pendingSettingsWriteCount() const { return timingsStorage_ ? timingsStorage_->pendingWriteCount() : 0; }
QString PetRefreshController::timingsStorageError() const { return timingsStorage_ ? timingsStorage_->error() : QString{}; }

void PetRefreshController::loadTimings() {
  timingsStorage_ = new ControllerCacheStorage(repository_->storageService(), QStringLiteral("settings.json"), 64 * 1024, this);
  timingsStorage_->mergeJsonObject = true;
  timingsStorage_->canWrite = [] { return true; }; // explicit local preference, shared across accounts
  timingsStorage_->snapshot = [this] { return timingsObject(); };
  timingsStorage_->stateChanged = [this] { emit timingsChanged(); };
  timingsStorage_->loaded = [this](StorageStatus status, const QJsonObject& object, const QString& error) {
    // A user edit made while the read was queued is newer than that disk image.
    if (timingsEdited_) return;
    Timings loaded;
    bool valid = status == StorageStatus::NotFound || status == StorageStatus::Loaded;
    qint64 schema = 0;
    if (object.contains(QStringLiteral("schema")) &&
        (!PacketContracts::checkedInteger(object.value(QStringLiteral("schema")), &schema) || schema != 1)) valid = false;
    const auto field = [&object, &valid](const QString& key, int* destination, int minimum) {
      if (!object.contains(key)) return;
      qint64 parsed = 0;
      if (!PacketContracts::checkedInteger(object.value(key), &parsed, minimum, std::numeric_limits<int>::max())) valid = false;
      else *destination = static_cast<int>(parsed);
    };
    field(QStringLiteral("automaticIntervalMs"), &loaded.automaticIntervalMs, 1);
    field(QStringLiteral("listRequestGapMs"), &loaded.listRequestGapMs, 0);
    field(QStringLiteral("listTimeoutMs"), &loaded.listTimeoutMs, 1);
    field(QStringLiteral("detailRequestGapMs"), &loaded.detailRequestGapMs, 0);
    field(QStringLiteral("detailBatchRestMs"), &loaded.detailBatchRestMs, 0);
    field(QStringLiteral("detailTimeoutMs"), &loaded.detailTimeoutMs, 1);
    field(QStringLiteral("detailBatchSize"), &loaded.detailBatchSize, 1);
    field(QStringLiteral("detailMaxRetries"), &loaded.detailMaxRetries, 0);
    field(QStringLiteral("moveRequestTimeoutMs"), &loaded.moveRequestTimeoutMs, 1);
    timingsKnown_ = valid;
    if (valid) { timings_ = loaded; scheduleAutomaticRefresh(); }
    else emit statusChanged(QStringLiteral("刷新参数读取失败，自动刷新暂未启动：%1").arg(error.isEmpty() ? QStringLiteral("设置字段类型或数值无效") : error));
  };
  timingsStorage_->changed = [this](const QString&, quint64, quint64 revision, StorageStatus status, const QString& error) {
    if (status == StorageStatus::Queued) timingsSaveRevision_ = revision;
    emit timingsPersistenceChanged(revision, status, error);
    if (status == StorageStatus::Saved && revision == timingsSaveRevision_)
      emit statusChanged(QStringLiteral("刷新参数已保存"));
    else if (status != StorageStatus::Saved && status != StorageStatus::Queued && status != StorageStatus::Superseded)
      emit statusChanged(QStringLiteral("刷新参数保存失败，本次内存设置仍生效：%1").arg(error));
  };
  timingsStorage_->start(repository_->storageService()->createSharedContext(), {}, 0);
}

void PetRefreshController::saveTimings() { if (timingsStorage_) timingsStorage_->save(); }

QJsonObject PetRefreshController::timingsObject() const {
  return {{QStringLiteral("schema"), 1},
      {QStringLiteral("automaticIntervalMs"), timings_.automaticIntervalMs},
      {QStringLiteral("listRequestGapMs"), timings_.listRequestGapMs},
      {QStringLiteral("listTimeoutMs"), timings_.listTimeoutMs},
      {QStringLiteral("detailRequestGapMs"), timings_.detailRequestGapMs},
      {QStringLiteral("detailBatchRestMs"), timings_.detailBatchRestMs},
      {QStringLiteral("detailTimeoutMs"), timings_.detailTimeoutMs},
      {QStringLiteral("detailBatchSize"), timings_.detailBatchSize},
      {QStringLiteral("detailMaxRetries"), timings_.detailMaxRetries},
      {QStringLiteral("moveRequestTimeoutMs"), timings_.moveRequestTimeoutMs}};
}

bool PetRefreshController::send(const QString& command, const QString& parameters,
                                qint64 instanceId, quint64 requestGeneration) {
  if (asyncSender_) {
    const DispatchKind kind = command == QStringLiteral("2_1_10") ? DispatchKind::Backpack
        : command == QStringLiteral("2_1_S") ? DispatchKind::Warehouse
        : command == QStringLiteral("2_1_R") ? DispatchKind::Detail : DispatchKind::Formation;
    return queueDispatch(command, parameters, instanceId, requestGeneration, kind);
  }
  emit commandSent(command, instanceId, requestGeneration);
  return sender_ && sender_(QStringLiteral("PJXExtension"), command, parameters);
}

bool PetRefreshController::queueDispatch(const QString& command, const QString& parameters,
    qint64 instanceId, quint64 generation, DispatchKind kind, qint64 deadline) {
  if (!asyncSender_ || !repository_->isAuthenticated()) return false;
  const bool move = kind == DispatchKind::MoveFlash || kind == DispatchKind::MoveSocket;
  if (move && (!moveJournal_ || movePhase_ != MovePhase::WaitingDispatch || !movePreflightStillValid())) return false;
  if ((kind == DispatchKind::Backpack || kind == DispatchKind::Warehouse) &&
      (!listRunning_ || generation != listRequestGeneration_)) return false;
  if (kind == DispatchKind::Detail &&
      (generation != currentDetailGeneration_ || instanceId != currentDetailId_)) return false;
  for (const auto& pending : std::as_const(pendingDispatches_)) {
    if (pending.intent.ticket.command != command) continue;
    return pending.generation == generation && pending.kind == kind;
  }
  const qint64 now = transportMonotonicMs();
  const qint64 timeout = move ? timings_.moveRequestTimeoutMs :
      kind == DispatchKind::Detail ? timings_.detailTimeoutMs : timings_.listTimeoutMs;
  OutboundIntent intent;
  intent.ticket = {nextTransportTaskId(), move ? moveAccount_ : account_,
      move ? moveSessionGeneration_ : sessionGeneration_, command, instanceId,
      deadline > 0 ? deadline : now + timeout, kind == DispatchKind::Formation ? 0 : timeout,
      kind == DispatchKind::Detail ? PacketCorrelationStrength::EntityCorrelated
                                  : PacketCorrelationStrength::CommandObserved};
  intent.source = repository_->sessionContext().source;
  intent.write = move;
  intent.preflightRevision = move ? movePreflightRevision_ : 0;
  if (kind == DispatchKind::MoveFlash) {
    intent.flashMethod = QStringLiteral("batchpet"); intent.flashArgument = parameters;
  } else { intent.extension = QStringLiteral("PJXExtension"); intent.parameters = parameters; }
  pendingDispatches_.insert(intent.ticket.taskId,
      {intent, kind, generation, moveJournal_ && move ? moveJournal_->operationId() : QString{}});
  if (move) moveSendPermit_ = intent.permit;
  if (!asyncSender_(intent)) {
    intent.permit.revoke();
    pendingDispatches_.remove(intent.ticket.taskId);
    return false;
  }
  if (!pendingDispatches_.isEmpty()) dispatchReceiptTimer_.start();
  return true;
}

void PetRefreshController::revokePendingDispatches() {
  for (const auto& pending : std::as_const(pendingDispatches_)) pending.intent.permit.revoke();
  pendingDispatches_.clear();
  dispatchReceiptTimer_.stop();
}

void PetRefreshController::expirePendingDispatches() {
  QList<SendReceipt> receipts;
  const qint64 now = transportMonotonicMs();
  for (const auto& pending : std::as_const(pendingDispatches_)) {
    const auto receipt = pollSendReceipt(pending.intent, now);
    if (receipt) receipts.append(*receipt);
  }
  for (const auto& receipt : receipts) handleSendReceipt(receipt);
}
void PetRefreshController::handleSendReceipt(const SendReceipt& receipt) {
  const auto found = pendingDispatches_.find(receipt.taskId);
  if (found == pendingDispatches_.end()) return;
  const PendingDispatch pending = found.value();
  if (receipt.account != pending.intent.ticket.account || receipt.sessionEpoch != pending.intent.ticket.sessionEpoch ||
      receipt.command != pending.intent.ticket.command) return;
  pendingDispatches_.erase(found);
  if (pendingDispatches_.isEmpty()) dispatchReceiptTimer_.stop();
  if (receipt.account != repository_->accountKey() || receipt.sessionEpoch != repository_->sessionGeneration()) return;
  if (pending.kind == DispatchKind::MoveFlash || pending.kind == DispatchKind::MoveSocket) {
    finishMoveDispatch(pending, receipt); return;
  }
  if (receipt.dispatchedAtMs >= 0 && receipt.outcome != SubmissionOutcome::DefinitelyNotSubmitted)
    emit commandSent(receipt.command, pending.intent.ticket.entityId, pending.generation);
  // Late receipts use the dispatch timestamp; handling this message never
  // starts a fresh full-length response timeout.
  const qint64 now = transportMonotonicMs();
  const qint64 remaining = receipt.dispatchedAtMs >= 0
      ? qMax<qint64>(0, receipt.dispatchedAtMs + pending.intent.ticket.responseTimeoutMs - now) : 0;
  const int delay = static_cast<int>(qMin<qint64>(remaining, std::numeric_limits<int>::max()));
  const bool submitted = receipt.outcome != SubmissionOutcome::DefinitelyNotSubmitted && receipt.dispatchedAtMs >= 0;
  if (pending.kind == DispatchKind::Backpack || pending.kind == DispatchKind::Warehouse) {
    if (!listRunning_ || pending.generation != listRequestGeneration_ ||
        receipt.account != account_ || receipt.sessionEpoch != sessionGeneration_) return;
    const bool backpack = pending.kind == DispatchKind::Backpack;
    if (!(backpack ? backpackDone_ : warehouseDone_)) {
      if (!submitted || !delay) {
        repository_->cancelListPart(receipt.command, pending.generation);
        finishListPart(receipt.command, false, submitted ? QStringLiteral("实际发送后的列表响应已超时") : receipt.error);
      } else (backpack ? backpackTimeoutTimer_ : warehouseTimeoutTimer_).start(delay);
    }
    if (backpack && listRunning_ && pending.generation == listRequestGeneration_ && !warehouseSent_) {
      const qint64 gap = submitted ? qMax<qint64>(0, receipt.dispatchedAtMs + timings_.listRequestGapMs - now) : 0;
      listGapTimer_.start(static_cast<int>(qMin<qint64>(gap, std::numeric_limits<int>::max())));
    }
  } else if (pending.kind == DispatchKind::Detail) {
    if (pending.generation != currentDetailGeneration_ || pending.intent.ticket.entityId != currentDetailId_ ||
        currentDetailAwaitingStorage_ || receipt.account != account_ || receipt.sessionEpoch != sessionGeneration_) return;
    if (!submitted || !delay) {
      repository_->cancelDetailRequest(currentDetailId_, currentDetailGeneration_);
      retryOrFinishCurrent(submitted ? QStringLiteral("实际发送后的详情响应已超时") : receipt.error,
                           submitted ? DetailFailure::ResponseTimeout : DetailFailure::Other);
    } else detailTimeoutTimer_.start(delay);
  }
}

void PetRefreshController::onAccountSessionChanged(const QString& account,
                                                    quint64 sessionGeneration) {
  resetForAccount(account, sessionGeneration);
}

void PetRefreshController::resetForAccount(const QString& account,
                                           quint64 sessionGeneration) {
  QScopedValueRollback<bool> terminalGuard(terminalNotificationGuard_, true);
  automaticTimer_.stop();
  listGapTimer_.stop();
  backpackTimeoutTimer_.stop();
  warehouseTimeoutTimer_.stop();
  detailTimer_.stop();
  detailTimeoutTimer_.stop();
  moveTimeoutTimer_.stop();
  if (movePhase_ == MovePhase::WaitingDispatch && moveSendPermit_.revoke() == SubmissionOutcome::Unknown)
    moveOutcome_ = MoveOutcome::Unknown;
  revokePendingDispatches();
  const bool hadMove = moveRunning();
  QString completedOperationId;
  QString completedAccount;
  QString completedMessage;
  MoveOutcome completedOutcome = MoveOutcome::NotSent;
  if (hadMove) {
    const MoveOutcome result = moveOutcome_ == MoveOutcome::NotSent
        ? MoveOutcome::NotSent : MoveOutcome::Unknown;
    const QString message = result == MoveOutcome::Unknown
        ? QStringLiteral("会话已变化；原账号移动请求的结果未知，保留记录等待只读核对。")
        : QStringLiteral("会话已变化，未提交移动请求。");
    recordMoveOutcome(result, message);
    lastMoveOutcome_ = result;
    completedOperationId = moveJournal_ ? moveJournal_->operationId() : QString{};
    completedAccount = moveAccount_;
    completedMessage = message;
    completedOutcome = result;
    moveJournal_.reset();
  }
  if (moveRequestGeneration_ > 0)
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
  moveKind_ = MoveKind::None;
  movePhase_ = MovePhase::Idle;
  moveTaskId_ = 0;
  moveInstanceId_ = 0;
  moveReplacementId_ = 0;
  moveRequestGeneration_ = 0;
  moveTargetSequence_.clear();
  moveStorageContext_.reset();
  moveDetailPersistenceTaskId_ = 0;
  moveDetailPersistenceInstanceId_ = 0;
  moveIntentTaskId_ = 0;
  moveOutcome_ = MoveOutcome::NotSent;
  account_ = account;
  sessionGeneration_ = sessionGeneration;
  pendingList_ = PendingList::None;
  if (listRunning_) emit listRefreshRunningChanged(false);
  listRunning_ = false;
  clearDetailState();
  if (hadMove) {
    emit moveRunningChanged(false);
    emit moveOutcomeChanged(completedOperationId, completedAccount, completedOutcome, completedMessage);
    emit moveFinished(false, completedMessage);
  }
  emit statusChanged(repository_->isAuthenticated()
      ? QStringLiteral("账号 %1 已就绪；使用本地缓存，手动刷新或点选精灵时获取最新数据。").arg(account_)
      : QStringLiteral("账号 %1 当前未在线；继续显示已保存的本地数据。").arg(account_));
  scheduleAutomaticRefresh();
}

void PetRefreshController::scheduleAutomaticRefresh() {
  // Retain old interval preferences for compatibility, but never schedule a
  // network read merely because time passed or a background task completed.
  automaticTimer_.stop();
}

void PetRefreshController::requestManualListRefresh() {
  if (!repository_->isAuthenticated()) {
    emit statusChanged(account_.isEmpty()
        ? QStringLiteral("尚未识别登录账号，不能刷新线上数据。")
        : QStringLiteral("账号已识别，但当前会话不可读取；请重新登录后再刷新。"));
    return;
  }
  if (moveRunning()) {
    emit statusChanged(QStringLiteral("精灵移动正在进行；移动完成后会自动刷新背包和仓库。"));
    return;
  }
  if (listRunning_) {
    listManual_ = true;
    emit statusChanged(QStringLiteral("列表刷新已在进行，本次手动请求已合并。"));
    return;
  }
  if (currentDetailId_ > 0) {
    pendingList_ = PendingList::Manual;
    emit statusChanged(QStringLiteral("当前单只详情请求完成后立即执行手动列表刷新。"));
    return;
  }
  startListRefresh(true);
}

void PetRefreshController::startListRefresh(bool manual) {
  if (!repository_->isAuthenticated() || listRunning_) return;
  automaticTimer_.stop();
  detailTimer_.stop();
  listRunning_ = true;
  listManual_ = manual;
  backpackDone_ = false;
  warehouseDone_ = false;
  backpackSucceeded_ = false;
  warehouseSucceeded_ = false;
  warehouseSent_ = false;
  pendingList_ = PendingList::None;
  listRequestGeneration_ = ++nextRequestGeneration_;
  repository_->beginListRefresh(listRequestGeneration_, account_, sessionGeneration_);
  emit listRefreshRunningChanged(true);
  emit statusChanged(QStringLiteral("正在刷新背包……"));
  const bool sent = send(QStringLiteral("2_1_10"), QStringLiteral("{}"), 0,
                         listRequestGeneration_);
  if (sent && !asyncSender_) {
    backpackTimeoutTimer_.start(timings_.listTimeoutMs);
  } else if (!sent) {
    repository_->cancelListPart(QStringLiteral("2_1_10"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_10"), false, QStringLiteral("背包请求发送失败"));
  }
  if (!asyncSender_ || !sent) listGapTimer_.start(asyncSender_ ? 0 : timings_.listRequestGapMs);
}

void PetRefreshController::sendWarehouseListRequest() {
  if (!listRunning_ || warehouseSent_) return;
  warehouseSent_ = true;
  repository_->expectListPart(QStringLiteral("2_1_S"), listRequestGeneration_,
                              account_, sessionGeneration_);
  emit statusChanged(QStringLiteral("正在刷新仓库……"));
  const bool sent = send(QStringLiteral("2_1_S"), QStringLiteral("null"), 0,
                         listRequestGeneration_);
  if (sent && !asyncSender_) {
    warehouseTimeoutTimer_.start(timings_.listTimeoutMs);
  } else if (!sent) {
    repository_->cancelListPart(QStringLiteral("2_1_S"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_S"), false, QStringLiteral("仓库请求发送失败"));
  }
}

void PetRefreshController::onListResponseAccepted(const QString& command,
                                                  quint64 requestGeneration) {
  if (!listRunning_ || requestGeneration != listRequestGeneration_) return;
  if (command == QStringLiteral("2_1_S")) {
    bool allGroups = true;
    for (const QString& group : {QStringLiteral("normal"), QStringLiteral("elite"), QStringLiteral("goodbye")}) {
      const auto observed = repository_->warehouseObservation(group);
      allGroups = allGroups && observed.complete && observed.receiveSequence == repository_->lastInboundSequence();
    }
    if (!allGroups) {
      finishListPart(command, false, QStringLiteral("仓库仅更新了有有效数据的部分分组"));
      return;
    }
  }
  finishListPart(command, true);
}

void PetRefreshController::finishListPart(const QString& command, bool succeeded,
                                          const QString& reason) {
  if (!listRunning_) return;
  if (command == QStringLiteral("2_1_10")) {
    if (backpackDone_) return;
    backpackDone_ = true;
    backpackSucceeded_ = succeeded;
    backpackTimeoutTimer_.stop();
  } else if (command == QStringLiteral("2_1_S")) {
    if (warehouseDone_) return;
    warehouseDone_ = true;
    warehouseSucceeded_ = succeeded;
    warehouseTimeoutTimer_.stop();
  } else {
    return;
  }
  if (!succeeded && !reason.isEmpty()) emit statusChanged(reason + QStringLiteral("，已保留旧缓存。"));
  maybeFinishListRefresh();
}

void PetRefreshController::maybeFinishListRefresh() {
  if (!listRunning_ || !backpackDone_ || !warehouseDone_) return;
  listRunning_ = false;
  DiagnosticLogger::info(
      QStringLiteral("request"),
      QStringLiteral("list refresh completed generation=%1 backpack=%2 warehouse=%3")
          .arg(listRequestGeneration_)
          .arg(backpackSucceeded_ ? QStringLiteral("ok") : QStringLiteral("failed"))
          .arg(warehouseSucceeded_ ? QStringLiteral("ok") : QStringLiteral("failed")));
  emit listRefreshRunningChanged(false);
  if (backpackSucceeded_ && warehouseSucceeded_) {
    emit statusChanged(QStringLiteral("背包/仓库列表读取完成，正在更新本地缓存；下次由手动操作刷新。"));
  } else {
    emit statusChanged(QStringLiteral("背包/仓库列表刷新结束：背包 %1，仓库 %2；失败部分已保留旧缓存。")
                           .arg(backpackSucceeded_ ? QStringLiteral("成功")
                                                  : QStringLiteral("超时或失败"),
                                warehouseSucceeded_ ? QStringLiteral("成功")
                                                   : QStringLiteral("超时或失败")));
  }
  if (moveRunning()) {
    const bool listsSucceeded = backpackSucceeded_ && warehouseSucceeded_;
    if (movePhase_ == MovePhase::Preflight) {
      continueMoveAfterPreflight(listsSucceeded);
      return;
    }
    if (movePhase_ == MovePhase::Verification) {
      finishMoveVerification(listsSucceeded);
      return;
    }
    if (movePhase_ == MovePhase::WaitingForIdle) {
      startMovePreflight();
      return;
    }
  }
  if (backpackSucceeded_ && warehouseSucceeded_ && !repository_->formationKnown())
    requestFormationLoad();
  if (!batchRunning_) scheduleAutomaticRefresh();
  scheduleNextDetail(0);
}

void PetRefreshController::startWarehouseDetailRefresh() {
  startWarehouseDetailRefreshForIds(repository_->warehouseIdsByDetailAge());
}

void PetRefreshController::startWarehouseDetailRefreshForIds(const QList<qint64>& instanceIds) {
  if (!repository_->isAuthenticated()) {
    emit statusChanged(account_.isEmpty()
        ? QStringLiteral("尚未识别登录账号，不能刷新仓库详情。")
        : QStringLiteral("账号已识别，但当前会话不可读取；请重新登录后再刷新详情。"));
    return;
  }
  if (moveRunning()) {
    emit statusChanged(QStringLiteral("精灵移动正在进行，暂时不能启动仓库详情批量刷新。"));
    return;
  }
  if (batchRunning_) {
    emit statusChanged(QStringLiteral("仓库详情刷新任务已在运行。"));
    return;
  }
  if (instanceIds.isEmpty()) {
    emit statusChanged(QStringLiteral("未选择需要刷新的仓库精灵。"));
    return;
  }
  const QSet<qint64> selectedIds(instanceIds.begin(), instanceIds.end());
  const QList<qint64> ids = repository_->warehouseIdsByDetailAge();
  batchQueue_.clear();
  batchIds_.clear();
  for (qint64 id : ids) {
    if (id <= 0 || id == currentDetailId_ || !selectedIds.contains(id)) continue;
    if (queuedIds_.contains(id)) {
      batchIds_.insert(id);
      continue;
    }
    batchQueue_.append(id);
    queuedIds_.insert(id);
    batchIds_.insert(id);
  }
  batchRunning_ = !batchIds_.isEmpty();
  batchPaused_ = false;
  batchTotal_ = batchIds_.size();
  batchCompleted_ = 0;
  batchSucceeded_ = 0;
  batchFailed_ = 0;
  consecutiveBatchTimeouts_ = 0;
  emitDetailProgress();
  if (!batchRunning_) {
    emit statusChanged(QStringLiteral("当前仓库没有可刷新的精灵详情。"));
    return;
  }
  automaticTimer_.stop();
  emit statusChanged(QStringLiteral("仓库详情刷新已启动：共 %1 只，逐只读取并覆盖保存本地缓存。").arg(batchTotal_));
  scheduleNextDetail(0);
}

void PetRefreshController::pauseWarehouseDetailRefresh() {
  if (moveRunning()) return;
  if (!batchRunning_) return;
  batchPaused_ = true;
  detailTimer_.stop();
  emitDetailProgress();
  emit statusChanged(currentDetailId_ > 0
                         ? QStringLiteral("将在当前详情请求结束后暂停批量任务。")
                         : QStringLiteral("仓库详情刷新已暂停。"));
}

void PetRefreshController::resumeWarehouseDetailRefresh() {
  if (moveRunning()) {
    emit statusChanged(QStringLiteral("精灵移动完成后会自动恢复仓库详情任务。"));
    return;
  }
  if (!batchRunning_ || !batchPaused_) return;
  batchPaused_ = false;
  consecutiveBatchTimeouts_ = 0;
  emitDetailProgress();
  emit statusChanged(QStringLiteral("仓库详情刷新已继续。"));
  scheduleNextDetail(0);
}

void PetRefreshController::cancelWarehouseDetailRefresh() {
  if (!batchRunning_ && currentDetailId_ <= 0 && priorityQueue_.isEmpty()) return;
  if (currentDetailId_ > 0)
    repository_->cancelDetailRequest(currentDetailId_, currentDetailGeneration_);
  detailTimer_.stop();
  detailTimeoutTimer_.stop();
  clearDetailState();
  emit statusChanged(QStringLiteral("仓库详情刷新任务已取消，已有本地缓存全部保留。"));
  scheduleAutomaticRefresh();
}

void PetRefreshController::requestSingleDetail(qint64 instanceId) {
  if (instanceId <= 0) return;
  if (!repository_->isAuthenticated()) {
    repository_->requestCachedDetail(instanceId);
    const QString reason = QStringLiteral("当前未在线，继续显示本地缓存");
    emit statusChanged(QStringLiteral("实例 %1 详情未刷新：%2。")
                           .arg(instanceId).arg(reason));
    emit detailRequestFinished(instanceId, false, reason);
    return;
  }
  if (moveRunning()) {
    emit statusChanged(QStringLiteral("精灵移动正在进行，详情刷新将在移动完成后恢复。"));
    return;
  }
  if (currentDetailId_ == instanceId) {
    emit statusChanged(QStringLiteral("实例 %1 的详情正在更新，请等待服务器返回。")
                           .arg(instanceId));
    return;
  }
  priorityQueue_.removeAll(instanceId);
  batchQueue_.removeAll(instanceId);
  if (!queuedIds_.contains(instanceId)) queuedIds_.insert(instanceId);
  priorityQueue_.prepend(instanceId);
  const QDateTime cachedAt = repository_->detailSavedAt(instanceId);
  emit statusChanged(cachedAt.isValid()
                         ? QStringLiteral("已显示实例 %1 的本地缓存（%2），正在获取最新详情……")
                               .arg(instanceId)
                               .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                         : QStringLiteral("实例 %1 尚无详情缓存，正在获取线上详情……")
                               .arg(instanceId));
  if (currentDetailId_ <= 0) scheduleNextDetail(0);
}

void PetRefreshController::scheduleNextDetail(int delayMs) {
  if (currentDetailId_ > 0 || listRunning_ || !repository_->isAuthenticated()) return;
  if (!priorityQueue_.isEmpty()) {
    detailTimer_.start(delayMs < 0 ? timings_.detailRequestGapMs : delayMs);
    return;
  }
  if (batchRunning_ && !batchPaused_ && !batchQueue_.isEmpty())
    detailTimer_.start(delayMs < 0 ? timings_.detailRequestGapMs : delayMs);
  else
    finishDetailBatchIfDone();
}

void PetRefreshController::sendNextDetail() {
  if (currentDetailId_ > 0 || listRunning_) return;
  if (!priorityQueue_.isEmpty()) {
    currentDetailId_ = priorityQueue_.takeFirst();
    currentDetailFromBatch_ = false;
  } else if (batchRunning_ && !batchPaused_ && !batchQueue_.isEmpty()) {
    currentDetailId_ = batchQueue_.takeFirst();
    currentDetailFromBatch_ = true;
  } else {
    finishDetailBatchIfDone();
    return;
  }
  currentDetailRetries_ = 0;
  currentDetailFailuresOnlyTimeouts_ = true;
  currentDetailAwaitingStorage_ = false;
  emitDetailProgress();
  sendCurrentDetailAttempt();
}

void PetRefreshController::sendCurrentDetailAttempt() {
  if (currentDetailId_ <= 0 || currentDetailAwaitingStorage_ || !repository_->isAuthenticated()) return;
  currentDetailGeneration_ = ++nextRequestGeneration_;
  repository_->expectDetail(currentDetailId_, currentDetailGeneration_, account_,
                            sessionGeneration_);
  const QString parameters = QStringLiteral("{\"pi\":%1}").arg(currentDetailId_);
  const qint64 instance = currentDetailId_;
  const quint64 generation = currentDetailGeneration_;
  const bool sent = send(QStringLiteral("2_1_R"), parameters, currentDetailId_,
                         currentDetailGeneration_);
  if (instance != currentDetailId_ || generation != currentDetailGeneration_ || currentDetailAwaitingStorage_) return;
  if (sent && !asyncSender_) {
    detailTimeoutTimer_.start(timings_.detailTimeoutMs);
  } else if (!sent) {
    repository_->cancelDetailRequest(currentDetailId_, currentDetailGeneration_);
    retryOrFinishCurrent(QStringLiteral("详情请求发送失败"));
  }
}

void PetRefreshController::onDetailResponseAccepted(qint64 instanceId,
                                                     quint64 requestGeneration) {
  if (instanceId != currentDetailId_ || requestGeneration != currentDetailGeneration_) return;
  detailTimeoutTimer_.stop();
  finishCurrentDetail(true);
}

void PetRefreshController::onDetailResponseRejected(qint64 instanceId,
                                                     quint64 requestGeneration,
                                                     const QString& reason) {
  if (instanceId != currentDetailId_ || requestGeneration != currentDetailGeneration_) return;
  detailTimeoutTimer_.stop();
  // Any matched valid response now attempts local persistence. An IO failure
  // is reported as such and must not trigger another network request.
  if (currentDetailAwaitingStorage_) finishCurrentDetail(false, reason);
  else retryOrFinishCurrent(reason);
}

void PetRefreshController::retryOrFinishCurrent(const QString& reason, DetailFailure failure) {
  if (currentDetailId_ <= 0) return;
  if (failure != DetailFailure::ResponseTimeout) currentDetailFailuresOnlyTimeouts_ = false;
  if (currentDetailRetries_ < timings_.detailMaxRetries) {
    ++currentDetailRetries_;
    emit statusChanged(QStringLiteral("实例 %1 详情失败：%2；正在重试 %3/%4。")
                           .arg(currentDetailId_).arg(reason)
                           .arg(currentDetailRetries_).arg(timings_.detailMaxRetries));
    detailTimer_.start(timings_.detailRequestGapMs);
    return;
  }
  finishCurrentDetail(false, reason, failure);
}

void PetRefreshController::finishCurrentDetail(bool succeeded, const QString& reason,
                                               DetailFailure failure) {
  const qint64 finishedId = currentDetailId_;
  const bool countedForBatch = batchIds_.remove(finishedId);
  const bool batchTimeout = countedForBatch && currentDetailFromBatch_ && !succeeded &&
      failure == DetailFailure::ResponseTimeout && currentDetailFailuresOnlyTimeouts_;
  if (succeeded || (countedForBatch && !batchTimeout)) consecutiveBatchTimeouts_ = 0;
  else if (batchTimeout) ++consecutiveBatchTimeouts_;
  const bool pauseAfterTimeouts = batchRunning_ && !batchPaused_ && !batchIds_.isEmpty() &&
      batchTimeout && consecutiveBatchTimeouts_ >= 3;
  if (pauseAfterTimeouts) {
    batchPaused_ = true;
    detailTimer_.stop();
  }
  queuedIds_.remove(finishedId);
  currentDetailId_ = 0;
  currentDetailGeneration_ = 0;
  currentDetailRetries_ = 0;
  currentDetailFromBatch_ = false;
  currentDetailFailuresOnlyTimeouts_ = true;
  currentDetailAwaitingStorage_ = false;
  if (countedForBatch) {
    ++batchCompleted_;
    if (succeeded) ++batchSucceeded_;
    else ++batchFailed_;
  }
  if (succeeded) {
    DiagnosticLogger::info(QStringLiteral("detail"),
                           QStringLiteral("completed instance=%1").arg(finishedId));
    emit statusChanged(QStringLiteral("实例 %1 的详情已保存至当前账号本地缓存。").arg(finishedId));
  } else {
    DiagnosticLogger::error(
        QStringLiteral("detail"),
        QStringLiteral("failed instance=%1 reason=%2; old cache retained")
            .arg(finishedId).arg(reason));
    emit statusChanged(QStringLiteral("实例 %1 详情更新失败：%2；旧缓存已保留。")
                           .arg(finishedId).arg(reason));
  }
  emit detailRequestFinished(finishedId, succeeded, reason);
  emitDetailProgress();
  if (pauseAfterTimeouts) {
    const QString message = QStringLiteral("连续 3 只在重试后仍未收到匹配的有效详情，已暂停以保留后续队列；已完成结果保留，可点击“继续”刷新尚未执行的精灵。");
    DiagnosticLogger::warning(QStringLiteral("detail"), message);
    emit statusChanged(message);
  }

  if (movePhase_ == MovePhase::WaitingForIdle) {
    startMovePreflight();
    return;
  }

  if (pendingList_ != PendingList::None) {
    const bool manual = pendingList_ == PendingList::Manual;
    pendingList_ = PendingList::None;
    startListRefresh(manual);
    return;
  }
  int delay = timings_.detailRequestGapMs;
  if (countedForBatch && batchCompleted_ > 0 &&
      batchCompleted_ % timings_.detailBatchSize == 0)
    delay = timings_.detailBatchRestMs;
  scheduleNextDetail(delay);
}

void PetRefreshController::finishDetailBatchIfDone() {
  if (!batchRunning_ || !batchIds_.isEmpty() || currentDetailId_ > 0) return;
  batchRunning_ = false;
  batchPaused_ = false;
  DiagnosticLogger::info(
      QStringLiteral("detail"),
      QStringLiteral("batch completed succeeded=%1 failed=%2 total=%3")
          .arg(batchSucceeded_).arg(batchFailed_).arg(batchTotal_));
  emitDetailProgress();
  emit statusChanged(QStringLiteral("仓库详情刷新完成：已保存 %1，失败 %2。")
                        .arg(batchSucceeded_).arg(batchFailed_));
  scheduleAutomaticRefresh();
}

void PetRefreshController::emitDetailProgress() {
  const int remaining = qMax(0, batchTotal_ - batchCompleted_);
  const int rests = timings_.detailBatchSize > 0 ? remaining / timings_.detailBatchSize : 0;
  const int estimateMs = remaining * timings_.detailRequestGapMs +
                         rests * timings_.detailBatchRestMs;
  emit detailProgressChanged(batchRunning_, batchPaused_, batchCompleted_, batchTotal_,
                             batchSucceeded_, batchFailed_, currentDetailId_,
                             (estimateMs + 999) / 1000);
}

void PetRefreshController::requestMoveToWarehouse(qint64 instanceId) {
  beginMove(MoveKind::ToWarehouse, instanceId);
}

void PetRefreshController::requestMoveToBackpack(qint64 instanceId) {
  beginMove(MoveKind::ToBackpack, instanceId);
}

void PetRefreshController::beginMove(MoveKind kind, qint64 instanceId) {
  if (terminalNotificationGuard_) {
    const QString requestedAccount = repository_->accountKey();
    const quint64 requestedSession = repository_->sessionGeneration();
    QTimer::singleShot(0, this, [this, kind, instanceId, requestedAccount, requestedSession]() {
      if (repository_->accountKey() == requestedAccount &&
          repository_->sessionGeneration() == requestedSession)
        beginMove(kind, instanceId);
    });
    return;
  }
  if (!repository_->isAuthenticated()) {
    emit statusChanged(QStringLiteral("尚未识别登录账号，不能移动精灵。"));
    return;
  }
  if (instanceId <= 0 || kind == MoveKind::None) return;
  if (moveRunning()) {
    emit statusChanged(QStringLiteral("已有精灵移动操作正在进行，请等待完成。"));
    return;
  }

  moveTaskId_ = nextTransportTaskId();
  moveKind_ = kind;
  movePhase_ = MovePhase::WaitingForIdle;
  moveInstanceId_ = instanceId;
  moveReplacementId_ = 0;
  moveRequestGeneration_ = 0;
  moveAccount_ = repository_->accountKey();
  moveSessionGeneration_ = repository_->sessionGeneration();
  moveTargetSequence_.clear();
  moveWriteRejected_ = false;
  moveWriteFailureReason_.clear();
  moveResponseBeforeReceipt_ = false;
  moveOutcome_ = MoveOutcome::NotSent;
  movePreflightRevision_ = 0;
  moveJournal_.reset();
  moveStorageContext_ = repository_->storageContext();
  moveDetailPersistenceTaskId_ = 0;
  moveDetailPersistenceInstanceId_ = 0;
  moveIntentTaskId_ = 0;
  moveSendPermit_ = SendPermit{};
  DiagnosticLogger::info(
      QStringLiteral("move"),
      QStringLiteral("started kind=%1 instance=%2 session_generation=%3")
          .arg(kind == MoveKind::ToWarehouse ? QStringLiteral("to-warehouse")
                                             : QStringLiteral("to-backpack"))
          .arg(instanceId).arg(moveSessionGeneration_));
  batchWasRunningBeforeMove_ = batchRunning_;
  batchWasPausedBeforeMove_ = batchPaused_;
  automaticTimer_.stop();
  if (batchRunning_) {
    batchPaused_ = true;
    detailTimer_.stop();
    emitDetailProgress();
  }
  emit moveRunningChanged(true);

  if (listRunning_ || currentDetailId_ > 0 || !priorityQueue_.isEmpty()) {
    emit statusChanged(QStringLiteral("正在等待当前网络请求结束，随后校验并移动实例 %1……")
                           .arg(instanceId));
    if (!listRunning_ && currentDetailId_ <= 0) scheduleNextDetail(0);
    return;
  }
  startMovePreflight();
}

void PetRefreshController::startMovePreflight() {
  if (!moveRunning() || movePhase_ == MovePhase::AwaitingWrite ||
      movePhase_ == MovePhase::Verification)
    return;
  if (!repository_->isAuthenticated() || repository_->accountKey() != moveAccount_ ||
      repository_->sessionGeneration() != moveSessionGeneration_) {
    finishMove(false, QStringLiteral("账号会话已经变化，移动操作已取消。"));
    return;
  }
  movePhase_ = MovePhase::Preflight;
  emit statusChanged(QStringLiteral("移动前正在强制刷新背包和仓库，避免使用旧顺序……"));
  startListRefresh(true);
}

void PetRefreshController::publishReplacementSelection(const QList<qint64>& eligibleBackpackIds) {
  if (movePhase_ != MovePhase::WaitingReplacement) return;
  const quint64 task = moveTaskId_;
  emit replacementSelectionRequired(task, moveAccount_, moveSessionGeneration_, moveInstanceId_, eligibleBackpackIds);
  if (movePhase_ == MovePhase::WaitingReplacement && moveTaskId_ == task)
    emit replacementRequired(moveInstanceId_, eligibleBackpackIds);
}

QList<qint64> PetRefreshController::eligibleReplacementIds() const {
  QList<qint64> eligible;
  for (qint64 id : repository_->backpackIds(0)) {
    const QJsonObject pet = repository_->backpackPet(id);
    if (pet.isEmpty() || !PetMovePolicy::restriction(pet).isEmpty()) continue;
    eligible.append(id);
  }
  return eligible;
}

void PetRefreshController::continueMoveAfterPreflight(bool listsSucceeded) {
  if (!moveRunning() || movePhase_ != MovePhase::Preflight) return;
  if (!listsSucceeded) {
    finishMove(false, QStringLiteral("移动前列表刷新失败，未发送任何写操作。"));
    return;
  }
  if (!repository_->listObservationsAuthoritativeForWrite()) {
    finishMove(MoveOutcome::NotSent,
               QStringLiteral("列表来源或写前观察顺序尚未确证，未提交移动请求；当前数据仅供查看。"));
    return;
  }
  movePreflightRevision_ = repository_->inventoryRevision();
  if (repository_->accountKey() != moveAccount_ ||
      repository_->sessionGeneration() != moveSessionGeneration_) {
    finishMove(false, QStringLiteral("账号会话已经变化，未发送移动请求。"));
    return;
  }

  const QList<qint64> sequence = repository_->backpackIds(0);

  if (moveKind_ == MoveKind::ToWarehouse) {
    const QJsonObject pet = repository_->backpackPet(moveInstanceId_);
    if (pet.isEmpty() || !sequence.contains(moveInstanceId_)) {
      finishMove(false, QStringLiteral("实例 %1 已不在普通背包中。")
                            .arg(moveInstanceId_));
      return;
    }
    const QString restriction = PetMovePolicy::restriction(pet);
    if (!restriction.isEmpty()) {
      finishMove(false, QStringLiteral("不能将实例 %1 放入仓库：%2。")
                            .arg(moveInstanceId_).arg(restriction));
      return;
    }
    int nonRentCount = 0;
    for (qint64 id : sequence) {
      if (!repository_->backpackPet(id).value(QStringLiteral("isRentPet")).toBool())
        ++nonRentCount;
    }
    if (nonRentCount <= 1) {
      finishMove(false, QStringLiteral("至少要在背包保留一只非租借精灵。"));
      return;
    }
    preserveMoveDetail(moveInstanceId_, PetMovePolicy::remove(sequence, moveInstanceId_));
    return;
  }

  const QJsonObject incoming = repository_->warehousePet(moveInstanceId_);
  if (incoming.isEmpty()) {
    finishMove(false, QStringLiteral("实例 %1 已不在仓库中。").arg(moveInstanceId_));
    return;
  }
  const QString incomingRestriction = PetMovePolicy::restriction(incoming);
  if (!incomingRestriction.isEmpty()) {
    finishMove(false, QStringLiteral("不能将实例 %1 放入背包：%2。")
                          .arg(moveInstanceId_).arg(incomingRestriction));
    return;
  }
  const int capacity = repository_->backpackCapacity(0);
  if (capacity <= 0) {
    finishMove(false, QStringLiteral("服务器背包数据缺少有效容量 ppc，未发送移动请求。"));
    return;
  }
  if (sequence.size() < capacity) {
    sendMoveSequence(PetMovePolicy::append(sequence, moveInstanceId_));
    return;
  }

  const QList<qint64> eligible = eligibleReplacementIds();
  if (eligible.isEmpty()) {
    finishMove(false,
               QStringLiteral("背包已满，并且没有可安全替换的背包精灵。"));
    return;
  }
  movePhase_ = MovePhase::WaitingReplacement;
  emit statusChanged(QStringLiteral("背包已满，请选择一只背包精灵与实例 %1 交换。")
                         .arg(moveInstanceId_));
  publishReplacementSelection(eligible);
}

void PetRefreshController::chooseMoveReplacement(qint64 outgoingInstanceId) {
  if (movePhase_ != MovePhase::WaitingReplacement ||
      moveKind_ != MoveKind::ToBackpack)
    return;
  if (!movePreflightStillValid()) {
    emit statusChanged(QStringLiteral("选择期间列表或会话已变化，重新核对后再选择替换精灵。"));
    startMovePreflight();
    return;
  }
  const QList<qint64> eligible = eligibleReplacementIds();
  if (!eligible.contains(outgoingInstanceId)) {
    emit statusChanged(QStringLiteral("所选背包精灵当前不可替换，请重新选择。"));
    publishReplacementSelection(eligible);
    return;
  }
  moveReplacementId_ = outgoingInstanceId;
  const QList<qint64> replacement = PetMovePolicy::replace(
      repository_->backpackIds(0), outgoingInstanceId, moveInstanceId_);
  if (replacement.isEmpty()) {
    finishMove(false, QStringLiteral("生成背包交换序列失败，未发送移动请求。"));
    return;
  }
  preserveMoveDetail(outgoingInstanceId, replacement);
}

void PetRefreshController::cancelMove() {
  if (terminalNotificationGuard_) return;
  if (!moveRunning()) return;
  if (movePhase_ == MovePhase::WaitingDispatch) {
    const auto cancelled = moveSendPermit_.revoke();
    finishMove(cancelled == SubmissionOutcome::DefinitelyNotSubmitted ? MoveOutcome::NotSent : MoveOutcome::Unknown,
        cancelled == SubmissionOutcome::DefinitelyNotSubmitted
            ? QStringLiteral("尚未执行的GUI移动意图已撤销，未提交请求。")
            : QStringLiteral("GUI已领取移动意图，提交结果未知；保留原账号记录，不会重复提交。"));
    return;
  }
  if (movePhase_ == MovePhase::AwaitingWrite ||
      movePhase_ == MovePhase::Verification) {
    emit statusChanged(QStringLiteral("移动请求已经发送，不能取消；正在确认服务器最终状态。"));
    return;
  }
  finishMove(false, QStringLiteral("移动操作已取消，未发送写请求。"));
}

void PetRefreshController::preserveMoveDetail(qint64 instanceId,
                                              const QList<qint64>& sequence) {
  if (!movePreflightStillValid() || sequence.isEmpty()) {
    finishMove(MoveOutcome::NotSent, QStringLiteral("详情保存前列表或会话已失效，未提交移动请求。"));
    return;
  }
  moveTargetSequence_ = sequence;
  movePhase_ = MovePhase::WaitingForDetailPersistence;
  moveDetailPersistenceInstanceId_ = instanceId;
  moveDetailPersistenceTaskId_ = repository_->preserveBackpackDetailAsync(instanceId);
  if (!moveDetailPersistenceTaskId_) {
    finishMove(MoveOutcome::NotSent, QStringLiteral("必要详情无法进入存储队列，未提交移动请求。"));
    return;
  }
  moveTimeoutTimer_.start(timings_.moveRequestTimeoutMs);
  emit statusChanged(QStringLiteral("正在保存实例 %1 的必要详情；保存成功前不会提交移动请求……").arg(instanceId));
}

void PetRefreshController::onDetailPreservationFinished(quint64 taskId, qint64 instanceId,
                                                         bool saved, const QString& reason) {
  if (movePhase_ != MovePhase::WaitingForDetailPersistence || taskId != moveDetailPersistenceTaskId_ ||
      instanceId != moveDetailPersistenceInstanceId_) return;
  moveTimeoutTimer_.stop();
  moveDetailPersistenceTaskId_ = 0;
  if (!saved || !movePreflightStillValid()) {
    finishMove(MoveOutcome::NotSent, !saved
        ? QStringLiteral("必要详情未保存，未提交移动请求：%1").arg(reason)
        : QStringLiteral("详情保存期间列表或会话已变化，未提交移动请求。"));
    return;
  }
  sendMoveSequence(moveTargetSequence_);
}

void PetRefreshController::trackOperationWrite(const StorageSubmission& admission, bool intent) {
  if (!admission.accepted || !moveJournal_) return;
  operationWrites_.insert(admission.taskId,
      {moveJournal_->operationId(), moveAccount_, moveJournal_->path(), moveSessionGeneration_,
       moveJournal_->revision(), intent, moveJournal_->context()});
}

bool PetRefreshController::recordMoveOutcome(MoveOutcome outcome, const QString& reason) {
  if (!moveJournal_) return true;
  const StorageSubmission admission = moveJournal_->record(outcome, reason);
  trackOperationWrite(admission, false);
  return admission.accepted;
}

void PetRefreshController::onOperationStorageCompleted(const StorageResult& result) {
  const auto found = operationWrites_.find(result.taskId);
  if (found == operationWrites_.end()) return;
  const PendingOperationWrite pending = found.value();
  operationWrites_.erase(found);
  const bool matchesRecord = result.account == pending.account && result.absolutePath == pending.path &&
                             result.revision == pending.revision;
  const bool saved = matchesRecord && result.status == StorageStatus::Saved;
  const auto current = [this, &pending] {
    return repository_->accountKey() == pending.account && repository_->sessionGeneration() == pending.epoch &&
        repository_->sessionContext().canPersist() && repository_->storageContext() && pending.context &&
        repository_->storageContext()->id() == pending.context->id();
  };
  if (!saved && result.status != StorageStatus::Superseded)
    DiagnosticLogger::warning(QStringLiteral("move-storage"),
        QStringLiteral("operation=%1 revision=%2 account=%3 not saved: %4")
            .arg(pending.operationId).arg(pending.revision)
            .arg(DiagnosticLogger::maskedAccount(pending.account), result.error));
  if (current() && (!moveRunning() || (moveJournal_ && moveJournal_->operationId() == pending.operationId)))
    emit movePersistenceChanged(pending.operationId, pending.account, pending.epoch,
                                matchesRecord ? result.status : StorageStatus::InvalidRequest, result.error);
  // Every notification can synchronously cancel/switch accounts. Recheck the
  // exact task, intent revision and frozen storage context after notifying.
  if (!pending.intent || !current() || movePhase_ != MovePhase::WaitingForIntent ||
      result.taskId != moveIntentTaskId_ || !moveJournal_ ||
      moveJournal_->operationId() != pending.operationId || moveJournal_->revision() != pending.revision) return;
  moveTimeoutTimer_.stop();
  moveIntentTaskId_ = 0;
  moveSendPermit_ = SendPermit{};
  if (!saved || !movePreflightStillValid()) {
    finishMove(MoveOutcome::NotSent, !saved
        ? QStringLiteral("移动意图未保存，未提交请求：%1").arg(result.error)
        : QStringLiteral("意图保存期间列表或会话已失效，未提交请求。"));
    return;
  }
  submitPreparedMove();
}

void PetRefreshController::sendMoveSequence(const QList<qint64>& sequence) {
  if (!moveRunning() || sequence.isEmpty()) {
    finishMove(false, QStringLiteral("新的背包序列无效，未发送移动请求。"));
    return;
  }
  if (!movePreflightStillValid()) {
    finishMove(MoveOutcome::NotSent, QStringLiteral("写前列表或会话凭证已失效，未提交移动请求。"));
    return;
  }
  moveTargetSequence_ = sequence;
  movePhase_ = MovePhase::WaitingForIntent;
  moveJournal_ = std::make_unique<MoveOperationJournal>(repository_->storageService(), moveStorageContext_);
  const StorageSubmission admission = moveJournal_->begin(
      moveSessionGeneration_, movePreflightRevision_, moveInstanceId_, moveTargetSequence_);
  moveIntentTaskId_ = admission.taskId;
  trackOperationWrite(admission, true);
  if (!admission.accepted) {
    finishMove(MoveOutcome::NotSent,
        QStringLiteral("移动意图保存失败，未提交请求：%1").arg(moveJournal_->error()));
    return;
  }
  moveTimeoutTimer_.start(timings_.moveRequestTimeoutMs);
  emit statusChanged(QStringLiteral("正在保存移动意图；确认写入磁盘后才会调用宿主……"));
}

void PetRefreshController::submitPreparedMove() {
  if (movePhase_ != MovePhase::WaitingForIntent || !moveJournal_ || moveIntentTaskId_ != 0) return;
  if (!movePreflightStillValid()) {
    finishMove(MoveOutcome::NotSent, QStringLiteral("保存意图后会话或列表已失效，未提交请求。"));
    return;
  }
  const QString operationId = moveJournal_->operationId();
  const QList<qint64> sequence = moveTargetSequence_;
  const auto sameOperation = [this, &operationId]() {
    return moveRunning() && moveJournal_ && moveJournal_->operationId() == operationId;
  };
  moveTargetSequence_ = sequence;
  movePhase_ = asyncSender_ ? MovePhase::WaitingDispatch : MovePhase::AwaitingWrite;
  moveRequestGeneration_ = ++nextRequestGeneration_;
  repository_->expectSequenceUpdate(moveRequestGeneration_, moveAccount_,
                                    moveSessionGeneration_);
  const QJsonObject parameters{
      {QStringLiteral("pps"), PetMovePolicy::serializeSequence(sequence)},
      {QStringLiteral("ppt"), 0}};
  emit statusChanged(moveReplacementId_ > 0
                         ? QStringLiteral("正在交换仓库实例 %1 与背包实例 %2……")
                               .arg(moveInstanceId_).arg(moveReplacementId_)
                         : moveKind_ == MoveKind::ToWarehouse
                               ? QStringLiteral("正在将实例 %1 放入仓库……")
                                     .arg(moveInstanceId_)
                               : QStringLiteral("正在将实例 %1 放入背包……")
                                     .arg(moveInstanceId_));
  // Official SocketClient.batchpet -> PetDataService.newSequence registers the
  // 2_1_11 listener and dispatches PACK_SEQUENCE_UPDATE, which is what the
  // in-game backpack and warehouse panels actually refresh from.
  const QString sequenceText = PetMovePolicy::serializeSequence(sequence);
  // Every Qt signal may synchronously change the source or start another
  // operation. Validate the exact intent after notifications, at the call site.
  if (!sameOperation()) return;
  if (!movePreflightStillValid()) {
    finishMove(MoveOutcome::NotSent, QStringLiteral("提交前来源已失效，未调用宿主。"));
    return;
  }
  if (asyncSender_) {
    if (!queueDispatch(QStringLiteral("batchpet"), sequenceText, moveInstanceId_,
                         moveRequestGeneration_, DispatchKind::MoveFlash))
      finishMove(MoveOutcome::NotSent, QStringLiteral("GUI发送队列未接纳移动意图，未提交请求。"));
    return;
  }
  // Persisted intent already survives an exception/crash inside the host call.
  moveOutcome_ = MoveOutcome::Unknown;
  SubmissionOutcome submitted = SubmissionOutcome::DefinitelyNotSubmitted;
  if (writeFlashInvoker_)
    submitted = writeFlashInvoker_(QStringLiteral("batchpet"), sequenceText);
  else if (flashInvoker_)
    submitted = flashInvoker_(QStringLiteral("batchpet"), sequenceText)
        ? SubmissionOutcome::Submitted : SubmissionOutcome::Unknown;
  if (!sameOperation()) return;
  if (submitted != SubmissionOutcome::DefinitelyNotSubmitted)
    emit commandSent(QStringLiteral("batchpet"), moveInstanceId_,
                     moveRequestGeneration_);
  if (submitted == SubmissionOutcome::DefinitelyNotSubmitted && sameOperation()) {
    moveOutcome_ = MoveOutcome::NotSent;
    const QString payload = QString::fromUtf8(QJsonDocument(parameters).toJson(QJsonDocument::Compact));
    emit commandSent(QStringLiteral("2_1_11"), moveInstanceId_, moveRequestGeneration_);
    if (!sameOperation()) return;
    if (!movePreflightStillValid()) {
      finishMove(MoveOutcome::NotSent, QStringLiteral("备用发送前来源已失效，未调用宿主。"));
      return;
    }
    moveOutcome_ = MoveOutcome::Unknown;
    if (writeSender_)
      submitted = writeSender_(QStringLiteral("PJXExtension"), QStringLiteral("2_1_11"), payload);
    else if (sender_)
      submitted = sender_(QStringLiteral("PJXExtension"), QStringLiteral("2_1_11"), payload)
          ? SubmissionOutcome::Submitted : SubmissionOutcome::Unknown;
  }
  // A synchronous source change may have completed/reset this operation while
  // inside the adapter. Never touch the new session or start its timers.
  if (!sameOperation()) return;
  if (submitted == SubmissionOutcome::DefinitelyNotSubmitted) {
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
    finishMove(MoveOutcome::NotSent, QStringLiteral("宿主确认未提交移动请求。"));
    return;
  }
  moveOutcome_ = submitted == SubmissionOutcome::Submitted ? MoveOutcome::Submitted : MoveOutcome::Unknown;
  const bool queued = recordMoveOutcome(moveOutcome_);
  emit moveOutcomeChanged(moveJournal_->operationId(), moveAccount_, moveOutcome_,
      queued ? QStringLiteral("请求已交给宿主，状态已进入存储队列，等待只读核对。")
             : QStringLiteral("请求可能已提交；状态无法入队，原始意图仍保留待核对。"));
  if (sameOperation() && movePhase_ == MovePhase::AwaitingWrite)
    moveTimeoutTimer_.start(timings_.moveRequestTimeoutMs);
}

void PetRefreshController::finishMoveDispatch(const PendingDispatch& pending,
                                              const SendReceipt& receipt) {
  if (movePhase_ != MovePhase::WaitingDispatch || pending.generation != moveRequestGeneration_ ||
      !moveJournal_ || moveJournal_->operationId() != pending.operationId) return;
  if (receipt.outcome == SubmissionOutcome::DefinitelyNotSubmitted) {
    moveOutcome_ = MoveOutcome::NotSent;
    if (pending.kind == DispatchKind::MoveFlash && movePreflightStillValid() &&
        transportMonotonicMs() < pending.intent.ticket.dispatchDeadlineMs) {
      const QString parameters = QString::fromUtf8(QJsonDocument(QJsonObject{
          {QStringLiteral("pps"), PetMovePolicy::serializeSequence(moveTargetSequence_)},
          {QStringLiteral("ppt"), 0}}).toJson(QJsonDocument::Compact));
      if (queueDispatch(QStringLiteral("2_1_11"), parameters, moveInstanceId_, moveRequestGeneration_,
                          DispatchKind::MoveSocket, pending.intent.ticket.dispatchDeadlineMs)) return;
    }
    finishMove(MoveOutcome::NotSent, QStringLiteral("GUI确认未提交移动请求：%1").arg(receipt.error));
    return;
  }
  moveOutcome_ = receipt.outcome == SubmissionOutcome::Submitted ? MoveOutcome::Submitted : MoveOutcome::Unknown;
  recordMoveOutcome(moveOutcome_, receipt.error);
  if (receipt.dispatchedAtMs < 0) {
    finishMove(MoveOutcome::Unknown, QStringLiteral("移动意图已被领取，但实际提交时刻未确认；不会重发。"));
    return;
  }
  movePhase_ = MovePhase::AwaitingWrite;
  const QString operationId = pending.operationId;
  emit commandSent(receipt.command, moveInstanceId_, moveRequestGeneration_);
  if (!moveJournal_ || moveJournal_->operationId() != operationId || movePhase_ != MovePhase::AwaitingWrite) return;
  emit moveOutcomeChanged(operationId, moveAccount_, moveOutcome_,
                          QStringLiteral("GUI已执行移动意图，正在等待只读核对。"));
  if (!moveJournal_ || moveJournal_->operationId() != operationId || movePhase_ != MovePhase::AwaitingWrite) return;
  if (moveResponseBeforeReceipt_) {
    deferMoveVerification(moveRequestGeneration_, QStringLiteral("发送回执前已观察到服务器响应，正在只读核对……"));
    return;
  }
  const qint64 remaining = qMax<qint64>(0,
      receipt.dispatchedAtMs + pending.intent.ticket.responseTimeoutMs - transportMonotonicMs());
  if (remaining > 0) moveTimeoutTimer_.start(static_cast<int>(qMin<qint64>(remaining, std::numeric_limits<int>::max())));
  else {
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
    startMoveVerification(QStringLiteral("实际发送后的确认期限已过，正在只读核对；不会重复提交。"));
  }
}

bool PetRefreshController::movePreflightStillValid() const {
  return repository_->isAuthenticated() && repository_->accountKey() == moveAccount_ &&
      repository_->sessionGeneration() == moveSessionGeneration_ &&
      moveStorageContext_ && repository_->storageContext() &&
      moveStorageContext_->id() == repository_->storageContext()->id() &&
      repository_->inventoryRevision() == movePreflightRevision_ &&
      repository_->listObservationsAuthoritativeForWrite();
}

void PetRefreshController::onSequenceUpdateAccepted(quint64 requestGeneration) {
  if (movePhase_ == MovePhase::WaitingDispatch && requestGeneration == moveRequestGeneration_ &&
      moveSendPermit_.state() != SendPermitState::Pending && moveSendPermit_.state() != SendPermitState::Revoked) {
    moveResponseBeforeReceipt_ = true;
    return;
  }
  if (movePhase_ != MovePhase::AwaitingWrite ||
      requestGeneration != moveRequestGeneration_)
    return;
  moveTimeoutTimer_.stop();
  deferMoveVerification(
      requestGeneration,
      QStringLiteral("服务器已响应，正在刷新并核对移动结果……"));
}

void PetRefreshController::onSequenceUpdateRejected(quint64 requestGeneration,
                                                    const QString& reason) {
  if (movePhase_ == MovePhase::WaitingDispatch && requestGeneration == moveRequestGeneration_ &&
      moveSendPermit_.state() != SendPermitState::Pending && moveSendPermit_.state() != SendPermitState::Revoked) {
    moveResponseBeforeReceipt_ = true;
    moveWriteRejected_ = true;
    moveWriteFailureReason_ = reason;
    return;
  }
  if (movePhase_ != MovePhase::AwaitingWrite ||
      requestGeneration != moveRequestGeneration_)
    return;
  moveTimeoutTimer_.stop();
  moveWriteRejected_ = true;
  moveWriteFailureReason_ = reason;
  deferMoveVerification(
      requestGeneration,
      QStringLiteral("服务器拒绝移动：%1；正在重新读取列表……").arg(reason));
}

void PetRefreshController::deferMoveVerification(quint64 requestGeneration,
                                                  const QString& status) {
  // The sequence signal is emitted while the original client is still
  // dispatching the 2_1_11 response. Sending 2_1_10 re-entrantly from that
  // stack can be dropped by the original socket/event code, leaving the
  // extension tables stale until the whole game is refreshed.
  QTimer::singleShot(0, this, [this, requestGeneration, status]() {
    if (movePhase_ != MovePhase::AwaitingWrite ||
        requestGeneration != moveRequestGeneration_ ||
        !repository_->isAuthenticated() ||
        repository_->accountKey() != moveAccount_ ||
        repository_->sessionGeneration() != moveSessionGeneration_)
      return;
    startMoveVerification(status);
  });
}

void PetRefreshController::startMoveVerification(const QString& status) {
  if (!moveRunning()) return;
  movePhase_ = MovePhase::Verification;
  emit statusChanged(status);
  startListRefresh(true);
}

void PetRefreshController::finishMoveVerification(bool listsSucceeded) {
  if (movePhase_ != MovePhase::Verification) return;
  if (!listsSucceeded) {
    finishMove(false,
               QStringLiteral("移动请求可能已经执行，但列表刷新失败，结果暂时无法确认；请稍后手动刷新。"));
    return;
  }
  if (!repository_->listObservationsAuthoritativeForWrite()) {
    finishMove(MoveOutcome::Unknown, QStringLiteral("写后列表来源或观察顺序尚未确证，移动结果未知。"));
    return;
  }
  const QList<qint64> sequence = repository_->backpackIds(0);
  bool verified = false;
  if (moveKind_ == MoveKind::ToWarehouse) {
    verified = !sequence.contains(moveInstanceId_) &&
               !repository_->warehousePet(moveInstanceId_).isEmpty();
  } else if (moveReplacementId_ > 0) {
    verified = sequence.contains(moveInstanceId_) &&
               !sequence.contains(moveReplacementId_) &&
               repository_->warehousePet(moveInstanceId_).isEmpty() &&
               !repository_->warehousePet(moveReplacementId_).isEmpty();
  } else {
    verified = sequence.contains(moveInstanceId_) &&
               repository_->warehousePet(moveInstanceId_).isEmpty();
  }
  verified = verified && sequence == moveTargetSequence_;
  if (!verified) {
    if (moveWriteRejected_) {
      finishMove(MoveOutcome::Rejected, QStringLiteral("移动被拒绝：%1；已重新读取服务器列表。")
                            .arg(moveWriteFailureReason_));
      return;
    }
    finishMove(false,
               QStringLiteral("服务器最新列表与预期不一致，未将本地缓存伪装成成功状态。"));
    return;
  }

  const QString message =
      moveKind_ == MoveKind::ToWarehouse
          ? QStringLiteral("实例 %1 已进入仓库。")
                .arg(moveInstanceId_)
          : moveReplacementId_ > 0
                ? QStringLiteral("实例 %1 已进入背包，实例 %2 已进入仓库。")
                      .arg(moveInstanceId_).arg(moveReplacementId_)
                : QStringLiteral("实例 %1 已进入背包。")
                      .arg(moveInstanceId_);
  finishMove(true, message);
}

bool PetRefreshController::removeQueuedDetail(qint64 instanceId) {
  priorityQueue_.removeAll(instanceId);
  batchQueue_.removeAll(instanceId);
  queuedIds_.remove(instanceId);
  if (!batchIds_.remove(instanceId)) return false;
  ++batchCompleted_;
  ++batchSucceeded_;
  return true;
}

void PetRefreshController::finishMove(bool succeeded, const QString& message) {
  if (!succeeded && movePhase_ == MovePhase::WaitingDispatch &&
      moveSendPermit_.revoke() == SubmissionOutcome::Unknown) moveOutcome_ = MoveOutcome::Unknown;
  finishMove(succeeded ? MoveOutcome::Confirmed :
      moveOutcome_ == MoveOutcome::NotSent ? MoveOutcome::NotSent : MoveOutcome::Unknown, message);
}

void PetRefreshController::finishMove(MoveOutcome outcome, const QString& message) {
  if (!moveRunning()) return;
  QScopedValueRollback<bool> terminalGuard(terminalNotificationGuard_, true);
  const QString completedOperationId = moveJournal_ ? moveJournal_->operationId() : QString{};
  const QString completedAccount = moveAccount_;
  const bool succeeded = outcome == MoveOutcome::Confirmed;
  for (auto pending = pendingDispatches_.begin(); pending != pendingDispatches_.end();) {
    if (pending.value().operationId == completedOperationId && !completedOperationId.isEmpty()) {
      pending.value().intent.permit.revoke();
      pending = pendingDispatches_.erase(pending);
    } else ++pending;
  }
  if (pendingDispatches_.isEmpty()) dispatchReceiptTimer_.stop();
  lastMoveOutcome_ = outcome;
  QString finalMessage = message;
  if (!recordMoveOutcome(outcome, message))
    finalMessage += QStringLiteral("；状态无法进入存储队列，磁盘意图仍需核对");
  moveJournal_.reset();
  moveOutcome_ = MoveOutcome::NotSent;
  moveTimeoutTimer_.stop();
  if (moveRequestGeneration_ > 0)
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
  if (succeeded && moveKind_ == MoveKind::ToBackpack)
    removeQueuedDetail(moveInstanceId_);
  moveKind_ = MoveKind::None;
  movePhase_ = MovePhase::Idle;
  moveTaskId_ = 0;
  moveInstanceId_ = 0;
  moveReplacementId_ = 0;
  moveRequestGeneration_ = 0;
  moveAccount_.clear();
  moveSessionGeneration_ = 0;
  moveTargetSequence_.clear();
  moveStorageContext_.reset();
  moveDetailPersistenceTaskId_ = 0;
  moveDetailPersistenceInstanceId_ = 0;
  moveIntentTaskId_ = 0;
  moveWriteRejected_ = false;
  moveWriteFailureReason_.clear();
  restoreAfterMove();
  emit moveRunningChanged(false);
  emit moveOutcomeChanged(completedOperationId, completedAccount, outcome, finalMessage);
  emit moveFinished(succeeded, finalMessage);
  emit statusChanged(finalMessage);
}

void PetRefreshController::restoreAfterMove() {
  if (batchRunning_ && batchWasRunningBeforeMove_) {
    batchPaused_ = batchWasPausedBeforeMove_;
    emitDetailProgress();
    if (!batchPaused_) scheduleNextDetail(0);
  } else if (!listRunning_) {
    scheduleAutomaticRefresh();
  }
  batchWasRunningBeforeMove_ = false;
  batchWasPausedBeforeMove_ = false;
}

void PetRefreshController::clearDetailState() {
  priorityQueue_.clear();
  batchQueue_.clear();
  queuedIds_.clear();
  batchIds_.clear();
  batchRunning_ = false;
  batchPaused_ = false;
  batchTotal_ = 0;
  batchCompleted_ = 0;
  batchSucceeded_ = 0;
  batchFailed_ = 0;
  consecutiveBatchTimeouts_ = 0;
  currentDetailId_ = 0;
  currentDetailGeneration_ = 0;
  currentDetailRetries_ = 0;
  currentDetailFromBatch_ = false;
  currentDetailFailuresOnlyTimeouts_ = true;
  currentDetailAwaitingStorage_ = false;
  emitDetailProgress();
}
