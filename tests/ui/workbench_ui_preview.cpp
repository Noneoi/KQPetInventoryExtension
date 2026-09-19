#include "ui/workbench/workbench_window.h"
#include "ui/pet/pet_window.h"
#include "ui/shop/shop_window.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "ui/routine/routine_overview_window.h"
#include "application/catalog/routine_overview_catalog.h"
#include "ui/analysis/asset_analysis_window.h"
#include "application/views/inventory_projection.h"
#include "application/views/analysis_projection.h"
#include "ui/pet/pet_filter_proxy_model.h"
#include "ui/pet/pet_table_model.h"
#include "domain/pet_identity.h"
#include "ui/common/pet_image_cache.h"
#include "storage/storage_service.h"
#include "application/analysis/analysis_worker.h"
#include "support/preview_inventory_support.h"
#include "domain/asset_derivation.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QLineEdit>
#include <QLabel>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPersistentModelIndex>
#include <QPainter>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>

#include <cstdio>

namespace {

bool require(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}

QJsonObject pet(int index, int race, bool backpack) {
  const qint64 id = 800001 + index;
  const QString name = index == 0
      ? QStringLiteral("[灵初]跨越星河的守望者·长名称与双职业布局样例")
      : QStringLiteral("[灵初]星河守望·样例精灵%1").arg(index + 1);
  QJsonArray stars;
  for (int slot=0; slot<130; ++slot) stars.append(slot%2 ? 66 : 67);
  return {{QStringLiteral("id"), id}, {QStringLiteral("r"), 990000 + index},
      {QStringLiteral("_metaRaceId"), race}, {QStringLiteral("n"), name},
      {QStringLiteral("_metaOriginalName"), QStringLiteral("星河守望者")},
      {QStringLiteral("_metaAttributes"), index % 2 ? QStringLiteral("3") : QStringLiteral("9")},
      {QStringLiteral("_metaJobs"), index % 3 ? QStringLiteral("1") : QStringLiteral("1,8")},
      {QStringLiteral("_metaEra"), QStringLiteral("灵初")}, {QStringLiteral("lv"), 120},
      {QStringLiteral("_location"), backpack ? QStringLiteral("backpack") : QStringLiteral("warehouse")},
      {QStringLiteral("_warehouseGroup"), index % 4 ? QStringLiteral("normal") : QStringLiteral("elite")},
      {QStringLiteral("_position"), index}, {QStringLiteral("_inFormation"), backpack && index < 6},
      {QStringLiteral("zdl"), 28000 + index * 180}, {QStringLiteral("xzdl"), 33000 + index * 180},
      {QStringLiteral("czdlv"), QJsonObject{{QStringLiteral("lv"), 8000}, {QStringLiteral("sgv"),0}, {QStringLiteral("bsv"), 2600},
                                            {QStringLiteral("iv"), 1800}, {QStringLiteral("asv"), 1000}}},
      {QStringLiteral("mzdlv"), QJsonObject{{QStringLiteral("lv"), 8500}, {QStringLiteral("sgv"),0}, {QStringLiteral("bsv"), 3000},
                                            {QStringLiteral("iv"), 2000}, {QStringLiteral("asv"), 1400}}},
      {QStringLiteral("sgs"), QStringLiteral("0:8#0:8#0:8")},
      {QStringLiteral("sgsp"), stars},{QStringLiteral("stargodSlotMaxLevel"),8},
      {QStringLiteral("badge"), QStringLiteral("101:5#201:0")},
      {QStringLiteral("shenjue"), QStringLiteral("1034#2#6|7:5")}};
}

QJsonObject daily() {
  return {{QStringLiteral("av"), 85}, {QStringLiteral("wav"), 574},
      {QStringLiteral("bi"), QJsonArray{true, true, false, false, false}},
      {QStringLiteral("wbi"), QJsonArray{true, false, false, false, false}},
      {QStringLiteral("ti"), QJsonArray{1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1}},
      {QStringLiteral("wti"), QJsonArray{7, 6, 12, 25, 12, 5, 90, 6, 13, 7, 9, 1, 0}}};
}

bool verifyHostWindowLifecycle(QApplication& application, WorkbenchWindow& workbench) {
  // Exercise Qt's real outer event loop: lastWindowClosed is a window-close
  // event, not an assertion about a widget attribute or a manual signal emit.
  // The host remains responsible for whether that signal quits or enters tray.
  const bool originalQuitPolicy = application.quitOnLastWindowClosed();
  application.setQuitOnLastWindowClosed(false);
  QWidget host;
  host.setObjectName(QStringLiteral("SyntheticHostMainWindow"));
  host.setWindowTitle(QStringLiteral("Synthetic host lifecycle fixture"));
  host.resize(320,200);
  QSignalSpy lastClosed(&application,&QGuiApplication::lastWindowClosed);
  bool finished = false;
  bool valid = lastClosed.isValid();
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline,&QTimer::timeout,&application,&QCoreApplication::quit);
  host.show();
  workbench.show();
  QTimer::singleShot(0,&application,[&] {
    valid &= require(host.isVisible() && workbench.isVisible(),"host/workbench lifecycle fixture did not show both windows");
    workbench.close();
    valid &= require(host.isVisible() && !workbench.isVisible() && lastClosed.isEmpty(),
        "hiding the workbench closed the host or emitted lastWindowClosed");
    workbench.show();
    QTimer::singleShot(0,&application,[&] {
      const bool accepted = host.close();
      QTimer::singleShot(0,&application,[&,accepted] {
        valid &= require(accepted && !host.isVisible() && workbench.isVisible() && lastClosed.size()==1,
            "visible workbench prevented the host's lastWindowClosed notification");
        workbench.close();
        valid &= require(!workbench.isVisible() && lastClosed.size()==1,
            "workbench hide changed the host's last-window lifecycle");
        finished = true;
        application.quit();
      });
    });
  });
  deadline.start(3000);
  const int exitCode = application.exec();
  deadline.stop();
  host.hide();
  workbench.hide();
  application.setQuitOnLastWindowClosed(originalQuitPolicy);
  return require(finished && exitCode==0,"host lifecycle event-loop check did not finish") && valid;
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const int requestedDpi = qEnvironmentVariableIntValue("KQPET_PREVIEW_DPI");
  const int dpi = requestedDpi > 0 ? requestedDpi : 100;
  qputenv("QT_SCALE_FACTOR", QByteArray::number(dpi / 100.0));
  QApplication application(argc, argv);
  // Windows' offscreen QPA does not enumerate host fonts. Load them only in
  // this isolated preview process; production Workbench never changes qApp.
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  application.setApplicationName(QStringLiteral("KQWorkbenchUiPreview"));
  const QFont originalFont = application.font();
  const QString originalStyle = application.styleSheet();
  QTemporaryDir dataRoot;
  const QString account = QStringLiteral("离线样例账号 · 仅合成数据");
  PreviewInventory::Fixture fixture(dataRoot.path(),account);
  auto& inventory = fixture.inventory;
  ImageServiceOptions imageOptions;
  imageOptions.dataRoot = dataRoot.path();
  imageOptions.verifiedUrls.insert(QStringLiteral("星河守望者"), QStringLiteral("http://127.0.0.1:1/isolated-preview.png"));
  // Preseed only this disposable fixture. Production image writes belong to I/O.
  QImage portrait(240, 240, QImage::Format_ARGB32_Premultiplied);
  portrait.fill(QColor(QStringLiteral("#edf5ff")));
  {
    QPainter painter(&portrait);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#70a8db")));
    painter.drawEllipse(QRectF(46, 58, 148, 140));
    painter.drawEllipse(QRectF(40, 30, 48, 88));
    painter.drawEllipse(QRectF(152, 30, 48, 88));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(82, 104, 20, 30));
    painter.drawEllipse(QRectF(138, 104, 20, 30));
    painter.setPen(QColor(QStringLiteral("#35638d")));
    painter.drawText(QRect(0, 203, 240, 30), Qt::AlignCenter, QStringLiteral("合成图片 · 离线预览"));
  }
  QDir().mkpath(QDir(dataRoot.path()).filePath(QStringLiteral("images/pets")));
  for (int index = 0; index < 42; ++index) {
    const auto sample = pet(index,1,index<12);
    imageOptions.verifiedUrls.insert(sample.value(QStringLiteral("n")).toString(),QStringLiteral("http://127.0.0.1:1/isolated-preview.png"));
    portrait.save(QDir(dataRoot.path()).filePath(QStringLiteral("images/pets/%1.png").arg(petVisualKey(sample))));
  }
  ImageService images(imageOptions, {
      [&](auto job) { return fixture.storage.postAuxiliary(std::move(job)); },
      [&](auto job) { return fixture.controller.postPriorityCompute(std::move(job)); }});
  PetImageCache sharedImages(dataRoot.path());
  sharedImages.setService(&images);
  bool ok = require(fixture.initialized,"production preview inventory did not initialize");
  AnalysisProjection analysis;
  const QList<ShopExchangeGood> goods = ShopExchangeCatalog::instance().onlineGoods(QDate(2026, 9, 9));
  ok &= require(!goods.isEmpty(), "embedded catalog unavailable in preview");
  if (goods.isEmpty()) return 1;

  const QDateTime observedAt = QDateTime::fromString(QStringLiteral("2026-09-09T10:30:00Z"), Qt::ISODate);
  QHash<qint64,QJsonObject> fixturePets;
  QJsonArray backpack, normal, elite;
  QList<QJsonObject> warehouseDetails;
  for (int index = 0; index < 42; ++index) {
    const auto value = pet(index, goods[index % goods.size()].raceIds.first(), index < 12);
    if (index < 12) backpack.append(value);
    else {
      (index%4 ? normal : elite).append(PreviewInventory::brief(value));
      warehouseDetails.append(value);
    }
    const qint64 id = value.value(QStringLiteral("id")).toInteger();
    fixturePets.insert(id,value);
  }
  ok &= require(fixture.publishLists(backpack,normal,elite,warehouseDetails) && fixture.factsReady(),
      "synthetic observations did not reach the real Repository/facts/Projection path");
  auto analysisData = std::make_shared<AnalysisViewSnapshot>();
  analysisData->publication = 1;
  analysisData->account = account;
  analysisData->sessionEpoch = 1;
  analysisData->hasAnalysis = true;
  analysisData->inventoryStale = true;
  analysisData->shopStale = true;
  analysisData->analyzedAt = observedAt;
  auto& overview = analysisData->overview;
  overview.account = account;
  overview.totalPets = 42;
  overview.backpackPets = 12;
  overview.normalWarehousePets = 22;
  overview.eliteWarehousePets = 8;
  overview.fullyCultivatedPets = 8;
  overview.improvablePets = 34;
  overview.redStarMissingPets = 18;
  overview.sacredMissingPets = 12;
  overview.soulMissingPets = 9;
  overview.shopImprovablePets = 16;
  overview.shopDataKnown = true;
  overview.routineDataKnown = true;
  overview.todayOpportunityKnown = true;
  overview.todayOpportunityRemaining = 8;
  overview.todayOpportunities.completeness = RoutineCompleteness::Complete;
  overview.todayOpportunities.total = 8;
  overview.todayOpportunities.confirmedSources = 5;
  overview.todayOpportunities.expectedSources = 5;
  overview.weekOpportunities.completeness = RoutineCompleteness::Complete;
  overview.weekOpportunities.total = 6;
  overview.weekOpportunities.confirmedSources = 2;
  overview.weekOpportunities.expectedSources = 2;
  for (int index = 0; index < 42; ++index) {
    const QJsonObject value = fixturePets.value(800001 + index);
    PetAssetRecord record;
    record.instanceId = 800001 + index;
    record.raceId = value.value(QStringLiteral("r")).toInt();
    record.name = value.value(QStringLiteral("n")).toString();
    record.location = index < 12 ? QStringLiteral("背包") : QStringLiteral("普通仓库");
    record.detailAvailable = true;
    record.currentPowerKnown = record.highestPowerKnown = record.completionKnown = true;
    record.powerGapKnown = record.cultivationKnown = true;
    record.currentPower = 28000 + index * 180;
    record.highestPower = 33000 + index * 180;
    record.completionPercent = 84 + index % 12;
    record.improvable = true;
    record.shopImprovable = index % 3 == 0;
    record.pet = AssetDerivation::identityFields(value);
    record.gaps = {QStringLiteral("元魂尚有缺口"), QStringLiteral("红色星神待补齐")};
    overview.pets.append(record);
    overview.totalCurrentPower += record.currentPower;
    if (index < 6) {
      ActionRecommendation recommendation;
      recommendation.stableId = QStringLiteral("preview:%1").arg(record.instanceId);
      recommendation.petInstanceId = record.instanceId;
      recommendation.petName = record.name;
      recommendation.type = RecommendationType::ConditionUnknown;
      recommendation.unknownConditionCount = 1;
      recommendation.title = QStringLiteral("来源尚未确认，兑换条件待确认");
      recommendation.shopGoodKey = goods[index % goods.size()].stableKey();
      recommendation.shopName = goods[index % goods.size()].shopName;
      recommendation.goodName = goods[index % goods.size()].description;
      recommendation.completionPercent = record.completionPercent;
      recommendation.completionKnown = true;
      recommendation.remainingExchangeCount = 2;
      recommendation.supportedGapCount = 1;
      recommendation.gaps = record.gaps;
      ResourceRequirement requirement;
      requirement.resourceKey = QStringLiteral("4:3237");
      requirement.resourceName = QStringLiteral("永恒战场勋章");
      requirement.required = 450;
      requirement.owned = index < 3 ? 920 : 310;
      requirement.ownedKnown = true;
      recommendation.requirements = {requirement};
      analysisData->recommendations.append(recommendation);
    }
  }
  analysisData->inventory = {account, observedAt, 42, 12, 22, 8, 0};
  analysisData->routine = overview;
  analysisData->historyChanged = true;
  analysisData->autoSnapshotKnown = true;
  for (int day = 0; day < 4; ++day) {
    AccountAssetSnapshot snapshot;
    snapshot.schemaVersion = 2;
    snapshot.analysisVersion = 6;
    snapshot.account = account;
    snapshot.createdAt = observedAt.addDays(day - 3);
    snapshot.storageKey = QStringLiteral("preview-%1").arg(day);
    snapshot.totalPets = 39 + day;
    snapshot.fullyCultivatedPets = 5 + day;
    snapshot.totalCurrentPower = 1100000 + day * 48000;
    snapshot.petsComplete = false;
    analysisData->history.append(snapshot);
  }
  analysis.publish(analysisData);
  QCoreApplication::processEvents();

  QJsonObject shopPacket;
  QHash<QString, qint64> counts;
  for (const auto& good : goods) {
    const QString shopKey = QStringLiteral("si%1").arg(good.shopId);
    auto shop = shopPacket.value(shopKey).toObject();
    shop.insert(good.itemKey(), QJsonObject{{good.limitKey, 0}});
    shopPacket.insert(shopKey, shop);
  }
  counts.insert(QStringLiteral("4:3237"), 920);
  counts.insert(QStringLiteral("134:1"), 1260);
  int gameRequests = 0;
  std::array<int, 4> factories{};
  WorkbenchWindow window([&](WorkbenchPage page, QWidget* parent) -> QWidget* {
    ++factories[static_cast<int>(page)];
    if (page == WorkbenchPage::Pets) {
      auto* widget = new PetWindow(&inventory, parent, &sharedImages);
      QObject::connect(widget, &PetWindow::detailRequested, widget, [&, widget](qint64) {
        ++gameRequests;
        widget->setStatus(QStringLiteral("离线样例：仅展示合成缓存，不连接游戏"));
      });
      QObject::connect(widget, &PetWindow::listRefreshRequested, widget, [&] { ++gameRequests; });
      return widget;
    }
    if (page == WorkbenchPage::Shop) {
      auto* widget = new ShopWindow(&inventory, parent, &sharedImages);
      widget->setCatalogSnapshot(ShopExchangeCatalog::instance().snapshot(),QDate(2026,9,9));
      widget->setPacket(shopPacket, true);
      widget->setMaterialCounts(counts, true);
      QObject::connect(widget, &ShopWindow::refreshRequested, widget, [&] { ++gameRequests; });
      QObject::connect(widget, &ShopWindow::detailRequested, widget, [&](qint64) { ++gameRequests; });
      return widget;
    }
    if (page == WorkbenchPage::Routine) {
      auto* widget = new RoutineOverviewWindow(parent);
      widget->setCatalogSnapshot(RoutineOverviewCatalog::instance().snapshot());
      widget->setData(daily(), true, {10011, 10025}, true,
          {{QStringLiteral("2_36_1"), QJsonObject{{QStringLiteral("t"), 6}}}});
      widget->setStatus(QStringLiteral("离线样例 · 日常、周常与活动数据均为合成"));
      QObject::connect(widget, &RoutineOverviewWindow::refreshRequested, widget, [&] { ++gameRequests; });
      return widget;
    }
    return new AssetAnalysisWindow(&analysis, parent);
  }, nullptr, WorkbenchOptions{QSize(1920, 1080), QSize(1280, 800)});
  window.setSession({account, 1, QStringLiteral("离线合成快照 · 09-09 19:30"), QStringLiteral("V1.1.4 · 离线预览"), false});
  window.setTask({QStringLiteral("详情队列 · 23 / 42 · 后台任务示例"), true, 23, 42});
  window.setPersistence({PersistenceState::Saved, QStringLiteral("演示状态；不会写入真实账号目录")});
  window.setTargetValidator([&](const NavigationTarget& target) {
    if (target.page == WorkbenchPage::Pets) return fixturePets.contains(target.petInstanceId);
    if (target.page == WorkbenchPage::Shop)
      for (const auto& good : goods) if (good.stableKey() == target.goodKey) return true;
    return false;
  });
  window.setTargetHandler([](QWidget* page, const NavigationTarget& target) {
    if (auto* pets = qobject_cast<PetWindow*>(page)) { pets->focusPet(target.petInstanceId); return true; }
    if (auto* shop = qobject_cast<ShopWindow*>(page)) { shop->focusGood(target.goodKey); return true; }
    return false;
  });
  window.show();
  QCoreApplication::processEvents();
  ok &= require(window.size() == QSize(1280, 800) && window.page(WorkbenchPage::Pets) &&
      window.creationCount(WorkbenchPage::Pets) == 1 && factories[0] == 1 &&
      factories[1] == 0, "shell geometry or lazy first-page creation failed");
  const auto* petsPage = qobject_cast<PetWindow*>(window.page(WorkbenchPage::Pets));
  auto* search = petsPage->findChild<QLineEdit*>();
  search->setText(QStringLiteral("星河"));
  auto* mainWarehouse = petsPage->findChild<QTableView*>(QStringLiteral("KQPetWarehouseTable"));
  const auto checkInventoryFit = [&] {
    bool fits = true;
    for (const QString& name : {QStringLiteral("KQPetBackpackTable"), QStringLiteral("KQPetWarehouseTable")}) {
      auto* table = petsPage->findChild<QTableView*>(name);
      int width = 0;
      for (int column=0; table && column<table->model()->columnCount(); ++column) width += table->columnWidth(column);
      fits &= table && table->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && width <= table->viewport()->width();
    }
    return fits;
  };
  ok &= require(checkInventoryFit(), "pet columns overflowed the viewport after initial workbench sizing");
  auto* backpackTable = petsPage->findChild<QTableWidget*>(QStringLiteral("KQPetBackpackTable"));
  ok &= require(backpackTable && backpackTable->viewport()->height() >= 12*backpackTable->verticalHeader()->defaultSectionSize(),
      "normal workbench did not leave room for all twelve backpack rows");
  auto* sidebar = window.findChild<QWidget*>(QStringLiteral("KQWorkbenchSidebar"));
  auto* accountLabel = window.findChild<QLabel*>(QStringLiteral("KQWorkbenchAccount"));
  auto* sourceLabel = window.findChild<QLabel*>(QStringLiteral("KQWorkbenchSource"));
  auto* taskLabel = window.findChild<QLabel*>(QStringLiteral("KQWorkbenchTask"));
  auto* statusLog = window.findChild<QPlainTextEdit*>(QStringLiteral("KQWorkbenchStatusLog"));
  ok &= require(sidebar && accountLabel && sourceLabel && taskLabel && statusLog &&
      sidebar->isAncestorOf(accountLabel) && sidebar->isAncestorOf(sourceLabel) && sidebar->isAncestorOf(taskLabel) &&
      sidebar->isAncestorOf(statusLog) && !window.findChild<QLabel*>(QStringLiteral("KQWorkbenchTitle")) &&
      !petsPage->findChild<QLabel*>(QStringLiteral("KQPetInlineStatus"))->isVisible() &&
      !petsPage->findChild<QLabel*>(QStringLiteral("KQPetInlineProgress"))->isVisible(),
      "session/status content still occupied duplicate rows above or below the pet tables");
  QElapsedTimer modelReady; modelReady.start();
  const auto modelsReady = [&] {
    for (auto* model : petsPage->findChildren<PetTableModel*>()) if (model->preparationRunning()) return false;
    return mainWarehouse && mainWarehouse->model() && mainWarehouse->model()->rowCount() > 0;
  };
  while (!modelsReady() && modelReady.elapsed() < 5000) QTest::qWait(5);
  // Filtering uses its own 150 ms debounce; select after both the asynchronous
  // rows and this explicit user query are applied, then test warm persistence.
  QTest::qWait(160);
  ok &= require(modelsReady(),"pet page did not finish async rows before selecting its persistent index");
  if (mainWarehouse && mainWarehouse->model()->rowCount() > 0) mainWarehouse->setCurrentIndex(mainWarehouse->model()->index(0, 0));
  const QPersistentModelIndex selection = mainWarehouse ? QPersistentModelIndex(mainWarehouse->currentIndex()) : QPersistentModelIndex{};
  const int requestsBeforeSwitches = gameRequests;
  for (int repeat = 0; repeat < 3; ++repeat)
    for (int index = 0; index < 4; ++index) window.showPage(static_cast<WorkbenchPage>(index));
  window.showPage(WorkbenchPage::Pets);
  QCoreApplication::processEvents();
  ok &= require(gameRequests == requestsBeforeSwitches && search->text() == QStringLiteral("星河") &&
      selection.isValid(), "warm page switching sent requests or discarded page state/selection");
  for (int count : factories) ok &= require(count == 1, "a page factory ran more than once");
  ok &= require(application.font() == originalFont && application.styleSheet() == originalStyle,
                "workbench changed the application's global font or stylesheet");

  QSignalSpy rejected(&window, &WorkbenchWindow::navigationRejected);
  ok &= require(!window.navigate({WorkbenchPage::Pets, QStringLiteral("other-account"), 1, 800001, {}}) &&
      !window.navigate({WorkbenchPage::Pets, account, 2, 800001, {}}) &&
      !window.navigate({WorkbenchPage::Pets, account, 1, 999999, {}}) && rejected.size() == 3,
      "navigation accepted a wrong account/epoch or nonexistent instance");
  ok &= require(window.navigate({WorkbenchPage::Pets, account, 1, 800001, {}}),
                "validated instance navigation failed");
  auto* petDetail = petsPage->findChild<QTextBrowser*>(QStringLiteral("KQPetPreparedDetail"));
  ok &= require(fixture.preparedVisible(petDetail,0,800001),"pet page did not render real prepared detail");
  window.showPage(WorkbenchPage::Shop);
  auto* shopPage = qobject_cast<ShopWindow*>(window.page(WorkbenchPage::Shop));
  shopPage->focusGood(goods.first().stableKey());
  auto* shopPets = shopPage->findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
  auto* shopDetail = shopPage->findChild<QTextBrowser*>(QStringLiteral("KQShopPreparedDetail"));
  const auto selectSharedPet = [&] {
    for (int row=0; shopPets && row<shopPets->rowCount(); ++row) {
      if (!shopPets->item(row,0) || shopPets->item(row,0)->data(Qt::UserRole).toLongLong()!=800001) continue;
      shopPets->setCurrentCell(row,0);
      QMetaObject::invokeMethod(shopPets,"cellClicked",Qt::DirectConnection,Q_ARG(int,row),Q_ARG(int,0));
      return true;
    }
    return false;
  };
  ok &= require(PreviewInventory::until(selectSharedPet) && fixture.preparedVisible(shopDetail,1,800001),
      "shop page did not render its own real prepared detail consumer");
  const auto petBeforeShopPage = inventory.preparedDetail(0,800001);
  const auto derivationsBeforePages = fixture.derivations.stats().computations;
  const int requestsBeforePages = gameRequests;
  ok &= require(fixture.followPage(shopDetail,1,800001,DetailSection::StargodBackpack,1) &&
      inventory.preparedDetail(0,800001)==petBeforeShopPage,
      "shop pagination did not load page two independently of pet detail");
  ok &= require(fixture.followPage(shopDetail,1,800001,DetailSection::Overview,0),"shop overview return did not rebuild its bounded section first pages");
  const auto shopBeforePetPage = inventory.preparedDetail(1,800001);
  window.showPage(WorkbenchPage::Pets);
  ok &= require(fixture.followPage(petDetail,0,800001,DetailSection::StargodBackpack,1) &&
      inventory.preparedDetail(1,800001)==shopBeforePetPage,
      "pet pagination did not load page two independently of shop detail");
  ok &= require(fixture.followPage(petDetail,0,800001,DetailSection::Overview,0) &&
      fixture.derivations.stats().computations==derivationsBeforePages && gameRequests==requestsBeforePages,
      "detail section navigation rederived facts or generated a game request");
  window.activateWindow();
  search->setFocus();
  QCoreApplication::processEvents();
  QTest::keyClick(&window, Qt::Key_3, Qt::ControlModifier);
  QCoreApplication::processEvents();
  ok &= require(window.currentPage() == WorkbenchPage::Routine, "Ctrl+3 did not select the local page");
  QTest::keyClick(&window, Qt::Key_1, Qt::ControlModifier);
  QCoreApplication::processEvents();
  window.findChild<QPushButton*>(QStringLiteral("KQWorkbenchPage0"))->setFocus();
  QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
  QCoreApplication::processEvents();
  ok &= require(search->hasFocus(), "Ctrl+F did not focus this page's search");
  QWidget unrelated;
  unrelated.show();
  unrelated.activateWindow();
  unrelated.setFocus();
  QCoreApplication::processEvents();
  const WorkbenchPage originalPage = window.currentPage();
  QTest::keyClick(&unrelated, Qt::Key_4, Qt::ControlModifier);
  QCoreApplication::processEvents();
  ok &= require(window.currentPage() == originalPage, "workbench shortcuts captured another window's keys");
  unrelated.hide();
  window.activateWindow();

  search->setText(QStringLiteral("星河"));
  window.setSession({account, 2, QStringLiteral("新会话样例"), QStringLiteral("V1.1.4 · 离线预览"), false});
  auto* detail = petsPage->findChild<QTextBrowser*>();
  ok &= require(search->text() == QStringLiteral("星河") && detail &&
      detail->toPlainText().contains(QStringLiteral("请选择一只精灵")),
      "session change kept private detail or discarded public search preference");
  // Reauthenticate through the synthetic transport, then replay observations.
  ok &= require(fixture.login(account) && fixture.repository.sessionGeneration()==2 &&
      fixture.publishLists(backpack,normal,elite,warehouseDetails) && fixture.factsReady(),
      "new preview epoch did not replace the production projected inputs");
  analysisData = std::make_shared<AnalysisViewSnapshot>(*analysisData);
  analysisData->publication = 2;
  analysisData->sessionEpoch = 2;
  analysis.publish(analysisData);
  QCoreApplication::processEvents();
  if (auto* shop = qobject_cast<ShopWindow*>(window.page(WorkbenchPage::Shop))) {
    shop->setPacket(shopPacket, true);
    shop->setMaterialCounts(counts, true);
    shop->focusGood(goods.first().stableKey());
    shop->setStatus(QStringLiteral("离线样例 · 仅展示合成快照，不连接游戏"));
  }
  if (auto* routine = qobject_cast<RoutineOverviewWindow*>(window.page(WorkbenchPage::Routine))) {
    routine->setData(daily(), true, {10011, 10025}, true,
                    {{QStringLiteral("2_36_1"), QJsonObject{{QStringLiteral("t"), 6}}}});
    routine->setStatus(QStringLiteral("离线合成观察 · 来源尚未确认"));
  }
  window.setSession({account, 2, QStringLiteral("离线合成快照 · 09-09 19:30"), QStringLiteral("V1.1.4 · 离线预览"), false});
  window.setTask({QStringLiteral("详情队列 · 23 / 42 · 后台任务示例"), true, 23, 42});
  window.setPersistence({PersistenceState::Saved, QStringLiteral("样例状态")});
  search->clear();
  QCoreApplication::processEvents();
  window.navigate({WorkbenchPage::Pets, account, 2, 800001, {}});
  ok &= require(fixture.preparedVisible(petDetail,0,800001),"new epoch selected pet did not finish prepared detail before PNG export");
  QElapsedTimer imageWait;
  imageWait.start();
  const QString selectedVisual = petVisualKey(fixturePets.value(800001));
  while ((sharedImages.cachedPetImage(selectedVisual).isEmpty() || sharedImages.attributeIcon(QStringLiteral("9")).isNull() ||
          sharedImages.attributeIcon(QStringLiteral("3")).isNull()) && imageWait.elapsed() < 3000) QTest::qWait(10);
  ok &= require(!sharedImages.cachedPetImage(selectedVisual).isEmpty(), "selected pet portrait did not finish before visual QA");
  ok &= require(!sharedImages.attributeIcon(QStringLiteral("9")).isNull() &&
      !sharedImages.attributeIcon(QStringLiteral("3")).isNull(), "attribute icons did not finish for the preview DPI");

  QString outputRoot = qEnvironmentVariable("KQPET_WORKBENCH_OUTPUT");
  if (outputRoot.isEmpty()) outputRoot = QDir::current().filePath(QStringLiteral("workbench-preview"));
  QDir().mkpath(outputRoot);
  const QStringList names{QStringLiteral("pets"), QStringLiteral("shop"), QStringLiteral("routine"), QStringLiteral("assets")};
  for (int index = 0; index < 4; ++index) {
    window.showPage(static_cast<WorkbenchPage>(index));
    QCoreApplication::processEvents();
    if (index == 1) {
      auto* table = window.page(WorkbenchPage::Shop)->findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
      ok &= require(table && table->rowCount() > 0, "shop fixture has no eligible instance to inspect");
      if (table && table->rowCount() > 0) {
        table->setCurrentCell(0, 0);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->item(0,0)).center());
        const qint64 id = table->item(0,0)->data(Qt::UserRole).toLongLong();
        auto* browser = window.page(WorkbenchPage::Shop)->findChild<QTextBrowser*>(QStringLiteral("KQShopPreparedDetail"));
        ok &= require(fixture.preparedVisible(browser,1,id),"shop selected detail was not prepared before PNG export");
      }
    }
    const QString path = QDir(outputRoot).absoluteFilePath(
        QStringLiteral("workbench-%1-%2.png").arg(names[index]).arg(dpi));
    ok &= require(window.grab().save(path), "failed to export a workbench page PNG");
    std::fprintf(stdout, "PNG: %s\n", path.toUtf8().constData());
  }
  window.showPage(WorkbenchPage::Pets);
  for (QTabWidget* tabs : petsPage->findChildren<QTabWidget*>()) {
    if (tabs->count() == 2 && tabs->tabText(1) == QStringLiteral("原始数据")) {
      tabs->setCurrentIndex(1);
      QCoreApplication::processEvents();
      const QString path = QDir(outputRoot).absoluteFilePath(QStringLiteral("workbench-raw-%1.png").arg(dpi));
      ok &= require(window.grab().save(path), "failed to export raw-data PNG");
      std::fprintf(stdout, "PNG: %s\n", path.toUtf8().constData());
      tabs->setCurrentIndex(0);
      break;
    }
  }
  window.showPage(WorkbenchPage::Assets);
  for (QTabWidget* tabs : window.page(WorkbenchPage::Assets)->findChildren<QTabWidget*>()) {
    if (tabs->count() == 4 && tabs->tabText(3) == QStringLiteral("历史快照")) {
      tabs->setCurrentIndex(3);
      QCoreApplication::processEvents();
      const QString path = QDir(outputRoot).absoluteFilePath(QStringLiteral("workbench-history-%1.png").arg(dpi));
      ok &= require(window.grab().save(path), "failed to export history PNG");
      std::fprintf(stdout, "PNG: %s\n", path.toUtf8().constData());
      tabs->setCurrentIndex(0);
      break;
    }
  }
  window.setAvailableLogicalSize(QSize(800, 600));
  window.resize(784, 584);
  window.showPage(WorkbenchPage::Pets);
  QCoreApplication::processEvents();
  ok &= require(checkInventoryFit(), "pet columns overflowed after switching to small-screen layout");
  auto* inventorySplit = petsPage->findChild<QSplitter*>(QStringLiteral("KQPetInventorySplitter"));
  auto* compactToggle = petsPage->findChild<QPushButton*>(QStringLiteral("KQPetCompactDetailToggle"));
  ok &= require(window.width() <= 784 && window.height() <= 584 && window.sidebarCollapsed() &&
      inventorySplit && inventorySplit->orientation() == Qt::Vertical &&
      compactToggle && compactToggle->isVisible(), "small-screen layout exceeded logical bounds or lost stacked inventory");
  const QString smallPath = QDir(outputRoot).absoluteFilePath(QStringLiteral("workbench-small-%1.png").arg(dpi));
  ok &= require(window.grab().save(smallPath), "failed to export the small-screen PNG");
  std::fprintf(stdout, "PNG: %s\n", smallPath.toUtf8().constData());
  compactToggle->click();
  QCoreApplication::processEvents();
  const QString smallDetailPath = QDir(outputRoot).absoluteFilePath(QStringLiteral("workbench-small-detail-%1.png").arg(dpi));
  ok &= require(window.grab().save(smallDetailPath), "failed to export compact selected-detail PNG");
  std::fprintf(stdout, "PNG: %s\n", smallDetailPath.toUtf8().constData());
  for (QTabWidget* tabs : petsPage->findChildren<QTabWidget*>()) {
    if (tabs->count() == 2 && tabs->tabText(1) == QStringLiteral("原始数据")) {
      tabs->setCurrentIndex(1);
      QCoreApplication::processEvents();
      const QString path = QDir(outputRoot).absoluteFilePath(QStringLiteral("workbench-small-raw-%1.png").arg(dpi));
      ok &= require(window.grab().save(path), "failed to export compact raw-data PNG");
      std::fprintf(stdout, "PNG: %s\n", path.toUtf8().constData());
      tabs->setCurrentIndex(0);
      break;
    }
  }
  const int requestsBeforeClose = gameRequests;
  window.close();
  ok &= require(!window.isVisible() && window.page(WorkbenchPage::Pets) == petsPage &&
      gameRequests == requestsBeforeClose, "closing destroyed pages or generated a game request");
  window.show();
  QCoreApplication::processEvents();
  ok &= require(window.creationCount(WorkbenchPage::Pets) == 1,
                "reopening rebuilt an existing page");
  window.hide();
  images.shutdown();
  ok &= require(fixture.close(), "shared preview executors did not stop");
  ok &= verifyHostWindowLifecycle(application,window);
  if (!ok) return 1;
  std::fprintf(stdout, "PASS: four native pages, lazy factory, local shortcuts, scoped routes, hide/reopen, host lifecycle, DPI preview\n");
  return 0;
}
