#include "application/pet/pet_detail_preparation_service.h"
#include "domain/asset_derivation.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QSemaphore>
#include <QThread>
#include <deque>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) { if (!valid) std::fprintf(stderr,"FAIL: %s\n",message); return valid; }
template<class Predicate> bool wait(Predicate predicate) { QElapsedTimer clock; clock.start(); while (!predicate() && clock.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents,5); return predicate(); }
struct ComputeThread : QThread { QObject* target = nullptr; QSemaphore started; void run() override { QObject object; target = &object; started.release(); exec(); target = nullptr; } };
struct Compute {
  ComputeThread thread; std::deque<std::function<void()>> queue;
  Compute() { thread.start(); thread.started.acquire(); } ~Compute() { thread.quit(); thread.wait(); }
  auto executor() { return [this](std::function<void()> task) { queue.push_back(std::move(task)); return true; }; }
  void one() { if (queue.empty()) return; auto task = std::move(queue.front()); queue.pop_front(); QMetaObject::invokeMethod(thread.target,[task = std::move(task)] { task(); },Qt::BlockingQueuedConnection); }
};
FrozenDetailInputs input(const DetailSelection& selected, quint64 metadataRevision = 1) {
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>(); metadata->revision = metadataRevision;
  QJsonArray stars; for (int i = 0; i < 700; ++i) stars.append(80);
  QJsonObject object{{QStringLiteral("id"),QString::number(selected.instanceId)},{QStringLiteral("r"),7001},{QStringLiteral("n"),QStringLiteral("Test")},
      {QStringLiteral("lv"),100},{QStringLiteral("sgsp"),stars},{QStringLiteral("asps"),QJsonArray{QStringLiteral("9001")}}};
  auto payload = std::make_shared<RawPetRecordPayload>(); payload->object = object; payload->chargedBytes = 8192;
  auto raw = std::make_shared<RawPetRecord>(); raw->key = {selected.account,selected.epoch,selected.instanceId,1}; raw->complete = raw->sourceKnown = true; raw->payload = payload; raw->brief = AssetDerivation::identityFields(object);
  auto facts = std::make_shared<PetDerivedFactsRecord>(); facts->key = {raw->key,metadataRevision,{}}; facts->facts.asset.raceId = 7001; facts->facts.asset.detailAvailable = facts->facts.asset.observationVerified = true;
  FrozenDetailInputs value; value.raw = raw; value.facts = facts; value.brief = raw->brief; value.summaryRevision = 1; value.metadata = metadata; value.sourceVerified = true; return value;
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); bool ok = true; Compute compute;
  auto service = std::make_unique<PetDetailPreparationService>(compute.executor()); service->bindSession(QStringLiteral("A"),1);
  quint64 metadataRevision = 1, summaryRevision = 1; int captures = 0, published = 0; QHash<int,PreparedPetDetailHandle> output;
  std::weak_ptr<const RawPetRecordPayload> latestRaw;
  std::weak_ptr<const PetDerivedFactsRecord> latestFacts;
  QObject::connect(service.get(),&PetDetailPreparationService::inputsNeeded,service.get(),[&](quint64 task,int,const DetailSelection& selected) { ++captures; auto value = input(selected,metadataRevision); value.summaryRevision = summaryRevision; latestRaw = value.raw->payload; latestFacts = value.facts; service->provideInputs(task,std::move(value)); });
  QObject::connect(service.get(),&PetDetailPreparationService::relatedSummariesNeeded,service.get(),[&](quint64 task,const QVector<qint64>& ids) { QVector<DetailRelatedSummary> values; for (auto id : ids) values.append({id,summaryRevision,true,{{QStringLiteral("id"),QString::number(id)},{QStringLiteral("n"),QStringLiteral("related %1").arg(summaryRevision)}}}); service->provideRelatedSummaries(task,std::move(values)); });
  QObject::connect(service.get(),&PetDetailPreparationService::ready,service.get(),[&](int consumer,quint64,const PreparedPetDetailHandle& detail) { output[consumer] = detail; ++published; });
  const auto drain = [&](int target) { return wait([&] { compute.one(); return published >= target && service->stats().activeTasks == 0; }); };
  ok &= check(service->request(0,{QStringLiteral("A"),1,101}).accepted && service->request(1,{QStringLiteral("A"),1,202}).accepted &&
      service->request(2,{QStringLiteral("A"),1,303}).accepted && !service->request(kDetailConsumerCount,{QStringLiteral("A"),1,404}).accepted,
      "service did not serve exactly its declared consumers");
  ok &= check(drain(3) && output.size() == 3 && service->stats().selectedConsumers == 3,"three selected pages did not complete on the existing Compute loop");
  service->release(2); output.remove(2);
  const auto beforeUnrelated = service->stats().postedSlices; service->summariesChanged({777777}); QCoreApplication::processEvents();
  ok &= check(service->stats().postedSlices == beforeUnrelated,"an unrelated summary invalidated visible details");
  ++summaryRevision; service->summariesChanged({9001}); ok &= check(drain(5) && output[0]->version.relatedSummaryRevision == summaryRevision,"related summary changes did not invalidate both dependent views");
  ++metadataRevision; service->invalidateMetadata(metadataRevision,{}); ok &= check(drain(7) && output[1]->version.facts.metadataRevision == metadataRevision,"metadata invalidation reused old detail facts");
  service->requestPage(0,DetailSection::StargodBackpack,10); ok &= check(drain(8) && output[0]->pages.size() == 1 && output[0]->pages[0].entries.size() == 60,"late backpack page was truncated or unavailable");
  DetailPage external = output[0]->pages[0]; const auto heldBytes = service->stats().retainedResultBytes;
  output.clear(); service->release(0); service->release(1); QCoreApplication::processEvents();
  ok &= check(service->stats().retainedResultBytes > 0 && service->stats().retainedResultBytes <= heldBytes && latestRaw.expired() && !latestFacts.expired(),"copied visible page lost its result/facts lease or retained raw JSON"); external = {};
  ok &= check(wait([&] { return service->stats().retainedResultBytes == 0; }),"final visible page did not return its lease");
  const int priorCaptures = captures, priorPublished = published;
  service->request(0,{QStringLiteral("A"),1,1000}); ok &= check(wait([&] { return compute.queue.size() == 1; }),"held Compute fixture was not dispatched");
  for (int i = 1; i <= 1000; ++i) service->request(0,{QStringLiteral("A"),1,1000 + i});
  ok &= check(captures == priorCaptures + 1 && compute.queue.size() == 1 && service->stats().waitingDescriptors == 1,"rapid replacement captured/queued one raw payload per click");
  ok &= check(drain(priorPublished + 1) && captures == priorCaptures + 2 && output[0]->identity.instanceId == 2000,"superseded selection published or newest selection did not resume");
  const int beforePartial = published;
  service->request(0,{QStringLiteral("A"),1,3000}); ok &= check(wait([&] { return compute.queue.size() == 1; }),"partial cancellation fixture was not dispatched");
  compute.one(); ok &= check(wait([&] { return compute.queue.size() == 1; }),"preparation did not yield before reading the complete backpack");
  service->request(0,{QStringLiteral("A"),1,3001});
  ok &= check(drain(beforePartial + 1) && output[0]->identity.instanceId == 3001 && service->stats().workItems < 100000,"cancellation after a completed slice published stale data or underflowed work counters");
  const int previous = published; service->request(0,{QStringLiteral("A"),1,4000}); ok &= check(wait([&] { return compute.queue.size() == 1; }),"epoch fixture was not dispatched");
  service->bindSession(QStringLiteral("A"),2); service->request(0,{QStringLiteral("A"),2,4000});
  ok &= check(drain(previous + 1) && output[0]->version.facts.record.epoch == 2,"same-account new epoch published an old prepared detail");
  service->request(0,{QStringLiteral("A"),2,4001}); ok &= check(wait([&] { return compute.queue.size() == 1; }),"closing fixture was not dispatched");
  ok &= check(!service->shutdown(),"shutdown waited or claimed the outstanding Compute closure had finished"); service.reset(); compute.one(); QCoreApplication::processEvents();
  output.clear();
  {
    PetDetailPreparationService missingFileService(compute.executor()); missingFileService.bindSession(QStringLiteral("A"),1);
    bool repaired = false; int readyA = 0, readyB = 0; quint64 rejectedRequest = 0; QString actualFailure;
    QObject::connect(&missingFileService,&PetDetailPreparationService::inputsNeeded,&missingFileService,
        [&](quint64 request,int consumer,const DetailSelection& selection) {
      if (consumer == 0 && !repaired) { rejectedRequest = request; missingFileService.rejectInputs(request,QStringLiteral("known raw source file was not found")); }
      else missingFileService.provideInputs(request,input(selection));
    });
    QObject::connect(&missingFileService,&PetDetailPreparationService::relatedSummariesNeeded,&missingFileService,
        [&](quint64 request,const QVector<qint64>& ids) { QVector<DetailRelatedSummary> values; for (auto id : ids) values.append({id,0,false,{}}); missingFileService.provideRelatedSummaries(request,std::move(values)); });
    QObject::connect(&missingFileService,&PetDetailPreparationService::ready,&missingFileService,
        [&](int consumer,quint64,const PreparedPetDetailHandle&) { consumer == 0 ? ++readyA : ++readyB; });
    QObject::connect(&missingFileService,&PetDetailPreparationService::failed,&missingFileService,
        [&](int consumer,quint64 request,DetailPreparationStatus status,const QString& reason) {
      if (consumer == 0 && request == rejectedRequest && status == DetailPreparationStatus::InvalidRequest) actualFailure = reason;
    });
    missingFileService.request(0,{QStringLiteral("A"),1,101}); missingFileService.request(1,{QStringLiteral("A"),1,202});
    ok &= check(wait([&] { compute.one(); return readyB == 1; }) && readyA == 0 && actualFailure == QStringLiteral("known raw source file was not found"),
        "terminal missing input lost its real error or blocked the second detail consumer");
    repaired = true;
    const auto retried = missingFileService.request(0,{QStringLiteral("A"),1,101});
    missingFileService.rejectInputs(rejectedRequest,QStringLiteral("late old rejection"));
    ok &= check(retried.accepted && retried.requestId != rejectedRequest && wait([&] { compute.one(); return readyA == 1; }) && readyB == 1,
        "same selection could not retry after its input was repaired or a stale rejection cancelled it");
  }
  {
    PetDetailPreparationService offlineService(compute.executor()); offlineService.bindSession(QStringLiteral("offline-local"),0);
    PreparedPetDetailHandle latest; int count = 0;
    QObject::connect(&offlineService,&PetDetailPreparationService::inputsNeeded,&offlineService,
        [&](quint64 request,int,const DetailSelection& selection) { offlineService.provideInputs(request,input(selection)); });
    QObject::connect(&offlineService,&PetDetailPreparationService::relatedSummariesNeeded,&offlineService,
        [&](quint64 request,const QVector<qint64>& ids) { QVector<DetailRelatedSummary> values; for (auto id : ids) values.append({id,0,false,{}}); offlineService.provideRelatedSummaries(request,std::move(values)); });
    QObject::connect(&offlineService,&PetDetailPreparationService::ready,&offlineService,
        [&](int,quint64,const PreparedPetDetailHandle& value) { latest = value; ++count; });
    const auto offline = offlineService.request(0,{QStringLiteral("offline-local"),0,101});
    ok &= check(offline.accepted && wait([&] { compute.one(); return count == 1; }) && latest->version.facts.record.epoch == 0 && !latest->sourceVerified,
        "offline detail was refused or claimed a verified current session");
    ok &= check(!offlineService.request(1,{QString(),0,202}).accepted,"empty local account was accepted for offline detail");
    if (offline.accepted) {
      offlineService.request(0,{QStringLiteral("offline-local"),0,102}); ok &= check(wait([&] { return compute.queue.size() == 1; }),"offline detail did not reach Compute");
      offlineService.bindSession(QStringLiteral("offline-local"),1); offlineService.request(0,{QStringLiteral("offline-local"),1,102});
      ok &= check(wait([&] { compute.one(); return count == 2; }) && latest->version.facts.record.epoch == 1,"old offline detail published after login epoch began");
    }
  }
  PetDetailPreparationService inlineService([](std::function<void()> task) { task(); return true; }); inlineService.bindSession(QStringLiteral("B"),1); bool executorRejected = false;
  QObject::connect(&inlineService,&PetDetailPreparationService::inputsNeeded,&inlineService,[&](quint64 task,int,const DetailSelection& selection) { inlineService.provideInputs(task,input(selection)); });
  QObject::connect(&inlineService,&PetDetailPreparationService::failed,&inlineService,[&](int,quint64,DetailPreparationStatus status,const QString&) { executorRejected |= status == DetailPreparationStatus::ComputeUnavailable; });
  inlineService.request(0,{QStringLiteral("B"),1,1}); ok &= check(wait([&] { return executorRejected; }),"service executed detail preparation inline on Core");
  if (ok) std::puts("PASS: shared multi-consumer detail service, paging, related/meta invalidation, leased pages, 1000 replacements, epoch and nonwaiting shutdown");
  return ok ? 0 : 1;
}
