#include "catalog_test_support.h"
#include "protocol_test_support.h"
#include "shop_exchange_catalog.h"
#include "shop_exchange_controller.h"
#include "domain/activity_shop_observation.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

namespace {
const QString kA = QStringLiteral("fixture/a-es10#TABLE");
const QString kB = QStringLiteral("fixture/b-es9#TABLE");
const QString kC = QStringLiteral("fixture/c-simple#TABLE");
const QString kBase = QStringLiteral("1008_20260313_es_0");
const QString kShared = QStringLiteral("1008_20260313_es_2");
const QString kSimple = QStringLiteral("1019_0");

bool require(bool value, const char* message) {
  if (!value) std::fprintf(stderr,"FAIL: %s\n",message);
  return value;
}
template<class Predicate> bool until(Predicate predicate) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents,5);
    QThread::msleep(1);
  }
  return predicate();
}
QJsonObject request(const QString& command,const QJsonValue& params,const QString& extension) {
  return {{QStringLiteral("key"),QStringLiteral("state")},{QStringLiteral("command"),command},
      {QStringLiteral("params"),params},{QStringLiteral("extension"),extension},
      {QStringLiteral("requiredFields"),QJsonArray{command == kSimple ? QStringLiteral("b0") : QStringLiteral("bi0")}}};
}
QJsonObject item(int id,bool activity,bool simple = false) {
  QJsonObject result{{QStringLiteral("itemServerId"),id},{QStringLiteral("description"),QStringLiteral("合成官方形状测试项目")},
      {QStringLiteral("cost"),QStringLiteral("4:500:1")},{QStringLiteral("enhanceType"),QStringLiteral("62")},
      {QStringLiteral("unlock"),QString()},{QStringLiteral("shelfTime"),QStringLiteral("20260101")},
      {QStringLiteral("raceIds"),QJsonArray{7001}},{QStringLiteral("limitKey"),QStringLiteral("pl")},
      {QStringLiteral("limitCount"),10}};
  if (activity) result.insert(QStringLiteral("quotaObservation"),QJsonObject{
      {QStringLiteral("requestKey"),QStringLiteral("state")},{QStringLiteral("valueKind"),QStringLiteral("used")},
      {QStringLiteral("path"),simple ? QJsonArray{QStringLiteral("b0"),QStringLiteral("b0")}
                                    : QJsonArray{QStringLiteral("bi0"),QStringLiteral("pl")}}});
  return result;
}
QJsonObject activity(const QString& source,const QJsonObject& read,bool simple = false) {
  QJsonArray requests{read};
  if (simple) {
    // An unsupported catalogue action must never turn into an outbound write.
    auto write = read; write.insert(QStringLiteral("key"),QStringLiteral("unapproved-write"));
    write.insert(QStringLiteral("command"),QStringLiteral("1019_1")); requests.append(write);
  }
  return {{QStringLiteral("shopId"),1},{QStringLiteral("sourceKey"),source},{QStringLiteral("name"),source},
      {QStringLiteral("goods"),QJsonArray{item(0,true,simple)}},
      {QStringLiteral("observation"),QJsonObject{{QStringLiteral("schema"),1},{QStringLiteral("requests"),requests}}}};
}
QJsonObject catalogFixture() {
  const auto timeliness = QStringLiteral("TimelinessActExtension");
  return {{QStringLiteral("schema"),2},{QStringLiteral("protocol"),QJsonObject{
      {QStringLiteral("extension"),timeliness},{QStringLiteral("getInfoCommand"),kBase},
      {QStringLiteral("getInfoParams"),QJsonObject{}}}},
      {QStringLiteral("shops"),QJsonArray{
          QJsonObject{{QStringLiteral("shopId"),1},{QStringLiteral("name"),QStringLiteral("常驻合成目录")},
              {QStringLiteral("goods"),QJsonArray{item(1,false)}}},
          activity(kA,request(kShared,QJsonObject{{QStringLiteral("i"),10}},timeliness)),
          activity(kB,request(kShared,QJsonObject{{QStringLiteral("i"),9}},timeliness)),
          activity(kC,request(kSimple,QJsonObject{{QStringLiteral("ai"),5735}},QStringLiteral("SimpleActExtension")),true)}}};
}
QJsonObject response(const QString& command,const QJsonValue& value,int ai = 5735) {
  return command == kSimple ? QJsonObject{{QStringLiteral("_cmd"),command},{QStringLiteral("ai"),ai},
      {QStringLiteral("b0"),QJsonObject{{QStringLiteral("b0"),value}}}}
      : QJsonObject{{QStringLiteral("_cmd"),command},{QStringLiteral("bi0"),QJsonObject{{QStringLiteral("pl"),value}}}};
}
QJsonObject state(const ShopExchangeController& controller,const QString& source) {
  return controller.packet().value(QStringLiteral("_activities")).toObject().value(source).toObject().value(QStringLiteral("state")).toObject();
}
QJsonObject stateData(const ShopExchangeController& controller,const QString& source) {
  return state(controller,source).value(QStringLiteral("data")).toObject();
}
ShopExchangeGood sourceGood(const QString& source) {
  for (const auto& shop : ShopExchangeCatalog::instance().snapshot()->allShops)
    if (shop.sourceKey == source && !shop.goods.isEmpty()) return shop.goods.first();
  return {};
}
QByteArray readFile(const QString& path) {
  QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
void login(PetRepository& repository,const QString& account) {
  repository.handlePacket(QStringLiteral("recivedata"),QString::fromUtf8(QJsonDocument(QJsonObject{
      {QStringLiteral("_cmd"),QStringLiteral("21_1")},{QStringLiteral("info"),QJsonObject{{QStringLiteral("n"),account}}}}).toJson(QJsonDocument::Compact)));
}
struct Sent { QString extension,command,parameters; };
}

int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;
  QTemporaryDir temporary;
  StorageService storage(temporary.path());
  CatalogIoService catalogs(&storage);
  const auto path = QDir(temporary.path()).filePath(QStringLiteral("catalog/shop-exchange-data.json"));
  ok &= require(writeCatalogFixture(path,QJsonDocument(catalogFixture()).toJson()) &&
      runCatalogRequest(&catalogs,CatalogKind::Shop,CatalogRequestMode::Reload),
      "synthetic activity catalogue did not load through CatalogIo");
  ok &= require(ShopExchangeCatalog::instance().protocolShops().size() == 1 &&
      ShopExchangeCatalog::instance().shops().size() == 4,"fixture source partitions were not preserved");

  PetRepository repository(nullptr,&storage);
  login(repository,QStringLiteral("activity-controller-A"));
  ok &= require(repository.isAuthenticated() && !repository.sessionContext().canPersist(),
      "fixture must model a read-only identified account");
  ShopExchangeController controller(&repository);
  QList<Sent> sent;
  QString status;
  controller.setSender([&](const QString& extension,const QString& command,const QString& parameters) {
    sent.append({extension,command,parameters}); return true;
  });
  QObject::connect(&controller,&ShopExchangeController::statusChanged,&controller,[&](const QString& value) { status = value; });
  quint64 sequence = 100;
  const auto deliver = [&](ShopExchangeController& target,const QJsonObject& packet,quint64 epoch = 0) {
    InboundEnvelope envelope; envelope.receiveSequence = ++sequence;
    envelope.receivedMonotonicMs = transportMonotonicMs(); envelope.capturedSessionEpoch = epoch;
    if (repository.sessionContext().canPersist()) {
      // After the explicit synthetic source transition below, subsequent
      // packets must use that source; an empty envelope must not borrow it.
      envelope.source = repository.sessionContext().source;
      if (!epoch) envelope.capturedSessionEpoch = repository.sessionGeneration();
      envelope.orderedObservation = true;
      envelope.orderEvidenceToken = QStringLiteral("activity-controller-fixture-%1").arg(sequence);
    }
    target.handleDecodedEnvelope(envelope,packet);
  };
  const auto baseReplies = [&](ShopExchangeController& target) {
    deliver(target,{{QStringLiteral("_cmd"),kBase},{QStringLiteral("si1"),QJsonObject{
        {QStringLiteral("b1"),QJsonObject{{QStringLiteral("pl"),0}}}}}});
    deliver(target,{{QStringLiteral("_cmd"),QStringLiteral("3_11")},{QStringLiteral("4"),QJsonArray{}}});
  };
  ok &= require(until([&] { return !controller.cacheLoading(); }) && sent.isEmpty(),
      "loading an activity cache sent a game query");
  deliver(controller,response(kShared,9));
  ok &= require(state(controller,kA).isEmpty(),"an unrequested activity response became an account observation");

  ok &= require(controller.requestInfo() && sent.size() == 2,"base info/material requests did not start");
  baseReplies(controller);
  ok &= require(until([&] { return sent.size() == 3; }) && sent[2].command == kShared &&
      QJsonDocument::fromJson(sent[2].parameters.toUtf8()).object().value(QStringLiteral("i")).toInt() == 10,
      "first activity request did not follow the completed base pair");
  QCoreApplication::processEvents(QEventLoop::AllEvents,5);
  ok &= require(sent.size() == 3 && state(controller,kB).isEmpty(),"same-command activity groups were sent concurrently");
  deliver(controller,response(kShared,1));
  ok &= require(until([&] { return sent.size() == 4; }) && sent[3].command == kShared &&
      QJsonDocument::fromJson(sent[3].parameters.toUtf8()).object().value(QStringLiteral("i")).toInt() == 9,
      "second shared-command group did not wait for the first response");
  deliver(controller,response(kShared,4));
  ok &= require(until([&] { return sent.size() == 5; }) && sent[4].command == kSimple,
      "the independent simple-activity query did not follow the second shared group");
  deliver(controller,response(kSimple,99,5747));
  ok &= require(controller.isRunning() && sent.size() == 5 && state(controller,kC).isEmpty(),
      "a wrong simple-activity ai completed or overwrote the pending group");
  deliver(controller,response(kSimple,2));
  ok &= require(until([&] { return !controller.isRunning(); }) && sent.size() == 5 &&
      observeActivityShopGood(sourceGood(kA),controller.packet()).used == 1 &&
      observeActivityShopGood(sourceGood(kB),controller.packet()).used == 4 &&
      observeActivityShopGood(sourceGood(kC),controller.packet()).used == 2,
      "valid independent activity quotas were lost or mixed by local item IDs");
  for (const auto& request : sent) {
    const auto* contract = PacketContracts::find(request.command);
    ok &= require(contract && contract->access == PacketAccess::Read &&
        PacketContracts::validateOutbound(request.extension,request.command,request.parameters),
        "activity refresh submitted an unregistered or writing command");
  }
  ok &= require(!repository.sessionContext().canPersist() && controller.cultivationMaterialInventory().counts.isEmpty(),
      "activity observations elevated write authority or changed the separately manual cultivation inventory");
  ok &= require(until([&] { return controller.pendingStorageCount() == 0; }),"activity observations did not settle to disk");
  const auto accountAPath = QDir(repository.storageContext()->directory()).filePath(QStringLiteral("activity-exchanges.json"));
  ok &= require(QFile::exists(accountAPath) && !readFile(accountAPath).isEmpty(),"independent activity cache was not persisted");
  const auto beforeA = state(controller,kA); const auto beforeC = state(controller,kC);

  sent.clear();
  ok &= require(controller.requestInfo(),"malformed-response refresh did not start");
  baseReplies(controller);
  ok &= require(until([&] { return sent.size() == 3; }),"malformed-response first activity did not start");
  deliver(controller,response(kShared,QJsonValue(QJsonValue::Null)));
  ok &= require(until([&] { return sent.size() == 4; }),"invalid first source blocked a sibling source");
  deliver(controller,response(kShared,5));
  ok &= require(until([&] { return sent.size() == 5; }),"malformed-response simple activity did not start");
  deliver(controller,response(kSimple,-3));
  ok &= require(until([&] { return !controller.isRunning(); }) &&
      state(controller,kA).value(QStringLiteral("data")) == beforeA.value(QStringLiteral("data")) &&
      state(controller,kA).value(QStringLiteral("observedAt")) == beforeA.value(QStringLiteral("observedAt")) &&
      state(controller,kC).value(QStringLiteral("data")) == beforeC.value(QStringLiteral("data")) &&
      state(controller,kC).value(QStringLiteral("observedAt")) == beforeC.value(QStringLiteral("observedAt")) &&
      observeActivityShopGood(sourceGood(kB),controller.packet()).used == 5,
      "null/invalid counters replaced valid history or cancelled another source's valid observation");
  ok &= require(until([&] { return controller.pendingStorageCount() == 0; }),"partial update did not settle");
  const auto savedA = readFile(accountAPath);
  ShopExchangeController historicalReader(&repository);
  ok &= require(until([&] { return !historicalReader.cacheLoading(); }) &&
      observeActivityShopGood(sourceGood(kA),historicalReader.packet()).used == 1 &&
      observeActivityShopGood(sourceGood(kA),historicalReader.packet()).historical &&
      state(historicalReader,kA).value(QStringLiteral("verified")).toBool(true) == false,
      "independent activity cache did not load as historical unverified quantities");

  // Switch while a shared-command request is in flight; the old epoch must
  // not seed the next account's cache even if its late command name matches.
  sent.clear(); ok &= require(controller.requestInfo(),"account-switch fixture did not start");
  baseReplies(controller);
  ok &= require(until([&] { return sent.size() == 3; }),"account-switch activity did not become pending");
  const auto oldEpoch = repository.sessionGeneration();
  // A second weak 21_1 on the same unverified stream cannot authorize an
  // account change. Establish the new synthetic transport source explicitly.
  deliverVerifiedFixture(&repository,{{QStringLiteral("_cmd"),QStringLiteral("21_1")},
      {QStringLiteral("info"),QJsonObject{{QStringLiteral("n"),QStringLiteral("activity-controller-B")}}}});
  const bool switched = repository.isAuthenticated() &&
      repository.accountKey() == QStringLiteral("activity-controller-B") && repository.sessionGeneration() != oldEpoch;
  if (!require(switched,"fixture did not establish authenticated account B in a new source session")) {
    catalogs.close(); storage.shutdown(); return 1;
  }
  if (!require(!controller.isRunning() && controller.packet().value(QStringLiteral("_activities")).toObject().isEmpty(),
      "account change kept another account's activity data or active request")) {
    catalogs.close(); storage.shutdown(); return 1;
  }
  ok &= require(until([&] { return !controller.cacheLoading() && !historicalReader.cacheLoading(); }),"new account cache reads did not settle");
  sent.clear(); ok &= require(controller.requestInfo(),"new account refresh did not start");
  baseReplies(controller);
  ok &= require(until([&] { return sent.size() == 3; }),"new account first activity did not start");
  deliver(controller,response(kShared,99),oldEpoch);
  ok &= require(controller.isRunning() && state(controller,kA).isEmpty() && sent.size() == 3,
      "late old-account activity response seeded the new account");
  deliver(controller,response(kShared,8));
  ok &= require(until([&] { return sent.size() == 4; }),"new account second activity did not start");
  deliver(controller,response(kShared,7));
  ok &= require(until([&] { return sent.size() == 5; }),"new account simple activity did not start");
  deliver(controller,response(kSimple,6));
  ok &= require(until([&] { return !controller.isRunning() && controller.pendingStorageCount() == 0; }) &&
      readFile(accountAPath) == savedA,"the new account mutated the prior account's independent activity cache");
  const auto accountBPath = QDir(repository.storageContext()->directory()).filePath(QStringLiteral("activity-exchanges.json"));
  ok &= require(accountBPath != accountAPath && QFile::exists(accountBPath),"activity caches did not remain account-separated");

  ShopExchangeController timed(&repository);
  QList<OutboundIntent> intents;
  int fallbackCalls = 0;
  QString timedStatus;
  timed.setSender([&](const QString&,const QString&,const QString&) { ++fallbackCalls; return false; });
  timed.setAsyncSender([&](const OutboundIntent& intent) { intents.append(intent); return true; });
  QObject::connect(&timed,&ShopExchangeController::statusChanged,&timed,[&](const QString& value) { timedStatus = value; });
  ok &= require(until([&] { return !timed.cacheLoading(); }) && timed.requestInfo() && intents.size() == 2,
      "async timeout fixture did not start the base request pair");
  for (int index = 0; index < qMin(2, int(intents.size())); ++index) {
    intents[index].permit.claim(transportMonotonicMs());
    intents[index].permit.complete(SubmissionOutcome::Submitted);
  }
  baseReplies(timed);
  ok &= require(until([&] { return intents.size() == 3; }) && intents[2].ticket.command == kShared,
      "async first shared-command group did not become pending");
  if (intents.size() >= 3) {
    const auto first = intents[2];
    timed.handleSendReceipt({first.ticket.taskId,first.ticket.account,first.ticket.sessionEpoch,first.ticket.command,
        SubmissionOutcome::Submitted,transportMonotonicMs()-first.ticket.responseTimeoutMs-1});
  }
  ok &= require(until([&] { return intents.size() == 4; }) && intents[3].ticket.command == kSimple,
      "timed-out shared command did not skip its remaining groups while continuing independent commands");
  int sharedCount = 0;
  for (const auto& intent : intents) if (intent.ticket.command == kShared) ++sharedCount;
  ok &= require(sharedCount == 1 && fallbackCalls == 0,
      "timeout retried the same shared command, sent its next group, or fell back to a second transport");
  deliver(timed,response(kShared,99));
  ok &= require(observeActivityShopGood(sourceGood(kA),timed.packet()).used == 8 &&
      observeActivityShopGood(sourceGood(kB),timed.packet()).used == 7,
      "a late shared-command response was attributed to a skipped activity source");
  if (intents.size() >= 4) {
    intents[3].permit.claim(transportMonotonicMs()); intents[3].permit.complete(SubmissionOutcome::Submitted);
  }
  deliver(timed,response(kSimple,1));
  ok &= require(until([&] { return !timed.isRunning() && timed.pendingStorageCount() == 0; }) &&
      observeActivityShopGood(sourceGood(kA),timed.packet()).historical &&
      observeActivityShopGood(sourceGood(kC),timed.packet()).used == 1 &&
      timedStatus.contains(QStringLiteral("超时")) && timedStatus.contains(QStringLiteral("留待下次")),
      "partial activity timeout lost history, a successful sibling or its explicit status");

  // A negative result code is a server refusal: the activity exists but this
  // account may not read it (the official unlock flag gates return-player
  // activities). It carries no counters, so the previous observation must
  // survive, the status must name the code instead of reporting a parse
  // failure, and that source must not be asked again in the same session.
  {
    const int beforeFirst = sent.size();
    ok &= require(controller.requestInfo() && sent.size() == beforeFirst + 2,
        "refusal fixture did not start its base requests");
    baseReplies(controller);
    ok &= require(until([&] { return sent.size() == beforeFirst + 3; }) &&
        sent[beforeFirst + 2].command == kShared &&
        QJsonDocument::fromJson(sent[beforeFirst + 2].parameters.toUtf8()).object()
                .value(QStringLiteral("i")).toInt() == 10,
        "refusal fixture did not reach the first activity read");
    const auto keptBefore = stateData(controller,kA);
    const int keptUsed = observeActivityShopGood(sourceGood(kA),controller.packet()).used;
    deliver(controller,{{QStringLiteral("_cmd"),kShared},{QStringLiteral("r"),-2}});    ok &= require(until([&] { return sent.size() == beforeFirst + 4; }) &&
        sent[beforeFirst + 3].command == kShared &&
        QJsonDocument::fromJson(sent[beforeFirst + 3].parameters.toUtf8()).object()
                .value(QStringLiteral("i")).toInt() == 9,
        "a refused group stopped the remaining independent activity groups");
    deliver(controller,response(kShared,2));
    ok &= require(until([&] { return sent.size() == beforeFirst + 5; }) &&
        sent[beforeFirst + 4].command == kSimple,
        "a refused group stopped the independent simple activity");
    deliver(controller,response(kSimple,4));
    ok &= require(until([&] { return !controller.isRunning(); }) &&
        status.contains(QStringLiteral("不适用")) && status.contains(QStringLiteral("-2")),
        "a server refusal was reported as a parse failure");
    ok &= require(stateData(controller,kA) == keptBefore &&
        observeActivityShopGood(sourceGood(kA),controller.packet()).used == keptUsed,
        "a refused activity overwrote its previous observation");
    const int beforeSecond = sent.size();
    ok &= require(controller.requestInfo() && sent.size() == beforeSecond + 2,
        "second refresh did not start after a refusal");
    baseReplies(controller);
    ok &= require(until([&] { return sent.size() == beforeSecond + 3; }) &&
        QJsonDocument::fromJson(sent[beforeSecond + 2].parameters.toUtf8()).object()
                .value(QStringLiteral("i")).toInt() == 9,
        "a server-refused activity was queried again in the same session");
    deliver(controller,response(kShared,5));
    ok &= require(until([&] { return sent.size() == beforeSecond + 4; }) &&
        sent[beforeSecond + 3].command == kSimple,"refusal retry flow lost the simple activity");
    deliver(controller,response(kSimple,6));
    ok &= require(until([&] { return !controller.isRunning(); }) &&
        observeActivityShopGood(sourceGood(kA),controller.packet()).used == keptUsed,
        "the second refresh changed a refused activity's stored observation");
  }

  ok &= require(until([&] { return controller.pendingStorageCount() == 0 && historicalReader.pendingStorageCount() == 0 &&
      timed.pendingStorageCount() == 0; }) && waitForRepositoryIdle(&repository),"fixture storage did not settle");
  catalogs.close();
  ok &= require(storage.shutdown(),"fixture storage did not shut down");
  if (ok) std::puts("PASS: serial activity reads, exact ai/session matching, durable history and shared-command timeout isolation");
  return ok ? 0 : 1;
}