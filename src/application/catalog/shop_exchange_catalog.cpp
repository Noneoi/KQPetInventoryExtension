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
std::atomic<quint64> shopRevisions{0};

bool excludedActivity(const QString& sourceKey) {
  static const QSet<QString> excluded{QStringLiteral("godfantasynuoyachallenge"), QStringLiteral("eternalbattlefield"),
      QStringLiteral("shenyundaqiaoevo"), QStringLiteral("lingchushenandishitianchallenge")};
  return excluded.contains(sourceKey.section(QLatin1Char('#'), 0, 0).section(QLatin1Char('/'), -1));
}

QString cultivationDescription(const QString& description, QString codes, const QString& reward, const QJsonObject& support) {
  const auto split = description.indexOf(QStringLiteral(" · "));
  const auto heading = split < 0 ? description : description.left(split);
  const auto suffix = split < 0 ? QString{} : description.mid(split);
  static const QRegularExpression generic(QStringLiteral("^(?:指定(?:精灵)?|精灵|通用|专属|完美|超完美|至尊|奖励|项目|养成|培养|强化|提升|套餐|\\s)*$"));
  const bool bundle = (heading.endsWith(QStringLiteral("极品")) || heading.endsWith(QStringLiteral("极品养成"))) &&
      !heading.contains(QRegularExpression(QStringLiteral("[#（(]")));
  const bool alternative = reward.startsWith(QStringLiteral("Choice,|")) || reward.startsWith(QStringLiteral("!Choice,|"));
  const bool isGeneric = generic.match(heading).hasMatch();
  if (alternative && description.contains(QStringLiteral("与材料奖励二选一"))) return description;
  if (!isGeneric && !bundle && !alternative && heading != QStringLiteral("养成内容待确认")) return description;
  if (codes.isEmpty() && reward.startsWith(QStringLiteral("Strengthen,"))) codes = reward.section(QLatin1Char(','), 1, 1);
  // Repair older caches before the next data update. Names/defaults below are
  // verified official BasicType/DescUtil values; fresh public definitions win.
  static const QHash<QString, QString> baseline{
      {QStringLiteral("11"),QStringLiteral("满级")}, {QStringLiteral("31"),QStringLiteral("满金星+万变金星")},
      {QStringLiteral("32"),QStringLiteral("满红星+万变红星")}, {QStringLiteral("33"),QStringLiteral("万变红星")},
      {QStringLiteral("34"),QStringLiteral("1个红色星神")}, {QStringLiteral("39"),QStringLiteral("1个红色星神")},
      {QStringLiteral("35"),QStringLiteral("1个指定红色星神")}, {QStringLiteral("83"),QStringLiteral("激活1个专属星迹")},
      {QStringLiteral("41"),QStringLiteral("满级元魂")}, {QStringLiteral("42"),QStringLiteral("元魂觉醒")},
      {QStringLiteral("44"),QStringLiteral("1个觉醒元魂")}, {QStringLiteral("62"),QStringLiteral("完美天赋")},
      {QStringLiteral("85"),QStringLiteral("满普通星迹")}, {QStringLiteral("86"),QStringLiteral("满星迹")},
      {QStringLiteral("89"),QStringLiteral("星迹突破")}, {QStringLiteral("91"),QStringLiteral("源兽神觉升至满星")},
      {QStringLiteral("92"),QStringLiteral("源兽神觉提升#level#阶")}, {QStringLiteral("94"),QStringLiteral("源兽神觉升至满阶")}};
  static const QRegularExpression tokenPattern(QStringLiteral("^([1-9][0-9]*)(?:\\$([1-9][0-9]*))?$"));
  QStringList effects;
  const auto names = support.value(QStringLiteral("descriptions")).toObject();
  const auto parameters = support.value(QStringLiteral("descriptionParameters")).toObject();
  for (const auto& token : codes.split(QLatin1Char('-'))) {
    const auto match = tokenPattern.match(token);
    if (!match.hasMatch()) return description;
    const auto code = match.captured(1);
    auto name = names.value(code).toString(baseline.value(code));
    if (name.isEmpty()) return description;
    if (name.contains(QStringLiteral("#level#"))) {
      auto rule = parameters.value(code).toObject();
      if (rule.isEmpty() && code == QStringLiteral("92")) rule = {{QStringLiteral("index"),0},{QStringLiteral("default"),1}};
      if (!rule.contains(QStringLiteral("index")) || !rule.contains(QStringLiteral("default"))) return description;
      const auto level = !match.captured(2).isEmpty() && rule.value(QStringLiteral("index")).toInt() == 0
          ? match.captured(2) : QString::number(rule.value(QStringLiteral("default")).toInt());
      name.replace(QStringLiteral("#level#"),level);
    }
    if (name.contains(QRegularExpression(QStringLiteral("#[A-Za-z]+#")))) return description;
    effects.append(name);
  }
  const auto effect = effects.join(QStringLiteral(" / "));
  return (bundle || (alternative && !isGeneric) ? heading + QStringLiteral("（") + effect + QStringLiteral("）") : QStringLiteral("指定精灵：") + effect) +
      (alternative ? QStringLiteral("（与材料奖励二选一）") : QString{}) + suffix;
}

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

bool cultivationAlternative(const QJsonObject& row) {
  auto raw = row.value(QStringLiteral("rewardRaw")).toString();
  if (raw.startsWith(QLatin1Char('!'))) raw.remove(0, 1);
  if (!raw.startsWith(QStringLiteral("Choice,|")) || !row.value(QStringLiteral("rewardOptions")).toArray().isEmpty()) return false;
  const auto parts = raw.split(QLatin1Char('|'));
  if (parts.size() != 3) return false;
  static const QRegularExpression material(QStringLiteral("^(?:Material|BatchMaterial),[1-9][0-9]*:[1-9][0-9]*:[1-9][0-9]*(?:#[1-9][0-9]*:[1-9][0-9]*:[1-9][0-9]*)*(?:,[1-9][0-9]*)?$"));
  static const QRegularExpression package(QStringLiteral("(?:^|[,#])139:"));
  static const QRegularExpression codes(QStringLiteral("^[1-9][0-9]*(?:\\$[1-9][0-9]*)?(?:-[1-9][0-9]*(?:\\$[1-9][0-9]*)?)*$"));
  static const QRegularExpression targets(QStringLiteral("^[1-9][0-9]*(?:#[1-9][0-9]*)*(?::(?:false|0))?$"));
  QString enhance;
  int materials = 0;
  for (const auto& part : parts.mid(1)) {
    if (part.startsWith(QStringLiteral("CommonEnhancePrize,"))) enhance = part;
    else if (material.match(part).hasMatch() && !package.match(part).hasMatch()) ++materials;
  }
  if (enhance.isEmpty() || materials != 1) return false;
  const auto fields = enhance.split(QLatin1Char(','));
  if ((fields.size() != 5 && fields.size() != 6) || (fields.size() == 6 && fields[5] != QStringLiteral("VALID_ALL") && fields[5] != QStringLiteral("GAIN_BATCH")) ||
      !codes.match(fields[3]).hasMatch() || !targets.match(fields[4]).hasMatch() || fields[3] != row.value(QStringLiteral("enhanceType")).toString()) return false;
  QVector<int> races;
  for (const auto& token : fields[4].section(QLatin1Char(':'), 0, 0).split(QLatin1Char('#'))) {
    bool ok = false;
    const auto id = token.toInt(&ok);
    if (!ok || id <= 0) return false;
    if (!races.contains(id)) races.append(id);
  }
  return races == intList(row.value(QStringLiteral("raceIds")));
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

bool isDiamondCost(const QString& cost) {
  return cost.startsWith(QStringLiteral("8:2:"));
}

bool hasDiamondEvidence(const QJsonObject& good) {
  if (isDiamondCost(good.value(QStringLiteral("cost")).toString())) return true;
  for (const auto& value : good.value(QStringLiteral("priceOptions")).toArray())
    if (isDiamondCost(value.toObject().value(QStringLiteral("cost")).toString())) return true;
  return false;
}

bool validNavigationLink(const QString& link) {
  static const QRegularExpression expression(
      QStringLiteral("^btnNewAct_[A-Za-z][A-Za-z0-9]{1,100}(?:_[A-Za-z0-9]{1,64}){1,8}$"));
  return link.size() <= 256 && expression.match(link).hasMatch();
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
    if (excludedActivity(shop.sourceKey)) continue;
    if (!DomainNumeric::checkedCount(shopObject.value(QStringLiteral("shopId")), &shop.shopId) ||
        shop.shopId <= 0 || shopIds.contains(shop.sourceKey + QLatin1Char(':') + QString::number(shop.shopId)) ||
        !shopObject.value(QStringLiteral("name")).isString() ||
        !shopObject.value(QStringLiteral("goods")).isArray()) return invalid();
    shopIds.insert(shop.sourceKey + QLatin1Char(':') + QString::number(shop.shopId));
    shop.name = shopObject.value(QStringLiteral("name")).toString();
    shop.navigationLink = shopObject.value(QStringLiteral("navigationLink")).toString();
    shop.activityEvidence = shopObject.value(QStringLiteral("activityEvidence")).toString();
    shop.activityEvidenceDate = shopObject.value(QStringLiteral("activityEvidenceDate")).toString();
    if ((!shop.navigationLink.isEmpty() && !validNavigationLink(shop.navigationLink)) ||
        shop.activityEvidence.size() > 64 || shop.activityEvidenceDate.size() > 16) return invalid();
    // The six permanent SEF shops share the official activity panel.  Keeping
    // this fallback in the typed catalog also supports older on-disk catalogs.
    if (shop.sourceKey.isEmpty() && shop.navigationLink.isEmpty() && shop.shopId >= 1 && shop.shopId <= 6)
      shop.navigationLink = QStringLiteral("btnNewAct_storeexchangeframework_showMainPanel_%1").arg(shop.shopId);
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
      if (goodObject.value(QStringLiteral("availabilityClosed")).toBool()) continue;
      ShopExchangeGood good;
      good.shopId = shop.shopId;
      good.shopName = shop.name;
      good.sourceKey = shop.sourceKey;
      if (goodObject.contains(QStringLiteral("manualSelectionRequired")) &&
          !goodObject.value(QStringLiteral("manualSelectionRequired")).isBool()) return invalid();
      good.manualSelectionRequired = goodObject.value(QStringLiteral("manualSelectionRequired")).toBool();
      good.rewardRaw = goodObject.value(QStringLiteral("rewardRaw")).toString();
      static const QRegularExpression selectable(QStringLiteral("(?:^!?|\\|!?)(?:Choice|SelectPrizes),[^\\n]+"));
      static const QRegularExpression namedChoice(QStringLiteral("自选|任选|[二三四五六七八九十0-9]+选[一二三四五六七八九十0-9]+"));
      static const QRegularExpression targeted(QStringLiteral("^Strengthen,[1-9][0-9]*(?:\\$[1-9][0-9]*)?(?:-[1-9][0-9]*(?:\\$[1-9][0-9]*)?)*,"));
      bool hasSelectablePackage = false;
      for (const auto& value : goodObject.value(QStringLiteral("selectablePackageIds")).toArray()) {
        int id = 0;
        if (!DomainNumeric::checkedCount(value, &id) || id <= 0) return invalid();
        const QRegularExpression package(QStringLiteral("(?:^|[,#])139:%1:[1-9][0-9]*(?:[,#|]|$)").arg(id));
        hasSelectablePackage |= package.match(good.rewardRaw).hasMatch();
      }
      const bool alternative = cultivationAlternative(goodObject);
      if (!alternative && (selectable.match(good.rewardRaw).hasMatch() || hasSelectablePackage ||
          !goodObject.value(QStringLiteral("rewardOptions")).toArray().isEmpty() ||
          namedChoice.match(goodObject.value(QStringLiteral("description")).toString()).hasMatch())) continue;
      if (alternative) good.manualSelectionRequired = true;
      // Older saved scans included generic materials and invented activity
      // entries. Hide these immediately, including previously selected rows.
      if (good.manualSelectionRequired &&
          (goodObject.value(QStringLiteral("enhanceType")).toString().isEmpty() ||
           goodObject.value(QStringLiteral("raceIds")).toArray().isEmpty()) &&
          !targeted.match(good.rewardRaw).hasMatch()) continue;
      good.acquisitionKind = goodObject.value(QStringLiteral("acquisitionKind")).toString();
      if (!QStringList{QString{},QStringLiteral("exchange"),QStringLiteral("signin"),QStringLiteral("progress"),
          QStringLiteral("reward"),QStringLiteral("lottery"),QStringLiteral("activity"),QStringLiteral("unknown")}.contains(good.acquisitionKind) ||
          (!good.acquisitionKind.isEmpty() && good.acquisitionKind != QStringLiteral("exchange") && !good.manualSelectionRequired)) return invalid();
      good.costDescription = goodObject.value(QStringLiteral("costDescription")).toString();
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
      if (good.sourceKey.isEmpty()) {
        good.section = ShopExchangeSection::Permanent;
      } else {
        const QString kind = goodObject.value(QStringLiteral("exchangeKind")).toString();
        if (!kind.isEmpty() && kind != QStringLiteral("activity") && kind != QStringLiteral("diamond"))
          return invalid();
        const bool diamond = hasDiamondEvidence(goodObject);
        // Old caches did not carry exchangeKind, so derive only from a concrete
        // 8:2 price.  A declared diamond kind must have the same evidence.
        if (kind == QStringLiteral("diamond") && !diamond) return invalid();
        good.section = diamond ? ShopExchangeSection::DiamondActivity
                               : ShopExchangeSection::ActivityShop;
      }
      if (!DomainNumeric::checkedCount(goodObject.value(QStringLiteral("itemServerId")),
                        &good.itemServerId) || (shop.sourceKey.isEmpty() && !good.manualSelectionRequired && good.itemServerId <= 0) ||
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
      good.quotaDescription = goodObject.value(QStringLiteral("quotaDescription")).toString();
      if (!DomainNumeric::checkedCount(goodObject.value(QStringLiteral("limitCount")),
                        &good.limitCount)) good.limitCount = -1;
      good.cost = goodObject.value(QStringLiteral("cost")).toString();
      if (goodObject.value(QStringLiteral("costKnown")).isBool() &&
          !goodObject.value(QStringLiteral("costKnown")).toBool()) good.cost.clear();
      good.enhanceType = goodObject.value(QStringLiteral("enhanceType")).toString();
      good.description = cultivationDescription(good.description, good.enhanceType, good.rewardRaw,
                                                root.value(QStringLiteral("rewardDescriptions")).toObject());
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
      if (good.raceIds.isEmpty() && !(good.manualSelectionRequired &&
          goodObject.value(QStringLiteral("raceIds")).isArray() &&
          goodObject.value(QStringLiteral("raceIds")).toArray().isEmpty() &&
          good.enhanceType.isEmpty() && !good.rewardRaw.isEmpty())) return invalid();
      good.shelfDate = parseYmd(goodObject.value(QStringLiteral("shelfTime")).toString());
      if (!good.shelfDate.isValid() && (good.sourceKey.isEmpty() || !goodObject.value(QStringLiteral("shelfTime")).toString().isEmpty())) return invalid();
      const QJsonValue removal = goodObject.value(QStringLiteral("removalTime"));
      if (!removal.isUndefined() && !removal.isString()) return invalid();
      good.removalDate = parseYmd(goodObject.value(QStringLiteral("removalTime")).toString());
      good.hasRemovalDate = good.removalDate.isValid();
      if (!removal.toString().isEmpty() && !good.hasRemovalDate) return invalid();
      if (good.hasRemovalDate && good.shelfDate.isValid() && good.removalDate < good.shelfDate) return invalid();
      for (const auto& field : {QStringLiteral("startsAt"), QStringLiteral("endsAt")}) {
        const auto raw = goodObject.value(field);
        if (!raw.isUndefined() && !raw.isString()) return invalid();
        if (raw.toString().isEmpty()) continue;
        const auto instant = QDateTime::fromString(raw.toString(), Qt::ISODate);
        if (!instant.isValid() || !raw.toString().endsWith(QStringLiteral("+08:00"))) return invalid();
        if (field == QStringLiteral("startsAt")) good.startsAt = instant;
        else good.endsAt = instant;
      }
      // Older scans kept full official timestamps but displayed just the date.
      for (const auto& field : {QStringLiteral("officialShelfTime"), QStringLiteral("officialRemovalTime")}) {
        auto& instant = field == QStringLiteral("officialShelfTime") ? good.startsAt : good.endsAt;
        const auto raw = goodObject.value(field).toString();
        if (instant.isValid() || !raw.contains(QLatin1Char(' '))) continue;
        auto parsed = QDateTime::fromString(raw, QStringLiteral("yyyyMMdd HH:mm:ss"));
        if (!parsed.isValid()) parsed = QDateTime::fromString(raw, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (parsed.isValid()) instant = QDateTime(parsed.date(), parsed.time(), Qt::OffsetFromUTC, 8 * 3600);
      }
      if (good.startsAt.isValid() && good.endsAt.isValid() && good.startsAt >= good.endsAt) continue;
      shop.goods.append(good);
      ++totalGoods;
    }
    if (!shop.goods.isEmpty()) parsedShops.append(shop);
  }
  if (parsedShops.isEmpty() || totalGoods == 0) {
    if (error) *error = QStringLiteral("没有解析到可用的精灵培养兑换项目");
    return {};
  }
  std::sort(parsedShops.begin(), parsedShops.end(),
      [](const ShopExchangeShop& left, const ShopExchangeShop& right) {
        return left.sourceKey == right.sourceKey ? left.shopId < right.shopId : left.sourceKey < right.sourceKey;
      });
  auto next = std::make_shared<ShopCatalogSnapshot>();
  next->root = root;
  QJsonArray visiblePending;
  for (const auto& value : root.value(QStringLiteral("activityPending")).toArray())
    if (!excludedActivity(value.toObject().value(QStringLiteral("module")).toString())) visiblePending.append(value);
  next->root.insert(QStringLiteral("activityPending"), visiblePending);
  next->discoveredShops = parsedShops;
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
  return withManualSelection(next, {});
}

std::shared_ptr<const ShopCatalogSnapshot> ShopExchangeCatalog::withManualSelection(
    std::shared_ptr<const ShopCatalogSnapshot> catalog, const QSet<QString>& selected,
    const QSet<QString>& excludedAutomatic) {
  auto next = std::make_shared<ShopCatalogSnapshot>(*catalog);
  next->revision = ++shopRevisions;
  next->manualSelection = selected;
  next->excludedAutomatic = excludedAutomatic;
  next->allShops.clear();
  for (auto shop : next->discoveredShops) {
    shop.goods.erase(std::remove_if(shop.goods.begin(), shop.goods.end(), [&](const auto& good) {
      return excludedAutomatic.contains(good.stableKey()) ||
          (good.manualSelectionRequired && !selected.contains(good.stableKey()));
    }), shop.goods.end());
    if (!shop.goods.isEmpty()) next->allShops.append(std::move(shop));
  }
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
    copy.navigationLink = shop.navigationLink;
    copy.activityEvidence = shop.activityEvidence;
    copy.activityEvidenceDate = shop.activityEvidenceDate;
    for (const ShopExchangeGood& good : shop.goods)
      if (good.isOnlineOn(date)) copy.goods.append(good);
    // Keep permanent protocol groups for quota observations even when their
    // visible reward list is empty. UI views omit empty shops themselves.
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
