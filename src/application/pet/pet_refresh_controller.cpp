#include "pet_refresh_controller.h"

#include "diagnostics/diagnostic_logger.h"
#include "application/common/controller_cache_storage.h"
#include "domain/pet_move_policy.h"
#include "protocol/packet_contract.h"
#include "pet_repository.h"
#include "storage/storage_service.h"

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
  for (QTimer* timer : {&listGapTimer_, &backpackTimeoutTimer_,
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
    if (valid) timings_ = loaded;
    else emit statusChanged(QStringLiteral("刷新参数读取失败，本次使用内置默认值：%1").arg(error.isEmpty() ? QStringLiteral("设置字段类型或数值无效") : error));
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
  scheduleNextDetail(0);
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
