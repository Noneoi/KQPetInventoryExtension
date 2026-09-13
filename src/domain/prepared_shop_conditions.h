#pragma once

#include "compiled_shop_catalog.h"

struct PreparedShopGoodConditions {
  ShopActionability account;
  int unknownConditionCount = 0;
  int resourceCoverageMillionths = 0;
  int resourceSupportedExchanges = 0;
  bool excluded = false;
};

class PreparedShopConditions final {
public:
  static PreparedShopConditions prepare(const CompiledShopCatalog& catalog,
      const QJsonObject& shopPacket, const AccountResourceView& resources,
      const ShopConditionContext& context = {},
      AlgorithmPipelineStats* stats = nullptr);
  // Freeze a completed compiled catalog, then prepare at most one item per
  // call. Consumers must wait until appendNext() returns false before use.
  void begin(const CompiledShopCatalog& catalog, const ShopConditionContext& context = {});
  bool appendNext(const QJsonObject& shopPacket, const AccountResourceView& resources,
                  AlgorithmPipelineStats* stats = nullptr);

  const CompiledShopCatalog& catalog() const { return catalog_; }
  const QList<PreparedShopGoodConditions>& goods() const { return goods_; }
  const ShopConditionContext& context() const { return context_; }

private:
  CompiledShopCatalog catalog_;
  QList<PreparedShopGoodConditions> goods_;
  ShopConditionContext context_;
};

ShopActionability describePreparedShopActionability(
    const CompiledShopGood& good, const PreparedShopGoodConditions& conditions,
    const ShopPetDerived& pet, const ShopConditionContext& context);
