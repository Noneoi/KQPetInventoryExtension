#include "pet_refresh_controller.h"

#include "pet_move_policy.h"
#include "pet_repository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QtGlobal>

#include <utility>

PetRefreshController::PetRefreshController(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {
  settingsPath_ = QDir(repository_->dataRoot()).filePath(QStringLiteral("settings.json"));
  loadTimings();
  for (QTimer* timer : {&automaticTimer_, &listGapTimer_, &backpackTimeoutTimer_,
                        &warehouseTimeoutTimer_, &detailTimer_, &detailTimeoutTimer_,
                        &moveTimeoutTimer_}) {
    timer->setSingleShot(true);
  }

  connect(&automaticTimer_, &QTimer::timeout, this, [this]() {
    if (!repository_->isAuthenticated()) return;
    if (batchRunning_) return;
    if (listRunning_ || currentDetailId_ > 0) {
      if (pendingList_ == PendingList::None) pendingList_ = PendingList::Automatic;
      return;
    }
    startListRefresh(false);
  });
  connect(&listGapTimer_, &QTimer::timeout, this,
          &PetRefreshController::sendWarehouseListRequest);
  connect(&backpackTimeoutTimer_, &QTimer::timeout, this, [this]() {
    repository_->cancelListPart(QStringLiteral("2_1_10"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_10"), false, QStringLiteral("背包请求超时"));
  });
  connect(&warehouseTimeoutTimer_, &QTimer::timeout, this, [this]() {
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
    repository_->cancelDetailRequest(currentDetailId_, currentDetailGeneration_);
    retryOrFinishCurrent(QStringLiteral("详情请求超时"));
  });
  connect(&moveTimeoutTimer_, &QTimer::timeout, this, [this]() {
    if (movePhase_ != MovePhase::AwaitingWrite) return;
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
  connect(repository_, &PetRepository::sequenceUpdateAccepted, this,
          &PetRefreshController::onSequenceUpdateAccepted);
  connect(repository_, &PetRepository::sequenceUpdateRejected, this,
          &PetRefreshController::onSequenceUpdateRejected);
  connect(repository_, &PetRepository::visualMismatchDetected, this,
          &PetRefreshController::requestSingleDetail);

  if (repository_->isAuthenticated())
    resetForAccount(repository_->accountKey(), repository_->sessionGeneration());
}

bool PetRefreshController::moveRunning() const {
  return movePhase_ != MovePhase::Idle;
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
  emit statusChanged(
      batchRunning_
          ? QStringLiteral("刷新参数已保存；仓库详情任务结束后再按 %1 秒重新计时自动刷新。")
                .arg(timings_.automaticIntervalMs / 1000.0, 0, 'f', 1)
          : QStringLiteral("刷新参数已保存；下一次自动刷新将在 %1 秒后执行。")
                .arg(timings_.automaticIntervalMs / 1000.0, 0, 'f', 1));
}

void PetRefreshController::loadTimings() {
  QFile file(settingsPath_);
  if (!file.open(QIODevice::ReadOnly)) return;
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return;
  const QJsonObject object = document.object();
  Timings loaded;
  loaded.automaticIntervalMs = object.value(QStringLiteral("automaticIntervalMs"))
                                   .toInt(loaded.automaticIntervalMs);
  loaded.listRequestGapMs = object.value(QStringLiteral("listRequestGapMs"))
                                .toInt(loaded.listRequestGapMs);
  loaded.listTimeoutMs = object.value(QStringLiteral("listTimeoutMs"))
                             .toInt(loaded.listTimeoutMs);
  loaded.detailRequestGapMs = object.value(QStringLiteral("detailRequestGapMs"))
                                  .toInt(loaded.detailRequestGapMs);
  loaded.detailBatchRestMs = object.value(QStringLiteral("detailBatchRestMs"))
                                  .toInt(loaded.detailBatchRestMs);
  loaded.detailTimeoutMs = object.value(QStringLiteral("detailTimeoutMs"))
                               .toInt(loaded.detailTimeoutMs);
  loaded.detailBatchSize = object.value(QStringLiteral("detailBatchSize"))
                               .toInt(loaded.detailBatchSize);
  loaded.detailMaxRetries = object.value(QStringLiteral("detailMaxRetries"))
                                  .toInt(loaded.detailMaxRetries);
  loaded.moveRequestTimeoutMs = object.value(QStringLiteral("moveRequestTimeoutMs"))
                                    .toInt(loaded.moveRequestTimeoutMs);
  timings_ = loaded;
  timings_.automaticIntervalMs = qMax(1, timings_.automaticIntervalMs);
  timings_.listRequestGapMs = qMax(0, timings_.listRequestGapMs);
  timings_.listTimeoutMs = qMax(1, timings_.listTimeoutMs);
  timings_.detailRequestGapMs = qMax(0, timings_.detailRequestGapMs);
  timings_.detailBatchRestMs = qMax(0, timings_.detailBatchRestMs);
  timings_.detailTimeoutMs = qMax(1, timings_.detailTimeoutMs);
  timings_.detailBatchSize = qMax(1, timings_.detailBatchSize);
  timings_.detailMaxRetries = qMax(0, timings_.detailMaxRetries);
  timings_.moveRequestTimeoutMs = qMax(1, timings_.moveRequestTimeoutMs);
}

void PetRefreshController::saveTimings() const {
  QDir().mkpath(QFileInfo(settingsPath_).absolutePath());
  QSaveFile file(settingsPath_);
  if (!file.open(QIODevice::WriteOnly)) return;
  const QJsonObject object{
      {QStringLiteral("schema"), 1},
      {QStringLiteral("automaticIntervalMs"), timings_.automaticIntervalMs},
      {QStringLiteral("listRequestGapMs"), timings_.listRequestGapMs},
      {QStringLiteral("listTimeoutMs"), timings_.listTimeoutMs},
      {QStringLiteral("detailRequestGapMs"), timings_.detailRequestGapMs},
      {QStringLiteral("detailBatchRestMs"), timings_.detailBatchRestMs},
      {QStringLiteral("detailTimeoutMs"), timings_.detailTimeoutMs},
      {QStringLiteral("detailBatchSize"), timings_.detailBatchSize},
      {QStringLiteral("detailMaxRetries"), timings_.detailMaxRetries},
      {QStringLiteral("moveRequestTimeoutMs"), timings_.moveRequestTimeoutMs}};
  file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
  file.commit();
}

bool PetRefreshController::send(const QString& command, const QString& parameters,
                                qint64 instanceId, quint64 requestGeneration) {
  emit commandSent(command, instanceId, requestGeneration);
  return sender_ && sender_(QStringLiteral("PJXExtension"), command, parameters);
}

void PetRefreshController::onAccountSessionChanged(const QString& account,
                                                    quint64 sessionGeneration) {
  resetForAccount(account, sessionGeneration);
}

void PetRefreshController::resetForAccount(const QString& account,
                                           quint64 sessionGeneration) {
  automaticTimer_.stop();
  listGapTimer_.stop();
  backpackTimeoutTimer_.stop();
  warehouseTimeoutTimer_.stop();
  detailTimer_.stop();
  detailTimeoutTimer_.stop();
  moveTimeoutTimer_.stop();
  const bool hadMove = moveRunning();
  if (moveRequestGeneration_ > 0)
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
  moveKind_ = MoveKind::None;
  movePhase_ = MovePhase::Idle;
  moveInstanceId_ = 0;
  moveReplacementId_ = 0;
  moveRequestGeneration_ = 0;
  moveTargetSequence_.clear();
  if (hadMove) emit moveRunningChanged(false);
  account_ = account;
  sessionGeneration_ = sessionGeneration;
  pendingList_ = PendingList::None;
  if (listRunning_) emit listRefreshRunningChanged(false);
  listRunning_ = false;
  clearDetailState();
  emit statusChanged(QStringLiteral("账号 %1 已就绪；60秒后自动刷新背包与仓库。")
                         .arg(account_));
  scheduleAutomaticRefresh();
}

void PetRefreshController::scheduleAutomaticRefresh() {
  automaticTimer_.stop();
  if (repository_->isAuthenticated() && !batchRunning_ && !moveRunning())
    automaticTimer_.start(timings_.automaticIntervalMs);
}

void PetRefreshController::requestManualListRefresh() {
  if (!repository_->isAuthenticated()) {
    emit statusChanged(QStringLiteral("尚未识别登录账号，不能刷新线上数据。"));
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
  if (sent) {
    backpackTimeoutTimer_.start(timings_.listTimeoutMs);
  } else {
    repository_->cancelListPart(QStringLiteral("2_1_10"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_10"), false, QStringLiteral("背包请求发送失败"));
  }
  listGapTimer_.start(timings_.listRequestGapMs);
}

void PetRefreshController::sendWarehouseListRequest() {
  if (!listRunning_ || warehouseSent_) return;
  warehouseSent_ = true;
  repository_->expectListPart(QStringLiteral("2_1_S"), listRequestGeneration_,
                              account_, sessionGeneration_);
  emit statusChanged(QStringLiteral("正在刷新仓库……"));
  const bool sent = send(QStringLiteral("2_1_S"), QStringLiteral("null"), 0,
                         listRequestGeneration_);
  if (sent) {
    warehouseTimeoutTimer_.start(timings_.listTimeoutMs);
  } else {
    repository_->cancelListPart(QStringLiteral("2_1_S"), listRequestGeneration_);
    finishListPart(QStringLiteral("2_1_S"), false, QStringLiteral("仓库请求发送失败"));
  }
}

void PetRefreshController::onListResponseAccepted(const QString& command,
                                                  quint64 requestGeneration) {
  if (!listRunning_ || requestGeneration != listRequestGeneration_) return;
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
  emit listRefreshRunningChanged(false);
  if (backpackSucceeded_ && warehouseSucceeded_) {
    emit statusChanged(QStringLiteral("背包/仓库列表刷新完成；下一次自动刷新在60秒后。"));
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
  if (!repository_->isAuthenticated()) {
    emit statusChanged(QStringLiteral("尚未识别登录账号，不能刷新仓库详情。"));
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
  const QList<qint64> ids = repository_->warehouseIdsByDetailAge();
  batchQueue_.clear();
  batchIds_.clear();
  for (qint64 id : ids) {
    if (id <= 0 || id == currentDetailId_) continue;
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
  emitDetailProgress();
  if (!batchRunning_) {
    emit statusChanged(QStringLiteral("当前仓库没有可刷新的精灵详情。"));
    return;
  }
  automaticTimer_.stop();
  emit statusChanged(QStringLiteral("仓库详情后台刷新已启动：共 %1 只，严格单请求串行。")
                         .arg(batchTotal_));
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
    const QString reason = QStringLiteral("账号未登录或会话尚未识别");
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
  } else if (batchRunning_ && !batchPaused_ && !batchQueue_.isEmpty()) {
    currentDetailId_ = batchQueue_.takeFirst();
  } else {
    finishDetailBatchIfDone();
    return;
  }
  currentDetailRetries_ = 0;
  emitDetailProgress();
  sendCurrentDetailAttempt();
}

void PetRefreshController::sendCurrentDetailAttempt() {
  if (currentDetailId_ <= 0 || !repository_->isAuthenticated()) return;
  currentDetailGeneration_ = ++nextRequestGeneration_;
  repository_->expectDetail(currentDetailId_, currentDetailGeneration_, account_,
                            sessionGeneration_);
  const QString parameters = QStringLiteral("{\"pi\":%1}").arg(currentDetailId_);
  const bool sent = send(QStringLiteral("2_1_R"), parameters, currentDetailId_,
                         currentDetailGeneration_);
  if (sent) {
    detailTimeoutTimer_.start(timings_.detailTimeoutMs);
  } else {
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
  retryOrFinishCurrent(reason);
}

void PetRefreshController::retryOrFinishCurrent(const QString& reason) {
  if (currentDetailId_ <= 0) return;
  if (currentDetailRetries_ < timings_.detailMaxRetries) {
    ++currentDetailRetries_;
    emit statusChanged(QStringLiteral("实例 %1 详情失败：%2；正在重试 %3/%4。")
                           .arg(currentDetailId_).arg(reason)
                           .arg(currentDetailRetries_).arg(timings_.detailMaxRetries));
    detailTimer_.start(timings_.detailRequestGapMs);
    return;
  }
  finishCurrentDetail(false, reason);
}

void PetRefreshController::finishCurrentDetail(bool succeeded, const QString& reason) {
  const qint64 finishedId = currentDetailId_;
  const bool countedForBatch = batchIds_.remove(finishedId);
  queuedIds_.remove(finishedId);
  currentDetailId_ = 0;
  currentDetailGeneration_ = 0;
  currentDetailRetries_ = 0;
  if (countedForBatch) {
    ++batchCompleted_;
    if (succeeded) ++batchSucceeded_;
    else ++batchFailed_;
  }
  if (succeeded) {
    emit statusChanged(QStringLiteral("实例 %1 的详情已写入当前账号本地缓存。")
                           .arg(finishedId));
  } else {
    emit statusChanged(QStringLiteral("实例 %1 详情更新失败：%2；旧缓存已保留。")
                           .arg(finishedId).arg(reason));
  }
  emit detailRequestFinished(finishedId, succeeded, reason);
  emitDetailProgress();

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
  emitDetailProgress();
  emit statusChanged(QStringLiteral("仓库详情刷新完成：成功 %1，失败 %2。")
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
  if (!repository_->isAuthenticated()) {
    emit statusChanged(QStringLiteral("尚未识别登录账号，不能移动精灵。"));
    return;
  }
  if (instanceId <= 0 || kind == MoveKind::None) return;
  if (moveRunning()) {
    emit statusChanged(QStringLiteral("已有精灵移动操作正在进行，请等待完成。"));
    return;
  }

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
    if (!repository_->preserveBackpackDetail(moveInstanceId_)) {
      finishMove(false, QStringLiteral("无法保存实例 %1 的完整详情，未执行入库。")
                            .arg(moveInstanceId_));
      return;
    }
    sendMoveSequence(PetMovePolicy::remove(sequence, moveInstanceId_));
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
  emit replacementRequired(moveInstanceId_, eligible);
}

void PetRefreshController::chooseMoveReplacement(qint64 outgoingInstanceId) {
  if (movePhase_ != MovePhase::WaitingReplacement ||
      moveKind_ != MoveKind::ToBackpack)
    return;
  const QList<qint64> eligible = eligibleReplacementIds();
  if (!eligible.contains(outgoingInstanceId)) {
    emit statusChanged(QStringLiteral("所选背包精灵当前不可替换，请重新选择。"));
    emit replacementRequired(moveInstanceId_, eligible);
    return;
  }
  if (!repository_->preserveBackpackDetail(outgoingInstanceId)) {
    finishMove(false, QStringLiteral("无法保存被替换精灵的完整详情，未执行交换。"));
    return;
  }
  moveReplacementId_ = outgoingInstanceId;
  const QList<qint64> replacement = PetMovePolicy::replace(
      repository_->backpackIds(0), outgoingInstanceId, moveInstanceId_);
  if (replacement.isEmpty()) {
    finishMove(false, QStringLiteral("生成背包交换序列失败，未发送移动请求。"));
    return;
  }
  sendMoveSequence(replacement);
}

void PetRefreshController::cancelMove() {
  if (!moveRunning()) return;
  if (movePhase_ == MovePhase::AwaitingWrite ||
      movePhase_ == MovePhase::Verification) {
    emit statusChanged(QStringLiteral("移动请求已经发送，不能取消；正在确认服务器最终状态。"));
    return;
  }
  finishMove(false, QStringLiteral("移动操作已取消，未发送写请求。"));
}

void PetRefreshController::sendMoveSequence(const QList<qint64>& sequence) {
  if (!moveRunning() || sequence.isEmpty()) {
    finishMove(false, QStringLiteral("新的背包序列无效，未发送移动请求。"));
    return;
  }
  moveTargetSequence_ = sequence;
  movePhase_ = MovePhase::AwaitingWrite;
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
  bool sent = false;
  if (flashInvoker_)
    sent = flashInvoker_(QStringLiteral("batchpet"), sequenceText);
  if (!sent)
    sent = send(
        QStringLiteral("2_1_11"),
        QString::fromUtf8(QJsonDocument(parameters).toJson(QJsonDocument::Compact)),
        moveInstanceId_, moveRequestGeneration_);
  else
    emit commandSent(QStringLiteral("batchpet"), moveInstanceId_,
                     moveRequestGeneration_);
  if (!sent) {
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
    finishMove(false, QStringLiteral("移动请求发送失败，原列表和缓存均未修改。"));
    return;
  }
  moveTimeoutTimer_.start(timings_.moveRequestTimeoutMs);
}

void PetRefreshController::onSequenceUpdateAccepted(quint64 requestGeneration) {
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
  if (!verified) {
    if (moveWriteRejected_) {
      finishMove(false, QStringLiteral("移动失败：%1；已重新读取服务器列表。")
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
  if (!moveRunning()) return;
  moveTimeoutTimer_.stop();
  if (moveRequestGeneration_ > 0)
    repository_->cancelSequenceUpdate(moveRequestGeneration_);
  if (succeeded && moveKind_ == MoveKind::ToBackpack)
    removeQueuedDetail(moveInstanceId_);
  moveKind_ = MoveKind::None;
  movePhase_ = MovePhase::Idle;
  moveInstanceId_ = 0;
  moveReplacementId_ = 0;
  moveRequestGeneration_ = 0;
  moveAccount_.clear();
  moveSessionGeneration_ = 0;
  moveTargetSequence_.clear();
  moveWriteRejected_ = false;
  moveWriteFailureReason_.clear();
  emit moveRunningChanged(false);
  emit moveFinished(succeeded, message);
  emit statusChanged(message);
  restoreAfterMove();
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
  currentDetailId_ = 0;
  currentDetailGeneration_ = 0;
  currentDetailRetries_ = 0;
  emitDetailProgress();
}
