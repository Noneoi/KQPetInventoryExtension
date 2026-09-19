#include "support/catalog_test_support.h"
#include "application/catalog/pet_detail_catalog.h"
#include "application/catalog/routine_overview_catalog.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "fixtures/legacy_shop_catalog_20260813.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) {
  if (!valid) std::fprintf(stderr, "FAIL: %s\n", message);
  return valid;
}
template<class Predicate>
bool waitUntil(Predicate predicate, int timeout = 5000) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return predicate();
}
QByteArray shopSource(const QString& name) {
  return QStringLiteral("public static const SERVER_ID_1_REWARD_CONFIG:Array = [{\"serverId\":1,"
      "\"tab\":1,\"basicDescription\":\"%1\",\"shelfTime\":\"20260101\",\"removalTime\":\"21000101\","
      "\"limit\":\"3:2\",\"cost\":\"4:3237:10\",\"simpleParams\":\"CommonEnhancePrize,x,x,62,9001#9011\","
      "\"unlock\":\"\"}];").arg(name).toUtf8();
}

bool cacheValidation(const QString& root) {
  bool ok = true;
  StorageService storage(root);
  CatalogIoOptions options;
  options.officialRoot = QDir(root).filePath(QStringLiteral("official"));
  options.petOverlayPath = QDir(root).filePath(QStringLiteral("catalog/pet-detail-data.json"));
  CatalogIoService service(&storage, options);
  const auto initialShop = ShopExchangeCatalog::instance().snapshot();
  QString invalidLimitSource = QString::fromUtf8(shopSource(QStringLiteral("unknown-limit")));
  invalidLimitSource.replace(QStringLiteral("\"limit\":\"3:2\""), QStringLiteral("\"limit\":\"bad:2\""));
  const auto unknownLimit = ShopExchangeCatalog::prepare(ShopExchangeCatalog::parseOfficialText(invalidLimitSource,
      initialShop->root.value(QStringLiteral("protocol")).toObject()), QStringLiteral("fixture"), {});
  ok &= check(unknownLimit && unknownLimit->allShops.front().goods.front().limitKey.isEmpty() &&
                  unknownLimit->allShops.front().goods.front().limitCount == -1,
              "invalid official quota prefix silently became a known daily quota");
  QString monthlySource = QString::fromUtf8(shopSource(QStringLiteral("new designated pet name")));
  monthlySource.replace(QStringLiteral("SERVER_ID_1_"), QStringLiteral("SERVER_ID_9_"));
  monthlySource.replace(QStringLiteral("21000101"), QStringLiteral("2026929"));
  monthlySource.prepend(QStringLiteral("public static const TOTAL_CONFIG:Object = {\"new-shop\":{"
      "\"serverId\":9,\"name\":\"new-shop\",\"isOnline\":\"TRUE\","
      "\"startTime\":\"20260828\",\"removalTime\":\"20260929\"}};"));
  const auto monthly = ShopExchangeCatalog::prepare(ShopExchangeCatalog::parseOfficialText(monthlySource,
      initialShop->root.value(QStringLiteral("protocol")).toObject()), QStringLiteral("fixture"), {});
  ok &= check(monthly && monthly->allShops.size() == 1 && monthly->allShops.front().shopId == 9 &&
                  monthly->allShops.front().name == QStringLiteral("new-shop") &&
                  monthly->allShops.front().goods.front().shelfDate == QDate(2026, 8, 28) &&
                  monthly->allShops.front().goods.front().removalDate == QDate(2026, 9, 29),
              "dynamic designated-pet shop or official date rollover/shop interval was dropped");
  const QString shopPath = QDir(root).filePath(QStringLiteral("catalog/shop-exchange-data.json"));
  ok &= check(writeCatalogFixture(shopPath, QJsonDocument(initialShop->root).toJson()), "shop cache fixture failed");
  ok &= check(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload), "valid shop cache did not publish");
  const auto loadedShop = ShopExchangeCatalog::instance().snapshot();
  ok &= check(loadedShop->revision > initialShop->revision && initialShop->sourceLabel != loadedShop->sourceLabel,
              "shop snapshot was mutated in-place or had no new revision");
  auto broken = loadedShop->root;
  QJsonArray shops = broken.value(QStringLiteral("shops")).toArray();
  shops.append(42); broken.insert(QStringLiteral("shops"), shops);
  writeCatalogFixture(shopPath, QJsonDocument(broken).toJson());
  ok &= check(!runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload) &&
                  ShopExchangeCatalog::instance().snapshot() == loadedShop,
              "invalid shop sibling partially replaced the previous snapshot");
  QJsonObject olderOfficial = initialShop->root;
  QJsonObject olderSource = olderOfficial.value(QStringLiteral("source")).toObject();
  olderSource.insert(QStringLiteral("shopVersion"), QStringLiteral("2026081364897007"));
  olderSource.insert(QStringLiteral("resources"), QJsonObject{{QStringLiteral("shop"), QJsonObject{
      {QStringLiteral("url"), QStringLiteral("https://aoqi.100bt.com/play/newactivityext/newact20260313/"
          "storeexchangeframework/storeexchangeframework~2026081364897007.swf")}}}});
  olderOfficial.insert(QStringLiteral("source"), olderSource);
  const QByteArray oldBytes = QJsonDocument(olderOfficial).toJson();
  writeCatalogFixture(shopPath, oldBytes);
  QString reloadReason;
  ok &= check(!runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload, &reloadReason) &&
                  ShopExchangeCatalog::instance().snapshot() == loadedShop &&
                  reloadReason.contains(QStringLiteral("早于")),
              "older versioned official cache replaced the current bundled catalog");
  QFile retainedFile(shopPath);
  ok &= check(retainedFile.open(QIODevice::ReadOnly) && retainedFile.readAll() == oldBytes,
              "superseding an old catalog modified or removed the user's cache file");
  retainedFile.close();
  const auto legacy = QJsonDocument::fromJson(QByteArray(kLegacyShopCatalog20260813)).object();
  writeCatalogFixture(shopPath, QJsonDocument(legacy).toJson());
  ok &= check(!runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload) &&
                  ShopExchangeCatalog::instance().snapshot() == loadedShop,
              "known unversioned 2026-08-13 official cache replaced the new bundled catalog");
  QJsonObject custom = legacy;
  QJsonArray customShops = custom.value(QStringLiteral("shops")).toArray();
  QJsonObject customShop = customShops.first().toObject();
  QJsonArray customGoods = customShop.value(QStringLiteral("goods")).toArray();
  QJsonObject customGood = customGoods.first().toObject();
  customGood.insert(QStringLiteral("description"), QStringLiteral("用户自定义培养项目"));
  customGoods[0] = customGood; customShop.insert(QStringLiteral("goods"), customGoods);
  customShops[0] = customShop; custom.insert(QStringLiteral("shops"), customShops);
  writeCatalogFixture(shopPath, QJsonDocument(custom).toJson());
  ok &= check(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload) &&
                  ShopExchangeCatalog::instance().snapshot()->allShops.front().goods.front().description ==
                      QStringLiteral("用户自定义培养项目"),
              "unknown-version custom catalog was silently treated as obsolete official data");
  // A future official cache must remain loadable; comparison is against the
  // immutable bundled version even after an unknown local overlay was loaded.
  QJsonObject future = olderOfficial;
  QJsonObject futureSource = olderSource;
  futureSource.insert(QStringLiteral("shopVersion"), QStringLiteral("2026091764897007"));
  futureSource.insert(QStringLiteral("resources"), QJsonObject{{QStringLiteral("shop"), QJsonObject{
      {QStringLiteral("url"), QStringLiteral("https://aoqi.100bt.com/play/newactivityext/newact20260313/"
          "storeexchangeframework/storeexchangeframework~2026091764897007.swf")}}}});
  future.insert(QStringLiteral("source"), futureSource);
  writeCatalogFixture(shopPath, QJsonDocument(future).toJson());
  ok &= check(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload),
              "newer official cache was incorrectly superseded by the bundled catalog");
  writeCatalogFixture(shopPath, QJsonDocument(initialShop->root).toJson());
  ok &= check(runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::Reload),
              "current official catalog could not be restored after cache precedence checks");
  const auto initialDetail = PetDetailCatalog::instance().snapshot();
  const QString detailPath = QDir(root).filePath(QStringLiteral("catalog/pet-detail-data.json"));
  const auto versionedDetail = [](QJsonObject detail, const QString& version) {
    detail.insert(QStringLiteral("source"), QJsonObject{
        {QStringLiteral("petDictionaryVersion"),version},
        {QStringLiteral("resources"),QJsonObject{{QStringLiteral("petDictionaryUpdate"),QJsonObject{
            {QStringLiteral("url"),QStringLiteral("https://aoqi.100bt.com/play/pet/petdictionarydataupdate~%1.swf").arg(version)}}}}}});
    return detail;
  };
  QJsonObject oldDetail = versionedDetail(initialDetail->root, QStringLiteral("2026081364897007"));
  QJsonObject oldPets = oldDetail.value(QStringLiteral("pets")).toObject();
  oldPets.insert(QStringLiteral("7529"), QJsonObject{{QStringLiteral("name"),QStringLiteral("obsolete-name")},
      {QStringLiteral("attributes"),QStringLiteral("24")},{QStringLiteral("jobs"),QStringLiteral("26")}});
  oldDetail.insert(QStringLiteral("pets"),oldPets);
  const QByteArray oldDetailBytes = QJsonDocument(oldDetail).toJson(QJsonDocument::Compact);
  writeCatalogFixture(detailPath,oldDetailBytes);
  ok &= check(!runCatalogRequest(&service,CatalogKind::PetDetail,CatalogRequestMode::Reload) &&
      PetDetailCatalog::instance().snapshot() == initialDetail &&
      PetDetailCatalog::instance().resolvedAttributes(QJsonObject{{QStringLiteral("r"),7529}}) == QStringLiteral("神灵"),
      "old official pet cache replaced current embedded identities on cold load");
  QFile oldDetailFile(detailPath);
  ok &= check(oldDetailFile.open(QIODevice::ReadOnly) && oldDetailFile.readAll() == oldDetailBytes,
      "superseding old pet metadata modified the existing cache file");
  oldDetailFile.close();
  auto futureDetail = versionedDetail(initialDetail->root,QStringLiteral("2026091764897007"));
  auto futurePets = futureDetail.value(QStringLiteral("pets")).toObject();
  futurePets.insert(QStringLiteral("990003"), QJsonObject{
      {QStringLiteral("name"),QStringLiteral("未来官方皮肤名")},
      {QStringLiteral("attributes"),QStringLiteral("26")},{QStringLiteral("jobs"),QStringLiteral("22")},
      {QStringLiteral("sign"),QStringLiteral("神运,灵初,皮肤")}});
  futureDetail.insert(QStringLiteral("pets"),futurePets);
  writeCatalogFixture(detailPath,QJsonDocument(futureDetail).toJson(QJsonDocument::Compact));
  QString futureReason;
  const bool futureLoaded = runCatalogRequest(&service,CatalogKind::PetDetail,CatalogRequestMode::Reload,&futureReason);
  ok &= check(futureLoaded, "future official pet metadata was rejected");
  if (!futureLoaded) std::fprintf(stderr,"future pet metadata reason: %s\n",futureReason.toUtf8().constData());
  ok &= check(futureLoaded && PetDetailCatalog::instance().petName(990003) == QStringLiteral("未来官方皮肤名") &&
      PetDetailCatalog::instance().pet(990003).value(QStringLiteral("sign")).toString() == QStringLiteral("神运,灵初,皮肤") &&
      PetDetailCatalog::instance().resolvedEra(QJsonObject{{QStringLiteral("r"),990003}}) == QStringLiteral("灵初"),
      "new official pet/sign was not preserved or a skin without an era prefix lost its official era");
  const auto latestDetail = PetDetailCatalog::instance().snapshot();
  writeCatalogFixture(detailPath,QJsonDocument(initialDetail->root).toJson(QJsonDocument::Compact));
  ok &= check(!runCatalogRequest(&service,CatalogKind::PetDetail,CatalogRequestMode::Reload) &&
      PetDetailCatalog::instance().snapshot() == latestDetail,
      "older official pet metadata replaced a newer already-loaded snapshot");
  const QJsonObject overlay{{QStringLiteral("schema"), 1}, {QStringLiteral("pets"), QJsonObject{
      {QStringLiteral("1"), QJsonObject{{QStringLiteral("name"), QStringLiteral("catalog-new-name")},
          {QStringLiteral("attributes"), QStringLiteral("4")}, {QStringLiteral("jobs"), QStringLiteral("1")}}}}}};
  writeCatalogFixture(detailPath, QJsonDocument(overlay).toJson());
  ok &= check(runCatalogRequest(&service, CatalogKind::PetDetail, CatalogRequestMode::Reload) &&
                  PetDetailCatalog::instance().petName(1) == QStringLiteral("catalog-new-name") &&
                  !PetDetailCatalog::instance().pet(2).isEmpty() &&
                  initialDetail->root.value(QStringLiteral("pets")).toObject().value(QStringLiteral("1")).toObject()
                      .value(QStringLiteral("name")).toString() != QStringLiteral("catalog-new-name"),
              "detail overlay did not preserve embedded siblings or mutated a held snapshot");
  const auto loadedDetail = PetDetailCatalog::instance().snapshot();
  const QJsonObject dynamicNames{{QStringLiteral("attributes"),QJsonObject{{QStringLiteral("99"),QStringLiteral("新增属性")}}},
      {QStringLiteral("jobs"),QJsonObject{{QStringLiteral("98"),QStringLiteral("新增职业甲")},{QStringLiteral("99"),QStringLiteral("新增职业乙")}}},
      {QStringLiteral("fusionJobs"),QJsonArray{QJsonObject{{QStringLiteral("name"),QStringLiteral("新增组合职业")},
          {QStringLiteral("jobs"),QJsonArray{QJsonArray{98},QJsonArray{99}}}}}},
      {QStringLiteral("money"),QJsonObject{{QStringLiteral("777"),QJsonObject{{QStringLiteral("name"),QStringLiteral("新增货币")}}}}}};
  const auto named = PetDetailCatalog::prepareOverlay(initialDetail,dynamicNames,QStringLiteral("new names fixture"),{});
  ok &= check(named && PetMetadataView(named).attributes(QStringLiteral("99")) == QStringLiteral("新增属性") &&
      PetMetadataView(named).jobs(QStringLiteral("0,98,99")) == QStringLiteral("新增组合职业") &&
      PetMetadataView(named).moneyName(777) == QStringLiteral("新增货币"),
      "new official attribute/job/fusion/currency names were not consumed by the metadata view");
  const QJsonObject legacyRules{{QStringLiteral("stargods"),QJsonObject{
      {QStringLiteral("71"),QJsonObject{{QStringLiteral("name"),QStringLiteral("legacy red star")}}}}},
      {QStringLiteral("pets"),QJsonObject{{QStringLiteral("7529"),QJsonObject{
          {QStringLiteral("name"),QStringLiteral("legacy pet without sign")}}}}},
      {QStringLiteral("astrolabe"),QJsonObject{{QStringLiteral("0"),QJsonObject{
          {QStringLiteral("name"),QStringLiteral("legacy wheel")}}}}}};
  const auto upgradedRules = PetDetailCatalog::prepareOverlay(initialDetail,legacyRules,QStringLiteral("legacy fixture"),{});
  ok &= check(upgradedRules && upgradedRules->root.value(QStringLiteral("stargods")).toObject()
      .value(QStringLiteral("71")).toObject().value(QStringLiteral("type")).toInt() == 16 &&
      upgradedRules->root.value(QStringLiteral("astrolabe")).toObject().value(QStringLiteral("0")).toObject()
          .value(QStringLiteral("battlePower")).toInt() > 0 &&
      upgradedRules->root.value(QStringLiteral("pets")).toObject().value(QStringLiteral("7529")).toObject()
          .value(QStringLiteral("sign")) == initialDetail->root.value(QStringLiteral("pets")).toObject()
              .value(QStringLiteral("7529")).toObject().value(QStringLiteral("sign")),
      "legacy local catalog erased newly bundled star type and wheel power rules");
  auto badRules = legacyRules;
  badRules.insert(QStringLiteral("stargods"),QJsonObject{{QStringLiteral("71"),QJsonObject{
      {QStringLiteral("name"),QStringLiteral("bad")},{QStringLiteral("limitJobs"),QJsonArray{QStringLiteral("oops")}}}}});
  ok &= check(!PetDetailCatalog::prepareOverlay(initialDetail,badRules,QStringLiteral("invalid fixture"),{}),
      "invalid profession restriction was accepted as a complete rule");
  const QJsonObject badSign{{QStringLiteral("pets"),QJsonObject{{QStringLiteral("990003"),QJsonObject{
      {QStringLiteral("name"),QStringLiteral("invalid sign")},{QStringLiteral("sign"),42}}}}}};
  ok &= check(!PetDetailCatalog::prepareOverlay(initialDetail,badSign,QStringLiteral("invalid sign fixture"),{}),
      "a numeric sign was accepted as official pet era metadata");
  writeCatalogFixture(detailPath, QByteArray("{\"schema\":1,\"pets\":{\"1\":{\"name\":\"bad\",\"stargodSlotMaxLevel\":1.5}}}"));
  ok &= check(!runCatalogRequest(&service, CatalogKind::PetDetail, CatalogRequestMode::Reload) &&
                  PetDetailCatalog::instance().snapshot() == loadedDetail,
              "invalid metadata numeric field replaced the previous complete overlay");
  writeCatalogFixture(detailPath, QByteArray(options.maximumSourceBytes + 1, 'x'));
  ok &= check(!runCatalogRequest(&service, CatalogKind::PetDetail, CatalogRequestMode::Reload) &&
                  PetDetailCatalog::instance().snapshot() == loadedDetail,
              "oversized catalog bypassed the file limit");
  ok &= check(writeRoutineOfficialFixture(options.officialRoot) &&
                  runCatalogRequest(&service, CatalogKind::Routine, CatalogRequestMode::OfficialUpdate),
              "bounded routine official scan/parse/save failed");
  const auto routine = RoutineOverviewCatalog::instance().snapshot();
  ok &= check(routine->tasks.size() == 1 && routine->activities.size() == 1 &&
                  routine->activities.front().redPointIds.contains(102), "routine source fields were lost");
  writeCatalogFixture(QDir(options.officialRoot).filePath(QStringLiteral("tasks/DiamondTaskConfig.as")),
      QByteArray("new DiamondTaskDefine(1,\"invalid\",not_a_number,15,15,3,15);"));
  ok &= check(!runCatalogRequest(&service, CatalogKind::Routine, CatalogRequestMode::OfficialUpdate) &&
                  RoutineOverviewCatalog::instance().snapshot() == routine,
              "invalid official counter silently became zero or replaced old catalog");
  std::printf("catalog max IO slice: %lld us; entries: %llu\n",
      static_cast<long long>(service.stats().maximumSliceMicroseconds),
      static_cast<unsigned long long>(service.stats().scannedEntries));
  service.close();
  ok &= check(service.requestReload(CatalogKind::Shop) == 0 && storage.shutdown(), "closing catalog accepted more work");
  return ok;
}

bool commitAndCoalescing(const QString& root) {
  QSemaphore entered, release;
  std::atomic_bool hold{true};
  bool ok = true;
  StorageService storage(root, {}, [&](const QString& path, const QByteArray& bytes) {
    if (path.endsWith(QStringLiteral("shop-exchange-data.json")) && hold.exchange(false)) {
      entered.release(); release.acquire();
    }
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
    const qint64 size = file.write(bytes);
    return StorageWriteAttempt{size == bytes.size() && file.commit(), size, file.errorString()};
  });
  CatalogIoOptions options;
  options.officialRoot = QDir(root).filePath(QStringLiteral("official"));
  const QString source = QDir(options.officialRoot).filePath(QStringLiteral("storeexchangeframework/SEFConfig.as"));
  ok &= check(writeCatalogFixture(source, shopSource(QStringLiteral("first"))), "coalescing official fixture failed");
  CatalogIoService service(&storage, options);
  int publications = 0, completions = 0;
  QObject::connect(&service, &CatalogIoService::catalogUpdated, &service, [&](CatalogKind, quint64) { ++publications; });
  QObject::connect(&service, &CatalogIoService::finished, &service, [&](quint64, CatalogKind, StorageStatus, const QString&) { ++completions; });
  const auto original = ShopExchangeCatalog::instance().snapshot();
  service.requestOfficialUpdate(CatalogKind::Shop);
  ok &= check(waitUntil([&] { return entered.available() > 0; }), "catalog commit fixture did not reach IO");
  int heartbeat = 0;
  QTimer heart; heart.setInterval(1);
  QObject::connect(&heart, &QTimer::timeout, &heart, [&] { ++heartbeat; });
  heart.start();
  for (int i = 0; i < 100; ++i) service.requestOfficialUpdate(CatalogKind::Shop);
  ok &= check(waitUntil([&] { return heartbeat >= 3; }) && service.stats().pendingRequests <= 2 &&
                  service.stats().pendingWrites == 1 && publications == 0 && ShopExchangeCatalog::instance().snapshot() == original,
              "waiting IO blocked Core, published before Saved or accumulated full candidates");
  writeCatalogFixture(source, shopSource(QStringLiteral("latest")));
  release.release();
  ok &= check(waitUntil([&] { return service.stats().pendingRequests == 0; }, 10000) && publications == 1 && completions == 101 &&
                  ShopExchangeCatalog::instance().snapshot()->allShops.front().goods.front().description == QStringLiteral("latest"),
              "coalesced/obsolete catalog request published or latest request did not finish");
  service.close();
  ok &= check(storage.shutdown(), "catalog commit storage did not drain");
  return ok;
}

bool scanBounds(const QString& root) {
  CatalogIoOptions options;
  options.officialRoot = QDir(root).filePath(QStringLiteral("official"));
  options.maximumScanEntries = 3;
  for (int i = 0; i < 8; ++i)
    writeCatalogFixture(QDir(options.officialRoot).filePath(QStringLiteral("file-%1.txt").arg(i)), QByteArray("x"));
  writeCatalogFixture(QDir(options.officialRoot).filePath(QStringLiteral("storeexchangeframework/SEFConfig.as")), shopSource(QStringLiteral("must-not-publish")));
  StorageService storage(root);
  CatalogIoService service(&storage, options);
  const auto original = ShopExchangeCatalog::instance().snapshot();
  bool ok = check(!runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::OfficialUpdate) &&
                      service.stats().scannedEntries == 4 && ShopExchangeCatalog::instance().snapshot() == original,
                  "exhausted directory scan published a partial candidate or exceeded its bound");
  service.close();
  ok &= check(storage.shutdown(), "bounded scan storage did not drain");
  return ok;
}

bool failureAndDestruction(const QString& root) {
  bool ok = true;
  CatalogIoOptions options;
  options.officialRoot = QDir(root).filePath(QStringLiteral("official"));
  writeCatalogFixture(QDir(options.officialRoot).filePath(QStringLiteral("storeexchangeframework/SEFConfig.as")), shopSource(QStringLiteral("failed-write")));
  const QString cachePath = QDir(root).filePath(QStringLiteral("catalog/shop-exchange-data.json"));
  writeCatalogFixture(cachePath, QByteArray("keep-old-file"));
  const auto original = ShopExchangeCatalog::instance().snapshot();
  {
    StorageService storage(root, {}, [](const QString&, const QByteArray&) {
      return StorageWriteAttempt{false, 0, QStringLiteral("injected atomic write failure")};
    });
    CatalogIoService service(&storage, options);
    ok &= check(!runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::OfficialUpdate) &&
                    ShopExchangeCatalog::instance().snapshot() == original,
                "failed catalog persistence replaced the last published version");
    QFile file(cachePath); file.open(QIODevice::ReadOnly);
    ok &= check(file.readAll() == QByteArray("keep-old-file"), "failed catalog write replaced its old file");
    service.close();
    ok &= check(storage.shutdown(), "failed catalog storage did not drain");
  }
  {
    QSemaphore entered, release;
    StorageService storage(root);
    const bool held = storage.postAuxiliary([&](QObject*) { entered.release(); release.acquire(); });
    ok &= check(held && entered.tryAcquire(1, 2000), "destruction fixture did not hold IO");
    auto service = std::make_unique<CatalogIoService>(&storage, options);
    service->requestOfficialUpdate(CatalogKind::Shop);
    ok &= check(waitUntil([&] { return storage.state().outstandingTasks == 2; }),
                "catalog IO bootstrap was not queued behind existing work");
    service.reset();
    release.release();
    ok &= check(waitUntil([&] { return storage.state().outstandingTasks == 0; }) && storage.shutdown() &&
                    ShopExchangeCatalog::instance().snapshot() == original,
                "destroyed catalog receiver was used or its obsolete candidate published");
  }
  return ok;
}

bool optionalOfficialIntegration(const QString& root) {
  const QString official = qEnvironmentVariable("KQPET_TEST_OFFICIAL_UNPACK_ROOT");
  if (official.isEmpty()) return true;
  StorageService storage(root);
  CatalogIoOptions options; options.officialRoot = official;
  CatalogIoService service(&storage, options);
  QString error;
  bool ok = runCatalogRequest(&service, CatalogKind::Shop, CatalogRequestMode::OfficialUpdate, &error, 20000);
  ok &= check(ok, qPrintable(QStringLiteral("real official shop: %1").arg(error)));
  const bool routine = runCatalogRequest(&service, CatalogKind::Routine, CatalogRequestMode::OfficialUpdate, &error, 20000);
  ok &= check(routine, qPrintable(QStringLiteral("real official routine: %1").arg(error)));
  std::printf("official integration: %lld us maximum IO slice, %llu scanned entries\n",
      static_cast<long long>(service.stats().maximumSliceMicroseconds),
      static_cast<unsigned long long>(service.stats().scannedEntries));
  service.close();
  return storage.shutdown() && ok;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  bool ok = check(root.isValid(), "temporary catalog root unavailable");
  ok &= cacheValidation(root.filePath(QStringLiteral("cache")));
  ok &= commitAndCoalescing(root.filePath(QStringLiteral("coalesce")));
  ok &= scanBounds(root.filePath(QStringLiteral("scan-limit")));
  ok &= failureAndDestruction(root.filePath(QStringLiteral("fail-close")));
  ok &= optionalOfficialIntegration(root.filePath(QStringLiteral("real-official")));
  if (ok) std::puts("PASS: bounded catalog IO, immutable publication, strict overlays, Saved gating and coalescing");
  return ok ? 0 : 1;
}
