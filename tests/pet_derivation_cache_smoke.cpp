#include "pet_derivation_cache.h"
#include "../src/domain/asset_derivation.h"
#include "storage_service.h"
#include "protocol_transport.h"
#include "pet_repository.h"
#include "protocol_test_support.h"
#include "../src/domain/pet_identity.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QSaveFile>
#include <QThread>
#include <QTemporaryDir>
#include <deque>
#include <atomic>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) {
  if (!valid) std::fprintf(stderr, "FAIL: %s\n", message);
  return valid;
}
template<class Predicate> bool waitUntil(Predicate predicate, int maximumMs = 2000) {
  QElapsedTimer elapsed; elapsed.start();
  while (!predicate() && elapsed.elapsed() < maximumMs) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return predicate();
}
struct TestComputeThread final : QThread {
  QSemaphore started;
  QObject* worker = nullptr;
  void run() override { QObject object; worker = &object; started.release(); exec(); worker = nullptr; }
};
struct ExistingCompute {
  TestComputeThread thread;
  std::deque<std::function<void()>> queued;
  ExistingCompute() { thread.start(); thread.started.acquire(); }
  ~ExistingCompute() { thread.quit(); thread.wait(); }
  PetDerivationCache::ComputeExecutor executor() {
    return [this](std::function<void()> operation) { queued.push_back(std::move(operation)); return true; };
  }
  bool runOne() {
    if (queued.empty()) return false;
    auto operation = std::move(queued.front()); queued.pop_front();
    return QMetaObject::invokeMethod(thread.worker, [operation = std::move(operation)] { operation(); }, Qt::BlockingQueuedConnection);
  }
};
PetDerivationRequest requestFor(qint64 id, int backpackEntries = 1, const QString& account = QStringLiteral("A"), quint64 epoch = 1) {
  QJsonArray stars;
  for (int i = 0; i < backpackEntries; ++i) stars.append(QJsonObject{{QStringLiteral("i"), 9}, {QStringLiteral("l"), 1}, {QStringLiteral("n"), 1}});
  QJsonObject object{{QStringLiteral("id"), QString::number(id)}, {QStringLiteral("r"), 7001}, {QStringLiteral("lv"), 100},
      {QStringLiteral("n"), QStringLiteral("synthetic-%1").arg(id)}, {QStringLiteral("zdl"), 1200},
      {QStringLiteral("czdlv"), QJsonObject{{QStringLiteral("lv"), 1000}, {QStringLiteral("sgv"), 0}, {QStringLiteral("bsv"), 200}}},
      {QStringLiteral("mzdlv"), QJsonObject{{QStringLiteral("lv"), 1000}, {QStringLiteral("sgv"), 0}, {QStringLiteral("bsv"), 300}}},
      {QStringLiteral("sgs"), QStringLiteral("0:9")}, {QStringLiteral("sgsp"), stars},
      {QStringLiteral("relations"), QJsonArray{QJsonObject{{QStringLiteral("ignored"), QString(512, QLatin1Char('x'))}}}}};
  auto payload = std::make_shared<RawPetRecordPayload>(); payload->object = object;
  payload->chargedBytes = quint64(QJsonDocument(object).toJson(QJsonDocument::Compact).size()) * 4 + 2048;
  auto raw = std::make_shared<RawPetRecord>(); raw->key = {account, epoch, id, 1}; raw->payload = payload;
  raw->complete = true; raw->sourceKnown = true; raw->brief = AssetDerivation::identityFields(object);
  PetDerivationRequest request; request.raw = raw; request.metadataRevision = 1;
  request.seed.instanceId = id; request.seed.raceId = 7001; request.seed.metadataSlotMaxLevel = 6;
  request.seed.name = QStringLiteral("synthetic-%1").arg(id); request.seed.location = QStringLiteral("仓库");
  request.seed.detailAvailable = true; request.seed.observationVerified = true; request.seed.pet = raw->brief;
  return request;
}
std::optional<PetAnalysisFacts> direct(const PetDerivationRequest& request) {
  auto seed = request.seed;
  seed.pet = AssetDerivation::analysisInputFields(request.raw->brief, request.raw->object());
  return derivePetAnalysisFacts(seed, request.metadata);
}
bool equivalenceAndLeases() {
  bool ok = true;
  ExistingCompute compute;
  PetDerivationCacheLimits limits; limits.residentEntries = 1;
  PetDerivationCache cache(compute.executor(), nullptr, limits); cache.bindSession(QStringLiteral("A"), 1);
  PetDerivedFactsHandle published;
  QObject::connect(&cache, &PetDerivationCache::ready, &cache, [&](quint64, const PetDerivationKey&, const PetDerivedFactsHandle& facts) { published = facts; });
  auto first = requestFor(101, 256);
  const auto expected = direct(first);
  ok &= check(expected.has_value(), "direct fact fixture was invalid");
  const auto firstKey = PetDerivationCache::keyFor(first);
  std::weak_ptr<const RawPetRecordPayload> rawPayload = first.raw->payload;
  const auto accepted = cache.request(first);
  const auto duplicate = cache.request(first);
  ok &= check(accepted.accepted && duplicate.taskId == accepted.taskId && duplicate.status == PetDerivationStatus::AlreadyQueued,
              "same key accumulated multiple raw derivation jobs");
  first.raw.reset();
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }) && !rawPayload.expired() && cache.stats().activeTasks == 1,
              "raw lease was released before Compute or no existing executor was used");
  ok &= check(cache.stats().completedComputeMicroseconds == 0 && cache.stats().completedRawDeriveMicroseconds == 0,
              "queued work was charged as executed Compute time before dispatch");
  compute.runOne();
  ok &= check(waitUntil([&] { return bool(published); }) && rawPayload.expired() &&
                  petAnalysisFactsToJson(published->facts) == petAnalysisFactsToJson(*expected) &&
                  !published->facts.asset.pet.contains(QStringLiteral("sgsp")),
              "cached facts differ from the raw factory or retain the raw payload");
  PetAnalysisFacts externalCopy = published->facts;
  const quint64 firstCharge = published->chargedBytes;
  published.reset();
  const auto beforeHit = cache.stats();
  ok &= check(cache.lookup(firstKey) && cache.stats().computations == 1,
              "cache hit recomputed the raw record");
  ok &= check(beforeHit.completedComputeMicroseconds > 0 && beforeHit.completedRawDeriveMicroseconds > 0 &&
                  beforeHit.completedIndexDecodeMicroseconds == 0 &&
                  beforeHit.completedComputeMicroseconds >= beforeHit.completedRawDeriveMicroseconds &&
                  cache.stats().completedComputeMicroseconds == beforeHit.completedComputeMicroseconds,
              "raw factory timing was missing, counted index work, or a RAM hit accrued Compute execution time");
  auto second = requestFor(102, 1024);
  const auto secondKey = PetDerivationCache::keyFor(second);
  cache.request(second); second.raw.reset();
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }), "second raw job was not posted");
  compute.runOne();
  ok &= check(waitUntil([&] { return published && published->key == secondKey; }) && cache.stats().residentEntries == 1 &&
                  !cache.lookup(firstKey) && cache.stats().retainedFactsBytes >= firstCharge + published->chargedBytes,
              "LRU eviction stopped charging facts still held by an external value");
  cache.clear();
  ok &= check(cache.stats().residentFactsBytes == 0 && cache.stats().retainedFactsBytes > 0,
              "clear falsely reported that external/GUI/Analysis references were released");
  published.reset();
  ok &= check(cache.stats().retainedFactsBytes == firstCharge, "lease accounting did not separate the remaining external copy");
  externalCopy = {};
  ok &= check(waitUntil([&] { return cache.stats().retainedFactsBytes == 0; }) && cache.stats().liveFactLeases == 0,
              "final external copy did not return its fact lease");
  return ok;
}
bool cancellationAndEpochs() {
  bool ok = true;
  ExistingCompute compute;
  auto cache = std::make_unique<PetDerivationCache>(compute.executor()); cache->bindSession(QStringLiteral("A"), 1);
  int readyA = 0, readyB = 0;
  QObject::connect(cache.get(), &PetDerivationCache::ready, cache.get(),
      [&](quint64, const PetDerivationKey& key, const PetDerivedFactsHandle&) { key.record.account == QStringLiteral("A") ? ++readyA : ++readyB; });
  auto old = requestFor(101);
  std::weak_ptr<const RawPetRecordPayload> oldPayload = old.raw->payload;
  cache->request(old); old.raw.reset();
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }), "epoch fixture was not posted");
  cache->bindSession(QStringLiteral("B"), 2);
  auto next = requestFor(201, 1, QStringLiteral("B"), 2);
  cache->request(next); next.raw.reset();
  QCoreApplication::processEvents();
  ok &= check(compute.queued.size() == 1 && !oldPayload.expired(), "new session overlapped a still-posted old raw derivation");
  compute.runOne();
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }) && oldPayload.expired() && readyA == 0,
              "cancelled old account published or retained its raw lease after completion");
  compute.runOne();
  ok &= check(waitUntil([&] { return readyB == 1; }), "new account did not resume after the old task released");
  auto closing = requestFor(202, 1, QStringLiteral("B"), 2);
  std::weak_ptr<const RawPetRecordPayload> closingPayload = closing.raw->payload;
  cache->request(closing); closing.raw.reset();
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }) && !cache->shutdown(), "shutdown waited or claimed an outstanding callback was gone");
  cache.reset();
  ok &= check(!closingPayload.expired(), "destroying Core cache prematurely released the Compute raw handle");
  compute.runOne(); QCoreApplication::processEvents();
  ok &= check(closingPayload.expired(), "detached completion retained raw or accessed a destroyed Core cache");
  return ok;
}
bool overloadAndIdentity() {
  bool ok = true;
  ExistingCompute compute;
  PetDerivationCacheLimits limits; limits.queuedTasks = 1;
  PetDerivationCache cache(compute.executor(), nullptr, limits); cache.bindSession(QStringLiteral("A"), 1);
  auto a = requestFor(101), b = requestFor(102);
  auto queued = cache.request(a);
  ok &= check(queued.accepted && !cache.request(b).accepted && cache.stats().pendingTasks == 1,
              "task queue exceeded its fixed admission limit");
  cache.cancel(queued.taskId);
  ok &= check(cache.stats().pendingTasks == 0 && cache.stats().queuedInputBytes == 0,
              "queued cancellation kept unneeded raw inputs");
  auto invalidRaw = std::make_shared<RawPetRecord>(*a.raw);
  auto payload = std::make_shared<RawPetRecordPayload>(); payload->object = a.raw->object();
  payload->object.insert(QStringLiteral("id"), QStringLiteral("999")); payload->chargedBytes = a.raw->payload->chargedBytes;
  invalidRaw->payload = payload; a.raw = invalidRaw;
  ok &= check(cache.request(a).status == PetDerivationStatus::RawIdentityMismatch,
              "raw identity was silently repaired by the seed's identity projection");
  PetDerivationCacheLimits bytes; bytes.queuedBytes = 1024;
  PetDerivationCache tinyInput(compute.executor(), nullptr, bytes); tinyInput.bindSession(QStringLiteral("A"), 1);
  ok &= check(tinyInput.request(b).status == PetDerivationStatus::QueueFull && tinyInput.stats().queuedInputBytes == 0,
              "oversized input was retained outside the queue-byte budget");
  return ok;
}
bool unknownAndMetadata() {
  bool ok = true;
  ExistingCompute compute;
  PetDerivationCache cache(compute.executor()); cache.bindSession(QStringLiteral("A"), 1);
  PetDerivedFactsHandle ready;
  QObject::connect(&cache, &PetDerivationCache::ready, &cache, [&](quint64, const PetDerivationKey&, const PetDerivedFactsHandle& facts) { ready = facts; });
  auto request = requestFor(301);
  auto unknown = std::make_shared<RawPetRecord>(*request.raw); unknown->complete = false; unknown->sourceKnown = false; unknown->payload.reset();
  request.raw = unknown; request.seed.detailAvailable = false;
  cache.request(request);
  auto changedPlans = request;
  changedPlans.metadata.sacredStarPlans = {{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),12}}}};
  ok &= check(cache.request(changedPlans).status == PetDerivationStatus::InvalidRequest,
              "one frozen metadata key accepted different sacred star plans");
  changedPlans.metadata.sacredStarPlans = {};
  changedPlans.metadata.sacredStagePlans = {{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),9}}}};
  ok &= check(cache.request(changedPlans).status == PetDerivationStatus::InvalidRequest,
              "one frozen metadata key accepted different sacred stage plans");
  changedPlans.metadata.sacredStagePlans = {};
  changedPlans.metadata.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}}};
  ok &= check(cache.request(changedPlans).status == PetDerivationStatus::InvalidRequest,
              "one frozen metadata key accepted different badge upgrade definitions");
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }), "summary-only unknown fact was not queued");
  compute.runOne();
  ok &= check(waitUntil([&] { return bool(ready); }) && !ready->facts.asset.detailAvailable && !ready->facts.asset.currentPowerKnown,
              "summary-only handle dropped the member or fabricated full detail");
  const auto oldKey = ready->key;
  cache.invalidateMetadata(2, QByteArray(32, 'm'));
  ok &= check(!cache.lookup(oldKey) && cache.stats().retainedFactsBytes > 0,
              "metadata invalidation served an old key or forgot external old-version leases");
  request.metadataRevision = 2; request.metadataDigest = QByteArray(32, 'm');
  auto versioned = cache.request(request);
  ok &= check(versioned.accepted && waitUntil([&] { return compute.queued.size() == 1; }), "new metadata could not rebuild facts");
  compute.runOne();
  ok &= check(waitUntil([&] { return ready && ready->key.metadataRevision == 2; }), "old metadata result replaced the new one");
  return ok;
}
QByteArray fileBytes(const QString& path) {
  QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool finishRequest(PetDerivationCache& cache, ExistingCompute& compute, const PetDerivationRequest& request,
                    PetDerivedFactsHandle* value) {
  *value = {};
  quint64 expected = 0;
  bool failed = false;
  const auto ready = QObject::connect(&cache, &PetDerivationCache::ready, &cache,
      [&](quint64 id, const PetDerivationKey&, const PetDerivedFactsHandle& result) { if (id == expected) *value = result; });
  const auto failure = QObject::connect(&cache, &PetDerivationCache::failed, &cache,
      [&](quint64 id, const PetDerivationKey&, PetDerivationStatus, const QString& error) {
    if (id == expected) { failed = true; std::fprintf(stderr, "derivation failure: %s\n", qPrintable(error)); }
  });
  const auto result = cache.request(request); expected = result.taskId;
  if (result.status == PetDerivationStatus::CacheHit) *value = result.facts;
  const bool completed = result.accepted && waitUntil([&] {
    if (!compute.queued.empty()) compute.runOne();
    return bool(*value) || failed;
  }, 5000);
  QObject::disconnect(ready); QObject::disconnect(failure);
  return completed && !failed && bool(*value);
}

// A list brief carries `ri` while a detail carries `r`; both are the same race.
// The cultivation facts of a record must be reused when the alias spelling
// changes, instead of rejecting the pet as "one derivation key was reused with
// different calculation seed" and stranding every later analysis of it.
bool raceAliasSeedEquivalence() {
  bool ok = true;
  ExistingCompute compute;
  PetDerivationCache cache(compute.executor(), nullptr);
  cache.bindSession(QStringLiteral("A"), 1);
  auto detailShaped = requestFor(501);
  auto listShaped = detailShaped;
  QJsonObject identity = detailShaped.seed.pet;
  identity.remove(QStringLiteral("r"));
  identity.insert(QStringLiteral("ri"), 7001);
  listShaped.seed.pet = identity;
  ok &= check(listShaped.seed.pet == AssetDerivation::identityFields(listShaped.seed.pet),
              "alias fixture identity is not a frozen identity projection");
  PetDerivedFactsHandle facts;
  ok &= check(finishRequest(cache, compute, listShaped, &facts) && bool(facts),
              "alias fixture facts were not derived");
  const auto reused = cache.request(detailShaped);
  ok &= check(reused.accepted && reused.status == PetDerivationStatus::CacheHit && reused.facts,
              "a race alias spelling change was reported as a different calculation seed");
  // A genuinely different calculation input must still be rejected.
  auto changed = detailShaped;
  QJsonObject other = changed.seed.pet;
  other.insert(QStringLiteral("lv"), 101);
  changed.seed.pet = other;
  const auto rejected = cache.request(changed);
  ok &= check(!rejected.accepted && rejected.status == PetDerivationStatus::InvalidRequest,
              "a changed level was accepted as the same calculation seed");
  return ok;
}
PetDerivationRequest durable(PetDerivationRequest request, const StorageContext& context) {
  auto raw = std::make_shared<RawPetRecord>(*request.raw);
  raw->persisted = true;
  raw->contentDigest = QCryptographicHash::hash(QJsonDocument(raw->object()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  request.raw = raw; request.storageContext = context;
  request.metadataDigest = QCryptographicHash::hash(QByteArray("synthetic-stargods-empty;slot-level-6"), QCryptographicHash::Sha256);
  return request;
}
bool persistentIndexes() {
  bool ok = true;
  QTemporaryDir root;
  StorageService storage(root.path());
  const auto context = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  ExistingCompute compute;
  auto input = durable(requestFor(501, 256), context);
  const QString path = QDir(context->directory()).filePath(QStringLiteral("derived/pets/501.json"));
  {
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, input, &facts), "first durable raw derivation did not finish");
    ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 0 && QFileInfo::exists(path); }),
                "first valid compact index was not atomically saved");
    const auto envelope = QJsonDocument::fromJson(fileBytes(path)).object();
    ok &= check(cache.stats().computations == 1 && cache.stats().indexWrites == 1 &&
                    envelope.value(QStringLiteral("sourceDigest")).toString() == QString::fromLatin1(input.raw->contentDigest.toHex()) &&
                    !fileBytes(path).contains("sgsp") && !fileBytes(path).contains("relations"),
                "index lacked its exact source binding or retained raw relation/backpack JSON");
  }
  {
    auto restored = input;
    auto raw = std::make_shared<RawPetRecord>(*input.raw); raw->key.epoch = 2; raw->key.detailMemoryRevision = 9;
    restored.raw = raw; restored.metadataRevision = 5; // process-local versions may differ; content binding may not
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 2);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, restored, &facts) && cache.stats().indexHits == 1 && cache.stats().computations == 0 &&
                    facts->key.record == raw->key && facts->key.metadataRevision == 5 && cache.stats().indexWrites == 0,
                "matching persistent index recomputed raw or restored its old epoch/memory version");
    ok &= check(cache.stats().completedIndexDecodeMicroseconds > 0 && cache.stats().completedRawDeriveMicroseconds == 0 &&
                    cache.stats().completedComputeMicroseconds >= cache.stats().completedIndexDecodeMicroseconds,
                "persistent index decode was counted as raw derivation or lacked its execution subspan");
  }
  {
    auto corrupted = QJsonDocument::fromJson(fileBytes(path)).object();
    corrupted.insert(QStringLiteral("factsDigest"), QString(64, QLatin1Char('0')));
    QFile file(path); file.open(QIODevice::WriteOnly | QIODevice::Truncate); file.write(QJsonDocument(corrupted).toJson()); file.close();
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, input, &facts) && cache.stats().indexRejected == 1 && cache.stats().computations == 1,
                "corrupt disposable index became facts instead of rebuilding on Compute");
    ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 0; }), "corrupt-index rebuild did not finish its replacement");
  }
  const QByteArray validBytes = fileBytes(path);
  {
    auto overlay = input;
    auto raw = std::make_shared<RawPetRecord>(*input.raw); raw->key.epoch = 3; raw->key.detailMemoryRevision = 10;
    raw->rawProjectionOnly = false; raw->brief.insert(QStringLiteral("lv"), 101);
    overlay.raw = raw; overlay.seed.pet.insert(QStringLiteral("lv"), 101);
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 3);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, overlay, &facts) && facts->facts.asset.pet.value(QStringLiteral("lv")).toInt() == 101 &&
                    cache.stats().indexHits == 0 && cache.stats().computations == 1 && cache.stats().pendingIndexTasks == 0 && fileBytes(path) == validBytes,
                "brief override borrowed an index bound only to raw file bytes or rewrote it as raw-only");
  }
  {
    auto untrusted = input;
    auto raw = std::make_shared<RawPetRecord>(*input.raw); raw->key.epoch = 4; raw->sourceKnown = false;
    untrusted.raw = raw;
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 4);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, untrusted, &facts) && !facts->facts.asset.observationVerified && cache.stats().indexHits == 0 &&
                    cache.stats().indexWrites == 0 && fileBytes(path) == validBytes,
                "unverified observation borrowed or wrote persistent fact authority");
  }
  {
    auto changed = input;
    changed.metadataDigest = QCryptographicHash::hash(QByteArray("different verified metadata content"), QCryptographicHash::Sha256);
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, changed, &facts) && cache.stats().indexRejected == 1 && cache.stats().computations == 1,
                "metadata digest change reused an incompatible index");
    ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 0; }), "new metadata index was not saved");
    ok &= check(QDir(QFileInfo(path).absolutePath()).entryList({QStringLiteral("*.json")}, QDir::Files).size() == 1,
                "index updates accumulated one file per revision instead of one current file per ID");
  }
  {
    auto pendingDigest = requestFor(502);
    pendingDigest.metadataDigest = input.metadataDigest; pendingDigest.storageContext = context;
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, pendingDigest, &facts) && cache.stats().indexWrites == 0,
                "missing durable digest was fabricated for an index write");
    auto completedDigest = durable(pendingDigest, context);
    const auto hit = cache.request(completedDigest);
    const QString nextPath = QDir(context->directory()).filePath(QStringLiteral("derived/pets/502.json"));
    ok &= check(hit.status == PetDerivationStatus::CacheHit && hit.facts && cache.stats().computations == 1 &&
                    waitUntil([&] { return cache.stats().pendingIndexTasks == 0 && QFileInfo::exists(nextPath); }),
                "late same-memory-version source digest recomputed facts or failed to supplement its index");
  }
  ok &= check(storage.shutdown(), "persistent index IO did not drain");
  return ok;
}
bool repositoryIndexRoundTrip() {
  bool ok = true;
  QTemporaryDir root;
  StorageService storage(root.path());
  ExistingCompute compute;
  const QString account = QStringLiteral("warehouse-index");
  const auto login = [&](PetRepository& repo) {
    deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), account}}}});
    return waitForRepositoryIdle(&repo, 5000);
  };
  const auto make = [&](PetRepository& repo, RawPetRecordHandle raw) {
    PetDerivationRequest request;
    request.raw = raw; request.metadataRevision = 1;
    request.metadataDigest = QCryptographicHash::hash(QByteArray("real-shape test empty metadata"), QCryptographicHash::Sha256);
    request.storageContext = repo.storageContext();
    request.seed.instanceId = raw->key.instanceId; request.seed.raceId = petRaceId(raw->brief);
    request.seed.name = QStringLiteral("warehouse-shape"); request.seed.location = QStringLiteral("普通仓库");
    request.seed.detailAvailable = raw->complete; request.seed.pet = AssetDerivation::identityFields(raw->brief);
    return request;
  };
  {
    PetRepository repo(nullptr, &storage, root.filePath(QStringLiteral("legacy")));
    ok &= check(login(repo), "real-shape index login did not settle");
    repo.expectListPart(QStringLiteral("2_1_S"), 1, repo.accountKey(), repo.sessionGeneration());
    deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
        {QStringLiteral("ns"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("601")},
            {QStringLiteral("ri"), 990601}, {QStringLiteral("n"), QStringLiteral("warehouse-shape")}, {QStringLiteral("lv"), 100}}}},
        {QStringLiteral("rb"), QJsonArray{}}, {QStringLiteral("es"), QJsonArray{}}});
    ok &= check(waitForRepositoryIdle(&repo), "real-shape roster did not settle");
    QJsonObject detail = requestFor(601, 128).raw->object();
    detail.insert(QStringLiteral("r"), 990601); detail.insert(QStringLiteral("n"), QStringLiteral("warehouse-shape"));
    repo.expectDetail(601, 2, repo.accountKey(), repo.sessionGeneration());
    deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")}, {QStringLiteral("p"), detail}});
    ok &= check(waitForRepositoryIdle(&repo), "real-shape detail did not finish durable write");
    const auto raw = repo.rawRecordHandle(601);
    ok &= check(raw && raw->complete && raw->persisted && raw->sourceKnown && raw->contentDigest.size() == 32 && raw->rawProjectionOnly &&
                    raw->brief.contains(QStringLiteral("ri")) && raw->object().contains(QStringLiteral("r")),
                "real warehouse ri/raw r alias was not eligible for a correctly bound index");
    if (!raw) return false;
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(repo.accountKey(), repo.sessionGeneration());
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, make(repo, raw), &facts) && repo.markRecordDerived(raw->key),
                "real Repo handle did not derive compact facts");
    const QString path = QDir(repo.storageContext()->directory()).filePath(QStringLiteral("derived/pets/601.json"));
    ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 0 && QFileInfo::exists(path); }),
                "real Repo handle failed to write its compact index");
  }
  {
    PetRepository repo(nullptr, &storage, root.filePath(QStringLiteral("legacy")));
    ok &= check(login(repo), "real Repo restart did not reload inventory and raw files");
    auto raw = repo.rawRecordHandle(601);
    if (!raw) { repo.requestCachedDetail(601); waitForRepositoryIdle(&repo); raw = repo.rawRecordHandle(601); }
    ok &= check(raw && raw->complete && raw->persisted && raw->rawProjectionOnly, "real Repo disk reload lost the index input contract");
    if (!raw) return false;
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(repo.accountKey(), repo.sessionGeneration());
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, make(repo, raw), &facts) && cache.stats().indexHits == 1 && cache.stats().computations == 0,
                "real Repo disk restart never reused the valid compact index");
    ok &= check(repo.rawCacheStats().untrackedExports == 0, "index path exported untracked full JSON from the repository");
  }
  ok &= check(storage.shutdown(), "real Repo index storage did not drain");
  return ok;
}
bool indexReadEpochAndWriteFailure() {
  bool ok = true;
  {
    QTemporaryDir root;
    StorageService storage(root.path());
    const auto aContext = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
    const auto bContext = storage.createAccountContext(QStringLiteral("B"), QStringLiteral("B"));
    QSemaphore entered, release;
    ok &= check(storage.postAuxiliary([&](QObject*) { entered.release(); release.acquire(); }) && entered.tryAcquire(1, 2000),
                "index epoch fixture did not hold IO");
    ExistingCompute compute;
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
    int publishedA = 0, publishedB = 0;
    QObject::connect(&cache, &PetDerivationCache::ready, &cache,
        [&](quint64, const PetDerivationKey& key, const PetDerivedFactsHandle&) { key.record.account == QStringLiteral("A") ? ++publishedA : ++publishedB; });
    auto old = durable(requestFor(801), aContext);
    std::weak_ptr<const RawPetRecordPayload> oldPayload = old.raw->payload;
    cache.request(old); old.raw.reset();
    ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 1; }), "index read was not queued on existing IO");
    cache.bindSession(QStringLiteral("B"), 2);
    auto next = durable(requestFor(802, 1, QStringLiteral("B"), 2), bContext);
    cache.request(next); next.raw.reset();
    release.release();
    ok &= check(waitUntil([&] { return !compute.queued.empty(); }) && publishedA == 0 && oldPayload.expired(),
                "old-epoch index callback published or kept its cancelled raw input");
    compute.runOne();
    ok &= check(waitUntil([&] { return publishedB == 1 && cache.stats().pendingIndexTasks == 0; }) &&
                    !QFileInfo::exists(QDir(aContext->directory()).filePath(QStringLiteral("derived/pets/801.json"))),
                "new epoch did not resume or cancelled old index was written");
    cache.shutdown(); ok &= check(storage.shutdown(), "epoch index IO did not drain");
  }
  {
    QTemporaryDir root;
    StorageService storage(root.path(), {}, [](const QString&, const QByteArray&) {
      return StorageWriteAttempt{false, 0, QStringLiteral("synthetic index write failure")};
    });
    const auto context = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
    ExistingCompute compute;
    PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
    auto input = durable(requestFor(901), context);
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, input, &facts) &&
                    waitUntil([&] { return cache.stats().pendingIndexTasks == 0; }) && cache.stats().indexWriteFailures == 1 &&
                    cache.lookup(PetDerivationCache::keyFor(input)) &&
                    !QFileInfo::exists(QDir(context->directory()).filePath(QStringLiteral("derived/pets/901.json"))),
                "optional index failure was reported as Saved or erased valid in-memory facts");
    cache.shutdown(); ok &= check(storage.shutdown(), "failed index IO did not drain");
  }
  return ok;
}
bool retainedBudgetAndExecutorGuard() {
  bool ok = true;
  ExistingCompute compute;
  PetDerivationCacheLimits limits;
  limits.retainedBytes = 32 * 1024; limits.residentBytes = 16 * 1024; limits.maximumFactBytes = 16 * 1024; limits.residentEntries = 1;
  PetDerivationCache cache(compute.executor(), nullptr, limits); cache.bindSession(QStringLiteral("A"), 1);
  QList<PetDerivedFactsHandle> held;
  quint64 finished = 0;
  PetDerivationStatus outcome = PetDerivationStatus::Queued;
  QObject::connect(&cache, &PetDerivationCache::ready, &cache,
      [&](quint64 id, const PetDerivationKey&, const PetDerivedFactsHandle& value) { finished = id; outcome = PetDerivationStatus::CacheHit; held.append(value); });
  QObject::connect(&cache, &PetDerivationCache::failed, &cache,
      [&](quint64 id, const PetDerivationKey&, PetDerivationStatus status, const QString&) { finished = id; outcome = status; });
  PetDerivationRequest last;
  for (int item = 0; item < 16; ++item) {
    last = requestFor(1001 + item);
    const auto submitted = cache.request(last);
    ok &= check(submitted.accepted && waitUntil([&] {
      if (!compute.queued.empty()) compute.runOne(); return finished == submitted.taskId;
    }), "retained-budget fixture did not finish");
    if (outcome == PetDerivationStatus::BudgetExceeded) break;
  }
  ok &= check(outcome == PetDerivationStatus::BudgetExceeded && !held.isEmpty() && cache.stats().retainedFactsBytes > 0 &&
                  cache.stats().residentEntries == 0 && cache.stats().peakRetainedBytes <= limits.retainedBytes,
              "LRU removal bypassed the hard retained+reserved fact ceiling");
  held.clear();
  ok &= check(waitUntil([&] { return cache.stats().retainedFactsBytes == 0; }), "external facts did not release the exhausted budget");
  PetDerivedFactsHandle recovered;
  ok &= check(finishRequest(cache, compute, last, &recovered), "freeing external leases did not restore real admission room");
  {
    PetDerivationCache wrongThread([](std::function<void()> work) { work(); return true; });
    wrongThread.bindSession(QStringLiteral("A"), 1);
    bool rejected = false;
    QObject::connect(&wrongThread, &PetDerivationCache::failed, &wrongThread,
        [&](quint64, const PetDerivationKey&, PetDerivationStatus status, const QString&) { rejected = status == PetDerivationStatus::ComputeUnavailable; });
    wrongThread.request(requestFor(2001));
    ok &= check(waitUntil([&] { return rejected; }) && wrongThread.stats().computations == 0,
                "misconfigured executor performed raw derivation on Core");
  }
  return ok;
}
bool optionalIndexThrottle() {
  bool ok = true;
  QTemporaryDir root;
  QSemaphore entered, release;
  std::atomic_bool hold{true};
  StorageService storage(root.path(), {}, [&](const QString& path, const QByteArray& bytes) {
    if (path.contains(QStringLiteral("/derived/")) && hold.exchange(false)) { entered.release(); release.acquire(); }
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
    const qint64 written = file.write(bytes);
    return StorageWriteAttempt{written == bytes.size() && file.commit(), written, file.errorString()};
  });
  const auto context = storage.createAccountContext(QStringLiteral("A"), QStringLiteral("A"));
  ExistingCompute compute;
  PetDerivationCache cache(compute.executor(), &storage); cache.bindSession(QStringLiteral("A"), 1);
  QList<PetDerivationRequest> requests;
  for (int id = 3001; id <= 3005; ++id) {
    auto request = requestFor(id);
    request.metadataDigest = QCryptographicHash::hash(QByteArray("synthetic-stargods-empty;slot-level-6"), QCryptographicHash::Sha256);
    request.storageContext = context;
    PetDerivedFactsHandle facts;
    ok &= check(finishRequest(cache, compute, request, &facts), "index-throttle RAM preparation failed");
    requests.append(request);
  }
  for (const auto& request : requests) ok &= check(cache.request(durable(request, context)).status == PetDerivationStatus::CacheHit,
      "late digest did not enqueue an optional index intent");
  ok &= check(waitUntil([&] { return entered.available() > 0; }) && storage.state().outstandingTasks == 2 &&
                  cache.stats().pendingIndexTasks == 5,
              "optional indexes consumed more than two shared IO slots while their writer was blocked");
  const auto required = storage.submitJsonWrite({context, QStringLiteral("analysis/user-snapshot.json"), nextTransportTaskId(),
      {{QStringLiteral("account"), QStringLiteral("A")}, {QStringLiteral("required"), true}}, 1024, true});
  ok &= check(required.accepted, "optional indexes starved a necessary snapshot admission");
  release.release();
  ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 0 && storage.state().outstandingTasks == 0; }, 5000) &&
                  QFileInfo::exists(QDir(context->directory()).filePath(QStringLiteral("analysis/user-snapshot.json"))),
              "throttled indexes or the necessary snapshot did not complete");
  cache.shutdown(); ok &= check(storage.shutdown(), "throttled index storage did not drain");
  return ok;
}
}

bool delayedExecutorReleaseKeepsQueueAlive() {
  bool ok = true; ExistingCompute compute; std::function<void()> heldDispatch; int attempts = 0, ready = 0;
  PetDerivationCache cache([&](std::function<void()> work) {
    ++attempts;
    if (attempts == 1) { heldDispatch = std::move(work); return true; }
    if (attempts == 2) return false; // The shared executor is briefly occupied.
    compute.queued.push_back(std::move(work)); return true;
  });
  cache.bindSession(QStringLiteral("A"),1);
  QObject::connect(&cache,&PetDerivationCache::ready,&cache,[&](quint64,const PetDerivationKey&,const PetDerivedFactsHandle&) { ++ready; });
  cache.request(requestFor(951)); cache.request(requestFor(952));
  ok &= check(waitUntil([&] { return bool(heldDispatch); }),"deferred executor token fixture did not dispatch");
  QMetaObject::invokeMethod(compute.thread.worker,[&] { heldDispatch(); },Qt::BlockingQueuedConnection);
  ok &= check(waitUntil([&] { return ready == 1; }),"Compute completion did not reach Core while its dispatch token was retained");
  QCoreApplication::processEvents(QEventLoop::AllEvents,5);
  ok &= check(attempts == 1 && cache.stats().pendingTasks == 1 && cache.stats().activeTasks == 1,
      "a second task overlapped an executor dispatch that had not unwound");
  heldDispatch = {};
  ok &= check(waitUntil([&] { return compute.queued.size() == 1; }) && attempts == 3,
      "executor token release or one rejected dispatch lost the queued task's wakeup");
  compute.runOne();
  ok &= check(waitUntil([&] { return ready == 2 && cache.stats().pendingTasks == 0; }),"queued task became orphaned after deferred token release");
  return ok;
}
bool offlineLastAccountFacts() {
  bool ok = true; QTemporaryDir root; const QString account = QStringLiteral("offline-local");
  const QString accountPath = root.filePath(QStringLiteral("accounts/") + account);
  QDir().mkpath(QDir(accountPath).filePath(QStringLiteral("details")));
  const auto write = [](const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); };
  const auto original = requestFor(941,8,account,0);
  const QJsonObject inventory{{QStringLiteral("schema"),3},{QStringLiteral("account"),account},
      {QStringLiteral("backpack"),QJsonArray{}},{QStringLiteral("warehouse"),QJsonArray{original.raw->brief}}};
  const QJsonObject detail{{QStringLiteral("schema"),3},{QStringLiteral("account"),account},{QStringLiteral("instanceId"),QStringLiteral("941")},
      {QStringLiteral("complete"),true},{QStringLiteral("pet"),original.raw->object()}};
  ok &= check(write(root.filePath(QStringLiteral("last-account.txt")),account.toUtf8()) &&
      write(QDir(accountPath).filePath(QStringLiteral("inventory.json")),QJsonDocument(inventory).toJson(QJsonDocument::Compact)) &&
      write(QDir(accountPath).filePath(QStringLiteral("details/941.json")),QJsonDocument(detail).toJson(QJsonDocument::Compact)),"offline last-account fixture could not be written");
  StorageService storage(root.path()); int saves = 0;
  QObject::connect(&storage,&StorageService::completed,&storage,[&](const StorageResult& result) { saves += result.status == StorageStatus::Saved; });
  PetRepository repo(nullptr,&storage,root.filePath(QStringLiteral("legacy")));
  ok &= check(waitForRepositoryIdle(&repo) && repo.accountKey() == account && repo.sessionGeneration() == 0 && !repo.isAuthenticated() && !repo.sessionContext().canPersist(),"last-account did not create the actual read-only epoch-zero view");
  auto raw = repo.rawRecordHandle(941); if (!check(bool(raw),"offline current raw detail was not loaded")) return false;
  ExistingCompute compute; PetDerivationCache cache(compute.executor(),&storage); cache.bindSession(account,0);
  auto request = original; request.raw = raw; request.seed.pet = AssetDerivation::identityFields(raw->brief);
  request.metadataDigest = QByteArray(32,'m'); request.storageContext = repo.storageContext();
  PetDerivedFactsHandle facts;
  ok &= check(finishRequest(cache,compute,request,&facts) && facts->key.record.epoch == 0 && repo.markRecordDerived(raw->key),"valid offline raw was rejected solely because its epoch is zero");
  ok &= check(waitUntil([&] { return cache.stats().pendingIndexTasks == 0; }) && saves == 0 && cache.stats().indexWrites == 0 &&
      !QFileInfo::exists(QDir(accountPath).filePath(QStringLiteral("derived/pets/941.json"))) && !repo.sessionContext().canPersist(),"read-only offline computation wrote an index or changed persistence authority");
  if (!facts) return false;
  int oldPublished = 0, newPublished = 0;
  QObject::connect(&cache,&PetDerivationCache::ready,&cache,[&](quint64,const PetDerivationKey& key,const PetDerivedFactsHandle&) { key.record.epoch == 0 ? ++oldPublished : ++newPublished; });
  auto waiting = requestFor(942,8,account,0); waiting.metadataDigest = request.metadataDigest;
  cache.request(waiting); ok &= check(waitUntil([&] { return compute.queued.size() == 1; }),"offline held computation was not queued");
  cache.bindSession(account,1); auto online = requestFor(942,8,account,1); online.metadataDigest = request.metadataDigest; cache.request(online);
  compute.runOne(); ok &= check(waitUntil([&] { return compute.queued.size() == 1; }),"new epoch did not resume after offline cancellation");
  compute.runOne(); ok &= check(waitUntil([&] { return newPublished == 1; }) && oldPublished == 0,"offline callback crossed the epoch-zero to login boundary");
  return ok;
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  bool ok = raceAliasSeedEquivalence();
  ok &= equivalenceAndLeases();
  ok &= cancellationAndEpochs();
  ok &= overloadAndIdentity();
  ok &= unknownAndMetadata();
  ok &= persistentIndexes();
  ok &= repositoryIndexRoundTrip();
  ok &= indexReadEpochAndWriteFailure();
  ok &= retainedBudgetAndExecutorGuard();
  ok &= optionalIndexThrottle();
  ok &= offlineLastAccountFacts();
  ok &= delayedExecutorReleaseKeepsQueueAlive();
  if (ok) std::puts("PASS: compact fact reuse, bounded single-Compute scheduling, external leases, cancellation, epoch and metadata identity");
  return ok ? 0 : 1;
}