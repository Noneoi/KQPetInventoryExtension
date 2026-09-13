#include "domain/activity_shop_observation.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <cstdio>

namespace {
bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}
QJsonObject query(const QString& command, const QJsonValue& params = QJsonValue(QJsonValue::Null),
                  const QString& extension = QStringLiteral("TimelinessActExtension")) {
  return {{QStringLiteral("key"),QStringLiteral("state")},{QStringLiteral("extension"),extension},
      {QStringLiteral("command"),command},{QStringLiteral("params"),params}};
}
QJsonObject quota(const QJsonArray& path, bool missingZero = false) {
  QJsonObject result{{QStringLiteral("requestKey"),QStringLiteral("state")},{QStringLiteral("path"),path},
      {QStringLiteral("valueKind"),QStringLiteral("used")}};
  if (missingZero) result.insert(QStringLiteral("missingValue"),0);
  return result;
}
ShopExchangeGood good(const QString& source, const QJsonObject& request, const QJsonArray& path,
                      int limit, int item = 0, bool missingZero = false) {
  ShopExchangeGood value;
  value.sourceKey = source; value.shopId = 1; value.itemServerId = item; value.limitCount = limit;
  value.activityQueries = {{QStringLiteral("state"),request}};
  value.quotaObservation = quota(path,missingZero);
  return value;
}
QJsonObject packetFor(const ShopExchangeGood& good, const QJsonObject& data,
                       bool historical = false, bool verified = true, QJsonObject previous = {}) {
  auto activities = previous.value(QStringLiteral("_activities")).toObject();
  auto source = activities.value(good.sourceKey).toObject();
  source.insert(QStringLiteral("state"),QJsonObject{
      {QStringLiteral("request"),activityRequestSignature(good.activityQueries.value(QStringLiteral("state")).toObject())},
      {QStringLiteral("data"),data},{QStringLiteral("observedAt"),QStringLiteral("2026-09-13T06:00:00.000Z")},
      {QStringLiteral("historical"),historical},{QStringLiteral("verified"),verified}});
  activities.insert(good.sourceKey,source);
  previous.insert(QStringLiteral("_activities"),activities);
  return previous;
}
QJsonObject activityCost(const QString& name, int count, const QJsonArray& path) {
  return {{QStringLiteral("name"),name},{QStringLiteral("count"),count},
      {QStringLiteral("requestKey"),QStringLiteral("state")},{QStringLiteral("path"),path}};
}
QJsonObject price(const QString& cost, const QString& op, int count) {
  return {{QStringLiteral("cost"),cost},{QStringLiteral("when"),QJsonObject{
      {QStringLiteral("requestKey"),QStringLiteral("state")},{QStringLiteral("path"),QJsonArray{QStringLiteral("bt")}},
      {QStringLiteral("op"),op},{QStringLiteral("value"),count}}}};
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;

  auto s2 = good(QStringLiteral("official/s2#EXCHANGES"),
      query(QStringLiteral("1008_20260313_es_2"),QJsonObject{{QStringLiteral("i"),10}}),
      {QStringLiteral("bi4"),QStringLiteral("pl")},1,4,true);
  s2.cost = QStringLiteral("4:3266:100");
  auto s2Packet = packetFor(s2,{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),0}}}});
  auto result = observeActivityShopGood(s2,s2Packet);
  ok &= require(result.quotaKnown && result.used == 0 && result.remaining == 1 && !result.historical &&
      result.observedAt.isValid() && result.priceKnown && result.standardCost == s2.cost,
      "S2 must use direct biN.period fields and preserve its exact static standard cost");
  result = observeActivityShopGood(s2,packetFor(s2,{}));
  ok &= require(result.quotaKnown && result.used == 0 && result.remaining == 1,
      "S2's explicitly proven missing-counter default was lost");
  for (const auto& invalid : QList<QJsonValue>{QJsonValue(QJsonValue::Null),true,-1,1.5,QStringLiteral("bad"),QJsonArray{0}}) {
    result = observeActivityShopGood(s2,packetFor(s2,{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),invalid}}}}));
    ok &= require(!result.quotaKnown && result.used == -1 && result.remaining == -1,
        "a null or invalid counter was converted to zero by missingValue");
  }
  result = observeActivityShopGood(s2,packetFor(s2,{{QStringLiteral("bi4"),QJsonValue(QJsonValue::Null)}}));
  ok &= require(!result.quotaKnown,"a null parent object was treated as a missing zero counter");
  result = observeActivityShopGood(s2,{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),1}}},
      {QStringLiteral("si1"),QJsonObject{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),1}}}}}});
  ok &= require(!result.quotaKnown,"activity quota was borrowed from unscoped or main-SEF counts");

  auto other = s2; other.sourceKey = QStringLiteral("official/other#EXCHANGES");
  auto multiple = packetFor(other,{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),1}}}},false,true,s2Packet);
  ok &= require(observeActivityShopGood(s2,multiple).remaining == 1 && observeActivityShopGood(other,multiple).remaining == 0,
      "equal item and shop IDs in independent activity sources shared quota");
  auto differentRequest = s2;
  differentRequest.activityQueries = {{QStringLiteral("state"),query(QStringLiteral("1008_20260313_es_2"),QJsonObject{{QStringLiteral("i"),9}})}};
  ok &= require(!observeActivityShopGood(differentRequest,s2Packet).quotaKnown,
      "a changed request fingerprint reused an earlier activity counter");
  auto corruptTimestamp = s2Packet;
  auto roots = corruptTimestamp.value(QStringLiteral("_activities")).toObject();
  auto records = roots.value(s2.sourceKey).toObject(); auto record = records.value(QStringLiteral("state")).toObject();
  record.insert(QStringLiteral("observedAt"),QStringLiteral("invalid")); records.insert(QStringLiteral("state"),record);
  roots.insert(s2.sourceKey,records); corruptTimestamp.insert(QStringLiteral("_activities"),roots);
  ok &= require(!observeActivityShopGood(s2,corruptTimestamp).quotaKnown,
      "an observation without a valid recorded time became current quota");

  auto r4 = good(QStringLiteral("official/reback4#EXCHANGE"),
      query(QStringLiteral("1008_20260313_es_2"),QJsonObject{{QStringLiteral("i"),9}}),
      {QStringLiteral("bi22"),QStringLiteral("tl")},10,22);
  auto returnCoin = activityCost(QStringLiteral("回归养成币"),300,{QStringLiteral("cn")});
  returnCoin.insert(QStringLiteral("encoding"),QStringLiteral("id-counts")); returnCoin.insert(QStringLiteral("itemId"),1);
  r4.activityCosts = {returnCoin};
  auto r4Packet = packetFor(r4,{{QStringLiteral("bi22"),QJsonObject{{QStringLiteral("tl"),6}}},{QStringLiteral("cn"),QStringLiteral("2:5#1:450")}});
  result = observeActivityShopGood(r4,r4Packet);
  ok &= require(result.quotaKnown && result.remaining == 4 && result.priceKnown && result.costs.size() == 1 &&
      result.costs[0].required == 300 && result.costs[0].owned == 450 && result.costs[0].ownedKnown && result.costs[0].current,
      "R4 direct total quota or encoded activity coin did not map correctly");
  result = observeActivityShopGood(r4,packetFor(r4,{{QStringLiteral("cn"),QStringLiteral("1:450")}}));
  ok &= require(!result.quotaKnown && result.costs[0].ownedKnown,
      "R4 missing biN without a proven zero default fabricated full quota");
  for (const auto& bad : QList<QJsonValue>{QJsonValue(QJsonValue::Null),QJsonArray{},QStringLiteral("1:-2"),
      QStringLiteral("1:3#1:4"),QStringLiteral("1:2##2:3"),QStringLiteral("1:2#"),QStringLiteral("1:2:3"),QStringLiteral("1:3.5")}) {
    result = observeActivityShopGood(r4,packetFor(r4,{{QStringLiteral("bi22"),QJsonObject{{QStringLiteral("tl"),1}}},{QStringLiteral("cn"),bad}}));
    ok &= require(result.costs.size() == 1 && !result.costs[0].ownedKnown && !result.costs[0].current,
        "malformed, duplicate, null or incomplete encoded coin was accepted as a balance");
  }
  result = observeActivityShopGood(r4,packetFor(r4,{{QStringLiteral("cn"),QStringLiteral("2:5")}}));
  ok &= require(!result.costs[0].ownedKnown,"an omitted activity coin without an explicit zero policy became known zero");
  auto emptyCoin = returnCoin; emptyCoin.insert(QStringLiteral("missingValue"),0);
  r4.activityCosts = {emptyCoin};
  result = observeActivityShopGood(r4,packetFor(r4,{{QStringLiteral("cn"),QString()}}));
  ok &= require(result.costs[0].ownedKnown && result.costs[0].owned == 0,
      "a valid empty encoded coin map with a proven zero policy was not known zero");
  result = observeActivityShopGood(r4,packetFor(r4,{}));
  ok &= require(!result.costs[0].ownedKnown,"missing entire coin map became zero even though no map was returned");

  auto cveg = good(QStringLiteral("official/vip#ArrayData"),query(QStringLiteral("1008_20251231_cvgv2_0")),
      {QStringLiteral("li"),5},1,8);
  cveg.activityCosts = {activityCost(QStringLiteral("V币"),30,{QStringLiteral("c")})};
  auto cvegData = QJsonObject{{QStringLiteral("li"),QJsonArray{0,0,0,0,0,1,0,0,0}},
      {QStringLiteral("c"),47},{QStringLiteral("cl"),999},{QStringLiteral("month"),9}};
  result = observeActivityShopGood(cveg,packetFor(cveg,cvegData));
  ok &= require(result.quotaKnown && result.used == 1 && result.remaining == 0 && result.costs[0].owned == 47,
      "CVEG quota used item ID instead of dataIndex or cumulative cl instead of current c");
  ok &= require(!observeActivityShopGood(cveg,s2Packet).costs[0].ownedKnown &&
      result.costs[0].key != observeActivityShopGood(r4,r4Packet).costs[0].key,
      "activity currency balances or identity keys crossed source namespaces");
  auto historical = observeActivityShopGood(cveg,packetFor(cveg,cvegData,true,true));
  ok &= require(historical.quotaKnown && historical.historical && historical.costs[0].ownedKnown && !historical.costs[0].current,
      "historical quantities were hidden or incorrectly labelled current");
  auto unverified = observeActivityShopGood(cveg,packetFor(cveg,cvegData,false,false));
  ok &= require(unverified.quotaKnown && !unverified.historical && unverified.costs[0].ownedKnown && !unverified.costs[0].current,
      "read-only unverified observation lost its value or was promoted to a current verified balance");

  auto lcn = good(QStringLiteral("official/jieshen#PRIZE_CONFIG"),
      query(QStringLiteral("1019_0"),QJsonObject{{QStringLiteral("ai"),5747}},QStringLiteral("SimpleActExtension")),
      {QStringLiteral("b4"),QStringLiteral("b4")},6,5,true);
  lcn.cost = QStringLiteral("8:2:29");
  result = observeActivityShopGood(lcn,packetFor(lcn,{{QStringLiteral("ai"),5747},{QStringLiteral("b4"),QJsonObject{{QStringLiteral("b4"),2}}},
      {QStringLiteral("b5"),QJsonObject{{QStringLiteral("b5"),0}}},{QStringLiteral("t"),99}}));
  ok &= require(result.quotaKnown && result.used == 2 && result.remaining == 4 && result.priceKnown && result.standardCost == QStringLiteral("8:2:29"),
      "LCNF special branch lost its inherited baseOnId quota or treated displayed total t as a price selector");

  auto plsq = good(QStringLiteral("official/luoshiqi#ArrayData"),
      query(QStringLiteral("1019_0"),QJsonObject{{QStringLiteral("ai"),5735}},QStringLiteral("SimpleActExtension")),
      {QStringLiteral("b1"),QStringLiteral("b1")},7,1);
  plsq.priceOptions = {price(QStringLiteral("8:2:47"),QStringLiteral("eq"),0),
      price(QStringLiteral("8:2:41"),QStringLiteral("eq"),1),price(QStringLiteral("8:2:38"),QStringLiteral("gte"),2)};
  const QStringList costs{QStringLiteral("8:2:47"),QStringLiteral("8:2:41"),QStringLiteral("8:2:38"),QStringLiteral("8:2:38")};
  for (int total = 0; total < costs.size(); ++total) {
    result = observeActivityShopGood(plsq,packetFor(plsq,{{QStringLiteral("ai"),5735},{QStringLiteral("bt"),total},
        {QStringLiteral("b1"),QJsonObject{{QStringLiteral("b1"),0}}}}));
    ok &= require(result.quotaKnown && result.remaining == 7 && result.priceKnown && result.standardCost == costs[total],
        "PLSQGF price did not use the shared total bt three-tier rule independently from item quota");
  }
  result = observeActivityShopGood(plsq,packetFor(plsq,{{QStringLiteral("ai"),5735},{QStringLiteral("b1"),QJsonObject{{QStringLiteral("b1"),0}}}}));
  ok &= require(result.quotaKnown && !result.priceKnown && result.standardCost.isEmpty(),
      "a missing price-selector count selected an arbitrary price tier");
  result = observeActivityShopGood(plsq,packetFor(plsq,{{QStringLiteral("bt"),1},
      {QStringLiteral("b1"),QJsonObject{{QStringLiteral("b1"),0}}}},true,true));
  ok &= require(result.priceKnown && !result.priceCurrent && result.standardCost == QStringLiteral("8:2:41") && result.historical,
      "an offline historical price was promoted to the current activity price or lost its recorded tier");
  auto ambiguousPrice = plsq; ambiguousPrice.priceOptions.append(price(QStringLiteral("8:2:1"),QStringLiteral("gte"),0));
  result = observeActivityShopGood(ambiguousPrice,packetFor(ambiguousPrice,{{QStringLiteral("bt"),0}}));
  ok &= require(!result.priceKnown && result.standardCost.isEmpty(),"overlapping price conditions silently selected one option");
  auto wrongAiDescriptor = plsq;
  wrongAiDescriptor.activityQueries = lcn.activityQueries;
  ok &= require(!observeActivityShopGood(wrongAiDescriptor,packetFor(plsq,{{QStringLiteral("bt"),0}})).priceKnown,
      "different simple-activity ai parameters shared a historical price selector");

  auto gatedReturn = r4;
  gatedReturn.observationWhen = {{QStringLiteral("requestKey"),QStringLiteral("state")},
      {QStringLiteral("path"),QJsonArray{QStringLiteral("lv")}},{QStringLiteral("op"),QStringLiteral("eq")},{QStringLiteral("value"),2}};
  result = observeActivityShopGood(gatedReturn,packetFor(gatedReturn,{{QStringLiteral("bi22"),QJsonObject{{QStringLiteral("tl"),1}}}}));
  ok &= require(!result.applicabilityKnown && !result.quotaKnown,
      "a return-level-specific item was granted quota without its required level observation");
  result = observeActivityShopGood(gatedReturn,packetFor(gatedReturn,{{QStringLiteral("lv"),1},
      {QStringLiteral("bi22"),QJsonObject{{QStringLiteral("tl"),1}}}}));
  ok &= require(result.applicabilityKnown && !result.applicable && !result.quotaKnown,
      "a return-level-specific item reused counts despite a mismatching level");

  auto r6 = good(QStringLiteral("official/reback6#ArrayData"),
      query(QStringLiteral("1039_4_0"),QJsonValue(QJsonValue::Null),QStringLiteral("null")),
      {QStringLiteral("bt"),3},3,3);
  r6.cost = QStringLiteral("8:2:59");
  result = observeActivityShopGood(r6,packetFor(r6,{{QStringLiteral("bt"),QJsonArray{0,0,0,2}},
      {QStringLiteral("ps"),QJsonArray{0,0,0,99}}}));
  ok &= require(result.quotaKnown && result.used == 2 && result.remaining == 1,
      "R6 interpreted pet ps status as the bt purchase-count array");

  auto ospl = good(QStringLiteral("official/lottery#Bonus_Defines"),query(QStringLiteral("1008_20250627_hdc_0")),
      {QStringLiteral("p"),QJsonObject{{QStringLiteral("find"),QStringLiteral("i")},{QStringLiteral("equals"),37}},QStringLiteral("l")},5,37,true);
  ospl.cost = QStringLiteral("4:3200:10");
  result = observeActivityShopGood(ospl,packetFor(ospl,{{QStringLiteral("p"),QJsonArray{
      QJsonObject{{QStringLiteral("i"),99},{QStringLiteral("l"),5}},QJsonObject{{QStringLiteral("i"),37},{QStringLiteral("l"),2}}}}}));
  ok &= require(result.quotaKnown && result.used == 2 && result.remaining == 3,
      "Ospl object-array selection used an array position rather than matching i");
  result = observeActivityShopGood(ospl,packetFor(ospl,{{QStringLiteral("p"),QJsonArray{}}}));
  ok &= require(result.quotaKnown && result.used == 0 && result.remaining == 5,
      "a complete empty Ospl item list lost its explicitly proven unused default");
  result = observeActivityShopGood(ospl,packetFor(ospl,{}));
  ok &= require(!result.quotaKnown,"a missing entire Ospl list was accepted as an empty complete list");
  result = observeActivityShopGood(ospl,packetFor(ospl,{{QStringLiteral("p"),QJsonArray{
      QJsonObject{{QStringLiteral("i"),37},{QStringLiteral("l"),1}},QJsonObject{{QStringLiteral("i"),37},{QStringLiteral("l"),2}}}}}));
  ok &= require(!result.quotaKnown,"duplicate array IDs silently selected one counter");

  auto remaining = s2; remaining.limitCount = 3;
  remaining.quotaObservation.insert(QStringLiteral("valueKind"),QStringLiteral("remaining"));
  result = observeActivityShopGood(remaining,packetFor(remaining,{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),2}}}}));
  ok &= require(result.quotaKnown && result.used == 1 && result.remaining == 2,"declared remaining values were subtracted as already-used values");
  result = observeActivityShopGood(remaining,packetFor(remaining,{{QStringLiteral("bi4"),QJsonObject{{QStringLiteral("pl"),4}}}}));
  ok &= require(!result.quotaKnown,"remaining quantity above the configured limit was accepted");

  if (ok) std::puts("PASS: isolated official activity observation shapes, typed paths, dynamic prices and historical resource counts");
  return ok ? 0 : 1;
}
