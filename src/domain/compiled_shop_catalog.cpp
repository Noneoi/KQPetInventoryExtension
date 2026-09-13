#include "compiled_shop_catalog.h"

#include <QSet>
#include <QJsonDocument>
#include <algorithm>
#include <limits>

namespace {

quint64 textBytes(const QString& value) {
  return value.isEmpty() ? 0 : 64 + quint64(value.size()) * sizeof(QChar);
}
quint64 conditionBytes(const ShopCondition& value) {
  return textBytes(value.reason) + textBytes(value.source);
}
quint64 goodPayloadBytes(const CompiledShopGood& value) {
  const auto& good = value.good;
  quint64 bytes = textBytes(good.shopName) + textBytes(good.description) + textBytes(good.cost) +
      textBytes(good.enhanceType) + textBytes(good.unlock) + textBytes(good.limitKey) +
      textBytes(good.limitText) + textBytes(good.limitLabel) + textBytes(good.tag) +
      textBytes(good.provenGapCode) + textBytes(good.sourceKey) + textBytes(good.sourceUrl) + textBytes(good.costDescription) +
      quint64(QJsonDocument(good.activityQueries).toJson(QJsonDocument::Compact).size() +
        QJsonDocument(good.quotaObservation).toJson(QJsonDocument::Compact).size() +
        QJsonDocument(good.observationWhen).toJson(QJsonDocument::Compact).size() +
        QJsonDocument(good.activityCosts).toJson(QJsonDocument::Compact).size() +
        QJsonDocument(good.priceOptions).toJson(QJsonDocument::Compact).size()) * 4 + quint64(good.raceIds.capacity()) * sizeof(int) +
      textBytes(value.stableKey) + conditionBytes(value.costCondition) +
      quint64(value.raceIds.size()) * 64 + quint64(value.raceIds.capacity()) * sizeof(int) +
      quint64(value.requirements.capacity()) * sizeof(ResourceRequirement) +
      quint64(value.petRule.components.capacity()) * sizeof(ShopPetComponentRule);
  for (const auto& requirement : value.requirements)
    bytes += textBytes(requirement.resourceKey) + textBytes(requirement.resourceName) + conditionBytes(requirement.condition);
  for (const auto& component : value.petRule.components)
    bytes += textBytes(component.code) + textBytes(component.name);
  return bytes;
}

bool positiveInteger(const QString& text, qint64 maximum, qint64* result) {
  if (text.isEmpty()) return false;
  qint64 value = 0;
  for (const QChar character : text) {
    if (character < QLatin1Char('0') || character > QLatin1Char('9'))
      return false;
    const int digit = character.unicode() - '0';
    if (value > (maximum - digit) / 10) return false;
    value = value * 10 + digit;
  }
  if (value <= 0) return false;
  *result = value;
  return true;
}

bool parseRequirements(const QString& cost,
                       QList<ResourceRequirement>* requirements,
                       const QHash<QString, QString>& names,
                       AlgorithmPipelineStats* stats) {
  QString normalized = cost;
  normalized.replace(QLatin1Char('|'), QLatin1Char('#'));
  QHash<QString, int> indexes;
  QList<ResourceRequirement> parsed;
  // Keep empty parts: doubled / trailing delimiters are malformed too.
  for (const QString& part : normalized.split(QLatin1Char('#'))) {
    if (stats) ++stats->costFragmentsParsed;
    const QStringList fields = part.split(QLatin1Char(':'));
    qint64 type = 0;
    qint64 id = 0;
    qint64 required = 0;
    if (fields.size() != 3 ||
        !positiveInteger(fields.at(0), std::numeric_limits<int>::max(), &type) ||
        !positiveInteger(fields.at(1), std::numeric_limits<int>::max(), &id) ||
        !positiveInteger(fields.at(2), std::numeric_limits<qint64>::max(),
                         &required))
      return false;
    const QString key = QStringLiteral("%1:%2").arg(type).arg(id);
    const auto existing = indexes.constFind(key);
    if (existing != indexes.cend()) {
      ResourceRequirement& requirement = parsed[*existing];
      if (requirement.required > std::numeric_limits<qint64>::max() - required)
        return false;
      requirement.required += required;
    } else {
      ResourceRequirement requirement;
      requirement.resourceKey = key;
      requirement.resourceName = names.value(key, key);
      requirement.required = required;
      indexes.insert(key, parsed.size());
      parsed.append(requirement);
    }
  }
  // Canonical resource order is independent of cost fragment order.
  std::sort(parsed.begin(), parsed.end(),
            [](const ResourceRequirement& a, const ResourceRequirement& b) {
              return a.resourceKey < b.resourceKey;
            });
  *requirements = parsed;
  return !parsed.isEmpty();
}


}  // namespace

CompiledShopCatalog CompiledShopCatalog::compile(
    const QList<ShopExchangeGood>& goods,
    const QHash<QString, QString>& materialNames, AlgorithmPipelineStats* stats) {
  CompiledShopCatalog result;
  result.goods_.reserve(goods.size());
  result.allGoodIndexes_.reserve(goods.size());
  for (const ShopExchangeGood& good : goods) result.appendGood(good, materialNames, stats);
  return result;
}

void CompiledShopCatalog::appendGood(const ShopExchangeGood& good,
    const QHash<QString, QString>& materialNames, AlgorithmPipelineStats* stats) {
  CompiledShopGood compiled;
  compiled.good = good;
  compiled.stableKey = good.stableKey();
  compiled.petRule = compileShopPetRule(good.enhanceType, stats);
  if (stats) {
    ++stats->goodsCompiled;
    ++stats->costExpressionsParsed;
  }
  const bool costKnown = good.cost.isEmpty() ? good.provenFree
      : parseRequirements(good.cost, &compiled.requirements, materialNames, stats);
  compiled.costCondition = {
      costKnown ? ShopConditionState::Satisfied : ShopConditionState::Unknown,
      costKnown ? QStringLiteral("目录成本已完整解析")
                : QStringLiteral("成本为空或包含无效片段、数量或溢出，无法确认"),
      QStringLiteral("商品目录")};
  const qsizetype index = goods_.size();
  QSet<int> seen;
  for (const int race : good.raceIds) {
    if (race <= 0 || seen.contains(race)) continue;
    seen.insert(race);
    auto& indexes = byRace_[race];
    const auto oldCapacity = indexes.capacity();
    indexes.append(index);
    raceIndexPayloadBytes_ += quint64(indexes.capacity() - oldCapacity) * sizeof(qsizetype);
    if (stats) ++stats->raceAssociationsIndexed;
  }
  compiled.raceIds = seen;
  goodPayloadBytes_ += goodPayloadBytes(compiled);
  allGoodIndexes_.append(index);
  goods_.append(compiled);

}

CompiledShopCatalog CompiledShopCatalog::withMaterialNames(
    const QHash<QString, QString>& names) const {
  CompiledShopCatalog result = *this;
  for (CompiledShopGood& good : result.goods_) {
    result.goodPayloadBytes_ -= goodPayloadBytes(good);
    for (ResourceRequirement& requirement : good.requirements)
      requirement.resourceName = names.value(requirement.resourceKey,
                                               requirement.resourceName);
    result.goodPayloadBytes_ += goodPayloadBytes(good);
  }
  return result;
}

const QList<qsizetype>& CompiledShopCatalog::goodsForRace(int raceId) const {
  static const QList<qsizetype> empty;
  const auto found = byRace_.constFind(raceId);
  return found == byRace_.cend() ? empty : *found;
}

quint64 CompiledShopCatalog::retainedBytes() const {
  return sizeof(CompiledShopCatalog) + goodPayloadBytes_ + raceIndexPayloadBytes_ +
      quint64(goods_.capacity()) * sizeof(CompiledShopGood) +
      quint64(allGoodIndexes_.capacity()) * sizeof(qsizetype) +
      quint64(byRace_.size()) * 64 + quint64(byRace_.capacity()) * (sizeof(int) + sizeof(QList<qsizetype>) + 16);
}
