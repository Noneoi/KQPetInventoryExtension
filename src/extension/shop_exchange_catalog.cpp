#include "shop_exchange_catalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

QVector<int> intList(const QJsonValue& value) {
  QVector<int> result;
  if (!value.isArray()) return result;
  for (const QJsonValue& item : value.toArray()) result.append(item.toInt());
  return result;
}

QJsonObject readObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object()
             : QJsonObject{};
}

bool writeObject(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
  return file.commit();
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
  const QRegularExpression expression(
      QStringLiteral("\"%1\":(?:\"((?:\\\\.|[^\"\\\\])*)\"|(-?\\d+))")
          .arg(QRegularExpression::escape(name)));
  const QRegularExpressionMatch match = expression.match(block);
  if (!match.hasMatch()) return {};
  return !match.captured(1).isNull() ? decodedString(match.captured(1))
                                     : match.captured(2);
}

QJsonArray raceArray(const QString& sequence) {
  QJsonArray result;
  for (const QString& value : sequence.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
    bool ok = false;
    const int id = value.toInt(&ok);
    if (ok && id > 0) result.append(id);
  }
  return result;
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

bool ShopExchangeGood::isOnlineOn(const QDate& date) const {
  if (!shelfDate.isValid() || date < shelfDate) return false;
  if (!hasRemovalDate) return true;
  return date <= removalDate;
}

QString ShopExchangeGood::itemKey() const {
  return QStringLiteral("bi%1").arg(itemServerId);
}

QString ShopExchangeGood::stableKey() const {
  QByteArray identity = QStringLiteral("%1|%2|%3|")
                            .arg(shopId).arg(itemServerId).arg(enhanceType).toUtf8();
  for (int raceId : raceIds) identity += QByteArray::number(raceId) + ',';
  return QStringLiteral("%1:%2:%3")
      .arg(shopId).arg(itemServerId)
      .arg(QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
                                   .toHex().left(10)));
}

ShopExchangeCatalog& ShopExchangeCatalog::instance() {
  static ShopExchangeCatalog catalog;
  return catalog;
}

ShopExchangeCatalog::ShopExchangeCatalog() {
  loadRoot(embeddedRoot(), QStringLiteral("程序内置官方配置"), {}, nullptr);
}

bool ShopExchangeCatalog::loadRoot(const QJsonObject& root, const QString& source,
                                   const QDateTime& updatedAt, QString* error) {
  QList<ShopExchangeShop> parsedShops;
  const QJsonObject protocol = root.value(QStringLiteral("protocol")).toObject();
  const QString extension = protocol.value(QStringLiteral("extension")).toString();
  const QString command = protocol.value(QStringLiteral("getInfoCommand")).toString();
  if (extension.isEmpty() || command.isEmpty()) {
    if (error) *error = QStringLiteral("商店协议配置不完整");
    return false;
  }
  for (const QJsonValue& shopValue : root.value(QStringLiteral("shops")).toArray()) {
    const QJsonObject shopObject = shopValue.toObject();
    ShopExchangeShop shop;
    shop.shopId = shopObject.value(QStringLiteral("shopId")).toInt();
    shop.name = shopObject.value(QStringLiteral("name")).toString();
    shop.siKey = shopObject.value(QStringLiteral("siKey")).toString();
    if (shop.siKey.isEmpty()) shop.siKey = QStringLiteral("si%1").arg(shop.shopId);
    for (const QJsonValue& goodValue : shopObject.value(QStringLiteral("goods")).toArray()) {
      const QJsonObject goodObject = goodValue.toObject();
      ShopExchangeGood good;
      good.shopId = shop.shopId;
      good.shopName = shop.name;
      good.itemServerId = goodObject.value(QStringLiteral("itemServerId")).toInt();
      good.tab = goodObject.value(QStringLiteral("tab")).toInt();
      good.description = goodObject.value(QStringLiteral("description")).toString();
      good.limitText = goodObject.value(QStringLiteral("limit")).toString();
      good.limitKey = goodObject.value(QStringLiteral("limitKey")).toString();
      good.limitLabel = goodObject.value(QStringLiteral("limitLabel")).toString();
      good.limitCount = goodObject.value(QStringLiteral("limitCount")).toInt();
      good.cost = goodObject.value(QStringLiteral("cost")).toString();
      good.enhanceType = goodObject.value(QStringLiteral("enhanceType")).toString();
      good.unlock = goodObject.value(QStringLiteral("unlock")).toString();
      good.tag = goodObject.value(QStringLiteral("tag")).toString();
      good.raceIds = intList(goodObject.value(QStringLiteral("raceIds")));
      good.shelfDate = parseYmd(goodObject.value(QStringLiteral("shelfTime")).toString());
      good.removalDate = parseYmd(goodObject.value(QStringLiteral("removalTime")).toString());
      good.hasRemovalDate = good.removalDate.isValid();
      if (good.itemServerId > 0 && !good.enhanceType.isEmpty() && !good.raceIds.isEmpty())
        shop.goods.append(good);
    }
    if (shop.shopId > 0) parsedShops.append(shop);
  }
  if (parsedShops.isEmpty()) {
    if (error) *error = QStringLiteral("没有解析到可用的精灵培养兑换项目");
    return false;
  }
  root_ = root;
  shops_ = parsedShops;
  extension_ = extension;
  getInfoCommand_ = command;
  const QJsonValue params = protocol.value(QStringLiteral("getInfoParams"));
  getInfoParams_ = params.isObject()
                       ? QString::fromUtf8(QJsonDocument(params.toObject()).toJson(QJsonDocument::Compact))
                       : QStringLiteral("{}");
  activityId_ = protocol.value(QStringLiteral("activityId")).toInt();
  loaded_ = true;
  sourceLabel_ = source;
  sourceUpdatedAt_ = updatedAt;
  return true;
}

bool ShopExchangeCatalog::reloadFromDataRoot(const QString& dataRoot, QString* error) {
  const QString path = QDir(dataRoot).filePath(QStringLiteral("catalog/shop-exchange-data.json"));
  if (!QFileInfo::exists(path)) {
    if (error) *error = QStringLiteral("尚无外部商店配置，继续使用内置配置");
    return false;
  }
  return loadRoot(readObject(path), QStringLiteral("本地官方配置缓存"),
                  QFileInfo(path).lastModified(), error);
}

QString ShopExchangeCatalog::findOfficialConfig(QString* error) {
  QString root = qEnvironmentVariable("KQPET_OFFICIAL_UNPACK_ROOT");
  if (root.isEmpty()) root = QStringLiteral("D:/奥奇工程/奥奇传说解包");
  if (!QFileInfo::exists(root)) {
    if (error) *error = QStringLiteral("找不到官方解包目录：%1").arg(root);
    return {};
  }
  QString best;
  QDateTime bestTime;
  QDirIterator iterator(root, {QStringLiteral("SEFConfig.as")}, QDir::Files,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString path = iterator.next();
    if (!path.contains(QStringLiteral("storeexchangeframework"), Qt::CaseInsensitive))
      continue;
    const QDateTime modified = QFileInfo(path).lastModified();
    if (best.isEmpty() || modified > bestTime || (modified == bestTime && path > best)) {
      best = path;
      bestTime = modified;
    }
  }
  if (best.isEmpty() && error)
    *error = QStringLiteral("官方解包中没有找到商店兑换框架的 SEFConfig.as");
  return best;
}

QJsonObject ShopExchangeCatalog::parseOfficialConfig(const QString& path,
                                                      const QJsonObject& protocol,
                                                      QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (error) *error = QStringLiteral("无法读取官方配置：%1").arg(path);
    return {};
  }
  const QString text = QString::fromUtf8(file.readAll());
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
      QStringLiteral("CommonEnhancePrize,[^,]+,[^,]+,([^,]+),([^,\\\"]+)"));
  QJsonArray shops;
  for (int shopId = 1; shopId <= 6; ++shopId) {
    const QString marker =
        QStringLiteral("public static const SERVER_ID_%1_REWARD_CONFIG:Array = ").arg(shopId);
    const int begin = text.indexOf(marker);
    if (begin < 0) continue;
    const int bodyBegin = begin + marker.size();
    int bodyEnd = text.indexOf(QStringLiteral("public static const"), bodyBegin);
    if (bodyEnd < 0) bodyEnd = text.size();
    const QString body = text.mid(bodyBegin, bodyEnd - bodyBegin);
    QJsonArray goods;
    QRegularExpressionMatchIterator matches = objectExpression.globalMatch(body);
    while (matches.hasNext()) {
      const QString block = matches.next().captured(1);
      const QString simple = asField(block, QStringLiteral("simpleParams"));
      const QRegularExpressionMatch prize = prizeExpression.match(simple);
      if (!prize.hasMatch()) continue;
      const QString enhanceType = prize.captured(1).trimmed();
      const QJsonArray races = raceArray(prize.captured(2).trimmed());
      if (enhanceType.isEmpty() || races.isEmpty()) continue;
      const QString limit = asField(block, QStringLiteral("limit"));
      const int limitIndex = limit.section(QLatin1Char(':'), 0, 0).toInt();
      const int limitCount = limit.section(QLatin1Char(':'), 1, 1).toInt();
      goods.append(QJsonObject{
          {QStringLiteral("id"), asField(block, QStringLiteral("id")).toInt()},
          {QStringLiteral("itemServerId"), asField(block, QStringLiteral("serverId")).toInt()},
          {QStringLiteral("tab"), asField(block, QStringLiteral("tab")).toInt()},
          {QStringLiteral("description"), asField(block, QStringLiteral("basicDescription"))},
          {QStringLiteral("shelfTime"), asField(block, QStringLiteral("shelfTime"))},
          {QStringLiteral("removalTime"), asField(block, QStringLiteral("removalTime"))},
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
    shops.append(QJsonObject{{QStringLiteral("shopId"), shopId},
                             {QStringLiteral("name"), shopNames.value(shopId)},
                             {QStringLiteral("siKey"), QStringLiteral("si%1").arg(shopId)},
                             {QStringLiteral("goods"), goods}});
  }
  return QJsonObject{{QStringLiteral("schema"), 2},
                     {QStringLiteral("generatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                     {QStringLiteral("sourceFile"), path},
                     {QStringLiteral("protocol"), protocol},
                     {QStringLiteral("shops"), shops}};
}

bool ShopExchangeCatalog::updateFromOfficialData(const QString& dataRoot, QString* error) {
  QString sourceError;
  const QString source = findOfficialConfig(&sourceError);
  if (source.isEmpty()) {
    if (error) *error = sourceError;
    return false;
  }
  QString parseError;
  const QJsonObject parsed = parseOfficialConfig(
      source, root_.value(QStringLiteral("protocol")).toObject(), &parseError);
  if (parsed.isEmpty()) {
    if (error) *error = parseError;
    return false;
  }
  const QJsonObject oldRoot = root_;
  const QString oldSource = sourceLabel_;
  const QDateTime oldTime = sourceUpdatedAt_;
  if (!loadRoot(parsed, QStringLiteral("官方解包动态配置"),
                QFileInfo(source).lastModified(), &parseError)) {
    loadRoot(oldRoot, oldSource, oldTime, nullptr);
    if (error) *error = parseError;
    return false;
  }
  const QString target = QDir(dataRoot).filePath(QStringLiteral("catalog/shop-exchange-data.json"));
  if (!writeObject(target, parsed)) {
    loadRoot(oldRoot, oldSource, oldTime, nullptr);
    if (error) *error = QStringLiteral("无法原子写入商店配置缓存：%1").arg(target);
    return false;
  }
  return true;
}

QDate ShopExchangeCatalog::parseYmd(const QString& text) {
  if (text.size() != 8) return {};
  return QDate::fromString(text, QStringLiteral("yyyyMMdd"));
}

QList<ShopExchangeShop> ShopExchangeCatalog::shops(const QDate& date) const {
  QList<ShopExchangeShop> result;
  for (const ShopExchangeShop& shop : shops_) {
    ShopExchangeShop copy;
    copy.shopId = shop.shopId;
    copy.name = shop.name;
    copy.siKey = shop.siKey;
    for (const ShopExchangeGood& good : shop.goods)
      if (good.isOnlineOn(date)) copy.goods.append(good);
    result.append(copy);
  }
  return result;
}

QList<ShopExchangeGood> ShopExchangeCatalog::onlineGoods(const QDate& date) const {
  QList<ShopExchangeGood> result;
  for (const ShopExchangeShop& shop : shops(date)) result.append(shop.goods);
  return result;
}

QJsonObject ShopExchangeCatalog::objectValue(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  return value.isObject() ? value.toObject() : QJsonObject{};
}

QJsonObject ShopExchangeCatalog::itemObject(const QJsonObject& packet,
                                            const ShopExchangeGood& good) {
  QJsonObject root = packet;
  const QString shopKey = QStringLiteral("si%1").arg(good.shopId);
  if (!root.value(shopKey).isObject()) {
    for (const QString& wrapper : {QStringLiteral("data"), QStringLiteral("p"),
                                   QStringLiteral("params")}) {
      const QJsonObject candidate = objectValue(packet, wrapper);
      if (candidate.value(shopKey).isObject()) { root = candidate; break; }
    }
  }
  const QJsonObject shop = objectValue(root, shopKey);
  QJsonObject item = objectValue(shop, good.itemKey());
  if (item.isEmpty()) item = objectValue(shop, QStringLiteral("b%1").arg(good.itemServerId));
  if (item.isEmpty()) item = objectValue(root, good.itemKey());
  if (item.isEmpty()) item = objectValue(root, QStringLiteral("b%1").arg(good.itemServerId));
  return item;
}

int ShopExchangeCatalog::usedCount(const QJsonObject& packet, const ShopExchangeGood& good) {
  if (good.limitKey.isEmpty()) return 0;
  const QJsonValue value = itemObject(packet, good).value(good.limitKey);
  if (value.isDouble()) return value.toInt();
  if (value.isString()) return value.toString().toInt();
  return 0;
}

int ShopExchangeCatalog::remainingCount(const QJsonObject& packet,
                                        const ShopExchangeGood& good) {
  return qMax(0, good.limitCount - usedCount(packet, good));
}
