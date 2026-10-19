// Warehouse detail refresh: batched, pausable single-pet detail
// queries with retry, progress and per-pet refresh.
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
#include <algorithm>
#include <limits>

void PetRefreshController::initializeDetailTaskStorage() {
  detailTaskStorage_ = new ControllerCacheStorage(repository_->storageService(),
      QStringLiteral("detail-refresh-task.json"), 8 * 1024 * 1024, this);
  detailTaskStorage_->canWrite = [this] {
    return detailTaskWritable_ && !account_.isEmpty() && repository_->accountKey() == account_ &&
        repository_->sessionGeneration() == sessionGeneration_ && repository_->storageContext();
  };
  detailTaskStorage_->snapshot = [this] {
    QJsonArray pending;
    QList<qint64> ids = detailTaskPending_.keys();
    std::sort(ids.begin(), ids.end());
    for (const qint64 id : ids) pending.append(QJsonObject{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("savedAtBefore"), detailTaskPending_.value(id).toUTC().toString(Qt::ISODateWithMs)}});
    return QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("account"), account_},
                       {QStringLiteral("pending"), pending}};
  };
  detailTaskStorage_->loaded = [this](StorageStatus status, const QJsonObject& value, const QString& error) {
    // A corrupt or foreign checkpoint is never replaced silently.
    detailTaskWritable_ = status == StorageStatus::NotFound ||
        (status == StorageStatus::Loaded && value.value(QStringLiteral("schemaVersion")).toInt() == 1 &&
         value.value(QStringLiteral("account")).toString() == account_ && value.value(QStringLiteral("pending")).isArray());
    if (detailTaskWritable_ && status == StorageStatus::Loaded) {
      const auto rows = value.value(QStringLiteral("pending")).toArray();
      for (const auto& row : rows) {
        const auto entry = row.toObject();
        bool valid = false;
        const qint64 id = entry.value(QStringLiteral("id")).toString().toLongLong(&valid);
        if (!valid || id <= 0) { detailTaskWritable_ = false; break; }
        detailTaskPending_.insert(id, QDateTime::fromString(entry.value(QStringLiteral("savedAtBefore")).toString(), Qt::ISODateWithMs));
      }
      if (!detailTaskWritable_) detailTaskPending_.clear();
      else pruneCompletedDetailTask();
    }
    if (!detailTaskWritable_)
      emit statusChanged(QStringLiteral("未完成详情任务记录无法读取；本轮仍可手动读取，但不能保存续读进度：%1").arg(error));
    emitDetailProgress();
    const auto requested = std::exchange(deferredDetailIds_, {});
    if (!requested.isEmpty()) startWarehouseDetailRefreshForIds(requested);
  };
  detailTaskStorage_->changed = [this](const QString& account, quint64 epoch, quint64, StorageStatus status, const QString& error) {
    if (account == account_ && epoch == sessionGeneration_ &&
        status != StorageStatus::Queued && status != StorageStatus::Saved && status != StorageStatus::Superseded)
      emit statusChanged(QStringLiteral("详情续读进度尚未保存：%1").arg(error));
  };
}

void PetRefreshController::saveDetailTask() {
  if (detailTaskWritable_) detailTaskStorage_->save();
}

bool PetRefreshController::detailTaskLoading() const { return detailTaskStorage_ && detailTaskStorage_->loading(); }

void PetRefreshController::pruneCompletedDetailTask() {
  bool changed = false;
  for (auto it = detailTaskPending_.begin(); it != detailTaskPending_.end();) {
    const QDateTime saved = repository_->detailSavedAt(it.key());
    if (saved.isValid() && saved != it.value() && repository_->isDetailPersisted(it.key())) {
      incompleteDetailIds_.remove(it.key());
      it = detailTaskPending_.erase(it); changed = true;
    } else ++it;
  }
  if (changed) saveDetailTask();
}

void PetRefreshController::startWarehouseDetailRefresh() {
  startWarehouseDetailRefreshForIds(repository_->warehouseIdsByDetailAge());
}
void PetRefreshController::retryIncompleteDetails(bool includeMissing) {
  if (detailTaskStorage_->loading() || repository_->cacheLoading()) {
    emit statusChanged(QStringLiteral("正在读取本地任务和精灵列表，请稍后继续未完成详情。")); return;
  }
  pruneCompletedDetailTask();
  QList<qint64> ids;
  for (qint64 id : repository_->warehouseIdsByDetailAge())
    if (detailTaskPending_.contains(id) || incompleteDetailIds_.contains(id) || (includeMissing && !repository_->detailSavedAt(id).isValid())) ids.append(id);
  if (repository_->listObservationsAuthoritativeForWrite()) {
    const auto current = repository_->currentInstanceIds();
    const QSet<qint64> present(current.begin(), current.end());
    bool changed = false;
    for (auto it = detailTaskPending_.begin(); it != detailTaskPending_.end();)
      if (!present.contains(it.key())) { it = detailTaskPending_.erase(it); changed = true; } else ++it;
    if (changed) { saveDetailTask(); emitDetailProgress(); }
  }
  if (ids.isEmpty()) { emit statusChanged(QStringLiteral("没有需要补读的失败、取消或缺失详情")); return; }
  startWarehouseDetailRefreshForIds(ids);
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
  if (detailTaskStorage_->loading()) {
    deferredDetailIds_ = instanceIds;
    emit statusChanged(QStringLiteral("正在读取本地续读记录，随后启动已选择的详情任务……"));
    return;
  }
  const QSet<qint64> selectedIds(instanceIds.begin(), instanceIds.end());
  const QList<qint64> ids = repository_->warehouseIdsByDetailAge();
  batchQueue_.clear();
  batchIds_.clear();
  for (qint64 id : ids) {
    if (id <= 0 || id == currentDetailId_ || !selectedIds.contains(id)) continue;
    if (queuedIds_.contains(id)) {
      if (shopDetailOwners_.remove(id)) oneShotDetailIds_.remove(id);
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
  for (qint64 id : batchIds_)
    if (!detailTaskPending_.contains(id)) detailTaskPending_.insert(id, repository_->detailSavedAt(id));
  if (batchRunning_) saveDetailTask();
  emitDetailProgress();
  if (!batchRunning_) {
    emit statusChanged(QStringLiteral("当前仓库没有可刷新的精灵详情。"));
    return;
  }
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
  if (!batchRunning_) return;
  incompleteDetailIds_.unite(batchIds_);
  // Priority requests have another consumer. Cancel only this batch's work.
  for (qint64 id : batchQueue_) {
    queuedIds_.remove(id); oneShotDetailIds_.remove(id);
  }
  batchQueue_.clear(); batchIds_.clear();
  batchRunning_ = batchPaused_ = false;
  if (currentDetailFromBatch_) {
    repository_->cancelDetailRequest(currentDetailId_, currentDetailGeneration_);
    for (auto it = pendingDispatches_.begin(); it != pendingDispatches_.end();) {
      if (it->kind == DispatchKind::Detail && it->generation == currentDetailGeneration_) {
        it->intent.permit.revoke(); it = pendingDispatches_.erase(it);
      } else ++it;
    }
    queuedIds_.remove(currentDetailId_); oneShotDetailIds_.remove(currentDetailId_);
    currentDetailId_ = 0; currentDetailGeneration_ = 0; currentDetailRetries_ = 0;
    currentDetailFromBatch_ = currentDetailAwaitingStorage_ = false;
    detailTimeoutTimer_.stop();
  }
  if (currentDetailId_ <= 0) { detailTimer_.stop(); scheduleNextDetail(); }
  saveDetailTask(); emitDetailProgress();
  emit statusChanged(QStringLiteral("仓库详情任务已取消；未完成名单已保留，可稍后继续。其他详情读取继续执行。"));
}

void PetRefreshController::requestSingleDetail(qint64 instanceId) {
  if (instanceId <= 0) return;
  // An explicit detail task now shares any pending shop request.
  if (shopDetailOwners_.remove(instanceId) &&
      (currentDetailId_ == instanceId || queuedIds_.contains(instanceId)))
    oneShotDetailIds_.remove(instanceId);
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
    currentDetailFromBatch_ = false;
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

bool PetRefreshController::requestMissingDetailOnce(qint64 instanceId) {
  if (instanceId <= 0 || !repository_->isAuthenticated() || moveRunning() ||
      !repository_->currentInstanceIds().contains(instanceId)) return false;
  // Share any existing user request; only a newly admitted automatic read
  // changes retry behavior, so analysis cannot alter a manual refresh task.
  if (currentDetailId_ == instanceId || queuedIds_.contains(instanceId)) {
    if (currentDetailId_ == instanceId) currentDetailFromBatch_ = false;
    shopDetailOwners_.remove(instanceId);
    return true;
  }
  oneShotDetailIds_.insert(instanceId);
  requestSingleDetail(instanceId);
  return true;
}

bool PetRefreshController::requestShopDetailOnce(qint64 instanceId, quint64 visitToken) {
  if (instanceId <= 0 || !visitToken) return false;
  QString reason;
  if (!repository_->isAuthenticated()) reason = QStringLiteral("当前未在线，继续显示本地数据");
  else if (moveRunning()) reason = QStringLiteral("精灵移动正在进行，本次详情读取未发送");
  else if (!repository_->currentInstanceIds().contains(instanceId)) reason = QStringLiteral("实例已不在当前精灵列表中");
  if (!reason.isEmpty()) {
    emit detailRequestFinished(instanceId, false, reason);
    return false;
  }
  if (currentDetailId_ == instanceId || queuedIds_.contains(instanceId)) return true;
  for (auto it = shopDetailOwners_.begin(); it != shopDetailOwners_.end();) {
    if (it.key() != currentDetailId_ && !queuedIds_.contains(it.key())) it = shopDetailOwners_.erase(it);
    else ++it;
  }
  shopDetailOwners_.insert(instanceId, visitToken);
  oneShotDetailIds_.insert(instanceId);
  queuedIds_.insert(instanceId);
  priorityQueue_.append(instanceId);
  // Respect the same gap as the existing detail queue. A UI completion event
  // must not reset that gap to zero when admitting the next usable pet.
  if (currentDetailId_ <= 0 && !detailTimer_.isActive()) scheduleNextDetail();
  return true;
}

void PetRefreshController::cancelShopDetailRequests(quint64 visitToken) {
  if (!visitToken) return;
  QList<qint64> owned;
  for (auto it = shopDetailOwners_.cbegin(); it != shopDetailOwners_.cend(); ++it)
    if (it.value() == visitToken) owned.append(it.key());
  for (qint64 id : owned) {
    shopDetailOwners_.remove(id);
    if (batchIds_.contains(id)) continue; // A warehouse task also needs it.
    if (id == currentDetailId_) {
      // A dispatched read is allowed to finish. Only revoke an exclusively
      // owned intent that the transport confirms was never submitted.
      quint64 pendingTask = 0;
      for (auto it = pendingDispatches_.cbegin(); it != pendingDispatches_.cend(); ++it) {
        if (it->kind == DispatchKind::Detail && it->intent.ticket.entityId == id &&
            it->generation == currentDetailGeneration_ &&
            it->intent.permit.revoke() == SubmissionOutcome::DefinitelyNotSubmitted) {
          pendingTask = it.key(); break;
        }
      }
      if (pendingTask) {
        pendingDispatches_.remove(pendingTask);
        if (pendingDispatches_.isEmpty()) dispatchReceiptTimer_.stop();
        detailTimer_.stop(); detailTimeoutTimer_.stop();
        repository_->cancelDetailRequest(id, currentDetailGeneration_);
        finishCurrentDetail(false, QStringLiteral("已离开兑换项目，未发送的详情读取已取消"));
      }
      continue;
    }
    priorityQueue_.removeAll(id);
    queuedIds_.remove(id);
    oneShotDetailIds_.remove(id);
  }
  if (currentDetailId_ <= 0) {
    if (priorityQueue_.isEmpty() && (!batchRunning_ || batchPaused_ || batchQueue_.isEmpty())) detailTimer_.stop();
    else if (!detailTimer_.isActive()) scheduleNextDetail();
  }
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

void PetRefreshController::retryOrFinishCurrent(const QString& reason, DetailFailure failure) {
  if (currentDetailId_ <= 0) return;
  if (failure != DetailFailure::ResponseTimeout) currentDetailFailuresOnlyTimeouts_ = false;
  if (!oneShotDetailIds_.contains(currentDetailId_) && currentDetailRetries_ < timings_.detailMaxRetries) {
    ++currentDetailRetries_;
    emit statusChanged(QStringLiteral("实例 %1 详情失败：%2；正在重试 %3/%4。")
                           .arg(currentDetailId_).arg(reason)
                           .arg(currentDetailRetries_).arg(timings_.detailMaxRetries));
    detailTimer_.start(timings_.detailRequestGapMs);
    return;
  }
  finishCurrentDetail(false, reason, failure);
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

bool PetRefreshController::removeQueuedDetail(qint64 instanceId) {
  shopDetailOwners_.remove(instanceId);
  oneShotDetailIds_.remove(instanceId);
  priorityQueue_.removeAll(instanceId);
  batchQueue_.removeAll(instanceId);
  queuedIds_.remove(instanceId);
  if (!batchIds_.remove(instanceId)) return false;
  ++batchCompleted_;
  ++batchSucceeded_;
  return true;
}

void PetRefreshController::clearDetailState() {
  priorityQueue_.clear();
  batchQueue_.clear();
  queuedIds_.clear();
  oneShotDetailIds_.clear();
  shopDetailOwners_.clear();
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
