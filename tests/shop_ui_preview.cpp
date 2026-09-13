#include "protocol_test_support.h"
#include "pet_repository.h"
#include "shop_window.h"
#include "shop_exchange_catalog.h"
#include "preview_inventory_support.h"
#include "inventory_projection.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
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
    if (source) shop.sourceKey = QStringLiteral("fixture/activity-same-numeric-id");
    for (int index = 0; index < 12; ++index) {
      ShopExchangeGood good; good.shopId = shop.shopId; good.shopName = shop.name; good.sourceKey = shop.sourceKey;
      good.itemServerId = index + 1; good.description = shop.name + QString::number(index);
      good.enhanceType = index % 2 ? QStringLiteral("91") : QStringLiteral("11"); good.raceIds = {index % 2 ? 990002 : 990001};
      good.shelfDate = QDate(2026,9,1); good.provenUnlimited = true; good.cost = QStringLiteral("8:1:1");
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
  window.setPacket(activityState(false),true); window.setMaterialCounts({{QStringLiteral("8:1"),99}},true); window.show();
  int requests = 0,pulses = 0;
  QObject::connect(&window,&ShopWindow::detailRequested,&window,[&](qint64) { ++requests; });
  QObject::connect(&window,&ShopWindow::refreshRequested,&window,[&] { ++requests; });
  auto* table = window.findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
  auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("KQShopTabs"));
  if (!table || !tabs || !PreviewInventory::until([&] { return tabs->count() == 2; })) return false;
  QTimer heartbeat; heartbeat.setInterval(0);
  QObject::connect(&heartbeat,&QTimer::timeout,&window,[&] { ++pulses; }); heartbeat.start();
  const auto a = catalog->allShops[0].goods[0].stableKey(), b = catalog->allShops[0].goods[1].stableKey();
  const auto activity = catalog->allShops[1].goods[1].stableKey();
  const auto indexBuilds = window.petIndexBuilds();
  QElapsedTimer elapsed; elapsed.start();
  window.focusGood(a);
  const qint64 firstSwitchMs = elapsed.elapsed();
  if (!window.petRowsPreparing() || table->isEnabled()) return false;
  window.focusGood(b); window.focusGood(activity);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 400; })) return false;
  auto* selectedGoods = qobject_cast<QTableWidget*>(tabs->currentWidget());
  if (!selectedGoods || !selectedGoods->item(1,0)->font().bold() ||
      selectedGoods->item(1,0)->text() != catalog->allShops[1].goods[1].description) return false;
  const auto* currency = window.findChild<QLabel*>(QStringLiteral("KQShopCurrencySummary"));
  if (!selectedGoods->item(0,1)->text().contains(QStringLiteral("活动券 ×30")) ||
      !selectedGoods->item(1,1)->text().contains(QStringLiteral("测试币 ×12")) ||
      selectedGoods->item(1,3)->text() != QStringLiteral("观察 1 / 3") ||
      !selectedGoods->item(2,2)->text().contains(QStringLiteral("1")) ||
      selectedGoods->item(2,3)->text() != QStringLiteral("需在活动中查看") ||
      selectedGoods->item(3,3)->text() != QStringLiteral("不适用当前活动等级") || !currency ||
      !currency->text().contains(QStringLiteral("测试币 99")) || !currency->text().contains(QStringLiteral("活动券 观察 88"))) return false;
  const int priorRebuild = window.property("rebuildCount").toInt(); window.setPacket(activityState(true),true);
  if (!PreviewInventory::until([&] {
    selectedGoods = qobject_cast<QTableWidget*>(tabs->currentWidget());
    return window.property("rebuildCount").toInt() > priorRebuild && !window.petRowsPreparing() &&
        selectedGoods && selectedGoods->item(1,3)->text() == QStringLiteral("上次 1 / 3");
  })) return false;
  for (int row = 0; row < table->rowCount(); ++row)
    if (!table->item(row,0) || table->item(row,0)->data(Qt::UserRole).toLongLong() < 31600) return false;
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 1600; })) return false;
  window.focusGood(b); window.focusGood(a);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 1600; }) ||
      window.petIndexBuilds() != indexBuilds || window.petMembershipCacheHits() < 2 || pulses < 2 || requests) return false;
  QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Qt::DirectConnection,Q_ARG(int,1));
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing(); }) ||
      table->item(0,1)->text().toInt() > table->item(table->rowCount()-1,1)->text().toInt()) return false;
  QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Qt::DirectConnection,Q_ARG(int,1));
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing(); }) ||
      table->item(0,1)->text().toInt() < table->item(table->rowCount()-1,1)->text().toInt()) return false;
  auto newerMetadata = std::make_shared<PetDetailCatalogSnapshot>(*metadata); newerMetadata->revision = 2; newerMetadata->contentDigest = QByteArray(32,'b');
  auto species = newerMetadata->root.value(QStringLiteral("pets")).toObject();
  species.insert(QStringLiteral("990001"),QJsonObject{{QStringLiteral("sign"),QStringLiteral("神运")}});
  newerMetadata->root.insert(QStringLiteral("pets"),species);
  auto newer = std::make_shared<InventoryViewSnapshot>(*frame); newer->publication = 2; newer->membershipChanged = false; newer->metadata = newerMetadata;
  inventory.publish(newer);
  if (!PreviewInventory::until([&] { return window.petIndexBuilds() > indexBuilds && !window.petRowsPreparing() &&
      table->rowCount() == 1600 && table->item(0,2) && table->item(0,2)->text() == QStringLiteral("神运"); })) return false;
  window.focusGood(b);
  auto other = std::make_shared<InventoryViewSnapshot>(*newer); other->publication = 3; other->account = QStringLiteral("other-account");
  other->sessionEpoch = 2; other->membershipChanged = true; other->warehouse = frame->warehouse.mid(0,3);
  for (auto& pet : other->warehouse) pet.insert(QStringLiteral("n"),QStringLiteral("新账号样本"));
  const int rebuilds = window.property("rebuildCount").toInt(); inventory.publish(other);
  if (!PreviewInventory::until([&] { return window.property("rebuildCount").toInt() > rebuilds; })) return false;
  window.focusGood(a);
  if (!PreviewInventory::until([&] { return !window.petRowsPreparing() && table->rowCount() == 3; })) return false;
  for (int row = 0; row < table->rowCount(); ++row) if (table->item(row,0)->text() != QStringLiteral("新账号样本")) return false;
  std::fprintf(stdout,"SWITCH: first_call_ms=%lld heartbeat_pulses=%d membership_cache_hits=%llu requests=%d\n",
      static_cast<long long>(firstSwitchMs),pulses,static_cast<unsigned long long>(window.petMembershipCacheHits()),requests);
  return requests == 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQShopUiPreview"));
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
  window->setCatalogSnapshot(ShopExchangeCatalog::instance().snapshot(),QDate(2026,9,9));
  int gameRequests = 0;
  QObject::connect(window,&ShopWindow::detailRequested,window,[&](qint64) { ++gameRequests; });
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
      const int requestsBeforePage = gameRequests;
      const auto computations = fixture.derivations.stats().computations;
      if (selectedId!=10001) { application.exit(failure(11,"sorted selection identity")); return; }
      if (!fixture.preparedVisible(detail,1,selectedId)) {
        std::fprintf(stderr,"DETAIL: %s\n",detail->toPlainText().left(400).toUtf8().constData());
        application.exit(failure(11,"prepared detail visible")); return;
      }
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
