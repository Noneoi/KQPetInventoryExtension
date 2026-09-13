#include "application_runtime.h"
#include "inventory_projection.h"
#include "analysis_projection.h"
#include "pet_repository.h"
#include "pet_refresh_controller.h"
#include "shop_exchange_controller.h"
#include "routine_overview_controller.h"
#include "asset_analysis_controller.h"
#include "storage_service.h"
#include "persistence_summary.h"
#include "protocol_test_support.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdio>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
bool waitUntil(const std::function<bool()>& condition, int timeout = 3500) {
  QElapsedTimer clock; clock.start();
  while (!condition() && clock.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return condition();
}
InboundEnvelope envelope(const QJsonObject& packet, const SessionSourceEvidence& source,
                          quint64 sequence, quint64 epoch) {
  InboundEnvelope result;
  result.receiveSequence = sequence;
  result.receivedMonotonicMs = sequence;
  result.capturedSessionEpoch = epoch;
  result.source = source;
  result.orderedObservation = true;
  result.orderEvidenceToken = QStringLiteral("runtime-fixture-order-%1").arg(sequence);
  result.method = QStringLiteral("recivedata");
  result.payload = QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
  return result;
}

bool ownershipAndRouting() {
  QTemporaryDir root, legacy;
  qputenv("KQPET_OFFICIAL_UNPACK_ROOT", legacy.path().toUtf8());
  InventoryProjection inventory(root.path(), nullptr);
  AnalysisProjection analysis;
  std::atomic_bool valid{true}, inspected{false}, correctOwner{false}, acceptedCacheRead{false};
  std::atomic<int> revocations{0}, staleActions{0};
  RuntimeOptions options;
  options.dataRoot = root.path(); options.legacyDataRoot = legacy.path();
  options.inventoryProjection = &inventory; options.analysisProjection = &analysis;
  options.sender = [](const OutboundIntent&) { return false; };
  options.inputValid = [&] { return valid.load(); };
  options.revokePending = [&] { ++revocations; };
  ApplicationRuntime runtime(options);
  bool ready = false, stopped = false, clean = false, guiOwner = true, rejected = false;
  RuntimeShopState shop;
  RuntimeRoutineState routine;
  QObject::connect(&runtime, &ApplicationRuntime::ready, &runtime, [&] {
    ready = true; guiOwner &= QThread::currentThread() == inventory.thread();
  });
  QObject::connect(&runtime, &ApplicationRuntime::stopped, &runtime, [&](bool value) { stopped = true; clean = value; });
  QObject::connect(&runtime, &ApplicationRuntime::shopStateChanged, &runtime, [&](const RuntimeShopState& value) {
    shop = value; guiOwner &= QThread::currentThread() == inventory.thread();
  });
  QObject::connect(&runtime, &ApplicationRuntime::routineStateChanged, &runtime, [&](const RuntimeRoutineState& value) {
    routine = value; guiOwner &= QThread::currentThread() == inventory.thread();
  });
  QObject::connect(&runtime, &ApplicationRuntime::commandRejected, &runtime,
      [&](const QString& account, quint64, const QString&) { if (account == QStringLiteral("A")) rejected = true; });
  bool ok = check(!runtime.postUnscoped([](const CoreServices&) {}), "work was admitted before receiver readiness");
  ok &= check(runtime.start() && !runtime.start() && waitUntil([&] { return ready; }), "Core did not initialize exactly once");
  ok &= check(runtime.postUnscoped([&](const CoreServices& services) {
    const QThread* core = QThread::currentThread();
    bool correct = core != inventory.thread();
    for (QObject* object : QList<QObject*>{services.root, services.storage, services.repo, services.refresh,
                                           services.shop, services.routine, services.analysis})
      correct &= object && object->thread() == core;
    for (QObject* object : services.root->findChildren<QObject*>()) correct &= object->thread() == core;
    correct &= services.repo->dataRoot() == root.path();
    correctOwner.store(correct); inspected.store(true);
  }) && waitUntil([&] { return inspected.load(); }) && correctOwner.load(),
      "Core services or their QObject children were created on GUI or lost the frozen data root");

  const SessionSourceEvidence source{QStringLiteral("A"), QStringLiteral("runtime-fixture-page"), 1, true, true,
                                      QStringLiteral("synthetic Runtime test boundary")};
  ok &= check(runtime.postUnscoped([source](const CoreServices& services) { services.repo->setSessionSourceEvidence(source); }),
              "source-boundary setup was rejected");
  ok &= check(runtime.deliverPacket(envelope({{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("A")}}}}, source, 1, 0)),
      "captured login envelope was rejected at runtime admission");
  ok &= check(waitUntil([&] { return inventory.accountKey() == QStringLiteral("A") && inventory.isAuthenticated() &&
                                       analysis.accountKey() == QStringLiteral("A"); }),
              "Core account state did not reach both GUI projections");
  const quint64 epochA = inventory.snapshot()->sessionEpoch;
  int summaryDeliveries = 0;
  RuntimePersistenceState summary;
  QObject::connect(&runtime, &ApplicationRuntime::persistenceChanged, &runtime,
      [&](const RuntimePersistenceState& value) { ++summaryDeliveries; summary = value; });
  std::atomic_bool burstDone{false};
  ok &= check(runtime.post(QStringLiteral("A"), epochA, [&](const CoreServices& services) {
    emit services.repo->persistenceChanged(QStringLiteral("A"), QStringLiteral("failed.json"), 5,
                                           StorageStatus::WriteFailed, QStringLiteral("injected write failure"), epochA);
    for (int index = 0; index < 1000; ++index)
      emit services.repo->persistenceChanged(QStringLiteral("A"), QStringLiteral("other-%1.json").arg(index), index + 10,
                                             StorageStatus::Saved, {}, epochA);
    burstDone.store(true);
  }), "persistence burst was not admitted");
  QElapsedTimer burstWait; burstWait.start();
  while (!burstDone.load() && burstWait.elapsed() < 3000) QThread::msleep(1);
  ok &= check(burstDone.load() && waitUntil([&] { return summary.failedRecords == 1; }) &&
      summaryDeliveries < 10 && revocations.load() == 0 && summary.error.contains(QStringLiteral("failed.json")),
      "persistence burst overflowed the GUI queue or another Saved erased a failure");
  ok &= check(runtime.post(QStringLiteral("A"), epochA, [epochA](const CoreServices& services) {
    services.repo->beginListRefresh(91, QStringLiteral("A"), epochA);
  }), "current scoped list preparation was rejected");
  const QJsonObject pet{{QStringLiteral("id"), QStringLiteral("11")}, {QStringLiteral("ri"), 7011},
                        {QStringLiteral("lv"), 100}, {QStringLiteral("n"), QStringLiteral("fixture pet")}};
  ok &= check(runtime.deliverPacket(envelope({{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
      {QStringLiteral("pl"), QJsonArray{pet}}, {QStringLiteral("pps"), QJsonArray{QStringLiteral("11")}},
      {QStringLiteral("ppc"), 12}}, source, 2, epochA)) &&
      waitUntil([&] { return inventory.backpackPets().size() == 1; }), "inventory data did not cross the immutable GUI mailbox");
  ok &= check(waitUntil([&] { return shop.account == QStringLiteral("A") && routine.account == QStringLiteral("A"); }) && guiOwner,
              "shop/routine values were emitted on Core or lacked account identity");

  ok &= check(runtime.postUnscoped([](const CoreServices& services) {
    deliverVerifiedFixture(services.repo, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("B")}}}});
  }) && runtime.post(QStringLiteral("A"), epochA, [&](const CoreServices&) { ++staleActions; }),
      "ordered account-switch test actions were not admitted");
  ok &= check(waitUntil([&] { return rejected && inventory.accountKey() == QStringLiteral("B"); }) && staleActions.load() == 0,
              "old GUI action ran against a later account with the same Core receiver");
  const quint64 epochB = inventory.snapshot()->sessionEpoch;
  ok &= check(runtime.post(QStringLiteral("B"), epochB, [](const CoreServices& services) {
    services.repo->setConnectionState(SessionConnectionState::Disconnected, QStringLiteral("synthetic disconnect"));
  }) && runtime.post(QStringLiteral("B"), epochB, [&](const CoreServices& services) {
    acceptedCacheRead.store(!services.repo->isAuthenticated());
    services.repo->backpackPets();
  }) && waitUntil([&] { return acceptedCacheRead.load(); }), "disconnected cached read incorrectly required authentication");
  ok &= check(runtime.deliverReceipt({999999, QStringLiteral("B"), epochB, QStringLiteral("3_11")}),
              "receipt value could not be queued to Core routing");
  ok &= check(runtime.shutdown() && runtime.closing() && !runtime.postUnscoped([](const CoreServices&) {}),
              "normal shutdown failed to close admission and drain fixed worker ownership");
  ok &= check(waitUntil([&] { return stopped; }) && clean && revocations.load() == 1,
              "shutdown did not revoke before stopping or reported an unclean normal exit");
  return ok;
}

bool boundedClosing() {
  QTemporaryDir root, legacy;
  InventoryProjection inventory(root.path(), nullptr);
  AnalysisProjection analysis;
  QSemaphore release;
  std::atomic_bool entered{false};
  std::atomic<int> revocations{0}, queuedActionRuns{0};
  RuntimeOptions options;
  options.dataRoot = root.path(); options.legacyDataRoot = legacy.path();
  options.inventoryProjection = &inventory; options.analysisProjection = &analysis;
  options.maximumQueuedTasks = 2;
  options.revokePending = [&] { ++revocations; };
  ApplicationRuntime runtime(options);
  bool ready = false, stopped = false;
  QObject::connect(&runtime, &ApplicationRuntime::ready, &runtime, [&] { ready = true; });
  QObject::connect(&runtime, &ApplicationRuntime::stopped, &runtime, [&](bool) { stopped = true; });
  bool ok = check(runtime.start() && waitUntil([&] { return ready; }), "bounded closing runtime did not start");
  ok &= check(runtime.postUnscoped([&](const CoreServices&) { entered.store(true); release.acquire(); }) &&
      waitUntil([&] { return entered.load(); }), "Core blocking fixture did not enter");
  ok &= check(runtime.postUnscoped([&](const CoreServices&) { ++queuedActionRuns; }) &&
      !runtime.postUnscoped([](const CoreServices&) {}), "Core mailbox exceeded its fixed outstanding capacity");
  QElapsedTimer clock; clock.start();
  const bool clean = runtime.shutdown(30);
  ok &= check(!clean && clock.elapsed() < 300 && revocations.load() == 1 && runtime.closing() &&
      !runtime.postUnscoped([](const CoreServices&) {}), "busy Core prevented immediate revocation or extended the shutdown budget");
  release.release();
  ok &= check(waitUntil([&] { return stopped; }) && queuedActionRuns.load() == 0,
              "pre-closing queued action executed after admission was revoked");
  // Projections remain alive through the late Core stop even if the initial
  // 30 ms shutdown result was false. No live thread/dependency is destructed.
  return ok;
}

bool persistenceSemantics() {
  PersistenceSummary state;
  state.update("A", 1, "A", 1, "record", 8, StorageStatus::WriteFailed, "failed", 1);
  auto result = state.update("A", 1, "A", 1, "record", 7, StorageStatus::Saved, {}, 0);
  bool ok = check(result.failedRecords == 1, "older Saved cleared a newer failure");
  result = state.update("A", 1, "A", 1, "record", 9, StorageStatus::Superseded, {}, 0);
  ok &= check(result.failedRecords == 1, "Superseded was mistaken for Saved");
  result = state.update("A", 1, "A", 1, "record", 9, StorageStatus::Saved, {}, 0);
  ok &= check(result.failedRecords == 0 && result.hasSaved, "successful retry did not clear its own failure");
  result = state.update("B", 2, "A", 1, "old", 10, StorageStatus::WriteFailed, "old account", 0);
  ok &= check(result.account == "B" && result.failedRecords == 0 && !result.hasSaved, "old epoch altered new scope summary");
  for (int index = 0; index < 1100; ++index)
    result = state.update("B", 2, "B", 2, QString::number(index), index + 1, StorageStatus::WriteFailed, "failure", 0);
  ok &= check(result.failedRecords == 1024 && result.failuresTruncated, "failure summary exceeded its bound or silently forgot overflow");
  return ok;
}

bool deletedBeforeReady() {
  QTemporaryDir root, legacy;
  InventoryProjection inventory(root.path(), nullptr);
  AnalysisProjection analysis;
  RuntimeOptions options;
  options.dataRoot = root.path(); options.legacyDataRoot = legacy.path();
  options.inventoryProjection = &inventory; options.analysisProjection = &analysis;
  auto* runtime = new ApplicationRuntime(options);
  bool deleted = false, ready = false;
  QObject::connect(runtime, &ApplicationRuntime::imageServiceReady, &inventory, [&](ImageService*) {
    delete runtime; runtime = nullptr; deleted = true;
  });
  QObject::connect(runtime, &ApplicationRuntime::ready, &inventory, [&] { ready = true; });
  bool ok = check(runtime->start() && waitUntil([&] { return deleted; }), "reentrant deletion fixture did not initialize");
  ok &= check(!ready, "Ready emitted after its facade was deleted");
  if (runtime) delete runtime;
  return ok;
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  const bool ok = persistenceSemantics() && ownershipAndRouting() && boundedClosing() && deletedBeforeReady();
  if (ok) std::puts("PASS: Core-thread construction, immutable GUI delivery, scoped actions and bounded closing");
  return ok ? 0 : 1;
}
