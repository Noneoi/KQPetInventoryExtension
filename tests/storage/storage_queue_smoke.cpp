#include "storage/storage_service.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QSemaphore>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
bool check(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool waitUntil(const std::function<bool()>& predicate, int timeout = 1500) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return predicate();
}

StorageWriteAttempt realWrite(const QString& path, const QByteArray& content) {
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return {false, 0, file.errorString()};
  const qint64 bytes = file.write(content);
  if (bytes != content.size() || !file.commit()) return {false, bytes, file.errorString()};
  return {true, bytes, {}};
}

bool writeFixture(const QString& path, const QByteArray& content) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  return realWrite(path, content).saved;
}

QByteArray readFixture(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool subprocessCanLock(const QString& path) {
  QProcess child;
  child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--probe-lock"), path});
  return child.waitForFinished(1500) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
}

bool unlockedNow(const QString& path) {
  QLockFile lock(path);
  lock.setStaleLockTime(0);
  return lock.tryLock(0);
}

bool queueAndRevisionTest(const QString& root) {
  bool ok = true;
  QSemaphore entered, release;
  std::atomic<int> calls{0};
  std::atomic<quintptr> writerThread{0};
  StorageLimits limits;
  limits.maximumBatchTasks = 1;
  limits.idleLeaseCheckMilliseconds = 5;
  StorageService storage(root, limits, [&](const QString& path, const QByteArray& content) {
    writerThread.store(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    if (calls.fetch_add(1) == 0) { entered.release(); release.acquire(); }
    return realWrite(path, content);
  });
  QHash<quint64, StorageResult> results;
  bool resultThreadCorrect = true;
  QObject::connect(&storage, &StorageService::completed, &storage, [&](const StorageResult& result) {
    resultThreadCorrect &= QThread::currentThread() == storage.thread();
    results.insert(result.taskId, result);
  });
  StorageContext account = storage.createAccountContext(QStringLiteral("account-A"), QStringLiteral("A"));
  const QString directory = account->directory();
  const auto first = storage.submitWrite({account, QStringLiteral("hold.json"), 1, QByteArray("hold"), true});
  ok &= check(first.accepted && entered.tryAcquire(1, 1500), "I/O worker did not start the first write");
  StoreWrite latest{account, QStringLiteral("details/pet.json"), 9, QByteArray("new-frozen"), false};
  const auto newer = storage.submitWrite(latest);
  latest.content = QByteArray("mutated-after-submit");
  latest.relativePath = QStringLiteral("wrong.json");
  latest.revision = 999;
  latest.context.reset();
  const auto older = storage.submitWrite({account, QStringLiteral("details/pet.json"), 3, QByteArray("old"), true});
  ok &= check(newer.accepted && older.accepted, "revision fixtures were not admitted");
  const QString lockPath = QDir(directory).filePath(QStringLiteral(".storage-writer.lock"));
  ok &= check(!subprocessCanLock(lockPath), "another process acquired an active account lease");
  StorageContext successor = storage.createAccountContext(QStringLiteral("account-A"), QStringLiteral("A"));
  account.reset();
  ok &= check(!subprocessCanLock(lockPath), "account switch released a queued/in-flight task's lease");
  release.release();
  ok &= check(waitUntil([&] { return results.size() == 3; }), "queued writes did not complete");
  ok &= check(results.value(older.taskId).status == StorageStatus::Superseded &&
                  results.value(newer.taskId).status == StorageStatus::Saved &&
                  readFixture(QDir(directory).filePath(QStringLiteral("details/pet.json"))) == QByteArray("new-frozen") &&
                  !QFileInfo::exists(QDir(directory).filePath(QStringLiteral("wrong.json"))),
              "old revision or caller mutation overwrote the frozen latest record");
  ok &= check(!subprocessCanLock(lockPath), "new context of the same account did not retain the existing lease");
  const auto duplicate = storage.submitWrite({successor, QStringLiteral("details/pet.json"), 9, QByteArray("new-frozen"), false});
  const auto conflict = storage.submitWrite({successor, QStringLiteral("details/pet.json"), 9, QByteArray("different"), false});
  ok &= check(duplicate.accepted && conflict.accepted && waitUntil([&] { return results.size() == 5; }) &&
                  results.value(duplicate.taskId).status == StorageStatus::Superseded &&
                  results.value(conflict.taskId).status == StorageStatus::InvalidRequest &&
                  readFixture(QDir(directory).filePath(QStringLiteral("details/pet.json"))) == QByteArray("new-frozen"),
              "equal revision with conflicting bytes silently overwrote a record");
  successor.reset();
  ok &= check(waitUntil([&] { return unlockedNow(lockPath); }) && subprocessCanLock(lockPath),
              "drained account lease was not released after all contexts expired");
  ok &= check(writerThread.load() != 0 &&
                  writerThread.load() != reinterpret_cast<quintptr>(QThread::currentThreadId()) && resultThreadCorrect,
              "file work or completion ran on the wrong owning thread");
  ok &= check(storage.shutdown() && storage.state().closing &&
                  !storage.submitWrite({account, QStringLiteral("late.json"), 10, {}, true}).accepted,
              "closed storage accepted new work or did not stop");
  return ok;
}

bool selectedReadAndBoundsTest(const QString& root) {
  bool ok = true;
  QSemaphore entered, release;
  StorageLimits limits;
  limits.maximumBatchTasks = 1;
  limits.maximumOutstandingTasks = 4;
  limits.maximumOutstandingBytes = 256;
  limits.maximumRecordBytes = 128;
  std::atomic<int> calls{0};
  StorageService storage(root, limits, [&](const QString& path, const QByteArray& content) {
    if (calls.fetch_add(1) == 0) { entered.release(); release.acquire(); }
    return realWrite(path, content);
  });
  const auto account = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  const QString readPath = QDir(account->directory()).filePath(QStringLiteral("detail.json"));
  ok &= check(writeFixture(readPath, QByteArray("read-value")), "read fixture could not be written");
  QList<quint64> completed;
  QHash<quint64, StorageResult> results;
  QObject::connect(&storage, &StorageService::completed, &storage, [&](const StorageResult& result) {
    completed.append(result.taskId); results.insert(result.taskId, result);
  });
  const auto hold = storage.submitWrite({account, QStringLiteral("hold.json"), 1, QByteArray(16, 'h'), true});
  ok &= check(hold.accepted && entered.tryAcquire(1, 1500), "priority test worker did not block");
  const auto background = storage.submitRead({account, QStringLiteral("detail.json"), 41, 64, false});
  const auto selected = storage.submitRead({account, QStringLiteral("detail.json"), 42, 64, true});
  QElapsedTimer selectedQueued; selectedQueued.start();
  const auto oversizedQueue = storage.submitRead({account, QStringLiteral("detail.json"), 43, 128, false});
  ok &= check(!oversizedQueue.accepted && oversizedQueue.status == StorageStatus::QueueFull,
              "read-buffer reservations were not included in queue byte limits");
  const auto finalSlot = storage.submitWrite({account, QStringLiteral("last.json"), 1, QByteArray("last"), false});
  const auto overflow = storage.submitWrite({account, QStringLiteral("overflow.json"), 1, {}, false});
  ok &= check(finalSlot.accepted && !overflow.accepted && overflow.status == StorageStatus::QueueFull,
              "outstanding task capacity was not bounded");
  const qint64 minimumQueueWait = selectedQueued.nsecsElapsed();
  release.release();
  ok &= check(waitUntil([&] { return completed.size() == 4; }), "priority/read jobs did not complete");
  ok &= check(completed.indexOf(selected.taskId) < completed.indexOf(background.taskId) &&
                  results.value(selected.taskId).status == StorageStatus::Loaded &&
                  results.value(selected.taskId).revision == 42 &&
                  results.value(selected.taskId).content == QByteArray("read-value"),
              "selected read priority or Core revision tag was lost");
  ok &= check(results.value(selected.taskId).readElapsedNanoseconds > 0 &&
                  results.value(selected.taskId).queueWaitNanoseconds >= minimumQueueWait &&
                  results.value(hold.taskId).readElapsedNanoseconds == 0 &&
                  results.value(finalSlot.taskId).readElapsedNanoseconds == 0,
              "file read span was missing, included a write, or queue residence excluded the blocked interval");
  const auto tooSmall = storage.submitRead({account, QStringLiteral("detail.json"), 99, 2, true});
  ok &= check(tooSmall.accepted && waitUntil([&] { return results.contains(tooSmall.taskId); }) &&
                  results.value(tooSmall.taskId).status == StorageStatus::ReadFailed &&
                  results.value(tooSmall.taskId).content.isEmpty() && results.value(tooSmall.taskId).readElapsedNanoseconds > 0,
              "oversized read escaped its buffer reservation");
  ok &= check(storage.shutdown(), "priority service did not stop");
  return ok;
}

bool pathLockAndAtomicFailureTest(const QString& root) {
  bool ok = true;
  StorageService storage(root);
  const auto account = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  const auto shared = storage.createSharedContext();
  QHash<quint64, StorageResult> results;
  QObject::connect(&storage, &StorageService::completed, &storage,
                   [&](const StorageResult& result) { results.insert(result.taskId, result); });
  for (const QString& path : {QStringLiteral("../escape.json"), QStringLiteral("nested/../../escape.json"),
                             QStringLiteral("C:/escape.json"), QStringLiteral("file.json:stream"),
                             QStringLiteral(".storage-writer.lock")})
    ok &= check(!storage.submitWrite({account, path, 1, QByteArray("bad"), false}).accepted,
                "path traversal, alternate stream, or lock-file overwrite was admitted");
  ok &= check(!storage.submitWrite({shared, QStringLiteral("accounts/A/bypass.json"), 1, {}, false}).accepted &&
                  !storage.createAccountContext(QStringLiteral("other"), QStringLiteral("a")),
              "shared capability bypassed account ownership or directory aliases collided");
  const QString sharedPath = QDir(root).filePath(QStringLiteral("settings.json"));
  ok &= check(writeFixture(sharedPath, QByteArray("old-settings")), "shared fixture write failed");
  QLockFile sharedLock(sharedPath + QStringLiteral(".lock"));
  sharedLock.setStaleLockTime(0);
  ok &= check(sharedLock.tryLock(0), "shared-file lock fixture could not acquire ownership");
  const auto blockedShared = storage.submitWrite({shared, QStringLiteral("settings.json"), 1, QByteArray("new-settings"), false});
  ok &= check(blockedShared.accepted && waitUntil([&] { return results.contains(blockedShared.taskId); }) &&
                  results.value(blockedShared.taskId).status == StorageStatus::LockUnavailable &&
                  readFixture(sharedPath) == QByteArray("old-settings"),
              "shared write ignored its short per-file lock");
  sharedLock.unlock();
  const auto acceptedShared = storage.submitWrite({shared, QStringLiteral("settings.json"), 2, QByteArray("new-settings"), false});
  ok &= check(acceptedShared.accepted && waitUntil([&] { return results.contains(acceptedShared.taskId); }) &&
                  results.value(acceptedShared.taskId).status == StorageStatus::Saved &&
                  unlockedNow(sharedPath + QStringLiteral(".lock")),
              "shared lock outlived its atomic write");

  const QString protectedPath = QDir(account->directory()).filePath(QStringLiteral("protected.json"));
  ok &= check(writeFixture(protectedPath, QByteArray("old-account-record")), "atomic failure fixture write failed");
#ifdef Q_OS_WIN
  const QString outside = QDir(QFileInfo(root).absolutePath()).filePath(QStringLiteral("outside-junction"));
  const QString junctionPath = QDir(account->directory()).filePath(QStringLiteral("junction"));
  QDir().mkpath(outside);
  QProcess junction;
  junction.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
    arguments->flags |= CREATE_NO_WINDOW;
  });
  junction.start(QStringLiteral("cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"),
      QStringLiteral("mklink"), QStringLiteral("/J"), QDir::toNativeSeparators(junctionPath),
      QDir::toNativeSeparators(outside)});
  const bool linked = junction.waitForFinished(1500) && junction.exitCode() == 0;
  ok &= check(linked, "could not create the isolated junction boundary fixture");
  if (linked) {
    const auto escape = storage.submitWrite({account, QStringLiteral("junction/escape.json"), 1, QByteArray("bad"), true});
    ok &= check(escape.accepted && waitUntil([&] { return results.contains(escape.taskId); }) &&
                    results.value(escape.taskId).status == StorageStatus::PathRejected &&
                    !QFileInfo::exists(QDir(outside).filePath(QStringLiteral("escape.json"))),
                "directory junction escaped the frozen account path boundary");
    QDir().rmdir(junctionPath);
  }
  HANDLE blocker = CreateFileW(reinterpret_cast<LPCWSTR>(protectedPath.utf16()), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ok &= check(blocker != INVALID_HANDLE_VALUE, "cannot deny replacement of atomic-write fixture");
  const auto denied = storage.submitWrite({account, QStringLiteral("protected.json"), 1, QByteArray("new-record"), true});
  ok &= check(denied.accepted && waitUntil([&] { return results.contains(denied.taskId); }) &&
                  results.value(denied.taskId).status == StorageStatus::WriteFailed &&
                  readFixture(protectedPath) == QByteArray("old-account-record"),
              "failed QSaveFile commit damaged the previous record");
  if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
#endif
  const QString blockedDirectory = QDir(account->directory()).filePath(QStringLiteral("directory.json"));
  QDir().mkpath(blockedDirectory);
  const auto directoryFailure = storage.submitWrite({account, QStringLiteral("directory.json"), 1, QByteArray("bad"), true});
  ok &= check(directoryFailure.accepted && waitUntil([&] { return results.contains(directoryFailure.taskId); }) &&
                  results.value(directoryFailure.taskId).status == StorageStatus::WriteFailed,
              "real atomic write to a directory was reported as saved");
  ok &= check(storage.shutdown(), "atomic failure service did not stop");
  return ok;
}

bool scanAndCancellationTest(const QString& root) {
  bool ok = true;
  const QString legacyRoot = QDir(QFileInfo(root).absolutePath()).filePath(QStringLiteral("legacy-read-source"));
  for (int index = 0; index < 23; ++index)
    ok &= check(writeFixture(QDir(legacyRoot).filePath(QStringLiteral("details/%1.json").arg(index)), QByteArray("{}")),
                "directory pagination fixture write failed");
  StorageService storage(root);
  const auto source = storage.createReadOnlyContext(legacyRoot);
  ok &= check(source && !storage.submitWrite({source, QStringLiteral("forbidden.json"), 1, QByteArray("bad"), true}).accepted,
              "legacy read capability acquired write authority");
  QHash<quint64, StorageResult> results;
  QObject::connect(&storage, &StorageService::completed, &storage,
                   [&](const StorageResult& result) { results.insert(result.taskId, result); });
  QSet<QString> names;
  QString cursor;
  int pages = 0;
  do {
    const auto page = storage.submitScan({source, QStringLiteral("details"), {QStringLiteral("*.json")}, 77, cursor, 7});
    ok &= check(page.accepted && waitUntil([&] { return results.contains(page.taskId); }),
                "directory page did not complete");
    const StorageResult result = results.value(page.taskId);
    ok &= check(result.status == StorageStatus::Scanned && result.revision == 77 && result.relativeNames.size() <= 7,
                "directory scan ignored its page or revision contract");
    for (const QString& name : result.relativeNames) names.insert(name);
    cursor = result.scanCursor;
    ++pages;
    if (result.status != StorageStatus::Scanned || !result.hasMore) break;
  } while (pages < 30);
  ok &= check(names.size() == 23 && pages >= 4 && cursor.isEmpty(),
              "paged directory scan omitted or repeated a continuation");
  const auto first = storage.submitScan({source, QStringLiteral("details"), {QStringLiteral("*.json")}, 78, {}, 1});
  ok &= check(first.accepted && waitUntil([&] { return results.contains(first.taskId); }), "cancel scan fixture failed");
  cursor = results.value(first.taskId).scanCursor;
  ok &= check(!cursor.isEmpty(), "cancel fixture did not create an open cursor");
  storage.cancelScan(cursor);
  const auto cancelled = storage.submitScan({source, QStringLiteral("details"), {QStringLiteral("*.json")}, 78, cursor, 1});
  ok &= check(cancelled.accepted && waitUntil([&] { return results.contains(cancelled.taskId); }) &&
                  results.value(cancelled.taskId).status != StorageStatus::Scanned,
              "closed scan cursor continued reading a stale account");
  storage.cancelReads(source);
  const auto denied = storage.submitRead({source, QStringLiteral("details/1.json"), 79, 64, true});
  ok &= check(!denied.accepted && denied.status == StorageStatus::Cancelled,
              "cancelled context admitted another cache read");
  ok &= check(storage.shutdown(), "scan service did not stop");
  return ok;
}

bool auxiliaryExecutorTest(const QString& root) {
  bool ok = true;
  StorageLimits limits;
  limits.maximumOutstandingTasks = 2;
  StorageService storage(root, limits);
  QSemaphore entered, release;
  std::atomic<int> completed{0};
  std::atomic<bool> ownerCorrect{true};
  const quintptr coreThread = reinterpret_cast<quintptr>(QThread::currentThreadId());
  const bool first = storage.postAuxiliary([&](QObject* ioRoot) {
    ownerCorrect.store(ioRoot && ioRoot->thread() == QThread::currentThread() &&
        reinterpret_cast<quintptr>(QThread::currentThreadId()) != coreThread);
    new QObject(ioRoot);
    entered.release();
    release.acquire();
    ++completed;
  });
  ok &= check(first && entered.tryAcquire(1, 1500), "auxiliary callback was lost before worker readiness");
  const bool second = storage.postAuxiliary([&](QObject* ioRoot) {
    ownerCorrect.store(ownerCorrect.load() && ioRoot && ioRoot->thread() == QThread::currentThread());
    ++completed;
  });
  ok &= check(second && !storage.postAuxiliary([](QObject*) {}),
              "auxiliary executor admitted more than two queued/executing callbacks");
  const auto context = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  const auto overflow = storage.submitWrite({context, QStringLiteral("blocked-by-budget.json"), 1, {}, false});
  ok &= check(!overflow.accepted && overflow.status == StorageStatus::QueueFull &&
                  storage.state().outstandingTasks == 2 && storage.state().outstandingBytes >= 1024,
              "auxiliary callbacks bypassed shared queue accounting");
  release.release();
  ok &= check(waitUntil([&] { return completed.load() == 2 && storage.state().outstandingTasks == 0; }) && ownerCorrect.load(),
              "auxiliary work ran inline/on Core or leaked its task reservations");
  ok &= check(storage.shutdown() && !storage.postAuxiliary([](QObject*) {}),
              "closing storage accepted auxiliary work");
  return ok;
}

bool shutdownLeaseTest(const QString& root) {
  bool ok = true;
  QSemaphore entered, release;
  auto* storage = new StorageService(root, {}, [&](const QString& path, const QByteArray& content) {
    entered.release(); release.acquire(); return realWrite(path, content);
  });
  auto context = storage->createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  const QString directory = context->directory();
  const QString lockPath = QDir(directory).filePath(QStringLiteral(".storage-writer.lock"));
  const QString path = QDir(directory).filePath(QStringLiteral("pending.json"));
  const auto accepted = storage->submitWrite({context, QStringLiteral("pending.json"), 1, QByteArray("durable-after-timeout"), true});
  ok &= check(accepted.accepted && entered.tryAcquire(1, 1500), "shutdown fixture did not begin I/O");
  context.reset();
  QElapsedTimer elapsed;
  elapsed.start();
  ok &= check(!storage->shutdown(2000), "blocked I/O incorrectly reported completed shutdown");
  delete storage;  // Must return after its budget without deleting the active QThread.
  ok &= check(elapsed.elapsed() < 2400 && !subprocessCanLock(lockPath),
              "timeout destroyed a live thread or released its in-flight account lock");
  release.release();
  ok &= check(waitUntil([&] { return readFixture(path) == QByteArray("durable-after-timeout") && unlockedNow(lockPath); }, 2000),
              "accepted write or its lease was lost after facade shutdown timed out");
  return ok;
}

bool jsonValueWriteTest(const QString& root) {
  bool ok = true;
  QSemaphore entered, release;
  std::atomic<bool> correctThread{true};
  const auto owner = QThread::currentThreadId();
  StorageLimits limits;
  limits.maximumOutstandingBytes = 1024 * 1024;
  limits.maximumRecordBytes = 16 * 1024 * 1024;
  StorageService storage(root, limits, [&](const QString& path, const QByteArray& bytes) {
    correctThread.store(correctThread.load() && QThread::currentThreadId() != owner);
    if (path.endsWith(QStringLiteral("hold.json"))) { entered.release(); release.acquire(); }
    return realWrite(path, bytes);
  });
  QHash<quint64, StorageResult> results;
  QObject::connect(&storage, &StorageService::completed, &storage,
      [&](const StorageResult& result) { results.insert(result.taskId, result); });
  const auto account = storage.createAccountContext(QStringLiteral("json-A"), QStringLiteral("json-A"));
  const auto hold = storage.submitWrite({account, QStringLiteral("hold.json"), 1, QByteArray("hold"), true});
  ok &= check(hold.accepted && entered.tryAcquire(1, 2000), "JSON admission fixture did not hold IO");
  StoreJsonWrite frozen{account, QStringLiteral("frozen.json"), 9,
      {{QStringLiteral("account"), QStringLiteral("json-A")}, {QStringLiteral("count"), 71}}, 1024, true};
  const auto first = storage.submitJsonWrite(frozen);
  frozen.object.insert(QStringLiteral("count"), 99);
  const auto conflict = storage.submitJsonWrite(frozen);
  frozen.relativePath = QStringLiteral("caller-mutated.json");
  frozen.context.reset();
  ok &= check(first.accepted && conflict.accepted && storage.state().outstandingBytes > 2048,
              "JSON queue omitted retained value and encoder working-set reservations");
  const auto oversized = storage.submitJsonWrite({account, QStringLiteral("oversized.json"), 1,
      {{QStringLiteral("text"), QString(80, QLatin1Char('x'))}}, 32, true});
  const auto unbounded = storage.submitJsonWrite({account, QStringLiteral("unbounded.json"), 1,
      {{QStringLiteral("text"), QString(1024 * 1024, QLatin1Char('x'))}}, 1024, true});
  ok &= check(oversized.accepted && !unbounded.accepted && unbounded.status == StorageStatus::InvalidRequest,
              "JSON object bypassed a permanent working-set budget failure");
  const qint64 beforeTiny = storage.state().outstandingBytes;
  const auto tiny = storage.submitJsonWrite({account, QStringLiteral("tiny.json"), 1,
      {{QStringLiteral("count"), 1}}, 16 * 1024 * 1024, true});
  const auto secondTiny = storage.submitJsonWrite({account, QStringLiteral("tiny-2.json"), 1,
      {{QStringLiteral("count"), 2}}, 16 * 1024 * 1024, true});
  ok &= check(tiny.accepted && secondTiny.accepted && storage.state().outstandingBytes - beforeTiny < 8192,
              "small JSON records reserved their full permitted 16 MiB file size");
  release.release();
  ok &= check(waitUntil([&] { return storage.state().outstandingTasks == 0; }) &&
                  results.value(first.taskId).status == StorageStatus::Saved &&
                  results.value(conflict.taskId).status == StorageStatus::InvalidRequest &&
                  results.value(oversized.taskId).status == StorageStatus::WriteFailed &&
                  results.value(tiny.taskId).status == StorageStatus::Saved &&
                  results.value(secondTiny.taskId).status == StorageStatus::Saved &&
                  QJsonDocument::fromJson(readFixture(QDir(account->directory()).filePath(QStringLiteral("frozen.json"))))
                      .object().value(QStringLiteral("count")).toInt() == 71 &&
                  !QFileInfo::exists(QDir(account->directory()).filePath(QStringLiteral("oversized.json"))) && correctThread.load(),
              "JSON encoding violated immutable bytes, digest conflict, record limit or IO ownership");
  const auto shared = storage.createSharedContext();
  const QString settings = QDir(root).filePath(QStringLiteral("settings.json"));
  ok &= check(writeFixture(settings, QByteArray("{\"schema\":1,\"unknown\":{\"keep\":true},\"automaticIntervalMs\":1}")),
              "JSON merge fixture was not written");
  StoreJsonWrite patch{shared, QStringLiteral("settings.json"), 1,
      {{QStringLiteral("schema"), 1}, {QStringLiteral("automaticIntervalMs"), 9000}}, 1024, true};
  patch.mergeJsonObject = true;
  const auto merged = storage.submitJsonWrite(patch);
  ok &= check(merged.accepted && waitUntil([&] { return results.contains(merged.taskId); }) &&
                  results.value(merged.taskId).status == StorageStatus::Saved,
              "immutable JSON settings patch did not commit");
  const auto object = QJsonDocument::fromJson(readFixture(settings)).object();
  ok &= check(results.value(merged.taskId).contentDigest == QCryptographicHash::hash(readFixture(settings), QCryptographicHash::Sha256) &&
                  results.value(first.taskId).contentDigest == QCryptographicHash::hash(readFixture(
                      QDir(account->directory()).filePath(QStringLiteral("frozen.json"))), QCryptographicHash::Sha256) &&
                  results.value(hold.taskId).contentDigest == QCryptographicHash::hash(QByteArray("hold"), QCryptographicHash::Sha256) &&
                  results.value(oversized.taskId).contentDigest.isEmpty(),
              "Saved byte/JSON/merged writes did not return the exact committed source digest, or a failed write claimed one");
  ok &= check(object.value(QStringLiteral("unknown")).toObject().value(QStringLiteral("keep")).toBool() &&
                  object.value(QStringLiteral("automaticIntervalMs")).toInt() == 9000,
              "IO merge discarded unknown settings fields");
  ok &= check(storage.shutdown(), "JSON storage did not drain");
  return ok;
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 3 && app.arguments().at(1) == QStringLiteral("--probe-lock")) {
    QLockFile lock(app.arguments().at(2));
    lock.setStaleLockTime(0);
    return lock.tryLock(0) ? 0 : 3;
  }
  QTemporaryDir temporary;
  bool ok = check(temporary.isValid(), "temporary storage root unavailable");
  ok &= queueAndRevisionTest(QDir(temporary.path()).filePath(QStringLiteral("queue")));
  ok &= selectedReadAndBoundsTest(QDir(temporary.path()).filePath(QStringLiteral("priority")));
  ok &= pathLockAndAtomicFailureTest(QDir(temporary.path()).filePath(QStringLiteral("faults")));
  ok &= scanAndCancellationTest(QDir(temporary.path()).filePath(QStringLiteral("scans")));
  ok &= auxiliaryExecutorTest(QDir(temporary.path()).filePath(QStringLiteral("auxiliary")));
  ok &= jsonValueWriteTest(QDir(temporary.path()).filePath(QStringLiteral("json")));
  ok &= shutdownLeaseTest(QDir(temporary.path()).filePath(QStringLiteral("shutdown")));
  if (ok) std::puts("PASS: bounded I/O, frozen writes, revision order, selected reads, account leases, atomic failures and timeout shutdown");
  return ok ? 0 : 1;
}
