#include "application/shop/shop_exchange_controller.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "application/pet/pet_repository.h"
#include "support/protocol_test_support.h"
#include "storage/storage_service.h"
#include "support/catalog_test_support.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QTemporaryDir>
#include <cstdio>

namespace {
bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}
template<class Predicate> bool until(Predicate predicate) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < 15000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return predicate();
}
QJsonObject item(int id, int count, int used = 0) {
  return {{QStringLiteral("i"), id}, {QStringLiteral("n"), count}, {QStringLiteral("uq"), used}};
}
QJsonObject materials(int soul, int source) {
  return {{QStringLiteral("_cmd"), QStringLiteral("3_11")},
      {QStringLiteral("4"), QJsonArray{item(3237, soul, 50)}},
      {QStringLiteral("24"), QJsonArray{item(10001, source, 99)}}};
}
QJsonObject sourcePack(int id, int level, int count, int experience = 0) {
  return {{QStringLiteral("dpi"), id}, {QStringLiteral("lvl"), level},
      {QStringLiteral("eqn"), count}, {QStringLiteral("exp"), experience}};
}
QJsonObject sources(int source) {
  return {{QStringLiteral("_cmd"), QStringLiteral("2_32_0")},
      {QStringLiteral("eps"), QJsonArray{sourcePack(10001, 1, source > 0 ? source - 1 : 0),
          sourcePack(10001, 6, source > 0 ? 1 : 0)}}};
}
QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  StorageService storage(root.path());
  // Keep the complete static catalog, but isolate this material-cache test
  // from the separately tested activity query stages after a shop refresh.
  auto protocolFixture = ShopExchangeCatalog::instance().snapshot()->root;
  auto fixtureShops = protocolFixture.value(QStringLiteral("shops")).toArray();
  for (auto entry = fixtureShops.begin(); entry != fixtureShops.end(); ++entry) {
    auto shop = entry->toObject();
    if (!shop.value(QStringLiteral("sourceKey")).toString().isEmpty()) shop.remove(QStringLiteral("observation"));
    *entry = shop;
  }
  protocolFixture.insert(QStringLiteral("shops"),fixtureShops);
  CatalogIoService catalogs(&storage);
  bool ok = require(writeCatalogFixture(QDir(root.path()).filePath(QStringLiteral("catalog/shop-exchange-data.json")),
      QJsonDocument(protocolFixture).toJson(QJsonDocument::Compact)) &&
      runCatalogRequest(&catalogs,CatalogKind::Shop,CatalogRequestMode::Reload),
      "material-only catalog fixture did not load through CatalogIo");
  ok &= require(ShopExchangeCatalog::instance().snapshot()->root.value(QStringLiteral("shops")).toArray() == fixtureShops,
      "material fixture removed or changed static activity projects");
  PetRepository repository(nullptr, &storage);
  repository.handlePacket(QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"cultivation-observation\"}}"));
  ok &= require(repository.isAuthenticated() && !repository.sessionContext().canPersist(),
                    "fixture did not establish a read-only identified account");
  ok &= require(PacketContracts::validateOutbound(QStringLiteral("PJXExtension"), QStringLiteral("2_32_0"), QStringLiteral("null")) &&
      !PacketContracts::validateOutbound(QStringLiteral("PJXExtension"), QStringLiteral("2_32_0"), QStringLiteral("{}")) &&
      !PacketContracts::validateOutbound(QStringLiteral("PJXExtension"), QStringLiteral("2_32_10"), QStringLiteral("null")),
      "the source inventory contract admitted a wrong shape or the ambiguous write command");
  auto invalidPack = sourcePack(10001, 0, 3);
  ok &= require(!PacketContracts::decodeSourceBeastInventory({{QStringLiteral("eps"), QJsonArray{invalidPack}}}).valid(),
      "a malformed or non-equippable zero-level source pack was counted");
  invalidPack = sourcePack(10001, 1, 3);
  invalidPack.remove(QStringLiteral("eqn"));
  invalidPack.insert(QStringLiteral("eqi"), 123);
  ok &= require(!PacketContracts::decodeSourceBeastInventory({{QStringLiteral("eps"), QJsonArray{invalidPack}}}).valid(),
      "an equipment instance was accepted in place of the complete warehouse pack quantity");
  ShopExchangeController controller(&repository);
  QList<QString> requests;
  QString status;
  QObject::connect(&controller, &ShopExchangeController::statusChanged, &controller,
                   [&](const QString& value) { status = value; });
  controller.setSender([&](const QString& extension, const QString& command, const QString& parameters) {
    requests.append(extension + QLatin1Char('|') + command + QLatin1Char('|') + parameters);
    return true;
  });
  quint64 sequence = 10;
  const auto deliver = [&](ShopExchangeController& target, const QJsonObject& packet, quint64 epoch) {
    InboundEnvelope envelope;
    envelope.receiveSequence = ++sequence;
    envelope.receivedMonotonicMs = transportMonotonicMs();
    envelope.capturedSessionEpoch = epoch;
    target.handleDecodedEnvelope(envelope, packet);
  };
  ok &= require(until([&] { return !controller.cacheLoading(); }) && requests.isEmpty(),
                "loading material cache automatically queried the game");
  deliver(controller, materials(7, 3), repository.sessionGeneration());
  ok &= require(controller.cultivationMaterialInventory().counts.isEmpty(),
                "an unrequested weak packet changed material quantities");
  ok &= require(controller.requestCultivationMaterials() && requests.size() == 2 &&
      requests.first() == QStringLiteral("MaterialExtension|3_11|{}") &&
      requests.last() == QStringLiteral("PJXExtension|2_32_0|null") &&
      controller.cultivationMaterialInventory().running && !controller.requestCultivationMaterials(),
      "manual material refresh queried shop quotas or admitted a duplicate request");
  deliver(controller, materials(7, 3), repository.sessionGeneration() + 1);
  ok &= require(controller.isRunning() && controller.cultivationMaterialInventory().counts.isEmpty(),
                "a wrong-session response changed the material snapshot");
  deliver(controller, materials(7, 3), 0);
  ok &= require(controller.isRunning() && controller.cultivationMaterialInventory().running &&
      !controller.cultivationMaterialInventory().knownTypes.contains(24),
      "generic material type 24 was trusted as the source-beast warehouse or ended the two-source refresh");
  deliver(controller, sources(3), repository.sessionGeneration() + 1);
  ok &= require(!controller.cultivationMaterialInventory().knownTypes.contains(24),
      "a wrong-session source-beast response changed stock");
  auto sourceReply = sources(3);
  sourceReply.insert(QStringLiteral("petEquips"), QJsonArray{sourcePack(10001, 6, 500)});
  deliver(controller, sourceReply, 0);
  auto snapshot = controller.cultivationMaterialInventory();
  ok &= require(!controller.isRunning() && !snapshot.running && snapshot.counts.value(QStringLiteral("4:3237")) == 7 &&
      snapshot.counts.value(QStringLiteral("24:10001:1")) == 3 && snapshot.knownTypes == QSet<int>({4, 24}) &&
      snapshot.knownExtraGroups.contains(QStringLiteral("24:1")) && snapshot.observedAt.isValid() &&
      !snapshot.knownTypes.contains(35) && controller.cachedMaterialCounts().isEmpty() && !controller.hasMaterialCounts() &&
      !repository.sessionContext().canPersist(),
      "valid read-only quantities disappeared, subtracted usedUp, lost source-beast selector, or gained shop authority");
  const auto accountRoot = repository.storageContext()->directory();
  const auto materialPath = QDir(accountRoot).filePath(QStringLiteral("cultivation-materials.json"));
  ok &= require(until([&] { return controller.pendingWriteCount() == 0; }) && QFile::exists(materialPath) &&
      !QFile::exists(QDir(accountRoot).filePath(QStringLiteral("shops.json"))),
      "read-only material quantities were not saved separately from authoritative shop cache");

  const auto savedMaterialBytes = readFile(materialPath);
  requests.clear();
  ok &= require(controller.requestInfo() && requests.size() == 2 &&
      !controller.cultivationMaterialInventory().running,
      "shop refresh was treated as a manual cultivation material refresh");
  deliver(controller, materials(99, 88), 0);
  const auto& catalog = ShopExchangeCatalog::instance();
  QJsonObject shopReply{{QStringLiteral("_cmd"), catalog.getInfoCommand()}};
  for (const auto& shop : catalog.protocolShops())
    shopReply.insert(QStringLiteral("si%1").arg(shop.shopId), QJsonObject{});
  deliver(controller, shopReply, 0);
  ok &= require(!controller.isRunning() && controller.cultivationMaterialInventory().counts == snapshot.counts &&
      controller.cultivationMaterialInventory().observedTimes == snapshot.observedTimes &&
      until([&] { return controller.pendingWriteCount() == 0; }) &&
      !savedMaterialBytes.isEmpty() && readFile(materialPath) == savedMaterialBytes,
      "ordinary shop refresh overwrote the explicit material observation or its disk cache");
  requests.clear();
  ok &= require(controller.requestCultivationMaterials() && requests.size() == 2,
      "explicit material refresh after shop refresh did not start");
  deliver(controller, materials(9, 4), 0);
  deliver(controller, sources(4), 0);
  snapshot = controller.cultivationMaterialInventory();
  ok &= require(snapshot.counts.value(QStringLiteral("4:3237")) == 9 &&
      snapshot.counts.value(QStringLiteral("24:10001:1")) == 4 &&
      until([&] { return controller.pendingWriteCount() == 0; }) && readFile(materialPath) != savedMaterialBytes,
      "explicit material refresh did not replace the prior quantities on disk");

  ok &= require(controller.requestCultivationMaterials(), "malformed-group refresh did not start");
  deliver(controller, {{QStringLiteral("_cmd"), QStringLiteral("3_11")},
      {QStringLiteral("4"), QJsonArray{item(3237, 9), item(3237, 10)}},
      {QStringLiteral("24"), QStringLiteral("bad")}, {QStringLiteral("35"), QJsonArray{item(1, 999)}}}, 0);
  deliver(controller, {{QStringLiteral("_cmd"), QStringLiteral("2_32_0")},
      {QStringLiteral("eps"), QJsonArray{sourcePack(10001, 1, 99), sourcePack(10001, 1, 99)}}}, 0);
  ok &= require(controller.cultivationMaterialInventory().counts == snapshot.counts && status.contains(QStringLiteral("无效")),
                "duplicate IDs, an invalid group, or an unrelated group replaced valid material counts");
  ok &= require(controller.requestCultivationMaterials(), "empty-group refresh did not start");
  deliver(controller, {{QStringLiteral("_cmd"), QStringLiteral("3_11")}, {QStringLiteral("4"), QJsonArray{}}}, 0);
  deliver(controller, {{QStringLiteral("_cmd"), QStringLiteral("2_32_0")}}, 0);
  snapshot = controller.cultivationMaterialInventory();
  ok &= require(snapshot.knownTypes.contains(4) && !snapshot.counts.contains(QStringLiteral("4:3237")) &&
      snapshot.counts.value(QStringLiteral("24:10001:1")) == 4 && status.contains(QStringLiteral("源兽仓库列表未返回")),
      "an explicit empty group was not zero or a missing group erased its prior count");
  ok &= require(until([&] { return controller.pendingWriteCount() == 0; }), "material update did not settle");

  ok &= require(controller.requestCultivationMaterials(), "source-empty refresh did not start");
  deliver(controller, materials(5, 999), 0);
  deliver(controller, {{QStringLiteral("_cmd"), QStringLiteral("2_32_0")}, {QStringLiteral("eps"), QJsonArray{}}}, 0);
  snapshot = controller.cultivationMaterialInventory();
  ok &= require(snapshot.knownTypes.contains(24) && snapshot.knownExtraGroups.contains(QStringLiteral("24:1")) &&
      !snapshot.counts.contains(QStringLiteral("24:10001:1")) && snapshot.counts.value(QStringLiteral("4:3237")) == 5,
      "an explicit empty warehouse did not become known zero or generic type24 leaked into it");

  ok &= require(controller.requestCultivationMaterials(), "partial-timeout refresh did not start");
  deliver(controller, materials(6, 999), 0);
  const auto timer = controller.findChild<QTimer*>();
  if (timer) timer->start(1);
  ok &= require(timer && until([&] { return !controller.isRunning(); }) && status.contains(QStringLiteral("源兽仓库")) &&
      status.contains(QStringLiteral("超时")) && !controller.cultivationMaterialInventory().counts.contains(QStringLiteral("24:10001:1")),
      "a partial timeout lost its warning or fabricated source-beast stock");

  // Receive a fresh group while the historical file is still queued for read.
  ShopExchangeController lateReader(&repository);
  lateReader.setSender([](const QString&, const QString&, const QString&) { return true; });
  ok &= require(lateReader.requestCultivationMaterials(), "late-read fixture did not start");
  deliver(lateReader, materials(11, 6), 0);
  deliver(lateReader, sources(6), 0);
  ok &= require(until([&] { return lateReader.pendingStorageCount() == 0; }) &&
      lateReader.cultivationMaterialInventory().counts.value(QStringLiteral("4:3237")) == 11 &&
      lateReader.cultivationMaterialInventory().counts.value(QStringLiteral("24:10001:1")) == 6,
      "a delayed historical read replaced a newer observation");
  // Cache schema 1 derived source beasts from the wrong 3_11 group. Keep its
  // ordinary item history but require a warehouse observation for source stock.
  auto legacy = QJsonDocument::fromJson(readFile(materialPath)).object();
  legacy.insert(QStringLiteral("schema"), 1);
  legacy.remove(QStringLiteral("sourceInventory"));
  {
    QFile oldFile(materialPath);
    ok &= require(oldFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "legacy fixture could not be created");
    oldFile.write(QJsonDocument(legacy).toJson(QJsonDocument::Compact));
  }
  ShopExchangeController legacyReader(&repository);
  ok &= require(until([&] { return !legacyReader.cacheLoading(); }) &&
      legacyReader.cultivationMaterialInventory().counts.value(QStringLiteral("4:3237")) == 11 &&
      !legacyReader.cultivationMaterialInventory().knownTypes.contains(24),
      "legacy generic type24 quantities were promoted to known consumable source inventory");
  legacyReader.setSender([](const QString&, const QString&, const QString&) { return true; });
  ok &= require(legacyReader.requestCultivationMaterials(), "legacy source repair did not start");
  deliver(legacyReader, materials(11, 0), 0);
  deliver(legacyReader, sources(6), 0);
  ok &= require(until([&] { return legacyReader.pendingStorageCount() == 0; }), "legacy repaired cache did not settle");
  repository.setConnectionState(SessionConnectionState::Disconnected, QStringLiteral("offline fixture"));
  ShopExchangeController offline(&repository);
  ok &= require(until([&] { return !offline.cacheLoading(); }) &&
      offline.cultivationMaterialInventory().counts.value(QStringLiteral("4:3237")) == 11 &&
      !offline.requestCultivationMaterials() && !repository.isAuthenticated(),
      "same-account offline cache was not restored or attempted an online read");

  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("different-material-account")}}}});
  ok &= require(controller.cultivationMaterialInventory().counts.isEmpty() &&
      lateReader.cultivationMaterialInventory().counts.isEmpty() && offline.cultivationMaterialInventory().counts.isEmpty(),
      "changing account retained another account's quantities");
  ok &= require(until([&] { return controller.pendingStorageCount() == 0 && lateReader.pendingStorageCount() == 0 &&
      offline.pendingStorageCount() == 0 && legacyReader.pendingStorageCount() == 0; }) && waitForRepositoryIdle(&repository), "fixture I/O did not settle");
  ok &= require(storage.shutdown(), "fixture storage did not shut down");
  if (ok) std::puts("PASS: manual material inventory, separate durable observations, grouped quantities and offline recovery");
  return ok ? 0 : 1;
}
