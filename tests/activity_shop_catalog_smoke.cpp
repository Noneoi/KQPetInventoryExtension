#include "catalog_test_support.h"
#include "shop_exchange_catalog.h"
#include "compiled_shop_catalog.h"
#include "prepared_shop_conditions.h"
#include "shop_limit_facts.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  bool ok = true;
  const auto require = [&ok](bool value,const char* text) { if (!value) { ok=false; std::fprintf(stderr,"FAIL: %s\n",text); } };
  QFile file(QStringLiteral(":/kqpet/shop-exchange-data.json"));
  require(file.open(QIODevice::ReadOnly),"embedded catalog unavailable");
  auto root = QJsonDocument::fromJson(file.readAll()).object();
  QJsonArray mainShops;
  for (const auto& value : root.value(QStringLiteral("shops")).toArray())
    if (value.toObject().value(QStringLiteral("sourceKey")).toString().isEmpty()) mainShops.append(value);
  require(!mainShops.isEmpty(),"base shops missing");
  const auto base = mainShops.first().toObject();
  const auto original = base.value(QStringLiteral("goods")).toArray().first().toObject();
  auto sameId = original;
  sameId.insert(QStringLiteral("shelfTime"),QString{});
  sameId.insert(QStringLiteral("removalTime"),QString{});
  auto zeroId = sameId; zeroId.insert(QStringLiteral("itemServerId"),0);
  QJsonObject activity{{QStringLiteral("shopId"),base.value(QStringLiteral("shopId"))},
      {QStringLiteral("sourceKey"),QStringLiteral("newactivityext/newact20990101/example/example#Config.TABLE")},
      {QStringLiteral("name"),QStringLiteral("活动·新活动")},
      {QStringLiteral("goods"),QJsonArray{sameId,zeroId}}};
  auto shops = mainShops; shops.append(activity); root.insert(QStringLiteral("shops"),shops);
  QString error;
  const auto catalog = ShopExchangeCatalog::prepare(root,QStringLiteral("fixture"),{},&error);
  require(bool(catalog),"activity namespace or item zero failed catalog parsing");
  if (catalog) {
    const auto main = catalog->allShops.first().goods.first();
    const auto added = catalog->allShops.last();
    const auto collision = added.goods.first();
    const auto zero = added.goods.last();
    require(collision.shopId == main.shopId && collision.itemServerId == main.itemServerId &&
            collision.stableKey() != main.stableKey(),"activity identity collided with SEF");
    require(zero.hasIdentity() && zero.itemServerId == 0 && zero.isOnlineOn(QDate(2026,9,13)),
            "activity-local zero ID or unspecified start date hid a real entry");
    const QJsonObject packet{{QStringLiteral("si%1").arg(main.shopId),QJsonObject{
        {collision.itemKey(),QJsonObject{{collision.limitKey,0}}},{zero.itemKey(),QJsonObject{{zero.limitKey,0}}}}}};
    require(shopUsedCount(packet,collision) < 0 && shopRemainingCount(packet,zero) < 0,
            "activity borrowed a base shop quota with matching identifiers");
    ShopConditionContext context;
    context.quotaValidity.insert(shopQuotaValidityKey(main.shopId,collision.limitKey),
        {ShopConditionState::Satisfied,QStringLiteral("main only"),QStringLiteral("fixture"),QDateTime::currentDateTimeUtc(),ShopConditionFreshness::Current});
    const auto compiled = CompiledShopCatalog::compile({collision,zero});
    const auto prepared = PreparedShopConditions::prepare(compiled,packet,AccountResourceView{},context);
    require(prepared.goods().size() == 2 && prepared.goods().first().account.remainingCount < 0 &&
        prepared.goods().first().account.limitCondition.state == ShopConditionState::Unknown,
        "main quota evidence was reused for an independent activity");
  }
  QTemporaryDir temporary;
  StorageService storage(temporary.path());
  CatalogIoService service(&storage);
  require(writeCatalogFixture(QDir(temporary.path()).filePath(QStringLiteral("catalog/shop-exchange-data.json")),
      QJsonDocument(root).toJson()),"fixture write failed");
  require(runCatalogRequest(&service,CatalogKind::Shop,CatalogRequestMode::Reload),"combined disk catalog did not load");
  require(ShopExchangeCatalog::instance().protocolShops().size() == mainShops.size() &&
          ShopExchangeCatalog::instance().shops().size() == shops.size(),"activity shops polluted the original request/cache group list");
  service.close(); require(storage.shutdown(),"test storage did not close");
  return ok ? 0 : 1;
}
