#include "application/pet/move_operation.h"
#include "storage/storage_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>

namespace {
bool waitUntil(const std::function<bool()>& condition, int timeout = 2000) {
  QElapsedTimer timer; timer.start();
  while (!condition() && timer.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return condition();
}
StorageWriteAttempt writeFixture(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path); file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return {false, 0, file.errorString()};
  const qint64 count = file.write(bytes);
  return count == bytes.size() && file.commit() ? StorageWriteAttempt{true, count, {}}
                                               : StorageWriteAttempt{false, count, file.errorString()};
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir root;
  bool ok = root.isValid();
  const auto check = [&](bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ok = false; }
  };
  StorageService storage(root.path()), contender(root.path());
  QHash<quint64, StorageResult> results, competingResults;
  QObject::connect(&storage, &StorageService::completed, &app,
      [&](const StorageResult& result) { results.insert(result.taskId, result); });
  QObject::connect(&contender, &StorageService::completed, &app,
      [&](const StorageResult& result) { competingResults.insert(result.taskId, result); });
  const auto accountA = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  const auto accountB = storage.createAccountContext(QStringLiteral("B"), QStringLiteral("B"));
  MoveOperationJournal original(&storage, accountA);
  const auto intent = original.begin(1, 2, 9007199254740993LL, {1, 9007199254740993LL});
  check(intent.accepted && !results.contains(intent.taskId), "admission was confused with completed durable storage");
  check(waitUntil([&] { return results.contains(intent.taskId); }) &&
        results.value(intent.taskId).status == StorageStatus::Saved,
        "original intent did not become durably saved");
  const auto readRecord = [&](MoveOperationJournal& journal) {
    const auto read = storage.submitRead({journal.context(),
        QDir(journal.context()->directory()).relativeFilePath(journal.path()), journal.revision(), 1024 * 1024, true});
    if (!read.accepted || !waitUntil([&] { return results.contains(read.taskId); })) return QByteArray{};
    return results.value(read.taskId).content;
  };
  auto recovered = MoveOperationJournal::recoveryRecord(readRecord(original));
  check(recovered.value(QStringLiteral("outcome")) == QStringLiteral("Unknown") &&
        recovered.value(QStringLiteral("instanceId")) == QStringLiteral("9007199254740993"),
        "durable intent recovery changed outcome or integer precision");
  MoveOperationJournal conflict(&contender, contender.createAccountContext(QStringLiteral("A"), QStringLiteral("A")));
  const auto conflicting = conflict.begin(1, 3, 2, {1, 2});
  check(conflicting.accepted && waitUntil([&] { return competingResults.contains(conflicting.taskId); }) &&
        competingResults.value(conflicting.taskId).status == StorageStatus::LockUnavailable,
        "another storage service acquired the active account lease");
  MoveOperationJournal sameService(&storage, accountA);
  const auto sharedIntent = sameService.begin(1, 3, 2, {1, 2});
  check(sharedIntent.accepted && waitUntil([&] { return results.contains(sharedIntent.taskId); }) &&
        results.value(sharedIntent.taskId).status == StorageStatus::Saved,
        "same account queue created a competing independent operation lock");
  MoveOperationJournal other(&storage, accountB);
  const auto otherIntent = other.begin(2, 1, 3, {1, 3});
  const auto notSent = other.record(MoveOutcome::NotSent);
  check(otherIntent.accepted && notSent.accepted && waitUntil([&] { return results.contains(notSent.taskId); }) &&
        results.value(notSent.taskId).status == StorageStatus::Saved &&
        MoveOperationJournal::recoveryRecord(readRecord(other)).isEmpty(),
        "NotSent update was not serialized on the other account queue");
  const auto submitted = original.record(MoveOutcome::Submitted);
  const auto uncertain = original.record(MoveOutcome::Unknown, QStringLiteral("source changed"));
  check(submitted.accepted && uncertain.accepted && waitUntil([&] { return results.contains(uncertain.taskId); }) &&
        results.value(uncertain.taskId).status == StorageStatus::Saved,
        "journal revisions were not serialized by shared storage");
  recovered = MoveOperationJournal::recoveryRecord(readRecord(original));
  check(recovered.value(QStringLiteral("recordRevision")).toString() == QString::number(original.revision()) &&
        recovered.value(QStringLiteral("account")) == QStringLiteral("A"),
        "older operation state overwrote latest revision or moved account");
  MoveOperationJournal invalid(&storage, storage.createSharedContext());
  check(!invalid.begin(1, 1, 1, {1}).accepted, "shared root capability authorized personal operation intent");
  check(MoveOperationJournal::recoveryRecord("{broken").isEmpty(), "invalid recovery JSON was accepted");

  QTemporaryDir heldRoot;
  QSemaphore entered, release;
  StorageService held(heldRoot.path(), {}, [&](const QString& path, const QByteArray& bytes) {
    entered.release(); release.acquire(); return writeFixture(path, bytes);
  });
  StorageService lateOwner(heldRoot.path());
  QHash<quint64, StorageResult> heldResults, lateResults;
  QObject::connect(&held, &StorageService::completed, &app,
      [&](const StorageResult& result) { heldResults.insert(result.taskId, result); });
  QObject::connect(&lateOwner, &StorageService::completed, &app,
      [&](const StorageResult& result) { lateResults.insert(result.taskId, result); });
  auto heldContext = held.createAccountContext(QStringLiteral("old"), QStringLiteral("old"));
  auto heldJournal = std::make_unique<MoveOperationJournal>(&held, heldContext);
  const auto heldIntent = heldJournal->begin(1, 1, 7, {7});
  check(heldIntent.accepted && entered.tryAcquire(1, 2000), "held intent never reached I/O worker");
  heldJournal.reset(); heldContext.reset();
  MoveOperationJournal premature(&lateOwner,
      lateOwner.createAccountContext(QStringLiteral("old"), QStringLiteral("old")));
  const auto prematureIntent = premature.begin(2, 1, 8, {8});
  const bool leaseRetained = prematureIntent.accepted &&
      waitUntil([&] { return lateResults.contains(prematureIntent.taskId); }) &&
      lateResults.value(prematureIntent.taskId).status == StorageStatus::LockUnavailable;
  release.release();
  check(leaseRetained, "destroyed journal released old-account lease before its accepted write drained");
  check(waitUntil([&] { return heldResults.contains(heldIntent.taskId); }) &&
        heldResults.value(heldIntent.taskId).status == StorageStatus::Saved,
        "accepted original-account intent was cancelled when journal/context changed");
  if (ok) std::puts("PASS: asynchronous durable intent, shared account lease, frozen context and monotonic journal states");
  return ok ? 0 : 1;
}
