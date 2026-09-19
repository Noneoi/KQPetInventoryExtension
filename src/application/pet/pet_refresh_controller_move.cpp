// Backpack/warehouse move: preflight, replacement choice, the single
// write submission, journaled outcome and read-only verification.
// Part of the pet_refresh_controller implementation; see pet_refresh_controller.cpp for the rest.

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
        (repository_->sessionContext().canPersist() || repository_->readContinuityWriteAllowed()) &&
        repository_->storageContext() && pending.context &&
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
    if (sender_)
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
                ? QStringLiteral("实例 %1 已放入背包，实例 %2 已放入仓库。")
                      .arg(moveInstanceId_).arg(moveReplacementId_)
                : QStringLiteral("实例 %1 已放入背包。")
                      .arg(moveInstanceId_);
  finishMove(true, message);
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
  }
  batchWasRunningBeforeMove_ = false;
  batchWasPausedBeforeMove_ = false;
}
