#include "protocol_test_support.h"
#include "asset_analysis_controller.h"
#include "pet_derivation_cache.h"
#include "inventory_publisher.h"
#include "inventory_projection.h"
#include "pet_detail_catalog.h"
#include "shop_exchange_catalog.h"
#include "storage_service.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <atomic>

namespace {
bool require(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
bool until(const std::function<bool()>& predicate, int timeout = 10000) {
  QElapsedTimer timer; timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (predicate()) return true;
    QThread::msleep(1);
  } while (timer.elapsed() < timeout);
  return false;
}
QJsonObject detail(qint64 id, int power = 7000) {
  return {{"id", QString::number(id)}, {"r", 7001}, {"fr", 7001}, {"n", "cached pet"},
      {"lv", 120}, {"zdl", power}, {"xzdl", 10000}, {"sgs", "66:8#67:8#70:8"},
      {"czdlv", QJsonObject{{"lv", power}}}, {"mzdlv", QJsonObject{{"lv", 10000}}},
      {"sgsp", QJsonArray{71, 72}}, {"raw_future_field", QJsonObject{{"preserve", true}}}};
}
bool writeFile(const QString& path, const QByteArray& bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path) {
  QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
void login(PetRepository& repository, const QString& account) {
  deliverVerifiedFixture(&repository, {{"_cmd", "21_1"}, {"info", QJsonObject{{"n", account}}}});
}
void lists(PetRepository& repository) {
  const auto account = repository.accountKey(); const auto epoch = repository.sessionGeneration();
  repository.beginListRefresh(1, account, epoch);
  deliverVerifiedFixture(&repository, {{"_cmd", "2_1_10"}, {"pl", QJsonArray{detail(1001)}},
      {"pps", QJsonArray{"1001"}}, {"ppc", 12}});
  repository.expectListPart("2_1_S", 1, account, epoch);
  deliverVerifiedFixture(&repository, {{"_cmd", "2_1_S"},
      {"ns", QJsonArray{QJsonObject{{"id", "2001"}, {"ri", 7001}, {"lv", 120}, {"n", "cached pet"}},
                         QJsonObject{{"id", "2002"}, {"ri", 7001}, {"lv", 120}, {"n", "never observed"}}}},
      {"es", QJsonArray{}}, {"rb", QJsonArray{}}});
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir directory;
  if (!directory.isValid()) return 1;
  qputenv("KQPET_DATA_ROOT", directory.path().toUtf8());
  bool ok = true;
  PetRecordCacheLimits limits; limits.maximumRecords = 1;
  PetRepository repository(nullptr, nullptr, {}, limits);
  ok &= require(waitForRepositoryIdle(&repository), "initial cache IO completes");
  login(repository, "cache-integration-a");
  ok &= require(waitForRepositoryIdle(&repository), "login cache IO completes");
  lists(repository);
  ok &= require(waitForRepositoryIdle(&repository), "fixture lists saved");
  const auto detailRoot = QFileInfo(repository.cachePath()).absolutePath() + "/details/";
  const auto firstPath = detailRoot + "1001.json";
  const auto firstBytes = readFile(firstPath);
  ok &= require(!firstBytes.isEmpty(), "complete backpack has independently durable original");
  // The account scan has already finished. Manual preparation must discover a
  // newly available file even when a cached summary-only fact already exists.
  const auto diskBytes = QJsonDocument(QJsonObject{{"schema", 3}, {"account", repository.accountKey()},
      {"instanceId", "2001"}, {"complete", true}, {"savedAt", "2026-09-09T03:00:00Z"},
      {"pet", detail(2001, 8000)}}).toJson(QJsonDocument::Compact);
  ok &= require(writeFile(detailRoot + "2001.json", diskBytes), "isolated local detail fixture created");
  auto frozenMetadata = std::make_shared<PetDetailCatalogSnapshot>(*PetDetailCatalog::instance().snapshot());
  frozenMetadata->revision += 100;
  frozenMetadata->root.insert("environment-fixture", true);
  frozenMetadata->contentDigest = QCryptographicHash::hash(
      QJsonDocument(frozenMetadata->root).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  auto frozenShop = std::make_shared<ShopCatalogSnapshot>(*ShopExchangeCatalog::instance().snapshot());
  frozenShop->revision += 100;
  const QDate frozenDate(2026, 9, 9);
  AnalysisEnvironment environment;
  environment.businessDate = [frozenDate] { return frozenDate; };
  environment.petMetadataSnapshot = [frozenMetadata] { return frozenMetadata; };
  environment.shopCatalogSnapshot = [frozenShop] { return frozenShop; };
  AssetAnalysisController controller(&repository, nullptr, nullptr, nullptr, environment);
  QList<AnalysisJobFinished> completions;
  QObject::connect(&controller, &AssetAnalysisController::analysisJobFinished,
      [&](const AnalysisJobFinished& finished) { completions.append(finished); });
  controller.setCompatibilityIdentity("fixture-build", "fixture-profile", true);
  bool deferCompute = false;
  QList<std::function<void()>> deferredCompute;
  PetDerivationCache cache([&](std::function<void()> work) {
    if (deferCompute) { deferredCompute.append(std::move(work)); return true; }
    return controller.postPriorityCompute(std::move(work));
  },
      repository.storageService());
  controller.setDerivationCache(&cache);
  InventoryProjection projection;
  InventoryPublisher publisher(&repository, &projection);
  publisher.metadataUpdated(frozenMetadata);
  QObject::connect(&controller, &AssetAnalysisController::derivedFactsChanged, &publisher,
      [&](qint64 id, const PetDerivedFactsHandle& facts) { publisher.factsUpdated(id, facts); });
  QObject::connect(&projection, &InventoryProjection::detailInterestsChanged, &publisher,
      [&](const QString& account, quint64 epoch, const QSet<qint64>& ids) {
    if (account == repository.accountKey() && epoch == repository.sessionGeneration()) publisher.setDetailInterests(ids);
  });
  QString lastStatus;
  QObject::connect(&controller, &AssetAnalysisController::statusChanged,
      [&](const QString& message) { lastStatus = message; });
  ok &= require(until([&] { return projection.derivedFactsFor(2002) && cache.stats().pendingTasks == 0; }),
      "background summary facts reach projection on the shared Compute executor");
  const auto absent = projection.derivedFactsFor(2002);
  ok &= require(absent && !absent->facts.asset.detailAvailable && !repository.recordVersion(2001).complete,
      "summary-only cache is explicitly incomplete before manual disk lookup");
  controller.requestAnalysis();
  const bool firstFinished = until([&] { return !controller.analysisRunning(); });
  ok &= require(firstFinished, "prepared analysis finishes");
  if (!firstFinished) {
    const auto rawStats = repository.rawCacheStats(); const auto cacheStats = cache.stats();
    std::fprintf(stderr, "STATE: raw=%d protected=%d bytes=%llu evicted=%llu reads=%d cacheTasks=%d active=%d computations=%llu rejected=%llu\n",
        rawStats.residentRecords, rawStats.protectedRecords, rawStats.chargedBytes, rawStats.evictedRecords,
        repository.pendingReadCount(), cacheStats.pendingTasks, cacheStats.activeTasks, cacheStats.computations, cacheStats.rejected);
    for (qint64 id : {1001, 2001, 2002}) {
      const auto version = repository.recordVersion(id);
      std::fprintf(stderr, "RECORD: id=%lld revision=%llu complete=%d persisted=%d derived=%d resident=%d projectedFacts=%d\n",
          id, version.key.detailMemoryRevision, version.complete, version.persisted, version.derived, version.resident,
          bool(projection.derivedFactsFor(id)));
    }
    controller.shutdownAnalysis(2000); return 1;
  }
  if (!controller.hasAnalysis()) std::fprintf(stderr, "STATUS: %s\n", lastStatus.toUtf8().constData());
  const auto first = controller.overview();
  const auto firstResult = controller.retainedAnalysisResult();
  ok &= require(firstResult && firstResult->key.versions.metadata == frozenMetadata->revision &&
      firstResult->key.versions.catalog == frozenShop->revision &&
      firstResult->key.versions.catalogDay == static_cast<quint64>(frozenDate.toJulianDay()) &&
      !completions.isEmpty() && completions.last().phase == AnalysisJobPhase::Compute &&
      completions.last().outcome == AnalysisJobOutcome::Published,
      "frozen reference providers bind both preparation and Worker versions with actual completion metrics");
  ok &= require(controller.hasAnalysis() && first.totalPets == 3 && first.pets.size() == 3 &&
      first.missingDetailPets == 1 && repository.recordVersion(2001).complete,
      "local detail promotes only its own unknown fact; all P survive preparation");
  ok &= require(until([&] { return projection.derivedFactsFor(2001) && cache.stats().pendingTasks == 0; }),
      "loaded fact reaches GUI projection");
  ok &= require(!repository.rawRecordResident(1001) && repository.recordVersion(1001).complete &&
      projection.derivedFactsFor(1001) && projection.snapshot()->rawDetails.isEmpty(),
      "raw LRU evicts durable derived payload without downgrading known facts or pinning all raw in GUI");
  const auto warmCalculations = cache.stats().computations;
  const auto previousRuns = controller.analysisRunCount();
  controller.requestAnalysis();
  ok &= require(until([&] { return !controller.analysisRunning(); }) &&
      controller.analysisRunCount() == previousRuns + 1 && cache.stats().computations == warmCalculations,
      "warm manual analysis reuses all matching facts without parsing cultivation again");
  projection.watchDetail(0, 2001);
  ok &= require(until([&] { return bool(projection.rawRecordHandle(2001)); }) &&
      projection.snapshot()->rawDetails.size() == 1 &&
      projection.detailFor(2001).contains("raw_future_field"), "one selected raw interest receives lossless leased detail");
  auto retainedRaw = projection.rawRecordHandle(2001);
  projection.watchDetail(0, 0);
  ok &= require(until([&] { return projection.snapshot()->rawDetails.isEmpty(); }), "unwatch drops projection raw lease");
  ok &= require(repository.rawCacheStats().untrackedExports == 0, "production preparation and publisher make no naked raw exports");
  // A missing original of a formerly complete version is a terminal failure.
  // It must retain the last valid overview, not silently issue Unknown facts.
  ok &= require(QFile::remove(firstPath), "remove only the isolated evicted original fixture");
  cache.clear(); lastStatus.clear();
  controller.requestAnalysis();
  ok &= require(until([&] { return !controller.analysisRunning(); }) && controller.hasAnalysis() &&
      controller.overview().missingDetailPets == 1 && controller.analysisRunCount() == previousRuns + 1 &&
      lastStatus.contains(QStringLiteral("本地原文不可用")), "lost durable original fails preparation and preserves last result");
  if (!lastStatus.contains(QStringLiteral("本地原文不可用"))) std::fprintf(stderr, "STATUS: %s\n", lastStatus.toUtf8().constData());
  ok &= require(!completions.isEmpty() && completions.last().phase == AnalysisJobPhase::Preparation &&
      completions.last().outcome == AnalysisJobOutcome::InputRejected && !completions.last().error.isEmpty(),
      "failed local preparation reports its phase rather than a successful Compute sample");
  ok &= require(writeFile(firstPath, firstBytes), "restore byte-identical isolated original");
  retainedRaw.reset();
  controller.requestAnalysis();
  ok &= require(until([&] { return !controller.analysisRunning(); }) && controller.analysisRunCount() == previousRuns + 2,
      "byte-identical durable original can restore the same known version");
  deferCompute = true;
  const auto burstStart = cache.stats().computations;
  const auto observe = [&](int value, quint64 request) {
    repository.expectDetail(2001, request, repository.accountKey(), repository.sessionGeneration());
    deliverVerifiedFixture(&repository, {{"_cmd", "2_1_R"}, {"p", detail(2001, value)}});
  };
  observe(9000, 10);
  ok &= require(until([&] { return deferredCompute.size() == 1; }), "hold one actual cache computation in flight");
  for (int revision = 0; revision < 1000; ++revision) observe(9001 + revision, 11 + revision);
  const auto latestVersion = repository.recordVersion(2001).key;
  deferCompute = false;
  for (auto& work : deferredCompute) {
    ok &= require(until([&] { return controller.postPriorityCompute(work); }), "resume held work on the shared Compute executor");
  }
  deferredCompute.clear();
  const bool burstPublished = until([&] {
    const auto facts = projection.derivedFactsFor(2001);
    // This fixture reply carries one power component, which by the confirmed
    // rules leaves the local total unknown. The observed server value is the
    // per-revision discriminator here, and the record version proves which
    // input actually reached the projection.
    return facts && facts->key.record == latestVersion &&
        facts->facts.battlePower.hasServerCurrent && facts->facts.battlePower.serverCurrent == 10000 &&
        !cache.stats().pendingTasks;
  });
  if (!burstPublished) {
    const auto facts = projection.derivedFactsFor(2001);
    const auto stats = cache.stats();
    std::fprintf(stderr,
        "BURST: projected=%d projectedRevision=%llu projectedServerPower=%d latestRevision=%llu "
        "pending=%d active=%d resident=%d computations=%llu rejected=%llu latestComplete=%d latestPersisted=%d "
        "latestDerived=%d reads=%d\n",
        bool(facts), facts ? facts->key.record.detailMemoryRevision : 0,
        facts ? facts->facts.battlePower.serverCurrent : -1, latestVersion.detailMemoryRevision,
        stats.pendingTasks, stats.activeTasks, stats.residentEntries, stats.computations, stats.rejected,
        int(repository.recordVersion(2001).complete), int(repository.recordVersion(2001).persisted),
        int(repository.recordVersion(2001).derived), repository.pendingReadCount());
  }
  ok &= require(burstPublished,
                "one thousand updates supersede old computation and publish only the newest version");
  ok &= require(cache.stats().computations <= burstStart + 2 && cache.stats().residentEntries <= 3 &&
      repository.rawCacheStats().chargedBytes <= limits.maximumBytes,
      "superseded detail versions do not accumulate compute tasks or resident facts");
  // The artificial 1000-write burst may reject the last save while Core has
  // not yet consumed queue receipts. Resolve that independent save failure
  // before asking a one-slot raw cache to evict this protected original.
  ok &= require(waitForRepositoryIdle(&repository, 20000), "burst write receipts drain");
  observe(10000, 1500);
  ok &= require(waitForRepositoryIdle(&repository, 20000) && repository.recordVersion(2001).persisted,
      "latest burst observation is durably saved before the separate read-pressure scenario");
  QJsonArray pressureBriefs{
      QJsonObject{{"id", "2001"}, {"ri", 7001}, {"lv", 120}, {"n", "cached pet"}},
      QJsonObject{{"id", "2002"}, {"ri", 7001}, {"lv", 120}, {"n", "never observed"}}};
  for (qint64 id = 3000; id < 3512; ++id) {
    pressureBriefs.append(QJsonObject{{"id", QString::number(id)}, {"ri", 7001}, {"lv", 120}, {"n", "cached pet"}});
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 3}, {"account", repository.accountKey()},
        {"instanceId", QString::number(id)}, {"complete", true}, {"savedAt", "2026-09-09T03:00:00Z"},
        {"pet", detail(id)}}).toJson(QJsonDocument::Compact);
    ok &= require(writeFile(detailRoot + QString::number(id) + ".json", bytes), "create isolated pressure original");
  }
  repository.expectListPart("2_1_S", 2000, repository.accountKey(), repository.sessionGeneration());
  deliverVerifiedFixture(&repository, {{"_cmd", "2_1_S"}, {"ns", pressureBriefs}, {"es", QJsonArray{}}, {"rb", QJsonArray{}}});
  const auto retriesBefore = controller.preparationReadAdmissionRetries();
  std::atomic_bool ioHeld{false};
  ok &= require(repository.storageService()->postAuxiliary([&ioHeld](QObject*) {
    ioHeld.store(true); QThread::msleep(250);
  }), "isolated bounded IO delay admitted");
  ok &= require(until([&] { return ioHeld.load(); }), "IO delay actually started before request burst");
  controller.requestAnalysis();
  const bool pressureFinished = until([&] { return !controller.analysisRunning(); }, 20000);
  if (!pressureFinished || controller.overview().totalPets != 515 || controller.overview().missingDetailPets != 1) {
    const auto rawState = repository.rawCacheStats(); const auto factState = cache.stats();
    std::fprintf(stderr, "PRESSURE: finished=%d P=%d missing=%d protected=%d raw=%d reads=%d facts=%d retries=%llu status=%s\n",
        pressureFinished, controller.overview().totalPets, controller.overview().missingDetailPets, rawState.protectedRecords,
        rawState.residentRecords, repository.pendingReadCount(), factState.pendingTasks,
        controller.preparationReadAdmissionRetries(), lastStatus.toUtf8().constData());
  }
  ok &= require(pressureFinished &&
      controller.overview().totalPets == 515 && controller.overview().missingDetailPets == 1 &&
      controller.preparationReadAdmissionRetries() > retriesBefore,
      "queue rejection retries actual reads for all 512 originals instead of manufacturing missing detail");
  controller.requestAnalysis(); controller.cancelAnalysis();
  ok &= require(!controller.analysisRunning() && !completions.isEmpty() &&
      completions.last().phase == AnalysisJobPhase::Preparation && completions.last().outcome == AnalysisJobOutcome::Cancelled,
      "preparation cancellation is immediate and reports its phase");
  const auto oldEpoch = repository.sessionGeneration();
  login(repository, "cache-integration-b");
  ok &= require(until([&] { return projection.accountKey() == "cache-integration-b"; }) &&
      repository.sessionGeneration() != oldEpoch && !controller.hasAnalysis() &&
      projection.snapshot()->facts.isEmpty() && projection.snapshot()->rawDetails.isEmpty(),
      "account transition clears prepared references and cannot publish old-account facts");
  ok &= require(controller.shutdownAnalysis(2000), "shared cache and Compute stop cleanly");
  // A burst of detail updates must not republish the whole list per response,
  // while every changed id and every final value still reaches the projection.
  {
    ok &= require(waitForRepositoryIdle(&repository, 10000), "burst account settles");
    lists(repository);
    ok &= require(waitForRepositoryIdle(&repository, 10000), "burst lists saved");
    QThread::msleep(40);
    quint64 publications = projection.snapshot() ? projection.snapshot()->publication : 0;
    const quint64 before = publications;
    const int burstUpdates = 200;
    QElapsedTimer burst;
    burst.start();
    for (int round = 1; round <= burstUpdates; ++round) {
      repository.expectDetail(2001, 40000 + round, repository.accountKey(), repository.sessionGeneration());
      deliverVerifiedFixture(&repository, {{"_cmd", "2_1_R"}, {"p", detail(2001, 9000 + round)}});
      QCoreApplication::processEvents();
      // A realistic response cadence: one event-loop turn per update.
      QThread::msleep(1);
    }
    const qint64 burstMs = burst.elapsed();
    std::fprintf(stderr, "BURST_RAW: start=%llu end=%llu publications=%llu\n",
                 static_cast<unsigned long long>(before),
                 static_cast<unsigned long long>(projection.snapshot()->publication),
                 static_cast<unsigned long long>(projection.snapshot()->publication - before));
    const quint64 revisionAfterBurst = repository.recordVersion(2001).key.detailMemoryRevision;
    // Wait for the merged publication that carries the newest version, and read
    // its accumulated changed-id set from that exact snapshot.
    quint64 mergedPublication = 0;
    QSet<qint64> mergedChanges;
    bool mergedFull = false;
    ok &= require(until([&] {
      const auto snapshot = projection.snapshot();
      if (!snapshot || snapshot->publication <= before ||
          snapshot->recordVersions.value(2001).key.detailMemoryRevision < revisionAfterBurst) return false;
      mergedPublication = snapshot->publication;
      mergedChanges = snapshot->changedDetails;
      mergedFull = snapshot->membershipChanged;
      return true;
    }, 5000), "a merged publication carrying the newest version reached the projection");
    const quint64 published = mergedPublication - before;
    // A detail-only publication carries the accumulated ids; a membership
    // publication carries every list record, which is a superset of the delta.
    const bool mergedBurst = published > 0 && published * 4 <= quint64(burstUpdates) &&
        (mergedChanges.contains(2001) || mergedFull);
    if (!mergedBurst) {
      std::fprintf(stderr,
          "BURST_STATE: published=%llu updates=%d changedIds=%lld contains2001=%d full=%d\n",
          static_cast<unsigned long long>(published), burstUpdates,
          static_cast<long long>(mergedChanges.size()), int(mergedChanges.contains(2001)),
          int(mergedFull));
    }
    ok &= require(mergedBurst,
                  "a detail burst republished the list per response or lost a changed version");
    std::fprintf(stdout,
        "publisher burst: updates=%d publications=%llu elapsedMs=%lld changedIds=%lld full=%d\n",
        burstUpdates, static_cast<unsigned long long>(published),
        static_cast<long long>(burstMs), static_cast<long long>(mergedChanges.size()),
        int(mergedFull));
  }
  std::puts(ok ? "PASS: Repository -> raw leases -> fact cache -> Worker -> projection integration" : "FAIL: analysis cache integration");
  return ok ? 0 : 1;
}