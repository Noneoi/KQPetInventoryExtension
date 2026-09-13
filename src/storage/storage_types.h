#pragma once

#include "storage_write_context.h"

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QJsonObject>
#include <QString>
#include <QStringList>

enum class StorageStatus {
  Queued, Saved, Loaded, Scanned, Superseded, Cancelled, QueueFull, InvalidRequest,
  PathRejected, LockUnavailable, WriteFailed, ReadFailed, NotFound, Closing
};

struct StorageLimits {
  int maximumOutstandingTasks = 256;
  // Headroom for a complete 10k-instance snapshot's retained JSON and encoding.
  qint64 maximumOutstandingBytes = 128 * 1024 * 1024;
  qint64 maximumRecordBytes = 16 * 1024 * 1024;
  int maximumBatchTasks = 8;
  qint64 maximumBatchBytes = 1024 * 1024;
  int maximumBatchMilliseconds = 8;
  int idleLeaseCheckMilliseconds = 100;
  int maximumOpenScans = 8;
};

// submitWrite copies this value. Later caller edits to strings/bytes cannot
// change the enqueued account, absolute target, revision or content.
struct StoreWrite {
  StorageContext context;
  QString relativePath;
  quint64 revision = 0;
  QByteArray content;
  bool required = false;
  bool onlyIfMissing = false;
  bool mergeJsonObject = false;
  qint64 mergeMaximumBytes = 64 * 1024;
};

// The implicitly shared value is frozen at admission. JSON encoding, hashing,
// optional merge and atomic replacement all execute on the one I/O thread.
struct StoreJsonWrite {
  StorageContext context;
  QString relativePath;
  quint64 revision = 0;
  QJsonObject object;
  qint64 maximumBytes = 1024 * 1024;
  bool required = false;
  bool onlyIfMissing = false;
  bool mergeJsonObject = false;
};

struct StoreRead {
  StorageContext context;
  QString relativePath;
  quint64 expectedMemoryRevision = 0;
  qint64 maximumBytes = 256 * 1024;
  bool selected = false;
};

struct StoreScan {
  StorageContext context;
  QString relativeDirectory;
  QStringList nameFilters{QStringLiteral("*.json")};
  quint64 expectedMemoryRevision = 0;
  QString cursor;
  int maximumEntriesPerPage = 64;
};

struct StorageSubmission {
  quint64 taskId = 0;
  bool accepted = false;
  StorageStatus status = StorageStatus::InvalidRequest;
  QString error;
};

struct StorageResult {
  quint64 taskId = 0;
  QString account;
  QString absolutePath;
  // For reads this is the caller's expectedMemoryRevision, not a disk clock.
  // Core must compare it with the live record revision before applying bytes.
  quint64 revision = 0;
  StorageStatus status = StorageStatus::InvalidRequest;
  QByteArray content;
  QByteArray contentDigest;
  qint64 bytes = 0;
  QString error;
  QDateTime modifiedAt;
  QStringList relativeNames;
  QString scanCursor;
  bool hasMore = false;
  // File path checks, open/stat/read/close on I/O; excludes hashing and queue wait.
  qint64 readElapsedNanoseconds = 0;
  // Accepted queue residence until I/O dispatch; excludes the Core callback delay.
  qint64 queueWaitNanoseconds = 0;
};

struct StorageWriteAttempt {
  bool saved = false;
  qint64 bytes = 0;
  QString error;
};

struct StorageQueueState {
  int outstandingTasks = 0;
  qint64 outstandingBytes = 0;
  int queuedTasks = 0;
  bool closing = false;
  quintptr ioThreadId = 0;
};

Q_DECLARE_METATYPE(StorageStatus)
Q_DECLARE_METATYPE(StorageResult)
