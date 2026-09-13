#pragma once

#include "shop_actionability.h"
#include "shop_pet_eligibility.h"
#include <QSet>

// Work counters, not timing guesses. They accumulate across explicitly invoked
// stages so a caller can distinguish compilation from cached analysis runs.
struct AlgorithmPipelineStats {
  qint64 goodsCompiled = 0;
  qint64 costExpressionsParsed = 0;
  qint64 costFragmentsParsed = 0;
  qint64 enhancementExpressionsParsed = 0;
  qint64 enhancementComponentsCompiled = 0;
  qint64 raceAssociationsIndexed = 0;
  qint64 accountConditionsPrepared = 0;
  qint64 resourceBalancesChecked = 0;
  qint64 petsDerived = 0;
  qint64 cultivationComponentsDerived = 0;
  qint64 candidatePairsVisited = 0;
  qint64 candidateComponentsChecked = 0;
  qint64 finalRowsMaterialized = 0;
  qint64 finalSortComparisons = 0;
  qint64 candidateComparisons = 0;
  qint64 maximumLiveCandidates = 0;
  qint64 preparedFactsReused = 0;
  qint64 rawPowerCalculations = 0;
  qint64 goodsReused = 0;
  qint64 compiledCatalogCacheHits = 0;
};

struct CompiledShopGood {
  ShopExchangeGood good;
  QString stableKey;
  QList<ResourceRequirement> requirements;
  ShopCondition costCondition;
  CompiledShopPetRule petRule;
  QSet<int> raceIds;
};

// Immutable value snapshot after construction. No repository, filesystem,
// singleton catalog, clock or UI dependencies are consulted by these methods.
class CompiledShopCatalog final {
public:
  static CompiledShopCatalog compile(const QList<ShopExchangeGood>& goods,
      const QHash<QString, QString>& materialNames = {},
      AlgorithmPipelineStats* stats = nullptr);
  // Builder-only mutation; publish/copy the completed value after construction.
  void appendGood(const ShopExchangeGood& good,
      const QHash<QString, QString>& materialNames = {},
      AlgorithmPipelineStats* stats = nullptr);
  CompiledShopCatalog withMaterialNames(const QHash<QString, QString>& names) const;

  const QList<CompiledShopGood>& goods() const { return goods_; }
  const QList<qsizetype>& goodsForRace(int raceId) const;
  const QList<qsizetype>& allGoodIndexes() const { return allGoodIndexes_; }
  const QHash<int, QList<qsizetype>>& raceIndex() const { return byRace_; }
  // Conservative logical retention including every race index/list capacity.
  // Updated incrementally by the builder; this getter performs no traversal.
  quint64 retainedBytes() const;

private:
  QList<CompiledShopGood> goods_;
  QHash<int, QList<qsizetype>> byRace_;
  QList<qsizetype> allGoodIndexes_;
  quint64 goodPayloadBytes_ = 0;
  quint64 raceIndexPayloadBytes_ = 0;
};
