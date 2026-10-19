#include "support/protocol_test_support.h"
#include "application/pet/pet_repository.h"
#include "ui/shop/shop_window.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "support/preview_inventory_support.h"
#include "application/views/inventory_projection.h"

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>

namespace {

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
}

bool verifyLargeProjectSwitches() {
  InventoryProjection inventory;
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>(); metadata->revision = 1; metadata->loaded = true;
  metadata->contentDigest = QByteArray(32,'a');
  metadata->root = {{QStringLiteral("pets"),QJsonObject{
      {QStringLiteral("990001"),QJsonObject{{QStringLiteral("name"),QStringLiteral("测试种族一")},{QStringLiteral("sign"),QString()}}},
      {QStringLiteral("990002"),QJsonObject{{QStringLiteral("name"),QStringLiteral("测试种族二")},{QStringLiteral("sign"),QString()}}}}}};
  metadata->root.insert(QStringLiteral("money"),QJsonObject{{QStringLiteral("1"),QJsonObject{{QStringLiteral("name"),QStringLiteral("测试币")}}}});
  auto frame = std::make_shared<InventoryViewSnapshot>(); frame->publication = 1; frame->account = QStringLiteral("switch-fixture");
  frame->sessionEpoch = 1; frame->metadata = metadata; frame->membershipChanged = true;
  for (int index = 0; index < 2000; ++index) frame->warehouse.append(QJsonObject{
      {QStringLiteral("id"),30000+index},{QStringLiteral("r"),index < 1600 ? 990001 : 990002},
      {QStringLiteral("n"),QStringLiteral("切换样本%1").arg(index,4,10,QLatin1Char('0'))},
      {QStringLiteral("lv"),index % 120 + 1},{QStringLiteral("_location"),QStringLiteral("warehouse")}});
  inventory.publish(frame); QCoreApplication::processEvents();
  auto catalog = std::make_shared<ShopCatalogSnapshot>(); catalog->revision = 1; catalog->loaded = true;
  for (int source = 0; source < 2; ++source) {
    ShopExchangeShop shop; shop.shopId = 1; shop.name = source ? QStringLiteral("活动性能样本") : QStringLiteral("常驻性能样本");
    if (source) {
      shop.sourceKey = QStringLiteral("fixture/activity-same-numeric-id");
      shop.navigationLink = QStringLiteral("btnNewAct_fixture_showMainPanel");
    }
    for (int index = 0; index < 12; ++index) {
      ShopExchangeGood good; good.shopId = shop.shopId; good.shopName = shop.name; good.sourceKey = shop.sourceKey;
      good.itemServerId = index + 1; good.description = shop.name + QString::number(index);
      good.enhanceType = index % 2 ? QStringLiteral("91") : QStringLiteral("11"); good.raceIds = {index % 2 ? 990002 : 990001};
      good.shelfDate = QDate(2026,9,1); good.provenUnlimited = true; good.cost = QStringLiteral("8:1:1");
      good.section = source ? ShopExchangeSection::ActivityShop : ShopExchangeSection::Permanent;
      if (source && index < 4) {
        const QJsonObject query{{QStringLiteral("key"),QStringLiteral("state")},{QStringLiteral("extension"),QStringLiteral("TimelinessActExtension")},
            {QStringLiteral("command"),QStringLiteral("1008_20260828_fixture_0")},{QStringLiteral("params"),QJsonValue(QJsonValue::Null)}};
        good.activityQueries = {{QStringLiteral("state"),query}};
        good.provenUnlimited = false; good.limitCount = index == 2 ? 1 : 3; good.limitLabel = QStringLiteral("总共");
        good.quotaObservation = {{QStringLiteral("requestKey"),QStringLiteral("state")},
            {QStringLiteral("path"),QJsonArray{QStringLiteral("counts"),QString::number(index)}},{QStringLiteral("valueKind"),QStringLiteral("used")}};
        if (index == 0) {
          good.cost.clear(); good.activityCosts = QJsonArray{QJsonObject{{QStringLiteral("name"),QStringLiteral("活动券")},
              {QStringLiteral("count"),30},{QStringLiteral("requestKey"),QStringLiteral("state")},{QStringLiteral("path"),QJsonArray{QStringLiteral("currency")}}}};
        } else if (index == 1) {
          good.cost.clear(); good.costDescription = QStringLiteral("12 / 8 测试币（随档位）");
          for (int tier = 0; tier < 2; ++tier) good.priceOptions.append(QJsonObject{
              {QStringLiteral("cost"),tier ? QStringLiteral("8:1:8") : QStringLiteral("8:1:12")},
              {QStringLiteral("when"),QJsonObject{{QStringLiteral("requestKey"),QStringLiteral("state")},
                  {QStringLiteral("path"),QJsonArray{QStringLiteral("tier")}},{QStringLiteral("op"),QStringLiteral("eq")},{QStringLiteral("value"),tier}}}});
        } else if (index == 2) good.quotaObservation.remove(QStringLiteral("requestKey"));
        else good.observationWhen = {{QStringLiteral("requestKey"),QStringLiteral("state")},
            {QStringLiteral("path"),QJsonArray{QStringLiteral("tier")}},{QStringLiteral("op"),QStringLiteral("eq")},{QStringLiteral("value"),9}};
      }
      shop.goods.append(good);
    }
    catalog->allShops.append(shop);
  }
  const auto activityState = [&](bool historical) {
    const auto& good = catalog->allShops[1].goods[1];
    const QJsonObject record{{QStringLiteral("request"),activityRequestSignature(good.activityQueries.value(QStringLiteral("state")).toObject())},
        {QStringLiteral("data"),QJsonObject{{QStringLiteral("tier"),0},{QStringLiteral("currency"),88},
            {QStringLiteral("counts"),QJsonObject{{QStringLiteral("0"),1},{QStringLiteral("1"),2}}}}},
        {QStringLiteral("observedAt"),QStringLiteral("2026-09-13T06:00:00.000Z")},
        {QStringLiteral("historical"),historical},{QStringLiteral("verified"),false}};
    return QJsonObject{{QStringLiteral("_activities"),QJsonObject{{good.sourceKey,QJsonObject{{QStringLiteral("state"),record}}}}}};
  };
  ShopWindow window(&inventory); window.setCatalogSnapshot(catalog,QDate(2026,9,9));
  const auto failedAt = [&window](int line) {
    auto* rows = window.findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("KQShopTabs"));
    int filled = 0;
    if (rows) for (int i = 0; i < rows->rowCount(); ++i) if (rows->item(i,0)) ++filled;
    std::fprintf(stderr,"FAIL: project-switch line=%d rows=%d filled=%d preparing=%d indexes=%llu rebuilds=%d tab=%d first=%s\n",
        line,rows ? rows->rowCount() : -1,filled,window.petRowsPreparing(),
        static_cast<unsigned long long>(window.petIndexBuilds()),window.property("rebuildCount").toInt(),
        tabs ? tabs->currentIndex() : -1,
        rows && rows->rowCount() && rows->item(0,2) ? rows->item(0,2)->text().toUtf8().constData() : "<empty>");
    return false;
  };
  window.setPacket(activityState(false),true); window.setMaterialCounts({{QStringLiteral("8:1"),99}},true); window.show();
  int requests = 0,pulses = 0,navigationRequests = 0;
  QString navigationLink;
  QObject::connect(&window,&ShopWindow::detailRequested,&window,[&](qint64) { ++requests; });
  QObject::connect(&window,&ShopWindow::usableDetailRequested,&window,[&](qint64,quint64) { ++requests; });
  QObject::connect(&window,&ShopWindow::refreshRequested,&window,[&] { ++requests; });
  QObject::connect(&window,&ShopWindow::openShopRequested,&window,[&](const QString& link) {
    ++navigationRequests;
    navigationLink = link;
  });
  auto* table = window.findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
  auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("KQShopTabs"));
  auto* openShop = window.findChild<QPushButton*>(QStringLiteral("KQOpenGameShop"));
  if (!table || !tabs || !openShop || !PreviewInventory::until([&] { return tabs->count() == 2; })) return failedAt(__LINE__);
  if (openShop->isEnabled()) return failedAt(__LINE__);
  QTimer heartbeat; heartbeat.setInterval(0);
  QObject::connect(&heartbeat,&QTimer::timeout,&window,[&] { ++pulses; }); heartbeat.start();
  const auto a = catalog->allShops[0].goods[0].stableKey(), b = catalog->allShops[0].goods[1].stableKey();
  const auto activity = catalog->allShops[1].goods[1].stableKey();
  const auto indexBuilds = window.petIndexBuilds();
  QElapsedTimer elapsed; elapsed.start();
  window.focusGood(a);
  const qint64 firstSwitchMs = elapsed.elapsed();
  if (!window.petRowsPreparing() || table->isEnabled() ||
      table->horizontalHeader()->sectionResizeMode(1) != QHeaderView::Fixed) return failedAt(__LINE__);
  window.focusGood(b); window.focusGood(activity);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 400; })) return failedAt(__LINE__);
  if (table->horizontalHeader()->sectionResizeMode(1) != QHeaderView::ResizeToContents ||
      table->horizontalHeader()->sectionResizeMode(0) != QHeaderView::Stretch) return failedAt(__LINE__);
  if (!openShop->isEnabled()) return failedAt(__LINE__);
  openShop->click();
  if (navigationRequests != 1 || navigationLink != QStringLiteral("btnNewAct_fixture_showMainPanel")) return failedAt(__LINE__);
  window.setNavigationRunning(true);
  tabs->setCurrentIndex(0); tabs->setCurrentIndex(1);
  openShop->click();
  if (openShop->isEnabled() || navigationRequests != 1) return failedAt(__LINE__);
  window.setNavigationRunning(false);
  if (!openShop->isEnabled()) return failedAt(__LINE__);
  window.focusGood(activity);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 400; })) return failedAt(__LINE__);
  auto* selectedGoods = qobject_cast<QTableWidget*>(tabs->currentWidget());
  if (!selectedGoods || !selectedGoods->item(1,0)->font().bold() ||
      selectedGoods->item(1,0)->text() != catalog->allShops[1].goods[1].description) return failedAt(__LINE__);
  const auto* currency = window.findChild<QLabel*>(QStringLiteral("KQShopCurrencySummary"));
  if (!selectedGoods->item(0,1)->text().contains(QStringLiteral("活动券 ×30")) ||
      !selectedGoods->item(1,1)->text().contains(QStringLiteral("测试币 ×12")) ||
      selectedGoods->item(1,3)->text() != QStringLiteral("已用 2｜剩 1") ||
      selectedGoods->item(1,0)->foreground().color() != QColor(QStringLiteral("#2563eb")) ||
      selectedGoods->item(1,3)->foreground().color() == QColor(QStringLiteral("#2563eb")) ||
      selectedGoods->item(1,0)->background().color() != QColor(QStringLiteral("#e8f2ff")) ||
      selectedGoods->item(1,5)->background().color() != QColor(QStringLiteral("#e8f2ff")) ||
      !selectedGoods->item(2,2)->text().contains(QStringLiteral("1")) ||
      selectedGoods->item(2,3)->text() != QStringLiteral("需在活动中查看") ||
      selectedGoods->item(3,3)->text() != QStringLiteral("不适用当前活动等级") || !currency ||
      !currency->text().contains(QStringLiteral("测试币 99")) || !currency->text().contains(QStringLiteral("活动券 88")) ||
      currency->text().contains(QStringLiteral("待确认"))) return failedAt(__LINE__);
  // Need / own / short: confirmed balances are green, unconfirmed ones say so.
  if (selectedGoods->columnCount() != 6 || !selectedGoods->item(1,5) ||
      selectedGoods->item(1,5)->text() != QStringLiteral("测试币 有 99；足够") ||
      !selectedGoods->item(1,5)->toolTip().contains(QStringLiteral("测试币：需要 12，拥有 99")) ||
      selectedGoods->item(0,5)->text() != QStringLiteral("活动券 有 88；足够") ||
      selectedGoods->item(0,5)->foreground().style() != Qt::NoBrush) return failedAt(__LINE__);
  // Searching hides other goods and jumps to the shop tab holding the match.
  auto* search = window.findChild<QLineEdit*>(QStringLiteral("KQShopGoodSearch"));
  if (!search) return failedAt(__LINE__);
  search->setText(QStringLiteral("常驻性能样本3"));
  if (!PreviewInventory::until([&] {
    auto* goods = qobject_cast<QTableWidget*>(tabs->widget(0));
    return tabs->currentIndex() == 0 && goods && !goods->isRowHidden(3) && goods->isRowHidden(2);
  })) return failedAt(__LINE__);
  search->clear();
  if (!PreviewInventory::until([&] {
    auto* goods = qobject_cast<QTableWidget*>(tabs->widget(0));
    return goods && !goods->isRowHidden(2);
  })) return failedAt(__LINE__);
  tabs->setCurrentIndex(1);
  const auto* previousGoods = selectedGoods;
  const auto* previousQuotaCell = selectedGoods->item(1,3);
  const auto* previousPetCell = table->item(0,0);
  const int structuresBefore = window.property("structureRebuildCount").toInt();
  const int priorRebuild = window.property("rebuildCount").toInt(); window.setPacket(activityState(true),true);
  if (!PreviewInventory::until([&] {
    selectedGoods = qobject_cast<QTableWidget*>(tabs->currentWidget());
    return window.property("rebuildCount").toInt() > priorRebuild && !window.petRowsPreparing() &&
        selectedGoods && selectedGoods->item(1,3)->text() == QStringLiteral("已用 2｜剩 1（上次）");
  })) return failedAt(__LINE__);
  if (selectedGoods != previousGoods || selectedGoods->item(1,3) != previousQuotaCell ||
      table->item(0,0) != previousPetCell || window.property("structureRebuildCount").toInt() != structuresBefore)
    return failedAt(__LINE__);
  for (int row = 0; row < table->rowCount(); ++row)
    if (!table->item(row,0) || table->item(row,0)->data(Qt::UserRole).toLongLong() < 31600) return failedAt(__LINE__);
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 1600; })) return failedAt(__LINE__);
  window.focusGood(b); window.focusGood(a);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 1600; }) ||
      window.petIndexBuilds() != indexBuilds || window.petMembershipCacheHits() < 2 || pulses < 2 || requests) return failedAt(__LINE__);
  QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Qt::DirectConnection,Q_ARG(int,1));
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing(); }) ||
      table->item(0,1)->text().toInt() > table->item(table->rowCount()-1,1)->text().toInt()) return failedAt(__LINE__);
  QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Qt::DirectConnection,Q_ARG(int,1));
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing(); }) ||
      table->item(0,1)->text().toInt() < table->item(table->rowCount()-1,1)->text().toInt()) return failedAt(__LINE__);
  auto newerMetadata = std::make_shared<PetDetailCatalogSnapshot>(*metadata); newerMetadata->revision = 2; newerMetadata->contentDigest = QByteArray(32,'b');
  auto species = newerMetadata->root.value(QStringLiteral("pets")).toObject();
  species.insert(QStringLiteral("990001"),QJsonObject{{QStringLiteral("sign"),QStringLiteral("神运")}});
  newerMetadata->root.insert(QStringLiteral("pets"),species);
  auto newer = std::make_shared<InventoryViewSnapshot>(*frame); newer->publication = 2; newer->membershipChanged = false; newer->metadata = newerMetadata;
  inventory.publish(newer);
  if (!PreviewInventory::until([&] { return window.petIndexBuilds() > indexBuilds && !window.petRowsPreparing() &&
      table->rowCount() == 1600 && table->item(0,2) && table->item(0,2)->text() == QStringLiteral("神运"); })) return failedAt(__LINE__);
  window.focusGood(b);
  auto other = std::make_shared<InventoryViewSnapshot>(*newer); other->publication = 3; other->account = QStringLiteral("other-account");
  other->sessionEpoch = 2; other->membershipChanged = true; other->warehouse = frame->warehouse.mid(0,3);
  for (auto& pet : other->warehouse) pet.insert(QStringLiteral("n"),QStringLiteral("新账号样本"));
  const int rebuilds = window.property("rebuildCount").toInt(); inventory.publish(other);
  if (!PreviewInventory::until([&] { return window.property("rebuildCount").toInt() > rebuilds; })) return failedAt(__LINE__);
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 3; })) return failedAt(__LINE__);
  for (int row = 0; row < table->rowCount(); ++row) if (table->item(row,0)->text() != QStringLiteral("新账号样本")) return failedAt(__LINE__);
  std::fprintf(stdout,"SWITCH: first_call_ms=%lld heartbeat_pulses=%d membership_cache_hits=%llu requests=%d\n",
      static_cast<long long>(firstSwitchMs),pulses,static_cast<unsigned long long>(window.petMembershipCacheHits()),requests);
  return requests == 0;
}

bool verifyUsableDetailVisits() {
  QTemporaryDir dataRoot;
  InventoryProjection inventory(dataRoot.path(), nullptr);
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>();
  metadata->revision = 1; metadata->loaded = true; metadata->contentDigest = QByteArray(32,'v');
  metadata->root = {{"pets",QJsonObject{{"990001",QJsonObject{{"name","visit-a"}}},
                                      {"990002",QJsonObject{{"name","visit-b"}}}}}};
  auto frame = std::make_shared<InventoryViewSnapshot>();
  frame->publication = 1; frame->sessionEpoch = 1; frame->account = QStringLiteral("visit-fixture");
  frame->metadata = metadata; frame->authenticated = true; frame->membershipChanged = true;
  for (qint64 id : {101,102,103,104,201}) {
    frame->warehouse.append(QJsonObject{{"id",id},{"r",id == 201 ? 990002 : 990001},
        {"n",QStringLiteral("visit-%1").arg(id)},{"lv",80},{"_location","warehouse"}});
  }
  const auto setFact = [&](qint64 id, ShopCultivationState state) {
    PetRecordVersion version;
    version.key = {frame->account,frame->sessionEpoch,id,frame->publication};
    version.complete = true;
    frame->recordVersions.insert(id,version);
    auto facts = std::make_shared<PetDerivedFactsRecord>();
    facts->key.record = version.key;
    facts->key.metadataRevision = metadata->revision; facts->key.metadataDigest = metadata->contentDigest;
    facts->facts.asset.instanceId = id; facts->facts.asset.detailAvailable = true;
    auto& eligibility = facts->facts.eligibility;
    eligibility.raceId = eligibility.metadataRaceId = id == 201 ? 990002 : 990001;
    eligibility.hasFullDetail = eligibility.hasData = true; eligibility.components[0] = state;
    frame->facts.insert(id,facts);
  };
  setFact(101,ShopCultivationState::Useful); setFact(102,ShopCultivationState::Full);
  setFact(104,ShopCultivationState::Useful); setFact(201,ShopCultivationState::Useful);
  inventory.publish(frame); QCoreApplication::processEvents();
  auto catalog = std::make_shared<ShopCatalogSnapshot>(); catalog->loaded = true; catalog->revision = 1;
  ShopExchangeShop shop; shop.shopId = 1; shop.name = QStringLiteral("visit-shop");
  for (int index = 0; index < 2; ++index) {
    ShopExchangeGood good; good.shopId = 1; good.shopName = shop.name; good.itemServerId = index+1;
    good.description = QStringLiteral("visit-good-%1").arg(index); good.enhanceType = QStringLiteral("11");
    good.raceIds = {990001+index}; good.provenUnlimited = true; good.shelfDate = QDate(2026,9,1);
    good.cost = QStringLiteral("8:1:1"); shop.goods.append(good);
  }
  catalog->allShops.append(shop);
  ShopWindow window(&inventory);
  QList<QPair<quint64,qint64>> requests;
  QList<quint64> cancelled;
  int legacyRequests = 0, writes = 0;
  QObject::connect(&window,&ShopWindow::usableDetailRequested,&window,
      [&](qint64 id,quint64 token) { requests.append(qMakePair(token,id)); });
  QObject::connect(&window,&ShopWindow::usableDetailRequestsCancelled,&window,
      [&](quint64 token) { cancelled.append(token); });
  QObject::connect(&window,&ShopWindow::detailRequested,&window,[&](qint64) { ++legacyRequests; });
  QObject::connect(&window,&ShopWindow::moveToBackpackRequested,&window,[&](qint64) { ++writes; });
  window.setCatalogSnapshot(catalog,QDate(2026,9,9)); window.show();
  auto* table = window.findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
  auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("KQShopTabs"));
  auto* progress = window.findChild<QLabel*>(QStringLiteral("KQShopEligibleDetailStatus"));
  const auto failure = [&](int line) {
    std::fprintf(stderr,"FAIL: usable visit line=%d requests=%d cancelled=%d progress=%s\n",
        line,int(requests.size()),int(cancelled.size()),progress ? progress->text().toUtf8().constData() : "<missing>");
    return false;
  };
  const auto pump = [] { PreviewInventory::until([] { return false; },30); };
  const auto publishFact = [&](qint64 id,ShopCultivationState state) {
    frame = std::make_shared<InventoryViewSnapshot>(*frame);
    ++frame->publication; frame->membershipChanged = false; frame->changedDetails = {id};
    setFact(id,state); inventory.publish(frame);
    PreviewInventory::until([&] { return inventory.snapshot()->publication == frame->publication; });
    pump();
  };
  if (!table || !tabs || !progress ||
      !PreviewInventory::until([&] { return tabs->count() == 1; })) return failure(__LINE__);
  const QString a = shop.goods[0].stableKey(), b = shop.goods[1].stableKey();
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return requests.size() == 1; }) ||
      requests[0].second != 101 || table->rowCount() != 4) return failure(__LINE__);
  const auto firstToken = requests[0].first;
  window.focusGood(a); // The same active visit is merged.
  QMetaObject::invokeMethod(table,"cellClicked",Qt::DirectConnection,Q_ARG(int,1),Q_ARG(int,0));
  window.setMaterialCounts({{QStringLiteral("8:1"),10}},true); // Background rebuild.
  QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Qt::DirectConnection,Q_ARG(int,1));
  frame = std::make_shared<InventoryViewSnapshot>(*frame);
  ++frame->publication; frame->membershipChanged = true;
  frame->warehouse[3].insert(QStringLiteral("n"),QStringLiteral("renamed-visit-104"));
  inventory.publish(frame); // An updated roster is still the same visit.
  pump();
  if (requests.size() != 1 || cancelled.contains(firstToken) || legacyRequests || writes) return failure(__LINE__);
  publishFact(101,ShopCultivationState::Full); // Fresh result must update this same list.
  if (!table->item(0,0) || table->item(0,0)->data(Qt::UserRole).toLongLong() != 101 ||
      table->item(0,0)->font().bold()) return failure(__LINE__);
  window.finishDetailRefresh(101,true,{});
  if (!PreviewInventory::until([&] { return requests.size() == 2; }) ||
      requests.last() != qMakePair(firstToken,qint64(104))) return failure(__LINE__);
  window.finishDetailRefresh(104,false,QStringLiteral("fixture-timeout")); pump();
  if (requests.size() != 2 || !progress->text().contains(QStringLiteral("失败 1"))) return failure(__LINE__);
  // Unknown is admitted only after usable local facts arrive. Originally full
  // pets remain excluded for the whole visit even if a later snapshot changes.
  publishFact(102,ShopCultivationState::Useful);
  publishFact(103,ShopCultivationState::Useful);
  if (!PreviewInventory::until([&] { return requests.size() == 3; }) ||
      requests.last() != qMakePair(firstToken,qint64(103))) return failure(__LINE__);
  window.finishDetailRefresh(103,true,{});
  publishFact(103,ShopCultivationState::Useful); pump();
  if (requests.size() != 3) return failure(__LINE__);
  int blueRows = 0;
  for (int row = 0; row < table->rowCount(); ++row)
    if (table->item(row,0) && table->item(row,0)->font().bold()) ++blueRows;
  if (blueRows != 3) return failure(__LINE__);
  window.focusGood(a); // Explicit completed re-entry is a fresh visit.
  if (!PreviewInventory::until([&] { return requests.size() == 4; }) ||
      requests.last().first == firstToken || requests.last().second == 101) return failure(__LINE__);
  const auto secondToken = requests.last().first;
  const auto oldInFlight = requests.last().second;
  window.focusGood(b);
  if (!PreviewInventory::until([&] { return requests.size() == 5; }) ||
      !cancelled.contains(secondToken) || requests.last().second != 201) return failure(__LINE__);
  const auto thirdToken = requests.last().first;
  window.finishDetailRefresh(oldInFlight,true,{}); pump();
  if (requests.size() != 5) return failure(__LINE__);
  window.hide(); pump();
  if (!cancelled.contains(thirdToken)) return failure(__LINE__);
  window.finishDetailRefresh(201,true,{}); window.show(); pump();
  if (requests.size() != 5) return failure(__LINE__);
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return requests.size() == 6; })) return failure(__LINE__);
  const auto sessionToken = requests.last().first;
  frame = std::make_shared<InventoryViewSnapshot>(*frame);
  ++frame->publication; ++frame->sessionEpoch; frame->account = QStringLiteral("next-visit-account");
  frame->membershipChanged = true; frame->changedDetails.clear(); frame->recordVersions.clear(); frame->facts.clear();
  setFact(101,ShopCultivationState::Useful); inventory.publish(frame); pump();
  if (!cancelled.contains(sessionToken) || requests.size() != 6) return failure(__LINE__);
  window.finishDetailRefresh(requests.last().second,true,{}); pump();
  if (requests.size() != 6) return failure(__LINE__);
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return requests.size() == 7; }) || requests.last().second != 101)
    return failure(__LINE__);
  frame = std::make_shared<InventoryViewSnapshot>(*frame); ++frame->publication;
  frame->authenticated = false; frame->membershipChanged = false; inventory.publish(frame); pump();
  const auto offlineToken = requests.last().first;
  window.focusGood(a); pump();
  if (!cancelled.contains(offlineToken) || requests.size() != 7 ||
      !progress->text().contains(QStringLiteral("仅展示本地数据")) || legacyRequests || writes) return failure(__LINE__);
  std::fprintf(stdout,"PASS: usable-only visit reads, late local facts, cancellation, cached rows and session isolation\n");
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQShopUiPreview"));
  if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--switch-only")
    return verifyLargeProjectSwitches() ? 0 : 13;
  if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--usable-refresh-only")
    return verifyUsableDetailVisits() ? 0 : 14;
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());

  PreviewInventory::Fixture fixture(dataRoot.path(),QStringLiteral("preview-account"));
  const auto failure = [&](int code,const char* stage) {
    std::fprintf(stderr,"FAIL: shop preview stage=%s code=%d %s\n",stage,code,fixture.diagnostics().toUtf8().constData());
    return code;
  };
  if (!fixture.initialized) return failure(1,"initialize");
  auto* repository = &fixture.repository;
  repository->beginListRefresh(1, repository->accountKey(),
                               repository->sessionGeneration());
  QJsonObject backpackPacket{
          {QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{PreviewInventory::pagedPet(10001,7115,QStringLiteral("预览精灵 A"),100,22000),
                       PreviewInventory::pagedPet(10003,7115,QStringLiteral("预览精灵 C"),80,18000)}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("10001#10003")}},
           {QStringLiteral("ppc"), 12}};
  QJsonArray pressureWarehouse;
  for (int index = 0; index < 1998; ++index)
    pressureWarehouse.append(QJsonObject{{QStringLiteral("id"), 20000 + index},
                                    {QStringLiteral("ri"), 999999},
                                    {QStringLiteral("n"), QStringLiteral("性能样本")},
                                    {QStringLiteral("lv"), 1}});
  deliver(repository, backpackPacket);
  repository->expectListPart(QStringLiteral("2_1_S"), 1,
                             repository->accountKey(), repository->sessionGeneration());
  deliver(repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
           {QStringLiteral("ns"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 10002},
                                   {QStringLiteral("ri"), 7185},
                                   {QStringLiteral("n"), QStringLiteral("预览精灵 B")},
                                   {QStringLiteral("lv"), 100}}}},
           {QStringLiteral("es"), pressureWarehouse},
           {QStringLiteral("rb"), QJsonArray{}}});
  repository->expectDetail(10002,1,repository->accountKey(),repository->sessionGeneration());
  deliver(repository,{{QStringLiteral("_cmd"),QStringLiteral("2_1_R")},
      {QStringLiteral("p"),PreviewInventory::pagedPet(10002,7185,QStringLiteral("预览精灵 B"),100,20000)}});
  if (!waitForRepositoryIdle(repository,30000)) return failure(10,"fixture repository IO");
  if (!fixture.factsReady()) return failure(10,"fixture projected facts");

  QElapsedTimer firstOpenTimer;
  firstOpenTimer.start();
  auto* window = new ShopWindow(&fixture.inventory);
  // UI behavior must not depend on the current release's live shelf dates.
  auto displayCatalog = std::make_shared<ShopCatalogSnapshot>();
  ShopExchangeShop displayShop; displayShop.shopId = 1; displayShop.name = QStringLiteral("预览商店");
  ShopExchangeGood displayGood; displayGood.shopId = 1; displayGood.itemServerId = 4;
  displayGood.shopName = displayShop.name; displayGood.description = QStringLiteral("预览养成商品");
  displayGood.enhanceType = QStringLiteral("91"); displayGood.raceIds = {7115,7185};
  displayGood.limitKey = QStringLiteral("pl"); displayGood.limitLabel = QStringLiteral("期"); displayGood.limitCount = 3;
  displayGood.cost = QStringLiteral("4:3237:1"); displayGood.shelfDate = QDate(2026,9,1);
  displayShop.goods.append(displayGood); displayCatalog->allShops.append(displayShop);
  window->setCatalogSnapshot(displayCatalog,QDate(2026,9,9));
  int gameRequests = 0;
  QObject::connect(window,&ShopWindow::detailRequested,window,[&](qint64) { ++gameRequests; });
  QObject::connect(window,&ShopWindow::usableDetailRequested,window,[&](qint64,quint64) { ++gameRequests; });
  QObject::connect(window,&ShopWindow::refreshRequested,window,[&] { ++gameRequests; });
  window->setPacket(
      {{QStringLiteral("si1"),
        QJsonObject{{QStringLiteral("b4"), QJsonObject{{QStringLiteral("pl"), 0}}},
                    {QStringLiteral("b6"), QJsonObject{{QStringLiteral("pl"), 1}}}}},
       {QStringLiteral("si4"),
        QJsonObject{{QStringLiteral("b4"), QJsonObject{{QStringLiteral("pl"), 0}}}}}},
      true);
  window->setStatus(QStringLiteral("界面预览：已载入模拟兑换次数"));
  window->setMaterialCounts({{QStringLiteral("4:3237"), 1888},
                             {QStringLiteral("4:3238"), 66},
                             {QStringLiteral("4:3241"), 777},
                             {QStringLiteral("4:3247"), 999},
                             {QStringLiteral("4:1344"), 1234},
                             {QStringLiteral("4:3189"), 3168},
                             {QStringLiteral("134:1"), 8800}}, true);
  window->show();

  if (qEnvironmentVariableIntValue("KQPET_PREVIEW_SELF_TEST") > 0) {
    QTimer::singleShot(0, &application, [&]() {
      auto* goods = window->findChild<QTableWidget*>(QStringLiteral("KQShopGoodsTable-1"));
      auto* pets = window->findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
      auto* detail = window->findChild<QTextBrowser*>(QStringLiteral("KQShopPreparedDetail"));
      auto* currency = window->findChild<QLabel*>(QStringLiteral("KQShopCurrencySummary"));
      auto* tabs = window->findChild<QTabWidget*>(QStringLiteral("KQShopTabs"));
      if (!goods || !pets || !detail || !currency || !tabs ||
          !PreviewInventory::until([&] { return goods->rowCount()>0; })) {
        application.exit(failure(2,"catalog widgets/rows ready"));
        return;
      }
      if (window->property("rebuildCount").toInt() != 1 ||
          firstOpenTimer.elapsed() > 3000 ||
          currency->text().contains(QStringLiteral("未查询")) ||
          !currency->text().contains(QStringLiteral("1888")) ||
          tabs->tabText(0).contains(QStringLiteral("（"))) {
        std::fprintf(stderr,"STATE: rebuilds=%d firstOpenMs=%lld currency=%s\n",window->property("rebuildCount").toInt(),firstOpenTimer.elapsed(),currency->text().toUtf8().constData());
        application.exit(failure(9,"initial rebuild/currency"));
        return;
      }
      if (!goods->item(0, 0) || !goods->item(0, 5) ||
          goods->item(0, 0)->foreground().color() != QColor(QStringLiteral("#2563eb")) ||
          goods->item(0, 0)->background().color() != QColor(QStringLiteral("#e8f2ff")) ||
          goods->item(0, 5)->background().color() != QColor(QStringLiteral("#e8f2ff"))) {
        application.exit(failure(11,"permanent remaining row whole-row highlight"));
        return;
      }
      const QPoint goodsTop = goods->mapToGlobal(QPoint(0, 0));
      const QPoint petsTop = pets->mapToGlobal(QPoint(0, 0));
      const QPoint detailTop = detail->mapToGlobal(QPoint(0, 0));
      if (petsTop.y() <= goodsTop.y() || detailTop.x() <= petsTop.x()) {
        application.exit(failure(7,"widget layout"));
        return;
      }
      QMetaObject::invokeMethod(goods, "cellClicked", Qt::DirectConnection,
                                Q_ARG(int, 0), Q_ARG(int, 0));
      if (!goods->item(0, 0) || !goods->item(0, 0)->font().bold()) {
        application.exit(failure(8,"good selection highlight"));
        return;
      }
      if (pets->rowCount() <= 0) {
        application.exit(failure(3,"eligible rows present"));
        return;
      }
      if (pets->rowCount() < 2) {
        application.exit(failure(5,"two comparable rows"));
        return;
      } else {
        const auto verifyTwoWaySort = [pets](int column) {
          QMetaObject::invokeMethod(pets->horizontalHeader(), "sectionClicked",
                                    Qt::DirectConnection, Q_ARG(int, column));
          const int firstAscending = pets->item(0, column)->text().toInt();
          const int lastAscending = pets->item(pets->rowCount() - 1, column)->text().toInt();
          QMetaObject::invokeMethod(pets->horizontalHeader(), "sectionClicked",
                                    Qt::DirectConnection, Q_ARG(int, column));
          const int firstDescending = pets->item(0, column)->text().toInt();
          const int lastDescending = pets->item(pets->rowCount() - 1, column)->text().toInt();
          return firstAscending <= lastAscending && firstDescending >= lastDescending;
        };
        if (!verifyTwoWaySort(1) || !verifyTwoWaySort(5) || !verifyTwoWaySort(6)) {
          application.exit(failure(6,"two-way numeric sorting"));
          return;
        }
      }
      QMetaObject::invokeMethod(pets, "cellClicked", Qt::DirectConnection,
                                Q_ARG(int, 0), Q_ARG(int, 0));
      const qint64 selectedId = pets->item(0,0)->data(Qt::UserRole).toLongLong();
      const auto computations = fixture.derivations.stats().computations;
      if (selectedId!=10001) { application.exit(failure(11,"sorted selection identity")); return; }
      if (!fixture.preparedVisible(detail,1,selectedId)) {
        std::fprintf(stderr,"DETAIL: %s\n",detail->toPlainText().left(400).toUtf8().constData());
        application.exit(failure(11,"prepared detail visible")); return;
      }
      const int requestsBeforePage = gameRequests;
      if (!fixture.followPage(detail,1,selectedId,DetailSection::StargodBackpack,1)) {
        application.exit(failure(11,"stargod page two")); return;
      }
      if (!fixture.followPage(detail,1,selectedId,DetailSection::Overview,0)) {
        application.exit(failure(11,"overview return")); return;
      }
      if (gameRequests!=requestsBeforePage || fixture.derivations.stats().computations!=computations) {
        application.exit(failure(11,"pagination requests/recomputation")); return;
      }

      repository->beginListRefresh(2, repository->accountKey(),
                                   repository->sessionGeneration());
      deliver(repository,
              {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
               {QStringLiteral("pl"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), 10001},
                                       {QStringLiteral("r"), 7115},
                                       {QStringLiteral("n"), QStringLiteral("预览精灵 A2")},
                                       {QStringLiteral("lv"), 100}}}},
               {QStringLiteral("pps"), QJsonArray{QStringLiteral("10001")}}});
      repository->expectListPart(QStringLiteral("2_1_S"), 2,
                                 repository->accountKey(),
                                 repository->sessionGeneration());
      deliver(repository,
              {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
               {QStringLiteral("ns"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), 10002},
                                       {QStringLiteral("ri"), 7185},
                                       {QStringLiteral("n"), QStringLiteral("预览精灵 B2")},
                                       {QStringLiteral("lv"), 100}}}},
                {QStringLiteral("es"), QJsonArray{}},
                {QStringLiteral("rb"), QJsonArray{}}});
      const bool preserved = PreviewInventory::until([&] {
        pets = window->findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
        const auto prepared = fixture.inventory.preparedDetail(1,10001);
        return pets && pets->rowCount()>0 && pets->currentRow()>=0 &&
            pets->item(pets->currentRow(),0)->data(Qt::UserRole).toLongLong()==10001 && prepared &&
            prepared->identity.name.contains(QStringLiteral("预览精灵 A2")) && detail &&
            detail->toPlainText().contains(QStringLiteral("预览精灵 A2"));
      });
      if (!preserved) std::fprintf(stderr,"DETAIL: %s\n",detail ? detail->toPlainText().left(400).toUtf8().constData() : "<missing browser>");
      if (!preserved) { application.exit(failure(4,"renamed selection/prepared update")); return; }
      application.exit(verifyLargeProjectSwitches() ? 0 : failure(13,"thousand-pet cancellable project switching"));
    });
  }

  bool validExitDelay = false;
  const int exitDelayMs =
      qEnvironmentVariableIntValue("KQPET_PREVIEW_EXIT_MS", &validExitDelay);
  if (validExitDelay && exitDelayMs > 0)
    QTimer::singleShot(exitDelayMs, &application, &QCoreApplication::quit);
  const int result = application.exec();
  delete window;
  return fixture.close() ? result : failure(12,"executor shutdown");
}
