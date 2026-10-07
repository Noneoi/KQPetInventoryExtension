#include "support/catalog_test_support.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "application/catalog/pet_detail_catalog.h"
#include "ui/shop/shop_selection_dialog.h"
#include "ui/workbench/pet_settings_dialog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QThread>
#include <QTreeWidget>
#include <cstdio>

namespace {
bool ok = true;
void require(bool value, const char* message) {
  if (!value) { ok = false; std::fprintf(stderr, "FAIL: %s\n", message); }
}
template<class F> bool waitFor(F predicate) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < 8000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1);
  }
  return predicate();
}
QJsonObject fixture() {
  auto root = ShopExchangeCatalog::instance().snapshot()->root;
  auto shop = root.value(QStringLiteral("shops")).toArray().first().toObject();
  shop.insert(QStringLiteral("name"), QStringLiteral("自动和手动商店"));
  auto good = shop.value(QStringLiteral("goods")).toArray().first().toObject();
  good.insert(QStringLiteral("shelfTime"), QStringLiteral("20200101"));
  good.insert(QStringLiteral("removalTime"), QStringLiteral("21000101"));
  auto manual = good;
  manual.insert(QStringLiteral("itemServerId"), 0);
  manual.insert(QStringLiteral("description"), QStringLiteral("指定精灵源兽神觉升1阶"));
  manual.insert(QStringLiteral("enhanceType"), QStringLiteral("92"));
  manual.insert(QStringLiteral("raceIds"), QJsonArray{7529});
  manual.insert(QStringLiteral("rewardRaw"), QStringLiteral("CommonEnhancePrize,-1,1,92,7529"));
  manual.insert(QStringLiteral("manualSelectionRequired"), true);
  shop.insert(QStringLiteral("goods"), QJsonArray{good, manual});
  auto independent = shop;
  independent.insert(QStringLiteral("sourceKey"), QStringLiteral("future/module#Config.EXCHANGES"));
  independent.insert(QStringLiteral("name"), QStringLiteral("额外商店"));
  auto signin = manual;
  signin.insert(QStringLiteral("acquisitionKind"),QStringLiteral("signin"));
  signin.insert(QStringLiteral("cost"),QString{});
  signin.insert(QStringLiteral("costKnown"),false);
  signin.insert(QStringLiteral("costDescription"),QStringLiteral("签到领取；请在活动内查看具体条件"));
  independent.insert(QStringLiteral("goods"), QJsonArray{signin});
  root.insert(QStringLiteral("shops"), QJsonArray{shop, independent});
  return root;
}
}

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  // Windows' offscreen platform does not enumerate installed fonts.
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  if (argc == 2) {
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "official replay catalog could not be opened");
    const auto replay = QJsonDocument::fromJson(file.readAll()).object();
    const auto catalog = ShopExchangeCatalog::prepare(replay, QStringLiteral("官方活动配置重放"), {});
    require(bool(catalog), "official replay catalog failed C++ validation");
    if (!catalog) return 1;
    int expected = 0, actual = 0;
    for (const auto& shop : replay.value(QStringLiteral("shops")).toArray())
      expected += shop.toObject().value(QStringLiteral("goods")).toArray().size();
    for (const auto& shop : catalog->discoveredShops) actual += shop.goods.size();
    require(expected > 0 && actual == expected, "recognized official rewards were discarded by the UI catalog");
    for (const auto& shop : catalog->allShops)
      for (const auto& good : shop.goods)
        require(!good.manualSelectionRequired, "unselected official reward appeared in mandatory automatic mode");
    ShopSelectionDialog preview(nullptr, PetDetailCatalog::instance().snapshot());
    preview.setCatalog(catalog, true, QStringLiteral("官方活动配置重放完成"));
    preview.findChild<QLineEdit*>(QStringLiteral("KQShopSelectionSearch"))->setText(QStringLiteral("通行证"));
    preview.findChild<QTreeWidget*>(QStringLiteral("KQShopSelectionTree"))->expandAll();
    preview.resize(1120, 700); preview.show(); application.processEvents();
    require(preview.grab().save(QStringLiteral("shop-activity-replay-preview.png")), "official preview could not be saved");
    std::printf("Official catalog: %d rewards validated.\n", actual);
    return ok ? 0 : 1;
  }
  const auto root = fixture();
  const auto base = ShopExchangeCatalog::prepare(root, QStringLiteral("fixture"), {});
  require(bool(base), "scanned general exchange did not parse");
  if (!base) return 1;
  auto excludedRoot = root;
  auto exclusionShops = root.value(QStringLiteral("shops")).toArray();
  QJsonArray excludedIssues;
  for (const auto* alias : {"godfantasynuoyachallenge", "eternalbattlefield", "shenyundaqiaoevo", "lingchushenandishitianchallenge"}) {
    const auto module = QStringLiteral("newactivityext/newact20260911/%1/%1").arg(QLatin1String(alias));
    auto excludedShop = exclusionShops.last().toObject();
    excludedShop.insert(QStringLiteral("sourceKey"), module + QStringLiteral("#Config.REWARDS"));
    exclusionShops.append(excludedShop);
    excludedIssues.append(QJsonObject{{QStringLiteral("module"), module}, {QStringLiteral("reason"), QStringLiteral("旧诊断")}});
  }
  excludedRoot.insert(QStringLiteral("shops"), exclusionShops);
  excludedRoot.insert(QStringLiteral("activityPending"), excludedIssues);
  const auto excluded = ShopExchangeCatalog::prepare(excludedRoot, QStringLiteral("old removed activities"), {});
  require(excluded && excluded->discoveredShops.size() == 2 && excluded->root.value(QStringLiteral("activityPending")).toArray().isEmpty(),
      "user-removed activities or their old diagnostics returned from cache");

  auto expiryRoot = root;
  auto expiryShops = root.value(QStringLiteral("shops")).toArray();
  auto expiryShop = expiryShops.first().toObject();
  auto expiryGoods = expiryShop.value(QStringLiteral("goods")).toArray();
  auto timedGood = expiryGoods.last().toObject();
  const auto now = QDateTime::currentDateTimeUtc().toOffsetFromUtc(8 * 3600);
  timedGood.insert(QStringLiteral("itemServerId"), 996);
  timedGood.insert(QStringLiteral("endsAt"), now.addSecs(-1).toString(Qt::ISODate));
  expiryGoods.append(timedGood);
  auto expiredOnly = expiryShops.last().toObject();
  expiredOnly.insert(QStringLiteral("goods"), QJsonArray{timedGood});
  expiryShops[1] = expiredOnly;
  timedGood.insert(QStringLiteral("itemServerId"), 997);
  timedGood.insert(QStringLiteral("endsAt"), QString{});
  timedGood.insert(QStringLiteral("startsAt"), now.addSecs(3600).toString(Qt::ISODate));
  expiryGoods.append(timedGood);
  expiryShop.insert(QStringLiteral("goods"), expiryGoods); expiryShops[0] = expiryShop;
  expiryRoot.insert(QStringLiteral("shops"), expiryShops);
  const auto expiry = ShopExchangeCatalog::prepare(expiryRoot, QStringLiteral("precise activity validity"), {});
  require(bool(expiry), "explicit activity timestamps did not parse");
  ShopSelectionDialog expiryDialog;
  expiryDialog.setCatalog(expiry, true, {});
  const auto expiryTree = expiryDialog.findChild<QTreeWidget*>(QStringLiteral("KQShopSelectionTree"));
  require(expiryTree->topLevelItemCount() == 1 && expiryTree->topLevelItem(0)->childCount() == 2,
      "expired or unopened reward/empty shop remained in the manual selection tree");
  expiryDialog.findChild<QLineEdit*>(QStringLiteral("KQShopSelectionSearch"))->setText(QStringLiteral("自动"));
  require(expiryTree->topLevelItemCount() == 1 && expiryTree->topLevelItem(0)->childCount() == 2,
      "search restored an out-of-period reward");
  auto legacyRoot = root;
  auto legacyShops = root.value(QStringLiteral("shops")).toArray();
  auto unrelated = legacyShops.last().toObject();
  auto material = unrelated.value(QStringLiteral("goods")).toArray().first().toObject();
  material.insert(QStringLiteral("enhanceType"), QString{});
  material.insert(QStringLiteral("raceIds"), QJsonArray{});
  material.insert(QStringLiteral("rewardRaw"), QStringLiteral("Material,4:55:10,1"));
  unrelated.insert(QStringLiteral("sourceKey"), QStringLiteral("old/module#materials"));
  unrelated.insert(QStringLiteral("goods"), QJsonArray{material});
  legacyShops.append(unrelated);
  material.insert(QStringLiteral("rewardRaw"), QStringLiteral("ActivityEntry,old/module"));
  unrelated.insert(QStringLiteral("sourceKey"), QStringLiteral("old/module#activity-entry"));
  unrelated.insert(QStringLiteral("goods"), QJsonArray{material});
  legacyShops.append(unrelated);
  legacyRoot.insert(QStringLiteral("shops"), legacyShops);
  const auto migrated = ShopExchangeCatalog::prepare(legacyRoot, QStringLiteral("legacy scan"), {});
  require(migrated && migrated->discoveredShops.size() == 2,
          "old material rewards and invented activity entries remained manually selectable");
  material.insert(QStringLiteral("rewardRaw"), QStringLiteral("Material,139:11:1#4:55:10,1"));
  material.insert(QStringLiteral("selectablePackageIds"), QJsonArray{11});
  unrelated.insert(QStringLiteral("sourceKey"), QStringLiteral("new/module#packages"));
  unrelated.insert(QStringLiteral("goods"), QJsonArray{material});
  legacyShops.append(unrelated); legacyRoot.insert(QStringLiteral("shops"), legacyShops);
  const auto packages = ShopExchangeCatalog::prepare(legacyRoot, QStringLiteral("verified choice packages"), {});
  require(packages && packages->discoveredShops.size() == 2 && packages->allShops.size() == 1,
          "selectable package remained in the manual list");
  auto typedRoot = root;
  auto typedShops = typedRoot.value(QStringLiteral("shops")).toArray();
  auto typedShop = typedShops.last().toObject();
  auto typedGood = typedShop.value(QStringLiteral("goods")).toArray().first().toObject();
  typedGood.insert(QStringLiteral("rewardRaw"), QStringLiteral("Strengthen,92,\"-8001\""));
  typedGood.insert(QStringLiteral("enhanceType"), QString{});
  typedGood.insert(QStringLiteral("raceIds"), QJsonArray{});
  typedGood.insert(QStringLiteral("acquisitionKind"), QStringLiteral("lottery"));
  typedShop.insert(QStringLiteral("goods"), QJsonArray{typedGood});
  typedShops[typedShops.size()-1] = typedShop;
  typedRoot.insert(QStringLiteral("shops"), typedShops);
  typedRoot.insert(QStringLiteral("activityPending"), QJsonArray{QJsonObject{
      {QStringLiteral("module"),QStringLiteral("future/activity")},
      {QStringLiteral("activityName"),QStringLiteral("未来活动")},
      {QStringLiteral("reason"),QStringLiteral("新的奖励结构待适配")}}});
  const auto typed = ShopExchangeCatalog::prepare(typedRoot, QStringLiteral("typed manual reward"), {});
  require(typed && typed->discoveredShops.size() == 2 && typed->allShops.size() == 1 &&
      typed->discoveredShops.last().goods.first().raceIds.isEmpty() &&
      typed->discoveredShops.last().goods.first().acquisitionLabel() == QStringLiteral("抽奖奖励"),
      "runtime-target reward became an automatic/all-pets rule or lost its lottery label");
  for (const auto& code : {QStringLiteral("62"), QStringLiteral("92")}) {
    auto passGood = typedGood;
    passGood.insert(QStringLiteral("enhanceType"), code);
    passGood.insert(QStringLiteral("raceIds"), QJsonArray{7529});
    passGood.insert(QStringLiteral("acquisitionKind"), QStringLiteral("progress"));
    passGood.insert(QStringLiteral("description"), QStringLiteral("指定精灵养成 · 第 30 项"));
    passGood.insert(QStringLiteral("rewardRaw"), QStringLiteral("CommonEnhancePrize,-1,1,%1,7529").arg(code));
    auto passShop = typedShop;
    passShop.insert(QStringLiteral("goods"), QJsonArray{passGood});
    auto passRoot = typedRoot;
    passRoot.insert(QStringLiteral("shops"), QJsonArray{passShop});
    const auto pass = ShopExchangeCatalog::prepare(passRoot, QStringLiteral("legacy pass"), {});
    require(bool(pass), "legacy pass reward could not be loaded");
    if (!pass) return 1;
    const auto good = pass->discoveredShops.first().goods.first();
    const auto expected = code == QStringLiteral("62") ? QStringLiteral("完美天赋") : QStringLiteral("源兽神觉提升1阶");
    require(good.description.contains(expected) &&
        good.description.endsWith(QStringLiteral("第 30 项")), "legacy pass hid its cultivation effect");
    auto renamed = good;
    renamed.description = QStringLiteral("指定精灵养成 · 第 30 项");
    require(renamed.stableKey() == good.stableKey(), "pass label repair discarded a manual selection");
    auto choice = passGood;
    choice.insert(QStringLiteral("itemServerId"), 998);
    choice.insert(QStringLiteral("manualSelectionRequired"), false);
    choice.insert(QStringLiteral("acquisitionKind"), QStringLiteral("exchange"));
    choice.insert(QStringLiteral("rewardRaw"), QStringLiteral("!Choice,|") + good.rewardRaw + QStringLiteral("|Material,4:1359:38,1"));
    auto namedChoice = passGood;
    namedChoice.insert(QStringLiteral("itemServerId"), 999);
    namedChoice.insert(QStringLiteral("description"), QStringLiteral("任选3颗红色星神"));
    passShop.insert(QStringLiteral("goods"), QJsonArray{passGood,choice,namedChoice});
    passRoot.insert(QStringLiteral("shops"), QJsonArray{passShop});
    auto selectedChoice = good;
    selectedChoice.itemServerId = 998;
    selectedChoice.rewardRaw = choice.value(QStringLiteral("rewardRaw")).toString();
    auto selectedNamedChoice = good;
    selectedNamedChoice.itemServerId = 999;
    const auto filtered = ShopExchangeCatalog::withManualSelection(
        ShopExchangeCatalog::prepare(passRoot,QStringLiteral("exclude legacy choices"),{}),
        {good.stableKey(), selectedChoice.stableKey(), selectedNamedChoice.stableKey()});
    require(filtered && filtered->discoveredShops.first().goods.size() == 2 && filtered->allShops.first().goods.size() == 2,
        "designated pass alternative was hidden or named pet/material choice leaked");
    require(filtered && filtered->discoveredShops.first().goods.last().manualSelectionRequired &&
        filtered->discoveredShops.first().goods.last().description.contains(expected) &&
        filtered->discoveredShops.first().goods.last().description.contains(QStringLiteral("与材料奖励二选一")),
        "cultivation alternative became compulsory or lost its concrete effect/condition");
    const auto unselected = ShopExchangeCatalog::withManualSelection(filtered, {good.stableKey()});
    require(unselected && unselected->allShops.first().goods.size() == 1,
        "unselected pass alternative leaked into default automatic mode");
  }
  for (const auto& title : {QStringLiteral("通用养成"), QStringLiteral("指定精灵完美极品")}) {
    auto bundleGood = typedGood;
    bundleGood.insert(QStringLiteral("description"), title + QStringLiteral(" · 第 5 项"));
    bundleGood.insert(QStringLiteral("rewardRaw"), QStringLiteral("Strengthen,11-62-86-89-92$3,\"-8001\""));
    auto bundleShop = typedShop;
    bundleShop.insert(QStringLiteral("goods"), QJsonArray{bundleGood});
    auto bundleRoot = typedRoot;
    bundleRoot.insert(QStringLiteral("shops"), QJsonArray{bundleShop});
    const auto bundle = ShopExchangeCatalog::prepare(bundleRoot, QStringLiteral("legacy generic cultivation"), {});
    require(bundle && !bundle->discoveredShops.isEmpty(), "generic cultivation cache failed to load");
    if (!bundle || bundle->discoveredShops.isEmpty()) return 1;
    const auto& restored = bundle->discoveredShops.first().goods.first();
    require(restored.description.contains(QStringLiteral("满级 / 完美天赋 / 满星迹 / 星迹突破 / 源兽神觉提升3阶")) &&
        restored.description.endsWith(QStringLiteral("第 5 项")) && restored.raceIds.isEmpty() && restored.manualSelectionRequired,
        "bundle cache hid concrete effects, changed quantity, or broadened applicability");
    bundleRoot.insert(QStringLiteral("rewardDescriptions"), QJsonObject{
        {QStringLiteral("descriptions"), QJsonObject{{QStringLiteral("92"), QStringLiteral("源兽神觉提升#level#层")}}},
        {QStringLiteral("descriptionParameters"), QJsonObject{{QStringLiteral("92"), QJsonObject{
            {QStringLiteral("index"), 0}, {QStringLiteral("default"), 1}}}}}});
    const auto fresh = ShopExchangeCatalog::prepare(bundleRoot, QStringLiteral("fresh official descriptions"), {});
    require(fresh && fresh->discoveredShops.first().goods.first().description.contains(QStringLiteral("源兽神觉提升3层")),
        "fresh official description was overridden by the legacy fallback");
  }
  const auto manual = base->discoveredShops.first().goods.last();
  const auto automatic = base->discoveredShops.first().goods.first();
  const auto extra = base->discoveredShops.last().goods.first();
  require(extra.acquisitionLabel() == QStringLiteral("签到领取") && extra.manualSelectionRequired && extra.cost.isEmpty() && !extra.provenFree,
          "sign-in reward was hidden or assigned an invented exchange price");
  require(manual.hasIdentity(), "official ordinary SEF item zero was rejected");
  require(base->discoveredShops.size() == 2 && base->allShops.size() == 1 && base->allShops.first().goods.size() == 1,
          "unselected manual shops/items leaked into the default shop view");
  require(manual.stableKey() != extra.stableKey(), "manual selection crossed independent activity namespaces");
  const auto selected = ShopExchangeCatalog::withManualSelection(base, {extra.stableKey()});
  require(selected->allShops.size() == 2 && selected->allShops.first().goods.size() == 1,
          "selecting one candidate affected unrelated items or automatic entries");
  const auto cleared = ShopExchangeCatalog::withManualSelection(selected, {});
  require(cleared->allShops.size() == 1 && cleared->allShops.first().goods.size() == 1,
          "clearing manual selection changed default automatic mode");
  const auto deselected = ShopExchangeCatalog::withManualSelection(selected, {extra.stableKey()}, {automatic.stableKey()});
  require(deselected->allShops.size() == 1 && deselected->allShops.first().goods.first().stableKey() == extra.stableKey(),
          "explicitly deselected automatic item remained in the active catalog");
  require(ShopExchangeCatalog::withManualSelection(base, {}, {automatic.stableKey()})->allShops.isEmpty(),
          "deselecting every item did not produce an empty active catalog");
  auto repriced = manual; repriced.cost = QStringLiteral("4:55:900"); repriced.description = QStringLiteral("新版名称");
  require(repriced.stableKey() == manual.stableKey(), "price/name updates discarded a selection");
  repriced.rewardRaw = QStringLiteral("Material,4:99:10,1");
  require(repriced.stableKey() != manual.stableKey(), "a replaced reward silently inherited selection");
  auto expandedRoot = root;
  auto expandedShops = root.value(QStringLiteral("shops")).toArray();
  auto expandedShop = expandedShops.last().toObject();
  auto expandedGoods = expandedShop.value(QStringLiteral("goods")).toArray();
  auto added = expandedGoods.first().toObject(); added.insert(QStringLiteral("itemServerId"), 999);
  expandedGoods.append(added); expandedShop.insert(QStringLiteral("goods"), expandedGoods);
  expandedShops[expandedShops.size() - 1] = expandedShop; expandedRoot.insert(QStringLiteral("shops"), expandedShops);
  const auto updated = ShopExchangeCatalog::withManualSelection(
      ShopExchangeCatalog::prepare(expandedRoot, QStringLiteral("new version"), {}), {extra.stableKey()});
  require(updated->allShops.last().goods.size() == 1 && updated->discoveredShops.last().goods.size() == 2,
          "a newly discovered ordinary reward inherited a previous whole-shop selection");

  ShopSelectionDialog dialog(nullptr, PetDetailCatalog::instance().snapshot());
  dialog.setCatalog(typed, true, QStringLiteral("部分奖励待解析"));
  require(!dialog.findChild<QPushButton*>(QStringLiteral("KQShopParseDiagnostics"))->isHidden(),
          "partial activity diagnostics were hidden from the selection dialog");
  dialog.setCatalog(base, true, QStringLiteral("测试"));
  require(dialog.findChild<QPushButton*>(QStringLiteral("KQShopParseDiagnostics"))->isHidden(),
          "resolved activity left stale pending diagnostics in the selection dialog");
  const auto tree = dialog.findChild<QTreeWidget*>(QStringLiteral("KQShopSelectionTree"));
  auto* parent = tree->topLevelItem(0);
  require((parent->child(0)->flags() & Qt::ItemIsUserCheckable) && parent->child(0)->checkState(0) == Qt::Checked,
          "automatic checkbox was not editable and initially selected");
  parent->child(0)->setCheckState(0, Qt::Unchecked);
  require(parent->child(0)->checkState(0) == Qt::Unchecked, "individual automatic item could not be unchecked");
  parent->setCheckState(0, Qt::Checked);
  require(parent->child(1)->checkState(0) == Qt::Checked, "whole-shop selection missed its manual child");
  parent->setCheckState(0, Qt::Unchecked);
  require(parent->child(0)->checkState(0) == Qt::Unchecked && parent->child(1)->checkState(0) == Qt::Unchecked,
          "whole-shop deselection left an automatic child checked");
  tree->topLevelItem(1)->setCheckState(0, Qt::Checked);
  QStringList submitted, submittedExcluded;
  QObject::connect(&dialog, &ShopSelectionDialog::selectionSaveRequested, &dialog, [&](const QStringList& values, const QStringList& excluded) { submitted = values; submittedExcluded = excluded; });
  dialog.findChild<QPushButton*>(QStringLiteral("KQSaveShopSelection"))->click();
  require(submitted == QStringList{extra.stableKey()}, "UI did not submit exactly the manually selected item");
  require(submittedExcluded == QStringList{automatic.stableKey()}, "UI did not submit the excluded automatic item");
  dialog.setCatalog(deselected, true, QStringLiteral("已保存"));
  require(!dialog.findChild<QPushButton*>(QStringLiteral("KQSaveShopSelection"))->isEnabled(), "saved selection stayed dirty");
  dialog.findChild<QLineEdit*>(QStringLiteral("KQShopSelectionSearch"))->setText(QStringLiteral("额外商店"));
  require(tree->topLevelItem(0)->isHidden() && !tree->topLevelItem(1)->isHidden(), "shop search did not filter candidates");
  dialog.findChild<QLineEdit*>(QStringLiteral("KQShopSelectionSearch"))->clear(); tree->expandAll();
  dialog.resize(1000, 620); dialog.show(); application.processEvents();
  dialog.grab().save(QStringLiteral("shop-selection-preview.png"));
  dialog.hide();
  PetSettingsDialog settings(RefreshTimings{});
  settings.findChild<QTabWidget*>(QStringLiteral("KQSettingsTabs"))->setCurrentIndex(1);
  settings.setShopSelection(base, true, QStringLiteral("测试"));
  settings.show(); application.processEvents();
  settings.grab().save(QStringLiteral("shop-settings-preview.png"));
  require(settings.findChild<QPushButton*>(QStringLiteral("KQShopSelection"))->isVisible(), "settings lost the manual-selection entry");
  settings.hide();

  QTemporaryDir directory;
  require(writeCatalogFixture(QDir(directory.path()).filePath(QStringLiteral("catalog/shop-exchange-data.json")),
      QJsonDocument(root).toJson()), "catalog fixture could not be saved");
  require(writeCatalogFixture(QDir(directory.path()).filePath(QStringLiteral("catalog/shop-selection.json")),
      QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("selected"), QJsonArray{extra.stableKey()}}}).toJson()),
      "legacy manual-selection fixture could not be saved");
  {
    StorageService storage(directory.path()); CatalogIoService service(&storage);
    require(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload), "catalog reload failed");
    service.loadShopSelection();
    require(waitFor([&] { return service.shopSelectionEditable(); }), "legacy selection file did not load");
    require(ShopExchangeCatalog::instance().snapshot()->manualSelection.contains(extra.stableKey()) &&
        ShopExchangeCatalog::instance().snapshot()->excludedAutomatic.isEmpty(), "legacy selections changed during migration");
    service.saveShopSelection({extra.stableKey()}, {automatic.stableKey()});
    require(waitFor([&] { return service.shopSelectionEditable(); }), "selection write did not finish");
    require(ShopExchangeCatalog::instance().snapshot()->manualSelection.contains(extra.stableKey()), "saved selection was not published");
    require(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload) &&
        ShopExchangeCatalog::instance().snapshot()->allShops.size() == 1 &&
        ShopExchangeCatalog::instance().snapshot()->excludedAutomatic.contains(automatic.stableKey()), "catalog update erased selection/exclusion");
    service.close(); require(storage.shutdown(), "first storage shutdown failed");
  }
  {
    StorageService storage(directory.path()); CatalogIoService service(&storage);
    service.loadShopSelection();
    require(waitFor([&] { return service.shopSelectionEditable(); }), "saved selection did not reload after restart");
    require(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload) &&
        ShopExchangeCatalog::instance().snapshot()->manualSelection == QSet<QString>{extra.stableKey()} &&
        ShopExchangeCatalog::instance().snapshot()->excludedAutomatic == QSet<QString>{automatic.stableKey()},
        "restart did not restore precisely the saved manual selection");
    service.close(); require(storage.shutdown(), "second storage shutdown failed");
  }
  {
    StorageService storage(directory.path(), {}, [](const QString&, const QByteArray&) {
      return StorageWriteAttempt{false, 0, QStringLiteral("fixture disk failure")};
    });
    CatalogIoService service(&storage); service.loadShopSelection();
    require(waitFor([&] { return service.shopSelectionEditable(); }), "fault test could not restore selection");
    service.saveShopSelection({manual.stableKey()});
    require(waitFor([&] { return service.shopSelectionEditable(); }), "failed write did not finish");
    require(ShopExchangeCatalog::instance().snapshot()->manualSelection == QSet<QString>{extra.stableKey()} &&
        ShopExchangeCatalog::instance().snapshot()->excludedAutomatic == QSet<QString>{automatic.stableKey()},
        "failed write changed the visible selection");
    service.close(); require(storage.shutdown(), "fault storage shutdown failed");
  }
  {
    const auto path = QDir(directory.path()).filePath(QStringLiteral("catalog/shop-selection.json"));
    require(writeCatalogFixture(path, QByteArray("broken-json")), "invalid selection fixture failed");
    StorageService storage(directory.path()); CatalogIoService service(&storage);
    service.loadShopSelection();
    require(waitFor([&] { return service.shopSelectionMessage().contains(QStringLiteral("失败")); }), "invalid selection was not reported");
    require(!service.shopSelectionEditable(), "invalid saved selection was silently treated as empty");
    service.saveShopSelection({});
    QFile file(path); require(file.open(QIODevice::ReadOnly) && file.readAll() == QByteArray("broken-json"),
        "unreadable selection was overwritten by an empty draft");
    service.close(); require(storage.shutdown(), "invalid-file storage shutdown failed");
  }
  if (argc > 1) {
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "supplied real catalog not readable");
    QString error;
    const auto live = ShopExchangeCatalog::prepare(QJsonDocument::fromJson(file.readAll()).object(), QStringLiteral("official validation"), {}, &error);
    require(bool(live), qPrintable(error));
    if (live) {
      std::printf("Official catalog validated: %lld scanned shops, %lld automatic shops\n",
          static_cast<long long>(live->discoveredShops.size()), static_cast<long long>(live->allShops.size()));
      dialog.setCatalog(live, true, QStringLiteral("官方扫描结果；默认项目可以取消，指定精灵提升项目可手动补充。"));
      dialog.findChild<QLineEdit*>(QStringLiteral("KQShopSelectionSearch"))->setText(QStringLiteral("特惠"));
      tree->expandAll(); dialog.show(); application.processEvents();
      dialog.grab().save(QStringLiteral("shop-selection-official.png")); dialog.hide();
    }
  }
  return ok ? 0 : 1;
}
