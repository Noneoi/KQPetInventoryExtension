#include "prepared_shop_conditions.h"
#include "shop_limit_facts.h"
#include "activity_shop_observation.h"

#include <limits>

namespace {

ShopCondition condition(ShopConditionState state, const QString& reason,
                        const QString& source = {}, const QDateTime& time = {},
                        ShopConditionFreshness freshness =
                            ShopConditionFreshness::Unknown) {
  return {state, reason, source, time.toUTC(), freshness};
}

// Exact floor(owned / required * 1,000,000), without overflowing multiplication
// or letting floating-point rounding change ties near a quantization boundary.
int coverageMillionths(qint64 owned, qint64 required) {
  constexpr qint64 scale = 1000000;
  if (owned >= required) return static_cast<int>(scale);
  int low = 0;
  int high = static_cast<int>(scale);
  while (low < high) {
    const int middle = low + (high - low + 1) / 2;
    const qint64 threshold = (required / scale) * middle +
        ((required % scale) * middle + scale - 1) / scale;
    if (owned >= threshold) low = middle;
    else high = middle - 1;
  }
  return low;
}

int resourceSupportedExchanges(const QList<ResourceRequirement>& requirements) {
  qint64 result = std::numeric_limits<int>::max();
  for (const ResourceRequirement& requirement : requirements) {
    if (!requirement.ownedKnown || requirement.required <= 0) return 0;
    result = qMin(result, requirement.owned / requirement.required);
  }
  return static_cast<int>(result);
}


PreparedShopGoodConditions prepareGood(const CompiledShopGood& compiled,
    const QJsonObject& shopPacket, const AccountResourceView& resources,
    const ShopConditionContext& context, AlgorithmPipelineStats* stats) {
  PreparedShopGoodConditions prepared;
  ShopActionability& result = prepared.account;
  const ShopExchangeGood& good = compiled.good;
  result.requirements = compiled.requirements;
  result.costCondition = compiled.costCondition;
  QHash<QString,ActivityShopCost> activityCosts;
  if (!good.sourceKey.isEmpty()) {
    const auto observed = observeActivityShopGood(good,shopPacket);
    if (!good.priceOptions.isEmpty() && observed.priceKnown) {
      auto priced = good; priced.cost = observed.standardCost;
      const auto catalog = CompiledShopCatalog::compile({priced});
      result.requirements = catalog.goods().first().requirements;
      result.costCondition = catalog.goods().first().costCondition;
      if (!observed.priceCurrent) result.costCondition = condition(ShopConditionState::Unknown,QStringLiteral("价格是上次读取的，请刷新确认"));
    }
    if (!good.activityCosts.isEmpty() && observed.costs.size() == good.activityCosts.size()) {
      if (good.cost.isEmpty() && good.priceOptions.isEmpty()) result.costCondition = condition(ShopConditionState::Satisfied,QStringLiteral("官方活动货币费用"));
      for (const auto& cost : observed.costs) {
        result.requirements.append({cost.key,cost.name,cost.required}); activityCosts.insert(cost.key,cost);
      }
    }
  }
  const bool costKnown = result.costCondition.state == ShopConditionState::Satisfied;
  bool allKnown = costKnown;
  bool enough = costKnown;
  bool blocked = false;
  for (ResourceRequirement& requirement : result.requirements) {
    if (stats) ++stats->resourceBalancesChecked;
    const auto activity = activityCosts.constFind(requirement.resourceKey);
    requirement.ownedKnown = activity == activityCosts.cend() ? resources.hasReliableCount(requirement.resourceKey) : activity->current;
    if (requirement.ownedKnown)
      requirement.owned = activity == activityCosts.cend() ? resources.count(requirement.resourceKey) : activity->owned;
    const ShopConditionState state =
        !requirement.ownedKnown ? ShopConditionState::Unknown
        : requirement.owned < requirement.required ? ShopConditionState::Blocked
                                                   : ShopConditionState::Satisfied;
    requirement.condition = condition(
        state, state == ShopConditionState::Unknown ? QStringLiteral("余额未知或已失效")
               : state == ShopConditionState::Blocked ? QStringLiteral("余额不足")
                                                      : QStringLiteral("余额足够"),
        resources.source(), resources.observedAt(),
        resources.invalidated() ? ShopConditionFreshness::Invalidated
                                : ShopConditionFreshness::Unknown);
    allKnown &= requirement.ownedKnown;
    enough &= state == ShopConditionState::Satisfied;
    blocked |= state == ShopConditionState::Blocked;
  }
  result.resourcesKnown = allKnown;
  result.resourcesEnough = enough;
  result.resourceCondition = condition(
      !allKnown ? ShopConditionState::Unknown
                : blocked ? ShopConditionState::Blocked
                          : ShopConditionState::Satisfied,
      !allKnown ? QStringLiteral("成本或部分余额待确认")
                : blocked ? QStringLiteral("已知资源不足")
                          : QStringLiteral("已知资源满足成本"),
      resources.source(), resources.observedAt(),
      resources.invalidated() && !result.requirements.isEmpty()
          ? ShopConditionFreshness::Invalidated : ShopConditionFreshness::Unknown);

  result.remainingCount = good.provenUnlimited
      ? std::numeric_limits<int>::max()
      : context.shopPacketKnown
            ? shopRemainingCount(shopPacket, good) : -1;
  const ShopConditionState limitState =
      result.remainingCount < 0 ? ShopConditionState::Unknown
      : result.remainingCount == 0 ? ShopConditionState::Blocked
                                   : ShopConditionState::Satisfied;
  result.limitCondition = condition(
      limitState, limitState == ShopConditionState::Unknown
                      ? QStringLiteral("限次字段缺失或无效，不能按零使用次数处理")
                  : limitState == ShopConditionState::Blocked
                      ? QStringLiteral("兑换次数已耗尽")
                      : good.provenUnlimited ? QStringLiteral("目录已确证不限次")
                                             : QStringLiteral("兑换次数尚有剩余"),
      good.provenUnlimited ? QStringLiteral("商品目录") : context.shopSource,
      good.provenUnlimited ? QDateTime{} : context.shopObservedAt,
      good.provenUnlimited ? ShopConditionFreshness::Unknown : context.shopFreshness);
  if (!good.provenUnlimited) {
    const auto validity = context.quotaValidity.constFind(shopQuotaValidityKey(good));
    const bool current = validity != context.quotaValidity.cend() &&
        validity->state == ShopConditionState::Satisfied &&
        validity->freshness == ShopConditionFreshness::Current &&
        !validity->source.isEmpty() && validity->observedAt.isValid();
    if (!current) {
      result.limitCondition.state = ShopConditionState::Unknown;
      result.limitCondition.reason = validity != context.quotaValidity.cend() && !validity->reason.isEmpty()
          ? validity->reason : QStringLiteral("次数周期未确认；数值是上次读取的结果，可能已变化");
      result.limitCondition.freshness = validity == context.quotaValidity.cend()
          ? ShopConditionFreshness::Unknown : validity->freshness;
      result.remainingCount = -1;
    } else if (context.shopFreshness != ShopConditionFreshness::Invalidated) {
      result.limitCondition.freshness = ShopConditionFreshness::Current;
      result.limitCondition.source = validity->source;
      result.limitCondition.observedAt = validity->observedAt;
    }
  }
  if (result.limitCondition.effectiveState() == ShopConditionState::Unknown)
    result.remainingCount = -1;
  if (!good.sourceKey.isEmpty()) {
    const auto observed = observeActivityShopGood(good,shopPacket);
    result.remainingCount = -1;
    result.limitCondition = condition(ShopConditionState::Unknown,
        observed.quotaKnown ? QStringLiteral("上次读取剩余 %1 / %2；活动周期未确认").arg(observed.remaining).arg(good.limitCount)
                            : QStringLiteral("活动兑换次数尚未查询或未有效返回"),good.shopName,observed.observedAt);
  }

  if (good.unlock.isEmpty()) {
    result.unlockCondition = condition(ShopConditionState::Satisfied,
                                      QStringLiteral("目录没有解锁限制"),
                                      QStringLiteral("商品目录"));
  } else {
    const auto fact = context.verifiedUnlockFacts.constFind(good.unlock);
    if (fact != context.verifiedUnlockFacts.cend() &&
        !fact->source.isEmpty() && fact->observedAt.isValid()) {
      result.unlockCondition = *fact;
      result.unlockCondition.observedAt = fact->observedAt.toUTC();
    } else {
      result.unlockCondition = condition(
          ShopConditionState::Unknown,
          QStringLiteral("解锁条件缺少已验证语义或可信事实：%1").arg(good.unlock),
          QStringLiteral("商品目录"));
    }
  }

  if (!good.sourceKey.isEmpty() && !good.observationWhen.isEmpty()) {
    const auto observed = observeActivityShopGood(good,shopPacket);
    if (!observed.applicabilityKnown) result.unlockCondition = condition(ShopConditionState::Unknown,QStringLiteral("活动等级尚未读取"),good.shopName);
    else if (!observed.applicable) result.unlockCondition = condition(ShopConditionState::Blocked,QStringLiteral("不适用于当前活动等级"),good.shopName);
  }

  // The unset pet condition is not part of account preparation.
  prepared.unknownConditionCount = result.unknownConditionCount() - 1;
  prepared.excluded = result.limitCondition.effectiveState() == ShopConditionState::Blocked ||
                      result.unlockCondition.effectiveState() == ShopConditionState::Blocked;
  if (result.resourcesKnown) {
    prepared.resourceCoverageMillionths = 1000000;
    for (const ResourceRequirement& requirement : result.requirements)
      prepared.resourceCoverageMillionths = qMin(prepared.resourceCoverageMillionths,
          coverageMillionths(requirement.owned, requirement.required));
    prepared.resourceSupportedExchanges = resourceSupportedExchanges(result.requirements);
  }
  return prepared;
}

}  // namespace

PreparedShopConditions PreparedShopConditions::prepare(const CompiledShopCatalog& catalog,
    const QJsonObject& shopPacket, const AccountResourceView& resources,
    const ShopConditionContext& context, AlgorithmPipelineStats* stats) {
  PreparedShopConditions result;
  result.begin(catalog, context);
  while (result.appendNext(shopPacket, resources, stats)) {}
  return result;
}

void PreparedShopConditions::begin(const CompiledShopCatalog& catalog,
                                  const ShopConditionContext& context) {
  catalog_ = catalog;
  context_ = context;
  goods_.clear();
  goods_.reserve(catalog.goods().size());
}

bool PreparedShopConditions::appendNext(const QJsonObject& shopPacket,
    const AccountResourceView& resources, AlgorithmPipelineStats* stats) {
  const qsizetype index = goods_.size();
  if (index >= catalog_.goods().size()) return false;
  goods_.append(prepareGood(catalog_.goods().at(index), shopPacket, resources, context_, stats));
  if (stats) ++stats->accountConditionsPrepared;
  return true;
}

ShopActionability describePreparedShopActionability(
    const CompiledShopGood& compiled, const PreparedShopGoodConditions& conditions,
    const ShopPetDerived& pet, const ShopConditionContext& context) {
  ShopActionability result = conditions.account;
  const ShopPetEligibility eligibility = describeShopPetRule(compiled.petRule, pet);
  const bool raceAllowed = (pet.raceId > 0 && compiled.raceIds.contains(pet.raceId)) ||
      (pet.metadataRaceId > 0 && compiled.raceIds.contains(pet.metadataRaceId));
  result.eligibilityReason = eligibility.reason;
  result.applicableCodes = eligibility.usefulCodes;
  ShopConditionState state = ShopConditionState::Unknown;
  if (!raceAllowed) {
    state = pet.raceId > 0 ? ShopConditionState::Blocked : ShopConditionState::Unknown;
    result.eligibilityReason = QStringLiteral("该真实项目未确认适用于此精灵种族");
    result.applicableCodes.clear();
  } else if (eligibility.state == ShopPetEligibilityState::Usable) {
    state = ShopConditionState::Satisfied;
  } else if (eligibility.state == ShopPetEligibilityState::NotUsable) {
    state = ShopConditionState::Blocked;
  }
  result.eligibilityCondition = condition(state, result.eligibilityReason,
      context.petSource, context.petObservedAt, context.petFreshness);
  result.petEligible = result.eligibilityCondition.effectiveState() == ShopConditionState::Satisfied;
  return result;
}
