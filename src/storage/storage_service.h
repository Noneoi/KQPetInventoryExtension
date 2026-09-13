#pragma once

#include "storage_types.h"

#include <QObject>
#include <QPointer>
#include <QThread>
#include <functional>
#include <memory>

namespace StorageInternal { struct Runtime; }

// Core-thread facade. The only I/O thread creates its own worker QObject and
// QTimer inside QThread::run; no controller/model is moved to another thread.
class StorageService final : public QObject {
  Q_OBJECT
public:
  // Explicit fault-injection seam. Called only on the I/O thread after path
  // and lock checks; production callers omit it to use strict QSaveFile.
  using Writer = std::function<StorageWriteAttempt(const QString&, const QByteArray&)>;

  explicit StorageService(const QString& resolvedDataRoot,
                          const StorageLimits& limits = {},
                          Writer writer = {}, QObject* parent = nullptr);
  ~StorageService() override;

  QString dataRoot() const;
  StorageContext createAccountContext(const QString& account,
                                      const QString& safeDirectoryName,
                                      QString* error = nullptr);
  // Shared writes use a short per-file QLockFile. This scope cannot address
  // accounts/, so shared capabilities cannot bypass account ownership.
  StorageContext createSharedContext();
  // Explicitly frozen legacy/read source. It never acquires write authority
  // and submitWrite rejects it, even if it points outside the current data root.
  StorageContext createReadOnlyContext(const QString& resolvedReadRoot);
  StorageSubmission submitWrite(const StoreWrite& write);
  StorageSubmission submitJsonWrite(const StoreJsonWrite& write);
  StorageSubmission submitRead(const StoreRead& read);
  StorageSubmission submitScan(const StoreScan& scan);
  void cancelScan(const QString& cursor);
  void cancelReads(const StorageContext& context);
  void prioritizeRead(quint64 taskId);
  // Core-thread admission only. At most two queued/executing bounded callbacks;
  // payload/image budgets remain with the caller. Always runs on the I/O loop.
  bool postAuxiliary(std::function<void(QObject* ioRoot)> job);
  StorageQueueState state() const;

  // Accepted writes drain; queued reads cancel. A timeout leaves the running
  // thread/runtime/leases alive to finish, without deleting an active QThread.
  // Returns true only after the worker has actually stopped. New work is then
  // rejected. The default matches the normal two-second shutdown budget.
  bool shutdown(unsigned long waitMilliseconds = 2000);

signals:
  void completed(const StorageResult& result);

private:
  std::shared_ptr<StorageInternal::Runtime> runtime_;
  QPointer<QThread> thread_;
};
