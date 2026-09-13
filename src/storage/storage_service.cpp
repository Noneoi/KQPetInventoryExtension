#include "storage_service.h"

#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTimer>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace StorageInternal {

struct Job {
  quint64 taskId = 0;
  QElapsedTimer queueClock;
  StorageContext context;
  QString path;
  quint64 revision = 0;
  QByteArray content;
  QJsonObject jsonObject;
  bool encodeJson = false;
  qint64 reservedBytes = 0;
  qint64 maximumContentBytes = 0;
  qint64 extraMemoryBytes = 0;
  bool write = false;
  bool onlyIfMissing = false;
  bool mergeJsonObject = false;
  bool scan = false;
  QString scanCursor;
  QString relativeDirectory;
  QStringList nameFilters;
  int maximumEntriesPerPage = 64;
  std::function<void(QObject*)> auxiliary;
  qint64 memoryReservation() const { return reservedBytes + extraMemoryBytes; }
};

struct Runtime {
  QMutex mutex;
  QString dataRoot;
  StorageLimits limits;
  StorageService::Writer writer;
  std::deque<Job> requiredWrites;
  std::deque<Job> selectedReads;
  std::deque<Job> normalWrites;
  std::deque<Job> backgroundReads;
  std::deque<Job> auxiliary;
  int auxiliaryOutstanding = 0;
  QHash<QString, quint64> latestAccepted;
  QHash<QString, QString> accountNames;
  QHash<QString, std::weak_ptr<const StorageWriteContext>> contexts;
  QSet<QString> liveScans;
  QSet<QString> cancelledScans;
  QSet<QString> cancelledReadContexts;
  QObject* worker = nullptr;              // Protected by mutex through invokeMethod.
  StorageService* receiver = nullptr;    // Cleared under mutex before facade destruction.
  quint64 nextTaskId = 0;
  int outstandingTasks = 0;              // Includes completions queued back to Core.
  qint64 outstandingBytes = 0;           // Includes read-buffer reservations.
  bool closing = false;
  quintptr ioThreadId = 0;
};

namespace {
// Conservative logical allocation budget, not sizeof(QJsonObject), which only
// measures the small shared handle. Count container/element slack and string
// storage separately from the worst-case escaped output and its growth buffer.
// Depth and byte limits also bound admission traversal on Core.
bool jsonBudget(const QJsonValue& value, qint64 limit, qint64* retained,
                qint64* encoded, int depth = 0) {
  if (depth > 64) return false;
  const auto add = [limit](qint64* total, qint64 amount) {
    if (amount < 0 || amount > limit - *total) return false;
    *total += amount;
    return true;
  };
  if (!add(retained, 256) || !add(encoded, 32)) return false;
  if (value.isString()) {
    const qint64 units = value.toString().size();
    return units <= limit / 6 && add(retained, units * 4) && add(encoded, units * 6);
  }
  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const auto& entry : array)
      if (!jsonBudget(entry, limit, retained, encoded, depth + 1)) return false;
  } else if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
      const qint64 units = entry.key().size();
      if (units > limit / 6 || !add(retained, 128 + units * 4) ||
          !add(encoded, 4 + units * 6) ||
          !jsonBudget(entry.value(), limit, retained, encoded, depth + 1)) return false;
    }
  }
  return true;
}

Qt::CaseSensitivity pathCase() {
#ifdef Q_OS_WIN
  return Qt::CaseInsensitive;
#else
  return Qt::CaseSensitive;
#endif
}

QString cleanPath(const QString& path) {
  return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString pathKey(const QString& path) {
#ifdef Q_OS_WIN
  return path.toCaseFolded();
#else
  return path;
#endif
}

bool safeRelative(const QString& input, QString* output) {
  const QString relative = QDir::fromNativeSeparators(input);
  if (relative.isEmpty() || relative.size() > 32767 || QDir::isAbsolutePath(relative) || relative.contains(QLatin1Char(':')) ||
      relative.contains(QChar::Null)) return false;
  const QStringList parts = relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
  for (const QString& part : parts) {
    if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") ||
        part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) return false;
    if (part.endsWith(QStringLiteral(".lock"), pathCase())) return false;
#ifdef Q_OS_WIN
    // Reject Windows device names and wildcard/stream syntax before any I/O.
    const QString base = part.section(QLatin1Char('.'), 0, 0).toUpper();
    if (base == QStringLiteral("CON") || base == QStringLiteral("PRN") ||
        base == QStringLiteral("AUX") || base == QStringLiteral("NUL") ||
        (base.size() == 4 && (base.startsWith(QStringLiteral("COM")) ||
                            base.startsWith(QStringLiteral("LPT"))) &&
         base.at(3) >= QLatin1Char('1') && base.at(3) <= QLatin1Char('9'))) return false;
    for (QChar character : part)
      if (character.unicode() < 32 || QStringLiteral("<>\"|?*").contains(character)) return false;
#endif
  }
  *output = relative;
  return true;
}

bool isInside(const QString& root, const QString& path) {
  const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
  return path.startsWith(prefix, pathCase());
}

bool existingPathHasReparse(const QString& path) {
  QString current = cleanPath(path);
  for (;;) {
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#else
    if (QFileInfo(current).isSymLink()) return true;
#endif
    const QString parent = cleanPath(QFileInfo(current).absolutePath());
    if (parent == current) break;
    current = parent;
  }
  return false;
}

bool preparePath(const Job& job, QString* error) {
  const QString root = job.context->dataRoot();
  if (!isInside(job.context->directory(), job.path) || existingPathHasReparse(job.path)) {
    *error = QStringLiteral("target or parent is outside the fixed scope or is a reparse path");
    return false;
  }
  if (job.write && !QDir().mkpath(QFileInfo(job.path).absolutePath())) {
    *error = QStringLiteral("cannot create target directory");
    return false;
  }
  const QString canonicalRoot = cleanPath(QFileInfo(root).canonicalFilePath());
  const QString canonicalParent = cleanPath(QFileInfo(QFileInfo(job.path).absolutePath()).canonicalFilePath());
  if (canonicalRoot.isEmpty() || canonicalRoot == QStringLiteral(".") ||
      canonicalRoot.compare(root, pathCase()) != 0 ||
      (canonicalParent.compare(root, pathCase()) != 0 && !isInside(root, canonicalParent)) ||
      existingPathHasReparse(job.path)) {
    *error = QStringLiteral("canonical target parent escaped the fixed data root");
    return false;
  }
  return true;
}

bool schemaNumber(const QJsonValue& value, qint64* result) {
  if (value.isDouble()) {
    const qint64 number = value.toInteger(-1);
    if (number <= 0 || number > std::numeric_limits<int>::max() || value.toDouble() != static_cast<double>(number)) return false;
    *result = number;
    return true;
  }
  if (!value.isString() || value.toString().isEmpty()) return false;
  const QString text = value.toString();
  for (QChar digit : text) if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) return false;
  bool ok = false;
  const qint64 number = text.toLongLong(&ok);
  if (!ok || number <= 0 || number > std::numeric_limits<int>::max()) return false;
  *result = number;
  return true;
}

StorageWriteAttempt atomicWrite(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return {false, 0, file.errorString()};
  const qint64 written = file.write(bytes);
  if (written != bytes.size()) {
    const QString error = file.errorString();
    file.cancelWriting();
    return {false, written, error.isEmpty() ? QStringLiteral("short write") : error};
  }
  if (!file.commit()) return {false, written, file.errorString()};
  return {true, written, {}};
}

int queuedCount(const Runtime& state) {
  return static_cast<int>(state.requiredWrites.size() + state.selectedReads.size() +
                          state.normalWrites.size() + state.backgroundReads.size() + state.auxiliary.size());
}

void wakeLocked(const std::shared_ptr<Runtime>& state) {
  if (!state->worker) return;
  QObject* worker = state->worker;
  QMetaObject::invokeMethod(worker, [worker] {
    // Wake-ups only target a live worker under Runtime::mutex. QObject's
    // event destruction removes a queued functor if the worker exits first.
    QMetaObject::invokeMethod(worker, "wake", Qt::DirectConnection);
  }, Qt::QueuedConnection);
}

void deliverResult(const std::shared_ptr<Runtime>& state, const Job& job, StorageResult result) {
  QMutexLocker guard(&state->mutex);
  if (!state->receiver) {
    --state->outstandingTasks;
    state->outstandingBytes -= job.memoryReservation();
    return;
  }
  StorageService* receiver = state->receiver;
  const qint64 reservation = job.memoryReservation();
  QMetaObject::invokeMethod(receiver, [state, receiver, reservation, result = std::move(result)] {
    {
      QMutexLocker guard(&state->mutex);
      --state->outstandingTasks;
      state->outstandingBytes -= reservation;
    }
    emit receiver->completed(result);
  }, Qt::QueuedConnection);
}

struct Lease {
  std::unique_ptr<QLockFile> lock;
};

struct DirectoryCursor {
  StorageContext context;
  QString path;
  QString relativeDirectory;
  QStringList filters;
  std::unique_ptr<QDirIterator> iterator;
};

class Worker final : public QObject {
  Q_OBJECT
public:
  explicit Worker(std::shared_ptr<Runtime> state) : state_(std::move(state)), timer_(this) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &Worker::processBatch);
  }

public slots:
  void wake() {
    if (!timer_.isActive() || timer_.remainingTime() > 0) timer_.start(0);
  }

private:
  bool ensureLease(const StorageContext& context, QString* error) {
    if (context->isShared()) return true;
    const QString key = pathKey(context->directory());
    const auto existing = leases_.value(key);
    if (existing) return true;
    auto lease = std::make_shared<Lease>();
    lease->lock = std::make_unique<QLockFile>(QDir(context->directory()).filePath(QStringLiteral(".storage-writer.lock")));
    lease->lock->setStaleLockTime(0);
    if (!lease->lock->tryLock(0)) {
      *error = QStringLiteral("account directory is already owned by another writer");
      return false;
    }
    leases_.insert(key, std::move(lease));
    return true;
  }

  void pruneLeases() {
    QSet<QString> active;
    {
      QMutexLocker guard(&state_->mutex);
      // Include contexts whose first job is queued, not only those already
      // encountered by ensureLease. Copy the decision under the queue lock.
      for (auto context = state_->contexts.cbegin(); context != state_->contexts.cend(); ++context) {
        const auto held = context.value().lock();
        if (held && !held->isShared() && !held->isReadOnly()) active.insert(pathKey(held->directory()));
      }
    }
    // QLockFile destruction performs I/O; never hold the admission mutex while
    // unlinking a lease file. Newly created contexts reacquire before writing.
    for (auto it = leases_.begin(); it != leases_.end();)
      if (!active.contains(it.key())) it = leases_.erase(it); else ++it;
  }

  bool takeJob(Job* job, bool* cancelled) {
    QMutexLocker guard(&state_->mutex);
    const auto take = [this, job, cancelled](std::deque<Job>& queue) {
      if (queue.empty()) return false;
      *job = std::move(queue.front()); queue.pop_front();
      if (!job->write && !job->auxiliary && job->context &&
          state_->cancelledReadContexts.contains(job->context->id())) *cancelled = true;
      return true;
    };
    *cancelled = false;
    if (state_->closing) {
      if (take(state_->selectedReads) || take(state_->backgroundReads)) { *cancelled = true; return true; }
      return take(state_->requiredWrites) || take(state_->auxiliary) || take(state_->normalWrites);
    }
    return take(state_->requiredWrites) || take(state_->auxiliary) || take(state_->selectedReads) ||
           take(state_->normalWrites) || take(state_->backgroundReads);
  }

  void removeCursor(const QString& id) {
    cursors_.remove(id);
    QMutexLocker guard(&state_->mutex);
    state_->liveScans.remove(id);
    state_->cancelledScans.remove(id);
  }

  void processCancellations() {
    QSet<QString> contexts, scans;
    bool closing = false;
    {
      QMutexLocker guard(&state_->mutex);
      contexts = state_->cancelledReadContexts;
      scans = state_->cancelledScans;
      closing = state_->closing;
    }
    const QStringList ids = cursors_.keys();
    for (const QString& id : ids)
      if (closing || scans.contains(id) || contexts.contains(cursors_.value(id)->context->id())) removeCursor(id);
  }

  StorageResult scanPage(const Job& job, StorageResult result) {
    QString cursorId = job.scanCursor;
    std::shared_ptr<DirectoryCursor> cursor;
    if (cursorId.isEmpty()) {
      if (cursors_.size() >= state_->limits.maximumOpenScans) {
        result.status = StorageStatus::QueueFull;
        result.error = QStringLiteral("directory scan cursor budget is full");
        return result;
      }
      if (!existingPathHasReparse(job.path) && !QFileInfo::exists(job.path)) {
        result.status = StorageStatus::NotFound;
        return result;
      }
      Job probe = job;
      probe.path = QDir(job.path).filePath(QStringLiteral(".scan-probe"));
      if (!preparePath(probe, &result.error)) { result.status = StorageStatus::PathRejected; return result; }
      if (!QFileInfo(job.path).isDir()) {
        result.status = StorageStatus::ReadFailed;
        result.error = QStringLiteral("scan directory is unavailable");
        return result;
      }
#ifdef Q_OS_WIN
      HANDLE listing = CreateFileW(reinterpret_cast<LPCWSTR>(job.path.utf16()), FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (listing == INVALID_HANDLE_VALUE) {
        result.status = StorageStatus::ReadFailed;
        result.error = QStringLiteral("directory listing is unavailable (%1)").arg(GetLastError());
        return result;
      }
      CloseHandle(listing);
#endif
      cursor = std::make_shared<DirectoryCursor>();
      cursor->context = job.context;
      cursor->path = job.path;
      cursor->relativeDirectory = job.relativeDirectory;
      cursor->filters = job.nameFilters;
      cursor->iterator = std::make_unique<QDirIterator>(job.path, job.nameFilters,
          QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::NoIteratorFlags);
      cursorId = QUuid::createUuid().toString(QUuid::WithoutBraces);
      cursors_.insert(cursorId, cursor);
      QMutexLocker guard(&state_->mutex);
      state_->liveScans.insert(cursorId);
    } else {
      cursor = cursors_.value(cursorId);
      if (!cursor || cursor->context->id() != job.context->id() || cursor->path != job.path ||
          cursor->filters != job.nameFilters) {
        result.status = StorageStatus::InvalidRequest;
        result.error = QStringLiteral("scan cursor does not match its frozen context and directory");
        return result;
      }
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (result.relativeNames.size() < job.maximumEntriesPerPage && cursor->iterator->hasNext()) {
      cursor->iterator->next();
      const QString name = cursor->iterator->fileName();
      QString safe;
      if (safeRelative(name, &safe)) {
        const QString relative = cursor->relativeDirectory.isEmpty()
            ? safe : cursor->relativeDirectory + QLatin1Char('/') + safe;
        result.bytes += relative.size() * static_cast<qint64>(sizeof(QChar)) + 64;
        if (result.bytes > job.reservedBytes) {
          removeCursor(cursorId);
          result.relativeNames.clear();
          result.status = StorageStatus::ReadFailed;
          result.error = QStringLiteral("directory page exceeded its reserved size");
          return result;
        }
        result.relativeNames.append(relative);
      }
      if (elapsed.elapsed() >= state_->limits.maximumBatchMilliseconds) break;
    }
    result.status = StorageStatus::Scanned;
    result.hasMore = cursor->iterator->hasNext();
    if (result.hasMore) result.scanCursor = cursorId;
    else removeCursor(cursorId);
    return result;
  }

  StorageResult runJob(const Job& job, bool cancelled) {
    StorageResult result;
    result.taskId = job.taskId;
    result.account = job.context->account();
    result.absolutePath = job.path;
    result.revision = job.revision;
    result.queueWaitNanoseconds = job.queueClock.isValid() ? job.queueClock.nsecsElapsed() : 0;
    if (cancelled) { result.status = StorageStatus::Cancelled; return result; }
    if (job.scan) return scanPage(job, std::move(result));
    bool newerNormalWrite = false;
    if (job.write) {
      QMutexLocker guard(&state_->mutex);
      const QString key = pathKey(job.path);
      newerNormalWrite = job.onlyIfMissing && state_->latestAccepted.contains(key);
      if (!job.onlyIfMissing && (job.revision < state_->latestAccepted.value(key) ||
          job.revision < committedRevisions_.value(key))) {
        result.status = StorageStatus::Superseded;
        return result;
      }
    }
    if (job.write) {
      const QString key = pathKey(job.path);
      const QByteArray content = job.encodeJson
          ? QJsonDocument(job.jsonObject).toJson(QJsonDocument::Compact) : job.content;
      if (content.size() > job.reservedBytes ||
          (job.encodeJson && content.size() > job.maximumContentBytes)) {
        result.status = StorageStatus::WriteFailed;
        result.error = QStringLiteral("encoded JSON exceeds its admitted record limit");
        return result;
      }
      const QByteArray digest = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
      if (attemptedRevisions_.value(key) == job.revision && attemptedDigests_.value(key) != digest) {
        result.status = StorageStatus::InvalidRequest;
        result.error = QStringLiteral("one record revision cannot identify different content");
        return result;
      }
      if (committedRevisions_.value(key) == job.revision) {
        result.status = StorageStatus::Superseded;
        return result;
      }
      attemptedRevisions_.insert(key, job.revision);
      attemptedDigests_.insert(key, digest);
      // Establish the account directory before acquiring its lifetime lease;
      // create deeper record paths only after the lease has been acquired.
      Job scope = job;
      scope.path = QDir(job.context->directory()).filePath(QStringLiteral(".scope-check"));
      if (!preparePath(scope, &result.error)) { result.status = StorageStatus::PathRejected; return result; }
      if (!ensureLease(job.context, &result.error)) { result.status = StorageStatus::LockUnavailable; return result; }
      if (!preparePath(job, &result.error)) { result.status = StorageStatus::PathRejected; return result; }
      std::unique_ptr<QLockFile> sharedLock;
      if (job.context->isShared()) {
        sharedLock = std::make_unique<QLockFile>(job.path + QStringLiteral(".lock"));
        sharedLock->setStaleLockTime(0);
        if (!sharedLock->tryLock(0)) {
          result.status = StorageStatus::LockUnavailable;
          result.error = QStringLiteral("shared file is already being written");
          return result;
        }
      }
      if (job.onlyIfMissing && QFileInfo::exists(job.path)) {
        result.status = StorageStatus::Superseded;
        return result;
      }
      if (newerNormalWrite) {
        result.status = StorageStatus::WriteFailed;
        result.error = QStringLiteral("a newer accepted record has not been persisted; migration remains incomplete");
        return result;
      }
      QByteArray writeBytes = content;
      if (job.mergeJsonObject) {
        QJsonParseError parseError{};
        const QJsonDocument patchDocument = job.encodeJson ? QJsonDocument(job.jsonObject)
            : QJsonDocument::fromJson(content, &parseError);
        if (parseError.error != QJsonParseError::NoError || !patchDocument.isObject()) {
          result.status = StorageStatus::WriteFailed;
          result.error = QStringLiteral("JSON patch is not an object");
          return result;
        }
        const QJsonObject patch = patchDocument.object();
        QJsonObject existing;
        if (QFileInfo::exists(job.path)) {
          QFile file(job.path);
          if (!file.open(QIODevice::ReadOnly) || file.size() > job.reservedBytes) {
            result.status = StorageStatus::WriteFailed;
            result.error = QStringLiteral("existing JSON object cannot be read within its budget");
            return result;
          }
          const QByteArray bytes = file.read(job.reservedBytes + 1);
          const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
          if (file.error() != QFileDevice::NoError || bytes.size() > job.reservedBytes ||
              parseError.error != QJsonParseError::NoError || !document.isObject()) {
            result.status = StorageStatus::WriteFailed;
            result.error = QStringLiteral("existing JSON object is invalid; preserving it");
            return result;
          }
          existing = document.object();
          qint64 oldSchema = 0, patchSchema = 0;
          const bool schemaMatches = !patch.contains(QStringLiteral("schema")) ||
              (schemaNumber(existing.value(QStringLiteral("schema")), &oldSchema) &&
               schemaNumber(patch.value(QStringLiteral("schema")), &patchSchema) && oldSchema == patchSchema);
          if ((!job.context->isShared() && existing.value(QStringLiteral("account")).toString() != job.context->account()) || !schemaMatches) {
            result.status = StorageStatus::WriteFailed;
            result.error = QStringLiteral("existing JSON account/schema does not match this patch");
            return result;
          }
        }
        if (!job.context->isShared() && patch.value(QStringLiteral("account")).toString() != job.context->account()) {
          result.status = StorageStatus::WriteFailed;
          result.error = QStringLiteral("JSON patch account does not match its frozen context");
          return result;
        }
        for (auto field = patch.begin(); field != patch.end(); ++field) existing.insert(field.key(), field.value());
        writeBytes = QJsonDocument(existing).toJson(QJsonDocument::Compact);
        if (writeBytes.size() > job.reservedBytes) {
          result.status = StorageStatus::WriteFailed;
          result.error = QStringLiteral("merged JSON object exceeds its reserved budget");
          return result;
        }
      }
      StorageWriteAttempt attempt;
      try {
        attempt = state_->writer ? state_->writer(job.path, writeBytes) : atomicWrite(job.path, writeBytes);
      } catch (const std::exception& error) {
        attempt.error = QString::fromUtf8(error.what());
      } catch (...) {
        attempt.error = QStringLiteral("writer raised an exception");
      }
      result.bytes = attempt.bytes;
      result.error = attempt.error;
      result.status = attempt.saved && attempt.bytes == writeBytes.size()
                          ? StorageStatus::Saved : StorageStatus::WriteFailed;
      if (result.status == StorageStatus::Saved) {
        // Hash the exact final bytes committed by this write, including any
        // JSON encoding/merge. Consumers bind durable raw/fact identities to
        // this receipt; a successful write must never leave the digest empty.
        result.contentDigest = QCryptographicHash::hash(writeBytes, QCryptographicHash::Sha256);
        committedRevisions_.insert(pathKey(job.path), job.revision);
      }
      return result;
    }
    QElapsedTimer readElapsed;
    readElapsed.start();
    const auto readFile = [&] {
      if (!existingPathHasReparse(job.path) && !QFileInfo::exists(job.path)) {
        result.status = StorageStatus::NotFound;
        return;
      }
      if (!preparePath(job, &result.error)) { result.status = StorageStatus::PathRejected; return; }
      QFile file(job.path);
      if (!file.open(QIODevice::ReadOnly)) {
        result.status = StorageStatus::ReadFailed;
        result.error = file.errorString();
        return;
      }
      if (file.size() > job.reservedBytes) {
        result.status = StorageStatus::ReadFailed;
        result.error = QStringLiteral("record exceeds the read reservation");
        return;
      }
      result.content = file.read(job.reservedBytes + 1);
      if (file.error() != QFileDevice::NoError || result.content.size() > job.reservedBytes || !file.atEnd()) {
        result.content.clear();
        result.status = StorageStatus::ReadFailed;
        result.error = QStringLiteral("read failed or record grew past its reservation");
        return;
      }
      result.bytes = result.content.size();
      result.modifiedAt = QFileInfo(file).lastModified();
      result.status = StorageStatus::Loaded;
    };
    readFile();
    result.readElapsedNanoseconds = readElapsed.nsecsElapsed();
    if (result.status == StorageStatus::Loaded)
      result.contentDigest = QCryptographicHash::hash(result.content, QCryptographicHash::Sha256);
    return result;
  }

  void processBatch() {
    processCancellations();
    QElapsedTimer elapsed;
    elapsed.start();
    int tasks = 0;
    qint64 bytes = 0;
    Job job;
    bool cancelled = false;
    while (takeJob(&job, &cancelled)) {
      if (job.auxiliary) {
        try { job.auxiliary(this); }
        catch (...) { /* The callback owns its result/error channel; do not unwind the I/O thread. */ }
        QMutexLocker guard(&state_->mutex);
        --state_->auxiliaryOutstanding;
        --state_->outstandingTasks;
        state_->outstandingBytes -= job.memoryReservation();
      } else {
        StorageResult result;
        try { result = runJob(job, cancelled); }
        catch (...) {
          result.taskId = job.taskId;
          result.account = job.context->account();
          result.absolutePath = job.path;
          result.revision = job.revision;
          result.status = job.write ? StorageStatus::WriteFailed : StorageStatus::ReadFailed;
          result.error = QStringLiteral("storage encoding or processing raised an exception");
        }
        deliverResult(state_, job, result);
      }
      bytes += job.memoryReservation();
      job = {};  // Drop the task's account capability before pruning leases.
      ++tasks;
      if (tasks >= state_->limits.maximumBatchTasks || bytes >= state_->limits.maximumBatchBytes ||
          elapsed.elapsed() >= state_->limits.maximumBatchMilliseconds) break;
    }
    pruneLeases();
    bool stop = false;
    bool queued = false;
    {
      QMutexLocker guard(&state_->mutex);
      for (auto context = state_->contexts.begin(); context != state_->contexts.end();) {
        if (context.value().expired()) {
          state_->cancelledReadContexts.remove(context.key());
          context = state_->contexts.erase(context);
        } else ++context;
      }
      queued = queuedCount(*state_) != 0;
      stop = !queued && state_->closing;
    }
    if (stop) {
      processCancellations();
      leases_.clear();
      QThread::currentThread()->quit();
      return;
    }
    timer_.start(queued ? 0 : state_->limits.idleLeaseCheckMilliseconds);
  }

  std::shared_ptr<Runtime> state_;
  QTimer timer_;
  QHash<QString, std::shared_ptr<Lease>> leases_;
  QHash<QString, std::shared_ptr<DirectoryCursor>> cursors_;
  QHash<QString, quint64> committedRevisions_;
  QHash<QString, quint64> attemptedRevisions_;
  QHash<QString, QByteArray> attemptedDigests_;
};

class IoThread final : public QThread {
public:
  explicit IoThread(std::shared_ptr<Runtime> state) : state_(std::move(state)) {}
protected:
  void run() override {
    Worker worker(state_);
    {
      QMutexLocker guard(&state_->mutex);
      state_->worker = &worker;
      state_->ioThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    }
    worker.wake();
    exec();
    {
      QMutexLocker guard(&state_->mutex);
      state_->worker = nullptr;
    }
  }
private:
  std::shared_ptr<Runtime> state_;
};

StorageSubmission submit(const std::shared_ptr<Runtime>& state, Job job,
                         const QString& relativeInput, bool priority) {
  StorageSubmission admission;
  QString relative;
  const bool relativeValid = job.scan && (relativeInput.isEmpty() || relativeInput == QStringLiteral("."))
                                 ? true : safeRelative(relativeInput, &relative);
  if (!job.context || !relativeValid || (job.write && job.context->isReadOnly()) ||
      (job.context->isShared() && relative.section(QLatin1Char('/'), 0, 0)
                                     .compare(QStringLiteral("accounts"), pathCase()) == 0) ||
      (job.write && job.revision == 0) || job.reservedBytes < 0 ||
      job.reservedBytes > state->limits.maximumRecordBytes || job.content.size() > job.reservedBytes) {
    admission.error = QStringLiteral("invalid context, relative path, revision or record budget");
    return admission;
  }
  job.path = cleanPath(QDir(job.context->directory()).filePath(relative));
  if (job.scan) job.relativeDirectory = relative;
  if (job.path.size() > 32767 ||
      (!isInside(job.context->directory(), job.path) && !(job.scan && job.path == job.context->directory()))) {
    admission.status = StorageStatus::PathRejected;
    admission.error = QStringLiteral("target escapes the context directory");
    return admission;
  }
  QMutexLocker guard(&state->mutex);
  const auto owned = state->contexts.value(job.context->id()).lock();
  if (!owned || owned.get() != job.context.get()) {
    admission.error = QStringLiteral("context belongs to another storage service");
    return admission;
  }
  if (!job.write && state->cancelledReadContexts.contains(job.context->id())) {
    admission.status = StorageStatus::Cancelled;
    admission.error = QStringLiteral("this context's cache reads have been cancelled");
    return admission;
  }
  if (state->closing) {
    admission.status = StorageStatus::Closing;
    admission.error = QStringLiteral("storage is closing");
    return admission;
  }
  if (state->outstandingTasks >= state->limits.maximumOutstandingTasks ||
      job.memoryReservation() > state->limits.maximumOutstandingBytes - state->outstandingBytes) {
    admission.status = StorageStatus::QueueFull;
    admission.error = QStringLiteral("bounded storage queue is full; caller retains unsaved content");
    return admission;
  }
  job.taskId = ++state->nextTaskId;
  job.queueClock.start();
  admission.taskId = job.taskId;
  admission.accepted = true;
  admission.status = StorageStatus::Queued;
  ++state->outstandingTasks;
  state->outstandingBytes += job.memoryReservation();
  if (job.write) {
    const QString key = pathKey(job.path);
    if (!job.onlyIfMissing)
      state->latestAccepted.insert(key, std::max(job.revision, state->latestAccepted.value(key)));
    (priority ? state->requiredWrites : state->normalWrites).push_back(std::move(job));
  } else {
    (priority ? state->selectedReads : state->backgroundReads).push_back(std::move(job));
  }
  wakeLocked(state);
  return admission;
}
}  // namespace
}  // namespace StorageInternal

StorageService::StorageService(const QString& resolvedDataRoot, const StorageLimits& requested,
                               Writer writer, QObject* parent)
    : QObject(parent), runtime_(std::make_shared<StorageInternal::Runtime>()) {
  qRegisterMetaType<StorageResult>();
  const QString root = StorageInternal::cleanPath(resolvedDataRoot);
  if (QDir::isAbsolutePath(root) && root.size() <= 32767 && !root.contains(QChar::Null)) runtime_->dataRoot = root;
  runtime_->limits = requested;
  runtime_->limits.maximumOutstandingTasks = std::max(1, requested.maximumOutstandingTasks);
  runtime_->limits.maximumOutstandingBytes = std::max<qint64>(1, requested.maximumOutstandingBytes);
  runtime_->limits.maximumRecordBytes = std::max<qint64>(1, std::min(requested.maximumRecordBytes,
      static_cast<qint64>(std::numeric_limits<int>::max()) - 1));
  runtime_->limits.maximumBatchTasks = std::max(1, requested.maximumBatchTasks);
  runtime_->limits.maximumBatchBytes = std::max<qint64>(1, requested.maximumBatchBytes);
  runtime_->limits.maximumBatchMilliseconds = std::max(1, requested.maximumBatchMilliseconds);
  runtime_->limits.idleLeaseCheckMilliseconds = std::max(1, requested.idleLeaseCheckMilliseconds);
  runtime_->limits.maximumOpenScans = std::max(1, requested.maximumOpenScans);
  runtime_->writer = std::move(writer);
  runtime_->receiver = this;
  thread_ = new StorageInternal::IoThread(runtime_);
  thread_->setObjectName(QStringLiteral("KQPetStorageIO"));
  connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
  thread_->start();
}

StorageService::~StorageService() {
  bool alreadyClosing = false;
  {
    QMutexLocker guard(&runtime_->mutex);
    alreadyClosing = runtime_->closing;
    runtime_->receiver = nullptr;
  }
  // An explicit shutdown already spent the caller's budget. Destruction must
  // not silently add another two-second wait to the same closing operation.
  shutdown(alreadyClosing ? 0 : 2000);
}

QString StorageService::dataRoot() const { return runtime_->dataRoot; }

StorageContext StorageService::createAccountContext(const QString& account,
    const QString& safeDirectoryName, QString* error) {
  if (account.isEmpty() || account.size() > 1024 || runtime_->dataRoot.isEmpty() ||
      safeDirectoryName.isEmpty() || safeDirectoryName.size() > 128) {
    if (error) *error = QStringLiteral("account and fixed data root are required");
    return {};
  }
  for (QChar character : safeDirectoryName) {
    if (!((character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
          (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) ||
          (character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
          character == QLatin1Char('_') || character == QLatin1Char('-'))) {
      if (error) *error = QStringLiteral("account directory name must already be safely encoded");
      return {};
    }
  }
  QString safe;
  if (!StorageInternal::safeRelative(safeDirectoryName, &safe)) return {};
  const QString directory = QDir(runtime_->dataRoot).filePath(QStringLiteral("accounts/") + safe);
  QMutexLocker guard(&runtime_->mutex);
  const QString key = StorageInternal::pathKey(directory);
  if (runtime_->closing || (runtime_->accountNames.contains(key) && runtime_->accountNames.value(key) != account)) {
    if (error) *error = QStringLiteral("closing storage or account-directory identity collision");
    return {};
  }
  StorageContext context(new StorageWriteContext(account, runtime_->dataRoot, directory, false));
  runtime_->accountNames.insert(key, account);
  runtime_->contexts.insert(context->id(), context);
  return context;
}

StorageContext StorageService::createSharedContext() {
  QMutexLocker guard(&runtime_->mutex);
  if (runtime_->closing || runtime_->dataRoot.isEmpty()) return {};
  StorageContext context(new StorageWriteContext({}, runtime_->dataRoot, runtime_->dataRoot, true));
  runtime_->contexts.insert(context->id(), context);
  return context;
}

StorageContext StorageService::createReadOnlyContext(const QString& resolvedReadRoot) {
  const QString root = StorageInternal::cleanPath(resolvedReadRoot);
  if (!QDir::isAbsolutePath(root) || root.size() > 32767 || root.contains(QChar::Null)) return {};
  QMutexLocker guard(&runtime_->mutex);
  if (runtime_->closing) return {};
  StorageContext context(new StorageWriteContext({}, root, root, false, true));
  runtime_->contexts.insert(context->id(), context);
  return context;
}

StorageSubmission StorageService::submitWrite(const StoreWrite& write) {
  StorageInternal::Job job;
  job.context = write.context;
  job.revision = write.revision;
  job.content = write.content;
  job.reservedBytes = write.mergeJsonObject ? write.mergeMaximumBytes : write.content.size();
  job.write = true;
  job.onlyIfMissing = write.onlyIfMissing;
  job.mergeJsonObject = write.mergeJsonObject;
  return StorageInternal::submit(runtime_, std::move(job), write.relativePath, write.required);
}

StorageSubmission StorageService::submitJsonWrite(const StoreJsonWrite& write) {
  StorageSubmission rejected;
  const qint64 limit = runtime_->limits.maximumOutstandingBytes;
  qint64 retained = 0, encoded = 0;
  if (write.maximumBytes <= 0 || write.maximumBytes > runtime_->limits.maximumRecordBytes ||
      !StorageInternal::jsonBudget(write.object, limit, &retained, &encoded)) {
    rejected.error = QStringLiteral("JSON depth, retained value or record limit exceeds storage budget");
    return rejected;
  }
  // A merge may retain the old bytes, parsed containers, detached merged tree
  // and encoded result simultaneously. Reserve their conservative peak too.
  const qint64 mergeFactor = write.mergeJsonObject ? 64 : 0;
  const qint64 recordReservation = write.mergeJsonObject ? write.maximumBytes
      : std::min(write.maximumBytes, encoded);
  if (retained > limit || encoded > (limit - retained) / 2 ||
      recordReservation > (limit - retained - encoded * 2) / (mergeFactor + 1)) {
    rejected.error = QStringLiteral("JSON working set cannot fit the bounded storage queue");
    return rejected;
  }
  StorageInternal::Job job;
  job.context = write.context;
  job.revision = write.revision;
  job.jsonObject = write.object;
  job.encodeJson = true;
  job.reservedBytes = recordReservation;
  job.maximumContentBytes = write.maximumBytes;
  job.extraMemoryBytes = retained + encoded * 2 + recordReservation * mergeFactor;
  job.write = true;
  job.onlyIfMissing = write.onlyIfMissing;
  job.mergeJsonObject = write.mergeJsonObject;
  return StorageInternal::submit(runtime_, std::move(job), write.relativePath, write.required);
}

StorageSubmission StorageService::submitRead(const StoreRead& read) {
  StorageInternal::Job job;
  job.context = read.context;
  job.revision = read.expectedMemoryRevision;
  job.reservedBytes = read.maximumBytes;
  return StorageInternal::submit(runtime_, std::move(job), read.relativePath, read.selected);
}

StorageSubmission StorageService::submitScan(const StoreScan& scan) {
  StorageInternal::Job job;
  job.context = scan.context;
  job.revision = scan.expectedMemoryRevision;
  job.scan = true;
  job.scanCursor = scan.cursor;
  job.nameFilters = scan.nameFilters;
  job.maximumEntriesPerPage = std::clamp(scan.maximumEntriesPerPage, 1, 128);
  job.reservedBytes = (scan.relativeDirectory.size() + 1 + 256 + 64) *
                      static_cast<qint64>(sizeof(QChar)) * job.maximumEntriesPerPage;
  return StorageInternal::submit(runtime_, std::move(job), scan.relativeDirectory, false);
}

void StorageService::cancelScan(const QString& cursor) {
  QMutexLocker guard(&runtime_->mutex);
  if (!runtime_->liveScans.contains(cursor)) return;
  runtime_->cancelledScans.insert(cursor);
  StorageInternal::wakeLocked(runtime_);
}

void StorageService::cancelReads(const StorageContext& context) {
  if (!context) return;
  QMutexLocker guard(&runtime_->mutex);
  if (runtime_->contexts.value(context->id()).lock().get() != context.get()) return;
  runtime_->cancelledReadContexts.insert(context->id());
  StorageInternal::wakeLocked(runtime_);
}

void StorageService::prioritizeRead(quint64 taskId) {
  QMutexLocker guard(&runtime_->mutex);
  auto& background = runtime_->backgroundReads;
  const auto found = std::find_if(background.begin(), background.end(), [taskId](const StorageInternal::Job& job) {
    return job.taskId == taskId && !job.scan;
  });
  if (found == background.end()) return;
  runtime_->selectedReads.push_front(std::move(*found));
  background.erase(found);
  StorageInternal::wakeLocked(runtime_);
}

bool StorageService::postAuxiliary(std::function<void(QObject*)> callback) {
  if (!callback || QThread::currentThread() != thread()) return false;
  constexpr qint64 metadataReservation = 512;
  QMutexLocker guard(&runtime_->mutex);
  if (runtime_->closing || runtime_->dataRoot.isEmpty() || runtime_->auxiliaryOutstanding >= 2 ||
      runtime_->outstandingTasks >= runtime_->limits.maximumOutstandingTasks ||
      metadataReservation > runtime_->limits.maximumOutstandingBytes - runtime_->outstandingBytes) return false;
  StorageInternal::Job job;
  job.taskId = ++runtime_->nextTaskId;
  job.reservedBytes = metadataReservation;
  job.auxiliary = std::move(callback);
  ++runtime_->auxiliaryOutstanding;
  ++runtime_->outstandingTasks;
  runtime_->outstandingBytes += metadataReservation;
  runtime_->auxiliary.push_back(std::move(job));
  StorageInternal::wakeLocked(runtime_);
  return true;
}

StorageQueueState StorageService::state() const {
  QMutexLocker guard(&runtime_->mutex);
  return {runtime_->outstandingTasks, runtime_->outstandingBytes,
          StorageInternal::queuedCount(*runtime_), runtime_->closing, runtime_->ioThreadId};
}

bool StorageService::shutdown(unsigned long waitMilliseconds) {
  {
    QMutexLocker guard(&runtime_->mutex);
    runtime_->closing = true;
    StorageInternal::wakeLocked(runtime_);
  }
  if (!thread_) return true;
  if (!thread_->wait(waitMilliseconds)) return false;
  delete thread_.data();
  thread_ = nullptr;
  return true;
}

#include "storage_service.moc"
