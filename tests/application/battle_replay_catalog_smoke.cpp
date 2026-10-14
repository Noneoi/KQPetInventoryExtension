#include "application/catalog/catalog_io_service.h"
#include "fixtures/battle_replay_catalog_fixture.h"
#include "support/catalog_test_support.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <memory>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
template<class Predicate>
bool waitUntil(Predicate predicate, int milliseconds = 10000) {
  if (predicate()) return true;
  QEventLoop loop;
  QTimer probe, deadline;
  probe.setInterval(1); deadline.setSingleShot(true);
  QObject::connect(&probe, &QTimer::timeout, &loop, [&] { if (predicate()) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  probe.start(); deadline.start(milliseconds); loop.exec();
  return predicate();
}
QString pathFor(const QString& root) {
  return QDir(root).filePath(QStringLiteral("catalog/battle-replay-catalog.json"));
}
QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString marker(const BattleReplayCatalogHandle& snapshot) {
  return snapshot ? snapshot->root.value(QStringLiteral("testOnlyMarker")).toString() : QString{};
}
bool reload(CatalogIoService& service, QString* error = nullptr, int timeout = 10000) {
  return runCatalogRequest(&service, CatalogKind::BattleReplay, CatalogRequestMode::Reload, error, timeout);
}

bool externalLifecycle(const QString& root) {
  StorageService storage(root);
  CatalogIoService service(&storage);
  const QString path = pathFor(root);
  int publications = 0;
  QObject::connect(&service, &CatalogIoService::catalogUpdated, &service,
      [&](CatalogKind kind, quint64) { if (kind == CatalogKind::BattleReplay) ++publications; });
  bool ok = check(!service.battleReplaySnapshot(), "replay catalog loaded before an explicit request");
  ok &= check(service.requestOfficialUpdate(CatalogKind::BattleReplay) == 0,
              "replay accepted the unsupported official-update operation");
  const QByteArray firstBytes = battleReplayCatalogFixtureBytes(QStringLiteral("first"));
  ok &= check(writeCatalogFixture(path, firstBytes) && reload(service), "valid external replay catalog failed to load");
  const auto first = service.battleReplaySnapshot();
  if (!check(first != nullptr, "external replay did not publish a snapshot")) {
    service.close(); storage.shutdown(); return false;
  }
  ok &= check(!first->embedded && marker(first) == QStringLiteral("first") && first->revision > 0 &&
      first->sha256 == digest(firstBytes) && QDir::cleanPath(first->sourcePath) == QDir::cleanPath(path) &&
      first->loadedAt.isValid() && publications == 1, "external source identity/hash/revision was not preserved");
  const quint64 firstRevision = first->revision;
  ok &= check(reload(service) && service.battleReplaySnapshot()->revision == firstRevision && publications == 1,
              "identical content and source caused a new revision/publication");
  const QByteArray secondBytes = battleReplayCatalogFixtureBytes(QStringLiteral("second"));
  ok &= check(writeCatalogFixture(path, secondBytes) && reload(service), "replacement replay catalog did not load");
  const auto second = service.battleReplaySnapshot();
  ok &= check(second && marker(second) == QStringLiteral("second") && second->revision > firstRevision &&
      second->sha256 == digest(secondBytes) && marker(first) == QStringLiteral("first") &&
      first->revision == firstRevision && first->sha256 == digest(firstBytes) && publications == 2,
      "publication mutated a retained snapshot or did not advance the replacement");
  if (!second) { service.close(); storage.shutdown(); return false; }

  QList<QByteArray> invalid;
  invalid.append(QByteArray("{\"format\":3,"));
  auto wrongFormat = battleReplayCatalogFixture(QStringLiteral("wrong-format"));
  wrongFormat.insert(QStringLiteral("format"), 2);
  invalid.append(QJsonDocument(wrongFormat).toJson(QJsonDocument::Compact));
  auto missingTable = battleReplayCatalogFixture(QStringLiteral("missing-table"));
  missingTable.remove(QStringLiteral("petMechanics"));
  invalid.append(QJsonDocument(missingTable).toJson(QJsonDocument::Compact));
  auto emptyTable = battleReplayCatalogFixture(QStringLiteral("empty-table"));
  emptyTable.insert(QStringLiteral("buffs"), QJsonObject{});
  invalid.append(QJsonDocument(emptyTable).toJson(QJsonDocument::Compact));
  auto wrongType = battleReplayCatalogFixture(QStringLiteral("wrong-table-type"));
  wrongType.insert(QStringLiteral("skills"), QJsonArray{});
  invalid.append(QJsonDocument(wrongType).toJson(QJsonDocument::Compact));
  auto emptyRow = battleReplayCatalogFixture(QStringLiteral("empty-row"));
  emptyRow.insert(QStringLiteral("skills"), QJsonObject{{QStringLiteral("900000002"), QJsonObject{}}});
  invalid.append(QJsonDocument(emptyRow).toJson(QJsonDocument::Compact));
  auto wrongId = battleReplayCatalogFixture(QStringLiteral("wrong-row-id"));
  auto rows = wrongId.value(QStringLiteral("skills")).toObject();
  auto row = rows.value(QStringLiteral("900000002")).toObject();
  row.insert(QStringLiteral("id"), QStringLiteral("900000099"));
  rows.insert(QStringLiteral("900000002"), row); wrongId.insert(QStringLiteral("skills"), rows);
  invalid.append(QJsonDocument(wrongId).toJson(QJsonDocument::Compact));
  auto wrongCounts = battleReplayCatalogFixture(QStringLiteral("wrong-summary-counts"));
  wrongCounts.insert(QStringLiteral("petMechanicsSummary"), QJsonObject{{QStringLiteral("pets"), 2}});
  invalid.append(QJsonDocument(wrongCounts).toJson(QJsonDocument::Compact));
  auto wrongPresentation = battleReplayCatalogFixture(QStringLiteral("wrong-presentation"));
  wrongPresentation.insert(QStringLiteral("clientPresentation"), QJsonObject{{QStringLiteral("schemaVersion"), 99}});
  invalid.append(QJsonDocument(wrongPresentation).toJson(QJsonDocument::Compact));
  auto wrongSource = battleReplayCatalogFixture(QStringLiteral("wrong-provenance"));
  auto provenance = wrongSource.value(QStringLiteral("provenance")).toObject();
  provenance.insert(QStringLiteral("manifestSha256"), QStringLiteral("not-a-hash"));
  wrongSource.insert(QStringLiteral("provenance"), provenance);
  invalid.append(QJsonDocument(wrongSource).toJson(QJsonDocument::Compact));
  auto duplicateSource = battleReplayCatalogFixture(QStringLiteral("duplicate-provenance"));
  provenance = duplicateSource.value(QStringLiteral("provenance")).toObject();
  auto packages = provenance.value(QStringLiteral("packages")).toArray();
  packages.append(packages.first()); provenance.insert(QStringLiteral("packages"), packages);
  duplicateSource.insert(QStringLiteral("provenance"), provenance);
  invalid.append(QJsonDocument(duplicateSource).toJson(QJsonDocument::Compact));
  for (const auto& bytes : invalid) {
    QString error;
    ok &= check(writeCatalogFixture(path, bytes) && !reload(service, &error) && !error.isEmpty(),
                "invalid replay catalog was accepted or failed without a reason");
    ok &= check(service.battleReplaySnapshot() == second && publications == 2 && readFile(path) == bytes,
                "invalid reload replaced the good snapshot, published a revision, or rewrote the failed file");
  }
  ok &= check(QFile::remove(path) && !reload(service) && service.battleReplaySnapshot() == second &&
      !QFileInfo::exists(path) && publications == 2, "missing external reload replaced good data or created a file");
  QFile oversized(path);
  constexpr qint64 tooLarge = qint64(128) * 1024 * 1024 + 1;
  const bool madeOversized = oversized.open(QIODevice::WriteOnly) && oversized.resize(tooLarge);
  oversized.close();
  QString sizeError;
  ok &= check(madeOversized && !reload(service, &sizeError) && !sizeError.isEmpty() &&
      service.battleReplaySnapshot() == second && QFileInfo(path).size() == tooLarge && publications == 2,
      "oversized replay catalog bypassed its budget or damaged the last good snapshot/file");
  service.close();
  ok &= check(service.requestReload(CatalogKind::BattleReplay) == 0 && storage.shutdown(),
              "closed replay catalog admitted work or did not drain storage");
  return ok;
}

bool completeEmbeddedFallback(const QString& root) {
  StorageService storage(root);
  CatalogIoService service(&storage);
  const QString path = pathFor(root);
  const QByteArray badExternal("{\"format\":2,\"testOnlyMarker\":\"invalid external\"}");
  bool ok = check(writeCatalogFixture(path, badExternal), "fallback fixture could not be written");
  int heartbeats = 0;
  QTimer heart; heart.setInterval(1);
  QObject::connect(&heart, &QTimer::timeout, &heart, [&] { ++heartbeats; });
  heart.start();
  QElapsedTimer elapsed; elapsed.start();
  QString error;
  const bool loaded = reload(service, &error, 50000);
  const qint64 loadMilliseconds = elapsed.elapsed();
  heart.stop();
  if (!loaded) std::fprintf(stderr, "embedded fallback: %s\n", error.toUtf8().constData());
  const auto snapshot = service.battleReplaySnapshot();
  ok &= check(loaded && snapshot && snapshot->embedded &&
      snapshot->sourcePath == QStringLiteral(":/kqpet/battle-replay-catalog.json") && !snapshot->warning.isEmpty() &&
      snapshot->root.value(QStringLiteral("format")).toInt() == 3 &&
      snapshot->root.value(QStringLiteral("skills")).toObject().size() > 20000 &&
      snapshot->root.value(QStringLiteral("buffs")).toObject().size() > 30000 && heartbeats > 1,
      "complete embedded catalog failed background validation/fallback or stalled the caller event loop");
  QFile resource(QStringLiteral(":/kqpet/battle-replay-catalog.json"));
  QCryptographicHash hash(QCryptographicHash::Sha256);
  const bool opened = resource.open(QIODevice::ReadOnly);
  if (opened) while (!resource.atEnd()) hash.addData(resource.read(64 * 1024));
  ok &= check(opened && resource.size() > 32 * 1024 * 1024 && resource.error() == QFileDevice::NoError &&
      snapshot && snapshot->sha256 == QString::fromLatin1(hash.result().toHex()) && readFile(path) == badExternal,
      "large embedded bytes were truncated, fingerprinted incorrectly, or copied over the user's file");
  std::printf("replay full catalog: bytes=%lld elapsed_ms=%lld heartbeat_pulses=%d max_io_slice_us=%lld\n",
      static_cast<long long>(resource.size()), static_cast<long long>(loadMilliseconds), heartbeats,
      static_cast<long long>(service.stats().maximumSliceMicroseconds));
  service.close();
  ok &= check(storage.shutdown(), "embedded fallback storage did not stop");
  return ok;
}

struct IoBarrier { QSemaphore entered, release; };
bool coalescingAndDestruction(const QString& root) {
  bool ok = true;
  {
    StorageService storage(QDir(root).filePath(QStringLiteral("coalesce")));
    CatalogIoService service(&storage);
    const auto path = pathFor(storage.dataRoot());
    ok &= check(writeCatalogFixture(path, battleReplayCatalogFixtureBytes(QStringLiteral("initial"))) && reload(service),
                "coalescing initial replay failed");
    const auto previous = service.battleReplaySnapshot();
    auto barrier = std::make_shared<IoBarrier>();
    const bool held = storage.postAuxiliary([barrier](QObject*) { barrier->entered.release(); barrier->release.acquire(); });
    ok &= check(held && barrier->entered.tryAcquire(1, 3000), "coalescing IO barrier did not enter");
    QHash<quint64, StorageStatus> completions;
    int publications = 0;
    QObject::connect(&service, &CatalogIoService::finished, &service,
        [&](quint64 id, CatalogKind kind, StorageStatus status, const QString&) {
      if (kind == CatalogKind::BattleReplay) completions.insert(id, status);
    });
    QObject::connect(&service, &CatalogIoService::catalogUpdated, &service,
        [&](CatalogKind kind, quint64) { if (kind == CatalogKind::BattleReplay) ++publications; });
    const quint64 firstId = service.requestReload(CatalogKind::BattleReplay);
    ok &= check(firstId && waitUntil([&] { return storage.state().outstandingTasks >= 2; }),
                "replay read was not queued behind the existing IO task");
    quint64 lastId = firstId;
    for (int i = 0; i < 100; ++i) lastId = service.requestReload(CatalogKind::BattleReplay);
    ok &= check(writeCatalogFixture(path, battleReplayCatalogFixtureBytes(QStringLiteral("last"))) &&
        service.stats().pendingRequests <= 2 && publications == 0 && service.battleReplaySnapshot() == previous,
        "queued reloads grew unbounded or published before IO was available");
    barrier->release.release();
    ok &= check(waitUntil([&] { return service.stats().pendingRequests == 0 && completions.size() == 101; }) &&
        completions.value(firstId) == StorageStatus::Cancelled && completions.value(lastId) == StorageStatus::Loaded &&
        publications == 1 && marker(service.battleReplaySnapshot()) == QStringLiteral("last") &&
        marker(previous) == QStringLiteral("initial"), "cancelled replay read published or the latest read was lost");
    service.close();
    ok &= check(storage.shutdown(), "coalescing replay storage did not drain");
  }
  {
    StorageService storage(QDir(root).filePath(QStringLiteral("destroy")));
    const auto bytes = battleReplayCatalogFixtureBytes(QStringLiteral("must-not-publish"));
    const auto path = pathFor(storage.dataRoot());
    ok &= check(writeCatalogFixture(path, bytes), "destruction fixture could not be written");
    auto barrier = std::make_shared<IoBarrier>();
    const bool held = storage.postAuxiliary([barrier](QObject*) { barrier->entered.release(); barrier->release.acquire(); });
    ok &= check(held && barrier->entered.tryAcquire(1, 3000), "destruction IO barrier did not enter");
    auto service = std::make_unique<CatalogIoService>(&storage);
    int publications = 0;
    QObject::connect(service.get(), &CatalogIoService::catalogUpdated, QCoreApplication::instance(),
        [&](CatalogKind kind, quint64) { if (kind == CatalogKind::BattleReplay) ++publications; });
    service->requestReload(CatalogKind::BattleReplay);
    ok &= check(waitUntil([&] { return storage.state().outstandingTasks >= 2; }),
                "destruction replay read did not reach the IO queue");
    service.reset();
    barrier->release.release();
    ok &= check(waitUntil([&] { return storage.state().outstandingTasks == 0; }) && storage.shutdown() &&
        publications == 0 && readFile(path) == bytes, "destroyed replay receiver published or altered its source file");
  }
  return ok;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  bool ok = check(root.isValid(), "temporary replay catalog root unavailable");
  if (!ok) return 1;
  ok &= externalLifecycle(root.filePath(QStringLiteral("external")));
  ok &= coalescingAndDestruction(root.filePath(QStringLiteral("lifecycle")));
  ok &= completeEmbeddedFallback(root.filePath(QStringLiteral("fallback")));
  if (ok) std::puts("PASS: replay catalog external priority, immutable revisions, failures, bounds, cancellation and full fallback");
  return ok ? 0 : 1;
}
