#include "performance_dataset.h"
#include "shop_exchange_catalog.h"
#include "pet_power_calculator.h"
#include "asset_analyzer.h"
#include "packet_contract.h"

#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <random>
#include <numeric>

namespace {
constexpr int kRaceBase = 900000;
constexpr int kUnmatchedRaceBase = 1900000;
const QDate kBusinessDate(2026, 9, 9);
const QDateTime kObservedAt(QDate(2026, 9, 9), QTime(10, 0), Qt::UTC);
QStringList enhancementCodes() {
  return {QStringLiteral("41"), QStringLiteral("62"), QStringLiteral("84"), QStringLiteral("11"),
          QStringLiteral("31"), QStringLiteral("34"), QStringLiteral("44"), QStringLiteral("91"),
          QStringLiteral("92"), QStringLiteral("999")};
}
int desiredCandidates(const PerformanceCase& spec, int goods) {
  if (spec.match == QStringLiteral("zero")) return 0;
  if (spec.match == QStringLiteral("full")) return goods;
  return qMin(goods, spec.match == QStringLiteral("sparse") ? 5 : 10);
}
QJsonObject cultivation(qint64 id, int race, const QString& name, quint32 salt) {
  QJsonObject current, maximum;
  int now = 0, target = 0;
  const QStringList components{QStringLiteral("lv"), QStringLiteral("iv"), QStringLiteral("pl"),
      QStringLiteral("ep"), QStringLiteral("gsv"), QStringLiteral("lav"), QStringLiteral("lsv"),
      QStringLiteral("bsv"), QStringLiteral("asv"), QStringLiteral("sjv"), QStringLiteral("sgv")};
  for (int index = 0; index < components.size(); ++index) {
    const int value = index == 0 ? 8500 : 100 + int((salt + index * 73) % 700);
    current.insert(components[index], value);
    maximum.insert(components[index], value + 150 + int(salt % 80));
    now += value; target += value + 150 + int(salt % 80);
  }
  return {{QStringLiteral("id"), id}, {QStringLiteral("r"), race}, {QStringLiteral("fr"), race},
      {QStringLiteral("n"), name}, {QStringLiteral("lv"), 120}, {QStringLiteral("zdl"), now},
      {QStringLiteral("xzdl"), target}, {QStringLiteral("czdlv"), current}, {QStringLiteral("mzdlv"), maximum},
      {QStringLiteral("sgs"), QStringLiteral("2001:8#2002:8#2003:8#0:8#0:8#2004:8#0:8#0:8")},
      {QStringLiteral("stargodSlotMaxLevel"), 8}, {QStringLiteral("sgsp"), QJsonArray{}},
      {QStringLiteral("sguln"), 16}, {QStringLiteral("badge"), QStringLiteral("101:5#201:0|102:4#202:1")},
      {QStringLiteral("shenjue"), QStringLiteral("1034#2#6|1:1")},
      {QStringLiteral("astrolabe"), QStringLiteral("11:1#12:2#13:0#14:1")}, {QStringLiteral("astrolabebr"), false},
      {QStringLiteral("ip"), QStringLiteral("25#26#30#20#24#28")},
      {QStringLiteral("gps"), QStringLiteral("50#60#75#30#35#40")}, {QStringLiteral("gt"), 4},
      {QStringLiteral("eps"), QStringLiteral("121:8:3#122:8:2#123:7:3#124:8:2")},
      {QStringLiteral("lss"), QStringLiteral("311:5#312:5#313:4#314:5")},
      {QStringLiteral("cps"), QStringLiteral("1:5#8:3")}, {QStringLiteral("ce"), 120000},
      {QStringLiteral("cte"), 360000}, {QStringLiteral("ne"), 150000}, {QStringLiteral("g"), int(salt % 2)}};
}
QJsonObject fullPet(const PerformanceDataset& data, const PetAssetRecord& summary, int index) {
  std::mt19937 random(data.spec.seed ^ quint32(index * 2654435761U));
  QJsonObject value = cultivation(summary.instanceId, summary.raceId, summary.name, random());
  for (auto it = summary.pet.constBegin(); it != summary.pet.constEnd(); ++it) value.insert(it.key(), it.value());
  value.insert(QStringLiteral("sppl"), cultivation(40000000 + index * 2, 6506,
      QStringLiteral("关系精灵·召唤样例"), random()));
  value.insert(QStringLiteral("cppl"), cultivation(40000001 + index * 2, 6510,
      QStringLiteral("关系精灵·携带样例"), random()));
  value.insert(QStringLiteral("srpi"), qint64(40000000 + index * 2));
  value.insert(QStringLiteral("cepi"), qint64(40000001 + index * 2));
  value.insert(QStringLiteral("crpis"), QString::number(summary.instanceId));
  value.insert(QStringLiteral("asps"), QJsonArray{qint64(40000000 + index * 2)});
  value.insert(QStringLiteral("acps"), QJsonArray{qint64(40000001 + index * 2)});
  // These bytes are actual cultivation input: calculatePetBattlePower traverses
  // and sorts this star inventory. No unused padding/string blob is appended.
  const qint64 targetBytes = qint64(data.spec.detailKiB) * 1024;
  qint64 bytes = QJsonDocument(value).toJson(QJsonDocument::Compact).size();
  QJsonArray stars;
  while (bytes < targetBytes && stars.size() < 8192) {
    stars.append(2001 + int(random() % 96));
    bytes += 5; // Every ID is four ASCII digits followed by a comma.
  }
  value.insert(QStringLiteral("sgsp"), stars);
  return value;
}
}

QString PerformanceCase::id() const {
  return QStringLiteral("p%1-g%2-%3-d%4-k%5-c%6-m%7")
      .arg(pets).arg(actualCatalog ? QStringLiteral("actual") : QString::number(goods))
      .arg(match).arg(detailPercent).arg(detailKiB).arg(actualCatalog ? 0 : costItems).arg(actualCatalog ? 0 : cultivationItems);
}
QJsonObject PerformanceCase::json() const {
  return {{QStringLiteral("caseId"), id()}, {QStringLiteral("pets"), pets},
      {QStringLiteral("goodsRequested"), actualCatalog ? QJsonValue(QStringLiteral("actual")) : QJsonValue(goods)},
      {QStringLiteral("matchRequested"), match}, {QStringLiteral("detailPercent"), detailPercent},
      {QStringLiteral("detailKiBRequested"), detailKiB}, {QStringLiteral("costItemsRequested"), actualCatalog ? 0 : costItems},
      {QStringLiteral("cultivationItemsRequested"), actualCatalog ? 0 : cultivationItems},
      {QStringLiteral("seed"), qint64(seed)}, {QStringLiteral("businessDate"), kBusinessDate.toString(Qt::ISODate)}};
}
QString PerformanceCase::validationError() const {
  if (pets < 1 || pets > 10000 || goods < 1 || goods > 1000) return QStringLiteral("pets must be 1..10000; goods 1..1000");
  if (!QStringList{QStringLiteral("zero"),QStringLiteral("sparse"),QStringLiteral("normal"),QStringLiteral("full")}.contains(match))
    return QStringLiteral("match must be zero, sparse, normal, or full");
  if (detailPercent != 0 && detailPercent != 50 && detailPercent != 100) return QStringLiteral("detail-percent must be 0, 50, or 100");
  if (detailKiB != 8 && detailKiB != 32) return QStringLiteral("detail-kib must be 8 or 32");
  for (int count : {costItems, cultivationItems}) if (count != 1 && count != 3 && count != 10)
    return QStringLiteral("cost/cultivation-items must be 1, 3, or 10");
  return {};
}
QList<PerformanceCase> performanceMatrix() {
  QList<PerformanceCase> result;
  const QList<QPair<int,int>> sizes{{2000,1},{2000,0},{1000,200},{2000,200},{10000,200},{2000,1000},{10000,1000}};
  for (const auto size : sizes) for (const QString& pattern : {QStringLiteral("zero"),QStringLiteral("sparse"),QStringLiteral("normal"),QStringLiteral("full")})
    for (int complete : {0,50,100}) for (int kib : {8,32}) for (int costs : {1,3,10}) for (int components : {1,3,10}) {
      if (size.second == 0 && (costs != 3 || components != 3)) continue;
      result.append({size.first, size.second ? size.second : 1, size.second == 0, pattern, complete, kib, costs, components});
    }
  return result;
}
std::shared_ptr<PerformanceDataset> createPerformanceDataset(const PerformanceCase& spec, const QString& directory) {
  auto data = std::make_shared<PerformanceDataset>();
  data->spec = spec; data->directory = directory;
  if (!(data->error = spec.validationError()).isEmpty()) return data;
  data->catalog = std::make_shared<ShopCatalogSnapshot>();
  data->catalog->revision = 1; data->catalog->loaded = true;
  data->catalog->sourceLabel = QStringLiteral("seed 20260909 synthetic performance fixture");
  if (spec.actualCatalog) {
    *data->catalog = *ShopExchangeCatalog::instance().snapshot();
    data->goods = ShopExchangeCatalog::instance().onlineGoods(kBusinessDate);
  } else {
    ShopExchangeShop shop; shop.shopId = 1; shop.name = QStringLiteral("合成压力目录");
    for (int index = 0; index < spec.goods; ++index) {
      ShopExchangeGood good;
      good.shopId = 1; good.itemServerId = index + 1; good.shopName = shop.name;
      good.description = QStringLiteral("合成培养项目 %1").arg(index + 1);
      good.limitKey = QStringLiteral("dl"); good.limitCount = 99;
      good.shelfDate = QDate(2020,1,1);
      good.enhanceType = enhancementCodes().mid(0, spec.cultivationItems).join(QLatin1Char('-'));
      QStringList costs;
      for (int item = 0; item < spec.costItems; ++item) costs.append(QStringLiteral("4:%1:%2").arg(100 + item).arg(20 + item));
      good.cost = costs.join(QLatin1Char('#'));
      const int matches = desiredCandidates(spec, spec.goods);
      // A circular race index makes exactly K candidates per pet without a
      // P x G table. A remains the real synthetic association count.
      for (int offset = 0; offset < matches; ++offset) good.raceIds.append(kRaceBase + (index + offset) % spec.goods);
      if (matches == 0) good.raceIds.append(kUnmatchedRaceBase + index);
      shop.goods.append(good); data->goods.append(good);
    }
    data->catalog->allShops.append(shop);
  }
  if (data->goods.isEmpty()) { data->error = QStringLiteral("frozen catalog has no online goods"); return data; }
  QCryptographicHash catalogHash(QCryptographicHash::Sha256);
  for (const auto& good : data->goods) {
    QJsonArray associations; for (int race : good.raceIds) associations.append(race);
    const QJsonObject audited{{QStringLiteral("shopId"),good.shopId},{QStringLiteral("itemId"),good.itemServerId},
        {QStringLiteral("cost"),good.cost},{QStringLiteral("enhanceType"),good.enhanceType},{QStringLiteral("unlock"),good.unlock},
        {QStringLiteral("limitKey"),good.limitKey},{QStringLiteral("limitCount"),good.limitCount},
        {QStringLiteral("provenFree"),good.provenFree},{QStringLiteral("provenUnlimited"),good.provenUnlimited},
        {QStringLiteral("provenGapCode"),good.provenGapCode},{QStringLiteral("provenGapUnits"),good.provenGapUnitsPerExchange},
        {QStringLiteral("shelfDate"),good.shelfDate.toString(Qt::ISODate)},{QStringLiteral("removalDate"),good.removalDate.toString(Qt::ISODate)},
        {QStringLiteral("hasRemovalDate"),good.hasRemovalDate},{QStringLiteral("raceIds"),associations}};
    catalogHash.addData(QJsonDocument(audited).toJson(QJsonDocument::Compact));
    QSet<int> races;
    for (int race : good.raceIds) races.insert(race);
    for (int race : races) { ++data->matchingGoodsByRace[race]; ++data->raceAssociations; }
    const QString shopKey = QStringLiteral("si%1").arg(good.shopId);
    auto info = data->shopPacket.value(shopKey).toObject();
    info.insert(good.itemKey(), QJsonObject{{good.limitKey, 0}}); data->shopPacket.insert(shopKey, info);
    for (const QString& cost : good.cost.split(QLatin1Char('#'))) {
      const auto fields = cost.split(QLatin1Char(':'));
      if (fields.size() == 3) data->balances.insert(fields[0] + QLatin1Char(':') + fields[1], 1000000);
    }
  }
  data->catalogContentDigest = catalogHash.result();
  if (spec.actualCatalog && spec.match != QStringLiteral("zero")) {
    auto races = data->matchingGoodsByRace.keys();
    std::sort(races.begin(), races.end());
    const int desired = desiredCandidates(spec, data->goods.size());
    int distance = 100000;
    for (int race : races) distance = qMin(distance, qAbs(data->matchingGoodsByRace[race] - desired));
    for (int race : races) if (qAbs(data->matchingGoodsByRace[race] - desired) == distance) data->selectedRaces.append(race);
  } else for (int race = 0; race < qMax(1, spec.goods); ++race) data->selectedRaces.append(kRaceBase + race);
  for (int id = 2001; id <= 2096; ++id) {
    QJsonObject powers; for (int level = 1; level <= 8; ++level) powers.insert(QString::number(level), 60 * level + id % 50);
    data->metadata.stargods.insert(QString::number(id), QJsonObject{{QStringLiteral("quality"), id % 4 == 0 ? 6 : 5},
        {QStringLiteral("changeable"), false}, {QStringLiteral("battlePower"), powers}});
  }
  data->metadata.stargods.insert(QStringLiteral("80"), QJsonObject{{QStringLiteral("quality"), 6},
      {QStringLiteral("changeable"), false}, {QStringLiteral("battlePower"), QJsonObject{{QStringLiteral("8"), 700}}}});
  data->metadata.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}},
      {QStringLiteral("102"),QJsonObject{{QStringLiteral("maxLevel"),6}}}};
  QJsonObject items;
  for (auto it = data->balances.constBegin(); it != data->balances.constEnd(); ++it) if (it.key().startsWith(QStringLiteral("4:")))
    items.insert(it.key().mid(2), QJsonObject{{QStringLiteral("name"), QStringLiteral("合成材料 %1").arg(it.key().mid(2))}});
  data->materials = {{QStringLiteral("items"), items}};
  data->minimumCandidates = data->goods.size();
  int permutationStep = 7919;
  while (std::gcd(permutationStep,spec.pets) != 1) ++permutationStep;
  for (int index = 0; index < spec.pets; ++index) {
    PetAssetRecord pet;
    pet.instanceId = 10000000 + index;
    const int race = data->selectedRaces.at(index % data->selectedRaces.size());
    const bool primaryRoute = index % 2 == 0;
    pet.raceId = primaryRoute ? race : kUnmatchedRaceBase + 10000 + index;
    pet.name = QStringLiteral("性能精灵%1·星河守望者").arg(index);
    pet.location = index < 12 ? QStringLiteral("背包") : index % 3 ? QStringLiteral("普通仓库") : QStringLiteral("精英仓库");
    pet.detailAvailable = ((qint64(index) * permutationStep + spec.seed) % spec.pets) < spec.pets * spec.detailPercent / 100;
    pet.observationVerified = true; pet.metadataSlotMaxLevel = 8;
    pet.pet = {{QStringLiteral("id"), pet.instanceId}, {QStringLiteral("r"), pet.raceId},
        {QStringLiteral("n"), pet.name}, {QStringLiteral("lv"), 120},
        {QStringLiteral("_metaRaceId"), primaryRoute ? kUnmatchedRaceBase + 10000 + index : race},
        {QStringLiteral("_metaOriginalName"), QStringLiteral("星河守望者")},
        {QStringLiteral("_metaAttributes"), QStringLiteral("9")}, {QStringLiteral("_metaJobs"), QStringLiteral("1,8")},
        {QStringLiteral("_metaEra"), QStringLiteral("灵初")},
        {QStringLiteral("_location"), index < 12 ? QStringLiteral("backpack") : QStringLiteral("warehouse")}};
    if (pet.detailAvailable) data->detailedIndexes.append(index);
    const int candidates = data->matchingGoodsByRace.value(race);
    data->expectedPairs += candidates;
    if (pet.detailAvailable) data->expectedQualifiedPairs += candidates;
    data->minimumCandidates = qMin(data->minimumCandidates, candidates);
    data->maximumCandidates = qMax(data->maximumCandidates, candidates);
    data->summaries.append(std::move(pet));
  }
  auto metadataCatalog = std::make_shared<PetDetailCatalogSnapshot>();
  metadataCatalog->revision = 1; metadataCatalog->loaded = true;
  metadataCatalog->sourceLabel = QStringLiteral("tests/performance_dataset.cpp synthetic metadata seed 20260909");
  QJsonObject definitions;
  for (const auto& summary : data->summaries) {
    definitions.insert(QString::number(summary.raceId), QJsonObject{
        {QStringLiteral("name"), summary.name}, {QStringLiteral("stargodSlotMaxLevel"), 8},
        {QStringLiteral("attributes"), QStringLiteral("9")}, {QStringLiteral("jobs"), QStringLiteral("1,8")}});
  }
  metadataCatalog->root = data->materials;
  metadataCatalog->root.insert(QStringLiteral("pets"), definitions);
  metadataCatalog->root.insert(QStringLiteral("stargods"), data->metadata.stargods);
  metadataCatalog->root.insert(QStringLiteral("badges"), data->metadata.badges);
  metadataCatalog->contentDigest = QCryptographicHash::hash(
      QJsonDocument(metadataCatalog->root).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  data->metadataCatalog = std::move(metadataCatalog);
  return data;
}
QJsonObject performanceFixturePet(const PerformanceDataset& data, int index) {
  const auto& summary = data.summaries.at(index);
  return summary.detailAvailable ? fullPet(data,summary,index) : summary.pet;
}
bool writePerformanceFixtureBatch(PerformanceDataset& data, int maximumPets) {
  if (!data.error.isEmpty() || !QDir().mkpath(data.directory)) { if (data.error.isEmpty()) data.error = QStringLiteral("fixture directory creation failed"); return false; }
  const int end = qMin(data.spec.pets, data.nextPet + maximumPets);
  for (; data.nextPet < end; ++data.nextPet) {
    const auto& summary = data.summaries.at(data.nextPet);
    const QJsonObject object = performanceFixturePet(data,data.nextPet);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QCryptographicHash chain(QCryptographicHash::Sha256);
    chain.addData(data.fixtureContentChainDigest); chain.addData(bytes);
    data.fixtureContentChainDigest = chain.result();
    data.fixtureBytes += bytes.size();
    if (!summary.detailAvailable) continue;
    if (qAbs(bytes.size() - qint64(data.spec.detailKiB) * 1024) > 64) {
      data.error = QStringLiteral("fixture shape did not reach the requested complete-detail byte size"); return false;
    }
    data.fullDetailBytes += bytes.size();
    if (!data.fullDetailMinimumBytes || bytes.size() < data.fullDetailMinimumBytes) data.fullDetailMinimumBytes = bytes.size();
    data.fullDetailMaximumBytes = qMax(data.fullDetailMaximumBytes, qint64(bytes.size()));
    if (summary.location == QStringLiteral("背包")) {
      // Its already observed packet is durably saved by Repository. Never
      // replace that original under a different digest behind Repository.
      data.durableFileBytes += QFileInfo(QDir(data.directory).filePath(QString::number(summary.instanceId)+QStringLiteral(".json"))).size();
      continue;
    }
    // Seed only durable originals. Repository discovers and decodes these via
    // its normal per-record path after the measured manual analysis starts.
    const QByteArray durable = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 3},
        {QStringLiteral("account"), QStringLiteral("performance-fixture")},
        {QStringLiteral("instanceId"), QString::number(summary.instanceId)},
        {QStringLiteral("complete"), true}, {QStringLiteral("savedAt"), kObservedAt.toString(Qt::ISODate)},
        {QStringLiteral("pet"), object}}).toJson(QJsonDocument::Compact);
    data.durableFileBytes += durable.size();
    QSaveFile file(QDir(data.directory).filePath(QString::number(summary.instanceId) + QStringLiteral(".json")));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(durable) != durable.size() || !file.commit()) {
      data.error = QStringLiteral("fixture detail write failed: %1").arg(file.errorString()); return false;
    }
  }
  return data.nextPet == data.spec.pets;
}
QJsonObject PerformanceDataset::description() const {
  QJsonObject result = spec.json();
  result.insert(QStringLiteral("goodsActual"), goods.size()); result.insert(QStringLiteral("HByRace"), expectedPairs);
  result.insert(QStringLiteral("HExpectedWithCompleteDetails"), expectedQualifiedPairs);
  result.insert(QStringLiteral("AActual"), raceAssociations); result.insert(QStringLiteral("fullDetailsActual"), detailedIndexes.size());
  result.insert(QStringLiteral("utf8BytesActual"), fixtureBytes); result.insert(QStringLiteral("fullDetailUtf8Bytes"), fullDetailBytes);
  result.insert(QStringLiteral("fullDetailMinimumBytes"), fullDetailMinimumBytes); result.insert(QStringLiteral("fullDetailMaximumBytes"), fullDetailMaximumBytes);
  result.insert(QStringLiteral("fullDetailMeanBytes"), detailedIndexes.isEmpty() ? 0.0 : double(fullDetailBytes) / detailedIndexes.size());
  result.insert(QStringLiteral("durableFileUtf8Bytes"), durableFileBytes);
  result.insert(QStringLiteral("metadataContentSha256"), QString::fromLatin1(metadataCatalog->contentDigest.toHex()));
  result.insert(QStringLiteral("catalogRulesContentSha256"), QString::fromLatin1(catalogContentDigest.toHex()));
  result.insert(QStringLiteral("fixtureBodiesChainSha256"), QString::fromLatin1(fixtureContentChainDigest.toHex()));
  result.insert(QStringLiteral("fixtureHashDefinition"),QStringLiteral("SHA256(previousDigest || canonical generated body UTF8), ordered by fixture index; excludes Repository persistence wrapper"));
  result.insert(QStringLiteral("minimumCandidatesPerPet"), minimumCandidates); result.insert(QStringLiteral("maximumCandidatesPerPet"), maximumCandidates);
  result.insert(QStringLiteral("syntheticCatalog"), !spec.actualCatalog);
  result.insert(QStringLiteral("quotaValidityEvidence"),QStringLiteral("explicit synthetic frozen validity facts for business date 2026-09-09; not real server evidence"));
  QJsonObject costCounts, componentCounts;
  for (const auto& good : goods) {
    const QString costCount = QString::number(good.cost.isEmpty() ? 0 : good.cost.split(QLatin1Char('#')).size());
    const QString componentCount = QString::number(compileShopPetRule(good.enhanceType).components.size());
    costCounts.insert(costCount,costCounts.value(costCount).toInt()+1);
    componentCounts.insert(componentCount,componentCounts.value(componentCount).toInt()+1);
  }
  result.insert(QStringLiteral("goodsByActualCostItemCount"),costCounts);
  result.insert(QStringLiteral("goodsByActualCultivationItemCount"),componentCounts);
  result.insert(QStringLiteral("tenComponentBoundaryIncludesUnknownRule"), !spec.actualCatalog && spec.cultivationItems == 10);
  return result;
}
AnalysisWorkInput performanceInputTemplate(const PerformanceDataset& data, const AnalysisJobKey& key) {
  AnalysisWorkInput input; input.key = key; input.deriveOverview = true;
  input.catalogSnapshot = data.catalog; input.catalogDate = kBusinessDate; input.metadata = data.metadata;
  input.materialDefinitions = data.materials; input.shopPacket = data.shopPacket;
  input.resourceCounts = data.balances; input.resourceCountsKnown = true; input.sourceInvalidated = false;
  input.conditionContext.shopPacketKnown = true;
  input.conditionContext.shopSource = QStringLiteral("synthetic verified fixture");
  input.conditionContext.shopObservedAt = kObservedAt;
  input.conditionContext.shopFreshness = ShopConditionFreshness::Current;
  input.conditionContext.petSource = QStringLiteral("synthetic verified fixture");
  input.conditionContext.petObservedAt = kObservedAt;
  input.conditionContext.petFreshness = ShopConditionFreshness::Current;
  for (const auto& good : data.goods) if (!good.provenUnlimited) {
    input.conditionContext.quotaValidity.insert(shopQuotaValidityKey(good.shopId,good.limitKey),
        {ShopConditionState::Satisfied,
         QStringLiteral("synthetic validity 2026-09-09T00:00Z..2026-09-10T00:00Z (fixed benchmark)"),
         QStringLiteral("tests/performance_dataset.cpp synthetic period evidence"),kObservedAt,ShopConditionFreshness::Current});
  }
  auto& overview = input.overview;
  overview.account = key.account; overview.inputSessionEpoch = key.epoch; overview.inventoryRevision = key.versions.inventory;
  overview.sourceVerified = true; overview.inventoryUpdatedAt = kObservedAt; overview.totalPets = data.spec.pets;
  overview.backpackPets = qMin(12, data.spec.pets); overview.missingDetailPets = data.spec.pets - data.detailedIndexes.size();
  overview.pets = data.summaries;
  for (const auto& pet : data.summaries) if (pet.location == QStringLiteral("普通仓库")) ++overview.normalWarehousePets;
    else if (pet.location == QStringLiteral("精英仓库")) ++overview.eliteWarehousePets;
  return input;
}
QJsonObject pipelineStatistics(const AlgorithmPipelineStats& s) {
  return {{QStringLiteral("goodsCompiled"),s.goodsCompiled},{QStringLiteral("costExpressionsParsed"),s.costExpressionsParsed},
      {QStringLiteral("goodsReused"),s.goodsReused},{QStringLiteral("compiledCatalogCacheHits"),s.compiledCatalogCacheHits},
      {QStringLiteral("costFragmentsParsed"),s.costFragmentsParsed},{QStringLiteral("enhancementExpressionsParsed"),s.enhancementExpressionsParsed},
      {QStringLiteral("enhancementComponentsCompiled"),s.enhancementComponentsCompiled},{QStringLiteral("raceAssociationsIndexed"),s.raceAssociationsIndexed},
      {QStringLiteral("accountConditionsPrepared"),s.accountConditionsPrepared},{QStringLiteral("resourceBalancesChecked"),s.resourceBalancesChecked},
      {QStringLiteral("petsDerived"),s.petsDerived},{QStringLiteral("cultivationComponentsDerived"),s.cultivationComponentsDerived},
      {QStringLiteral("preparedFactsReused"),s.preparedFactsReused},{QStringLiteral("rawPowerCalculations"),s.rawPowerCalculations},
      {QStringLiteral("candidatePairsVisited"),s.candidatePairsVisited},{QStringLiteral("candidateComponentsChecked"),s.candidateComponentsChecked},
      {QStringLiteral("finalRowsMaterialized"),s.finalRowsMaterialized},{QStringLiteral("finalSortComparisons"),s.finalSortComparisons},
      {QStringLiteral("candidateComparisons"),s.candidateComparisons},{QStringLiteral("maximumLiveCandidates"),s.maximumLiveCandidates}};
}
QJsonObject performancePhaseProbes(const AnalysisWorkInput& input, const QList<ShopExchangeGood>& goods) {
  AlgorithmPipelineStats stats;
  QElapsedTimer timer; timer.start();
  QHash<QString,QString> materialNames{{QStringLiteral("134:1"),QStringLiteral("个人贡献币")},
                                    {QStringLiteral("134:2"),QStringLiteral("联盟资金")}};
  for (const QString& section : {QStringLiteral("items"),QStringLiteral("money")}) {
    const auto definitions = input.materialDefinitions.value(section).toObject();
    for (auto item = definitions.constBegin(); item != definitions.constEnd(); ++item) {
      const QString name = item.value().toObject().value(QStringLiteral("name")).toString();
      if (!name.isEmpty()) materialNames.insert((section == QStringLiteral("items") ? QStringLiteral("4:") : QStringLiteral("8:")) + item.key(),name);
    }
  }
  const qint64 materialIndexNs = timer.nsecsElapsed(); timer.restart();
  const auto compiled = CompiledShopCatalog::compile(goods, materialNames, &stats);
  const qint64 compileNs = timer.nsecsElapsed(); timer.restart();
  const AccountResourceView resources(input.resourceCounts, input.resourceCountsKnown, {}, input.conditionContext.shopSource, input.sourceInvalidated);
  const auto prepared = PreparedShopConditions::prepare(compiled, input.shopPacket, resources, input.conditionContext, &stats);
  const qint64 prepareNs = timer.nsecsElapsed(); timer.restart();
  QList<ShopPetDerived> derived; derived.reserve(input.overview.pets.size());
  qint64 checksum = 0;
  for (const auto& pet : input.overview.pets) {
    const auto record = AssetAnalyzer::derivePet(pet, input.metadata.stargods);
    checksum += record.currentPower;
    if (record.detailAvailable) derived.append(deriveShopPet(record.pet, true, input.metadata, &stats));
  }
  const qint64 deriveNs = timer.nsecsElapsed(); timer.restart();
  for (const auto& pet : derived) {
    const auto& primary = compiled.goodsForRace(pet.raceId);
    const auto& secondary = compiled.goodsForRace(pet.metadataRaceId);
    for (qsizetype index : primary) { ++stats.candidatePairsVisited; checksum += evaluateShopPetRule(compiled.goods()[index].petRule, pet, &stats).usefulCount; }
    for (qsizetype index : secondary) if (!primary.contains(index)) { ++stats.candidatePairsVisited; checksum += evaluateShopPetRule(compiled.goods()[index].petRule, pet, &stats).usefulCount; }
  }
  return {{QStringLiteral("mode"),QStringLiteral("independent-production-API-probes-do-not-sum-with-end-to-end")},
      {QStringLiteral("catalogCompileNs"),compileNs},{QStringLiteral("accountPrepareNs"),prepareNs},
      {QStringLiteral("materialIndexNs"),materialIndexNs},
      {QStringLiteral("petDeriveNs"),deriveNs},{QStringLiteral("candidateQualificationNs"),timer.nsecsElapsed()},
      {QStringLiteral("checksum"),checksum},{QStringLiteral("counters"),pipelineStatistics(stats)}};
}
