#include "shop_exchange_catalog.h"
#include "domain/checked_json_numbers.h"
#include "domain/shop_limit_facts.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

QVector<int> intList(const QJsonValue& value) {
  QVector<int> result;
  QSet<int> seen;
  if (!value.isArray()) return result;
  for (const QJsonValue& item : value.toArray()) {
    int id = 0;
    if (!DomainNumeric::checkedCount(item, &id) || id <= 0) return {};
    if (!seen.contains(id)) { seen.insert(id); result.append(id); }
  }
  return result;
}

QString decodedString(const QString& encoded) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(
      (QStringLiteral("[\"") + encoded + QStringLiteral("\"]")).toUtf8(), &error);
  if (error.error == QJsonParseError::NoError && document.isArray())
    return document.array().at(0).toString();
  return encoded;
}

QString asField(const QString& block, const QString& name) {
  static const QHash<QString, QRegularExpression> expressions = [] {
    QHash<QString, QRegularExpression> result;
    for (const QString& field : {QStringLiteral("simpleParams"), QStringLiteral("limit"),
         QStringLiteral("id"), QStringLiteral("serverId"), QStringLiteral("tab"),
         QStringLiteral("basicDescription"), QStringLiteral("shelfTime"), QStringLiteral("removalTime"),
         QStringLiteral("cost"), QStringLiteral("filterKey"), QStringLiteral("unlock"), QStringLiteral("tag"),
         QStringLiteral("name"), QStringLiteral("startTime"), QStringLiteral("isOnline")})
      result.insert(field, QRegularExpression(QStringLiteral("\"%1\":(?:\"((?:\\\\.|[^\"\\\\])*)\"|(-?\\d+))")
                                             .arg(QRegularExpression::escape(field))));
    return result;
  }();
  if (!expressions.contains(name)) return {};
  const QRegularExpression expression = expressions.value(name);
  const QRegularExpressionMatch match = expression.match(block);
  if (!match.hasMatch()) return {};
  return !match.captured(1).isNull() ? decodedString(match.captured(1))
                                     : match.captured(2);
}

QJsonArray raceArray(const QString& sequence) {
  QJsonArray result;
  for (const QString& value : sequence.split(QLatin1Char('#'), Qt::KeepEmptyParts)) {
    bool ok = false;
    const int id = value.toInt(&ok);
    if (!ok || id <= 0 || QString::number(id) != value.trimmed()) return {};
    result.append(id);
  }
  return result;
}

QDate officialDate(const QString& text) {
  if (text.isEmpty()) return {};
  static const QRegularExpression digits(QStringLiteral("^[0-9]{7,8}$"));
  if (!digits.match(text).hasMatch()) return {};
  const int year = text.left(4).toInt();
  if (year < 100) return {};
  // Official DateUtil.parseDate uses fixed substrings and AS Date rollover.
  // The current monthly source's "2026929" is therefore 2033-08-09;
  // TOTAL_CONFIG's independent 2026-09-29 shop end caps its availability.
  return QDate(year, 1, 1).addMonths(text.mid(4, 2).toInt() - 1)
      .addDays(text.mid(6, 2).toInt() - 1);
}

QJsonObject embeddedRoot() {
  QFile file(QStringLiteral(":/kqpet/shop-exchange-data.json"));
  if (!file.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object()
             : QJsonObject{};
}

}  // namespace

ShopExchangeCatalog& ShopExchangeCatalog::instance() {
  static ShopExchangeCatalog catalog;
  return catalog;
}

ShopExchangeCatalog::ShopExchangeCatalog() {
  snapshot_ = prepare(embeddedRoot(), QStringLiteral("程序内置官方配置"), {}, nullptr);
  if (!snapshot_) snapshot_ = std::make_shared<ShopCatalogSnapshot>();
}

std::shared_ptr<const ShopCatalogSnapshot> ShopExchangeCatalog::prepare(const QJsonObject& root, const QString& source,
                                   const QDateTime& updatedAt, QString* error) {
  QList<ShopExchangeShop> parsedShops;
  const auto invalid = [error]() -> std::shared_ptr<const ShopCatalogSnapshot> {
    if (error) *error = QStringLiteral("商店目录结构或标识无效，保留上一份目录");
    return {};
  };
  if (!root.value(QStringLiteral("protocol")).isObject() ||
      !root.value(QStringLiteral("shops")).isArray()) return invalid();
  const QJsonObject protocol = root.value(QStringLiteral("protocol")).toObject();
  const QString extension = protocol.value(QStringLiteral("extension")).toString();
  const QString command = protocol.value(QStringLiteral("getInfoCommand")).toString();
  if (extension.isEmpty() || command.isEmpty()) {
    if (error) *error = QStringLiteral("商店协议配置不完整");
    return {};
  }
  QSet<QString> shopIds;
  int totalGoods = 0;
  for (const QJsonValue& shopValue : root.value(QStringLiteral("shops")).toArray()) {
    if (!shopValue.isObject()) return invalid();
    const QJsonObject shopObject = shopValue.toObject();
    ShopExchangeShop shop;
    shop.sourceKey = shopObject.value(QStringLiteral("sourceKey")).toString();
    if (shopObject.contains(QStringLiteral("sourceKey")) &&
        (!shopObject.value(QStringLiteral("sourceKey")).isString() || shop.sourceKey.size() > 512)) return invalid();
    if (!DomainNumeric::checkedCount(shopObject.value(QStringLiteral("shopId")), &shop.shopId) ||
        shop.shopId <= 0 || shopIds.contains(shop.sourceKey + QLatin1Char(':') + QString::number(shop.shopId)) ||
        !shopObject.value(QStringLiteral("name")).isString() ||
        !shopObject.value(QStringLiteral("goods")).isArray()) return invalid();
    shopIds.insert(shop.sourceKey + QLatin1Char(':') + QString::number(shop.shopId));
    shop.name = shopObject.value(QStringLiteral("name")).toString();
    if (!shop.sourceKey.isEmpty() && shopObject.contains(QStringLiteral("observation"))) {
      if (!shopObject.value(QStringLiteral("observation")).isObject()) return invalid();
      shop.observation = shopObject.value(QStringLiteral("observation")).toObject();
      if (shop.observation.value(QStringLiteral("schema")).toInt() != 1 ||
          !shop.observation.value(QStringLiteral("requests")).isArray() ||
          shop.observation.value(QStringLiteral("requests")).toArray().size() > 8) return invalid();
    }
    shop.siKey = shopObject.value(QStringLiteral("siKey")).toString();
    if (shop.siKey.isEmpty()) shop.siKey = QStringLiteral("si%1").arg(shop.shopId);
    QSet<int> itemIds;
    for (const QJsonValue& goodValue : shopObject.value(QStringLiteral("goods")).toArray()) {
      if (!goodValue.isObject()) return invalid();
      const QJsonObject goodObject = goodValue.toObject();
      ShopExchangeGood good;
      good.shopId = shop.shopId;
      good.shopName = shop.name;
      good.sourceKey = shop.sourceKey;
      good.sourceUrl = goodObject.value(QStringLiteral("sourceUrl")).toString();
      if (!good.sourceKey.isEmpty()) {
        for (const auto& request : shop.observation.value(QStringLiteral("requests")).toArray()) {
          const auto object = request.toObject(); const auto key = object.value(QStringLiteral("key")).toString();
          if (key.isEmpty() || key.size() > 64 || good.activityQueries.contains(key) ||
              !object.value(QStringLiteral("command")).isString() || !object.value(QStringLiteral("extension")).isString() ||
              (!object.value(QStringLiteral("params")).isObject() && !object.value(QStringLiteral("params")).isNull())) return invalid();
          good.activityQueries.insert(key,object);
        }
        for (const QString& field : {QStringLiteral("costDescription")})
          if (goodObject.contains(field) && (!goodObject.value(field).isString() || goodObject.value(field).toString().size() > 4096)) return invalid();
        for (const QString& field : {QStringLiteral("activityCosts"),QStringLiteral("priceOptions")})
          if (goodObject.contains(field) && (!goodObject.value(field).isArray() || goodObject.value(field).toArray().size() > 16)) return invalid();
        if (goodObject.contains(QStringLiteral("quotaObservation")) && !goodObject.value(QStringLiteral("quotaObservation")).isObject()) return invalid();
        if (goodObject.contains(QStringLiteral("observationWhen")) && !goodObject.value(QStringLiteral("observationWhen")).isObject()) return invalid();
        good.costDescription = goodObject.value(QStringLiteral("costDescription")).toString();
        good.activityCosts = goodObject.value(QStringLiteral("activityCosts")).toArray();
        good.priceOptions = goodObject.value(QStringLiteral("priceOptions")).toArray();
        good.quotaObservation = goodObject.value(QStringLiteral("quotaObservation")).toObject();
        good.observationWhen = goodObject.value(QStringLiteral("observationWhen")).toObject();
      }
      if (!DomainNumeric::checkedCount(goodObject.value(QStringLiteral("itemServerId")),
                        &good.itemServerId) || (shop.sourceKey.isEmpty() && good.itemServerId <= 0) ||
          itemIds.contains(good.itemServerId)) return invalid();
      itemIds.insert(good.itemServerId);
      for (const QString& field : {QStringLiteral("description"),
           QStringLiteral("cost"), QStringLiteral("enhanceType"),
           QStringLiteral("unlock"), QStringLiteral("shelfTime")})
        if (!goodObject.value(field).isString()) return invalid();
      if (goodObject.contains(QStringLiteral("tab")) && !DomainNumeric::checkedCount(goodObject.value(QStringLiteral("tab")), &good.tab)) return invalid();
      good.description = goodObject.value(QStringLiteral("description")).toString();
      good.limitText = goodObject.value(QStringLiteral("limit")).toString();
      good.limitKey = goodObject.value(QStringLiteral("limitKey")).toString();
      good.limitLabel = goodObject.value(QStringLiteral("limitLabel")).toString();
      if (!DomainNumeric::checkedCount(goodObject.value(QStringLiteral("limitCount")),
                        &good.limitCount)) good.limitCount = -1;
      good.cost = goodObject.value(QStringLiteral("cost")).toString();
      if (!good.sourceKey.isEmpty() && goodObject.value(QStringLiteral("costKnown")).isBool() &&
          !goodObject.value(QStringLiteral("costKnown")).toBool()) good.cost.clear();
      good.enhanceType = goodObject.value(QStringLiteral("enhanceType")).toString();
      good.unlock = goodObject.value(QStringLiteral("unlock")).toString();
      good.tag = goodObject.value(QStringLiteral("tag")).toString();
      const QStringList enhanceCodes =
          good.enhanceType.split(QLatin1Char('-'), Qt::SkipEmptyParts);
      // These phrases are copied from the real official project description.
      // A file's self-declared "proven" fields are not semantic evidence.
      // Do not infer a quantity from generic wording such as "满级".
      if (good.provenGapUnitsPerExchange <= 0 && enhanceCodes.size() == 1 &&
          enhanceCodes.constFirst().trimmed() == QStringLiteral("34") &&
          (good.description.contains(QStringLiteral("1颗红星")) ||
           good.description.contains(QStringLiteral("1个1级红色星神")))) {
        good.provenGapUnitsPerExchange = 1;
        good.provenGapCode = enhanceCodes.constFirst().trimmed();
      }
      good.raceIds = intList(goodObject.value(QStringLiteral("raceIds")));
      if (good.raceIds.isEmpty()) return invalid();
      good.shelfDate = parseYmd(goodObject.value(QStringLiteral("shelfTime")).toString());
      if (!good.shelfDate.isValid() && (good.sourceKey.isEmpty() || !goodObject.value(QStringLiteral("shelfTime")).toString().isEmpty())) return invalid();
      const QJsonValue removal = goodObject.value(QStringLiteral("removalTime"));
      if (!removal.isUndefined() && !removal.isString()) return invalid();
      good.removalDate = parseYmd(goodObject.value(QStringLiteral("removalTime")).toString());
      good.hasRemovalDate = good.removalDate.isValid();
      if (!removal.toString().isEmpty() && !good.hasRemovalDate) return invalid();
      if (good.hasRemovalDate && good.shelfDate.isValid() && good.removalDate < good.shelfDate) return invalid();
      shop.goods.append(good);
      ++totalGoods;
    }
    if (shop.shopId > 0) parsedShops.append(shop);
  }
  if (parsedShops.isEmpty() || totalGoods == 0) {
    if (error) *error = QStringLiteral("没有解析到可用的精灵培养兑换项目");
    return {};
  }
  std::sort(parsedShops.begin(), parsedShops.end(),
      [](const ShopExchangeShop& left, const ShopExchangeShop& right) {
        return left.sourceKey == right.sourceKey ? left.shopId < right.shopId : left.sourceKey < right.sourceKey;
      });
  static std::atomic<quint64> revisions{0};
  auto next = std::make_shared<ShopCatalogSnapshot>();
  next->revision = ++revisions;
  next->root = root;
  next->allShops = parsedShops;
  next->extension = extension;
  next->getInfoCommand = command;
  const QJsonValue params = protocol.value(QStringLiteral("getInfoParams"));
  next->getInfoParams = params.isObject()
                       ? QString::fromUtf8(QJsonDocument(params.toObject()).toJson(QJsonDocument::Compact))
                       : QStringLiteral("{}");
  next->activityId = protocol.value(QStringLiteral("activityId")).toInt();
  next->loaded = true;
  next->sourceLabel = source;
  next->sourceUpdatedAt = updatedAt;
  return next;
}

QJsonObject ShopExchangeCatalog::parseOfficialText(const QString& text,
    const QJsonObject& protocol, QString* error) {
  static const QHash<int, QString> shopNames = {
      {1, QStringLiteral("永恒战场商店")}, {2, QStringLiteral("奥奇之星商店")},
      {3, QStringLiteral("排位赛商店")}, {4, QStringLiteral("竞技场商店")},
      {5, QStringLiteral("月福利中心商店")}, {6, QStringLiteral("联盟商店")}};
  static const QStringList limitKeys = {QStringLiteral("dl"), QStringLiteral("wl"),
                                        QStringLiteral("ml"), QStringLiteral("pl"),
                                        QStringLiteral("tl")};
  static const QStringList limitLabels = {QStringLiteral("日"), QStringLiteral("周"),
                                          QStringLiteral("月"), QStringLiteral("期"),
                                          QStringLiteral("总")};
  const QRegularExpression objectExpression(QStringLiteral("\\{([^{}]+)\\}"));
  const QRegularExpression prizeExpression(
      QStringLiteral("^CommonEnhancePrize,[^,]+,[^,]+,([1-9][0-9]*(?:-[1-9][0-9]*)*),([1-9][0-9]*(?:#[1-9][0-9]*)*)$"));
  QHash<int, QString> officialShops;
  const QRegularExpression totalExpression(QStringLiteral("TOTAL_CONFIG:Object\\s*=\\s*(.*?);"),
      QRegularExpression::DotMatchesEverythingOption);
  const auto totalMatch = totalExpression.match(text);
  if (totalMatch.hasMatch()) {
    auto definitions = objectExpression.globalMatch(totalMatch.captured(1));
    while (definitions.hasNext()) {
      const QString block = definitions.next().captured(1);
      const int id = asField(block, QStringLiteral("serverId")).toInt();
      if (id > 0) officialShops.insert(id, block);
    }
  }
  const QRegularExpression markerExpression(
      QStringLiteral("public static const SERVER_ID_([0-9]+)_REWARD_CONFIG:Array\\s*=\\s*"));
  QJsonArray shops;
  auto markers = markerExpression.globalMatch(text);
  while (markers.hasNext()) {
    const auto marker = markers.next();
    const int shopId = marker.captured(1).toInt();
    const QString shopBlock = officialShops.value(shopId);
    if (asField(shopBlock, QStringLiteral("isOnline")) == QStringLiteral("FALSE")) continue;
    const QString shopStartText = asField(shopBlock, QStringLiteral("startTime"));
    const QString shopEndText = asField(shopBlock, QStringLiteral("removalTime"));
    const QDate shopStart = officialDate(shopStartText);
    const QDate shopEnd = officialDate(shopEndText);
    if ((!shopStartText.isEmpty() && !shopStart.isValid()) ||
        (!shopEndText.isEmpty() && !shopEnd.isValid())) {
      if (error) *error = QStringLiteral("官方商店开放日期无效，保留旧目录");
      return {};
    }
    const int bodyBegin = marker.capturedEnd();
    int bodyEnd = text.indexOf(QStringLiteral("public static const"), bodyBegin);
    if (bodyEnd < 0) bodyEnd = text.size();
    const QString body = text.mid(bodyBegin, bodyEnd - bodyBegin);
    QJsonArray goods;
    QRegularExpressionMatchIterator matches = objectExpression.globalMatch(body);
    while (matches.hasNext()) {
      const QString block = matches.next().captured(1);
      const QString simple = asField(block, QStringLiteral("simpleParams"));
      const QRegularExpressionMatch prize = prizeExpression.match(simple);
      if (simple.section(QLatin1Char(','), 0, 0) != QStringLiteral("CommonEnhancePrize")) continue;
      if (!prize.hasMatch()) {
        if (error) *error = QStringLiteral("官方培养奖励格式无效，保留旧目录");
        return {};
      }
      const QString enhanceType = prize.captured(1).trimmed();
      const QJsonArray races = raceArray(prize.captured(2).trimmed());
      if (enhanceType.isEmpty() || races.isEmpty()) {
        if (error) *error = QStringLiteral("官方培养奖励种族或类型无效，保留旧目录");
        return {};
      }
      const QString limit = asField(block, QStringLiteral("limit"));
      const QStringList limitParts = limit.split(QLatin1Char(':'), Qt::KeepEmptyParts);
      int limitIndex = -1, limitCount = -1;
      if (limitParts.size() != 2 || !DomainNumeric::checkedCount(limitParts.value(0), &limitIndex) ||
          !DomainNumeric::checkedCount(limitParts.value(1), &limitCount) || limitIndex >= limitKeys.size()) {
        // Unsupported limit syntax remains visible as Unknown. It must not
        // silently become the daily (index zero) quota through toInt().
        limitIndex = limitCount = -1;
      }
      int tab = 0;
      const QString tabText = asField(block, QStringLiteral("tab"));
      if (!tabText.isEmpty() && !DomainNumeric::checkedCount(tabText, &tab)) tab = -1;
      const QString originalStart = asField(block, QStringLiteral("shelfTime"));
      const QString originalEnd = asField(block, QStringLiteral("removalTime"));
      QDate start = officialDate(originalStart);
      QDate end = officialDate(originalEnd);
      if (!start.isValid() || (!originalEnd.isEmpty() && !end.isValid())) {
        if (error) *error = QStringLiteral("官方兑换项目日期无效，保留旧目录");
        return {};
      }
      if (shopEnd.isValid() && (!end.isValid() || shopEnd < end)) end = shopEnd;
      if (shopStart.isValid() && (!end.isValid() || end >= shopStart) && start < shopStart)
        start = shopStart;
      goods.append(QJsonObject{
          {QStringLiteral("id"), asField(block, QStringLiteral("id")).toInt()},
          {QStringLiteral("itemServerId"), asField(block, QStringLiteral("serverId")).toInt()},
          {QStringLiteral("tab"), tab},
          {QStringLiteral("description"), asField(block, QStringLiteral("basicDescription"))},
          {QStringLiteral("shelfTime"), start.toString(QStringLiteral("yyyyMMdd"))},
          {QStringLiteral("removalTime"), end.toString(QStringLiteral("yyyyMMdd"))},
          {QStringLiteral("officialShelfTime"), originalStart},
          {QStringLiteral("officialRemovalTime"), originalEnd},
          {QStringLiteral("limit"), limit}, {QStringLiteral("limitIndex"), limitIndex},
          {QStringLiteral("limitCount"), limitCount},
          {QStringLiteral("limitKey"), limitKeys.value(limitIndex)},
          {QStringLiteral("limitLabel"), limitLabels.value(limitIndex)},
          {QStringLiteral("cost"), asField(block, QStringLiteral("cost"))},
          {QStringLiteral("enhanceType"), enhanceType}, {QStringLiteral("raceIds"), races},
          {QStringLiteral("filterKey"), asField(block, QStringLiteral("filterKey"))},
          {QStringLiteral("unlock"), asField(block, QStringLiteral("unlock"))},
          {QStringLiteral("tag"), asField(block, QStringLiteral("tag"))}});
    }
    if (goods.isEmpty()) continue;
    QString shopName = asField(shopBlock, QStringLiteral("name"));
    if (shopName.isEmpty()) shopName = shopNames.value(shopId, QStringLiteral("商店 %1").arg(shopId));
    shops.append(QJsonObject{{QStringLiteral("shopId"), shopId},
                             {QStringLiteral("name"), shopName},
                             {QStringLiteral("siKey"), QStringLiteral("si%1").arg(shopId)},
                             {QStringLiteral("goods"), goods}});
  }
  return QJsonObject{{QStringLiteral("schema"), 2},
                     {QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},

                     {QStringLiteral("protocol"), protocol},
                     {QStringLiteral("shops"), shops}};
}

QDate ShopExchangeCatalog::parseYmd(const QString& text) {
  if (text.size() != 8) return {};
  return QDate::fromString(text, QStringLiteral("yyyyMMdd"));
}

QList<ShopExchangeShop> ShopExchangeCatalog::shops(const QDate& date) const {
  QList<ShopExchangeShop> result;
  const auto current = snapshot();
  for (const ShopExchangeShop& shop : current->allShops) {
    ShopExchangeShop copy;
    copy.shopId = shop.shopId;
    copy.name = shop.name;
    copy.siKey = shop.siKey;
    copy.sourceKey = shop.sourceKey;
    copy.observation = shop.observation;
    for (const ShopExchangeGood& good : shop.goods)
      if (good.isOnlineOn(date)) copy.goods.append(good);
    result.append(copy);
  }
  return result;
}

QList<ShopExchangeShop> ShopExchangeCatalog::protocolShops(const QDate& date) const {
  QList<ShopExchangeShop> result;
  for (const auto& shop : shops(date)) if (shop.sourceKey.isEmpty()) result.append(shop);
  return result;
}

QList<ShopExchangeGood> ShopExchangeCatalog::onlineGoods(const QDate& date) const {
  QList<ShopExchangeGood> result;
  for (const ShopExchangeShop& shop : shops(date)) result.append(shop.goods);
  return result;
}

int ShopExchangeCatalog::usedCount(const QJsonObject& packet, const ShopExchangeGood& good) {
  return shopUsedCount(packet, good);
}

int ShopExchangeCatalog::remainingCount(const QJsonObject& packet, const ShopExchangeGood& good) {
  return shopRemainingCount(packet, good);
}
