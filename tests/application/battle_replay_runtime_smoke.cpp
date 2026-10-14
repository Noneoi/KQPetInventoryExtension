#include "application/runtime/application_runtime.h"
#include "application/runtime/data_update_service.h"
#include "application/catalog/catalog_io_service.h"
#include "application/catalog/public_dataset_store.h"
#include "application/views/analysis_projection.h"
#include "application/views/inventory_projection.h"
#include "fixtures/battle_replay_catalog_fixture.h"
#include "support/catalog_test_support.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <cstdio>
#include <memory>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
template<class Predicate>
bool waitUntil(Predicate predicate, int milliseconds = 8000) {
  if (predicate()) return true;
  QEventLoop loop;
  QTimer probe, deadline;
  probe.setInterval(1); deadline.setSingleShot(true);
  QObject::connect(&probe, &QTimer::timeout, &loop, [&] { if (predicate()) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  probe.start(); deadline.start(milliseconds); loop.exec();
  return predicate();
}
QString marker(const BattleReplayCatalogHandle& snapshot) {
  return snapshot ? snapshot->root.value(QStringLiteral("testOnlyMarker")).toString() : QString{};
}
QString pathFor(const QString& root) {
  return QDir(root).filePath(QStringLiteral("catalog/battle-replay-catalog.json"));
}
QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QByteArray compact(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QString digest(const QByteArray& value) {
  return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
QJsonObject fileObject(const QString& path) { return QJsonDocument::fromJson(readFile(path)).object(); }
struct PublicVersionFixture { QString revision; QJsonObject manifest; QByteArray replayBytes; };
PublicVersionFixture versionFixture(const QString& root, const QString& name) {
  const auto pointer = fileObject(root + QStringLiteral("/public/current.json"));
  auto manifest = fileObject(root + QStringLiteral("/public/releases/%1/manifest.json").arg(pointer.value("revision").toString()));
  auto components = manifest.value("components").toObject();
  const auto bytes = battleReplayCatalogFixtureBytes(name);
  components.insert("battleReplay", QJsonObject{{"sha256", digest(bytes)}, {"bytes", bytes.size()}, {"format", "json"},
      {"origin", QJsonObject{{"kind", "synthetic-runtime-test"}}}});
  manifest.insert("components", components);
  const auto manifestBytes = compact(manifest);
  const auto revision = digest(manifestBytes);
  writeCatalogFixture(root + QStringLiteral("/public/objects/%1/data").arg(digest(bytes)), bytes);
  writeCatalogFixture(root + QStringLiteral("/public/releases/%1/manifest.json").arg(revision), manifestBytes);
  return {revision, manifest, bytes};
}
bool publishVersionFixture(const QString& root, const QString& name) {
  const auto version = versionFixture(root, name);
  PublicDatasetStore store(root);
  auto result = store.prepareRestoreRevision(version.revision);
  if (result.ok()) result = store.commitPrepared(result.prepared);
  if (!result.ok()) std::fprintf(stderr, "FIXTURE PUBLICATION: %s\n", qPrintable(result.error));
  return result.status == PublicDatasetStatus::DiskCommitted;
}

// TEST ONLY. These synthetic receipts model the already-completed producer
// boundary. Actual PublicDatasetStore validates every hash, fixed dependency,
// source-file binding and native snapshot; this is not an official semantic
// regression or a claim that this synthetic catalog ran real game battles.
QString candidateFixture(const QString& root, const QString& name, bool retainedIcons = false) {
  const auto pointerBytes = readFile(root + QStringLiteral("/public/current.json"));
  const auto pointer = QJsonDocument::fromJson(pointerBytes).object();
  const auto version = versionFixture(root, name);
  const auto id = QUuid::createUuid().toString(QUuid::Id128);
  const auto transaction = root + QStringLiteral("/public/transactions/") + id;
  const QByteArray checkerBytes("# TEST ONLY candidate producer identity\n");
  const QByteArray nativeBytes("TEST ONLY native-report producer identity\n");
  const QByteArray runtimeBytes("TEST ONLY runtime identity\n");
  writeCatalogFixture(root + QStringLiteral("/data-tools/scripts/public_dataset_store.py"), checkerBytes);
  writeCatalogFixture(root + QStringLiteral("/data-tools/scripts/KQBattleReplayUpdateCheck.exe"), nativeBytes);
  writeCatalogFixture(root + QStringLiteral("/data-tools/scripts/Qt6Core.dll"), runtimeBytes);
  const auto source = root + QStringLiteral("/data-tools/test-only-replay-source.json");
  writeCatalogFixture(source, version.replayBytes);
  const QString sourceSha = digest(version.replayBytes);
  const QString evidenceFile = QStringLiteral("public/source-cache/evidence/%1/data").arg(sourceSha);
  writeCatalogFixture(root + QLatin1Char('/') + evidenceFile, version.replayBytes);
  const QJsonObject fixtureCase{{"id", "SYNTHETIC STORAGE BINDING"}, {"sha256", sourceSha}};
  const QByteArray fixtureSet = compact({{"schemaVersion", 1}, {"cases", QJsonArray{fixtureCase}}});
  writeCatalogFixture(root + QStringLiteral("/data-tools/scripts/battle-replay-regression-manifest.json"), fixtureSet);
  const QJsonObject native{{"schemaVersion", 1}, {"mode", "candidate-regression"}, {"passed", true}, {"errors", QJsonArray{}},
      {"catalogFileSha256", sourceSha}, {"catalogSha256", sourceSha}, {"fixtureSetSha256", digest(fixtureSet)}, {"caseCount", 1},
      {"cases", QJsonArray{QJsonObject{{"id", fixtureCase.value("id")}, {"fixtureSha256", sourceSha}, {"passed", true}}}}};
  const QJsonObject quality{{"schemaVersion", 1}, {"component", "battleReplay"}, {"passed", true}, {"findings", QJsonArray{}},
      {"candidateContentSha256", sourceSha}, {"decision", "validated-not-yet-applied"},
      {"summary", QJsonObject{{"errors", 0}, {"reviewRequired", 0}}},
      {"sourceFidelity", QJsonObject{{"schemaVersion", 1}, {"passed", true}, {"findings", QJsonArray{}},
          {"candidateContentSha256", sourceSha}, {"inputs", QJsonArray{QJsonObject{{"role", "test-source"}, {"status", "read"},
              {"path", source}, {"sha256", sourceSha}, {"bytes", version.replayBytes.size()}}}}}},
      {"replayRegression", QJsonObject{{"schemaVersion", 1}, {"passed", true}, {"candidateContentSha256", sourceSha},
          {"catalogFileSha256", sourceSha}, {"catalogSha256", sourceSha}, {"processExitCode", 0}, {"errors", QJsonArray{}},
          {"nativeReport", native}, {"fixtureSetSha256", digest(fixtureSet)}, {"checkerSha256", digest(nativeBytes)},
          {"runtimeSha256", QJsonObject{{"Qt6Core.dll", digest(runtimeBytes)}}}}}};
  const auto qualityBytes = compact(quality);
  writeCatalogFixture(transaction + QStringLiteral("/quality/battleReplay.json"), qualityBytes);
  const QJsonArray effective{"pets", "skills", "shop", "images", "icons", "battleReplay"};
  const auto previous = fileObject(root + QStringLiteral("/public/releases/%1/manifest.json").arg(pointer.value("revision").toString()));
  const bool changed = previous.value("components").toObject().value("battleReplay").toObject().value("sha256") != sourceSha;
  QJsonObject results;
  for (const auto& value : effective) results.insert(value.toString(), QJsonObject{{"status", value == "battleReplay" && changed ? "updated" : "unchanged"}});
  if (retainedIcons) results.insert("icons", QJsonObject{{"status", "retained"}, {"retained", true},
      {"attemptStatus", "failed"}, {"error", "TEST ONLY unavailable optional icon"}});
  const QJsonObject checker{{"kind", "public-dataset-python"}, {"schema", 1}, {"sha256", digest(checkerBytes)}};
  const QJsonObject parserFiles{{"public_dataset_store.py", digest(checkerBytes)}};
  const QJsonObject report{{"schema", 1}, {"attempt", id}, {"baseRevision", pointer.value("revision")},
      {"baseCurrentSha256", digest(pointerBytes)}, {"revision", version.revision}, {"requestedComponents", QJsonArray{"battleReplay"}},
      {"effectiveComponents", effective}, {"results", results}, {"components", version.manifest.value("components")},
      {"attachments", version.manifest.value("attachments")}, {"passed", true}, {"checker", checker},
      {"checks", QJsonObject{{"objects", true}, {"dependencies", true}, {"sourceStable", true}}},
      {"parserIdentity", QJsonObject{{"files", parserFiles}, {"sha256", digest(compact(parserFiles))}, {"formatSchema", 1}}},
      {"changedComponents", changed ? QJsonArray{"battleReplay"} : QJsonArray{}},
      {"evidenceFiles", QJsonArray{QJsonObject{{"component", "battleReplay"}, {"role", "test-source"}, {"originalPath", source},
          {"file", evidenceFile}, {"sha256", sourceSha}, {"bytes", version.replayBytes.size()}}}},
      {"qualityReports", QJsonObject{{"battleReplay", QJsonObject{{"file", "quality/battleReplay.json"}, {"sha256", digest(qualityBytes)},
          {"bytes", qualityBytes.size()}, {"candidateContentSha256", sourceSha}}}}}};
  const auto reportBytes = compact(report);
  writeCatalogFixture(transaction + QStringLiteral("/validation.json"), reportBytes);
  writeCatalogFixture(transaction + QStringLiteral("/candidate.json"), compact({{"schema", 1}, {"attempt", id},
      {"baseRevision", pointer.value("revision")}, {"baseCurrentSha256", digest(pointerBytes)}, {"revision", version.revision},
      {"requestedComponents", QJsonArray{"battleReplay"}}, {"effectiveComponents", effective}, {"results", results},
      {"validation", QJsonObject{{"passed", true}, {"manifestSha256", version.revision}, {"reportSha256", digest(reportBytes)}, {"checker", checker}}}}));
  return id;
}
struct CoreProbe {
  std::atomic_bool done{false}, idle{false}, replayAbsent{false}, correctThread{false};
};
bool waitForStartupIo(ApplicationRuntime& runtime, bool* replayAbsent = nullptr) {
  QElapsedTimer total; total.start();
  while (total.elapsed() < 10000) {
    auto probe = std::make_shared<CoreProbe>();
    if (!runtime.postUnscoped([probe](const CoreServices& services) {
      probe->idle = services.catalogs->stats().pendingRequests == 0 && services.storage->state().outstandingTasks == 0;
      probe->replayAbsent = !services.catalogs->battleReplaySnapshot();
      probe->correctThread = services.catalogs->thread() == QThread::currentThread() &&
          services.storage->thread() == QThread::currentThread() &&
          QThread::currentThread() != QCoreApplication::instance()->thread();
      probe->done = true;
    }) || !waitUntil([&] { return probe->done.load(); })) return false;
    if (probe->idle.load()) {
      if (replayAbsent) *replayAbsent = probe->replayAbsent.load();
      return probe->correctThread.load();
    }
  }
  return false;
}
struct IoBarrier {
  QSemaphore entered, release, exited;
  std::atomic_bool admitted{false}, admissionDone{false};
};
bool holdIo(ApplicationRuntime& runtime, const std::shared_ptr<IoBarrier>& barrier) {
  if (!runtime.postUnscoped([barrier](const CoreServices& services) {
    barrier->admitted = services.storage->postAuxiliary([barrier](QObject*) {
      barrier->entered.release(); barrier->release.acquire(); barrier->exited.release();
    });
    barrier->admissionDone = true;
  })) return false;
  return waitUntil([&] { return barrier->admissionDone.load(); }) && barrier->admitted.load() &&
      barrier->entered.tryAcquire(1, 3000);
}

bool runtimePublication() {
  QTemporaryDir root, legacy;
  bool ok = check(root.isValid() && legacy.isValid(), "runtime temporary data roots unavailable");
  if (!ok) return false;
  qputenv("KQPET_OFFICIAL_UNPACK_ROOT", legacy.path().toUtf8());
  qunsetenv("KQPET_CATALOG_PATH");
  const QString path = pathFor(root.path());
  ok &= check(writeCatalogFixture(path, battleReplayCatalogFixtureBytes(QStringLiteral("runtime-A"))),
              "runtime external fixture could not be written");
  InventoryProjection inventory(root.path(), nullptr);
  AnalysisProjection analysis;
  auto validInput = std::make_shared<std::atomic_bool>(true);
  auto gameRequests = std::make_shared<std::atomic_int>(0);
  RuntimeOptions options;
  options.dataRoot = root.path(); options.legacyDataRoot = legacy.path();
  options.inventoryProjection = &inventory; options.analysisProjection = &analysis;
  options.inputValid = [validInput] { return validInput->load(); };
  options.sender = [gameRequests](const OutboundIntent&) { ++*gameRequests; return false; };
  ApplicationRuntime runtime(options);
  bool ready = false, stopped = false, clean = false, guiThread = true, busy = false;
  int terminalEvents = 0, busyEvents = 0, publicUpdateEvents = 0;
  int resultEvents = 0, datasetEvents = 0;
  PublicUpdateState updateState;
  PublicDatasetSnapshotHandle visibleDataset;
  BattleReplayCatalogHandle current;
  QString lastMessage;
  QObject::connect(&runtime, &ApplicationRuntime::ready, &runtime, [&] { ready = true; });
  QObject::connect(&runtime, &ApplicationRuntime::publicUpdateStateChanged, &runtime,
      [&](const PublicUpdateState& result) {
    guiThread &= QThread::currentThread() == inventory.thread(); updateState = result; ++resultEvents;
  });
  QObject::connect(&runtime, &ApplicationRuntime::publicDatasetChanged, &runtime,
      [&](PublicDatasetSnapshotHandle value) { visibleDataset = value; ++datasetEvents; });
  QObject::connect(&runtime, &ApplicationRuntime::stopped, &runtime,
      [&](bool value) { stopped = true; clean = value; });
  QObject::connect(&runtime, &ApplicationRuntime::dataUpdateStatusChanged, &runtime,
      [&](const QString&, bool) { ++publicUpdateEvents; });
  QObject::connect(&runtime, &ApplicationRuntime::battleReplayCatalogChanged, &runtime,
      [&](BattleReplayCatalogHandle snapshot, const QString& message, bool running) {
    guiThread &= QThread::currentThread() == inventory.thread();
    busy = running; current = std::move(snapshot); lastMessage = message;
    if (running) ++busyEvents; else ++terminalEvents;
  });
  ok &= check(runtime.start() && waitUntil([&] { return ready; }), "replay runtime failed to start");
  bool absent = false;
  ok &= check(ready && waitForStartupIo(runtime, &absent) && absent && !current && terminalEvents == 0,
              "runtime eagerly loaded replay data, used the wrong owner, or emitted a false ready catalog");

  auto barrier = std::make_shared<IoBarrier>();
  const bool held = holdIo(runtime, barrier);
  ok &= check(held, "runtime replay IO barrier was not admitted");
  int heartbeats = 0;
  QTimer heart; heart.setInterval(1);
  QObject::connect(&heart, &QTimer::timeout, &heart, [&] { ++heartbeats; });
  heart.start();
  runtime.requestBattleReplayCatalog();
  auto ping = std::make_shared<std::atomic_bool>(false);
  const bool pingAccepted = runtime.postUnscoped([ping](const CoreServices&) { ping->store(true); });
  ok &= check(pingAccepted && waitUntil([&] { return busyEvents > 0 && ping->load() && heartbeats >= 3; }) &&
      !current && terminalEvents == 0, "waiting replay IO blocked GUI/Core or prematurely published a catalog");
  barrier->release.release();
  ok &= check(waitUntil([&] { return !busy && marker(current) == QStringLiteral("runtime-A"); }) &&
      current && !current->embedded && current->sourcePath.startsWith(QDir(root.path()).filePath(QStringLiteral("public/objects/"))) &&
      current->sourcePath.endsWith(QStringLiteral("/data")) && readFile(path) == battleReplayCatalogFixtureBytes(QStringLiteral("runtime-A")) && guiThread,
      "runtime did not publish an immutable shared object on the GUI thread or changed the retained legacy source");
  heart.stop();
  const auto first = current;
  if (!first) { runtime.shutdown(); waitUntil([&] { return stopped; }); return false; }

  ok &= check(publishVersionFixture(root.path(), QStringLiteral("runtime-B")),
              "second complete public version could not be published by the native store");
  int before = terminalEvents;
  runtime.requestBattleReplayCatalog();
  ok &= check(waitUntil([&] { return terminalEvents > before; }) && current == first && !busy,
              "cached request unexpectedly reread the replaced disk catalog");
  before = terminalEvents;
  runtime.requestBattleReplayCatalog(true);
  ok &= check(waitUntil([&] { return terminalEvents > before; }) && marker(current) == QStringLiteral("runtime-B") &&
      current->revision > first->revision && marker(first) == QStringLiteral("runtime-A"),
      "forced runtime reload failed or mutated a retained consumer snapshot");
  const auto second = current;
  before = terminalEvents;
  runtime.requestBattleReplayCatalog(true);
  ok &= check(waitUntil([&] { return terminalEvents > before; }) && current == second,
              "same-content forced runtime reload changed its snapshot identity");

  // Local public data remains usable without the game capture channel. This
  // special path must not grant the ordinary Core/game-action entry points it.
  validInput->store(false);
  auto forbiddenRan = std::make_shared<std::atomic_bool>(false);
  ok &= check(!runtime.postUnscoped([forbiddenRan](const CoreServices&) { forbiddenRan->store(true); }),
              "local catalog support bypassed the ordinary invalid-input admission gate");
  ok &= check(publishVersionFixture(root.path(), QStringLiteral("offline-C")),
              "offline complete public version could not be published");
  before = terminalEvents;
  runtime.requestBattleReplayCatalog(true);
  ok &= check(waitUntil([&] { return terminalEvents > before; }) && marker(current) == QStringLiteral("offline-C") &&
      current->revision > second->revision && !forbiddenRan->load(),
      "local replay reload required a valid capture channel or executed a forbidden ordinary action");
  const auto offline = current;
  // The settings/result facade is local, too. Reopening reads actual receipt
  // state and current Core revision, not a prior process's sessionApplied bit.
  const int beforeResults = resultEvents, beforeDatasets = datasetEvents;
  runtime.requestPublicUpdateState(); runtime.requestPublicDatasetState();
  ok &= check(waitUntil([&] { return resultEvents > beforeResults && datasetEvents > beforeDatasets &&
      visibleDataset && updateState.sessionApplied && updateState.diskRevision == visibleDataset->revision; }),
      "offline settings did not restore the actual receipt/Core publication");
  bool settingsReceived = false, settingsReady = false;
  QObject::connect(&runtime, &ApplicationRuntime::settingsStateReady, &runtime,
      [&](const RefreshTimings&, bool available, const QString&) { settingsReceived = true; settingsReady = available; });
  runtime.requestSettingsState();
  ok &= check(waitUntil([&] { return settingsReceived; }) && settingsReady,
      "local settings incorrectly require game capture admission");
  bool exported = false, exportOk = false;
  QObject::connect(&runtime, &ApplicationRuntime::publicDiagnosticsExported, &runtime,
      [&](bool success, const QString&, const QString&) { exported = true; exportOk = success; });
  const QString exportPath = root.path() + QStringLiteral("/public-only-diagnostics.json");
  runtime.exportPublicDataDiagnostics(exportPath);
  ok &= check(waitUntil([&] { return exported; }) && exportOk && !readFile(exportPath).isEmpty() && gameRequests->load() == 0,
      "public diagnostic export failed offline or sent a game request");
  const QByteArray badBytes("{\"format\":3,\"skills\":null}");
  const auto corruptedObject = offline->sourcePath;
  ok &= check(writeCatalogFixture(corruptedObject, badBytes), "runtime failure object could not be injected");
  before = terminalEvents;
  runtime.requestBattleReplayCatalog(true);
  ok &= check(waitUntil([&] { return terminalEvents > before; }) && current == offline && !busy &&
      !lastMessage.isEmpty() && readFile(corruptedObject) == badBytes && marker(first) == QStringLiteral("runtime-A"),
      "runtime failure lost its last good catalog, changed retained data, or rewrote the source");
  ok &= check(guiThread && gameRequests->load() == 0 && publicUpdateEvents > 0 &&
      !QFileInfo::exists(root.path() + QStringLiteral("/data-tools/scripts/bootstrap-public-data.ps1")),
              "local replay loading crossed GUI ownership, started update tools or failed to report shared-version readiness");
  std::printf("replay runtime background: heartbeat_pulses=%d catalog_terminal_events=%d\n", heartbeats, terminalEvents);
  ok &= check(runtime.shutdown() && waitUntil([&] { return stopped; }) && clean,
              "replay runtime did not finish a clean shutdown");
  return ok;
}

bool dataUpdateRefreshesReplay() {
  QTemporaryDir root, legacy;
  if (!check(root.isValid() && legacy.isValid(), "update runtime temporary roots unavailable")) return false;
  qputenv("KQPET_OFFICIAL_UNPACK_ROOT", legacy.path().toUtf8());
  const QString path = pathFor(root.path());
  bool ok = check(writeCatalogFixture(path, battleReplayCatalogFixtureBytes(QStringLiteral("before-update"))),
                  "update runtime catalog fixture could not be written");
  ok &= check(DataUpdateService::componentNames().count(QStringLiteral("battleReplay")) == 1,
              "all-data updates do not include exactly one replay component");
  InventoryProjection inventory(root.path(), nullptr);
  AnalysisProjection analysis;
  auto validInput = std::make_shared<std::atomic_bool>(true);
  auto gameRequests = std::make_shared<std::atomic_int>(0);
  RuntimeOptions options;
  options.dataRoot = root.path(); options.legacyDataRoot = legacy.path();
  options.inventoryProjection = &inventory; options.analysisProjection = &analysis;
  options.inputValid = [validInput] { return validInput->load(); };
  options.sender = [gameRequests](const OutboundIntent&) { ++*gameRequests; return false; };
  ApplicationRuntime runtime(options);
  bool ready = false, stopped = false, clean = false, guiThread = true, busy = false;
  int catalogCompletions = 0, updateCompletions = 0;
  BattleReplayCatalogHandle current;
  QString catalogMessage;
  QObject::connect(&runtime, &ApplicationRuntime::ready, &runtime, [&] { ready = true; });
  QObject::connect(&runtime, &ApplicationRuntime::stopped, &runtime,
      [&](bool value) { stopped = true; clean = value; });
  QObject::connect(&runtime, &ApplicationRuntime::battleReplayCatalogChanged, &runtime,
      [&](BattleReplayCatalogHandle snapshot, const QString& message, bool running) {
    guiThread &= QThread::currentThread() == inventory.thread();
    current = std::move(snapshot); busy = running; catalogMessage = message;
    if (!running) ++catalogCompletions;
  });
  QObject::connect(&runtime, &ApplicationRuntime::dataUpdateStatusChanged, &runtime,
      [&](const QString&, bool running) {
    guiThread &= QThread::currentThread() == inventory.thread();
    if (!running) ++updateCompletions;
  });
  if (!check(runtime.start() && waitUntil([&] { return ready; }) && waitForStartupIo(runtime),
      "update runtime did not reach idle startup")) return false;
  runtime.requestBattleReplayCatalog();
  ok &= check(waitUntil([&] { return marker(current) == QStringLiteral("before-update") && !busy; }),
              "update runtime did not load its initial catalog");
  const auto pinned = current;
  auto services = std::make_shared<CoreServices>();
  auto captured = std::make_shared<std::atomic_bool>(false);
  ok &= check(runtime.postUnscoped([services, captured](const CoreServices& value) {
    *services = value; captured->store(true);
  }) && waitUntil([&] { return captured->load(); }) && services->dataUpdater,
      "update runtime fixture could not access its Core update boundary");
  if (!pinned || !services->dataUpdater) {
    runtime.shutdown(); waitUntil([&] { return stopped; }); return false;
  }
  auto decodedPublication = std::make_shared<std::atomic_bool>(false);
  auto observerInstalled = std::make_shared<std::atomic_bool>(false);
  ok &= check(QMetaObject::invokeMethod(services->catalogs,
      [catalogs = services->catalogs, decodedPublication, observerInstalled] {
    QObject::connect(catalogs, &CatalogIoService::publicDatasetChanged, catalogs,
        [decodedPublication](PublicDatasetSnapshotHandle value) {
      if (value && value->replay) decodedPublication->store(true);
    });
    observerInstalled->store(true);
  }, Qt::QueuedConnection) && waitUntil([&] { return observerInstalled->load(); }),
      "could not observe the complete Core publication boundary");
  const auto applyCandidate = [&](const QString& attempt) {
    return QMetaObject::invokeMethod(services->catalogs,
        [catalogs = services->catalogs, attempt] { catalogs->requestPublicCandidate(attempt); }, Qt::QueuedConnection);
  };
  validInput->store(false);
  auto forbidden = std::make_shared<std::atomic_bool>(false);
  ok &= check(!runtime.postUnscoped([forbidden](const CoreServices&) { forbidden->store(true); }) &&
      !runtime.post(QStringLiteral("synthetic-account"), 1,
          [forbidden](const CoreServices&) { forbidden->store(true); }),
      "public update support admitted an ordinary command without a valid game input channel");

  // This candidate changes replay bytes, so native pre-read returns a decoded
  // complete bundle. There is no lazy BattleReplay job to notify the page.
  // GUI delivery must follow the successful whole-bundle session ack instead.
  const auto updatedAttempt = candidateFixture(root.path(), QStringLiteral("updated"));
  int beforeCatalog = catalogCompletions, beforeUpdate = updateCompletions;
  ok &= check(applyCandidate(updatedAttempt) &&
      waitUntil([&] { return updateCompletions > beforeUpdate && catalogCompletions > beforeCatalog; }) &&
      decodedPublication->load() && marker(current) == QStringLiteral("updated") && current->revision > pinned->revision &&
      marker(pinned) == QStringLiteral("before-update"),
      "an already-decoded complete public candidate did not notify the replay GUI or changed the opened battle's pinned data");
  const auto updated = current;
  const auto receiptPath = root.path() + QStringLiteral("/public/transactions/%1/receipt.json").arg(updatedAttempt);
  ok &= check(fileObject(receiptPath).value("diskCommitted") == QJsonValue(true) &&
      fileObject(receiptPath).value("sessionApplied") == QJsonValue(true), "GUI success preceded the real session application receipt");
  const auto unchanged = candidateFixture(root.path(), QStringLiteral("updated"));
  const auto unchangedPointer = readFile(root.path() + QStringLiteral("/public/current.json"));
  beforeCatalog = catalogCompletions; beforeUpdate = updateCompletions;
  ok &= check(applyCandidate(unchanged) &&
      waitUntil([&] { return updateCompletions > beforeUpdate && catalogCompletions > beforeCatalog; }) &&
      current == updated && readFile(root.path() + QStringLiteral("/public/current.json")) == unchangedPointer,
      "unchanged complete candidate replaced the replay handle or rewrote the public pointer");

  const auto invalidAttempt = candidateFixture(root.path(), QStringLiteral("invalid-quality"));
  const auto invalidReport = root.path() + QStringLiteral("/public/transactions/%1/validation.json").arg(invalidAttempt);
  writeCatalogFixture(invalidReport, "{}");
  beforeCatalog = catalogCompletions; beforeUpdate = updateCompletions;
  ok &= check(applyCandidate(invalidAttempt) && waitUntil([&] { return updateCompletions > beforeUpdate; }) &&
      current == updated && catalogCompletions == beforeCatalog && readFile(invalidReport) == "{}",
      "a rejected candidate cleared, reloaded or replaced the last valid replay snapshot");

  // Optional icon failure retains that independent block while the complete
  // candidate can still publish its checked replay/core group.
  const auto partialAttempt = candidateFixture(root.path(), QStringLiteral("partial-success"), true);
  beforeCatalog = catalogCompletions; beforeUpdate = updateCompletions;
  ok &= check(applyCandidate(partialAttempt) &&
      waitUntil([&] { return updateCompletions > beforeUpdate && catalogCompletions > beforeCatalog; }) &&
      marker(current) == QStringLiteral("partial-success") && marker(pinned) == QStringLiteral("before-update"),
      "an independently retained icon block prevented checked replay data from reaching the GUI");
  const auto partial = current;
  const auto brokenAttempt = candidateFixture(root.path(), QStringLiteral("corrupted-object"));
  const auto brokenCandidate = fileObject(root.path() + QStringLiteral("/public/transactions/%1/candidate.json").arg(brokenAttempt));
  const auto brokenManifest = fileObject(root.path() + QStringLiteral("/public/releases/%1/manifest.json").arg(brokenCandidate.value("revision").toString()));
  const auto objectSha = brokenManifest.value("components").toObject().value("battleReplay").toObject().value("sha256").toString();
  const auto brokenObject = root.path() + QStringLiteral("/public/objects/%1/data").arg(objectSha);
  const QByteArray invalid("{\"format\":3,\"skills\":null}");
  writeCatalogFixture(brokenObject, invalid);
  beforeCatalog = catalogCompletions; beforeUpdate = updateCompletions;
  ok &= check(applyCandidate(brokenAttempt) && waitUntil([&] { return updateCompletions > beforeUpdate; }) &&
      current == partial && !busy && catalogCompletions == beforeCatalog && readFile(brokenObject) == invalid &&
      marker(pinned) == QStringLiteral("before-update"),
      "an unreadable candidate object discarded the valid GUI snapshot or rewrote the damaged source");

  // Hold Storage I/O before requesting a real update. Confirm its admission
  // while offline, then close the updater BEFORE allowing Job::start to run.
  // This checks the production request path without preparing tools or network.
  auto barrier = std::make_shared<IoBarrier>();
  const bool postedBarrier = QMetaObject::invokeMethod(services->dataUpdater,
      [storage = services->storage, barrier] {
    barrier->admitted = storage->postAuxiliary([barrier](QObject*) {
      barrier->entered.release(); barrier->release.acquire(); barrier->exited.release();
    });
    barrier->admissionDone = true;
  }, Qt::QueuedConnection);
  const bool held = postedBarrier && waitUntil([&] { return barrier->admissionDone.load(); }) &&
      barrier->admitted.load() && barrier->entered.tryAcquire(1, 3000);
  ok &= check(held, "offline update IO barrier could not be held");
  if (held) {
    runtime.requestDataUpdate({QStringLiteral("battleReplay")});
    auto checked = std::make_shared<std::atomic_bool>(false);
    auto admitted = std::make_shared<std::atomic_bool>(false);
    const bool posted = QMetaObject::invokeMethod(services->dataUpdater,
        [updater = services->dataUpdater, checked, admitted] {
      admitted->store(updater->busy()); updater->close(); checked->store(true);
    }, Qt::QueuedConnection);
    ok &= check(posted && waitUntil([&] { return checked->load(); }) && admitted->load(),
                "an explicit public replay update was rejected solely because the capture channel was offline");
  }
  barrier->release.release();
  ok &= check(!held || waitUntil([&] { return barrier->exited.available() > 0; }),
              "offline update IO barrier did not release");
  ok &= check(runtime.shutdown() && waitUntil([&] { return stopped; }) && clean && guiThread &&
      !forbidden->load() && gameRequests->load() == 0 &&
      !QFileInfo::exists(QDir(root.path()).filePath(QStringLiteral("data-tools/scripts/bootstrap-public-data.ps1"))),
      "public update test crossed game/GUI boundaries, started external tools or did not shut down cleanly");
  return ok;
}

bool closingPendingReload() {
  QTemporaryDir root, legacy;
  if (!check(root.isValid() && legacy.isValid(), "closing runtime temporary roots unavailable")) return false;
  qputenv("KQPET_OFFICIAL_UNPACK_ROOT", legacy.path().toUtf8());
  const QString path = pathFor(root.path());
  const auto bytes = battleReplayCatalogFixtureBytes(QStringLiteral("must-not-arrive-after-close"));
  bool ok = check(writeCatalogFixture(path, bytes), "closing runtime fixture could not be written");
  InventoryProjection inventory(root.path(), nullptr);
  AnalysisProjection analysis;
  auto gameRequests = std::make_shared<std::atomic_int>(0);
  RuntimeOptions options;
  options.dataRoot = root.path(); options.legacyDataRoot = legacy.path();
  options.inventoryProjection = &inventory; options.analysisProjection = &analysis;
  options.inputValid = [] { return true; };
  options.sender = [gameRequests](const OutboundIntent&) { ++*gameRequests; return false; };
  ApplicationRuntime runtime(options);
  bool ready = false, stopped = false, loading = false;
  int publications = 0;
  QObject::connect(&runtime, &ApplicationRuntime::ready, &runtime, [&] { ready = true; });
  QObject::connect(&runtime, &ApplicationRuntime::stopped, &runtime, [&](bool) { stopped = true; });
  QObject::connect(&runtime, &ApplicationRuntime::battleReplayCatalogChanged, &runtime,
      [&](BattleReplayCatalogHandle snapshot, const QString&, bool busy) {
    loading |= busy;
    if (snapshot) ++publications;
  });
  ok &= check(runtime.start() && waitUntil([&] { return ready; }) && waitForStartupIo(runtime),
              "closing runtime did not reach idle startup");
  auto barrier = std::make_shared<IoBarrier>();
  ok &= check(holdIo(runtime, barrier), "closing runtime IO barrier did not enter");
  runtime.requestBattleReplayCatalog(true);
  ok &= check(waitUntil([&] { return loading; }), "pending reload did not report its busy state");
  QElapsedTimer elapsed; elapsed.start();
  const bool clean = runtime.shutdown(30);
  const qint64 closingMilliseconds = elapsed.elapsed();
  ok &= check(!clean && runtime.closing() && closingMilliseconds < 500 &&
      !runtime.postUnscoped([](const CoreServices&) {}), "pending replay IO extended bounded shutdown or admitted later work");
  barrier->release.release();
  ok &= check(waitUntil([&] { return stopped && barrier->exited.available() > 0; }) &&
      publications == 0 && readFile(path) == bytes && gameRequests->load() == 0,
      "closing replay runtime published a late snapshot, modified its source, or issued a game request");
  std::printf("replay pending shutdown: elapsed_ms=%lld\n", static_cast<long long>(closingMilliseconds));
  return ok;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  const bool publication = runtimePublication();
  const bool updates = dataUpdateRefreshesReplay();
  const bool closing = closingPendingReload();
  if (publication && updates && closing)
    std::puts("PASS: runtime replay dataRoot, async publication, public updates, offline admission, immutable failure and shutdown");
  return publication && updates && closing ? 0 : 1;
}
