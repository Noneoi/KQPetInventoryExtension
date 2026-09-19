#include "support/quota_test_support.h"
#include "application/analysis/analysis_worker.h"
#include "domain/asset_derivation.h"
#include "domain/pet_analysis_facts.h"
#include <QJsonDocument>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <stdexcept>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMilliseconds = 5000) {
  if (predicate()) return true;
  QEventLoop loop;
  QTimer poll, deadline;
  poll.setInterval(1);
  deadline.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (predicate()) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  deadline.start(timeoutMilliseconds);
  loop.exec();
  return predicate();
}

AnalysisJobKey key(quint64 id, const QString& account = QStringLiteral("account-a"),
                   quint64 epoch = 1) {
  AnalysisJobKey result;
  result.jobId = id;
  result.account = account;
  result.epoch = epoch;
  result.versions.inventory = 1;
  result.versions.details = 2;
  result.versions.catalog = 3;
  result.versions.resources = 4;
  result.versions.limits = 5;
  return result;
}

PreparedShopConditions conditions(int goodCount) {
  QList<ShopExchangeGood> goods;
  QJsonObject items;
  for (int index = 0; index < goodCount; ++index) {
    ShopExchangeGood good;
    good.shopId = 1;
    good.itemServerId = index + 1;
    good.limitKey = QStringLiteral("dl");
    good.limitCount = 3;
    good.cost = QStringLiteral("4:100:20");
    good.enhanceType = QStringLiteral("41");
    good.raceIds = {7000};
    good.description = QStringLiteral("project %1").arg(index);
    goods.append(good);
    items.insert(good.itemKey(), QJsonObject{{QStringLiteral("dl"), 0}});
  }
  return prepareWithSyntheticPeriod(CompiledShopCatalog::compile(goods),
      QJsonObject{{QStringLiteral("si1"), items}},
      AccountResourceView({{QStringLiteral("4:100"), 1000}}, true));
}

std::shared_ptr<const AnalysisWorkInput> input(
    const AnalysisJobKey& identity, const PreparedShopConditions& prepared,
    int petCount = 30, qsizetype rawBytes = 0) {
  auto snapshot = std::make_shared<AnalysisWorkInput>();
  snapshot->key = identity;
  snapshot->conditions = prepared;
  snapshot->overview.account = identity.account;
  snapshot->overview.inputSessionEpoch = identity.epoch;
  snapshot->overview.inventoryRevision = identity.versions.inventory;
  snapshot->metadata.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}}};
  for (int index = 0; index < petCount; ++index) {
    PetAssetRecord pet;
    pet.instanceId = 10000 + index;
    pet.raceId = 7000;
    pet.name = QStringLiteral("pet %1").arg(index);
    pet.detailAvailable = true;
    pet.improvable = true;
    pet.completionKnown = true;
    pet.powerGapKnown = true;
    pet.completionPercent = 80;
    pet.currentPower = 8000;
    pet.highestPower = 10000;
    pet.pet = {{QStringLiteral("id"), pet.instanceId}, {QStringLiteral("r"), 7000},
        {QStringLiteral("badge"),QStringLiteral("101:1")},
        {QStringLiteral("czdlv"), QJsonObject{{QStringLiteral("bsv"), 10}}},
        {QStringLiteral("mzdlv"), QJsonObject{{QStringLiteral("bsv"), 100}}}};
    if (index == 0) {
      QJsonValue nested = QJsonArray{1, 2, QStringLiteral("retained nested JSON")};
      for (int depth = 0; depth < 30; ++depth)
        nested = QJsonObject{{QStringLiteral("nested"), nested}};
      pet.pet.insert(QStringLiteral("nestedFixture"), nested);
    }
    snapshot->overview.pets.append(pet);
  }
  if (rawBytes > 0) snapshot->retainedRawPayloads.append(QByteArray(rawBytes, 'x'));
  return snapshot;
}

QStringList identities(const QList<ActionRecommendation>& rows) {
  QStringList result;
  for (const auto& row : rows) result.append(row.stableId);
  return result;
}

AnalysisMemoryLimits limits() {
  AnalysisMemoryLimits result;
  result.inputBytes = 32ULL * 1024 * 1024;
  result.resultBytes = 8ULL * 1024 * 1024;
  result.candidateBytes = 1024 * 1024;
  result.totalBytes = 64ULL * 1024 * 1024;
  result.sliceWorkUnits = 64;
  return result;
}

ShopPetMetadataSnapshot factsMetadata() {
  ShopPetMetadataSnapshot metadata;
  metadata.sacredStarPlans = {{QStringLiteral("2"),QJsonObject{{QStringLiteral("maxLevel"),10}}}};
  metadata.sacredStagePlans = {{QStringLiteral("6"),QJsonObject{{QStringLiteral("maxLevel"),7}}}};
  metadata.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}}};
  for (int id : {80,2001,2002,2003}) metadata.stargods.insert(QString::number(id),
      QJsonObject{{QStringLiteral("quality"),id == 80 || id == 2002 ? 6 : 5},
          {QStringLiteral("changeable"),false},{QStringLiteral("battlePower"),
          QJsonObject{{QStringLiteral("8"),id == 80 ? 400 : id == 2002 ? 300 : 200}}}});
  return metadata;
}
PetAssetRecord factSeed(qint64 id, bool available = true, int inventorySize = 2) {
  PetAssetRecord seed;
  seed.instanceId = id; seed.raceId = 7000; seed.metadataSlotMaxLevel = 8;
  seed.name = QStringLiteral("compact-%1").arg(id); seed.location = QStringLiteral("普通仓库");
  seed.detailAvailable = available; seed.observationVerified = true;
  QJsonArray inventory;
  for (int index = 0; index < inventorySize; ++index) inventory.append(index % 2 ? 2002 : 2003);
  seed.pet = {{QStringLiteral("id"),QString::number(id)},{QStringLiteral("r"),7000},
      {QStringLiteral("n"),seed.name},{QStringLiteral("_metaRaceId"),7001},{QStringLiteral("lv"),120},
      {QStringLiteral("zdl"),1320},{QStringLiteral("xzdl"),3200},
      {QStringLiteral("czdlv"),QJsonObject{{QStringLiteral("lv"),1000},{QStringLiteral("sgv"),100},
          {QStringLiteral("bsv"),30},{QStringLiteral("iv"),100},{QStringLiteral("asv"),50},{QStringLiteral("sjv"),40}}},
      {QStringLiteral("mzdlv"),QJsonObject{{QStringLiteral("lv"),2000},{QStringLiteral("sgv"),700},
          {QStringLiteral("bsv"),100},{QStringLiteral("iv"),200},{QStringLiteral("asv"),100},{QStringLiteral("sjv"),100}}},
      {QStringLiteral("sgs"),QStringLiteral("2001:8#0:8#0:8")},{QStringLiteral("sgsp"),inventory},
      {QStringLiteral("stargodSlotMaxLevel"),8},{QStringLiteral("astrolabebr"),false},
      {QStringLiteral("badge"),QStringLiteral("101:5#201:0")},{QStringLiteral("shenjue"),QStringLiteral("1034#2#6|1:1")},
      {QStringLiteral("sppl"),QJsonObject{{QStringLiteral("id"),99001},{QStringLiteral("r"),7001}}}};
  return seed;
}
bool equivalentRows(const QList<ActionRecommendation>& a, const QList<ActionRecommendation>& b) {
  if (a.size() != b.size()) return false;
  for (int index = 0; index < a.size(); ++index) {
    const auto& x = a[index]; const auto& y = b[index];
    if (x.stableId != y.stableId || x.type != y.type || x.petInstanceId != y.petInstanceId ||
        x.currentPower != y.currentPower || x.highestPower != y.highestPower || x.completionKnown != y.completionKnown ||
        x.completionPercent != y.completionPercent || x.powerGapKnown != y.powerGapKnown ||
        x.unknownConditionCount != y.unknownConditionCount || x.supportedGapCount != y.supportedGapCount ||
        x.alternateGoodCount != y.alternateGoodCount || x.remainingExchangeCount != y.remainingExchangeCount ||
        x.actionableCountKnown != y.actionableCountKnown || x.actionableCount != y.actionableCount ||
        x.closesKnownGap != y.closesKnownGap || x.gaps != y.gaps || x.requirements.size() != y.requirements.size()) return false;
    for (int item = 0; item < x.requirements.size(); ++item)
      if (x.requirements[item].resourceKey != y.requirements[item].resourceKey ||
          x.requirements[item].required != y.requirements[item].required || x.requirements[item].owned != y.requirements[item].owned ||
          x.requirements[item].condition.effectiveState() != y.requirements[item].condition.effectiveState()) return false;
  }
  return true;
}
bool equivalentOverview(const AccountAssetOverview& a, const AccountAssetOverview& b) {
  if (a.pets.size() != b.pets.size() || a.totalPets != b.totalPets || a.missingDetailPets != b.missingDetailPets ||
      a.totalCurrentPower != b.totalCurrentPower || a.fullyCultivatedPets != b.fullyCultivatedPets ||
      a.improvablePets != b.improvablePets || a.shopImprovablePets != b.shopImprovablePets ||
      a.redStarMissingPets != b.redStarMissingPets || a.astrolabeMissingPets != b.astrolabeMissingPets ||
      a.sacredMissingPets != b.sacredMissingPets || a.soulMissingPets != b.soulMissingPets) return false;
  for (int index = 0; index < a.pets.size(); ++index) {
    const auto& x = a.pets[index]; const auto& y = b.pets[index];
    if (x.instanceId != y.instanceId || x.name != y.name || x.location != y.location ||
        x.currentPower != y.currentPower || x.highestPower != y.highestPower ||
        x.currentPowerKnown != y.currentPowerKnown || x.highestPowerKnown != y.highestPowerKnown ||
        x.completionKnown != y.completionKnown || x.completionPercent != y.completionPercent ||
        x.fullyCultivated != y.fullyCultivated || x.improvable != y.improvable ||
        x.shopImprovable != y.shopImprovable || x.redStarKnown != y.redStarKnown ||
        x.astrolabeKnown != y.astrolabeKnown || x.gaps != y.gaps || x.pet != y.pet) return false;
  }
  return true;
}
std::shared_ptr<AnalysisWorkInput> compactInput(const AnalysisJobKey& identity, const PreparedShopConditions& prepared,
                                             const PetAnalysisFacts& prototype, int count) {
  auto value = std::make_shared<AnalysisWorkInput>();
  value->key = identity; value->conditions = prepared; value->usePreparedFacts = true;
  value->overview.account = identity.account; value->overview.inputSessionEpoch = identity.epoch;
  value->overview.inventoryRevision = identity.versions.inventory; value->overview.totalPets = count;
  value->overview.sourceVerified = true;
  value->preparedFacts.reserve(count);
  for (int index = 0; index < count; ++index) {
    auto fact = prototype;
    fact.asset.instanceId = 100000 + index;
    fact.asset.pet.insert(QStringLiteral("id"),QString::number(fact.asset.instanceId));
    fact.asset.name = QStringLiteral("compact-%1").arg(index);
    fact.asset.pet.insert(QStringLiteral("n"),fact.asset.name);
    value->preparedFacts.append(std::move(fact));
  }
  return value;
}
struct CacheLeaseProbe {
  std::shared_ptr<std::atomic_int> destroyed;
  explicit CacheLeaseProbe(std::shared_ptr<std::atomic_int> value) : destroyed(std::move(value)) {}
  ~CacheLeaseProbe() { ++*destroyed; }
};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  bool ok = true;
  qint64 measuredCancellation = 0, measuredSlice = 0, measuredSort = 0;
  quint64 measuredPeakCharge = 0;
  const auto small = conditions(3);
  const auto large = conditions(1000);

  // A slice can stop inside one pet's candidate list, retaining only its best.
  const auto onePet = input(key(1), large, 1);
  AlgorithmPipelineStats slicedStats;
  RecommendationSession sliced(onePet->key.account, onePet->overview,
      onePet->conditions, onePet->metadata, &slicedStats);
  ok &= require(sliced.step(nullptr, 16, 1) == RecommendationSession::Status::Running &&
      slicedStats.petsDerived == 1 && slicedStats.candidatePairsVisited == 0,
      "first session slice did not stop after one pet derivation");
  ok &= require(sliced.step(nullptr, 16, 1) == RecommendationSession::Status::Running &&
      slicedStats.candidatePairsVisited == 1,
      "candidate traversal cannot yield within a single pet");
  std::atomic_bool cancelled{true};
  ok &= require(sliced.step(&cancelled, 16, 1) == RecommendationSession::Status::Cancelled &&
      slicedStats.candidatePairsVisited == 1 && sliced.takeResults().isEmpty(),
      "atomic cancellation waited for the whole pet or exposed partial results");

  const auto ordinary = input(key(2), small, 50);
  const auto synchronous = PreparedRecommendationEngine::generatePrepared(ordinary->key.account,
      ordinary->overview, ordinary->conditions, ordinary->metadata);
  RecommendationSession stepped(ordinary->key.account, ordinary->overview,
      ordinary->conditions, ordinary->metadata);
  while (stepped.step(nullptr, 16, 7) == RecommendationSession::Status::Running) {}
  ok &= require(identities(stepped.takeResults()) == identities(synchronous) &&
      stepped.sliceStats().slices > 1 && stepped.sliceStats().sortNanoseconds > 0 &&
      stepped.sliceStats().peakCandidateChargedBytes > 0,
      "resumable selection/sort changed synchronous results or omitted timing/charges");

  {
    AnalysisWorker worker(limits());
    QList<AnalysisJobFinished> finished;
    QList<quint64> published;
    int captures = 0;
    bool wrongThread = false, overlappingInput = false;
    std::weak_ptr<const AnalysisWorkInput> previous;
    worker.setPublicationGuard([&](const AnalysisJobKey&) {
      wrongThread |= QThread::currentThread() != application.thread();
      return true;
    });
    worker.setInputFactory([&](const AnalysisJobKey& identity) {
      wrongThread |= QThread::currentThread() != application.thread();
      overlappingInput |= !previous.expired();
      ++captures;
      auto next = input(identity, captures == 1 ? large : small,
                        captures == 1 ? 1000 : 30, 4096);
      previous = next;
      return next;
    });
    worker.setResultHandler([&](auto result) {
      wrongThread |= QThread::currentThread() != application.thread();
      published.append(result->generation);
    });
    worker.setFinishedHandler([&](const AnalysisJobFinished& result) {
      wrongThread |= QThread::currentThread() != application.thread();
      finished.append(result);
    });
    const quint64 first = worker.submit(key(10));
    ok &= require(first > 0 && waitUntil([&] {
      return captures == 1 && worker.memoryUsage().candidatePairsVisited >= 10000 &&
             worker.memoryUsage().computeEventLoopTurns > 0;
    }), "first Compute analysis never reached candidate work");
    quint64 latest = 0;
    for (quint64 id = 11; id < 61; ++id)
      latest = worker.submit(key(id, QStringLiteral("account-b"), 2));
    ok &= require(captures == 1 && worker.hasPendingDescriptor(),
                  "replacement requests captured multiple snapshots before old input release");
    ok &= require(waitUntil([&] { return published.size() == 1 && !worker.busy(); }),
                  "latest replacement analysis did not complete");
    const auto current = worker.lastResult();
    ok &= require(captures == 2 && !overlappingInput && !wrongThread &&
        current && current->generation == latest && current->key.account == QStringLiteral("account-b") &&
        current->key.epoch == 2 && published.first() == latest,
        "cross-account coalescing, Core callback affinity or release barrier failed");
    ok &= require(!finished.isEmpty() && finished.first().generation == first &&
        finished.first().outcome == AnalysisJobOutcome::Superseded &&
        finished.first().cancellationLatencyNanoseconds <= 100000000 &&
        finished.first().slices.slices > 0 && finished.first().inputMeasurementComplete,
        "old analysis cancellation exceeded 100 ms or was reported as current");
    if (!finished.isEmpty() && current) {
      measuredCancellation = finished.first().cancellationLatencyNanoseconds;
      measuredSlice = finished.first().slices.maximumSliceNanoseconds;
      measuredSort = current->slices.sortNanoseconds;
      measuredPeakCharge = worker.memoryUsage().peakChargedBytes;
    }
    ok &= require(current && current->retainedRawPayloadBytes == 4096 &&
        current->measuredInputBytes > current->retainedRawPayloadBytes &&
        current->maximumInputMeterSliceNanoseconds > 0 &&
        worker.memoryUsage().computeSlices > 1 &&
        worker.memoryUsage().peakInputChargedBytes == limits().inputBytes &&
        worker.memoryUsage().peakCandidateChargedBytes > 0,
        "raw JSON/payload input accounting or sliced metering was omitted");
    const auto beforeStale = current;
    worker.setPublicationGuard([](const AnalysisJobKey&) { return false; });
    worker.submit(key(70, QStringLiteral("account-b"), 2));
    ok &= require(waitUntil([&] { return finished.size() >= 3 && !worker.busy(); }) &&
        finished.last().outcome == AnalysisJobOutcome::Stale &&
        worker.lastResult() == beforeStale && published.size() == 1,
        "publication freshness guard failed to preserve the prior result");
    ok &= require(worker.shutdown(), "normal Compute shutdown did not join within 2 seconds");
  }

  // Same identity, epoch, revisions and jobId still require the current local
  // generation. Matching inputs cannot authorize an older computation.
  {
    AnalysisWorker worker(limits());
    int captures = 0;
    QList<quint64> published;
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const AnalysisJobKey& identity) {
      ++captures;
      return input(identity, captures == 1 ? large : small, captures == 1 ? 1000 : 20);
    });
    worker.setPublicationGuard([](const AnalysisJobKey&) { return true; });
    worker.setResultHandler([&](auto result) { published.append(result->generation); });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    const auto same = key(80);
    const quint64 oldGeneration = worker.submit(same);
    ok &= require(waitUntil([&] { return worker.memoryUsage().candidatePairsVisited >= 10000; }),
                  "same-key cancellation fixture did not begin candidate work");
    const quint64 newGeneration = worker.submit(same);
    ok &= require(waitUntil([&] { return published.size() == 1 && !worker.busy(); }) &&
        newGeneration != oldGeneration && published.first() == newGeneration &&
        captures == 2 && finished.first().outcome == AnalysisJobOutcome::Superseded,
        "identical input/jobId allowed an old generation to publish");
  }

  // Retained old results stay charged after a newer result is published.
  {
    auto policy = limits();
    policy.inputBytes = 2 * 1024 * 1024;
    policy.totalBytes = 8 * 1024 * 1024;
    AnalysisWorker worker(policy);
    int publications = 0;
    worker.setInputFactory([&](const AnalysisJobKey& identity) { return input(identity, small, 80); });
    worker.setPublicationGuard([](const AnalysisJobKey&) { return true; });
    worker.setResultHandler([&](auto) { ++publications; });
    worker.submit(key(90));
    ok &= require(waitUntil([&] { return publications == 1; }), "initial retained result missing");
    auto retainedOld = worker.lastResult();
    const quint64 oldBytes = worker.memoryUsage().resultChargedBytes;
    worker.submit(key(91));
    ok &= require(waitUntil([&] { return publications == 2; }), "replacement retained result missing");
    const auto overlapping = worker.memoryUsage();
    ok &= require(retainedOld && oldBytes > 0 &&
        overlapping.resultChargedBytes > oldBytes &&
        overlapping.peakPublicationOverlapBytes >= overlapping.resultChargedBytes &&
        overlapping.peakResultChargedBytes >= overlapping.resultChargedBytes,
        "old/new published result overlap escaped memory accounting");
    retainedOld.reset();
    ok &= require(worker.memoryUsage().resultChargedBytes < overlapping.resultChargedBytes,
                  "released old result did not return its memory charge");

    bool submittedInGuard = false;
    const quint64 previous = worker.lastResult()->generation;
    worker.setPublicationGuard([&](const AnalysisJobKey& identity) {
      if (!submittedInGuard) {
        submittedInGuard = true;
        worker.submit(identity);
      }
      return true;
    });
    worker.submit(key(92));
    ok &= require(waitUntil([&] { return publications == 3 && !worker.busy(); }) &&
        worker.lastResult()->generation >= previous + 2,
        "reentrant freshness guard let the superseded job publish");
    worker.setPublicationGuard([&](const AnalysisJobKey&) {
      worker.cancel();
      return true;
    });
    worker.submit(key(93));
    ok &= require(waitUntil([&] { return !worker.busy() && !worker.hasPendingDescriptor(); }) &&
        publications == 3,
        "reentrant cancellation during the publication guard was ignored");
  }

  // Mismatched captures, factory failures, unknown freshness and resource
  // budgets all fail closed without publishing partial results.
  for (int scenario = 0; scenario < 8; ++scenario) {
    auto policy = limits();
    if (scenario == 3) policy.inputBytes = 4096;
    if (scenario == 4) policy.resultBytes = 1;
    AnalysisWorker worker(policy);
    QList<AnalysisJobFinished> finished;
    int published = 0;
    worker.setInputFactory([&, scenario](const AnalysisJobKey& identity)
        -> std::shared_ptr<const AnalysisWorkInput> {
      if (scenario == 1) throw std::runtime_error("synthetic factory failure");
      auto captureKey = identity;
      if (scenario == 0) captureKey.account = QStringLiteral("different-account");
      if (scenario == 5) ++captureKey.epoch;
      if (scenario == 6) ++captureKey.versions.details;
      return input(captureKey, small, 10, scenario == 3 ? 8192 : 0);
    });
    if (scenario != 2)
      worker.setPublicationGuard([](const AnalysisJobKey&) { return true; });
    if (scenario == 7)
      worker.setPublicationGuard([](const AnalysisJobKey&) -> bool {
        throw std::runtime_error("synthetic guard failure");
      });
    worker.setResultHandler([&](auto) { ++published; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    worker.submit(key(100 + scenario));
    ok &= require(waitUntil([&] { return finished.size() == 1; }),
                  "invalid input/budget fixture did not finish");
    const AnalysisJobOutcome expected = (scenario == 0 || scenario == 5 || scenario == 6)
        ? AnalysisJobOutcome::InputRejected
        : scenario == 1 ? AnalysisJobOutcome::FactoryFailed
        : (scenario == 2 || scenario == 7) ? AnalysisJobOutcome::Stale
                                          : AnalysisJobOutcome::BudgetExceeded;
    ok &= require(published == 0 && !worker.lastResult() &&
        finished.first().outcome == expected &&
        worker.memoryUsage().inputChargedBytes == 0 &&
        worker.memoryUsage().resultChargedBytes == 0,
        "invalid input, absent guard or exceeded budget published/leaked data");
  }

  // A bounded shutdown may leave the thread alive briefly. Its worker owns
  // outstanding references and releases them without touching the old facade.
  {
    auto worker = std::make_unique<AnalysisWorker>(limits());
    int captures = 0;
    std::weak_ptr<const AnalysisWorkInput> retained;
    worker->setInputFactory([&](const AnalysisJobKey& identity) {
      ++captures;
      auto value = input(identity, large, 1000);
      retained = value;
      return value;
    });
    worker->submit(key(200));
    ok &= require(waitUntil([&] { return captures == 1 && worker->busy(); }),
                  "shutdown fixture did not capture an input");
    worker->submit(key(201));
    worker->shutdown(0);
    ok &= require(worker->submit(key(202)) == 0, "Closing accepted another request");
    worker.reset();
    ok &= require(waitUntil([&] { return retained.expired(); }) && captures == 1,
                  "shutdown captured pending input or failed to release the running snapshot");
  }

  {
    AnalysisWorker worker(limits());
    const auto coreThread = QThread::currentThreadId();
    std::atomic_bool firstDone{false};
    std::atomic_bool differentThread{false};
    ok &= require(worker.postPriority([&] {
      differentThread.store(QThread::currentThreadId() != coreThread);
      firstDone.store(true);
    }), "priority work was rejected before Compute initialization");
    ok &= require(!worker.postPriority([] {}), "priority queue exceeded its one-task bound");
    ok &= require(waitUntil([&] { return firstDone.load(); }) && differentThread.load(),
                  "priority work ran on Core or did not execute");
    std::atomic_bool secondDone{false};
    bool secondAccepted = false;
    ok &= require(waitUntil([&] {
      if (!secondAccepted) secondAccepted = worker.postPriority([&] { secondDone.store(true); });
      return secondAccepted;
    }),
                  "priority reservation was not released");
    ok &= require(waitUntil([&] { return secondDone.load(); }), "second priority work did not finish");
    worker.shutdown();
    ok &= require(!worker.postPriority([] {}), "Closing accepted priority work");
  }

  // Reusing compact facts must preserve the existing full-overview semantics,
  // including missing details, invalidated observations and fully grown pets.
  const auto metadata = factsMetadata();
  const auto prototype = derivePetAnalysisFacts(factSeed(1,true,7000),metadata);
  ok &= require(prototype && !prototype->asset.pet.contains(QStringLiteral("sgsp")) &&
      prototype->battlePower.backpackStars == 7000 && petAnalysisFactsRetainedBytes(*prototype) < 16384,
      "large raw cultivation input was not compacted into bounded facts");
  if (!prototype) return 1;
  {
    AccountAssetOverview raw; raw.account = QStringLiteral("equivalent-facts"); raw.sourceVerified = true;
    auto weak = factSeed(2); weak.observationVerified = false;
    auto full = factSeed(4);
    auto fullCurrent = full.pet.value(QStringLiteral("mzdlv")).toObject();
    fullCurrent.insert(QStringLiteral("lv"),2150); fullCurrent.insert(QStringLiteral("sgv"),1200);
    full.pet.insert(QStringLiteral("czdlv"),fullCurrent); full.pet.insert(QStringLiteral("zdl"),3850);
    full.pet.insert(QStringLiteral("sgs"),QStringLiteral("80:8#80:8#80:8"));
    full.pet.insert(QStringLiteral("sgsp"),QJsonArray{}); full.pet.insert(QStringLiteral("astrolabebr"),true);
    auto weakFull = full; weakFull.instanceId = 5; weakFull.observationVerified = false;
    weakFull.pet.insert(QStringLiteral("id"),QStringLiteral("5"));
    raw.pets = {factSeed(1),weak,factSeed(3,false),full,weakFull};
    raw.totalPets = raw.pets.size();
    QList<PetAnalysisFacts> facts;
    AlgorithmPipelineStats preparationStats;
    for (const auto& seed : raw.pets) {
      const auto value = derivePetAnalysisFacts(seed,metadata,&preparationStats);
      ok &= require(bool(value),"equivalence fixture failed compact derivation");
      if (value) facts.append(*value);
    }
    if (facts.size() != raw.pets.size()) return 1;
    AccountAssetOverview header = raw; header.pets.clear();
    AlgorithmPipelineStats rawStats,compactStats;
    RecommendationSession original(raw.account,raw,small,metadata,&rawStats,true);
    RecommendationSession compact(raw.account,header,small,{},&compactStats,false,&facts);
    while (original.step(nullptr,16,7) == RecommendationSession::Status::Running) {}
    while (compact.step(nullptr,16,7) == RecommendationSession::Status::Running) {}
    const auto originalRows = original.takeResults(); const auto compactRows = compact.takeResults();
    const auto originalOverview = original.takeOverview(); const auto compactOverview = compact.takeOverview();
    ok &= require(original.status() == RecommendationSession::Status::Complete &&
        compact.status() == RecommendationSession::Status::Complete && equivalentRows(originalRows,compactRows) &&
        equivalentOverview(originalOverview,compactOverview),"compact facts changed rankings, classification, unknowns or overview values");
    ok &= require(preparationStats.rawPowerCalculations == 4 && preparationStats.petsDerived == 4 &&
        rawStats.rawPowerCalculations == 4 && rawStats.petsDerived == 4 &&
        compactStats.preparedFactsReused == 5 && compactStats.petsDerived == 0 && compactStats.rawPowerCalculations == 0 &&
        compactStats.candidatePairsVisited == rawStats.candidatePairsVisited,
        "prepared facts were reported as reparsed raw input or changed H");
  }

  // Use the current production defaults. This is a compact-input memory/correctness
  // test, not end-to-end performance: the immutable fixture is already derived.
  {
    AnalysisWorker worker;
    auto captured = compactInput(key(400),small,*prototype,10000);
    auto cacheLeaseDestroyed = std::make_shared<std::atomic_int>(0);
    captured->preparedFacts[0].asset.memoryRetention = std::make_shared<CacheLeaseProbe>(cacheLeaseDestroyed);
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const AnalysisJobKey& identity) {
      auto value = std::make_shared<AnalysisWorkInput>(*captured); value->key = identity; return value;
    });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& result) { finished.append(result); });
    worker.submit(key(400));
    ok &= require(waitUntil([&] { return finished.size() == 1; },15000),"10,000 compact facts did not finish");
    auto result = worker.lastResult();
    if (result) {
      ok &= require(finished.last().outcome == AnalysisJobOutcome::Published && result->overview.pets.size() == 10000 &&
          result->work.preparedFactsReused == 10000 && result->work.rawPowerCalculations == 0 && result->work.petsDerived == 0 &&
          result->work.candidatePairsVisited == 30000 && result->measuredInputBytes < AnalysisMemoryLimits{}.inputBytes &&
          worker.memoryUsage().peakChargedBytes <= AnalysisMemoryLimits{}.totalBytes,
          "compact P/H result or current default input/peak budgets failed");
      std::optional<PetAssetRecord> guiAsset;
      std::optional<ActionRecommendation> guiAdvice;
      for (const auto& pet : result->overview.pets) if (pet.instanceId == 100000) guiAsset = pet;
      for (const auto& advice : result->recommendations) if (advice.petInstanceId == 100000) guiAdvice = advice;
      ok &= require(guiAsset && guiAdvice,"lease fixture did not publish its asset and advice");
      const quint64 firstResultBytes = result->slices.resultChargedBytes;
      captured.reset(); result.reset();
      worker.setInputFactory([&](const AnalysisJobKey& identity) { return compactInput(identity,small,*prototype,20); });
      worker.submit(key(401));
      ok &= require(waitUntil([&] { return finished.size() == 2; }) && worker.lastResult() &&
          worker.lastResult()->overview.pets.size() == 20 && cacheLeaseDestroyed->load() == 0 &&
          worker.memoryUsage().peakPublicationOverlapBytes > firstResultBytes,
          "replacing a result lost externally held cache/result charges");
      guiAsset.reset();
      ok &= require(cacheLeaseDestroyed->load() == 0,"advice lost its inherited cache lease when the asset was released");
      guiAdvice.reset();
      ok &= require(waitUntil([&] { return cacheLeaseDestroyed->load() == 1; }),"last external advice failed to release the inherited cache lease");
      std::fprintf(stdout,"COMPACT: P=10000 H=30000 input=%llu result=%llu peak=%llu\n",
          static_cast<unsigned long long>(finished.first().measuredInputBytes),static_cast<unsigned long long>(firstResultBytes),
          static_cast<unsigned long long>(worker.memoryUsage().peakChargedBytes));
    } else {
      ok &= require(false,"10,000 compact input was not published under production budgets");
      if (!finished.isEmpty()) std::fprintf(stderr,"compact outcome=%d measured=%llu\n",int(finished.last().outcome),
          static_cast<unsigned long long>(finished.last().measuredInputBytes));
    }
    ok &= require(worker.shutdown(),"compact memory test did not shut down cleanly");
  }

  // Fresh reads share the immutable row array. Stale projections detach exactly
  // once, preserve per-good sharing, and keep their additional Worker lease
  // until the last external view disappears (even after cache replacement).
  {
    AnalysisWorker worker;
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const auto& identity) { return compactInput(identity,small,*prototype,2000); });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    worker.submit(key(410));
    ok &= require(waitUntil([&] { return !finished.isEmpty(); }) && worker.lastResult(),
        "freshness copy fixture failed its source calculation");
    const auto source = worker.lastResult();
    if (source) {
      const quint64 originalBytes = worker.memoryUsage().resultChargedBytes;
      bool exceeded = false;
      auto fresh = worker.freshnessProjection(source->recommendations,false,false,&exceeded);
      ok &= require(!exceeded && fresh.constData() == source->recommendations.constData() &&
          worker.memoryUsage().resultChargedBytes == originalBytes,
          "fresh read detached/sorted rows or allocated another charged projection");
      auto stale = worker.freshnessProjection(source->recommendations,false,true,&exceeded);
      const quint64 firstBytes = worker.memoryUsage().resultChargedBytes;
      ok &= require(!exceeded && stale.size() == fresh.size() && stale.constData() != fresh.constData() &&
          firstBytes > originalBytes,"stale rows escaped their incremental array/lease budget");
      QHash<QString,const ResourceRequirement*> byGood;
      for (const auto& row : std::as_const(stale)) {
        if (row.shopGoodKey.isEmpty()) continue;
        ok &= require(row.shopStale && row.type == RecommendationType::ConditionUnknown &&
            row.requirements.first().condition.freshness == ShopConditionFreshness::Invalidated,
            "stale projection retained actionable resource conditions");
        if (byGood.contains(row.shopGoodKey))
          ok &= require(byGood.value(row.shopGoodKey) == row.requirements.constData(),
              "stale projection detached a requirements array for every pet");
        else byGood.insert(row.shopGoodKey,row.requirements.constData());
      }
      auto again = worker.freshnessProjection(source->recommendations,false,true,&exceeded);
      ok &= require(again.constData() == stale.constData() && worker.memoryUsage().resultChargedBytes == firstBytes &&
          !source->recommendations.first().shopStale &&
          source->recommendations.first().requirements.first().condition.freshness != ShopConditionFreshness::Invalidated,
          "repeated stale reads copied arrays or mutated the source result");
      auto another = worker.freshnessProjection(source->recommendations,true,true,&exceeded);
      const quint64 overlapping = worker.memoryUsage().resultChargedBytes;
      ok &= require(!exceeded && overlapping > firstBytes && stale.constData() == again.constData(),
          "replacing the projection cache forgot a still visible previous copy");
      stale.clear(); again.clear();
      ok &= require(worker.memoryUsage().resultChargedBytes < overlapping &&
          worker.memoryUsage().resultChargedBytes > originalBytes,
          "external stale-copy destruction did not return its independent budget lease");
      worker.submit(key(411));
      const auto unavailable = worker.freshnessProjection(source->recommendations,true,false,&exceeded);
      ok &= require(exceeded && unavailable.isEmpty(),
          "new Core freshness allocation raced an admitted Compute input or returned green rows on rejection");
      ok &= require(waitUntil([&] { return finished.size() == 2; }),"freshness admission fixture did not finish");
      const auto retry = worker.freshnessProjection(source->recommendations,true,false,&exceeded);
      ok &= require(!exceeded && retry.size() == source->recommendations.size(),
          "idle freshness projection could not recover after a busy admission rejection");
      auto tiny = limits(); tiny.resultBytes = 1024;
      AnalysisWorker constrained(tiny);
      const auto rejected = constrained.freshnessProjection(source->recommendations,false,true,&exceeded);
      ok &= require(exceeded && rejected.isEmpty() && constrained.memoryUsage().resultChargedBytes == 0,
          "freshness budget rejection leaked reservations or returned actionable old rows");
      ok &= require(worker.memoryUsage().peakChargedBytes <= AnalysisMemoryLimits{}.totalBytes,
          "stale projections bypassed the existing total Worker budget");
    }
  }

  // Mode/identity/version corruption is rejected before any result appears.
  for (int scenario = 0; scenario < 6; ++scenario) {
    AnalysisWorker worker;
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&,scenario](const auto& identity) {
      auto value = compactInput(identity,small,*prototype,2);
      if (scenario == 0) value->preparedFacts[1] = value->preparedFacts[0];
      if (scenario == 1) ++value->preparedFacts[0].asset.instanceId;
      if (scenario == 2) value->preparedFacts[0].asset.pet.insert(QStringLiteral("sgsp"),QJsonArray{});
      if (scenario == 3) value->overview.pets.append(factSeed(42));
      if (scenario == 4) --value->preparedFacts[0].analysisVersion;
      if (scenario == 5) value->retainedRawPayloads.append(QByteArray("raw mixed with facts"));
      return value;
    });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    worker.submit(key(420+scenario));
    ok &= require(waitUntil([&] { return !finished.isEmpty(); }) && finished.first().outcome == AnalysisJobOutcome::InputRejected &&
        !worker.lastResult() && worker.memoryUsage().inputChargedBytes == 0,
        "invalid compact facts reached publication or retained input reservations");
  }

  {
    AnalysisWorker worker;
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const auto& identity) { return compactInput(identity,large,*prototype,2000); });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    worker.submit(key(450));
    ok &= require(waitUntil([&] { return worker.memoryUsage().candidatePairsVisited > 0; }),
                  "compact cancellation test never entered H evaluation");
    worker.cancel();
    ok &= require(waitUntil([&] { return !finished.isEmpty(); }) && finished.first().outcome == AnalysisJobOutcome::Cancelled &&
        finished.first().cancellationLatencyNanoseconds < 100000000 && !worker.lastResult(),
        "compact H evaluation ignored bounded cancellation or published a cancelled result");
  }

  // Compiled rules/race indexes survive jobs, but no account preparation does.
  {
    auto snapshot = std::make_shared<ShopCatalogSnapshot>();
    snapshot->revision = 3;
    ShopExchangeShop shop; shop.shopId = 1;
    const auto fixture = conditions(6);
    for (auto value : fixture.catalog().goods()) {
      value.good.shelfDate = QDate(2026,9,9);
      value.good.provenUnlimited = true;
      shop.goods.append(value.good);
    }
    shop.goods[0].hasRemovalDate = true;
    shop.goods[0].removalDate = QDate(2026,9,9);
    snapshot->allShops.append(shop);
    std::shared_ptr<const ShopCatalogSnapshot> source = snapshot;
    snapshot.reset();
    QDate date(2026,9,9);
    QString materialName = QStringLiteral("original material");
    qint64 balance = 1000;
    AnalysisWorker worker(limits());
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const AnalysisJobKey& identity) {
      auto value = std::make_shared<AnalysisWorkInput>(*input(identity,{},12));
      value->catalogSnapshot = source; value->catalogDate = date;
      value->materialDefinitions = QJsonObject{{QStringLiteral("items"),QJsonObject{{QStringLiteral("100"),
          QJsonObject{{QStringLiteral("name"),materialName}}}}}};
      value->resourceCounts = {{QStringLiteral("4:100"),balance}};
      value->resourceCountsKnown = true; value->sourceInvalidated = false;
      return value;
    });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    auto run = [&](AnalysisJobKey identity, bool hit, int goods) {
      const int previous = finished.size();
      worker.submit(identity);
      const bool done = waitUntil([&] { return finished.size() == previous + 1; });
      auto result = worker.lastResult();
      ok &= require(done && finished.last().outcome == AnalysisJobOutcome::Published && result &&
          result->key == identity && result->catalogCacheHit == hit &&
          result->work.goodsCompiled == (hit ? 0 : goods) && result->work.goodsReused == (hit ? goods : 0) &&
          result->work.compiledCatalogCacheHits == (hit ? 1 : 0) &&
          result->work.raceAssociationsIndexed == (hit ? 0 : goods) && result->work.accountConditionsPrepared == goods,
          "compiled cache failed cold/warm/date/revision/material/metadata identity counters");
      if (result) {
        ok &= require(result->recommendations.size() == 12 &&
            result->recommendations.first().requirements.first().owned == balance &&
            result->recommendations.first().requirements.first().resourceName == materialName &&
            result->activeInputMeterNanoseconds > 0 && result->conditionPrepareNanoseconds > 0 &&
            result->candidateComputeNanoseconds > 0 && result->sortNanoseconds > 0 &&
            (hit ? result->catalogCompileNanoseconds == 0 : result->catalogCompileNanoseconds > 0),
            "cache reused account balances/names or reported queue waiting as active compile work");
      }
      const auto usage = worker.memoryUsage();
      ok &= require(usage.compiledCatalogChargedBytes > 0 && usage.compiledCatalogChargedBytes <= limits().compiledCatalogBytes &&
          usage.peakChargedBytes <= limits().totalBytes && usage.inputChargedBytes == 0,
          "compiled cache escaped its separate or shared Worker budget");
      return result;
    };
    auto oldResult = run(key(500),false,6);
    balance = 0;
    const auto warm = run(key(501,QStringLiteral("account-b"),2),true,6);
    ok &= require(oldResult && warm && oldResult->recommendations.first().requirements.first().owned == 1000 &&
        warm->recommendations.first().requirements.first().owned == 0,
        "new account preparation mutated an old published result");
    date = date.addDays(1); run(key(502),false,5);
    auto replacement = std::make_shared<ShopCatalogSnapshot>(*source);
    replacement->allShops[0].goods[1].description = QStringLiteral("replaced content, same revision");
    source = replacement; replacement.reset();
    run(key(503),false,5);
    replacement = std::make_shared<ShopCatalogSnapshot>(*source); ++replacement->revision;
    source = replacement; replacement.reset(); run(key(504),false,5);
    materialName = QStringLiteral("changed material"); run(key(505),false,5);
    auto metadataKey = key(506); ++metadataKey.versions.metadata; run(metadataKey,false,5);
    const std::weak_ptr<const ShopCatalogSnapshot> weak = source;
    source.reset();
    ok &= require(weak.expired(),"compiled cache retained the original unmetered catalog JSON snapshot");
    ok &= require(worker.memoryUsage().compiledCatalogCacheHits == 1 &&
        worker.memoryUsage().compiledCatalogCacheMisses == 6 && worker.shutdown() &&
        worker.memoryUsage().compiledCatalogChargedBytes == 0 && worker.memoryUsage().resultChargedBytes > 0,
        "cache counters or cache/result lease shutdown lifetime failed");
  }

  {
    auto source = std::make_shared<ShopCatalogSnapshot>(); source->revision = 3;
    ShopExchangeShop shop;
    const auto fixture = conditions(1);
    auto good = fixture.catalog().goods().first().good;
    good.shelfDate = QDate(2026,9,9); good.unlock = QStringLiteral("test-unlock");
    shop.goods.append(good); source->allShops.append(shop);
    int used = 0;
    ShopConditionState unlocked = ShopConditionState::Satisfied;
    AnalysisWorker worker(limits());
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const auto& identity) {
      auto value = std::make_shared<AnalysisWorkInput>(*input(identity,{},12));
      value->catalogSnapshot = source; value->catalogDate = QDate(2026,9,9);
      value->conditionContext = syntheticQuotaContext({good});
      value->conditionContext.shopPacketKnown = true;
      value->conditionContext.verifiedUnlockFacts.insert(good.unlock,{unlocked,QStringLiteral("test unlock fact"),
          QStringLiteral("explicit fixture"),QDateTime(QDate(2026,9,9),QTime(12,0),Qt::UTC),ShopConditionFreshness::Current});
      value->shopPacket = QJsonObject{{QStringLiteral("si1"),QJsonObject{{good.itemKey(),QJsonObject{{QStringLiteral("dl"),used}}}}}};
      return value;
    });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    for (int phase = 0; phase < 4; ++phase) {
      if (phase == 1) used = 3;
      if (phase == 2) { used = 0; unlocked = ShopConditionState::Blocked; }
      if (phase == 3) unlocked = ShopConditionState::Satisfied;
      worker.submit(key(510+phase));
      ok &= require(waitUntil([&] { return finished.size() == phase+1; }) && worker.lastResult() &&
          finished.last().outcome == AnalysisJobOutcome::Published &&
          worker.lastResult()->catalogCacheHit == (phase > 0) &&
          worker.lastResult()->recommendations.size() == ((phase == 0 || phase == 3) ? 12 : 0) &&
          worker.lastResult()->work.accountConditionsPrepared == 1,
          "warm catalog reused a previous quota or unlock state");
    }
  }

  // A catalogue too large for the configured cache remains a metered job input.
  // Cancellation after cache admission drops account state but keeps reusable
  // immutable rules; quitting releases that final independent cache lease.
  for (bool cacheEnabled : {false,true}) {
    auto source = std::make_shared<ShopCatalogSnapshot>(); source->revision = 3;
    ShopExchangeShop shop;
    const auto fixture = conditions(100);
    for (const auto& compiled : fixture.catalog().goods()) {
      auto good = compiled.good; good.shelfDate = QDate(2026,9,9); good.provenUnlimited = true;
      shop.goods.append(good);
    }
    source->allShops.append(shop);
    auto policy = limits(); policy.compiledCatalogBytes = cacheEnabled ? policy.compiledCatalogBytes : 1;
    AnalysisWorker worker(policy);
    QList<AnalysisJobFinished> finished;
    worker.setInputFactory([&](const auto& identity) {
      auto value = std::make_shared<AnalysisWorkInput>(*input(identity,{},2000));
      value->catalogSnapshot = source; value->catalogDate = QDate(2026,9,9); return value;
    });
    worker.setPublicationGuard([](const auto&) { return true; });
    worker.setFinishedHandler([&](const auto& value) { finished.append(value); });
    worker.submit(key(520 + cacheEnabled));
    ok &= require(waitUntil([&] { return worker.memoryUsage().candidatePairsVisited > 0; }),
        "cache cancellation fixture did not enter candidate work");
    const auto active = worker.memoryUsage();
    ok &= require((active.compiledCatalogChargedBytes > 0) == cacheEnabled &&
        active.inputChargedBytes + active.resultChargedBytes + active.candidateChargedBytes +
            active.compiledCatalogChargedBytes <= policy.totalBytes,
        "in-flight compiled catalog lacked its shared budget lease");
    worker.cancel();
    ok &= require(waitUntil([&] { return !finished.isEmpty(); }) &&
        finished.last().outcome == AnalysisJobOutcome::Cancelled && !worker.lastResult() &&
        worker.memoryUsage().inputChargedBytes == 0 && worker.memoryUsage().resultChargedBytes == 0 &&
        ((worker.memoryUsage().compiledCatalogChargedBytes > 0) == cacheEnabled),
        "cancelled job retained account input/result or lost its independent compiled lease");
    ok &= require(worker.shutdown() && worker.memoryUsage().compiledCatalogChargedBytes == 0,
        "shutdown retained a compiled catalog lease");
  }

  if (!ok) return 1;
  std::fprintf(stdout,
      "METRICS: cancellation_ns=%lld maximum_slice_ns=%lld sort_ns=%lld peak_logical_charge_bytes=%llu\n",
      static_cast<long long>(measuredCancellation), static_cast<long long>(measuredSlice),
      static_cast<long long>(measuredSort), static_cast<unsigned long long>(measuredPeakCharge));
  std::fprintf(stdout, "PASS: fixed Compute thread, sliced cancellation, latest descriptors, release barrier, publication guard, memory budgets\n");
  return 0;
}
