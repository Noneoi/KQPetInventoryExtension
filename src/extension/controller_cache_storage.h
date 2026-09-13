#pragma once

#include "protocol_transport.h"
#include "storage_service.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <functional>

// Core-owned transport for the small controller JSON records. At most one
// not-yet-admitted update is retained; StorageService owns all admitted bytes.
// This is neither another repository nor an I/O executor.
class ControllerCacheStorage final : public QObject {
public:
  using Loaded = std::function<void(StorageStatus, const QJsonObject&, const QString&)>;
  using Changed = std::function<void(const QString&, quint64, quint64, StorageStatus, const QString&)>;
  ControllerCacheStorage(StorageService* storage, QString path, qint64 maximumBytes,
                         QObject* parent = nullptr)
      : QObject(parent), storage_(storage), path_(std::move(path)), maximumBytes_(maximumBytes) {
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, [this] { pump(); });
    if (storage_) connect(storage_, &StorageService::completed, this,
                          [this](const StorageResult& result) { receive(result); });
  }
  ~ControllerCacheStorage() override {
    if (storage_) storage_->cancelReads(readContext_);
  }

  Loaded loaded;
  Changed changed;
  std::function<QJsonObject()> snapshot;
  std::function<bool()> canWrite;
  std::function<void()> stateChanged;
  bool mergeJsonObject = false;

  bool loading() const { return loading_; }
  bool pendingWrite() const { return dirty_ || !writes_.isEmpty(); }
  int pendingWriteCount() const { return writes_.size() + (dirty_ ? 1 : 0); }
  int pendingCount() const { return reads_.size() + writes_.size() + (readWanted_ ? 1 : 0) + (dirty_ ? 1 : 0); }
  QString error() const { return error_; }

  void start(const StorageContext& context, const QString& account, quint64 epoch) {
    retry_.stop();
    if (storage_) storage_->cancelReads(readContext_);
    // Previously admitted writes keep their own frozen context and complete.
    // An unadmitted update never reported Queued and cannot change account.
    const bool abandoned = dirty_;
    const QString previousAccount = account_;
    const quint64 previousEpoch = epoch_;
    dirty_ = false;
    ++generation_;
    context_ = context;
    account_ = account;
    epoch_ = epoch;
    error_.clear();
    readContext_.reset();
    if (storage_ && context_) readContext_ = context_->isShared()
        ? storage_->createSharedContext()
        : storage_->createAccountContext(account, QFileInfo(context_->directory()).fileName());
    loading_ = readWanted_ = static_cast<bool>(readContext_);
    const QPointer<ControllerCacheStorage> guard(this);
    const quint64 started = generation_;
    if (abandoned && changed)
      changed(previousAccount, previousEpoch, 0, StorageStatus::Cancelled,
              QStringLiteral("会话已切换，上一账号尚未入队的缓存更新未保存"));
    if (!guard || started != generation_ || !notifyState()) return;
    if (started != generation_) return;
    if (readWanted_) retry_.start(0);
  }

  void save() {
    if (!context_ || !storage_ || !canWrite || !canWrite()) {
      error_ = QStringLiteral("当前会话或存储上下文不允许保存");
      if (changed) changed(account_, epoch_, 0, StorageStatus::InvalidRequest, error_);
      return;
    }
    dirty_ = true;
    error_.clear();
    if (!notifyState()) return;
    // Freeze the value after a pending read has merged historical groups.
    // Queued is emitted only after this complete immutable record is admitted.
    if (!loading_) pump();
  }

private:
  struct Pending {
    StorageContext context;
    QString account;
    quint64 epoch = 0;
    quint64 generation = 0;
    quint64 revision = 0;
  };
  bool notifyState() {
    const QPointer<ControllerCacheStorage> guard(this);
    if (stateChanged) stateChanged();
    return static_cast<bool>(guard);
  }
  void pump() {
    if (!storage_) return;
    if (readWanted_) {
      const auto result = storage_->submitRead({readContext_, path_, generation_, maximumBytes_, true});
      if (!result.accepted) {
        if (result.status == StorageStatus::QueueFull) { retry_.start(10); return; }
        readWanted_ = loading_ = false;
        error_ = result.error;
        const QPointer<ControllerCacheStorage> guard(this);
        const quint64 started = generation_;
        if (loaded) loaded(result.status, {}, error_);
        if (!guard || started != generation_ || !notifyState()) return;
        if (started != generation_) return;
      } else {
        reads_.insert(result.taskId, {readContext_, account_, epoch_, generation_, generation_});
        readWanted_ = false;
      }
    }
    if (loading_ || !dirty_) return;
    if (!canWrite || !canWrite()) {
      dirty_ = false;
      error_ = QStringLiteral("保存前会话已失效，未提交缓存写入");
      const QPointer<ControllerCacheStorage> guard(this);
      if (changed) changed(account_, epoch_, 0, StorageStatus::Cancelled, error_);
      if (guard) notifyState();
      return;
    }
    // All revisions come from a process-wide monotonic allocator, including
    // replacement controller instances that share one service/path.
    const quint64 revision = nextTransportTaskId();
    StoreJsonWrite write{context_, path_, revision, snapshot ? snapshot() : QJsonObject{}, maximumBytes_, true};
    write.mergeJsonObject = mergeJsonObject;
    const auto result = storage_->submitJsonWrite(write);
    if (!result.accepted && result.status == StorageStatus::QueueFull) { retry_.start(10); return; }
    dirty_ = false;
    error_ = result.error;
    if (result.accepted) writes_.insert(result.taskId, {context_, account_, epoch_, generation_, revision});
    const QPointer<ControllerCacheStorage> guard(this);
    if (changed) changed(account_, epoch_, revision, result.status, result.error);
    if (guard) notifyState();
  }
  void receive(const StorageResult& result) {
    auto read = reads_.find(result.taskId);
    if (read != reads_.end()) {
      const Pending task = read.value();
      reads_.erase(read);
      if (task.generation != generation_ || task.account != account_ || task.epoch != epoch_) {
        notifyState();
        return;
      }
      loading_ = false;
      error_ = result.error;
      QJsonParseError parseError;
      const QJsonDocument document = QJsonDocument::fromJson(result.content, &parseError);
      StorageStatus status = result.status;
      if (status == StorageStatus::Loaded && (parseError.error != QJsonParseError::NoError || !document.isObject())) {
        status = StorageStatus::ReadFailed;
        error_ = QStringLiteral("缓存JSON格式无效");
      }
      const QPointer<ControllerCacheStorage> guard(this);
      if (loaded) loaded(status, document.object(), error_);
      if (!guard || task.generation != generation_) return;
      if (!notifyState()) return;
      if (dirty_) pump();
      return;
    }
    auto write = writes_.find(result.taskId);
    if (write == writes_.end()) return;
    const Pending task = write.value();
    writes_.erase(write);
    const bool current = task.generation == generation_ && task.account == account_ && task.epoch == epoch_;
    // Completion metadata may be observed for auditing across account switches;
    // controller callbacks filter it before touching current UI state.
    if (current && result.status != StorageStatus::Superseded) error_ = result.error;
    const QPointer<ControllerCacheStorage> guard(this);
    if (changed) changed(task.account, task.epoch, task.revision, result.status, result.error);
    if (!guard) return;
    if (!notifyState()) return;
    if (dirty_ || readWanted_) retry_.start(0);
  }

  QPointer<StorageService> storage_;
  QString path_;
  qint64 maximumBytes_;
  QTimer retry_;
  StorageContext context_, readContext_;
  QString account_, error_;
  quint64 epoch_ = 0, generation_ = 0;
  bool loading_ = false, readWanted_ = false, dirty_ = false;
  QHash<quint64, Pending> reads_, writes_;
};
