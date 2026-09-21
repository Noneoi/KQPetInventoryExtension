#include "application/common/observation_freshness.h"
#include "application/shop/shop_exchange_controller.h"
#include "application/routine/routine_overview_controller.h"
#include "domain/prepared_shop_conditions.h"
#include "support/protocol_test_support.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <limits>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) {
  if (!valid) std::fprintf(stderr, "FAIL: %s\n", message);
  return valid;
}
QDateTime instant() { return QDateTime(QDate(2026, 9, 9), QTime(10, 0), Qt::UTC); }
ObservationClockSample sample(qint64 mono = 1000) { return {instant(), mono, 0, QStringLiteral("synthetic/UTC")}; }
TrustedObservationValidity proof(const QString& account, quint64 epoch, const QString& group,
    quint64 sequence, const QString& period, int durationMs = 1000) {
  return {account, epoch, group, sequence, period, QStringLiteral("synthetic-period-A"),
      instant().addSecs(-3600), instant().addMSecs(durationMs), instant(), 0,
      QStringLiteral("tests/observation_freshness_smoke.cpp synthetic server-period evidence")};
}
ShopExchangeGood good() {
  ShopExchangeGood value;
  value.shopId = 1; value.itemServerId = 1; value.limitKey = QStringLiteral("dl"); value.limitCount = 3;
  value.cost = QStringLiteral("4:100:1"); value.raceIds = {7001}; value.enhanceType = QStringLiteral("41");
  return value;
}
PreparedShopGoodConditions prepared(const ShopExchangeGood& value, const ShopConditionContext& context, int used = 3) {
  const auto result = PreparedShopConditions::prepare(CompiledShopCatalog::compile({value}),
      {{QStringLiteral("si1"), QJsonObject{{QStringLiteral("bi1"), QJsonObject{{value.limitKey, used}}}}}},
      AccountResourceView({{QStringLiteral("4:100"), 100}}, true), context);
  return result.goods().front();
}
ShopCondition condition(const ObservationValidity& value) {
  return {value.current() ? ShopConditionState::Satisfied : ShopConditionState::Unknown,
      value.reason, value.evidenceReference, value.observedAtUtc,
      value.current() ? ShopConditionFreshness::Current : value.state == ObservationValidityState::Invalidated
          ? ShopConditionFreshness::Invalidated : ShopConditionFreshness::Unknown};
}
bool periodAndDomainTest() {
  bool ok = true;
  auto clock = sample();
  ObservationFreshness freshness([&] { return clock; });
  freshness.bindSession(QStringLiteral("A"), 1);
  freshness.observe(QStringLiteral("si1"), 7, clock.monotonicMs);
  ShopConditionContext context;
  context.shopFreshness = ShopConditionFreshness::Current;
  const auto unknown = prepared(good(), context);
  ok &= check(unknown.account.limitCondition.effectiveState() == ShopConditionState::Unknown && !unknown.excluded &&
                  freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).state == ObservationValidityState::Unknown,
              "unknown period inherited Current from login or excluded an observed exhausted item");
  const auto authority = proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 7, QStringLiteral("dl"));
  ok &= check(freshness.acceptValidityEvidence(authority), "explicit synthetic period evidence was rejected");
  ok &= check(freshness.acceptValidityEvidence(proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 7,
                  QStringLiteral("wl"), 7 * 24 * 3600 * 1000)), "independent weekly period evidence was rejected");
  context.quotaValidity.insert(QStringLiteral("si1:dl"), condition(freshness.status(QStringLiteral("si1"), QStringLiteral("dl"))));
  context.quotaValidity.insert(QStringLiteral("si1:wl"), condition(freshness.status(QStringLiteral("si1"), QStringLiteral("wl"))));
  auto weekly = good(); weekly.limitKey = QStringLiteral("wl");
  const auto frozen = context;
  const quint64 capturedRevision = freshness.revision();
  ok &= check(prepared(good(), context).excluded, "current exhausted quota lost its valid exclusion");
  clock.utc = clock.utc.addMSecs(999); clock.monotonicMs += 999;
  freshness.check();
  ok &= check(freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).current(), "period expired before its exact boundary");
  clock.utc = clock.utc.addMSecs(1); ++clock.monotonicMs;
  ok &= check(freshness.check() && freshness.revision() != capturedRevision,
              "publication check did not see a period crossed during calculation");
  context.quotaValidity.insert(QStringLiteral("si1:dl"), condition(freshness.status(QStringLiteral("si1"), QStringLiteral("dl"))));
  ok &= check(freshness.status(QStringLiteral("si1"), QStringLiteral("wl")).current() && prepared(weekly, context).excluded,
              "one expired quota period invalidated an independently verified weekly quota");
  ok &= check(!prepared(good(), context).excluded && prepared(good(), context).account.remainingCount == -1 &&
                  frozen.quotaValidity.value(QStringLiteral("si1:dl")).state == ShopConditionState::Satisfied,
              "expired old exhaustion still excluded an item or mutated the captured immutable context");
  auto unlimited = good(); unlimited.provenUnlimited = true;
  ok &= check(prepared(unlimited, context).account.limitCondition.effectiveState() == ShopConditionState::Satisfied &&
                  prepared(unlimited, context).account.remainingCount == std::numeric_limits<int>::max(),
              "unlimited quota incorrectly depended on period evidence");
  freshness.bindSession(QStringLiteral("A"), 2);
  ok &= check(!freshness.acceptValidityEvidence(authority) && freshness.observationCount() == 0 && freshness.evidenceCount() == 0,
              "same account with a new epoch reused old period authority");
  freshness.observe(QStringLiteral("si1"), 8, clock.monotonicMs);
  auto renewed = proof(QStringLiteral("A"), 2, QStringLiteral("si1"), 8, QStringLiteral("dl"), 2000);
  renewed.periodId = QStringLiteral("synthetic-period-B");
  renewed.validFromUtc = renewed.observedServerUtc = clock.utc;
  ok &= check(freshness.acceptValidityEvidence(renewed) && freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).current(),
              "a new verified observation and epoch could not establish a new independent period");
  return ok;
}
bool clockAndEvidenceTest() {
  bool ok = true;
  {
    auto clock = sample();
    ObservationFreshness freshness([&] { return clock; });
    freshness.bindSession(QStringLiteral("A"), 1);
    freshness.observe(QStringLiteral("si1"), 1, clock.monotonicMs);
    clock.utc = clock.utc.addSecs(24 * 3600); clock.monotonicMs += 24 * 3600 * 1000;
    freshness.check();
    ok &= check(freshness.evidenceCount() == 0 && freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).state == ObservationValidityState::Unknown &&
                    freshness.observationSequence(QStringLiteral("si1")) == 1,
                "crossing a wall-clock date invented a server period or reset its observation");
  }
  for (int event = 0; event < 5; ++event) {
    auto clock = sample();
    ObservationFreshness freshness([&] { return clock; });
    freshness.bindSession(QStringLiteral("A"), 1);
    freshness.observe(QStringLiteral("si1"), 1, clock.monotonicMs);
    ok &= check(freshness.acceptValidityEvidence(proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 1, QStringLiteral("dl"), 60000)),
                "clock-event fixture did not establish period authority");
    if (event == 0) clock.utc = clock.utc.addSecs(-1);
    if (event == 1) clock.utc = clock.utc.addSecs(1);
    if (event == 2) --clock.monotonicMs;
    if (event == 3) clock.zoneId = QStringLiteral("synthetic/other-zone");
    if (event == 4) clock.utc = {};
    ok &= check(freshness.check() && freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).state == ObservationValidityState::Invalidated,
                "wall/monotonic/timezone/invalid-clock event retained an old Current quota");
  }
  {
    auto clock = sample(); clock.utc = clock.utc.addSecs(6 * 3600); // stable, wrong local wall clock
    ObservationFreshness freshness([&] { return clock; });
    freshness.bindSession(QStringLiteral("A"), 1);
    freshness.observe(QStringLiteral("si1"), 1, 1000);
    auto value = proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 1, QStringLiteral("dl"));
    auto missingServer = value; missingServer.observedServerUtc = {};
    ok &= check(!freshness.acceptValidityEvidence(missingServer) && freshness.acceptValidityEvidence(value),
                "local UTC replaced a missing server anchor or a valid server anchor used the wrong local offset");
    clock.utc = clock.utc.addMSecs(1000); clock.monotonicMs += 1000;
    freshness.check();
    ok &= check(!freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).current(),
                "expiry followed stable-but-wrong local wall time instead of the verified server anchor");
  }
  {
    auto clock = sample(21000); clock.utc = clock.utc.addSecs(20);
    ObservationFreshness freshness([&] { return clock; });
    freshness.bindSession(QStringLiteral("A"), 1);
    freshness.observe(QStringLiteral("si1"), 1, 1000); // queue delay must not extend expiry
    ok &= check(!freshness.acceptValidityEvidence(proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 1, QStringLiteral("dl"))),
                "late Core acceptance restarted the period clock");
  }
  {
    auto clock = sample();
    ObservationFreshness freshness([&] { return clock; });
    freshness.bindSession(QStringLiteral("A"), 1);
    freshness.observe(QStringLiteral("si1"), 1, 1000);
    auto value = proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 1, QStringLiteral("dl"));
    auto bad = value; bad.serverUncertaintyMs = -1;
    ok &= check(!freshness.acceptValidityEvidence(bad), "missing server uncertainty was silently zero");
    bad = value; bad.validUntilUtc = bad.observedServerUtc;
    ok &= check(!freshness.acceptValidityEvidence(bad), "half-open period accepted its end instant");
    bad = value; ++bad.observationSequence;
    ok &= check(!freshness.acceptValidityEvidence(bad), "period evidence borrowed another observation's sequence");
    ok &= check(freshness.acceptValidityEvidence(value), "valid evidence fixture failed");
    value.periodId = QStringLiteral("conflicting-period");
    ok &= check(!freshness.acceptValidityEvidence(value) &&
                    freshness.status(QStringLiteral("si1"), QStringLiteral("dl")).state == ObservationValidityState::Invalidated,
                "conflicting period identity silently replaced an observation's authority");
  }
  {
    auto clock = sample(std::numeric_limits<qint64>::max() - 1);
    ObservationFreshness freshness([&] { return clock; });
    freshness.bindSession(QStringLiteral("A"), 1);
    freshness.observe(QStringLiteral("si1"), 1, clock.monotonicMs);
    ok &= check(!freshness.acceptValidityEvidence(proof(QStringLiteral("A"), 1, QStringLiteral("si1"), 1, QStringLiteral("dl"))),
                "monotonic expiry arithmetic overflowed");
  }
  return ok;
}
bool controllerTest() {
  bool ok = true;
  QTemporaryDir root;
  qputenv("KQPET_DATA_ROOT", root.path().toUtf8());
  PetRepository repository;
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("synthetic-A")}}}});
  auto clock = sample();
  ShopExchangeController shop(&repository);
  RoutineOverviewController routine(&repository);
  shop.setObservationClock([&] { return clock; });
  routine.setObservationClock([&] { return clock; });
  QObject::connect(&repository, &PetRepository::packetObserved, &shop,
      [&](const QJsonObject& packet, const InboundEnvelope& envelope) { shop.handleDecodedEnvelope(envelope, packet); });
  QObject::connect(&repository, &PetRepository::packetObserved, &routine,
      [&](const QJsonObject& packet, const InboundEnvelope& envelope) { routine.handleDecodedEnvelope(envelope, packet); });
  int sends = 0;
  const auto sender = [&](const QString&, const QString&, const QString&) { ++sends; return true; };
  shop.setSender(sender); routine.setSender(sender);
  const auto deliver = [&](const QJsonObject& packet) {
    auto envelope = verifiedFixtureEnvelope(&repository, packet);
    envelope.receivedMonotonicMs = clock.monotonicMs;
    repository.handleEnvelope(envelope);
  };
  shop.requestInfo();
  QJsonObject packet{{QStringLiteral("_cmd"), ShopExchangeCatalog::instance().getInfoCommand()}};
  for (const auto& definition : ShopExchangeCatalog::instance().snapshot()->allShops)
    packet.insert(QStringLiteral("si%1").arg(definition.shopId), QJsonObject{{QStringLiteral("bi1"), QJsonObject{{QStringLiteral("dl"), 3}}}});
  deliver(packet);
  deliver({{QStringLiteral("_cmd"), QStringLiteral("3_11")}, {QStringLiteral("4"), QJsonArray{
      QJsonObject{{QStringLiteral("i"), 100}, {QStringLiteral("n"), 100}}}}, {QStringLiteral("8"), QJsonArray{}}});
  deliver({{QStringLiteral("_cmd"), QStringLiteral("1015_2A")}, {QStringLiteral("r"), 1},
      {QStringLiteral("infos"), QJsonObject{{QStringLiteral("UnionMemberInfo"),
          QJsonObject{{QStringLiteral("lCToken"), 9}}}}}});
  ok &= check(shop.hasObservedPacket() && shop.quotaValiditySnapshot().value(QStringLiteral("si1:dl")).state == ShopConditionState::Unknown,
              "ordinary trusted shop observation was promoted into verified period authority");
  auto shopProof = proof(repository.accountKey(), repository.sessionGeneration(), QStringLiteral("si1"), shop.observedSequence(QStringLiteral("si1")), QStringLiteral("dl"));
  ok &= check(shop.acceptQuotaValidityEvidence(shopProof) &&
                  shop.quotaValiditySnapshot().value(QStringLiteral("si1:dl")).state == ShopConditionState::Satisfied,
              "controller failed to bind explicit period evidence to its accepted shop group");
  routine.requestRefresh();
  deliver({{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")}, {QStringLiteral("av"), 20},
      {QStringLiteral("wav"), 30}, {QStringLiteral("bi"), QJsonArray{}}, {QStringLiteral("wbi"), QJsonArray{}},
      {QStringLiteral("ti"), QJsonArray{}}, {QStringLiteral("wti"), QJsonArray{}}, {QStringLiteral("wdti"), QJsonArray{}}});
  ok &= check(routine.periodValiditySnapshot().value(QStringLiteral("av:daily")).state == ObservationValidityState::Unknown,
              "daily field names guessed an unproven reset boundary");
  auto routineProof = proof(repository.accountKey(), repository.sessionGeneration(), QStringLiteral("av"), routine.observedSequence(QStringLiteral("av")), QStringLiteral("daily"));
  ok &= check(routine.acceptPeriodValidityEvidence(routineProof), "explicit routine period evidence was rejected");
  const int beforeBoundary = sends;
  clock.utc = clock.utc.addMSecs(1000); clock.monotonicMs += 1000;
  ok &= check(shop.checkFreshness() && routine.checkFreshness() && sends == beforeBoundary &&
                  shop.quotaValiditySnapshot().value(QStringLiteral("si1:dl")).state == ShopConditionState::Unknown &&
                  routine.periodValiditySnapshot().value(QStringLiteral("av:daily")).state == ObservationValidityState::Invalidated &&
                  routine.dailyPacket().value(QStringLiteral("av")).toInt() == 20 &&
                  shop.packet().value(QStringLiteral("si1")).toObject().value(QStringLiteral("bi1")).toObject().value(QStringLiteral("dl")).toInt() == 3,
              "expiry queried/reset the server, erased read-only observations or left quota Current");
  const auto oldEpoch = repository.sessionGeneration();
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("synthetic-A")}}}});
  ok &= check(repository.sessionGeneration() != oldEpoch && !shop.acceptQuotaValidityEvidence(shopProof) && !routine.acceptPeriodValidityEvidence(routineProof),
              "same-account authentication epoch retained old period evidence");
  return ok;
}
bool boundedFactsTest() {
  auto clock = sample();
  ObservationFreshness freshness([&] { return clock; });
  freshness.bindSession(QStringLiteral("A"), 1);
  bool ok = true;
  for (int group = 0; group < 128; ++group)
    ok &= check(freshness.observe(QStringLiteral("group-%1").arg(group), 1, clock.monotonicMs), "bounded observation fixture failed");
  ok &= check(!freshness.observe(QStringLiteral("overflow"), 1, clock.monotonicMs) && freshness.observationCount() == 128,
              "observation cache exceeded its fixed count bound");
  for (int group = 0; group < 128; ++group)
    for (int period = 0; period < 4; ++period)
      ok &= check(freshness.acceptValidityEvidence(proof(QStringLiteral("A"), 1, QStringLiteral("group-%1").arg(group), 1,
                      QStringLiteral("period-%1").arg(period))), "bounded period fixture failed");
  ok &= check(freshness.evidenceCount() == 512 &&
                  !freshness.acceptValidityEvidence(proof(QStringLiteral("A"), 1, QStringLiteral("group-0"), 1, QStringLiteral("overflow"))),
              "period evidence cache exceeded its fixed count bound");
  freshness.observe(QStringLiteral("group-0"), 2, clock.monotonicMs);
  ok &= check(freshness.evidenceCount() == 508 && !freshness.status(QStringLiteral("group-0"), QStringLiteral("period-0")).current(),
              "replacement observation accumulated or reused the old period evidence");
  return ok;
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  bool ok = periodAndDomainTest();
  ok &= clockAndEvidenceTest();
  ok &= controllerTest();
  ok &= boundedFactsTest();
  if (ok) std::puts("PASS: unknown/verified/expired periods, server clock anchors, epoch guards, clock changes and no auto-query");
  return ok ? 0 : 1;
}
