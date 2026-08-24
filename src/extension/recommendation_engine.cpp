#include "recommendation_engine.h"

#include "shop_actionability.h"

#include <algorithm>
#include <limits>

namespace {

int typeRank(RecommendationType type) {
  switch (type) {
    case RecommendationType::ReadyNow: return 0;
    case RecommendationType::ResourceMissing: return 1;
    case RecommendationType::ResourceUnknown: return 2;
    case RecommendationType::NearFullCultivation: return 3;
  }
  return 4;
}

QString typeTitle(RecommendationType type) {
  switch (type) {
    case RecommendationType::ReadyNow:
      return QStringLiteral("当前可以处理");
    case RecommendationType::ResourceMissing:
      return QStringLiteral("还缺资源");
    case RecommendationType::ResourceUnknown:
      return QStringLiteral("资源状态未知");
    case RecommendationType::NearFullCultivation:
      return QStringLiteral("接近满培养");
  }
  return {};
}

qint64 totalRequired(const QList<ResourceRequirement>& requirements) {
  qint64 result = 0;
  for (const ResourceRequirement& requirement : requirements)
    result += requirement.required;
  return result;
}

qint64 totalMissing(const QList<ResourceRequirement>& requirements) {
  qint64 result = 0;
  for (const ResourceRequirement& requirement : requirements)
    result += requirement.missing();
  return result;
}

int knownGapUnits(const PetAssetRecord& pet, const QString& code) {
  if (code == QStringLiteral("34")) return pet.missingRedStars;
  return 0;
}

int resourceSupportedExchanges(
    const QList<ResourceRequirement>& requirements) {
  if (requirements.isEmpty()) return std::numeric_limits<int>::max();
  qint64 result = std::numeric_limits<int>::max();
  for (const ResourceRequirement& requirement : requirements) {
    if (!requirement.ownedKnown || requirement.required <= 0) return 0;
    result = qMin(result, requirement.owned / requirement.required);
  }
  return static_cast<int>(qMin<qint64>(result,
                                      std::numeric_limits<int>::max()));
}

struct Candidate {
  ActionRecommendation recommendation;
  qint64 requiredTotal = 0;
};

bool betterCandidate(const Candidate& left, const Candidate& right) {
  const int leftRank = typeRank(left.recommendation.type);
  const int rightRank = typeRank(right.recommendation.type);
  if (leftRank != rightRank) return leftRank < rightRank;
  if (left.recommendation.supportedGapCount !=
      right.recommendation.supportedGapCount)
    return left.recommendation.supportedGapCount >
           right.recommendation.supportedGapCount;
  if (left.recommendation.type == RecommendationType::ResourceMissing &&
      left.recommendation.totalMissingResources !=
          right.recommendation.totalMissingResources)
    return left.recommendation.totalMissingResources <
           right.recommendation.totalMissingResources;
  if (left.requiredTotal != right.requiredTotal)
    return left.requiredTotal < right.requiredTotal;
  return left.recommendation.shopGoodKey <
         right.recommendation.shopGoodKey;
}

bool recommendationLess(const ActionRecommendation& left,
                        const ActionRecommendation& right) {
  const int leftRank = typeRank(left.type);
  const int rightRank = typeRank(right.type);
  if (leftRank != rightRank) return leftRank < rightRank;
  if (left.type == RecommendationType::ReadyNow) {
    if (left.actionableCountKnown != right.actionableCountKnown)
      return left.actionableCountKnown;
    if (left.actionableCountKnown && left.remainingGapAfterAction !=
                                         right.remainingGapAfterAction)
      return left.remainingGapAfterAction < right.remainingGapAfterAction;
    if (left.supportedGapCount != right.supportedGapCount)
      return left.supportedGapCount > right.supportedGapCount;
  }
  if (left.type == RecommendationType::ResourceMissing &&
      left.totalMissingResources != right.totalMissingResources)
    return left.totalMissingResources < right.totalMissingResources;
  if (left.completionPercent != right.completionPercent)
    return left.completionPercent > right.completionPercent;
  const int leftPowerGap = qMax(0, left.highestPower - left.currentPower);
  const int rightPowerGap = qMax(0, right.highestPower - right.currentPower);
  if (leftPowerGap != rightPowerGap) return leftPowerGap > rightPowerGap;
  return left.stableId < right.stableId;
}

}  // namespace

QList<ActionRecommendation> RecommendationEngine::generate(
    const QString& account, const AccountAssetOverview& overview,
    const QList<ShopExchangeGood>& realGoods,
    const QJsonObject& shopPacket, bool shopPacketKnown,
    const AccountResourceView& resources) {
  QList<ActionRecommendation> result;
  for (const PetAssetRecord& pet : overview.pets) {
    if (!pet.detailAvailable || pet.fullyCultivated || !pet.improvable)
      continue;

    QList<Candidate> candidates;
    if (shopPacketKnown) {
      for (const ShopExchangeGood& good : realGoods) {
        const ShopActionability actionability = evaluateShopActionability(
            good, pet.pet, shopPacket, resources);
        if (!actionability.petEligible ||
            actionability.remainingCount <= 0)
          continue;
        Candidate candidate;
        ActionRecommendation& recommendation = candidate.recommendation;
        recommendation.type = !actionability.resourcesKnown
                                  ? RecommendationType::ResourceUnknown
                                  : actionability.resourcesEnough
                                        ? RecommendationType::ReadyNow
                                        : RecommendationType::ResourceMissing;
        recommendation.stableId =
            QStringLiteral("shop:%1:%2:%3")
                .arg(account)
                .arg(pet.instanceId)
                .arg(good.stableKey());
        recommendation.petInstanceId = pet.instanceId;
        recommendation.petName = pet.name;
        recommendation.title = typeTitle(recommendation.type);
        recommendation.gaps = pet.gaps;
        recommendation.shopGoodKey = good.stableKey();
        recommendation.shopName = good.shopName;
        recommendation.goodName = good.description;
        recommendation.requirements = actionability.requirements;
        recommendation.remainingExchangeCount =
            actionability.remainingCount;
        recommendation.completionPercent = pet.completionPercent;
        recommendation.currentPower = pet.currentPower;
        recommendation.highestPower = pet.highestPower;
        recommendation.supportedGapCount =
            actionability.applicableCodes.size();
        recommendation.totalMissingResources =
            totalMissing(actionability.requirements);
        candidate.requiredTotal = totalRequired(actionability.requirements);

        if (recommendation.type == RecommendationType::ReadyNow &&
            good.provenGapUnitsPerExchange > 0 &&
            !good.provenGapCode.isEmpty()) {
          const int gapUnits = knownGapUnits(pet, good.provenGapCode);
          const int resourceExchanges =
              resourceSupportedExchanges(actionability.requirements);
          if (gapUnits > 0 && resourceExchanges > 0) {
            const int exchangesForGap =
                (gapUnits + good.provenGapUnitsPerExchange - 1) /
                good.provenGapUnitsPerExchange;
            recommendation.actionableCountKnown = true;
            recommendation.actionableCount =
                qMin(actionability.remainingCount,
                     qMin(resourceExchanges, exchangesForGap));
            recommendation.remainingGapAfterAction = qMax(
                0, gapUnits - recommendation.actionableCount *
                                  good.provenGapUnitsPerExchange);
          }
        }
        candidates.append(candidate);
      }
    }

    if (!candidates.isEmpty()) {
      std::sort(candidates.begin(), candidates.end(), betterCandidate);
      ActionRecommendation best = candidates.constFirst().recommendation;
      best.alternateGoodCount = candidates.size() - 1;
      result.append(best);
      continue;
    }
    if (pet.completionPercent >= 90) {
      ActionRecommendation nearFull;
      nearFull.stableId =
          QStringLiteral("near-full:%1:%2").arg(account).arg(pet.instanceId);
      nearFull.type = RecommendationType::NearFullCultivation;
      nearFull.petInstanceId = pet.instanceId;
      nearFull.petName = pet.name;
      nearFull.title = typeTitle(nearFull.type);
      nearFull.gaps = pet.gaps;
      nearFull.completionPercent = pet.completionPercent;
      nearFull.currentPower = pet.currentPower;
      nearFull.highestPower = pet.highestPower;
      result.append(nearFull);
    }
  }
  std::sort(result.begin(), result.end(), recommendationLess);
  return result;
}

QList<ActionRecommendation> RecommendationEngine::applyFreshness(
    const QList<ActionRecommendation>& recommendations,
    bool cultivationStale, bool shopStale) {
  QList<ActionRecommendation> result = recommendations;
  for (ActionRecommendation& recommendation : result) {
    recommendation.cultivationStale = cultivationStale;
    recommendation.shopStale =
        shopStale && !recommendation.shopGoodKey.isEmpty();
    recommendation.stale = recommendation.cultivationStale ||
                           recommendation.shopStale;
  }
  return result;
}
