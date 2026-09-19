#include "application/pet/pet_repository.h"
#include "application/analysis/asset_analyzer.h"
#include "application/catalog/routine_overview_catalog.h"
#include "application/routine/routine_overview_controller.h"
#include "support/protocol_test_support.h"
#include "storage/storage_service.h"
#include "support/catalog_test_support.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSemaphore>

#include <algorithm>
#include <iostream>
#include <limits>

namespace {

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
}

bool require(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

template<class Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 3000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return predicate();
}

// Prints the actual summary next to a failing expectation: a bare FAIL line
// cannot separate a stale expectation from an aggregation defect.
void reportSummary(const char* label, const RoutineOpportunitySummary& value) {
  std::cerr << "  " << label << ": completeness=" << static_cast<int>(value.completeness)
            << " total=" << value.total << " confirmed=" << value.confirmedSources
            << " expected=" << value.expectedSources
            << " pending=" << value.pendingSources.size();
  for (const QString& pending : value.pendingSources) std::cerr << " [" << qPrintable(pending) << "]";
  std::cerr << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  if (!root.isValid()) return 1;
  qputenv("KQPET_DATA_ROOT", root.path().toUtf8());

  QTemporaryDir officialRoot;
  QString catalogError;
  bool ok = require(writeRoutineOfficialFixture(officialRoot.path()), "routine official fixture could not be created");
  StorageService catalogStorage(root.path());
  CatalogIoOptions catalogOptions;
  catalogOptions.officialRoot = officialRoot.path();
  CatalogIoService catalogIo(&catalogStorage, catalogOptions);
  ok &= require(runCatalogRequest(&catalogIo, CatalogKind::Routine, CatalogRequestMode::OfficialUpdate, &catalogError),
                qPrintable(QStringLiteral("dynamic catalog: %1").arg(catalogError)));
  ok &= require(!RoutineOverviewCatalog::instance().tasks().isEmpty(),
                "daily/weekly task catalog is empty");
  ok &= require(!RoutineOverviewCatalog::instance().activities().isEmpty(),
                "activity catalog is empty");

  PetRepository repository;
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                        {QStringLiteral("info"),
                         QJsonObject{{QStringLiteral("n"), QStringLiteral("routine-a")}}}});
  RoutineOverviewController controller(&repository);
  QObject::connect(&repository, &PetRepository::packetObserved, &controller,
      [&controller](const QJsonObject& packet, const InboundEnvelope& envelope) {
        controller.handleDecodedEnvelope(envelope, packet);
      });
  const auto sendRoutinePacket = [&repository](const QString&, const QString& payload) {
    deliverVerifiedFixture(&repository, QJsonDocument::fromJson(payload.toUtf8()).object());
  };
  struct Sent { QString service; QString command; QString params; };
  QList<Sent> sent;
  controller.setSender([&sent](const QString& service, const QString& command,
                               const QString& params) {
    sent.append({service, command, params});
    return true;
  });
  ok &= require(controller.requestRefresh(), "manual refresh did not start");
  ok &= require(sent.size() == 7, "manual refresh must send exactly seven read-only requests");
  ok &= require(sent.value(0).command == QStringLiteral("1008_20170623_dt_0"),
                "daily/weekly command mismatch");
  ok &= require(sent.value(1).service == QStringLiteral("null") &&
                    sent.value(1).command == QStringLiteral("1037_0"),
                "activity red-point command mismatch");
  ok &= require(sent.value(2).command == QStringLiteral("1008_20220603_swa_0_0"),
                "opportunity command mismatch");
  ok &= require(std::none_of(sent.cbegin(), sent.cend(), [](const Sent& request) {
                  return request.command == QStringLiteral("16_24_A") ||
                         request.command == QStringLiteral("16_6_0") ||
                         request.command == QStringLiteral("100_13_0") ||
                         request.command == QStringLiteral("100_2_0");
                }),
                "arena/ranking or removed pet-park commands must never be sent");
  ok &= require(sent.value(3).command == QStringLiteral("1008_20190531_gbt_1") &&
                    sent.value(4).command == QStringLiteral("2_36_1") &&
                    sent.value(5).command == QStringLiteral("110_123_0") &&
                    sent.value(6).command == QStringLiteral("1008_20260522_nf_0") &&
                    sent.value(6).params == QStringLiteral("{\"un\":-1}"),
                "target opportunity query mapping mismatch");

  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(
          QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
                      {QStringLiteral("av"), 30}, {QStringLiteral("wav"), 300},
                      {QStringLiteral("ti"), QJsonArray{1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0}},
                      {QStringLiteral("wti"), QJsonArray{15, 6, 13, 32, 13, 5, 90, 7, 15, 7, 10, 1, 8}},
                      {QStringLiteral("bi"), QJsonArray{true, false, false, false, false}},
                      {QStringLiteral("wbi"), QJsonArray{false, false, false, false, false}}})
                            .toJson(QJsonDocument::Compact)));
  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"1008_20220603_swa_0_0\",\"r\":1,\"ti\":2,\"wgt\":4}"));
  // An incomplete ArenaV3 packet must not be presented as challenge counts.
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"16_24_A\",\"r\":1,\"sweep\":3}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20190531_gbt_1\",\"r\":1,\"ti\":5}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"2_36_1\",\"t\":6,\"cclt\":0}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"110_123_0\",\"r\":1,\"rwwt\":18,\"wwt\":2,\"rdt\":4,\"rdb\":0}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20260522_nf_0\",\"pt\":8,\"rft\":16}"));
  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(
          QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1037_0")},
                      {QStringLiteral("r"), 1},
                      {QStringLiteral("rs"), QStringLiteral("10011#10025")}})
                            .toJson(QJsonDocument::Compact)));
  ok &= require(controller.hasDailyPacket(), "daily packet was not stored");
  ok &= require(controller.hasRedPointPacket() &&
                    controller.activeRedPoints().contains(10011),
                "red-point packet was not stored");
  ok &= require(controller.opportunityPackets()
                        .value(QStringLiteral("1008_20220603_swa_0_0"))
                        .toObject().value(QStringLiteral("ti")).toInt() == 2 &&
                    !controller.opportunityPackets().contains(QStringLiteral("16_24_A")),
                "safe opportunity packet was not stored or arena data leaked in");

  // The game may request ArenaV3 itself when the user opens it. Observing the
  // returned challenge counters is safe even when no plugin refresh is active.
  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"16_24_A\",\"r\":1,"
                     "\"zao1\":{\"curz\":102,\"ct\":2,\"bct\":0,\"cd\":99},"
                     "\"zao2\":{\"curz\":201,\"ct\":3,\"bct\":1,\"cd\":88}}"));
  ok &= require(controller.opportunityPackets()
                        .value(QStringLiteral("16_24_A")).toObject()
                        .value(QStringLiteral("zao1")).toObject()
                        .value(QStringLiteral("ct")).toInt() == 2,
                "passive ArenaV3 challenge counters were not cached");

  sent.clear();
  ok &= require(controller.requestRefresh(), "partial-failure refresh did not start");
  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(
          QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
                      {QStringLiteral("av"), 31},
                      {QStringLiteral("ti"), QJsonArray{}},
                      {QStringLiteral("wti"), QJsonArray{}}})
                            .toJson(QJsonDocument::Compact)));
  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"1008_20220603_swa_0_0\",\"r\":0}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20190531_gbt_1\",\"r\":1,\"ti\":4}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"2_36_1\",\"t\":5}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"110_123_0\",\"r\":1,\"rwwt\":17,\"rdt\":5}"));
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20260522_nf_0\",\"pt\":7,\"rft\":15}"));
  sendRoutinePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"1037_0\",\"r\":1,\"rs\":\"10026\"}"));
  ok &= require(controller.dailyPacket().value(QStringLiteral("av")).toInt() == 31 &&
                    controller.activeRedPoints().contains(10026),
                "successful sibling responses were discarded after one request failed");
  ok &= require(controller.cachedOpportunityPackets()
                        .value(QStringLiteral("1008_20220603_swa_0_0"))
                        .toObject().value(QStringLiteral("ti")).toInt() == 2,
                "failed opportunity response overwrote its old cache");
  ok &= require(!controller.opportunityPackets().contains(QStringLiteral("1008_20220603_swa_0_0")),
                "failed opportunity response left its old observation marked current");

  const auto sendObject = [&repository](const QJsonObject& packet) {
    deliverVerifiedFixture(&repository, packet);
  };
  const auto finishPending = [&sent, &sendObject]() {
    for (const Sent& request : std::as_const(sent))
      sendObject({{QStringLiteral("_cmd"), request.command}, {QStringLiteral("r"), 0}});
  };
  const QJsonObject completeDaily{
      {QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
      {QStringLiteral("av"), 30}, {QStringLiteral("wav"), 300},
      {QStringLiteral("ti"), QJsonArray{1, 0}}, {QStringLiteral("wti"), QJsonArray{2, 3}},
      {QStringLiteral("bi"), QJsonArray{true, false}},
      {QStringLiteral("wbi"), QJsonArray{false, true}}};
  sent.clear();
  ok &= require(controller.requestRefresh(), "daily validation baseline did not start");
  sendObject(completeDaily);
  finishPending();
  const QDateTime dayTime = controller.observedAt(QStringLiteral("ti"));
  sent.clear();
  ok &= require(controller.requestRefresh(), "partial-daily batch did not start");
  sendObject({{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
              {QStringLiteral("av"), 0}, {QStringLiteral("ti"), QJsonArray{1, 1.5}},
              {QStringLiteral("bi"), QJsonArray{0}}});
  finishPending();
  ok &= require(!controller.hasDailyPacket() &&
                    controller.dailyPacket().value(QStringLiteral("av")).toInt() == 0 &&
                    controller.dailyPacket().value(QStringLiteral("ti")) == completeDaily.value(QStringLiteral("ti")) &&
                    controller.dailyPacket().value(QStringLiteral("bi")) == completeDaily.value(QStringLiteral("bi")) &&
                    controller.fieldState(QStringLiteral("ti")) == PacketFieldState::Invalid &&
                    controller.fieldState(QStringLiteral("wav")) == PacketFieldState::Missing &&
                    controller.observedAt(QStringLiteral("ti")) == dayTime,
                "partial daily packet lost valid zero, replaced invalid arrays or stayed current");

  const QSet<int> previousPoints = controller.activeRedPoints();
  const QDateTime pointsTime = controller.observedAt(QStringLiteral("rs"));
  for (const QJsonValue& invalid : QList<QJsonValue>{QJsonValue(QJsonValue::Undefined),
       QJsonValue(QJsonValue::Null), 0, true, QStringLiteral("10026#not-an-id"),
       QStringLiteral("10026##10027"), QStringLiteral("0"), QStringLiteral("2147483648")}) {
    sent.clear();
    ok &= require(controller.requestRefresh(), "invalid-redpoint batch did not start");
    QJsonObject packet{{QStringLiteral("_cmd"), QStringLiteral("1037_0")},
                        {QStringLiteral("r"), 1}};
    if (!invalid.isUndefined()) packet.insert(QStringLiteral("rs"), invalid);
    sendObject(packet);
    finishPending();
    ok &= require(!controller.hasRedPointPacket() && controller.activeRedPoints() == previousPoints &&
                      controller.observedAt(QStringLiteral("rs")) == pointsTime,
                  "invalid red-point payload cleared old set or advanced its observation");
  }
  sent.clear();
  ok &= require(controller.requestRefresh(), "explicit-empty batch did not start");
  QJsonObject emptyDaily = completeDaily;
  for (const QString& field : {QStringLiteral("ti"), QStringLiteral("wti"),
                               QStringLiteral("bi"), QStringLiteral("wbi")})
    emptyDaily.insert(field, QJsonArray{});
  emptyDaily.insert(QStringLiteral("av"), 0);
  emptyDaily.insert(QStringLiteral("wav"), 0);
  sendObject(emptyDaily);
  sendObject({{QStringLiteral("_cmd"), QStringLiteral("1037_0")},
              {QStringLiteral("r"), 1}, {QStringLiteral("rs"), QStringLiteral("")}});
  finishPending();
  ok &= require(controller.hasDailyPacket() && controller.hasRedPointPacket() &&
                    controller.activeRedPoints().isEmpty() &&
                    controller.fieldState(QStringLiteral("ti")) == PacketFieldState::Empty &&
                    controller.fieldState(QStringLiteral("rs")) == PacketFieldState::Empty,
                "explicit empty arrays/string were confused with missing data");

  for (const QJsonValue& invalid : QList<QJsonValue>{QJsonValue(QJsonValue::Undefined),
       QJsonValue(QJsonValue::Null), -1, 1.5, true, QStringLiteral("bad"), 9007199254740992.0}) {
    sent.clear();
    ok &= require(controller.requestRefresh(), "invalid-opportunity batch did not start");
    QJsonObject packet{{QStringLiteral("_cmd"), QStringLiteral("1008_20220603_swa_0_0")},
                        {QStringLiteral("wgt"), 4}};
    if (!invalid.isUndefined()) packet.insert(QStringLiteral("ti"), invalid);
    sendObject(packet);
    finishPending();
    ok &= require(!controller.opportunityPackets().contains(QStringLiteral("1008_20220603_swa_0_0")) &&
                      controller.cachedOpportunityPackets().value(QStringLiteral("1008_20220603_swa_0_0"))
                          .toObject().value(QStringLiteral("ti")).toInt() == 2,
                  "invalid opportunity counter overwrote cached value or remained current");
  }
  sent.clear();
  ok &= require(controller.requestRefresh(), "zero-opportunity batch did not start");
  sendObject({{QStringLiteral("_cmd"), QStringLiteral("1008_20220603_swa_0_0")},
              {QStringLiteral("ti"), 0}, {QStringLiteral("wgt"), 0}});
  sendObject({{QStringLiteral("_cmd"), QStringLiteral("110_123_0")},
              {QStringLiteral("rwwt"), 0}, {QStringLiteral("rdt"), 0},
              {QStringLiteral("rdb"), std::numeric_limits<int>::max()},
              {QStringLiteral("wwt"), 0}});
  finishPending();
  ok &= require(controller.opportunityPackets().contains(QStringLiteral("1008_20220603_swa_0_0")) &&
                    controller.opportunityPackets().value(QStringLiteral("1008_20220603_swa_0_0"))
                        .toObject().value(QStringLiteral("ti")).toInt(-1) == 0 &&
                    !controller.opportunityPackets().contains(QStringLiteral("110_123_0")),
                "zero counter was lost or overflowing derived opportunity count was accepted");

  sendObject({{QStringLiteral("_cmd"), QStringLiteral("16_24_A")},
              {QStringLiteral("zao1"), QJsonObject{{QStringLiteral("ct"), 0}, {QStringLiteral("bct"), 0}}},
              {QStringLiteral("zao2"), QJsonObject{{QStringLiteral("ct"), 1.5}, {QStringLiteral("bct"), 0}}}});
  ok &= require(controller.opportunityPackets().value(QStringLiteral("16_24_A"))
                        .toObject().value(QStringLiteral("zao1")).toObject()
                        .value(QStringLiteral("ct")).toInt(-1) == 0 &&
                    !controller.opportunityPackets().value(QStringLiteral("16_24_A"))
                        .toObject().contains(QStringLiteral("zao2")) &&
                    controller.cachedOpportunityPackets().value(QStringLiteral("16_24_A"))
                        .toObject().value(QStringLiteral("zao2")).toObject()
                        .value(QStringLiteral("ct")).toInt() == 3,
                "partially valid ArenaV3 data replaced the other mode or discarded valid zero");

  const QString cachePath = QDir(QFileInfo(repository.cachePath()).absolutePath())
                                .filePath(QStringLiteral("routines.json"));
  ok &= require(waitUntil([&] { return controller.pendingStorageCount() == 0; }),
                "routine asynchronous persistence did not complete");
  QFile cache(cachePath);
  ok &= require(cache.open(QIODevice::ReadOnly), "routine cache was not persisted");
  const QByteArray saved = cache.readAll();
  cache.close();
  const QJsonObject beforeWeak = controller.cachedOpportunityPackets();
  controller.handlePacket(QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"16_24_A\",\"zao1\":{\"ct\":7,\"bct\":0}}"));
  ok &= require(controller.cachedOpportunityPackets() == beforeWeak &&
                    !controller.unverifiedPackets().contains(QStringLiteral("16_24_A")) &&
                    cache.open(QIODevice::ReadOnly) && cache.readAll() == saved,
                "unverified routine observation borrowed current source or persisted");
  cache.close();
  RoutineOverviewController cachedController(&repository);
  ok &= require(waitUntil([&] { return !cachedController.cacheLoading(); }),
                "routine asynchronous cache read did not complete");
  ok &= require(!cachedController.hasDailyPacket() && !cachedController.hasRedPointPacket() &&
                    cachedController.opportunityPackets().isEmpty() &&
                    !cachedController.cachedOpportunityPackets().isEmpty(),
                "routine cache reload promoted old observations to current");
  {
    QSemaphore entered, release;
    const bool held = repository.storageService()->postAuxiliary([&](QObject*) {
      entered.release(); release.acquire();
    });
    ok &= require(held && entered.tryAcquire(1, 2000), "routine delayed-read fixture did not hold IO");
    RoutineOverviewController lateReader(&repository);
    const QJsonObject arena{{QStringLiteral("_cmd"), QStringLiteral("16_24_A")},
        {QStringLiteral("zao1"), QJsonObject{{QStringLiteral("ct"), 2}, {QStringLiteral("bct"), 0}}}};
    lateReader.handleDecodedEnvelope(verifiedFixtureEnvelope(&repository, arena), arena);
    ok &= require(lateReader.cacheLoading() && lateReader.pendingStorageCount() <= 2,
                  "routine network observation waited for disk or unbounded pending copies");
    release.release();
    ok &= require(waitUntil([&] { return lateReader.pendingStorageCount() == 0; }) &&
                      lateReader.opportunityPackets().value(QStringLiteral("16_24_A")).toObject()
                          .value(QStringLiteral("zao1")).toObject().value(QStringLiteral("ct")).toInt(-1) == 2 &&
                      lateReader.cachedOpportunityPackets().value(QStringLiteral("16_24_A")).toObject()
                          .value(QStringLiteral("zao2")).toObject().value(QStringLiteral("ct")).toInt(-1) == 3 &&
                      !lateReader.opportunityPackets().value(QStringLiteral("16_24_A")).toObject().contains(QStringLiteral("zao2")),
                  "late routine disk image overwrote a fresh group or promoted its historical sibling");
  }

  sent.clear();
  ok &= require(controller.requestRefresh(), "second manual refresh did not start");
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                        {QStringLiteral("info"),
                         QJsonObject{{QStringLiteral("n"), QStringLiteral("routine-b")}}}});
  sendRoutinePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1037_0\",\"r\":1,\"rs\":\"99999\"}"));
  ok &= require(!controller.activeRedPoints().contains(99999),
                "old account response leaked into the new account");
  {
    QTemporaryDir weakRoot;
    StorageService weakStorage(weakRoot.path());
    PetRepository weakRepository(nullptr, &weakStorage);
    weakRepository.handlePacket(QStringLiteral("recivedata"),
        QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"routine-read-only\"}}"));
    RoutineOverviewController weak(&weakRepository);
    QObject::connect(&weakRepository, &PetRepository::packetObserved, &weak,
        [&weak](const QJsonObject& packet, const InboundEnvelope& envelope) {
          weak.handleDecodedEnvelope(envelope, packet);
        });
    QList<OutboundIntent> requests;
    QString status;
    QObject::connect(&weak, &RoutineOverviewController::statusChanged, &weak,
                     [&](const QString& value) { status = value; });
    weak.setAsyncSender([&](const OutboundIntent& intent) { requests.append(intent); return true; });
    ok &= require(weak.requestRefresh() && requests.size() == 7, "weak routine batch did not start");
    quint64 sequence = 1;
    const auto sendWeak = [&](const QJsonObject& value, quint64 epoch) {
      InboundEnvelope envelope;
      envelope.receiveSequence = ++sequence;
      envelope.receivedMonotonicMs = transportMonotonicMs();
      envelope.capturedSessionEpoch = epoch;
      envelope.method = QStringLiteral("recivedata");
      envelope.payload = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
      weakRepository.handleEnvelope(envelope);
    };
    const QJsonObject daily{{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
        {QStringLiteral("av"), 30}, {QStringLiteral("wav"), 300},
        {QStringLiteral("ti"), QJsonArray{1}}, {QStringLiteral("wti"), QJsonArray{2}},
        {QStringLiteral("bi"), QJsonArray{false}}, {QStringLiteral("wbi"), QJsonArray{true}}};
    sendWeak(daily, 0); // actual host capture has no verified epoch
    ok &= require(weak.unverifiedPackets().isEmpty() && weak.isRunning(),
                  "not-yet-dispatched weak routine response completed a request");
    for (const auto& request : requests) {
      request.permit.claim(transportMonotonicMs());
      request.permit.complete(SubmissionOutcome::Submitted);
    }
    sendWeak(daily, weakRepository.sessionGeneration() + 1);
    ok &= require(weak.unverifiedPackets().isEmpty(), "wrong-epoch weak routine response was displayed");
    sendWeak(daily, 0);
    for (const auto& request : requests) {
      if (request.ticket.command == daily.value(QStringLiteral("_cmd")).toString()) continue;
      QJsonObject response{{QStringLiteral("_cmd"), request.ticket.command},
          {QStringLiteral("rs"), QStringLiteral("123#456")},
          {QStringLiteral("ti"), 2}, {QStringLiteral("wgt"), 4}, {QStringLiteral("t"), 6},
          {QStringLiteral("rwwt"), 0}, {QStringLiteral("rdt"), 0}, {QStringLiteral("rdb"), 0},
          {QStringLiteral("wwt"), 0}, {QStringLiteral("pt"), 3}, {QStringLiteral("rft"), 1}};
      sendWeak(response, 0);
    }
    ok &= require(!weak.isRunning() && weak.unverifiedPackets().size() == 7 &&
                      weak.unverifiedPackets().value(QStringLiteral("1008_20170623_dt_0")).toObject()
                          .value(QStringLiteral("av")).toInt() == 30 &&
                      weak.dailyPacket().isEmpty() && weak.activeRedPoints().isEmpty() &&
                      weak.cachedOpportunityPackets().isEmpty() && weak.pendingWriteCount() == 0 &&
                      !weakRepository.sessionContext().canPersist() && status.contains(QStringLiteral("只读")) &&
                      !status.contains(QStringLiteral("超时")),
                  "valid weak routine replies timed out, disappeared or became persisted account facts");
    const auto observations = weak.unverifiedPackets();
    requests.clear();
    ok &= require(weak.requestRefresh(), "malformed weak routine batch did not start");
    for (const auto& request : requests) {
      request.permit.claim(transportMonotonicMs());
      request.permit.complete(SubmissionOutcome::Submitted);
      sendWeak({{QStringLiteral("_cmd"), request.ticket.command}}, weakRepository.sessionGeneration());
    }
    ok &= require(!weak.isRunning() && weak.unverifiedPackets() == observations &&
                      !status.contains(QStringLiteral("超时")) && status.contains(QStringLiteral("未更新数据")),
                  "malformed weak routine replies became data or were mislabeled as timeouts");
    requests.clear();
    ok &= require(weak.requestRefresh(), "missing weak routine reply batch did not start");
    for (const auto& request : requests)
      weak.handleSendReceipt({request.ticket.taskId, request.ticket.account, request.ticket.sessionEpoch,
          request.ticket.command, SubmissionOutcome::Submitted, transportMonotonicMs() - 10001});
    ok &= require(!weak.isRunning() && status.contains(QStringLiteral("超时")),
                  "a genuinely missing routine response stopped reporting its deadline");
  }
  // A period total means nothing without its completeness: one answered source
  // must never be presented as the whole period.
  {
    PetRepository summaryRepository;
    deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("routine-summary")}}}});
    RoutineOverviewController summaryController(&summaryRepository);
    QObject::connect(&summaryRepository, &PetRepository::packetObserved, &summaryController,
        [&summaryController](const QJsonObject& packet, const InboundEnvelope& envelope) {
          summaryController.handleDecodedEnvelope(envelope, packet);
        });
    ObservationClockSample clock{QDateTime(QDate(2026, 9, 9), QTime(10, 0), Qt::UTC), 250000, 0,
                                 QStringLiteral("synthetic/UTC")};
    summaryController.setObservationClock([&clock] { return clock; });
    QStringList requested;
    summaryController.setSender([&requested](const QString&, const QString& command, const QString&) {
      requested.append(command);
      return true;
    });
    const auto acceptPeriod = [&](const QString& group, const QString& period) {
      TrustedObservationValidity evidence{summaryRepository.accountKey(),
          summaryRepository.sessionGeneration(), group, summaryController.observedSequence(group), period,
          QStringLiteral("synthetic-routine-period"), clock.utc.addSecs(-3600), clock.utc.addSecs(3600),
          clock.utc, 0, QStringLiteral("tests/routine_overview_smoke.cpp synthetic routine period evidence")};
      QString error;
      const bool accepted = summaryController.acceptPeriodValidityEvidence(evidence, &error);
      if (!accepted) std::cerr << "routine period evidence rejected: " << qPrintable(error) << '\n';
      return accepted;
    };
    AssetAnalyzer analyzer(&summaryRepository, nullptr, &summaryController);
    const QJsonObject dailyReply{{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
        {QStringLiteral("av"), 30}, {QStringLiteral("wav"), 300},
        {QStringLiteral("ti"), QJsonArray{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}},
        {QStringLiteral("wti"), QJsonArray{15, 6, 13, 32, 13, 5, 90, 7, 15, 7, 10, 1, 8}},
        {QStringLiteral("bi"), QJsonArray{true, false, false, false, false}},
        {QStringLiteral("wbi"), QJsonArray{false, false, false, false, false}}};
    const QJsonObject redPointReply{{QStringLiteral("_cmd"), QStringLiteral("1037_0")},
        {QStringLiteral("rs"), QStringLiteral("123#456")}};
    const auto startCycle = [&]() {
      requested.clear();
      return summaryController.requestRefresh() && requested.size() == 7;
    };
    const auto closeCycle = [&]() {
      deliver(&summaryRepository, dailyReply);
      deliver(&summaryRepository, redPointReply);
      return !summaryController.isRunning();
    };
    const QStringList queried{QStringLiteral("1008_20220603_swa_0_0"),
        QStringLiteral("1008_20190531_gbt_1"), QStringLiteral("2_36_1"),
        QStringLiteral("1008_20260522_nf_0"), QStringLiteral("110_123_0")};
    const auto deliverQueried = [&](const QJsonValue& beast, qint64 tree) {
      deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("1008_20220603_swa_0_0")},
          {QStringLiteral("ti"), 2}, {QStringLiteral("wgt"), 4}});
      deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("1008_20190531_gbt_1")},
          {QStringLiteral("ti"), tree}});
      deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("2_36_1")},
          {QStringLiteral("t"), beast}});
      deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("1008_20260522_nf_0")},
          {QStringLiteral("pt"), 3}, {QStringLiteral("rft"), 1}});
      deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("110_123_0")},
          {QStringLiteral("rwwt"), 4}, {QStringLiteral("rdt"), 1}, {QStringLiteral("rdb"), 0},
          {QStringLiteral("wwt"), 0}});
    };
    // Cycle 1: one queried source answers with an unusable value. The other
    // four own the subtotal, and the period is never called complete.
    ok &= require(startCycle(), "the routine summary refresh did not start as the full read-only batch");
    deliverQueried(QJsonValue(QStringLiteral("bad")), 1);
    ok &= require(closeCycle(), "the first routine summary cycle did not complete");
    for (const QString& group : {QStringLiteral("1008_20220603_swa_0_0"),
             QStringLiteral("1008_20190531_gbt_1"), QStringLiteral("1008_20260522_nf_0"),
             QStringLiteral("110_123_0")})
      ok &= require(acceptPeriod(group, QStringLiteral("activity")),
                    qPrintable(QStringLiteral("period evidence rejected for %1").arg(group)));
    const AccountAssetOverview partialSummary = analyzer.routineSummary();
    const bool partialReported = !partialSummary.todayOpportunityKnown &&
        partialSummary.todayOpportunityRemaining == 2 + 1 + 3 + 39 &&
        partialSummary.todayOpportunities.completeness == RoutineCompleteness::Partial &&
        partialSummary.todayOpportunities.expectedSources == 5 &&
        partialSummary.todayOpportunities.confirmedSources == 4 &&
        partialSummary.todayOpportunities.total == 2 + 1 + 3 + 39 &&
        partialSummary.todayOpportunities.pendingSources.size() == 1 &&
        partialSummary.todayOpportunities.observedAt.isValid() &&
        partialSummary.routineDataKnown;
    if (!partialReported) reportSummary("partial", partialSummary.todayOpportunities);
    ok &= require(partialReported,
                  "a partially confirmed source set was reported as the whole period total");
    // The weekly sources are independent of the daily ones even inside the same
    // reply: both answered here, so the weekly period stays complete.
    ok &= require(partialSummary.weekOpportunityKnown &&
                      partialSummary.weekOpportunities.completeness == RoutineCompleteness::Complete &&
                      partialSummary.weekOpportunities.total == 2 + 4,
                  "an unusable daily source made the independent weekly sources unknown");

    // Cycle 2: the same sources answer properly, so the period becomes complete.
    ok &= require(startCycle(), "the second routine summary cycle did not start");
    deliverQueried(2, 1);
    ok &= require(closeCycle(), "the second routine summary cycle did not complete");
    for (const QString& group : queried)
      ok &= require(acceptPeriod(group, QStringLiteral("activity")),
                    qPrintable(QStringLiteral("period evidence rejected for %1").arg(group)));
    const AccountAssetOverview completeSummary = analyzer.routineSummary();
    const bool completeReported = completeSummary.todayOpportunityKnown &&
        completeSummary.todayOpportunities.completeness == RoutineCompleteness::Complete &&
        completeSummary.todayOpportunities.expectedSources == 5 &&
        completeSummary.todayOpportunities.confirmedSources == 5 &&
        completeSummary.todayOpportunities.pendingSources.isEmpty() &&
        completeSummary.todayOpportunities.total == 2 + 1 + 2 + 3 + 39 &&
        completeSummary.weekOpportunityKnown &&
        completeSummary.weekOpportunities.completeness == RoutineCompleteness::Complete &&
        completeSummary.weekOpportunities.total == 2 + 4;
    if (!completeReported) reportSummary("complete", completeSummary.todayOpportunities);
    ok &= require(completeReported, "fully confirmed sources were not reported as a complete total");
    // Arena counters are the split sources: each returned field is its own
    // contribution, and a field the game never returned stays pending.
    deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("16_24_A")},
        {QStringLiteral("zao1"), QJsonObject{{QStringLiteral("ct"), 1}, {QStringLiteral("bct"), 0}}}});
    ok &= require(acceptPeriod(QStringLiteral("16_24_A:zao1"), QStringLiteral("activity")),
                  "the arena challenge period evidence was rejected");
    const AccountAssetOverview arenaSummary = analyzer.routineSummary();
    const bool arenaReported =
        arenaSummary.todayOpportunities.completeness == RoutineCompleteness::Partial &&
        arenaSummary.todayOpportunities.expectedSources == 7 &&
        arenaSummary.todayOpportunities.confirmedSources == 6 &&
        arenaSummary.todayOpportunities.total == 2 + 1 + 2 + 3 + 39 + 7 &&
        arenaSummary.todayOpportunities.pendingSources.size() == 1 && !arenaSummary.todayOpportunityKnown;
    if (!arenaReported) reportSummary("arena split", arenaSummary.todayOpportunities);
    ok &= require(arenaReported, "one arena field made its unreturned sibling complete");

    // Cycle 3: an overflowed sum is not a reliable value at all.
    ok &= require(startCycle(), "the overflow routine summary cycle did not start");
    deliverQueried(2, std::numeric_limits<int>::max());
    ok &= require(closeCycle(), "the overflow routine summary cycle did not complete");
    for (const QString& group : queried)
      ok &= require(acceptPeriod(group, QStringLiteral("activity")),
                    qPrintable(QStringLiteral("period evidence rejected for %1").arg(group)));
    const AccountAssetOverview overflowSummary = analyzer.routineSummary();
    const bool overflowReported =
        overflowSummary.todayOpportunities.completeness == RoutineCompleteness::Overflow &&
        !overflowSummary.todayOpportunityKnown && overflowSummary.todayOpportunities.total == 0;
    if (!overflowReported) reportSummary("overflow", overflowSummary.todayOpportunities);
    ok &= require(overflowReported, "an overflowed period total was presented as a usable value");

    // Switching the account clears the previous account's coverage.
    deliver(&summaryRepository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("routine-summary-2")}}}});
    const AccountAssetOverview switchedSummary = analyzer.routineSummary();
    ok &= require(switchedSummary.todayOpportunities.expectedSources == 0 &&
                      switchedSummary.todayOpportunities.completeness == RoutineCompleteness::Unknown &&
                      !switchedSummary.routineDataKnown,
                  "another account inherited the previous routine coverage");
  }
  return ok ? 0 : 2;
}
