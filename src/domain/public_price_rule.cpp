#include "public_price_rule.h"
#include "checked_json_numbers.h"
#include <QJsonArray>
#include <limits>

namespace {
enum class Truth { No, Yes, Unknown };

Truth test(const QJsonObject& condition, const QDateTime& at,
           const AccountResourceView* resources, const QJsonObject& facts, int depth = 0) {
  if (depth > 24) return Truth::Unknown;
  const auto type = condition.value(QStringLiteral("type")).toString();
  if (type == QStringLiteral("always")) {
    const auto value = condition.value(QStringLiteral("value"));
    return !value.isBool() ? Truth::Unknown : value.toBool() ? Truth::Yes : Truth::No;
  }
  if (type == QStringLiteral("all")) {
    if (!condition.value(QStringLiteral("conditions")).isArray()) return Truth::Unknown;
    bool unknown = false;
    for (const auto& child : condition.value(QStringLiteral("conditions")).toArray()) {
      const auto value = test(child.toObject(), at, resources, facts, depth + 1);
      if (value == Truth::No) return Truth::No;
      unknown |= value == Truth::Unknown;
    }
    return unknown ? Truth::Unknown : Truth::Yes;
  }
  if (type == QStringLiteral("date")) {
    if (!at.isValid()) return Truth::Unknown;
    for (const auto& field : {QStringLiteral("startsAt"), QStringLiteral("endsAt")}) {
      if (condition.contains(field) && !condition.value(field).isString()) return Truth::Unknown;
      const auto raw = condition.value(field).toString();
      if (raw.isEmpty()) continue;
      const auto boundary = QDateTime::fromString(raw, Qt::ISODate);
      if (!boundary.isValid()) return Truth::Unknown;
      if ((field == QStringLiteral("startsAt") && at < boundary) ||
          (field == QStringLiteral("endsAt") && at >= boundary)) return Truth::No;
    }
    return Truth::Yes;
  }
  if (type == QStringLiteral("vip")) {
    const auto value = facts.value(QStringLiteral("rmbVip"));
    return !value.isBool() ? Truth::Unknown : value.toBool() ? Truth::Yes : Truth::No;
  }
  if (type == QStringLiteral("item")) {
    qint64 material = 0, id = 0, required = 0;
    if (!resources || !DomainNumeric::checkedInteger(condition.value(QStringLiteral("materialType")), &material, 1) ||
        !DomainNumeric::checkedInteger(condition.value(QStringLiteral("id")), &id, 1) ||
        !DomainNumeric::checkedInteger(condition.value(QStringLiteral("count")), &required, 0)) return Truth::Unknown;
    const auto key = QStringLiteral("%1:%2").arg(material).arg(id);
    if (!resources->hasReliableCount(key)) return Truth::Unknown;
    return resources->count(key) >= required ? Truth::Yes : Truth::No;
  }
  if (type == QStringLiteral("selected-pet-sacred-stage-material")) {
    const auto value = facts.value(QStringLiteral("sacredStageRequiresEquipment"));
    const auto expected = condition.value(QStringLiteral("requiresEquipment"));
    if (!value.isBool() || !expected.isBool()) return Truth::Unknown;
    return value.toBool() == expected.toBool() ? Truth::Yes : Truth::No;
  }
  return Truth::Unknown;
}

Truth first(const QJsonArray& cases, const QDateTime& at, const AccountResourceView* resources,
            const QJsonObject& facts, qint64* amount) {
  for (const auto& value : cases) {
    const auto entry = value.toObject();
    const auto active = test(entry.value(QStringLiteral("when")).toObject(), at, resources, facts);
    if (active == Truth::Unknown) return active; // Later branches cannot overtake an unknown earlier one.
    if (active == Truth::Yes) {
      return DomainNumeric::checkedInteger(entry.value(QStringLiteral("amount")), amount, 0, 1000000000)
          ? Truth::Yes : Truth::Unknown;
    }
  }
  return Truth::No;
}

void collectBoundaries(const QJsonValue& node, const QDateTime& after, QDateTime* next, int depth) {
  if (depth > 24) return;
  if (node.isArray()) for (const auto& child : node.toArray()) collectBoundaries(child, after, next, depth + 1);
  if (!node.isObject()) return;
  const auto object = node.toObject();
  if (object.value(QStringLiteral("type")).toString() == QStringLiteral("date")) {
    for (const auto& key : {QStringLiteral("startsAt"), QStringLiteral("endsAt")}) {
      const auto boundary = QDateTime::fromString(object.value(key).toString(), Qt::ISODate);
      if (boundary.isValid() && boundary > after && (!next->isValid() || boundary < *next)) *next = boundary;
    }
  }
  for (auto it = object.begin(); it != object.end(); ++it) collectBoundaries(it.value(), after, next, depth + 1);
}
}

PublicPriceQuote evaluatePublicPriceRule(const QJsonObject& rule, const QDateTime& at,
    const AccountResourceView* resources, const QJsonObject& facts) {
  PublicPriceQuote result;
  result.reason = QStringLiteral("价格条件尚未确认");
  if (!DomainNumeric::checkedInteger(rule.value(QStringLiteral("schema")), nullptr, 1, 1) ||
      (rule.contains(QStringLiteral("unresolved")) && !rule.value(QStringLiteral("unresolved")).isArray()) ||
      !rule.value(QStringLiteral("unresolved")).toArray().isEmpty()) return result;
  const auto kind = rule.value(QStringLiteral("kind")).toString();
  if (kind == QStringLiteral("constructed-cultivation")) {
    const auto selected = first(rule.value(QStringLiteral("cases")).toArray(), at, resources, facts, &result.amount);
    result.known = selected == Truth::Yes;
    result.reason = result.known ? QStringLiteral("所选精灵对应报价") : QStringLiteral("费用取决于所选精灵升阶所需材料");
    return result;
  }
  if (kind != QStringLiteral("shared-cultivation")) return result;
  qint64 tag = 0;
  const auto preset = first(rule.value(QStringLiteral("preset")).toArray(), at, resources, facts, &tag);
  const auto unconditional = rule.value(QStringLiteral("unconditionalOverride"));
  if (!unconditional.isNull() && !unconditional.isUndefined()) {
    if (!DomainNumeric::checkedInteger(unconditional, &tag, 0, 1000000000)) return result;
  } else if (preset != Truth::Yes) {
    result.availabilityKnown = preset == Truth::No;
    result.available = preset != Truth::No;
    result.reason = preset == Truth::No ? QStringLiteral("当前不满足公共养成开放条件") : QStringLiteral("公共养成开放条件需要账号资料");
    return result;
  }
  result.availabilityKnown = true;
  const auto selected = first(rule.value(QStringLiteral("overrideCases")).toArray(), at, resources, facts, &result.amount);
  if (selected == Truth::Unknown) return result;
  if (selected == Truth::No) result.amount = tag;
  const auto discount = rule.value(QStringLiteral("globalDiscount")).toObject();
  const auto active = test(discount.value(QStringLiteral("when")).toObject(), at, resources, facts);
  if (active == Truth::Unknown) return result;
  if (active == Truth::Yes) {
    qint64 numerator = 0, denominator = 0;
    if (!DomainNumeric::checkedInteger(discount.value(QStringLiteral("numerator")), &numerator, 1, 1000000000) ||
        !DomainNumeric::checkedInteger(discount.value(QStringLiteral("denominator")), &denominator, numerator, 1000000000)) return result;
    result.amount = (result.amount * numerator + denominator - 1) / denominator;
    result.discounted = true;
  }
  const auto ticket = rule.value(QStringLiteral("payment")).toObject().value(QStringLiteral("ticket")).toObject();
  result.optionalPayment = !ticket.isEmpty() && rule.value(QStringLiteral("componentCount")).toInt() >= ticket.value(QStringLiteral("minComponents")).toInt(2) &&
      test(ticket.value(QStringLiteral("when")).toObject(), at, resources, facts) != Truth::No;
  result.known = true;
  result.reason = result.optionalPayment ? QStringLiteral("钻石报价；券及优惠券需在支付面板选择") : QStringLiteral("按当前公开条件计算");
  return result;
}

QDateTime nextPublicPriceChange(const QJsonObject& rule, const QDateTime& after) {
  QDateTime result;
  collectBoundaries(rule, after, &result, 0);
  return result;
}
