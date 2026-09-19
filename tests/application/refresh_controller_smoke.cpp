#include "support/protocol_test_support.h"
#include "application/pet/pet_refresh_controller.h"
#include "application/pet/pet_repository.h"
#include "application/common/controller_cache_storage.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QSemaphore>
#include <QLockFile>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

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

QJsonObject warehousePacket() {
  QJsonArray elite;
  for (int id = 101; id <= 103; ++id) {
    elite.append(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("ri"), 7000},
                             {QStringLiteral("n"), QStringLiteral("same-pet")},
                             {QStringLiteral("lv"), 100}});
  }
  return {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
          {QStringLiteral("ns"), QJsonArray{}},
          {QStringLiteral("rb"), QJsonArray{}},
          {QStringLiteral("es"), elite}};
}

bool asynchronousSettingsTest(const QString& root) {
  QDir().mkpath(root);
  const QString settingsPath = QDir(root).filePath(QStringLiteral("settings.json"));
  QFile fixture(settingsPath);
  bool ok = require(fixture.open(QIODevice::WriteOnly), "settings race fixture did not open");
  fixture.write("{\"schema\":1,\"automaticIntervalMs\":77,\"unknown\":{\"keep\":true}}");
  fixture.close();
  StorageLimits limits;
  limits.maximumOutstandingTasks = 1;
  StorageService storage(root, limits);
  QSemaphore entered, release;
  const bool held = storage.postAuxiliary([&](QObject*) { entered.release(); release.acquire(); });
  ok &= require(held && entered.tryAcquire(1, 2000), "settings race fixture did not hold IO");
  PetRepository repository(nullptr, &storage, QDir(root).filePath(QStringLiteral("legacy")));
  PetRefreshController controller(&repository);
  int savedCount = 0;
  int failedCount = 0;
  QObject::connect(&controller, &PetRefreshController::timingsPersistenceChanged, &controller,
      [&](quint64, StorageStatus status, const QString&) {
        if (status == StorageStatus::Saved) ++savedCount;
        if (status == StorageStatus::WriteFailed) ++failedCount;
      });
  PetRefreshController::Timings timings;
  for (int edit = 0; edit < 100; ++edit) {
    timings.automaticIntervalMs = 9000 + edit;
    controller.setTimings(timings);
  }
  ok &= require(controller.timingsKnown() && controller.timings().automaticIntervalMs == 9099 &&
                    savedCount == 0 && controller.pendingSettingsCount() <= 2 && storage.state().outstandingTasks == 1,
                "settings edit waited for IO, reported Saved early or accumulated unbounded snapshots");
  release.release();
  ok &= require(waitUntil([&] { return !controller.timingsPending(); }, 5000) && savedCount == 1 &&
                    controller.timings().automaticIntervalMs == 9099,
                "late settings read overwrote user changes or final coalesced edit was not persisted");
  fixture.open(QIODevice::ReadOnly);
  const QJsonObject persisted = QJsonDocument::fromJson(fixture.readAll()).object();
  fixture.close();
  ok &= require(persisted.value(QStringLiteral("automaticIntervalMs")).toInt() == 9099 &&
                    persisted.value(QStringLiteral("unknown")).toObject().value(QStringLiteral("keep")).toBool(),
                "shared settings merge lost the latest edit or unknown nested fields");
  fixture.open(QIODevice::WriteOnly | QIODevice::Truncate);
  fixture.write("invalid-existing-settings");
  fixture.close();
  timings.automaticIntervalMs = 10001;
  controller.setTimings(timings);
  ok &= require(waitUntil([&] { return !controller.timingsPending(); }, 5000) && failedCount == 1 && savedCount == 1 &&
                    !controller.timingsStorageError().isEmpty() && controller.timings().automaticIntervalMs == 10001,
                "invalid existing settings did not fail visibly while keeping the in-memory edit");
  fixture.open(QIODevice::ReadOnly);
  ok &= require(fixture.readAll() == QByteArray("invalid-existing-settings"),
                "failed settings merge overwrote the old file");
  fixture.close();
  ok &= require(storage.shutdown(), "settings race storage did not drain");
  return ok;
}

bool controllerCacheAccountDrainTest(const QString& root) {
  QSemaphore entered, release;
  StorageService storage(root, {}, [&](const QString& path, const QByteArray& bytes) {
    if (path.contains(QStringLiteral("/accounts/A/")) && path.endsWith(QStringLiteral("record.json"))) {
      entered.release(); release.acquire();
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
    const qint64 size = file.write(bytes);
    return StorageWriteAttempt{size == bytes.size() && file.commit(), size, file.errorString()};
  });
  ControllerCacheStorage cache(&storage, QStringLiteral("record.json"), 1024);
  QString currentAccount = QStringLiteral("A");
  int value = 11;
  cache.canWrite = [] { return true; };
  cache.snapshot = [&] { return QJsonObject{{QStringLiteral("account"), currentAccount}, {QStringLiteral("value"), value}}; };
  QList<QString> savedAccounts;
  cache.changed = [&](const QString& account, quint64 epoch, quint64, StorageStatus status, const QString&) {
    if (status == StorageStatus::Saved) savedAccounts.append(account + QString::number(epoch));
  };
  auto account = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  const QString oldPath = QDir(account->directory()).filePath(QStringLiteral("record.json"));
  const QString lockPath = QDir(account->directory()).filePath(QStringLiteral(".storage-writer.lock"));
  cache.start(account, currentAccount, 1);
  bool ok = require(waitUntil([&] { return !cache.loading(); }), "account cache initial read did not complete");
  cache.save();
  ok &= require(entered.tryAcquire(1, 2000), "old account write did not begin");
  currentAccount = QStringLiteral("B");
  value = 22;
  account = storage.createAccountContext(QStringLiteral("B"), QStringLiteral("B"));
  const QString newPath = QDir(account->directory()).filePath(QStringLiteral("record.json"));
  cache.start(account, currentAccount, 2);
  cache.save();
  QLockFile competing(lockPath);
  competing.setStaleLockTime(0);
  const bool stolen = competing.tryLock(0);
  ok &= require(!stolen && savedAccounts.isEmpty(), "switching account released the old in-flight lease or claimed an early save");
  if (stolen) competing.unlock();
  release.release();
  ok &= require(waitUntil([&] { return cache.pendingCount() == 0; }, 5000), "account cache writes did not drain");
  const auto read = [](const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
  };
  ok &= require(read(oldPath).value(QStringLiteral("value")).toInt() == 11 &&
                    read(oldPath).value(QStringLiteral("account")).toString() == QStringLiteral("A") &&
                    read(newPath).value(QStringLiteral("value")).toInt() == 22 &&
                    savedAccounts.contains(QStringLiteral("A1")) && savedAccounts.contains(QStringLiteral("B2")),
                "account switch retargeted accepted JSON or corrupted completion identity");
  ok &= require(storage.shutdown(), "account cache storage did not drain");
  return ok;
}

bool weakDetailBatchTest(const QString& root) {
  StorageService storage(root);
  PetRecordCacheLimits rawLimits;
  rawLimits.maximumRecords = 1;
  PetRepository repository(nullptr, &storage, QDir(root).filePath(QStringLiteral("legacy")), rawLimits);
  PetRefreshController controller(&repository);
  PetRefreshController::Timings timings;
  timings.automaticIntervalMs = 100000;
  timings.detailRequestGapMs = 1;
  timings.detailBatchRestMs = 1;
  timings.detailTimeoutMs = 200;
  timings.detailMaxRetries = 2;
  controller.setTimings(timings);
  const auto deliverWeak = [&](const QJsonObject& packet) {
    InboundEnvelope envelope;
    envelope.receiveSequence = repository.lastInboundSequence() + 1;
    envelope.receivedMonotonicMs = transportMonotonicMs();
    envelope.method = QStringLiteral("recivedata");
    envelope.payload = QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
    repository.handleEnvelope(envelope);
  };
  deliverWeak({{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("weak-batch")}}}});
  repository.expectListPart(QStringLiteral("2_1_S"), 1, repository.accountKey(), repository.sessionGeneration());
  deliverWeak(warehousePacket());
  bool ok = require(repository.isAuthenticated() && !repository.sessionContext().canPersist() &&
      repository.warehouseIdsByDetailAge().size() == 3,
      "weak batch fixture did not establish an identified read-only warehouse session");
  // The production derivation consumer acknowledges each record after compute.
  // Keep this controller fixture small while exercising the same release point.
  QObject::connect(&repository, &PetRepository::rawRecordAvailable, &repository,
      [&](const RawPetRecordHandle& record) { if (record && record->complete) repository.markRecordDerived(record->key); });
  QList<qint64> sent;
  int succeeded = 0, failed = 0, saved = 0;
  QStringList statuses;
  QObject::connect(&controller, &PetRefreshController::statusChanged, &controller,
      [&](const QString& status) { statuses.append(status); });
  QObject::connect(&repository, &PetRepository::detailResponseAccepted, &repository,
      [&](qint64, quint64) { ++saved; });
  QObject::connect(&controller, &PetRefreshController::detailRequestFinished, &controller,
      [&](qint64 id, bool success, const QString& reason) {
    if (success) ++succeeded; else ++failed;
    ok &= require(success && reason.isEmpty() && repository.isDetailPersisted(id) &&
        !repository.recordVersion(id).sourceKnown && !repository.sessionContext().canPersist(),
        "observed detail was not saved or local persistence promoted source/write trust");
  });
  controller.setSender([&](const QString&, const QString& command, const QString& parameters) {
    if (command != QStringLiteral("2_1_R")) return false;
    const qint64 id = QJsonDocument::fromJson(parameters.toUtf8()).object().value(QStringLiteral("pi")).toInteger();
    sent.append(id);
    QTimer::singleShot(10, &repository, [&, id] {
      deliverWeak({{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
          {QStringLiteral("p"), QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("r"), 7000},
              {QStringLiteral("lv"), 100}, {QStringLiteral("zdl"), 30000 + id}}}});
    });
    return true;
  });
  bool paused = false;
  QObject::connect(&controller, &PetRefreshController::detailProgressChanged, &controller,
      [&](bool running, bool, int completed, int, int, int, qint64, int) {
    if (running && completed == 1 && !paused) { paused = true; controller.pauseWarehouseDetailRefresh(); }
  });
  controller.startWarehouseDetailRefresh();
  ok &= require(waitUntil([&] { return controller.detailBatchPaused(); }) && succeeded == 1,
      "weak batch was blocked or could not pause after a successful read");
  waitUntil([] { return false; }, 25);
  ok &= require(sent.size() == 1, "paused weak batch continued requesting ordinary details");
  controller.resumeWarehouseDetailRefresh();
  ok &= require(waitUntil([&] { return !controller.detailBatchRunning(); }) &&
      sent == QList<qint64>{101, 102, 103} && succeeded == 3 && failed == 0 && saved == 3 &&
      repository.rawCacheStats().evictedRecords >= 2,
      "resumed weak batch did not finish serially, retried unsaved reads, or retained every session-only raw page");
  ok &= require(statuses.last().contains(QStringLiteral("已保存 3，失败 0")),
      "observed batch summary did not reflect completed local saves");
  controller.startWarehouseDetailRefreshForIds({});
  controller.startWarehouseDetailRefreshForIds({-1, 9999});
  ok &= require(!controller.detailBatchRunning() && sent.size() == 3,
      "empty or non-warehouse selected IDs expanded into a full batch");
  paused = false;
  QList<qint64> selectedIds{103, 101, 103, 9999};
  controller.startWarehouseDetailRefreshForIds(selectedIds);
  selectedIds = {102};
  ok &= require(waitUntil([&] { return controller.detailBatchPaused(); }) && sent.size() == 4 && sent.last() == 101,
      "selected batch did not preserve warehouse age ordering or pause after its first selected member");
  controller.resumeWarehouseDetailRefresh();
  ok &= require(waitUntil([&] { return !controller.detailBatchRunning(); }) &&
      sent == QList<qint64>{101,102,103,101,103} && succeeded == 5,
      "selected batch broadened after caller mutation/pause or sent duplicate/non-warehouse IDs");
  controller.startWarehouseDetailRefresh();
  ok &= require(waitUntil([&] { return sent.size() == 6; }), "weak cancel fixture did not start");
  controller.cancelWarehouseDetailRefresh();
  waitUntil([] { return false; }, 35);
  ok &= require(!controller.detailBatchRunning() && sent.size() == 6 && succeeded == 5 && failed == 0,
      "cancelled weak batch accepted its late response or continued sending");
  repository.markSessionUncertain(QStringLiteral("test ingress overflow"));
  controller.startWarehouseDetailRefresh();
  ok &= require(!controller.detailBatchRunning() && sent.size() == 6 && !repository.isAuthenticated(),
      "unhealthy weak session still allowed a new batch");
  ok &= require(waitForRepositoryIdle(&repository) && waitUntil([&] { return !controller.timingsPending(); }),
      "weak batch repository did not settle");
  const QDir accountDirectory(QFileInfo(repository.cachePath()).absolutePath());
  const QDir detailDirectory(accountDirectory.filePath(QStringLiteral("details")));
  ok &= require(detailDirectory.entryList({QStringLiteral("*.json")}, QDir::Files).size() == 3 &&
      QFile::exists(repository.cachePath()) && QFile::exists(QDir(root).filePath(QStringLiteral("last-account.txt"))),
      "observed details/list/account hint were not retained or repeated refresh created history copies");
  QFile latest(detailDirectory.filePath(QStringLiteral("103.json")));
  ok &= require(latest.open(QIODevice::ReadOnly), "observed detail cache did not open");
  const auto latestObject = QJsonDocument::fromJson(latest.readAll()).object();
  ok &= require(latestObject.value(QStringLiteral("schema")).toInt() == 4 &&
      latestObject.value(QStringLiteral("trust")).toString() == QStringLiteral("read-only-observation"),
      "observed detail omitted the persistent trust marker");
  ok &= require(storage.shutdown(), "weak batch storage did not drain");
  return ok;
}


bool weakBatchTimeoutPauseTest(const QString& root) {
  StorageService storage(root);
  PetRepository repository(nullptr, &storage, QDir(root).filePath(QStringLiteral("legacy")));
  PetRefreshController controller(&repository);
  PetRefreshController::Timings timings;
  timings.automaticIntervalMs = 100000;
  timings.detailRequestGapMs = 1;
  timings.detailBatchRestMs = 1;
  timings.detailTimeoutMs = 20;
  timings.detailMaxRetries = 1;
  controller.setTimings(timings);
  const auto deliverWeak = [&](const QJsonObject& packet) {
    InboundEnvelope envelope;
    envelope.receiveSequence = repository.lastInboundSequence() + 1;
    envelope.receivedMonotonicMs = transportMonotonicMs();
    envelope.method = QStringLiteral("recivedata");
    envelope.payload = QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
    repository.handleEnvelope(envelope);
  };
  deliverWeak({{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("weak-batch-timeouts")}}}});
  QJsonArray pets;
  for (qint64 id = 101; id <= 109; ++id)
    pets.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("ri"), 7000}, {QStringLiteral("lv"), 100}});
  repository.expectListPart(QStringLiteral("2_1_S"), 1, repository.accountKey(), repository.sessionGeneration());
  deliverWeak({{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
      {QStringLiteral("ns"), pets}, {QStringLiteral("rb"), QJsonArray{}}, {QStringLiteral("es"), QJsonArray{}}});
  QObject::connect(&repository, &PetRepository::rawRecordAvailable, &repository,
      [&](const RawPetRecordHandle& record) { if (record && record->complete) repository.markRecordDerived(record->key); });
  enum class Replies { Intermittent, Reject, Malformed, ManualTimeouts };
  Replies replies = Replies::Intermittent;
  QList<qint64> sent;
  int finished = 0, succeeded = 0, failed = 0;
  bool sawPaused = false;
  QString lastStatus;
  QObject::connect(&controller, &PetRefreshController::detailRequestFinished, &controller,
      [&](qint64, bool success, const QString&) {
    ++finished;
    if (success) ++succeeded; else ++failed;
  });
  QObject::connect(&controller, &PetRefreshController::detailProgressChanged, &controller,
      [&](bool, bool paused, int, int, int, int, qint64, int) { sawPaused |= paused; });
  QObject::connect(&controller, &PetRefreshController::statusChanged, &controller,
      [&](const QString& status) { lastStatus = status; });
  controller.setSender([&](const QString&, const QString& command, const QString& parameters) {
    if (command != QStringLiteral("2_1_R")) return false;
    const qint64 id = QJsonDocument::fromJson(parameters.toUtf8()).object().value(QStringLiteral("pi")).toInteger();
    sent.append(id);
    if ((replies == Replies::Intermittent && id != 103 && id != 109) ||
        (replies == Replies::ManualTimeouts && id <= 103)) return true;
    QTimer::singleShot(1, &repository, [&, id, kind = replies] {
      QJsonObject packet{{QStringLiteral("_cmd"), QStringLiteral("2_1_R")}};
      if (kind == Replies::Reject) packet.insert(QStringLiteral("r"), 0);
      else {
        QJsonObject detail{{QStringLiteral("id"), id}, {QStringLiteral("r"), 7000}, {QStringLiteral("zdl"), 30000 + id}};
        if (kind != Replies::Malformed) detail.insert(QStringLiteral("lv"), 100);
        packet.insert(QStringLiteral("p"), detail);
      }
      deliverWeak(packet);
    });
    return true;
  });
  controller.startWarehouseDetailRefresh();
  bool ok = require(waitUntil([&] { return controller.detailBatchPaused(); }) &&
      controller.detailBatchRunning() && finished == 6 && succeeded == 1 && failed == 5 &&
      sent == QList<qint64>{101, 101, 102, 102, 103, 104, 104, 105, 105, 106, 106},
      "consecutive timeouts did not pause after retries, or an intervening success failed to reset the count");
  ok &= require(lastStatus.contains(QStringLiteral("连续 3 只")) && lastStatus.contains(QStringLiteral("继续")),
      "timeout pause did not explain that the remaining queue can be resumed");
  const int sendsAtPause = sent.size();
  waitUntil([] { return false; }, 70);
  ok &= require(sent.size() == sendsAtPause && finished == 6,
      "automatically paused batch continued sending or discarded completion counts");
  sawPaused = false;
  controller.resumeWarehouseDetailRefresh();
  ok &= require(waitUntil([&] { return !controller.detailBatchRunning(); }) && !sawPaused &&
      finished == 9 && succeeded == 2 && failed == 7 &&
      sent == QList<qint64>{101, 101, 102, 102, 103, 104, 104, 105, 105, 106, 106, 107, 107, 108, 108, 109},
      "resume lost remaining instances or failed to reset the timeout streak");
  ok &= require(controller.timings().detailTimeoutMs == 20 && controller.timings().detailMaxRetries == 1 &&
      controller.timings().detailRequestGapMs == 1, "automatic pause changed the user's refresh timings");

  for (const Replies mode : {Replies::Reject, Replies::Malformed}) {
    replies = mode;
    sent.clear(); finished = succeeded = failed = 0; sawPaused = false;
    controller.startWarehouseDetailRefresh();
    ok &= require(waitUntil([&] { return !controller.detailBatchRunning(); }) && !sawPaused &&
        finished == 9 && failed == 9 && succeeded == 0 && sent.size() == 18,
        "explicit server rejection or malformed detail incorrectly triggered a network-timeout pause");
    if (controller.detailBatchRunning()) controller.cancelWarehouseDetailRefresh();
  }

  replies = Replies::ManualTimeouts;
  sent.clear(); finished = succeeded = failed = 0; sawPaused = false;
  controller.requestSingleDetail(101);
  controller.requestSingleDetail(102);
  controller.requestSingleDetail(103);
  controller.startWarehouseDetailRefresh();
  ok &= require(waitUntil([&] { return !controller.detailBatchRunning(); }) && !sawPaused &&
      finished == 9 && failed == 3 && succeeded == 6 && sent.size() == 12,
      "three manually selected detail timeouts paused the remaining ordinary batch");
  if (controller.detailBatchRunning()) controller.cancelWarehouseDetailRefresh();
  ok &= require(waitForRepositoryIdle(&repository) && waitUntil([&] { return !controller.timingsPending(); }),
      "timeout pause fixture did not drain repository and settings I/O");
  ok &= require(storage.shutdown(), "timeout pause storage did not drain");
  return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  PetRepository repository;
  PetRefreshController controller(&repository);
  PetRefreshController::Timings timings;
  timings.automaticIntervalMs = 40;
  timings.listRequestGapMs = 20;
  timings.listTimeoutMs = 150;
  timings.detailRequestGapMs = 10;
  timings.detailBatchRestMs = 20;
  timings.detailTimeoutMs = 80;
  timings.detailBatchSize = 2;
  timings.detailMaxRetries = 1;
  controller.setTimings(timings);

  QElapsedTimer clock;
  clock.start();
  QList<QString> listCommands;
  QList<qint64> listCommandTimes;
  QList<qint64> detailOrder;
  int detailOutstanding = 0;
  int maxDetailOutstanding = 0;
  bool listCycleOutstanding = false;
  bool listOverlap = false;
  int warehouseResponses = 0;
  int detailResponseDelayMs = 4;

  controller.setSender([&](const QString&, const QString& command, const QString& parameters) {
    if (command == QStringLiteral("2_1_10")) {
      if (listCycleOutstanding) listOverlap = true;
      listCycleOutstanding = true;
      listCommands.append(command);
      listCommandTimes.append(clock.elapsed());
      QTimer::singleShot(4, &repository, [&repository]() {
        deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                              {QStringLiteral("pl"), QJsonArray{}},
                              {QStringLiteral("pps"), QJsonArray{}}});
      });
      return true;
    }
    if (command == QStringLiteral("2_1_S")) {
      listCommands.append(command);
      listCommandTimes.append(clock.elapsed());
      QTimer::singleShot(35, &repository, [&]() {
        ++warehouseResponses;
        listCycleOutstanding = false;
        deliver(&repository, warehousePacket());
      });
      return true;
    }
    if (command == QStringLiteral("2_2_10")) return true;
    if (command == QStringLiteral("2_1_R")) {
      const QJsonObject request = QJsonDocument::fromJson(parameters.toUtf8()).object();
      const qint64 id = request.value(QStringLiteral("pi")).toVariant().toLongLong();
      detailOrder.append(id);
      ++detailOutstanding;
      maxDetailOutstanding = qMax(maxDetailOutstanding, detailOutstanding);
      QTimer::singleShot(detailResponseDelayMs, &repository, [&, id]() {
        --detailOutstanding;
        deliver(&repository,
                {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
                 {QStringLiteral("p"),
                  QJsonObject{{QStringLiteral("id"), id},
                              {QStringLiteral("r"), 7000},
                              {QStringLiteral("n"), QStringLiteral("same-pet")},
                              {QStringLiteral("lv"), 100},
                              {QStringLiteral("zdl"), 30000 + static_cast<int>(id)},
                              {QStringLiteral("xzdl"), 29000}}}});
      });
      return true;
    }
    return false;
  });

  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                        {QStringLiteral("info"),
                         QJsonObject{{QStringLiteral("n"), QStringLiteral("account-test")}}}});
  controller.requestManualListRefresh();
  ok &= require(waitUntil([&]() { return !controller.listRefreshRunning() && warehouseResponses >= 1; }),
                "manual list refresh did not complete");
  ok &= require(listCommands.size() >= 2 &&
                    listCommands.at(0) == QStringLiteral("2_1_10") &&
                    listCommands.at(1) == QStringLiteral("2_1_S"),
                "list refresh command order is incorrect");
  ok &= require(listCommandTimes.at(1) - listCommandTimes.at(0) >= 15,
                "warehouse request was not delayed after backpack request");

  waitUntil([] { return false; }, 120);
  ok &= require(warehouseResponses == 1 && !listOverlap,
                "old automatic interval still triggered an unrequested list refresh");

  timings.automaticIntervalMs = 40;
  controller.setTimings(timings);
  detailOrder.clear();
  bool pauseIssued = false;
  QObject::connect(&controller, &PetRefreshController::detailProgressChanged,
                   &application,
                   [&](bool running, bool paused, int completed, int, int, int, qint64, int) {
    if (running && !paused && completed == 1 && !pauseIssued) {
      pauseIssued = true;
      controller.pauseWarehouseDetailRefresh();
    }
  });

  controller.startWarehouseDetailRefresh();
  const int warehouseResponsesAtBatchStart = warehouseResponses;
  QTimer::singleShot(1, &controller, [&controller]() { controller.requestSingleDetail(103); });
  ok &= require(waitUntil([&]() {
                  return controller.detailBatchPaused() && detailOrder.size() >= 2;
                }), "batch did not pause after current/priority detail requests");
  ok &= require(detailOrder.size() >= 2 && detailOrder.at(0) == 101 && detailOrder.at(1) == 103,
                "clicked detail did not jump ahead of the batch queue");
  const int pausedSendCount = detailOrder.size();
  waitUntil([&]() { return false; }, 45);
  ok &= require(detailOrder.size() == pausedSendCount,
                "paused batch continued sending ordinary queued details");
  ok &= require(warehouseResponses == warehouseResponsesAtBatchStart,
                "automatic list refresh ran while warehouse detail batch was paused");
  controller.resumeWarehouseDetailRefresh();
  ok &= require(waitUntil([&]() { return !controller.detailBatchRunning(); }),
                "resumed detail batch did not complete");
  ok &= require(detailOrder == QList<qint64>({101, 103, 102}),
                "detail batch order or deduplication is incorrect");
  ok &= require(maxDetailOutstanding == 1,
                "detail queue sent more than one request concurrently");

  const int warehouseResponsesAtBatchFinish = warehouseResponses;
  waitUntil([&]() { return false; }, 25);
  ok &= require(warehouseResponses == warehouseResponsesAtBatchFinish,
                "automatic countdown did not restart from the detail batch finish time");
  waitUntil([] { return false; }, 120);
  ok &= require(warehouseResponses == warehouseResponsesAtBatchFinish,
                "finishing a manual detail batch restarted automatic list queries");

  timings.automaticIntervalMs = 10000;
  controller.setTimings(timings);
  detailResponseDelayMs = 50;
  detailOrder.clear();
  controller.startWarehouseDetailRefresh();
  ok &= require(waitUntil([&]() { return !detailOrder.isEmpty(); }),
                "cancel test did not start a detail request");
  controller.cancelWarehouseDetailRefresh();
  const int sendsAtCancel = detailOrder.size();
  waitUntil([&]() { return false; }, 90);
  ok &= require(!controller.detailBatchRunning() && detailOrder.size() == sendsAtCancel,
                "cancelled batch continued sending requests");

  QFile settingsFile(QDir(temporary.path()).filePath(QStringLiteral("settings.json")));
  ok &= require(waitUntil([&] { return !controller.timingsPending(); }),
                "asynchronous refresh settings did not complete");
  ok &= require(settingsFile.open(QIODevice::ReadOnly),
                "refresh settings were not persisted");
  const QJsonObject savedSettings =
      QJsonDocument::fromJson(settingsFile.readAll()).object();
  ok &= require(savedSettings.value(QStringLiteral("detailBatchSize")).toInt() == 2 &&
                    savedSettings.value(QStringLiteral("automaticIntervalMs")).toInt() == 10000,
                "persisted refresh settings do not match active timings");
  ok &= asynchronousSettingsTest(QDir(temporary.path()).filePath(QStringLiteral("async-settings")));
  ok &= controllerCacheAccountDrainTest(QDir(temporary.path()).filePath(QStringLiteral("account-cache-drain")));
  ok &= weakDetailBatchTest(QDir(temporary.path()).filePath(QStringLiteral("weak-detail-batch")));
  ok &= weakBatchTimeoutPauseTest(QDir(temporary.path()).filePath(QStringLiteral("weak-batch-timeouts")));

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: timed list refresh and serial detail queue\n");
  return 0;
}
