#include "protocol_test_support.h"
#include "pet_move_policy.h"
#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "storage_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QSemaphore>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <algorithm>
#include <functional>
#include <atomic>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 1500) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return predicate();
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
}

// Fixture inspection only. Production recovery consumes StorageService reads
// through MoveOperationJournal::recoveryRecord and has no synchronous scanner.
QList<QJsonObject> unresolvedFixture(const QString& accountDirectory, QStringList* readErrors = nullptr) {
  QList<QJsonObject> records;
  const QDir directory(QDir(accountDirectory).filePath(QStringLiteral("operations")));
  for (const QString& name : directory.entryList({QStringLiteral("*.json")}, QDir::Files)) {
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::ReadOnly)) { if (readErrors) readErrors->append(name + QStringLiteral(": ") + file.errorString()); continue; }
    const auto bytes = file.readAll(); QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes,&error);
    if (file.error() != QFileDevice::NoError || error.error != QJsonParseError::NoError || !document.isObject()) {
      if (readErrors) readErrors->append(name + QStringLiteral(": ") + (file.error() != QFileDevice::NoError ? file.errorString() : error.errorString()));
      continue;
    }
    const QJsonObject record = MoveOperationJournal::recoveryRecord(bytes);
    if (!record.isEmpty()) records.append(record);
  }
  return records;
}

QJsonArray journalFixtureDiagnostics(const QString& accountDirectory) {
  QJsonArray records; const QDir directory(QDir(accountDirectory).filePath(QStringLiteral("operations")));
  for (const auto& name : directory.entryList({QStringLiteral("*.json")},QDir::Files)) {
    QFile file(directory.filePath(name)); QJsonObject diagnostic{{QStringLiteral("file"),name}};
    if (!file.open(QIODevice::ReadOnly)) diagnostic.insert(QStringLiteral("readError"),file.errorString());
    else {
      const auto bytes = file.readAll(); const auto object = QJsonDocument::fromJson(bytes).object();
      for (const auto& key : {"operationId","account","outcome","recordRevision","sessionEpoch","instanceId"}) diagnostic.insert(QString::fromLatin1(key),object.value(QString::fromLatin1(key)));
      diagnostic.insert(QStringLiteral("bytes"),bytes.size());
      diagnostic.insert(QStringLiteral("unresolved"),!MoveOperationJournal::recoveryRecord(bytes).isEmpty());
    }
    records.append(diagnostic);
  }
  return records;
}

// Prints the observable move state next to a failing expectation: a bare FAIL
// line cannot separate a stale expectation from a state-machine defect.
void dumpMoveState(const char* label, const QList<qint64>& pack, const QSet<qint64>& warehouse,
                   int writes, bool succeeded, const QString& result) {
  QStringList packed;
  for (qint64 id : pack) packed.append(QString::number(id));
  QStringList stored;
  for (qint64 id : warehouse) stored.append(QString::number(id));
  std::fprintf(stderr, "  %s: pack=[%s] warehouse=[%s] writes=%d succeeded=%d result='%s'\n",
               label, qPrintable(packed.join(QLatin1Char(','))),
               qPrintable(stored.join(QLatin1Char(','))), writes, int(succeeded),
               qPrintable(result));
}

QJsonObject pet(qint64 id) {  return {{QStringLiteral("id"), id},
          {QStringLiteral("ri"), 7000 + static_cast<int>(id)},
          {QStringLiteral("n"), QStringLiteral("pet-%1").arg(id)},
          {QStringLiteral("lv"), 100},
          {QStringLiteral("zdl"), 30000 + static_cast<int>(id)},
          {QStringLiteral("xzdl"), 30000}};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  std::atomic_bool holdNextDetail{false}, holdNextIntent{false}, failNextIntent{false};
  QSemaphore storageEntered, storageRelease;
  StorageService storage(temporary.path(), {}, [&](const QString& path, const QByteArray& bytes) {
    const QString normalized = QDir::fromNativeSeparators(path);
    const bool detail = normalized.contains(QStringLiteral("/details/"));
    const bool intent = normalized.contains(QStringLiteral("/operations/")) &&
        QJsonDocument::fromJson(bytes).object().value(QStringLiteral("recordRevision")).toString() == QStringLiteral("1");
    if ((detail && holdNextDetail.exchange(false)) || (intent && holdNextIntent.exchange(false))) {
      storageEntered.release(); storageRelease.acquire();
    }
    if (intent && failNextIntent.exchange(false))
      return StorageWriteAttempt{false, 0, QStringLiteral("synthetic intent failure")};
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
    const qint64 written = file.write(bytes);
    return written == bytes.size() && file.commit() ? StorageWriteAttempt{true, written, {}}
                                                    : StorageWriteAttempt{false, written, file.errorString()};
  });
  PetRepository repository(nullptr, &storage);
  PetRefreshController controller(&repository);
  QString observedOperationId;
  QObject::connect(&controller,&PetRefreshController::moveOutcomeChanged,&application,
      [&](const QString& operationId,const QString&,MoveOutcome,const QString&) { if (!operationId.isEmpty()) observedOperationId = operationId; });
  PetRefreshController::Timings timings;
  timings.automaticIntervalMs = 100000;
  timings.listRequestGapMs = 2;
  timings.listTimeoutMs = 100;
  timings.detailRequestGapMs = 2;
  timings.detailTimeoutMs = 50;
  timings.moveRequestTimeoutMs = 150;
  controller.setTimings(timings);

  QList<qint64> pack{1, 2};
  QSet<qint64> warehouse{3};
  QHash<qint64, QJsonObject> pets{{1, pet(1)}, {2, pet(2)}, {3, pet(3)}};
  int capacity = 2;
  int writes = 0;
  int detailRequests = 0;
  bool acknowledgeWrites = true;
  bool deliveringMoveAck = false;
  bool embeddedAckApplied = true;
  int reentrantVerificationSends = 0;
  QObject::connect(&controller,&PetRefreshController::statusChanged,&application,
      [&](const QString& message) {
        std::fprintf(stderr, "STATUS rev=%llu detailReq=%d pendingReads=%d: %s\n",
                     repository.inventoryRevision(), detailRequests, repository.pendingReadCount(),
                     qPrintable(message));
      });

  auto backpackPacket = [&]() {
    QJsonArray list;
    for (qint64 id : pack) list.append(pets.value(id));
    return QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                       {QStringLiteral("pl"), list},
                       {QStringLiteral("pps"),
                        QJsonArray{PetMovePolicy::serializeSequence(pack)}},
                       {QStringLiteral("ppc"), capacity}};
  };
  auto warehousePacket = [&]() {
    QJsonArray normal;
    QList<qint64> ids = warehouse.values();
    std::sort(ids.begin(), ids.end());
    for (qint64 id : ids) normal.append(pets.value(id));
    return QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
                       {QStringLiteral("ns"), normal},
                       {QStringLiteral("rb"), QJsonArray{}},
                       {QStringLiteral("es"), QJsonArray{}}};
  };

  auto applySequenceWrite = [&](const QList<qint64>& next) {
    ++writes;
    for (qint64 id : pack)
      if (!next.contains(id)) warehouse.insert(id);
    for (qint64 id : next) warehouse.remove(id);
    pack = next;
    if (acknowledgeWrites) {
      QTimer::singleShot(1, &repository, [&]() {
        QJsonObject response = backpackPacket();
        response.insert(QStringLiteral("_cmd"), QStringLiteral("2_1_11"));
        response.insert(QStringLiteral("r"), 1);
        deliveringMoveAck = true;
        deliver(&repository, response);
        embeddedAckApplied =
            embeddedAckApplied && repository.backpackIds(0) == pack;
        deliveringMoveAck = false;
      });
    }
  };
  int flashMoves = 0;
  controller.setFlashInvoker([&](const QString& method, const QString& argument) {
    if (method != QStringLiteral("batchpet")) return false;
    ++flashMoves;
    applySequenceWrite(PetMovePolicy::parseSequence(argument));
    return true;
  });
  controller.setSender([&](const QString&, const QString& command,
                           const QString& parameters) {
    if (command == QStringLiteral("2_1_10")) {
      if (deliveringMoveAck) ++reentrantVerificationSends;
      QTimer::singleShot(1, &repository,
                         [&]() { deliver(&repository, backpackPacket()); });
      return true;
    }
    if (command == QStringLiteral("2_1_S")) {
      if (deliveringMoveAck) ++reentrantVerificationSends;
      QTimer::singleShot(1, &repository,
                         [&]() { deliver(&repository, warehousePacket()); });
      return true;
    }
    if (command == QStringLiteral("2_2_10")) return true;
    if (command == QStringLiteral("2_1_R")) {
      ++detailRequests;
      const qint64 id = QJsonDocument::fromJson(parameters.toUtf8())
                            .object().value(QStringLiteral("pi")).toVariant().toLongLong();
      QJsonObject detail = pets.value(id);
      detail.insert(QStringLiteral("r"), detail.value(QStringLiteral("ri")));
      QTimer::singleShot(1, &repository, [&, detail]() {
        deliver(&repository,
                {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
                 {QStringLiteral("p"), detail}});
      });
      return true;
    }
    if (command == QStringLiteral("2_1_11")) {
      const QJsonObject request =
          QJsonDocument::fromJson(parameters.toUtf8()).object();
      applySequenceWrite(PetMovePolicy::parseSequence(
          request.value(QStringLiteral("pps")).toString()));
      return true;
    }
    return false;
  });

  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("move-test")}}}});

  const QStringList relationFields = {
      QStringLiteral("srpi"), QStringLiteral("sepi"), QStringLiteral("sdpi"),
      QStringLiteral("cepi"), QStringLiteral("crpis"), QStringLiteral("asps"),
      QStringLiteral("acps"), QStringLiteral("sppl"), QStringLiteral("cppl")};
  for (const QString& field : relationFields) {
    QJsonObject related = pet(90);
    related.insert(field, 99);
    ok &= require(PetMovePolicy::restriction(related).isEmpty(),
                  "summon/carry/divine relation was still blocked");
  }
  ok &= require(PetMovePolicy::deploymentText(QJsonObject{}) ==
                    QStringLiteral("—") &&
                    PetMovePolicy::deploymentText(
                        QJsonObject{{QStringLiteral("inFormation"), false}}) ==
                        QStringLiteral("否") &&
                    PetMovePolicy::deploymentText(
                        QJsonObject{{QStringLiteral("inTeam"), QStringLiteral("1")}}) ==
                        QStringLiteral("是") &&
                    PetMovePolicy::deploymentText(
                        QJsonObject{{QStringLiteral("isTeamPet"),
                                     QStringLiteral("false")}}) ==
                        QStringLiteral("否"),
                "deployment overview text did not handle tri-state flags");

  bool finished = false;
  bool succeeded = false;
  QString result;
  bool invalidateReplacementOnce = false;
  int replacementPrompts = 0;
  QObject::connect(&controller, &PetRefreshController::moveFinished, &application,
                   [&](bool success, const QString& message) {
                     finished = true;
                     succeeded = success;
                     result = message;
                   });
  QObject::connect(
      &controller, &PetRefreshController::replacementRequired, &application,
      [&](qint64, const QList<qint64>& eligible) {
        ++replacementPrompts;
        std::fprintf(stderr, "PROMPT %d rev=%llu eligible=%d\n", replacementPrompts,
                     repository.inventoryRevision(), int(eligible.size()));
        if (invalidateReplacementOnce) {
          invalidateReplacementOnce = false;
          repository.beginListRefresh(9000, repository.accountKey(), repository.sessionGeneration());
          deliver(&repository, backpackPacket());
        }
        if (eligible.contains(3))
          controller.chooseMoveReplacement(3);
        else if (eligible.contains(2))
          controller.chooseMoveReplacement(2);
      });

  controller.requestMoveToWarehouse(2);
  ok &= require(waitUntil([&]() { return finished; }),
                "move-to-warehouse did not finish");
  if (!(succeeded && pack == QList<qint64>({1}) && warehouse.contains(2)))
    dumpMoveState("move-to-warehouse", pack, warehouse, writes, succeeded, result);
  ok &= require(succeeded && pack == QList<qint64>({1}) && warehouse.contains(2),
                "move-to-warehouse result is incorrect");
  ok &= require(repository.hasCachedDetail(2),
                "outgoing backpack detail was not preserved");
  ok &= require(embeddedAckApplied,
                "embedded 2_1_11 backpack data was not applied immediately");
  ok &= require(reentrantVerificationSends == 0,
                "verification list refresh was sent inside the 2_1_11 response stack");
  ok &= require(flashMoves > 0,
                "move did not go through official Flash batchpet path");

  finished = false;
  controller.requestSingleDetail(3);
  controller.requestMoveToBackpack(3);
  ok &= require(waitUntil([&]() { return finished; }),
                "move-to-backpack did not finish");
  ok &= require(succeeded && pack == QList<qint64>({1, 3}) &&
                    !warehouse.contains(3),
                "move-to-backpack result is incorrect");
  ok &= require(detailRequests == 1 && repository.hasCachedDetail(3),
                "clicked instance detail did not finish before move preflight");

  finished = false;
  invalidateReplacementOnce = true;
  const int promptsBeforeReplacement = replacementPrompts;
  controller.requestMoveToBackpack(2);
  ok &= require(waitUntil([&]() { return finished; }),
                "full-pack replacement did not finish");
  if (!(succeeded && pack == QList<qint64>({1, 2}) && warehouse.contains(3) && !warehouse.contains(2)))
    dumpMoveState("full-pack replacement", pack, warehouse, writes, succeeded, result);
  ok &= require(succeeded && pack == QList<qint64>({1, 2}) &&
                    warehouse.contains(3) && !warehouse.contains(2),
                "full-pack replacement result is incorrect");
  ok &= require(repository.hasCachedDetail(3),
                "replaced backpack detail was not preserved");
  ok &= require(replacementPrompts >= promptsBeforeReplacement + 2,
                "changed list revision did not invalidate replacement selection and redo preflight");

  pets[1].insert(QStringLiteral("srpi"), 99);
  finished = false;
  const int writesBeforeRestriction = writes;
  controller.requestMoveToWarehouse(1);
  ok &= require(waitUntil([&]() { return finished; }),
                "related-pet move did not finish");
  ok &= require(succeeded && writes == writesBeforeRestriction + 1 &&
                    pack == QList<qint64>({2}) && warehouse.contains(1),
                "summon/carry/divine relation was not allowed to move");
  pets[1].remove(QStringLiteral("srpi"));

  pets[2].insert(QStringLiteral("inFormation"), true);
  finished = false;
  const int writesBeforeFormationBlock = writes;
  controller.requestMoveToWarehouse(2);
  ok &= require(waitUntil([&]() { return finished; }),
                "formation restriction did not finish");
  ok &= require(!succeeded && writes == writesBeforeFormationBlock &&
                    result.contains(QStringLiteral("阵型")),
                "active formation pet was not blocked");
  pets[2].remove(QStringLiteral("inFormation"));

  acknowledgeWrites = false;
  finished = false;
  const int writesBeforeTimeout = writes;
  controller.requestMoveToBackpack(3);
  ok &= require(waitUntil([&]() { return finished; }, 2000),
                "write-timeout reconciliation did not finish");
  ok &= require(succeeded && writes == writesBeforeTimeout + 1,
                "timed-out write was resent or not reconciled by reads");

  const auto restoreFixtureSource = [&]() {
    deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("move-test")}}}});
  };
  const auto drainStorage = [&]() {
    return waitUntil([&] { return storage.state().outstandingTasks == 0; }, 2500);
  };
  const auto waitForStorageEntry = [&]() {
    bool entered = false;
    // waitUntil may inspect its predicate again after the loop. Consume a
    // semaphore token once and retain the observation for subsequent checks.
    return waitUntil([&] { entered = entered || storageEntered.tryAcquire(1, 0); return entered; });
  };
  ok &= require(drainStorage(), "baseline storage did not drain");
  const int beforeStorageCases = writes;
  holdNextDetail.store(true);
  finished = false;
  controller.requestMoveToWarehouse(3);
  const bool detailHeld = waitForStorageEntry();
  ok &= require(detailHeld, "detail persistence fixture did not reach its held writer");
  ok &= require(!finished && writes == beforeStorageCases,
                "move submitted before necessary detail was durably saved");
  controller.cancelMove();
  storageRelease.release();
  ok &= require(finished && drainStorage() && writes == beforeStorageCases &&
                    controller.lastMoveOutcome() == MoveOutcome::NotSent,
                "cancel during detail preservation was sent by a late Saved callback");

  holdNextIntent.store(true);
  finished = false;
  controller.requestMoveToWarehouse(3);
  const bool intentHeld = waitForStorageEntry();
  ok &= require(intentHeld, "intent persistence fixture did not reach its held writer");
  ok &= require(!finished && writes == beforeStorageCases,
                "move submitted after intent admission but before Saved");
  const bool timedOutBeforeSend = waitUntil([&] { return finished; }, 2000);
  storageRelease.release();
  ok &= require(timedOutBeforeSend && drainStorage() && writes == beforeStorageCases &&
                    controller.lastMoveOutcome() == MoveOutcome::NotSent,
                "intent persistence timeout or its late completion submitted a write");

  failNextIntent.store(true);
  finished = false;
  controller.requestMoveToWarehouse(3);
  ok &= require(waitUntil([&] { return finished; }) && drainStorage() && writes == beforeStorageCases &&
                    controller.lastMoveOutcome() == MoveOutcome::NotSent,
                "failed intent persistence reached a host write");

  const auto savedIntentReentry = QObject::connect(&controller, &PetRefreshController::movePersistenceChanged,
      &application, [&](const QString&, const QString&, quint64, StorageStatus status, const QString&) {
    if (status == StorageStatus::Saved && controller.moveRunning())
      repository.markSessionUncertain(QStringLiteral("source changed in intent Saved observer"));
  });
  finished = false;
  controller.requestMoveToWarehouse(3);
  ok &= require(waitUntil([&] { return finished; }) && drainStorage() && writes == beforeStorageCases &&
                    controller.lastMoveOutcome() == MoveOutcome::NotSent,
                "Saved observer source loss still triggered host submission");
  QObject::disconnect(savedIntentReentry);
  restoreFixtureSource();

  // A received detail can be waiting on disk longer than the network timeout.
  // Its observation must not cause a second 2_1_R request while I/O is pending.
  ok &= require(drainStorage(), "storage before slow-detail test did not drain");
  holdNextDetail.store(true);
  const int beforeSlowDetail = detailRequests;
  controller.requestSingleDetail(3);
  const bool responseHeld = waitForStorageEntry();
  QElapsedTimer diskWait; diskWait.start();
  waitUntil([&] { return diskWait.elapsed() > timings.detailTimeoutMs * 3; }, 600);
  ok &= require(responseHeld, "received-detail fixture did not reach its held writer");
  ok &= require(detailRequests == beforeSlowDetail + 1,
                "received detail was retried as a network timeout while awaiting persistence");
  storageRelease.release();
  ok &= require(drainStorage() && repository.isDetailPersisted(3),
                "slow detail did not finish after its real Saved completion");

  const int beforeReentry = writes;
  const auto statusReentry = QObject::connect(&controller, &PetRefreshController::statusChanged,
      &application, [&](const QString& message) {
    if (message.startsWith(QStringLiteral("正在将实例")))
      repository.markSessionUncertain(QStringLiteral("synthetic synchronous source loss"));
  });
  finished = false;
  controller.requestMoveToWarehouse(3);
  ok &= require(waitUntil([&]() { return finished; }) && writes == beforeReentry &&
                    controller.lastMoveOutcome() == MoveOutcome::NotSent,
                "status observer source loss still called host or mislabeled intent");
  QObject::disconnect(statusReentry);
  restoreFixtureSource();

  controller.setWriteFlashInvoker([](const QString&, const QString&) {
    return SubmissionOutcome::DefinitelyNotSubmitted;
  });
  const auto commandReentry = QObject::connect(&controller, &PetRefreshController::commandSent,
      &application, [&](const QString& command, qint64, quint64) {
    if (command == QStringLiteral("2_1_11"))
      repository.markSessionUncertain(QStringLiteral("synthetic fallback source loss"));
  });
  finished = false;
  controller.requestMoveToWarehouse(3);
  ok &= require(waitUntil([&]() { return finished; }) && writes == beforeReentry &&
                    controller.lastMoveOutcome() == MoveOutcome::NotSent,
                "command observer source loss still called fallback host");
  QObject::disconnect(commandReentry);
  restoreFixtureSource();

  // A false legacy/ambiguous host result cannot prove non-submission and must
  // never trigger the raw command fallback.
  controller.setWriteFlashInvoker([](const QString&, const QString&) {
    return SubmissionOutcome::Unknown;
  });
  finished = false;
  const int beforeUnknown = writes;
  controller.requestMoveToWarehouse(3);
  ok &= require(waitUntil([&]() { return finished; }), "unknown submission did not reconcile");
  ok &= require(writes == beforeUnknown && !succeeded &&
                    controller.lastMoveOutcome() == MoveOutcome::Unknown,
                "ambiguous host submission retried via fallback or became NotSent");
  const QString originalAccountDirectory = QFileInfo(repository.cachePath()).absolutePath();
  const QString originalAccount = repository.accountKey();
  const QString originalUnknownOperationId = observedOperationId;
  // The move signal describes the UI outcome; accepted journal updates still
  // need their I/O terminal result before this synchronous fixture inspects disk.
  ok &= require(drainStorage(),"unknown-operation storage did not reach its terminal receipt");
  QStringList initialReadErrors;
  const auto initialUnknowns = unresolvedFixture(originalAccountDirectory,&initialReadErrors);
  const bool initialOperationFound = std::any_of(initialUnknowns.begin(),initialUnknowns.end(),[&](const QJsonObject& record) {
    return record.value(QStringLiteral("operationId")).toString() == originalUnknownOperationId && record.value(QStringLiteral("account")).toString() == originalAccount;
  });
  ok &= require(initialReadErrors.isEmpty() && initialOperationFound,
                "unknown operation was not retained on original account");

  controller.setWriteFlashInvoker([](const QString&, const QString&) {
    return SubmissionOutcome::DefinitelyNotSubmitted;
  });
  int terminalNotifications = 0;
  const auto terminalReentry = QObject::connect(&controller, &PetRefreshController::moveOutcomeChanged,
      &application, [&](const QString&, const QString&, MoveOutcome outcome, const QString&) {
    if (outcome == MoveOutcome::Confirmed) {
      ++terminalNotifications;
      controller.cancelMove();
      repository.markSessionUncertain(QStringLiteral("source changed in terminal observer"));
    }
  });
  finished = false;
  controller.requestMoveToWarehouse(3);
  ok &= require(waitUntil([&]() { return finished; }), "definite non-submission fallback did not finish");
  ok &= require(writes == beforeUnknown + 1 && succeeded &&
                    controller.lastMoveOutcome() == MoveOutcome::Confirmed,
                "confirmed non-submission did not permit exactly one fallback write");
  ok &= require(terminalNotifications == 1,
                "terminal observer generated another completion for same operation");
  QObject::disconnect(terminalReentry);
  restoreFixtureSource();

  // The adapter may observe a source boundary synchronously before returning.
  // The already durable original-account intent must then remain Unknown.
  controller.setWriteFlashInvoker([&](const QString&, const QString& argument) {
    applySequenceWrite(PetMovePolicy::parseSequence(argument));
    deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("move-other")}}}});
    return SubmissionOutcome::Submitted;
  });
  finished = false;
  const int beforeSwitch = writes;
  controller.requestMoveToBackpack(1);
  ok &= require(waitUntil([&]() { return finished; }), "account switch did not resolve UI operation");
  ok &= require(!succeeded && writes == beforeSwitch + 1 &&
                    controller.lastMoveOutcome() == MoveOutcome::Unknown,
                "account switch after submission was reported as failure/not-sent or replayed");
  const QString switchedOperationId = observedOperationId;
  const auto beforeJournalDrain = storage.state();
  // Include all already accepted old-account updates in the isolation check;
  // checking the new directory while those writes are queued could miss a leak.
  const bool journalWritesDrained = drainStorage();
  const QString newAccountDirectory = QFileInfo(repository.cachePath()).absolutePath();
  QStringList oldReadErrors,newReadErrors;
  const auto oldUnresolved = unresolvedFixture(originalAccountDirectory,&oldReadErrors);
  const auto newUnresolved = unresolvedFixture(newAccountDirectory,&newReadErrors);
  QSet<QString> originalPendingIds;
  for (const auto& record : oldUnresolved)
    if (record.value(QStringLiteral("account")).toString() == originalAccount) originalPendingIds.insert(record.value(QStringLiteral("operationId")).toString());
  const bool journalsIsolated = journalWritesDrained && repository.accountKey() == QStringLiteral("move-other") &&
      originalAccountDirectory != newAccountDirectory && oldReadErrors.isEmpty() && newReadErrors.isEmpty() &&
      oldUnresolved.size() >= 2 && newUnresolved.isEmpty() && originalUnknownOperationId != switchedOperationId &&
      originalPendingIds.contains(originalUnknownOperationId) && originalPendingIds.contains(switchedOperationId);
  if (!journalsIsolated) {
    const auto queue = storage.state();
    const QJsonObject diagnostic{{QStringLiteral("stage"),QStringLiteral("after-account-switch-storage-terminal")},
        {QStringLiteral("oldDirectory"),originalAccountDirectory},{QStringLiteral("newDirectory"),newAccountDirectory},
        {QStringLiteral("currentAccount"),repository.accountKey()},{QStringLiteral("epoch"),QString::number(repository.sessionGeneration())},
        {QStringLiteral("oldUnresolvedCount"),oldUnresolved.size()},{QStringLiteral("newUnresolvedCount"),newUnresolved.size()},
        {QStringLiteral("storageOutstanding"),queue.outstandingTasks},{QStringLiteral("storageQueued"),queue.queuedTasks},
        {QStringLiteral("storageOutstandingAtUiFinished"),beforeJournalDrain.outstandingTasks},
        {QStringLiteral("storageDrained"),journalWritesDrained},
        {QStringLiteral("expectedOriginalOperationId"),originalUnknownOperationId},{QStringLiteral("expectedSwitchedOperationId"),switchedOperationId},
        {QStringLiteral("oldReadErrors"),QJsonArray::fromStringList(oldReadErrors)},{QStringLiteral("newReadErrors"),QJsonArray::fromStringList(newReadErrors)},
        {QStringLiteral("repositoryWrites"),repository.pendingPersistenceCount()},
        {QStringLiteral("oldJournals"),journalFixtureDiagnostics(originalAccountDirectory)},
        {QStringLiteral("newJournals"),journalFixtureDiagnostics(newAccountDirectory)}};
    std::fprintf(stderr,"MOVE_ISOLATION_STATE %s\n",QJsonDocument(diagnostic).toJson(QJsonDocument::Compact).constData());
    // Diagnose the receipt boundary without rescuing the original assertion:
    // UI completion and a queued record are not themselves a Saved receipt.
    const bool drained = drainStorage();
    const QJsonObject settled{{QStringLiteral("stage"),QStringLiteral("after-diagnostic-storage-drain")},
        {QStringLiteral("drained"),drained},{QStringLiteral("oldDirectory"),originalAccountDirectory},
        {QStringLiteral("newDirectory"),QFileInfo(repository.cachePath()).absolutePath()},
        {QStringLiteral("oldUnresolvedCount"),unresolvedFixture(originalAccountDirectory).size()},
        {QStringLiteral("newUnresolvedCount"),unresolvedFixture(QFileInfo(repository.cachePath()).absolutePath()).size()},
        {QStringLiteral("storageOutstanding"),storage.state().outstandingTasks},
        {QStringLiteral("oldJournals"),journalFixtureDiagnostics(originalAccountDirectory)},
        {QStringLiteral("newJournals"),journalFixtureDiagnostics(QFileInfo(repository.cachePath()).absolutePath())}};
    std::fprintf(stderr,"MOVE_ISOLATION_STATE %s\n",QJsonDocument(settled).toJson(QJsonDocument::Compact).constData());
  }
  ok &= require(journalsIsolated,
                "original account pending operations leaked into new account");

  if (!ok) return 1;
  std::fprintf(stdout,
               "PASS: safe pet movement, async verification, relations and deployment state\n");
  return 0;
}
