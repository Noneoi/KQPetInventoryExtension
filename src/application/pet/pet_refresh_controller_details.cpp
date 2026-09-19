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
#include <limits>

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
