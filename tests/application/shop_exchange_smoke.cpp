#include "application/catalog/shop_exchange_catalog.h"
#include "application/shop/shop_exchange_controller.h"
#include "application/shop/shop_legacy_adapters.h"
#include "application/pet/pet_repository.h"
#include "support/protocol_test_support.h"
#include "storage/storage_service.h"
#include "support/catalog_test_support.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QSet>

#include <cstdio>
#include <limits>

namespace {

bool require(bool condition, const char* message) {
  if (!condition)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

template<class Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 3000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return predicate();
}

ShopExchangeGood findGood(const QList<ShopExchangeShop>& shops, int shopId,
                          const QString& description) {
  for (const ShopExchangeShop& shop : shops) {
    if (shop.shopId != shopId) continue;
    for (const ShopExchangeGood& good : shop.goods) {
      if (good.description == description) return good;
    }
  }
  return {};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const ShopExchangeCatalog& catalog = ShopExchangeCatalog::instance();
  bool ok = true;
  ok &= require(catalog.isLoaded(), "shop exchange catalog did not load");
  ok &= require(catalog.extension() == QStringLiteral("TimelinessActExtension"),
                "shop extension mismatch");
  ok &= require(catalog.getInfoCommand() == QStringLiteral("1008_20260313_es_0"),
                "shop getInfo command mismatch");
  const QDate date(2026, 9, 20);
  // Freeze the six established SEF shops separately from independently named
  // activity sources; local shopId values may overlap across those sources.
  const QList<ShopExchangeShop> shops = catalog.protocolShops(date);
  ok &= require(shops.size() == 6, "expected six established protocol shops");
  for (const auto& shop : shops)
    ok &= require(shop.sourceKey.isEmpty(), "an activity source entered the established protocol shop list");
  int activityShopCount = 0, activityGoodCount = 0;
  QSet<QString> activitySources;
  for (const auto& shop : catalog.snapshot()->allShops) {
    if (shop.sourceKey.isEmpty()) continue;
    ++activityShopCount;
    activityGoodCount += shop.goods.size();
    ok &= require(!activitySources.contains(shop.sourceKey), "built-in activity source was duplicated");
    activitySources.insert(shop.sourceKey);
    for (const auto& good : shop.goods)
      ok &= require(good.sourceKey == shop.sourceKey, "activity good lost its parent source identity");
  }
  ok &= require(activityShopCount == 9 && activityGoodCount == 76,
      "the frozen public activity snapshot is incomplete");
  ok &= require(catalog.shops(date).size() == shops.size() + activityShopCount,
      "merged catalog omitted established or independent activity shops");
  const ShopExchangeShop* kunwuSale = nullptr;
  for (const auto& shop : catalog.snapshot()->allShops)
    if (shop.sourceKey.contains(QStringLiteral("lingchukunwudiscountstoretab3"))) {
      kunwuSale = &shop;
      break;
    }
  ok &= require(kunwuSale && kunwuSale->goods.size() == 4 &&
                    kunwuSale->navigationLink == QStringLiteral("btnNewAct_lingchukunwudiscountstore_showMainPanel_3") &&
                    kunwuSale->activityEvidence == QStringLiteral("recent-release"),
                "current cultivation sale or its official navigation evidence is missing");
  if (kunwuSale) {
    const auto breakthrough = findGood({*kunwuSale}, kunwuSale->shopId, QStringLiteral("装备星迹突破"));
    const auto threeStars = findGood({*kunwuSale}, kunwuSale->shopId, QStringLiteral("装备任选红星3颗"));
    ok &= require(breakthrough.cost == QStringLiteral("8:2:29") && breakthrough.limitCount == 3 &&
                      breakthrough.enhanceType == QStringLiteral("89$1") && !breakthrough.quotaObservation.isEmpty() &&
                      threeStars.cost == QStringLiteral("8:2:109") && threeStars.limitCount == 1 &&
                      threeStars.enhanceType == QStringLiteral("39$3") &&
                      breakthrough.section == ShopExchangeSection::DiamondActivity &&
                      threeStars.section == ShopExchangeSection::DiamondActivity,
                  "current diamond prices, limits, cultivation meaning, or quota paths are incomplete");
  }

  const ShopExchangeGood redStar =
      findGood(shops, 1, QStringLiteral("指定通灵师装备1颗红星"));
  ok &= require(redStar.itemServerId == 4 && redStar.limitCount == 1 &&
                    redStar.limitKey == QStringLiteral("pl") &&
                    redStar.raceIds.size() == 10,
                "eternal battlefield red-star mapping mismatch");

  const ShopExchangeGood starShop =
      findGood(shops, 2, QStringLiteral("指定精灵源兽升1阶"));
  int starShopCount = 0;
  for (const ShopExchangeShop& shop : shops) {
    if (shop.shopId == 2) starShopCount = shop.goods.size();
  }
  ok &= require(starShop.itemServerId == 26 && starShop.limitKey == QStringLiteral("ml"),
                "aoqi star shop should keep the current monthly source-beast item");
  ok &= require(starShopCount == 1, "expired aoqi star shop item should be filtered by date");

  const ShopExchangeGood arenaStar =
      findGood(shops, 4, QStringLiteral("指定精灵装备1个1级红色星神"));
  ok &= require(arenaStar.itemServerId == 4 && arenaStar.limitCount == 1,
                "arena shop red stargod mapping mismatch");

  const ShopExchangeGood welfareBadge =
      findGood(shops, 5, QStringLiteral("指定精灵装备1个专属元魂"));
  ok &= require(welfareBadge.itemServerId == 3 && welfareBadge.raceIds.size() == 24,
                "monthly welfare exclusive badge mapping mismatch");

  const ShopExchangeGood league =
      findGood(shops, 6, QStringLiteral("指定精灵源兽升1阶"));
  ok &= require(league.itemServerId == 2 && league.limitKey == QStringLiteral("ml"),
                "league shop source-beast mapping mismatch");

  int onlineCount = 0;
  for (const auto& shop : shops) onlineCount += shop.goods.size();
  ok &= require(onlineCount == 25, "current established protocol designated-pet exchanges are incomplete");
  const QList<ShopExchangeGood> newlyIncluded{
      findGood(shops, 3, QStringLiteral("指定精灵满金星+万变金星")),
      findGood(shops, 3, QStringLiteral("指定精灵装备1颗红星")),
      findGood(shops, 5, QStringLiteral("指定精灵装备1颗红星")),
      findGood(shops, 5, QStringLiteral("指定精灵满金色星神+万变金星")),
      findGood(shops, 6, QStringLiteral("指定精灵元魂觉醒"))};
  for (const auto& good : newlyIncluded) {
    const auto rule = compileShopPetRule(good.enhanceType);
    bool knownRule = !rule.components.isEmpty();
    for (const auto& component : rule.components) knownRule &= component.componentIndex >= 0;
    ok &= require(good.itemServerId > 0 && !good.raceIds.isEmpty() && knownRule,
                  "new official reward is missing or its enhancement is unsupported");
  }
  ok &= require(welfareBadge.removalDate == QDate(2026, 9, 29) &&
                    welfareBadge.isOnlineOn(QDate(2026, 9, 29)) &&
                    !welfareBadge.isOnlineOn(QDate(2026, 9, 30)),
                "monthly official malformed item date ignored the real shop closing date");

  QJsonObject packet{
      {QStringLiteral("si1"),
       QJsonObject{{QStringLiteral("b4"), QJsonObject{{QStringLiteral("pl"), 1}}},
                   {QStringLiteral("b6"), QJsonObject{{QStringLiteral("pl"), 2}}}}},
      {QStringLiteral("si4"),
       QJsonObject{{QStringLiteral("bi4"),
                    QJsonObject{{QStringLiteral("wl"), 3}, {QStringLiteral("pl"), 0}}}}}};
  ok &= require(ShopExchangeCatalog::usedCount(packet, redStar) == 1 &&
                    ShopExchangeCatalog::remainingCount(packet, redStar) == 0,
                "period remaining should be limit minus siN.bM.pl");
  const ShopExchangeGood awaken =
      findGood(shops, 1, QStringLiteral("指定精灵源兽神觉升1阶"));
  ok &= require(ShopExchangeCatalog::remainingCount(packet, awaken) == 3,
                "unused remaining should be limit minus used");
  ok &= require(ShopExchangeCatalog::remainingCount(packet, arenaStar) == 1,
                "arena remaining should read si4.b4.pl rather than weekly leftover");
  const QJsonObject wrapped{{QStringLiteral("data"), packet}};
  ok &= require(ShopExchangeCatalog::remainingCount(wrapped, redStar) == 0,
                "wrapped server response should resolve the same remaining count");

  QJsonObject improvable{{QStringLiteral("czdlv"),
                          QJsonObject{{QStringLiteral("sgv"), 2450}}},
                         {QStringLiteral("mzdlv"),
                          QJsonObject{{QStringLiteral("sgv"), 2450}}},
                         {QStringLiteral("r"), 7001},
                         {QStringLiteral("sgsp"), QJsonArray{}},
                         {QStringLiteral("stargodSlotMaxLevel"), 8},
                         {QStringLiteral("sgs"),
                          QStringLiteral("29:6:29#0:6")}};
  const ShopPetEligibility canUse =
      analyzeShopPetEligibility(redStar, improvable, true);
  ok &= require(canUse.state == ShopPetEligibilityState::Usable,
                "a pet with a non-red stargod should be green/usable");
  const ShopPetEligibility missingDetail =
      analyzeShopPetEligibility(redStar, improvable, false);
  ok &= require(missingDetail.state == ShopPetEligibilityState::Unknown,
                "missing local detail must never be marked usable");
  auto unknownBackpack = improvable;
  unknownBackpack.remove(QStringLiteral("sgsp"));
  ok &= require(analyzeShopPetEligibility(redStar, unknownBackpack, true).state == ShopPetEligibilityState::Unknown,
                "missing star backpack was silently assumed to be empty");

  ShopExchangeGood sacredStar;
  sacredStar.enhanceType = QStringLiteral("91");
  QJsonObject sacredPet{{QStringLiteral("shenjue"),
                         QStringLiteral("1034#2#6|9:6")}};
  ok &= require(analyzeShopPetEligibility(sacredStar, sacredPet, true).state ==
                    ShopPetEligibilityState::Usable,
                "star plan 2 at 9/10 should be usable");
  sacredPet.insert(QStringLiteral("shenjue"), QStringLiteral("1034#2#6|10:6"));
  ok &= require(analyzeShopPetEligibility(sacredStar, sacredPet, true).state ==
                    ShopPetEligibilityState::NotUsable,
                "full sacred star plan must not be green");

  ShopExchangeGood sacredStage;
  sacredStage.enhanceType = QStringLiteral("92");
  sacredPet.insert(QStringLiteral("shenjue"), QStringLiteral("1034#2#6|10:6"));
  ok &= require(analyzeShopPetEligibility(sacredStage, sacredPet, true).state ==
                    ShopPetEligibilityState::Usable,
                "stage plan 6 at 6/7 should be usable");
  sacredPet.insert(QStringLiteral("shenjue"), QStringLiteral("1034#2#6|10:7"));
  ok &= require(analyzeShopPetEligibility(sacredStage, sacredPet, true).state ==
                    ShopPetEligibilityState::NotUsable,
                "full sacred stage plan must not be green");

  ShopExchangeGood badgeGood;
  badgeGood.enhanceType = QStringLiteral("44");
  const QJsonObject badgePet{{QStringLiteral("badge"),
                              QStringLiteral("101:5#201:0|102:5#202:1")}};
  ok &= require(analyzeShopPetEligibility(badgeGood, badgePet, true).state ==
                    ShopPetEligibilityState::Usable,
                "unawakened exclusive badge should be usable");

  // Currency data is requested and accepted only inside an explicit manual
  // shop refresh batch. An unrelated 3_11 packet must not mutate the cache.
  QTemporaryDir controllerRoot;
  qputenv("KQPET_DATA_ROOT", controllerRoot.path().toUtf8());
  PetRepository repository;
  // This fixture exercises the original SEF/material refresh stages. Keep all
  // activity goods for catalog checks, while their independent read pipeline
  // is covered by activity_shop_controller_smoke with explicit mock replies.
  auto protocolFixture = catalog.snapshot()->root;
  auto protocolFixtureShops = protocolFixture.value(QStringLiteral("shops")).toArray();
  for (auto entry = protocolFixtureShops.begin(); entry != protocolFixtureShops.end(); ++entry) {
    auto shop = entry->toObject();
    if (!shop.value(QStringLiteral("sourceKey")).toString().isEmpty()) shop.remove(QStringLiteral("observation"));
    *entry = shop;
  }
  protocolFixture.insert(QStringLiteral("shops"),protocolFixtureShops);
  CatalogIoService protocolFixtureIo(repository.storageService());
  ok &= require(writeCatalogFixture(QDir(controllerRoot.path()).filePath(QStringLiteral("catalog/shop-exchange-data.json")),
      QJsonDocument(protocolFixture).toJson(QJsonDocument::Compact)) &&
      runCatalogRequest(&protocolFixtureIo,CatalogKind::Shop,CatalogRequestMode::Reload),
      "SEF/material-only catalog fixture did not load through CatalogIo");
  ok &= require(catalog.snapshot()->root.value(QStringLiteral("shops")).toArray() == protocolFixtureShops,
      "SEF/material fixture removed or changed static activity projects");
  deliverVerifiedFixture(&repository,
      {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
       {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("shop-test")}}}});
  ShopExchangeController controller(&repository);
  QObject::connect(&repository, &PetRepository::packetObserved, &controller,
      [&controller](const QJsonObject& packet, const InboundEnvelope& envelope) {
        controller.handleDecodedEnvelope(envelope, packet);
      });
  struct SentRequest { QString service; QString command; QString params; };
  QList<SentRequest> requests;
  controller.setSender([&requests](const QString& service, const QString& command,
                                   const QString& params) {
    requests.append({service, command, params});
    return true;
  });
  ok &= require(controller.requestInfo(), "manual shop refresh did not start");
  ok &= require(requests.size() == 3 &&
                    requests.at(1).service == QStringLiteral("MaterialExtension") &&
                    requests.at(1).command == QStringLiteral("3_11") &&
                    requests.at(2).service == QStringLiteral("LeagueExtension") &&
                    requests.at(2).command == QStringLiteral("1015_2A") &&
                    requests.at(2).params == QStringLiteral("null"),
                "manual shop refresh must request material and personal contribution exactly once");
  const auto sendControllerPacket = [&repository](const QJsonObject& value) {
    deliverVerifiedFixture(&repository, value);
  };
  sendControllerPacket({{QStringLiteral("_cmd"), QStringLiteral("3_11")},
                        {QStringLiteral("r"), 1},
                        {QStringLiteral("4"),
                         QJsonArray{QJsonObject{{QStringLiteral("i"), 3237},
                                                {QStringLiteral("n"), 456}}}},
                        {QStringLiteral("8"),
                         QJsonArray{QJsonObject{{QStringLiteral("i"), 25},
                                                {QStringLiteral("n"), 120}},
                                    QJsonObject{{QStringLiteral("i"), 26},
                                                {QStringLiteral("n"), 34}}}}});
  sendControllerPacket({{QStringLiteral("_cmd"), catalog.getInfoCommand()},
                        {QStringLiteral("r"), 1},
                        {QStringLiteral("si1"), QJsonObject{}}});
  ok &= require(controller.hasMaterialCounts() &&
                    controller.materialCounts().value(QStringLiteral("4:3237")) == 456 &&
                    controller.materialCounts().value(QStringLiteral("8:2")) == 154 &&
                    !controller.materialCounts().contains(QStringLiteral("134:1")),
                "manual material response or official diamond total was not cached by material identity");
  sendControllerPacket(
      {{QStringLiteral("_cmd"), QStringLiteral("1015_2A")},
       {QStringLiteral("r"), 1},
       {QStringLiteral("infos"),
        QJsonObject{{QStringLiteral("UnionMemberInfo"),
                     QJsonObject{{QStringLiteral("lCToken"), 192147}}}}}});
  ok &= require(controller.materialCounts().value(QStringLiteral("134:1")) == 192147,
                "passive league overview did not update personal contribution currency");
  sendControllerPacket({{QStringLiteral("_cmd"), QStringLiteral("3_11")},
                        {QStringLiteral("r"), 1},
                        {QStringLiteral("4"),
                         QJsonArray{QJsonObject{{QStringLiteral("i"), 3237},
                                                {QStringLiteral("n"), 999}}}}});
  ok &= require(controller.materialCounts().value(QStringLiteral("4:3237")) == 456,
                "material response outside a manual batch mutated the cache");

  QJsonObject completeShop{{QStringLiteral("_cmd"), catalog.getInfoCommand()},
                            {QStringLiteral("r"), 1}};
  for (int shopId = 1; shopId <= 6; ++shopId)
    completeShop.insert(QStringLiteral("si%1").arg(shopId), QJsonObject{});
  completeShop.insert(QStringLiteral("si1"),
      QJsonObject{{QStringLiteral("bi4"), QJsonObject{{QStringLiteral("pl"), 1}}}});
  const auto materials = [](const QJsonValue& group) {
    QJsonObject value{{QStringLiteral("_cmd"), QStringLiteral("3_11")},
                       {QStringLiteral("r"), 1}, {QStringLiteral("8"),QJsonArray{}}};
    if (!group.isUndefined()) value.insert(QStringLiteral("4"), group);
    return value;
  };
  const auto material = [](const QJsonValue& count) {
    return QJsonArray{QJsonObject{{QStringLiteral("i"), 3237},
                                  {QStringLiteral("n"), count}}};
  };
  const auto leagueReply = [](const QJsonValue& count = QJsonValue(192147)) {
    return QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1015_2A")},
        {QStringLiteral("r"), 1},
        {QStringLiteral("infos"), QJsonObject{{QStringLiteral("UnionMemberInfo"),
            QJsonObject{{QStringLiteral("lCToken"), count}}}}}};
  };
  const auto withUnrelatedInventories = [](QJsonObject packet) {
    packet.insert(QStringLiteral("1"), QJsonObject{{QStringLiteral("wearing"), QJsonArray{}}});
    packet.insert(QStringLiteral("128"), QJsonArray{QStringLiteral("another-inventory-schema")});
    packet.insert(QStringLiteral("2"), 42);
    packet.insert(QStringLiteral("33"), QJsonArray{QJsonObject{{QStringLiteral("i"), 7}}});
    packet.insert(QStringLiteral("35"), QJsonValue::Null);
    return packet;
  };
  const QList<QJsonValue> invalidMaterialGroups{
      QJsonValue(QJsonValue::Undefined), QJsonValue(QJsonValue::Null), QJsonObject{},
      QJsonArray{QJsonValue(QJsonValue::Null)},
      QJsonArray{QJsonObject{{QStringLiteral("i"), 3237}}},
      QJsonArray{QJsonObject{{QStringLiteral("i"), 1.5}, {QStringLiteral("n"), 4}}},
      material(-1), material(0.5), material(QStringLiteral("not-a-number")),
      material(9007199254740992.0), material(QStringLiteral("9223372036854775808")),
      QJsonArray{QJsonObject{{QStringLiteral("i"), 3237}, {QStringLiteral("n"), 1}},
                 QJsonObject{{QStringLiteral("i"), 3237}, {QStringLiteral("n"), 2}}}};
  const QDateTime materialTime = controller.observedAt(QStringLiteral("material:4"));
  for (const QJsonValue& invalid : invalidMaterialGroups) {
    ok &= require(controller.requestInfo(), "invalid-material batch did not start");
    sendControllerPacket(materials(invalid));
    // A malformed material reply must not abort the successful sibling.
    sendControllerPacket(completeShop);
    sendControllerPacket(leagueReply());
    ok &= require(!controller.isRunning() && controller.hasPacket() &&
                      controller.cachedMaterialCounts().value(QStringLiteral("4:3237")) == 456 &&
                      !controller.materialCounts().contains(QStringLiteral("4:3237")) &&
                      controller.observedAt(QStringLiteral("material:4")) == materialTime,
                  "invalid materials replaced history, stayed fresh or discarded shop sibling");
  }
  ok &= require(controller.requestInfo(), "partial-material batch did not start");
  QString materialStatus;
  QObject::connect(&controller, &ShopExchangeController::statusChanged, &controller,
                   [&](const QString& status) { materialStatus = status; });
  QJsonObject partialMaterial = withUnrelatedInventories(materials(material(0)));
  partialMaterial.insert(QStringLiteral("9876"), QJsonObject{});
  sendControllerPacket(partialMaterial);
  sendControllerPacket(completeShop);
  sendControllerPacket(leagueReply());
  ok &= require(controller.materialCounts().contains(QStringLiteral("4:3237")) &&
                    controller.materialCounts().value(QStringLiteral("4:3237")) == 0 &&
                    !controller.materialCounts().contains(QStringLiteral("4:3189")) &&
                    controller.fieldState(QStringLiteral("material:9876")) == PacketFieldState::Missing &&
                    !materialStatus.contains(QStringLiteral("材料类型")),
                "unused inventory groups caused false currency errors or explicit zero was lost");

  ok &= require(controller.requestInfo(), "empty-material batch did not start");
  sendControllerPacket(materials(QJsonArray{}));
  sendControllerPacket(completeShop);
  sendControllerPacket(leagueReply());
  ok &= require(controller.hasMaterialCounts() &&
                    !controller.cachedMaterialCounts().contains(QStringLiteral("4:3237")) &&
                    controller.fieldState(QStringLiteral("material:4")) == PacketFieldState::Empty,
                "explicit empty material group was confused with missing or invented zero");

  const QJsonObject oldSecondShop = controller.packet().value(QStringLiteral("si2")).toObject();
  const QDateTime oldSecondTime = controller.observedAt(QStringLiteral("si2"));
  ok &= require(controller.requestInfo(), "partial-shop batch did not start");
  sendControllerPacket({{QStringLiteral("_cmd"), catalog.getInfoCommand()},
                        {QStringLiteral("si1"), QJsonObject{{QStringLiteral("bi4"),
                            QJsonObject{{QStringLiteral("pl"), 0}}}}},
                        {QStringLiteral("si2"), QStringLiteral("wrong-type")}});
  sendControllerPacket(materials(material(17)));
  sendControllerPacket(leagueReply());
  ok &= require(!controller.hasPacket() &&
                    ShopExchangeCatalog::usedCount(controller.packet(), redStar) == 0 &&
                    controller.packet().value(QStringLiteral("si2")).toObject() == oldSecondShop &&
                    controller.observedAt(QStringLiteral("si2")) == oldSecondTime &&
                    controller.fieldState(QStringLiteral("si2")) == PacketFieldState::Invalid &&
                    controller.fieldState(QStringLiteral("si3")) == PacketFieldState::Missing,
                "partial shop data replaced invalid siblings or claimed complete currentness");

  for (const QJsonObject& badShop : QList<QJsonObject>{
       {{QStringLiteral("si1"), QJsonObject{{QStringLiteral("bi4"),
          QJsonObject{{QStringLiteral("pl"), 0.5}}}}}},
       {{QStringLiteral("si1"), QJsonObject{{QStringLiteral("bi4"),
          QJsonObject{{QStringLiteral("pl"), 1}}}, {QStringLiteral("b4"),
          QJsonObject{{QStringLiteral("pl"), 2}}}}}},
       {{QStringLiteral("si1"), QJsonObject{}}, {QStringLiteral("data"),
          QJsonObject{{QStringLiteral("si1"), QJsonObject{{QStringLiteral("bi4"),
             QJsonObject{{QStringLiteral("pl"), 4}}}}}}}}}) {
    const QJsonObject before = controller.packet();
    QJsonObject value = badShop;
    value.insert(QStringLiteral("_cmd"), catalog.getInfoCommand());
    ok &= require(controller.requestInfo(), "bad-shop batch did not start");
    sendControllerPacket(value);
    sendControllerPacket(materials(material(17)));
    sendControllerPacket(leagueReply());
    ok &= require(controller.packet() == before && !controller.hasPacket(),
                  "invalid or conflicting shop counter overwrote accepted history");
  }
  ok &= require(controller.requestInfo(), "rejected-shop batch did not start");
  sendControllerPacket({{QStringLiteral("_cmd"), catalog.getInfoCommand()},
                        {QStringLiteral("r"), false}});
  sendControllerPacket(materials(material(QStringLiteral("9223372036854775807"))));
  sendControllerPacket(leagueReply());
  ok &= require(!controller.isRunning() &&
                    controller.materialCounts().value(QStringLiteral("4:3237")) ==
                        std::numeric_limits<qint64>::max(),
                "rejected shop cancelled material sibling or exact integer count was truncated");

  const QString cachePath = QDir(QFileInfo(repository.cachePath()).absolutePath())
                                .filePath(QStringLiteral("shops.json"));
  ok &= require(waitUntil([&] { return controller.pendingStorageCount() == 0; }),
                "shop asynchronous persistence did not complete");
  QFile cache(cachePath);
  ok &= require(cache.open(QIODevice::ReadOnly), "shop cache was not persisted");
  const QByteArray saved = cache.readAll();
  cache.close();
  const auto countsBeforeWeak = controller.cachedMaterialCounts();
  controller.handlePacket(QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(materials(material(99))).toJson(QJsonDocument::Compact)));
  ok &= require(controller.cachedMaterialCounts() == countsBeforeWeak &&
                    !controller.unverifiedPackets().contains(QStringLiteral("3_11")),
                "unverified packet borrowed the active source and changed account facts");
  ok &= require(cache.open(QIODevice::ReadOnly) && cache.readAll() == saved,
                "unverified packet changed the persisted shop cache");
  cache.close();
  ShopExchangeController cachedController(&repository);
  ok &= require(waitUntil([&] { return !cachedController.cacheLoading(); }),
                "shop asynchronous cache read did not complete");
  ok &= require(!cachedController.hasPacket() && !cachedController.hasMaterialCounts() &&
                    cachedController.cachedMaterialCounts().value(QStringLiteral("4:3237")) ==
                        std::numeric_limits<qint64>::max() && cachedController.materialCounts().isEmpty(),
                "cache reload lost exact count or promoted historical data to current");
  {
    QSemaphore entered, release;
    const bool held = repository.storageService()->postAuxiliary([&](QObject*) {
      entered.release(); release.acquire();
    });
    ok &= require(held && entered.tryAcquire(1, 2000), "shop delayed-read fixture did not hold IO");
    ShopExchangeController lateReader(&repository);
    const QJsonObject freshLeague{{QStringLiteral("_cmd"), QStringLiteral("1015_2A")},
        {QStringLiteral("infos"), QJsonObject{{QStringLiteral("UnionMemberInfo"),
            QJsonObject{{QStringLiteral("lCToken"), 55}}}}}};
    lateReader.handleDecodedEnvelope(verifiedFixtureEnvelope(&repository, freshLeague), freshLeague);
    // Three independent cache reads (shop, cultivation and activity history)
    // plus the one fresh league write intent; no cache waits on network data.
    ok &= require(lateReader.cacheLoading() && lateReader.pendingStorageCount() <= 4 &&
                      lateReader.pendingWriteCount() == 1 &&
                      lateReader.cachedMaterialCounts().value(QStringLiteral("134:1")) == 55,
                  "shop network observation waited for disk or unbounded pending copies");
    release.release();
    ok &= require(waitUntil([&] { return lateReader.pendingStorageCount() == 0; }) &&
                      lateReader.materialCounts().value(QStringLiteral("134:1")) == 55 &&
                      lateReader.cachedMaterialCounts().value(QStringLiteral("4:3237")) ==
                          std::numeric_limits<qint64>::max() &&
                      !lateReader.materialCounts().contains(QStringLiteral("4:3237")),
                  "late shop disk image overwrote network data or historical material became current");
  }
  const auto beforeSpoof = controller.cachedMaterialCounts();
  QJsonObject spoofedLeague{{QStringLiteral("_cmd"), QStringLiteral("1015_2A")},
      {QStringLiteral("infos"), QJsonObject{{QStringLiteral("UnionMemberInfo"),
          QJsonObject{{QStringLiteral("lCToken"), 1}}}}}};
  InboundEnvelope wrongSource = verifiedFixtureEnvelope(&repository, spoofedLeague);
  wrongSource.source.account = QStringLiteral("another-account");
  controller.handleDecodedEnvelope(wrongSource, spoofedLeague);
  ok &= require(controller.cachedMaterialCounts() == beforeSpoof,
                "verified evidence belonging to another account was accepted");
  const QJsonObject weakBeforeMalformed = controller.unverifiedPackets();
  controller.handlePacket(QStringLiteral("recivedata"),
      QStringLiteral("prefix{\"_cmd\":\"3_11\",\"4\":[]}suffix"));
  ok &= require(controller.unverifiedPackets() == weakBeforeMalformed,
                "malformed protocol text was recovered by extracting a JSON substring");

  // Production read-only mode: complete only an issued request in this local
  // reading epoch, without promoting its observation into account facts.
  {
    QTemporaryDir weakRoot;
    StorageService weakStorage(weakRoot.path());
    PetRepository weakRepository(nullptr, &weakStorage);
    weakRepository.handlePacket(QStringLiteral("recivedata"),
        QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"shop-read-only\"}}"));
    ShopExchangeController weak(&weakRepository);
    QObject::connect(&weakRepository, &PetRepository::packetObserved, &weak,
        [&weak](const QJsonObject& packet, const InboundEnvelope& envelope) {
          weak.handleDecodedEnvelope(envelope, packet);
        });
    QList<OutboundIntent> requests;
    QString status;
    QObject::connect(&weak, &ShopExchangeController::statusChanged, &weak,
                     [&](const QString& value) { status = value; });
    weak.setAsyncSender([&](const OutboundIntent& intent) { requests.append(intent); return true; });
    ok &= require(weak.requestInfo() && requests.size() == 3, "weak shop request batch did not start");
    quint64 sequence = 1;
    const auto sendWeak = [&](const QJsonObject& value, quint64 epoch) {
      InboundEnvelope envelope;
      envelope.receiveSequence = ++sequence;
      envelope.receivedMonotonicMs = transportMonotonicMs();
      envelope.capturedSessionEpoch = epoch;
      envelope.method = QStringLiteral("recivedata");
      envelope.payload = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
      weakRepository.handleEnvelope(envelope);
    };
    sendWeak(completeShop, weakRepository.sessionGeneration());
    ok &= require(weak.unverifiedPackets().isEmpty() && weak.isRunning(),
                  "not-yet-dispatched weak response completed a request");
    for (const auto& request : requests) {
      request.permit.claim(transportMonotonicMs());
      request.permit.complete(SubmissionOutcome::Submitted);
    }
    sendWeak(completeShop, weakRepository.sessionGeneration() + 1);
    ok &= require(weak.unverifiedPackets().isEmpty(), "wrong-epoch weak shop reply was displayed");
    // A response can beat its queued receipt; the shared permit is sufficient.
    sendWeak(completeShop, 0); // actual host capture has no verified epoch
    sendWeak(withUnrelatedInventories(materials(material(99))), 0);
    sendWeak(leagueReply(77), 0);
    const QString weakRefreshStatus = status;
    ok &= require(!weak.isRunning() && weak.unverifiedPackets().contains(catalog.getInfoCommand()) &&
                      weak.unverifiedPackets().value(QStringLiteral("3_11")).toObject()
                          .value(QStringLiteral("4")).toObject().value(QStringLiteral("3237")).toString() == QStringLiteral("99") &&
                      weak.unverifiedPackets().value(QStringLiteral("1015_2A")).toObject()
                          .value(QStringLiteral("134")).toObject().value(QStringLiteral("1")).toString() == QStringLiteral("77") &&
                      weak.packet().isEmpty() && weak.cachedMaterialCounts().isEmpty() &&
                      waitUntil([&] { return weak.pendingWriteCount() == 0; }) &&
                      !QFile::exists(QDir(weakRepository.storageContext()->directory()).filePath(QStringLiteral("cultivation-materials.json"))) &&
                      weak.cultivationMaterialInventory().counts.isEmpty() &&
                      !QFile::exists(QDir(weakRepository.storageContext()->directory()).filePath(QStringLiteral("shops.json"))) &&
                      !weak.hasMaterialCounts() && !weakRepository.sessionContext().canPersist() &&
                      weakRefreshStatus.contains(QStringLiteral("只读")) && !weakRefreshStatus.contains(QStringLiteral("超时")) &&
                      !weakRefreshStatus.contains(QStringLiteral("材料类型")),
                  "valid weak shop replies disappeared, changed the manual cultivation cache, or gained authoritative shop facts");
    const QJsonObject observations = weak.unverifiedPackets();
    requests.clear();
    ok &= require(weak.requestInfo(), "malformed weak shop batch did not start");
    for (const auto& request : requests) {
      request.permit.claim(transportMonotonicMs());
      request.permit.complete(SubmissionOutcome::Submitted);
    }
    sendWeak({{QStringLiteral("_cmd"), catalog.getInfoCommand()},
              {QStringLiteral("si1"), QStringLiteral("bad")}}, weakRepository.sessionGeneration());
    sendWeak({{QStringLiteral("_cmd"), QStringLiteral("3_11")},
              {QStringLiteral("4"), QStringLiteral("bad")}}, weakRepository.sessionGeneration());
    sendWeak(leagueReply(QJsonValue(QJsonValue::Null)), weakRepository.sessionGeneration());
    ok &= require(!weak.isRunning() && weak.unverifiedPackets() == observations &&
                      status.contains(QStringLiteral("无效")) && !status.contains(QStringLiteral("超时")),
                  "malformed weak shop reply became data or was mislabeled as a timeout");
    requests.clear();
    ok &= require(weak.requestInfo(), "missing weak shop reply batch did not start");
    for (const auto& request : requests) {
      weak.handleSendReceipt({request.ticket.taskId, request.ticket.account, request.ticket.sessionEpoch,
          request.ticket.command, SubmissionOutcome::Submitted, transportMonotonicMs() - 10001});
    }
    ok &= require(!weak.isRunning() && status.contains(QStringLiteral("超时")),
                  "a genuinely missing shop response stopped reporting its deadline");
  }

  // Optional integration check against the user's current official unpack.
  // The ordinary CTest run stays hermetic when the variable is not set.
  const QByteArray officialRoot = qgetenv("KQPET_TEST_OFFICIAL_UNPACK_ROOT");
  if (!officialRoot.isEmpty()) {
    QTemporaryDir officialDataRoot;
    qputenv("KQPET_OFFICIAL_UNPACK_ROOT", officialRoot);
    QString officialError;
    StorageService officialStorage(officialDataRoot.path());
    CatalogIoService officialIo(&officialStorage);
    ok &= require(runCatalogRequest(&officialIo, CatalogKind::Shop, CatalogRequestMode::OfficialUpdate, &officialError),
                  "current official unpack integration parse failed");
    const auto officialShops = ShopExchangeCatalog::instance().protocolShops(date);
    int officialGoodCount = 0;
    for (const ShopExchangeShop& shop : officialShops)
      officialGoodCount += shop.goods.size();
    ok &= require(officialShops.size() == 6 && officialGoodCount >= 10,
                  "current official unpack yielded too few shop goods");
  }

  // A renamed item and a changed pet allow-list must be learned from the
  // official config without relying on the old display name.
  QTemporaryDir unpackRoot;
  QTemporaryDir dataRoot;
  const QString module = unpackRoot.filePath(QStringLiteral("storeexchangeframework_new"));
  QDir().mkpath(module);
  QFile config(QDir(module).filePath(QStringLiteral("SEFConfig.as")));
  ok &= require(config.open(QIODevice::WriteOnly | QIODevice::Text),
                "failed to create dynamic official config fixture");
  QString source;
  for (int shopId = 1; shopId <= 6; ++shopId) {
    source += QStringLiteral(
        "public static const SERVER_ID_%1_REWARD_CONFIG:Array = [{\"id\":1,"
        "\"serverId\":%1,\"tab\":1,\"basicDescription\":\"全新名称%1\","
        "\"shelfTime\":\"20260101\",\"removalTime\":\"21000101\","
        "\"limit\":\"3:2\",\"cost\":\"4:3237:10\","
        "\"simpleParams\":\"CommonEnhancePrize,x,x,62,900%1#901%1\","
        "\"filterKey\":\"改名\",\"unlock\":\"\",\"tag\":\"\"}];\n")
                  .arg(shopId);
  }
  config.write(source.toUtf8());
  config.close();
  qputenv("KQPET_OFFICIAL_UNPACK_ROOT", unpackRoot.path().toUtf8());
  QString updateError;
  StorageService catalogStorage(dataRoot.path());
  CatalogIoService catalogIo(&catalogStorage);
  ok &= require(runCatalogRequest(&catalogIo, CatalogKind::Shop, CatalogRequestMode::OfficialUpdate, &updateError),
                "dynamic official config update failed");
  const QList<ShopExchangeShop> updated =
      ShopExchangeCatalog::instance().protocolShops(QDate(2026, 8, 21));
  ok &= require(updated.size() == 6 && updated.at(0).goods.size() == 1 &&
                    updated.at(0).goods.at(0).description == QStringLiteral("全新名称1") &&
                    updated.at(0).goods.at(0).raceIds.contains(9001),
                "renamed item or changed race list was not learned dynamically");
  ok &= require(QFile::exists(QDir(dataRoot.path()).filePath(
                    QStringLiteral("catalog/shop-exchange-data.json"))),
                "dynamic catalog was not cached atomically");
  qunsetenv("KQPET_OFFICIAL_UNPACK_ROOT");
  if (!ok)
    return 1;
  std::fprintf(stdout, "PASS: shop exchange catalog\n");
  return 0;
}
