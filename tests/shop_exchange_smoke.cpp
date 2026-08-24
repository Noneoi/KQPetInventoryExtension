#include "shop_exchange_catalog.h"
#include "shop_exchange_controller.h"
#include "shop_pet_eligibility.h"
#include "pet_repository.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
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
  const QDate date(2026, 8, 21);
  const QList<ShopExchangeShop> shops = catalog.shops(date);
  ok &= require(shops.size() == 6, "expected six shops");

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
                         {QStringLiteral("sgs"),
                          QStringLiteral("29:6:29#44:6:44")}};
  const ShopPetEligibility canUse =
      analyzeShopPetEligibility(redStar, improvable, true);
  ok &= require(canUse.state == ShopPetEligibilityState::Usable,
                "a pet with a non-red stargod should be green/usable");
  const ShopPetEligibility missingDetail =
      analyzeShopPetEligibility(redStar, improvable, false);
  ok &= require(missingDetail.state == ShopPetEligibilityState::Unknown,
                "missing local detail must never be marked usable");

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
  repository.handlePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"shop-test\"}}"));
  ShopExchangeController controller(&repository);
  struct SentRequest { QString service; QString command; QString params; };
  QList<SentRequest> requests;
  controller.setSender([&requests](const QString& service, const QString& command,
                                   const QString& params) {
    requests.append({service, command, params});
    return true;
  });
  ok &= require(controller.requestInfo(), "manual shop refresh did not start");
  ok &= require(requests.size() == 2 &&
                    requests.at(1).service == QStringLiteral("MaterialExtension") &&
                    requests.at(1).command == QStringLiteral("3_11"),
                "manual shop refresh must request material counts exactly once");
  const auto sendControllerPacket = [&controller](const QJsonObject& value) {
    controller.handlePacket(
        QStringLiteral("recivedata"),
        QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
  };
  sendControllerPacket({{QStringLiteral("_cmd"), QStringLiteral("3_11")},
                        {QStringLiteral("r"), 1},
                        {QStringLiteral("4"),
                         QJsonArray{QJsonObject{{QStringLiteral("i"), 3237},
                                                {QStringLiteral("n"), 456}}}}});
  sendControllerPacket({{QStringLiteral("_cmd"), catalog.getInfoCommand()},
                        {QStringLiteral("r"), 1},
                        {QStringLiteral("si1"), QJsonObject{}}});
  ok &= require(controller.hasMaterialCounts() &&
                    controller.materialCounts().value(QStringLiteral("4:3237")) == 456 &&
                    !controller.materialCounts().contains(QStringLiteral("134:1")),
                "manual material response was not cached by material identity");
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

  // Optional integration check against the user's current official unpack.
  // The ordinary CTest run stays hermetic when the variable is not set.
  const QByteArray officialRoot = qgetenv("KQPET_TEST_OFFICIAL_UNPACK_ROOT");
  if (!officialRoot.isEmpty()) {
    QTemporaryDir officialDataRoot;
    qputenv("KQPET_OFFICIAL_UNPACK_ROOT", officialRoot);
    QString officialError;
    ok &= require(ShopExchangeCatalog::instance().updateFromOfficialData(
                      officialDataRoot.path(), &officialError),
                  "current official unpack integration parse failed");
    const auto officialShops = ShopExchangeCatalog::instance().shops(date);
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
  ok &= require(ShopExchangeCatalog::instance().updateFromOfficialData(
                    dataRoot.path(), &updateError),
                "dynamic official config update failed");
  const QList<ShopExchangeShop> updated =
      ShopExchangeCatalog::instance().shops(QDate(2026, 8, 21));
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
