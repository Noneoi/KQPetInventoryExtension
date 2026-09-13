#include "activity_shop_observation.h"
#include "checked_json_numbers.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>
#include <limits>

namespace {
QJsonValue pathValue(QJsonValue current, const QJsonArray& path) {
  if (path.isEmpty() || path.size() > 12) return QJsonValue::Null;
  for (const auto& segment : path) {
    if (segment.isObject()) {
      const auto selector = segment.toObject();
      const auto field = selector.value(QStringLiteral("find")).toString();
      if (!current.isArray() || field.isEmpty() || selector.size() != 2 || !selector.contains(QStringLiteral("equals"))) return QJsonValue::Null;
      QJsonValue found(QJsonValue::Undefined); qint64 target = 0;
      if (!DomainNumeric::checkedInteger(selector.value(QStringLiteral("equals")),&target,0)) return QJsonValue::Null;
      for (const auto& element : current.toArray()) {
        qint64 actual = 0;
        if (!element.isObject() || !DomainNumeric::checkedInteger(element.toObject().value(field),&actual,0)) return QJsonValue::Null;
        if (actual == target) { if (!found.isUndefined()) return QJsonValue::Null; found = element; }
      }
      current = found; continue;
    }
    qint64 numericIndex = 0;
    const bool numeric = segment.isDouble() && DomainNumeric::checkedInteger(segment,&numericIndex,0,std::numeric_limits<int>::max());
    if ((!segment.isString() && !numeric) || (segment.isString() && (segment.toString().isEmpty() || segment.toString().size() > 128))) return QJsonValue::Null;
    const auto key = numeric ? QString::number(numericIndex) : segment.toString();
    if (current.isUndefined()) return current;
    if (current.isObject()) current = current.toObject().value(key);
    else if (current.isArray()) {
      qint64 index = -1;
      if (!DomainNumeric::checkedInteger(key,&index,0,std::numeric_limits<int>::max())) return QJsonValue::Null;
      if (index >= current.toArray().size()) return QJsonValue::Undefined;
      current = current.toArray().at(index);
    } else return QJsonValue::Null;
  }
  return current;
}
bool conditionMatches(const ShopExchangeGood& good,const QJsonObject& test,const QJsonObject& packet,bool* known,bool* current = nullptr) {
  *known = false;
  if (current) *current = false;
  if (test.isEmpty()) return false;
  if (test.value(QStringLiteral("all")).isArray()) {
    const auto conditions = test.value(QStringLiteral("all")).toArray();
    if (conditions.isEmpty() || conditions.size() > 8) return false;
    bool result = true,currentValues = true;
    for (const auto& value : conditions) {
      if (!value.isObject() || value.toObject().contains(QStringLiteral("all"))) return false;
      bool partKnown = false,partCurrent = false;
      const bool matches = conditionMatches(good,value.toObject(),packet,&partKnown,&partCurrent);
      if (!partKnown) return false;
      result &= matches;
      currentValues &= partCurrent;
    }
    *known = true; if (current) *current = currentValues; return result;
  }
  qint64 actual = 0,expected = 0;
  bool historical = true,verified = false;
  if (!DomainNumeric::checkedInteger(activityObservedValue(good,test,packet,&historical,&verified),&actual,0) ||
      !DomainNumeric::checkedInteger(test.value(QStringLiteral("value")),&expected,0)) return false;
  const auto op = test.value(QStringLiteral("op")).toString();
  *known = true;
  if (current) *current = !historical && verified;
  if (op == QStringLiteral("eq")) return actual == expected;
  if (op == QStringLiteral("lt")) return actual < expected;
  if (op == QStringLiteral("lte")) return actual <= expected;
  if (op == QStringLiteral("gt")) return actual > expected;
  if (op == QStringLiteral("gte")) return actual >= expected;
  *known = false; return false;
}
}

QString activityRequestSignature(const QJsonObject& request) {
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(request).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256).toHex());
}

QJsonValue activityObservedValue(const ShopExchangeGood& good,const QJsonObject& descriptor,
    const QJsonObject& packet,bool* historical,bool* verified,QDateTime* observedAt) {
  if (historical) *historical = true;
  if (verified) *verified = false;
  const auto key = descriptor.value(QStringLiteral("requestKey")).toString();
  const auto request = good.activityQueries.value(key).toObject();
  const auto record = packet.value(QStringLiteral("_activities")).toObject().value(good.sourceKey).toObject().value(key).toObject();
  if (good.sourceKey.isEmpty() || key.isEmpty() || request.isEmpty() || !record.value(QStringLiteral("data")).isObject() ||
      record.value(QStringLiteral("request")).toString() != activityRequestSignature(request)) return QJsonValue::Undefined;
  const auto time = QDateTime::fromString(record.value(QStringLiteral("observedAt")).toString(),Qt::ISODateWithMs);
  if (!time.isValid()) return QJsonValue::Undefined;
  if (observedAt) *observedAt = time;
  if (historical) *historical = record.value(QStringLiteral("historical")).toBool(true);
  if (verified) *verified = record.value(QStringLiteral("verified")).toBool(false);
  auto value = pathValue(record.value(QStringLiteral("data")),descriptor.value(QStringLiteral("path")).toArray());
  if (value.isUndefined() && !descriptor.contains(QStringLiteral("missingValue")) && record.value(QStringLiteral("previousData")).isObject()) {
    value = pathValue(record.value(QStringLiteral("previousData")),descriptor.value(QStringLiteral("path")).toArray());
    if (!value.isUndefined()) {
      if (historical) *historical = true;
      if (verified) *verified = false;
      const auto previousTime = QDateTime::fromString(record.value(QStringLiteral("previousObservedAt")).toString(),Qt::ISODateWithMs);
      if (observedAt && previousTime.isValid()) *observedAt = previousTime;
    }
  }
  if (descriptor.value(QStringLiteral("encoding")).toString() == QStringLiteral("id-counts")) {
    qint64 id = 0;
    if (!value.isString() || value.toString().size() > 32768 ||
        !DomainNumeric::checkedInteger(descriptor.value(QStringLiteral("itemId")),&id,1)) return QJsonValue::Null;
    QSet<qint64> seen; QJsonValue found(QJsonValue::Undefined);
    const auto tokens = value.toString().isEmpty() ? QStringList{} : value.toString().split(QLatin1Char('#'),Qt::KeepEmptyParts);
    for (const auto& token : tokens) {
      const auto pair = token.split(QLatin1Char(':')); qint64 item = 0,count = 0;
      if (pair.size() != 2 || !DomainNumeric::checkedInteger(pair[0],&item,1) ||
          !DomainNumeric::checkedInteger(pair[1],&count,0) || seen.contains(item)) return QJsonValue::Null;
      seen.insert(item); if (item == id) found = QString::number(count);
    }
    value = found;
  }
  if (value.isUndefined() && descriptor.value(QStringLiteral("missingValue")).isDouble() &&
      descriptor.value(QStringLiteral("missingValue")).toDouble(-1) == 0) return 0;
  return value;
}

ActivityShopObservation observeActivityShopGood(const ShopExchangeGood& good,const QJsonObject& packet) {
  ActivityShopObservation result;
  result.standardCost = good.cost; result.priceKnown = !good.cost.isEmpty() || good.provenFree;
  result.priceCurrent = result.priceKnown;
  if (good.sourceKey.isEmpty()) return result;
  if (!good.observationWhen.isEmpty()) result.applicable = conditionMatches(good,good.observationWhen,packet,&result.applicabilityKnown);
  if (!good.priceOptions.isEmpty()) {
    int matches = 0; bool allKnown = true,allCurrent = true; QString price;
    for (const auto& value : good.priceOptions) {
      const auto option = value.toObject(); bool known = false,current = false;
      const bool constant = option.value(QStringLiteral("selected")).isBool();
      const bool match = constant ? option.value(QStringLiteral("selected")).toBool()
          : conditionMatches(good,option.value(QStringLiteral("when")).toObject(),packet,&known,&current);
      if (constant) known = current = true;
      allKnown &= known;
      allCurrent &= current;
      if (match && option.value(QStringLiteral("cost")).isString()) { ++matches; price = option.value(QStringLiteral("cost")).toString(); }
    }
    result.priceKnown = allKnown && matches == 1 && !price.isEmpty();
    result.standardCost = result.priceKnown ? price : QString{};
    result.priceCurrent = result.priceKnown && allCurrent;
  }
  qint64 count = 0;
  const auto quota = activityObservedValue(good,good.quotaObservation,packet,&result.historical,nullptr,&result.observedAt);
  if (result.applicabilityKnown && result.applicable && good.limitCount > 0 && DomainNumeric::checkedInteger(quota,&count,0,std::numeric_limits<int>::max())) {
    const auto kind = good.quotaObservation.value(QStringLiteral("valueKind")).toString();
    if (kind == QStringLiteral("used")) { result.used = int(count); result.remaining = qMax(0,good.limitCount-result.used); result.quotaKnown = true; }
    else if (kind == QStringLiteral("remaining") && count <= good.limitCount) { result.remaining = int(count); result.used = good.limitCount-result.remaining; result.quotaKnown = true; }
  }
  for (const auto& value : good.activityCosts) {
    const auto descriptor = value.toObject(); ActivityShopCost cost;
    cost.name = descriptor.value(QStringLiteral("name")).toString();
    if (cost.name.isEmpty() || !DomainNumeric::checkedInteger(descriptor.value(QStringLiteral("count")),&cost.required,1)) continue;
    bool historical = true,verified = false;
    cost.ownedKnown = DomainNumeric::checkedInteger(activityObservedValue(good,descriptor,packet,&historical,&verified),&cost.owned,0);
    cost.current = cost.ownedKnown && !historical && verified;
    auto identity = descriptor; identity.remove(QStringLiteral("name")); identity.remove(QStringLiteral("count"));
    cost.key = good.sourceKey + QStringLiteral(":activity-cost:") + activityRequestSignature(identity).left(20);
    result.costs.append(cost);
  }
  if (good.cost.isEmpty() && good.priceOptions.isEmpty() && !result.costs.isEmpty() && result.costs.size() == good.activityCosts.size()) {
    result.priceKnown = true; result.priceCurrent = true;
  }
  return result;
}
