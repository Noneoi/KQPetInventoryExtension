#include "ui/pet/pet_filter_proxy_model.h"
#include "ui/pet/pet_table_model.h"
#include "ui/common/pet_image_cache.h"
#include "ui/pet/pet_window.h"
#include "application/views/inventory_projection.h"
#include "ui/shop/shop_window.h"
#include "ui/common/pet_facts_ui.h"
#include "domain/pet_analysis_facts.h"

#include <QApplication>
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QPersistentModelIndex>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QLineEdit>
#include <QCheckBox>
#include <QHeaderView>
#include <QFontMetrics>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTableView>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QJsonObject>
#include <QTimer>
#include <QThread>
#include "application/catalog/shop_exchange_catalog.h"
#include <QLabel>

#include <cstdio>
#include <limits>
#include <algorithm>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}
bool waitUntil(const std::function<bool()>& condition, int maximumMs = 5000) {
  QElapsedTimer deadline; deadline.start();
  while (!condition() && deadline.elapsed() < maximumMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return condition();
}

std::shared_ptr<PetDetailCatalogSnapshot> uiMetadata(quint64 revision) {
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>();
  metadata->revision = revision; metadata->contentDigest = QByteArray(32,char(revision)); metadata->loaded = true;
  metadata->root = {{QStringLiteral("stargods"),QJsonObject{
      {QStringLiteral("2001"),QJsonObject{{QStringLiteral("quality"),5},{QStringLiteral("changeable"),false},
          {QStringLiteral("type"),1},{QStringLiteral("limitJobs"),QJsonArray{}},
          {QStringLiteral("battlePower"),QJsonObject{{QStringLiteral("8"),100}}}}},
      {QStringLiteral("2002"),QJsonObject{{QStringLiteral("quality"),6},{QStringLiteral("changeable"),false},
          {QStringLiteral("type"),1},{QStringLiteral("limitJobs"),QJsonArray{}},
          {QStringLiteral("battlePower"),QJsonObject{{QStringLiteral("8"),500}}}}},
      {QStringLiteral("80"),QJsonObject{{QStringLiteral("quality"),6},{QStringLiteral("changeable"),false},
          {QStringLiteral("type"),2},{QStringLiteral("limitJobs"),QJsonArray{}},
          {QStringLiteral("battlePower"),QJsonObject{{QStringLiteral("8"),600}}}}}}}};
  metadata->root.insert(QStringLiteral("badges"),QJsonObject{{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}}});
  metadata->root.insert(QStringLiteral("sacredStarPlans"),QJsonObject{{QStringLiteral("2"),QJsonObject{{QStringLiteral("maxLevel"),10}}}});
  metadata->root.insert(QStringLiteral("sacredStagePlans"),QJsonObject{{QStringLiteral("6"),QJsonObject{{QStringLiteral("maxLevel"),7}}}});
  return metadata;
}
QJsonObject powerParts(const QJsonValue& base) {
  QJsonObject result;
  for (const auto* field : {"lv","iv","pl","sgv","ep","gsv","lav","lsv","bsv","asv","sjv"})
    result.insert(QString::fromLatin1(field),0);
  result.insert(QStringLiteral("lv"),base); return result;
}
QJsonObject withLocalPower(QJsonObject pet, const QJsonValue& current, const QJsonValue& highest) {
  // A complete local detail for numeric-row tests. Server totals stay separate;
  // invalid local components remain invalid instead of being normalized away.
  pet.insert(QStringLiteral("czdlv"),powerParts(current));
  pet.insert(QStringLiteral("mzdlv"),powerParts(highest));
  pet.insert(QStringLiteral("sgs"),QString()); pet.insert(QStringLiteral("sgsp"),QJsonArray{});
  pet.insert(QStringLiteral("astrolabe"),QString()); pet.insert(QStringLiteral("badge"),QString());
  pet.insert(QStringLiteral("shenjue"),QString());
  return pet;
}
QJsonObject uiRaw(qint64 id, int race, int base = 1000) {
  QJsonArray stars;
  for (int index = 0; index < 4000; ++index) stars.append(2002);
  auto current = powerParts(base), highest = powerParts(2000);
  current.insert(QStringLiteral("iv"),100); current.insert(QStringLiteral("bsv"),10); current.insert(QStringLiteral("sgv"),100);
  highest.insert(QStringLiteral("iv"),200); highest.insert(QStringLiteral("bsv"),100); highest.insert(QStringLiteral("sgv"),800);
  return {{QStringLiteral("id"),id},{QStringLiteral("r"),race},{QStringLiteral("n"),QStringLiteral("缓存精灵%1").arg(id)},
      {QStringLiteral("lv"),120},{QStringLiteral("_location"),QStringLiteral("warehouse")},
      {QStringLiteral("_warehouseGroup"),QStringLiteral("normal")},
      {QStringLiteral("zdl"),base+210},{QStringLiteral("xzdl"),3200},
      {QStringLiteral("czdlv"),current},{QStringLiteral("mzdlv"),highest},
      {QStringLiteral("sgs"),QStringLiteral("2001:8#0:8")},{QStringLiteral("sgsp"),stars},
      {QStringLiteral("astrolabe"),QString()},
      {QStringLiteral("stargodSlotMaxLevel"),8},{QStringLiteral("astrolabebr"),false},
      {QStringLiteral("badge"),QStringLiteral("101:5#201:0")},{QStringLiteral("shenjue"),QStringLiteral("1034#2#6|1:1")}};
}
PetDerivedFactsHandle attachUiFacts(InventoryViewSnapshot& snapshot, const QJsonObject& raw, quint64 revision) {
  const qint64 id = raw.value(QStringLiteral("id")).toInteger();
  PetRecordVersion version;
  version.key = {snapshot.account,snapshot.sessionEpoch,id,revision};
  version.sourceKnown = true; version.complete = true; version.persisted = true;
  version.brief = PetFactsUi::rowSummary(raw);
  snapshot.recordVersions.insert(id,version);
  PetAssetRecord seed; seed.instanceId = id; seed.raceId = raw.value(QStringLiteral("r")).toInt();
  seed.name = raw.value(QStringLiteral("n")).toString(); seed.location = QStringLiteral("普通仓库");
  seed.metadataSlotMaxLevel = 8; seed.detailAvailable = true; seed.observationVerified = true; seed.pet = raw;
  // Pure setup fixture only. Production UI never calls the factory.
  const PetMetadataView metadata(snapshot.metadata);
  const auto facts = derivePetAnalysisFacts(seed,{metadata.stargodDefinitions(),metadata.astrolabeDefinitions(),metadata.petDefinitions(),
      metadata.sacredStarPlans(),metadata.sacredStagePlans(),metadata.badgeDefinitions()});
  if (!facts) return {};
  auto handle = std::make_shared<PetDerivedFactsRecord>();
  handle->key = {version.key,snapshot.metadata->revision,snapshot.metadata->contentDigest,AssetAnalysisVersion::kCurrentAnalysis};
  handle->facts = *facts; snapshot.facts.insert(id,handle); return handle;
}

std::shared_ptr<PetDetailCatalogSnapshot> tableMetadata() {
  auto metadata = uiMetadata(1);
  metadata->root.insert(QStringLiteral("pets"),QJsonObject{
      {QStringLiteral("7516"),QJsonObject{{QStringLiteral("name"),QStringLiteral("五行御玄·乾坤")},
          {QStringLiteral("jobs"),QStringLiteral("24-26")},{QStringLiteral("attributes"),QStringLiteral("1")}}},
      {QStringLiteral("7135"),QJsonObject{{QStringLiteral("name"),QStringLiteral("逆时空·湮灭神女")},
          {QStringLiteral("jobs"),QStringLiteral("22")},{QStringLiteral("attributes"),QStringLiteral("2")}}}});
  metadata->root.insert(QStringLiteral("jobs"),QJsonObject{{QStringLiteral("24"),QStringLiteral("神肉盾")},
      {QStringLiteral("26"),QStringLiteral("神召唤师")},{QStringLiteral("22"),QStringLiteral("神灵")}});
  metadata->root.insert(QStringLiteral("attributes"),QJsonObject{{QStringLiteral("1"),QStringLiteral("火")},
      {QStringLiteral("2"),QStringLiteral("暗")}});
  return metadata;
}

std::shared_ptr<InventoryViewSnapshot> modelFixture(const QList<QJsonObject>& pets,
    std::shared_ptr<const PetDetailCatalogSnapshot> metadata, quint64 publication = 1) {
  auto frame = std::make_shared<InventoryViewSnapshot>();
  frame->publication = publication; frame->sessionEpoch = 1; frame->account = QStringLiteral("model-facts-fixture");
  frame->metadata = std::move(metadata); frame->membershipChanged = true;
  frame->sourceVerified = true; frame->sessionState = SessionConnectionState::Active;
  frame->warehouse = pets;
  for (const auto& pet : pets) if (pet.contains(QStringLiteral("zdl")) || pet.contains(QStringLiteral("xzdl")))
    attachUiFacts(*frame,pet,publication);
  return frame;
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  const QJsonObject qiankun = withLocalPower({{QStringLiteral("id"), 1001},
                            {QStringLiteral("r"), 7516},
                            {QStringLiteral("n"), QStringLiteral("[灵初]五行御玄·乾坤")},
                            {QStringLiteral("rt"), 26},
                            {QStringLiteral("lv"), 120},
                            {QStringLiteral("zdl"), 30000},
                            {QStringLiteral("xzdl"), 30000},
                            {QStringLiteral("_position"), 2},
                            {QStringLiteral("_warehouseGroup"), QStringLiteral("normal")}},30000,30000);
  const QJsonObject annihilation = withLocalPower({{QStringLiteral("id"), 1002},
                                 {QStringLiteral("r"), 7135},
                                 {QStringLiteral("n"), QStringLiteral("逆时空·湮灭神女")},
                                 {QStringLiteral("lv"), 100},
                                 {QStringLiteral("zdl"), 18000},
                                 {QStringLiteral("xzdl"), 24000},
                                 {QStringLiteral("_position"), 1},
                                 {QStringLiteral("_warehouseGroup"), QStringLiteral("elite")}},18000,24000);
  const QJsonObject unknown{{QStringLiteral("id"), 1003},
                            {QStringLiteral("r"), 990003},
                            {QStringLiteral("n"), QStringLiteral("未来测试精灵")},
                            {QStringLiteral("_position"), 3},
                            {QStringLiteral("_warehouseGroup"), QStringLiteral("normal")}};

  const auto explicitMetadata = tableMetadata();
  InventoryProjection modelView;
  auto initialFrame = modelFixture({qiankun,annihilation,unknown},explicitMetadata);
  modelView.publish(initialFrame); QCoreApplication::processEvents();
  PetTableModel model(PetTableModel::Location::Warehouse,&modelView);
  QAbstractItemModelTester sourceTester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
  QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
  QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
  QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
  model.setPets({qiankun, annihilation, unknown});
  bool ok = true;
  {
    PetTableModel detached(PetTableModel::Location::Warehouse);
    detached.setPets({qiankun});
    ok &= require(detached.index(0,PetTableModel::JobsColumn).data().toString() == QStringLiteral("—") &&
        detached.index(0,PetTableModel::BattlePowerColumn).data().toString() == QStringLiteral("—") &&
        !detached.cachedRow(0)->sortKeys[1].known && detached.cacheStats().battlePowerAnalyses == 0,
        "uninjected model silently read a global catalog or recalculated cultivation from raw fields");
    detached.setMetadataRevision(9);
    ok &= require(waitUntil([&] { return !detached.preparationRunning(); }) &&
        detached.cachedRow(0)->metadataRevision == 9 && detached.index(0,PetTableModel::JobsColumn).data().toString() == QStringLiteral("—"),
        "legacy revision invalidation substituted a global metadata snapshot");
  }
  ok &= require(model.rowCount() == 3 && model.columnCount() == 8 &&
                    model.headerData(PetTableModel::BattlePowerColumn, Qt::Horizontal).toString() ==
                        QStringLiteral("战斗力 /\n极限战斗力") &&
                    model.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() ==
                        1001 &&
                    model.index(1, PetTableModel::WarehousePositionColumn).data().toString() == QStringLiteral("精英") &&
                    model.index(0, PetTableModel::DisplayNameColumn).data().toString() == QStringLiteral("[灵初]五行御玄·乾坤") &&
                    model.index(0, PetTableModel::OriginalNameColumn).data().toString() == QStringLiteral("五行御玄·乾坤"),
                "table model did not expose the stable warehouse columns and roles");

  PetFilterProxyModel proxy;
  proxy.setSourceModel(&model);
  QAbstractItemModelTester proxyTester(&proxy, QAbstractItemModelTester::FailureReportingMode::Fatal);
  QSignalSpy proxyResets(&proxy, &QAbstractItemModel::modelReset);
  proxy.setQuery(QStringLiteral("乾坤"));
  ok &= require(proxy.rowCount() == 1 &&
                    proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1001,
                "proxy query did not retain the matching pet");
  proxy.setQuery({});
  proxy.setJobFilter(QStringLiteral("神召唤师"));
  ok &= require(proxy.rowCount() == 1 &&
                    proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1001,
                "proxy profession filter did not use resolved dual-category metadata");
  proxy.setJobFilter({});
  proxy.setSortMode(PetFilterProxyModel::SortMode::BattlePower, false);
  ok &= require(proxy.rowCount() == 3 &&
                    proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1001 &&
                    proxy.index(1, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1002 &&
                    proxy.index(2, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1003,
                "proxy descending sort did not keep missing values last");

  QJsonObject updated = annihilation;
  const auto beforeName = model.cacheStats();
  updated.insert(QStringLiteral("customName"), QStringLiteral("已更新昵称"));
  ok &= require(model.updatePet(updated) &&
                    model.index(model.rowForInstanceId(1002), 0).data().toString() ==
                        annihilation.value(QStringLiteral("n")).toString() &&
                    petQueryMatches(preparePetSearchQuery(QStringLiteral("已更新昵称")),
                        model.cachedRow(model.rowForInstanceId(1002))->search),
                "nickname replaced the server name or stopped matching search");
  ok &= require(model.cacheStats().searchIndexesBuilt == beforeName.searchIndexesBuilt + 1 &&
      model.cacheStats().battlePowerAnalyses == beforeName.battlePowerAnalyses &&
      model.cacheStats().sortKeysBuilt == beforeName.sortKeysBuilt,
      "nickname change rebuilt unrelated power or numeric sort caches");
  const auto beforePaint = model.cacheStats();
  for (int round = 0; round < 10; ++round) {
    proxy.setQuery(round % 2 ? QStringLiteral("已更新") : QStringLiteral("qk"));
    proxy.setSortMode(PetFilterProxyModel::SortMode::ObtainedAt, round % 2);
    for (int row = 0; row < model.rowCount(); ++row)
      for (int column = 0; column < model.columnCount(); ++column)
        model.index(row, column).data(Qt::DisplayRole);
  }
  const auto afterPaint = model.cacheStats();
  ok &= require(beforePaint.searchIndexesBuilt == afterPaint.searchIndexesBuilt &&
      beforePaint.sortKeysBuilt == afterPaint.sortKeysBuilt &&
      beforePaint.battlePowerAnalyses == afterPaint.battlePowerAnalyses &&
      resets.isEmpty() && proxyResets.isEmpty(),
      "search/sort/paint rebuilt source rows, details, pinyin or reset a model");

  proxy.setFilters({}, {}, {}, {});
  proxy.setSortMode(PetFilterProxyModel::SortMode::BattlePower, false);
  changed.clear();
  updated.insert(QStringLiteral("zdl"), 40000);
  updated = withLocalPower(updated,40000,24000);
  auto newPower = modelFixture({qiankun,updated,unknown},explicitMetadata,2);
  // Only the changed version is replaced. Name-only/UI changes reuse the
  // already prepared numeric fact; this fixture does no calculation in model.
  newPower->recordVersions[1001] = initialFrame->recordVersions.value(1001);
  newPower->facts[1001] = initialFrame->facts.value(1001);
  newPower->membershipChanged = false; newPower->changedDetails = {1002};
  modelView.publish(newPower); QCoreApplication::processEvents();
  model.updatePet(updated);
  bool powerOnlyDisplay = false;
  for (const auto& signal : changed) {
    const QModelIndex first = signal[0].value<QModelIndex>();
    const QModelIndex last = signal[1].value<QModelIndex>();
    const QList<int> roles = signal[2].value<QList<int>>();
    if (roles.contains(Qt::DisplayRole))
      powerOnlyDisplay |= first.column() == PetTableModel::BattlePowerColumn && last.column() == PetTableModel::BattlePowerColumn;
  }
  ok &= require(powerOnlyDisplay && proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1002,
      "power detail update did not emit its own column or dynamically resort the proxy");

  const auto beforeRaw = model.cacheStats();
  updated.insert(QStringLiteral("rawOnly"), QJsonObject{{QStringLiteral("fixture"), 1}});
  model.updatePet(updated);
  ok &= require(model.cacheStats().battlePowerAnalyses == beforeRaw.battlePowerAnalyses &&
      model.cacheStats().searchIndexesBuilt == beforeRaw.searchIndexesBuilt &&
      model.cacheStats().sortKeysBuilt == beforeRaw.sortKeysBuilt,
      "unrelated raw detail field invalidated derived row caches");

  QPersistentModelIndex kept(model.index(model.rowForInstanceId(1001), 0));
  QPersistentModelIndex deleted(model.index(model.rowForInstanceId(1002), 0));
  QJsonObject added = unknown;
  added.insert(QStringLiteral("id"), 1004);
  inserted.clear();
  removed.clear();
  model.setPets({unknown, added, qiankun});
  ok &= require(kept.isValid() && kept.row() == 2 &&
      kept.data(PetTableModel::InstanceIdRole).toLongLong() == 1001 && !deleted.isValid() &&
      inserted.size() == 1 && removed.size() == 1 && model.rowForInstanceId(1004) == 1 &&
      model.rowForInstanceId(1002) == -1 && resets.isEmpty(),
      "batch insert/remove/reorder lost persistent identity or reset the model");
  const auto stable = model.cacheStats();
  changed.clear();
  model.setPets({unknown, added, qiankun});
  ok &= require(changed.isEmpty() && model.cacheStats().rowRevisions == stable.rowRevisions,
                "identical snapshot rebuilt or republished unchanged rows");
  ok &= require(!model.applyPets({qiankun, qiankun}) && model.rowCount() == 3 && kept.isValid(),
                "duplicate IDs partially replaced a valid model");
  QJsonObject invalid = qiankun;
  invalid.insert(QStringLiteral("id"), 1.5);
  ok &= require(!model.applyPets({invalid}) && model.rowCount() == 3,
                "fractional identity was truncated and accepted into the ID map");
  {
    PetTableModel mutations(PetTableModel::Location::Warehouse);
    QAbstractItemModelTester mutationTester(&mutations, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QList<QJsonObject> original;
    for (int id = 1; id <= 8; ++id) {
      auto pet = unknown;
      pet.insert(QStringLiteral("id"), id);
      original.append(pet);
    }
    mutations.setPets(original);
    QPersistentModelIndex survivor(mutations.index(3, 0));
    QSignalSpy removalBatches(&mutations, &QAbstractItemModel::rowsRemoved);
    QSignalSpy insertionBatches(&mutations, &QAbstractItemModel::rowsInserted);
    auto newPet = unknown;
    newPet.insert(QStringLiteral("id"), 10);
    mutations.setPets({original[7], original[1], newPet, original[3]});
    ok &= require(removalBatches.size() == 1 && insertionBatches.size() == 1 &&
        survivor.isValid() && survivor.row() == 3 &&
        survivor.data(PetTableModel::InstanceIdRole).toLongLong() == 4,
        "disjoint removals were not compacted as a batch or broke a persistent index");
    auto upsert = newPet;
    upsert.insert(QStringLiteral("id"), 20);
    mutations.applyChanges({upsert}, {2, 4});
    for (int row = 0; row < mutations.rowCount(); ++row)
      ok &= require(mutations.rowForInstanceId(mutations.instanceIdAt(row)) == row,
                    "ID hash position became stale after combined structural delta");
    ok &= require(!survivor.isValid() && mutations.rowForInstanceId(2) == -1 &&
        mutations.rowForInstanceId(20) >= 0,
        "combined removal/upsert did not invalidate deleted identities");
  }

  InventoryProjection extremeView;
  extremeView.publish(modelFixture({},explicitMetadata)); QCoreApplication::processEvents();
  PetTableModel extremeModel(PetTableModel::Location::Warehouse,&extremeView);
  QAbstractItemModelTester extremeTester(&extremeModel, QAbstractItemModelTester::FailureReportingMode::Fatal);
  QJsonObject low = unknown, high = unknown;
  const qint64 maximumId = std::numeric_limits<qint64>::max();
  low.insert(QStringLiteral("id"), maximumId - 1);
  high.insert(QStringLiteral("id"), maximumId);
  low.remove(QStringLiteral("_position"));
  high.remove(QStringLiteral("_position"));
  extremeModel.setPets({high, low});
  PetFilterProxyModel extremeProxy;
  extremeProxy.setSourceModel(&extremeModel);
  ok &= require(extremeModel.rowForInstanceId(maximumId - 1) == 1 &&
      extremeModel.instanceIdAt(0) == maximumId &&
      extremeProxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == maximumId - 1,
      "extreme qint64 identity lost precision or sorted as text");
  extremeProxy.setQuery(QString::number(maximumId - 1));
  ok &= require(extremeProxy.rowCount() == 1, "exact extreme ID query used a rounded identifier");
  QJsonObject zero = unknown, fraction = unknown, integer = unknown;
  zero.insert(QStringLiteral("id"), 2001);
  zero.insert(QStringLiteral("zdl"), 0);
  fraction.insert(QStringLiteral("id"), 2002);
  fraction.insert(QStringLiteral("zdl"), 1.5);
  integer.insert(QStringLiteral("id"), 2003);
  integer.insert(QStringLiteral("zdl"), 100);
  zero = withLocalPower(zero,0,0);
  fraction = withLocalPower(fraction,1.5,100);
  integer = withLocalPower(integer,100,100);
  extremeView.publish(modelFixture({fraction,zero,integer},explicitMetadata,2));
  QCoreApplication::processEvents();
  extremeModel.setPets({fraction, zero, integer});
  extremeProxy.setQuery({});
  extremeProxy.setSortMode(PetFilterProxyModel::SortMode::BattlePower, false);
  ok &= require(extremeProxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 2003 &&
      extremeProxy.index(2, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 2002,
      "fractional power became a valid sort key");
  extremeProxy.setSortMode(PetFilterProxyModel::SortMode::BattlePower, true);
  ok &= require(extremeProxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 2001 &&
      extremeProxy.index(2, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 2002,
      "ascending sort conflated real zero with unknown power");

  QJsonObject skin = unknown;
  skin.insert(QStringLiteral("id"), 5001);
  skin.insert(QStringLiteral("n"), QStringLiteral("[灵初]不移之月·影月"));
  skin.insert(QStringLiteral("_metaOriginalName"), QStringLiteral("神运月影王"));
  extremeModel.setPets({skin});
  extremeProxy.setQuery(QStringLiteral("syy"));
  ok &= require(extremeProxy.rowCount() == 1 &&
      extremeModel.index(0, PetTableModel::DisplayNameColumn).data().toString() == QStringLiteral("[灵初]不移之月·影月") &&
      extremeModel.index(0, PetTableModel::OriginalNameColumn).data().toString() == QStringLiteral("神运月影王") &&
      extremeModel.index(0, PetTableModel::OriginalNameColumn).data(PetTableModel::NameSearchIndexRole).value<PetSearchText>().text ==
          QStringLiteral("神运月影王"), "skin display/original columns or original-name search did not stay distinct");
  extremeProxy.setQuery(QStringLiteral("BYZ"));
  const auto cachedText = extremeModel.index(0, 0).data(PetTableModel::NameSearchIndexRole).value<PetSearchText>();
  const auto highlight = petQueryHighlightRange(preparePetSearchQuery(QStringLiteral("BYZ")), cachedText);
  ok &= require(extremeProxy.rowCount() == 1 &&
      cachedText.text.mid(highlight.first, highlight.second) == QStringLiteral("不移之"),
      "cached initials did not retain the original highlight position mapping");

  QTemporaryDir imageRoot;
  PetImageCache imageCache(imageRoot.path());
  PetTableModel iconModel(PetTableModel::Location::Warehouse, nullptr, &imageCache);
  iconModel.setMetadataSnapshot(explicitMetadata);
  iconModel.setPets({qiankun, annihilation});
  for (int round = 0; round < 100; ++round)
    for (int row = 0; row < iconModel.rowCount(); ++row)
      iconModel.index(row, PetTableModel::AttributesColumn).data(Qt::DecorationRole);
  ok &= require(iconModel.cacheStats().attributeIconsBuilt <= 2 &&
      iconModel.cacheStats().attributeIconEntries <= 2,
      "attribute decoration cropped/scaled a pixmap for every paint");

  // Metadata-only updates must replace cached roles and protect newer detail.
  {
    auto first = std::make_shared<PetDetailCatalogSnapshot>(); first->revision = 50; first->loaded = true;
    first->root = {{QStringLiteral("pets"), QJsonObject{{QStringLiteral("990003"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("旧元数据名称")},
                    {QStringLiteral("attributes"), QStringLiteral("1")}, {QStringLiteral("jobs"), QStringLiteral("1")}}}}},
        {QStringLiteral("attributes"), QJsonObject{{QStringLiteral("1"), QStringLiteral("旧属性")}}},
        {QStringLiteral("jobs"), QJsonObject{{QStringLiteral("1"), QStringLiteral("旧职业")}}}};
    PetTableModel metadataModel(PetTableModel::Location::Warehouse);
    PetFilterProxyModel metadataProxy; metadataProxy.setSourceModel(&metadataModel);
    QAbstractItemModelTester metadataTester(&metadataModel, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QSignalSpy metadataResets(&metadataModel, &QAbstractItemModel::modelReset);
    metadataModel.setMetadataSnapshot(first); metadataModel.setPetsAsync({unknown});
    ok &= require(waitUntil([&] { return !metadataModel.preparationRunning(); }) && metadataModel.rowCount() == 1 &&
        metadataModel.index(0, PetTableModel::AttributesColumn).data().toString() == QStringLiteral("旧属性"), "asynchronous initial metadata was not applied");
    QPersistentModelIndex retained(metadataModel.index(0, 0));
    metadataProxy.setAttributeFilter(QStringLiteral("新属性"));
    ok &= require(metadataProxy.rowCount() == 0, "metadata filter fixture was not initially excluded");
    auto second = std::make_shared<PetDetailCatalogSnapshot>(*first); second->revision = 51;
    second->root.insert(QStringLiteral("attributes"), QJsonObject{{QStringLiteral("1"), QStringLiteral("新属性")}});
    second->root.insert(QStringLiteral("jobs"), QJsonObject{{QStringLiteral("1"), QStringLiteral("新职业")}});
    metadataModel.setMetadataSnapshot(second);
    QJsonObject newer = unknown; newer.insert(QStringLiteral("n"), QStringLiteral("派生期间的新详情"));
    metadataModel.updatePet(newer);
    ok &= require(waitUntil([&] { return !metadataModel.preparationRunning(); }) && retained.isValid() &&
        metadataProxy.rowCount() == 1 && metadataModel.index(0, PetTableModel::JobsColumn).data().toString() == QStringLiteral("新职业") &&
        metadataModel.index(0, 0).data().toString() == QStringLiteral("派生期间的新详情") && metadataResets.isEmpty(),
        "metadata refresh failed to refilter, retain selection or protect a newer detail");
    metadataModel.setPetsAsync({unknown, unknown});
    ok &= require(waitUntil([&] { return !metadataModel.preparationRunning(); }) && metadataModel.rowCount() == 1 && retained.isValid(),
        "invalid asynchronous snapshot partially replaced the valid model");
    QList<QJsonObject> abandoned;
    for (int index = 0; index < 10000; ++index) {
      auto value = unknown; value.insert(QStringLiteral("id"), 20000 + index); abandoned.append(value);
    }
    const auto builtBefore = metadataModel.cacheStats().preparationRows;
    bool replaced = false;
    QTimer replaceTimer; replaceTimer.setInterval(0);
    QObject::connect(&replaceTimer, &QTimer::timeout, &metadataModel, [&] {
      if (metadataModel.cacheStats().preparationRows <= builtBefore) return;
      replaced = true; replaceTimer.stop(); metadataModel.setPetsAsync({unknown});
    });
    metadataModel.setPetsAsync(abandoned); replaceTimer.start();
    ok &= require(waitUntil([&] { return replaced && !metadataModel.preparationRunning(); }) &&
        metadataModel.rowCount() == 1 && retained.isValid() && metadataModel.instanceIdAt(0) == 1003 &&
        metadataModel.cacheStats().supersededPreparations > 0,
        "a superseded partial preparation replaced newer membership or lost the retained identity");
  }

  // Account changes invalidate selection and personal details even if another
  // account has an identical instance ID. Public filter preferences survive.
  {
    QTemporaryDir root;
    InventoryProjection view(root.path(), nullptr);
    auto first = std::make_shared<InventoryViewSnapshot>();
    first->publication = 1;
    first->account = QStringLiteral("account-a");
    first->sessionEpoch = 1;
    first->dataRoot = root.path();
    first->authenticated = true;
    first->membershipChanged = true;
    QJsonObject personal = unknown;
    personal.insert(QStringLiteral("n"), QStringLiteral("账号A私密名称"));
    personal.insert(QStringLiteral("_location"), QStringLiteral("warehouse"));
    first->warehouse = {personal};
    first->details.insert(1003, personal);
    first->detailSavedTimes.insert(1003, QDateTime::currentDateTimeUtc());
    view.publish(first);
    QCoreApplication::processEvents();
    PetWindow window(&view);
    window.focusPet(1003);
    auto* detail = window.findChild<QTextBrowser*>();
    auto* search = window.findChild<QLineEdit*>();
    auto* raw = window.findChild<QTreeWidget*>();
    ok &= require(detail && search && raw &&
        detail->toPlainText().contains(QStringLiteral("账号A私密名称")),
        "account reset fixture did not expose the selected cached detail");
    search->setText(QStringLiteral("公开筛选偏好"));
    auto second = std::make_shared<InventoryViewSnapshot>(*first);
    second->publication = 2;
    second->account = QStringLiteral("account-b");
    second->sessionEpoch = 2;
    personal.insert(QStringLiteral("n"), QStringLiteral("账号B名称"));
    second->warehouse = {personal};
    second->details.insert(1003, personal);
    view.publish(second);
    QCoreApplication::processEvents();
    bool movesDisabled = true;
    int moveButtons = 0;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
      if (button->text() != QStringLiteral("进入背包") &&
          button->text() != QStringLiteral("放入仓库")) continue;
      ++moveButtons;
      movesDisabled &= !button->isEnabled();
    }
    ok &= require(search->text() == QStringLiteral("公开筛选偏好") &&
        !detail->toPlainText().contains(QStringLiteral("账号A私密名称")) &&
        detail->toPlainText().contains(QStringLiteral("请选择一只精灵")) &&
        raw->topLevelItemCount() == 1 && movesDisabled && moveButtons == 2,
        "account switch retained personal detail/selection or enabled an old move target");
  }

  // Era checkboxes select a frozen warehouse membership, independent of the
  // visible search. All headings stay available even in a compact workbench.
  {
    QTemporaryDir root;
    InventoryProjection view(root.path(), nullptr);
    auto frame = std::make_shared<InventoryViewSnapshot>();
    frame->publication = 1; frame->account = QStringLiteral("era-filter-fixture");
    frame->sessionEpoch = 1; frame->authenticated = true; frame->membershipChanged = true;
    frame->metadata = uiMetadata(40); frame->dataRoot = root.path();
    const QStringList eras{QStringLiteral("灵初"), QStringLiteral("神运"), QStringLiteral("星迹"),
                           QStringLiteral("启元"), QStringLiteral("神启"), QString()};
    for (int index = 0; index < eras.size(); ++index) {
      auto pet = unknown;
      pet.insert(QStringLiteral("id"), 6000 + index);
      pet.insert(QStringLiteral("n"), index == 0 ? QStringLiteral("[星迹]另一个皮肤名") : QStringLiteral("测试显示名称"));
      pet.insert(QStringLiteral("_metaEra"), eras[index]);
      pet.insert(QStringLiteral("_warehouseGroup"), index % 2 ? QStringLiteral("elite") : QStringLiteral("normal"));
      frame->warehouse.append(pet);
    }
    view.publish(frame); QCoreApplication::processEvents();
    PetWindow window(&view);
    QList<QList<qint64>> requests;
    QObject::connect(&window, &PetWindow::warehouseDetailRefreshRequested, &window,
        [&](const QList<qint64>& ids) { requests.append(ids); });
    auto* button = window.findChild<QPushButton*>(QStringLiteral("KQPetRefreshWarehouseDetails"));
    QList<QCheckBox*> checks;
    for (int index = 0; index < 5; ++index)
      checks.append(window.findChild<QCheckBox*>(QStringLiteral("KQPetDetailEra%1").arg(index)));
    ok &= require(button && std::all_of(checks.begin(), checks.end(), [](QCheckBox* check) {
      return check && check->isChecked();
    }), "warehouse era filters were missing or not selected by default");
    if (!button || checks.contains(nullptr)) return 1;
    button->click();
    ok &= require(requests.size() == 1 && requests.last() == QList<qint64>{6000,6001,6002,6003,6004,6005},
        "default era selection did not include both warehouses");
    for (auto* check : checks) check->setChecked(false);
    button->click();
    ok &= require(requests.size() == 1, "empty era selection expanded to all warehouse pets");
    checks[0]->setChecked(true); checks[2]->setChecked(true);
    window.findChild<QLineEdit*>()->setText(QStringLiteral("no-visible-match"));
    button->click();
    ok &= require(requests.size() == 2 && requests.last() == QList<qint64>{6000,6002},
        "selected eras used a skin prefix instead of frozen metadata or used visible search membership");
    checks[0]->setChecked(false); checks[2]->setChecked(false); checks[4]->setChecked(true);
    button->click();
    ok &= require(requests.size() == 3 && requests.last() == QList<qint64>{6004,6005},
        "other era did not include older and unknown eras exclusively");
    window.setDetailProgress(true, true, 1, 2, 1, 0, 0, 1);
    ok &= require(std::none_of(checks.begin(), checks.end(), [](QCheckBox* check) { return check->isEnabled(); }),
        "paused batch allowed its selected eras to change");
    window.setDetailProgress(false, false, 2, 2, 2, 0, 0, 0);
    ok &= require(std::all_of(checks.begin(), checks.end(), [](QCheckBox* check) { return check->isEnabled(); }),
        "completed batch did not release era checkboxes");
    window.setWorkbenchMode(true, true);
    for (const QString& name : {QStringLiteral("KQPetBackpackTable"), QStringLiteral("KQPetWarehouseTable"),
                                QStringLiteral("KQPetEliteWarehouseTable")}) {
      auto* table = window.findChild<QTableView*>(name);
      ok &= require(table && table->model()->columnCount() == (name.contains(QStringLiteral("Backpack")) ? 9 : 8),
          "inventory table omitted a requested name or location column");
      if (!table) continue;
      int totalWidth = 0;
      for (int column=0; column<table->model()->columnCount(); ++column) {
        totalWidth += table->columnWidth(column);
        ok &= require(!table->isColumnHidden(column) && table->columnWidth(column) > 0,
            "compact table hid one of the requested fields");
      }
      ok &= require(table->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
          totalWidth == table->viewport()->width() &&
          table->horizontalHeader()->sectionResizeMode(PetTableModel::DisplayNameColumn) == QHeaderView::Fixed,
          "inventory columns did not fit their viewport without horizontal scrolling");
    }
  }

  // The suffix compares the displayed local power with the known highest
  // target; official current/extreme values alone cannot grant it.
  for (const auto location : {PetTableModel::Location::Backpack,PetTableModel::Location::Warehouse}) {
    InventoryProjection view;
    auto frame = modelFixture({qiankun},explicitMetadata);
    if (location == PetTableModel::Location::Backpack) { frame->backpack = frame->warehouse; frame->warehouse.clear(); }
    auto facts = std::make_shared<PetDerivedFactsRecord>(*frame->facts.value(1001));
    auto& power = facts->facts.battlePower;
    power.hasCurrent = power.currentLocallyCalculated = power.hasExtreme = power.hasHighest = true;
    power.current = 30000; power.extreme = 28000; power.highest = 30000; power.serverCurrent = 99999;
    frame->facts.insert(1001,facts);
    view.publish(frame); QCoreApplication::processEvents();
    PetTableModel marked(location,&view); marked.setPets({qiankun});
    ok &= require(marked.index(0,PetTableModel::BattlePowerColumn).data().toString() == QStringLiteral("30000 / 28000（至高）"),
        "a known highest-power pet was not marked in the inventory column");
    const auto publishPower = [&](int current,bool highestKnown,bool locallyCalculated,const QString& expected) {
      auto next = std::make_shared<InventoryViewSnapshot>(*frame); ++next->publication; next->membershipChanged = false;
      auto updatedFacts = std::make_shared<PetDerivedFactsRecord>(*next->facts.value(1001));
      updatedFacts->facts.battlePower.current = current;
      updatedFacts->facts.battlePower.hasHighest = highestKnown;
      updatedFacts->facts.battlePower.currentLocallyCalculated = locallyCalculated;
      next->facts.insert(1001,updatedFacts); next->changedDetails = {1001};
      frame = next; view.publish(next);
      return waitUntil([&] { return marked.index(0,PetTableModel::BattlePowerColumn).data().toString() == expected; });
    };
    ok &= require(publishPower(30001,true,true,QStringLiteral("30001 / 28000（至高）")) &&
        publishPower(29999,true,true,QStringLiteral("29999 / 28000")) &&
        publishPower(30000,false,true,QStringLiteral("30000 / 28000")) &&
        publishPower(30000,true,false,QStringLiteral("— / 28000")),
        "highest suffix failed to update or used official/unknown power as a substitute");
  }

  // A real read projection must consume prepared facts, including late facts
  // for unchanged JSON, and must never retain/parse a raw cultivation tree.
  {
    QTemporaryDir root;
    InventoryProjection view(root.path(),nullptr);
    auto frame = std::make_shared<InventoryViewSnapshot>();
    frame->publication = 1; frame->account = QStringLiteral("facts-ui"); frame->sessionEpoch = 1;
    frame->metadata = uiMetadata(80); frame->membershipChanged = true;
    frame->sourceVerified = true; frame->sessionState = SessionConnectionState::Active;
    const auto firstRaw = uiRaw(501,7000);
    const auto secondRaw = uiRaw(502,7000,1500);
    frame->warehouse = {firstRaw,secondRaw};
    const auto firstFacts = attachUiFacts(*frame,firstRaw,1);
    const auto secondFacts = attachUiFacts(*frame,secondRaw,1);
    ok &= require(firstFacts && secondFacts,"UI prepared-fact fixture did not derive");
    if (!firstFacts || !secondFacts) return 1;
    view.publish(frame); QCoreApplication::processEvents();
    PetTableModel runtimeModel(PetTableModel::Location::Warehouse,&view);
    PetFilterProxyModel runtimeProxy; runtimeProxy.setSourceModel(&runtimeModel);
    QSignalSpy runtimeResets(&runtimeModel,&QAbstractItemModel::modelReset);
    QSignalSpy runtimeChanges(&runtimeModel,&QAbstractItemModel::dataChanged);
    runtimeModel.setPetsAsync(view.warehousePets());
    ok &= require(waitUntil([&] { return !runtimeModel.preparationRunning() && runtimeModel.rowCount() == 2; }),"facts-backed model did not initialize");
    runtimeProxy.setSortMode(PetFilterProxyModel::SortMode::BattlePower,false);
    const auto firstRow = runtimeModel.rowForInstanceId(501);
    ok &= require(runtimeModel.cacheStats().battlePowerAnalyses == 0 && runtimeModel.cacheStats().preparedFactsConsumed == 2 &&
        runtimeModel.cachedRow(firstRow)->sortKeys[1].value == firstFacts->facts.battlePower.current &&
        runtimeModel.cachedRow(firstRow)->sortKeys[1].value != firstRaw.value(QStringLiteral("zdl")).toInt() &&
        !runtimeModel.petAt(firstRow).contains(QStringLiteral("sgsp")) &&
        runtimeProxy.index(0,0).data(PetTableModel::InstanceIdRole).toLongLong() == 502,
        "production model parsed raw power, retained raw JSON, or sorted a different value than displayed");
    QPersistentModelIndex stable(runtimeModel.index(firstRow,0));
    auto newer = std::make_shared<InventoryViewSnapshot>(*frame); newer->publication = 2; newer->membershipChanged = false;
    const auto newestFacts = attachUiFacts(*newer,uiRaw(501,7000,2300),2);
    newer->changedDetails = {501};
    const auto parsedBefore = runtimeModel.cacheStats().battlePowerAnalyses;
    view.publish(newer);
    ok &= require(waitUntil([&] { return runtimeModel.cachedRow(runtimeModel.rowForInstanceId(501))->sortKeys[1].value == newestFacts->facts.battlePower.current; }) &&
        stable.isValid() && runtimeProxy.index(0,0).data(PetTableModel::InstanceIdRole).toLongLong() == 501 &&
        runtimeModel.cacheStats().battlePowerAnalyses == parsedBefore && runtimeResets.isEmpty() && !runtimeChanges.isEmpty(),
        "facts update with identical summary failed row refresh/resort or parsed the raw tree");
    auto partial = std::make_shared<InventoryViewSnapshot>(*newer); partial->publication = 3;
    partial->recordVersions[501].complete = false; partial->details.insert(501,firstRaw);
    view.publish(partial);
    ok &= require(waitUntil([&] { return !runtimeModel.cachedRow(runtimeModel.rowForInstanceId(501))->sortKeys[1].known; }) &&
        view.hasCachedDetail(501),"saved partial detail was treated as complete cultivation");

    auto metadataChanged = std::make_shared<InventoryViewSnapshot>(*newer); metadataChanged->publication = 4;
    metadataChanged->metadata = uiMetadata(81); metadataChanged->changedDetails = {501,502};
    view.publish(metadataChanged);
    ok &= require(waitUntil([&] { return !runtimeModel.preparationRunning() &&
        !runtimeModel.cachedRow(runtimeModel.rowForInstanceId(501))->sortKeys[1].known; }),"old metadata facts survived a metadata generation change");
    auto pending = std::make_shared<InventoryViewSnapshot>(*metadataChanged); pending->publication = 5; pending->membershipChanged = true;
    for (int index = 0; index < 512; ++index) {
      auto summary = PetFactsUi::rowSummary(firstRaw); summary.insert(QStringLiteral("id"),10000+index); pending->warehouse.append(summary);
    }
    auto late = std::make_shared<InventoryViewSnapshot>(*pending); late->publication = 6; late->membershipChanged = false;
    const auto lateFacts = attachUiFacts(*late,uiRaw(501,7000,2800),3); late->changedDetails = {501};
    view.publish(pending); QCoreApplication::processEvents();
    const auto preparedBefore = runtimeModel.cacheStats().preparationRows;
    bool delivered = false;
    QTimer delivery; delivery.setInterval(0);
    QObject::connect(&delivery,&QTimer::timeout,&runtimeModel,[&] {
      if (runtimeModel.cacheStats().preparationRows <= preparedBefore) return;
      delivered = true; delivery.stop(); view.publish(late);
    });
    runtimeModel.setPetsAsync(view.warehousePets()); delivery.start();
    ok &= require(waitUntil([&] { return delivered && !runtimeModel.preparationRunning() &&
        runtimeModel.cachedRow(runtimeModel.rowForInstanceId(501))->sortKeys[1].value == lateFacts->facts.battlePower.current; }) &&
        stable.isValid() && runtimeModel.cacheStats().battlePowerAnalyses == 0 && runtimeResets.isEmpty(),
        "an asynchronous cache commit overwrote newer facts or mixed metadata generations");
  }

  // Frozen catalog/date admission and strictly compiled resource presentation.
  // Malformed costs remain visible and cannot masquerade as a partial/free cost.
  {
    auto metadata = std::make_shared<PetDetailCatalogSnapshot>(*explicitMetadata);
    metadata->revision = 100; metadata->contentDigest = QByteArray(32,'m');
    metadata->root.insert(QStringLiteral("items"),QJsonObject{{QStringLiteral("100"),
        QJsonObject{{QStringLiteral("name"),QStringLiteral("冻结材料")}}}});
    metadata->root.insert(QStringLiteral("money"),QJsonObject{{QStringLiteral("1"),
        QJsonObject{{QStringLiteral("name"),QStringLiteral("测试币")}}}});
    InventoryProjection view;
    auto frame = modelFixture({},metadata); view.publish(frame); QCoreApplication::processEvents();
    auto catalog = std::make_shared<ShopCatalogSnapshot>(); catalog->revision = 1; catalog->loaded = true;
    ShopExchangeShop shop; shop.shopId = 1; shop.name = QStringLiteral("成本测试");
    const QStringList expressions{QStringLiteral("4:100:2#4:100:3"),QStringLiteral("4:100:2##8:1:3"),
        QStringLiteral("4:100:1.5"),QStringLiteral("4:100:9223372036854775807#4:100:1"),
        QString{},QString{},QStringLiteral("8:1:3000000000")};
    for (int index = 0; index < expressions.size(); ++index) {
      ShopExchangeGood good;
      good.shopId = 1; good.itemServerId = index+1; good.shopName = shop.name;
      good.description = QStringLiteral("测试项目%1").arg(index); good.cost = expressions.at(index);
      good.enhanceType = QStringLiteral("41"); good.raceIds = {7000}; good.shelfDate = QDate(2026,9,1);
      good.provenUnlimited = true; good.provenFree = index == 5;
      if (index == 0) { good.hasRemovalDate = true; good.removalDate = QDate(2026,9,9); }
      shop.goods.append(good);
    }
    auto future = shop.goods.first(); future.itemServerId = 20; future.shelfDate = QDate(2026,10,1);
    future.hasRemovalDate = false; shop.goods.append(future); catalog->allShops.append(shop);
    ShopWindow window(&view);
    window.setCatalogSnapshot(catalog,QDate(2026,9,9));
    window.setMaterialCounts({{QStringLiteral("4:100"),0},{QStringLiteral("8:1"),0}},true);
    auto table = [&] { return window.findChild<QTableWidget*>(QStringLiteral("KQShopGoodsTable-1")); };
    auto* currency = window.findChild<QLabel*>(QStringLiteral("KQShopCurrencySummary"));
    ok &= require(waitUntil([&] { return table() && table()->rowCount() == 7; }),
        "shop ignored the explicit business date or failed frozen catalog admission");
    if (table() && table()->rowCount() == 7) {
      ok &= require(table()->item(0,1)->text() == QStringLiteral("冻结材料 ×5") &&
          table()->item(6,1)->text() == QStringLiteral("测试币 ×3000000000"),
          "resource display failed duplicate merging, frozen metadata or qint64 quantity preservation");
      for (int row : {1,2,3}) ok &= require(table()->item(row,1)->text().startsWith(QStringLiteral("成本待确认")) &&
          table()->item(row,1)->text().contains(expressions.at(row)),
          "invalid/empty/fractional/overflow cost fragment was dropped or converted to zero");
      ok &= require(table()->item(4,1)->text().contains(QStringLiteral("待确认")) &&
          table()->item(5,1)->text().contains(QStringLiteral("无需资源")) && currency &&
          currency->text().contains(QStringLiteral("冻结材料 0")) &&
          currency->text().contains(QStringLiteral("部分项目成本待确认")),
          "unknown cost became free or a real zero balance became an unread value");
      const int builds = window.property("rebuildCount").toInt();
      window.setCatalogSnapshot(catalog,QDate(2026,9,9)); QCoreApplication::processEvents();
      ok &= require(window.property("rebuildCount").toInt() == builds,
          "repeated frozen catalog/date setter rebuilt the full shop UI");
      auto newMetadata = std::make_shared<PetDetailCatalogSnapshot>(*metadata);
      ++newMetadata->revision; newMetadata->contentDigest = QByteArray(32,'n');
      newMetadata->root.insert(QStringLiteral("items"),QJsonObject{{QStringLiteral("100"),
          QJsonObject{{QStringLiteral("name"),QStringLiteral("新冻结材料")}}}});
      auto newFrame = std::make_shared<InventoryViewSnapshot>(*frame); newFrame->publication = 2;
      newFrame->metadata = newMetadata; newFrame->membershipChanged = false; view.publish(newFrame);
      ok &= require(waitUntil([&] { return table() && table()->item(0,1)->text() == QStringLiteral("新冻结材料 ×5"); }),
          "metadata publication left cached shop material labels stale");
      window.focusGood(shop.goods.first().stableKey());
      window.setCatalogSnapshot(catalog,QDate(2026,9,10));
      ok &= require(waitUntil([&] { return table() && table()->rowCount() == 6; }),
          "changed business date reused an expired shop item");
      window.setCatalogSnapshot(catalog,{});
      ok &= require(waitUntil([&] { return !table() && currency->text().contains(QStringLiteral("业务日期尚未确认")); }),
          "missing business date implicitly used the local clock");
    }
  }

  // Cultivation applicability remains available from valid local facts after
  // source loss. This does not establish current material/remaining quotas.
  {
    const QDate businessDate(2026,9,9);
    const auto goods = ShopExchangeCatalog::instance().onlineGoods(businessDate);
    ok &= require(!goods.isEmpty(),"shop facts fixture lacks an embedded good");
    if (goods.isEmpty()) return 1;
    const auto good = goods.first();
    InventoryProjection view;
    auto frame = std::make_shared<InventoryViewSnapshot>();
    frame->publication = 1; frame->account = QStringLiteral("shop-facts"); frame->sessionEpoch = 1;
    frame->metadata = uiMetadata(90); frame->sourceVerified = true;
    frame->sessionState = SessionConnectionState::Active; frame->membershipChanged = true;
    const auto raw = uiRaw(601,good.raceIds.first()); frame->warehouse = {PetFactsUi::rowSummary(raw)};
    const auto facts = attachUiFacts(*frame,raw,1);
    ok &= require(bool(facts),"shop facts fixture did not derive");
    if (!facts) return 1;
    view.publish(frame); QCoreApplication::processEvents();
    ShopWindow shop(&view); shop.setCatalogSnapshot(ShopExchangeCatalog::instance().snapshot(),businessDate);
    QSignalSpy requests(&shop,&ShopWindow::detailRequested);
    shop.focusGood(good.stableKey());
    auto* table = shop.findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
    ok &= require(waitUntil([&] { return table && table->rowCount() == 1; }),"shop facts list did not appear");
    const auto expected = describeShopPetRule(compileShopPetRule(good.enhanceType),facts->facts.eligibility);
    ok &= require(expected.state == ShopPetEligibilityState::Usable && table->item(0,0)->font().bold() &&
        table->item(0,5)->text() == QString::number(facts->facts.battlePower.current),"shop did not consume cached eligibility/power");
    const auto compiled = shop.compiledEligibilityRules();
    auto uncertain = std::make_shared<InventoryViewSnapshot>(*frame); uncertain->publication = 2;
    uncertain->sourceVerified = false; uncertain->sessionState = SessionConnectionState::Uncertain;
    view.publish(uncertain);
    ok &= require(waitUntil([&] { return !view.currentSourceVerified(); }) && table->item(0,0)->font().bold() &&
        table->item(0,5)->text() == QString::number(facts->facts.battlePower.current) &&
        shop.compiledEligibilityRules() == compiled && requests.isEmpty(),
        "source loss discarded valid offline cultivation facts or caused new queries/recompilation");
    auto partial = std::make_shared<InventoryViewSnapshot>(*frame); partial->publication = 3;
    partial->recordVersions[601].complete = false; partial->details.insert(601,raw);
    view.publish(partial);
    ok &= require(waitUntil([&] { return table->item(0,5)->text() == QStringLiteral("—"); }) &&
        view.hasCachedDetail(601) && !table->item(0,0)->font().bold(),"shop promoted persisted partial detail to current Usable");
  }

  // Large fixtures measure cache work, without model-tester traversal overhead.
  for (int count : {2000, 10000}) {
    PetTableModel workload(PetTableModel::Location::Warehouse);
    PetFilterProxyModel filtered;
    filtered.setSourceModel(&workload);
    QList<QJsonObject> pets;
    pets.reserve(count);
    for (int index = 0; index < count; ++index) {
      QJsonObject pet = unknown;
      pet.insert(QStringLiteral("id"), 100000 + index);
      pet.insert(QStringLiteral("n"), QStringLiteral("[灵初]测试精灵%1").arg(index));
      pet.insert(QStringLiteral("_position"), index);
      pet.insert(QStringLiteral("zdl"), index % 1000);
      pet.insert(QStringLiteral("gd"), qint64(1700000000000LL + index));
      pets.append(pet);
    }
    QElapsedTimer timer;
    timer.start();
    workload.setMetadataSnapshot(explicitMetadata);
    int pulses = 0;
    QTimer heartbeat; heartbeat.setInterval(0);
    QObject::connect(&heartbeat, &QTimer::timeout, &workload, [&] { ++pulses; }); heartbeat.start();
    workload.setPetsAsync(pets);
    ok &= require(waitUntil([&] { return !workload.preparationRunning(); }), "large asynchronous model did not finish");
    heartbeat.stop();
    const qint64 buildMilliseconds = timer.elapsed();
    const auto warm = workload.cacheStats();
    timer.restart();
    for (int round = 0; round < 5; ++round) {
      filtered.setQuery(round % 2 ? QStringLiteral("nohit") : QStringLiteral("csjl"));
      filtered.setSortMode(static_cast<PetFilterProxyModel::SortMode>(round), round % 2);
      filtered.rowCount();
      for (int row = 0; row < workload.rowCount(); ++row) {
        workload.index(row, PetTableModel::BattlePowerColumn).data();
        workload.rowForInstanceId(100000 + row);
      }
    }
    const qint64 queryMilliseconds = timer.elapsed();
    const auto after = workload.cacheStats();
    ok &= require(pulses > 1 && after.preparationBatches > 1 && after.maximumBatchNanoseconds < 50000000,
        "model preparation failed to yield or blocked GUI for 50ms");
    ok &= require(warm.searchIndexesBuilt == quint64(count) && warm.sortKeysBuilt == quint64(count) &&
        warm.battlePowerAnalyses == 0 && warm.preparedFactsUnavailable == quint64(count) &&
        after.searchIndexesBuilt == warm.searchIndexesBuilt && after.sortKeysBuilt == warm.sortKeysBuilt &&
        after.battlePowerAnalyses == warm.battlePowerAnalyses,
        "2k/10k search/sort/display path reparsed details, numeric keys or pinyin");
    QList<QJsonObject> rawUpdates;
    for (int index = 0; index < 100; ++index) {
      auto pet = pets[index];
      pet.insert(QStringLiteral("rawOnly"), index);
      rawUpdates.append(pet);
    }
    QSignalSpy batches(&workload, &QAbstractItemModel::dataChanged);
    workload.applyChanges(rawUpdates);
    ok &= require(batches.size() == 1 &&
        workload.cacheStats().battlePowerAnalyses == warm.battlePowerAnalyses &&
        workload.cacheStats().searchIndexesBuilt == warm.searchIndexesBuilt,
        "batch raw updates emitted one notification per row or rebuilt unrelated caches");
    std::fprintf(stdout,
        "MODEL: rows=%d build_ms=%lld query_sort_paint_5x_ms=%lld search_builds=%llu power_parses=%llu max_batch_us=%lld pulses=%d\n",
        count, static_cast<long long>(buildMilliseconds), static_cast<long long>(queryMilliseconds),
        static_cast<unsigned long long>(after.searchIndexesBuilt),
        static_cast<unsigned long long>(after.battlePowerAnalyses), static_cast<long long>(after.maximumBatchNanoseconds / 1000), pulses);
    QPersistentModelIndex retained(workload.index(count / 2, 0));
    QSignalSpy bulkResets(&workload, &QAbstractItemModel::modelReset);
    auto changedMetadata = std::make_shared<PetDetailCatalogSnapshot>(*explicitMetadata);
    changedMetadata->revision += 10;
    pulses = 0; heartbeat.start(); timer.restart();
    workload.setMetadataSnapshot(changedMetadata);
    ok &= require(waitUntil([&] { return !workload.preparationRunning(); }), "bulk metadata refresh did not finish");
    heartbeat.stop();
    const auto refreshed = workload.cacheStats();
    ok &= require(refreshed.searchIndexesBuilt == after.searchIndexesBuilt + count &&
        refreshed.metadataCachesBuilt == after.metadataCachesBuilt + count && retained.isValid() &&
        retained.data(PetTableModel::InstanceIdRole).toLongLong() == 100000 + count / 2 && bulkResets.isEmpty() &&
        pulses > 1 && refreshed.maximumBatchNanoseconds < 50000000,
        "bulk metadata refresh was synchronous, lost identity or reset the model");
    std::fprintf(stdout, "METADATA: rows=%d complete_ms=%lld max_batch_us=%lld pulses=%d\n", count,
        static_cast<long long>(timer.elapsed()), static_cast<long long>(refreshed.maximumBatchNanoseconds / 1000), pulses);
  }

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: pet table model and filter proxy\n");
  return 0;
}
