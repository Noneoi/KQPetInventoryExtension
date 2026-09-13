#include "recommendation_engine.h"
#include "asset_derivation.h"
#include "pet_power_calculator.h"

#include "shop_actionability.h"

#include <QElapsedTimer>
#include <QBitArray>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

int typeRank(RecommendationType type) {
  switch (type) {
    case RecommendationType::ReadyNow: return 0;
    case RecommendationType::ResourceMissing: return 1;
    case RecommendationType::ConditionUnknown: return 2;
    case RecommendationType::NearFullCultivation: return 3;
    case RecommendationType::LocalCultivation: return 4;
  }
  return 4;
}

QString typeTitle(RecommendationType type) {
  switch (type) {
    case RecommendationType::ReadyNow: return QStringLiteral("按已知数据可处理");
    case RecommendationType::ResourceMissing: return QStringLiteral("还缺资源");
    case RecommendationType::ConditionUnknown: return QStringLiteral("条件待确认");
    case RecommendationType::NearFullCultivation: return QStringLiteral("接近满培养");
    case RecommendationType::LocalCultivation: return QStringLiteral("本地培养建议");
  }
  return {};
}

void fillPetRanking(ActionRecommendation* result, const PetAssetRecord& pet) {
  result->petInstanceId = pet.instanceId;
  result->completionPercent = pet.completionPercent;
  result->currentPower = pet.currentPower;
  result->highestPower = pet.highestPower;
  result->completionKnown = pet.completionKnown &&
      pet.completionPercent >= 0 && pet.completionPercent <= 100;
  result->powerGapKnown = pet.powerGapKnown &&
      pet.currentPower >= 0 && pet.highestPower >= 0;
}

void fillPresentation(ActionRecommendation* result, const QString& account,
                      const PetAssetRecord& pet) {
  result->stableId = result->shopGoodKey.isEmpty()
      ? QStringLiteral("%1:%2:%3").arg(result->type == RecommendationType::LocalCultivation
            ? QStringLiteral("local-cultivation") : QStringLiteral("near-full"), account).arg(pet.instanceId)
      : QStringLiteral("shop:%1:%2:%3").arg(account).arg(pet.instanceId)
            .arg(result->shopGoodKey);
  result->petName = pet.name;
  result->title = typeTitle(result->type);
  result->gaps = pet.gaps;
  result->memoryRetention = pet.memoryRetention;
}

}  // namespace

bool PreparedRecommendationEngine::less(const ActionRecommendation& left,
                                const ActionRecommendation& right) {
  const int leftRank = typeRank(left.type);
  const int rightRank = typeRank(right.type);
  if (leftRank != rightRank) return leftRank < rightRank;
  if (left.type == RecommendationType::ReadyNow) {
    if (left.closesKnownGap != right.closesKnownGap) return left.closesKnownGap;
    if (left.supportedGapCount != right.supportedGapCount)
      return left.supportedGapCount > right.supportedGapCount;
  } else if (left.type == RecommendationType::ResourceMissing) {
    if (left.resourceCoverageKnown != right.resourceCoverageKnown)
      return left.resourceCoverageKnown;
    if (left.resourceCoverageKnown &&
        left.resourceCoverageMillionths != right.resourceCoverageMillionths)
      return left.resourceCoverageMillionths > right.resourceCoverageMillionths;
  } else if (left.type == RecommendationType::ConditionUnknown) {
    if (left.unknownConditionCount != right.unknownConditionCount)
      return left.unknownConditionCount < right.unknownConditionCount;
    if (left.supportedGapCount != right.supportedGapCount)
      return left.supportedGapCount > right.supportedGapCount;
  }
  if (left.completionKnown != right.completionKnown) return left.completionKnown;
  if (left.completionKnown && left.completionPercent != right.completionPercent)
    return left.completionPercent > right.completionPercent;
  if (left.powerGapKnown != right.powerGapKnown) return left.powerGapKnown;
  if (left.powerGapKnown) {
    const qint64 leftGap = qMax<qint64>(0, qint64(left.highestPower) - left.currentPower);
    const qint64 rightGap = qMax<qint64>(0, qint64(right.highestPower) - right.currentPower);
    if (leftGap != rightGap) return leftGap > rightGap;
  }
  if (left.petInstanceId != right.petInstanceId)
    return left.petInstanceId < right.petInstanceId;
  return left.shopGoodKey < right.shopGoodKey;
}

namespace {

quint64 textBytes(const QString& text) {
  return text.isEmpty() ? 0 : 64 + quint64(text.size()) * sizeof(QChar);
}

quint64 conditionBytes(const ShopCondition& condition) {
  return textBytes(condition.reason) + textBytes(condition.source);
}

quint64 rowHeapBytes(const ActionRecommendation& row) {
  quint64 bytes = textBytes(row.stableId) + textBytes(row.petName) + textBytes(row.title) +
      textBytes(row.shopGoodKey) + textBytes(row.shopName) + textBytes(row.goodName);
  bytes += quint64(row.gaps.capacity()) * sizeof(QString);
  for (const QString& gap : row.gaps) bytes += textBytes(gap);
  bytes += quint64(row.requirements.capacity()) * sizeof(ResourceRequirement);
  for (const ResourceRequirement& requirement : row.requirements)
    bytes += textBytes(requirement.resourceKey) + textBytes(requirement.resourceName) +
             conditionBytes(requirement.condition);
  for (const ShopCondition* condition : {&row.eligibilityCondition, &row.costCondition,
       &row.resourceCondition, &row.limitCondition, &row.unlockCondition})
    bytes += conditionBytes(*condition);
  if (row.memoryRetention) bytes += 64; // Worker publication combines the inherited lease with its result lease.
  return bytes;
}

quint64 catalogRowHeapBytes(const ActionRecommendation& row) {
  quint64 bytes = textBytes(row.shopGoodKey) + textBytes(row.shopName) + textBytes(row.goodName) +
      quint64(row.requirements.capacity()) * sizeof(ResourceRequirement);
  for (const auto& requirement : row.requirements)
    bytes += textBytes(requirement.resourceKey) + textBytes(requirement.resourceName) + conditionBytes(requirement.condition);
  for (const auto* condition : {&row.costCondition,&row.resourceCondition,&row.limitCondition,&row.unlockCondition})
    bytes += conditionBytes(*condition);
  return bytes;
}

quint64 assetHeapBytes(const PetAssetRecord& pet) {
  quint64 bytes = textBytes(pet.name) + textBytes(pet.location) +
      quint64(pet.gaps.capacity() + pet.gapKeys.capacity()) * sizeof(QString);
  for (const auto& text : pet.gaps) bytes += textBytes(text);
  for (const auto& text : pet.gapKeys) bytes += textBytes(text);
  const QJsonObject identity = AssetDerivation::identityFields(pet.pet);
  for (auto field = identity.begin(); field != identity.end(); ++field)
    bytes += 96 + textBytes(field.key()) + (field.value().isString() ? textBytes(field.value().toString()) : 0);
  if (pet.memoryRetention) bytes += 64;
  return bytes;
}

}  // namespace

struct RecommendationSession::Impl {
  QString account;
  AccountAssetOverview overview;
  PreparedShopConditions prepared;
  ShopPetMetadataSnapshot metadata;
  QList<PetAnalysisFacts> preparedFacts;
  QSet<qint64> validatedIdentities;
  qsizetype validationIndex = 0;
  bool compactFacts = false;
  bool factsValidated = true;
  bool outputReserved = false;
  QBitArray retainedCatalogRows;
  bool retainedTitles[5] = {};
  bool retainedPetSource = false;
  AlgorithmPipelineStats ownedStats;
  AlgorithmPipelineStats* stats = nullptr;
  RecommendationSliceStats timing;
  QList<ActionRecommendation> result;
  quint64 resultHeapBytes = 0;
  quint64 overviewHeapBytes = 0;
  bool deriveOverview = false;
  bool recommendationEligible = false;
  qsizetype petIndex = 0;
  bool petActive = false;
  bool readyToSort = false;
  Status status = Status::Running;
  ShopPetDerived derived;
  const QList<qsizetype>* currentIndexes = nullptr;
  const QList<qsizetype>* originalIndexes = nullptr;
  QList<qsizetype> empty;
  qsizetype currentPosition = 0;
  qsizetype originalPosition = 0;
  qsizetype bestIndex = -1;
  ActionRecommendation best;
  int candidateCount = 0;

  Impl(const QString& key, const AccountAssetOverview& pets,
       const PreparedShopConditions& conditions, const ShopPetMetadataSnapshot& definitions,
       AlgorithmPipelineStats* counters, bool fullOverview, const QList<PetAnalysisFacts>* facts)
      : account(key), overview(pets), prepared(conditions), metadata(definitions),
        stats(counters ? counters : &ownedStats), deriveOverview(fullOverview || facts) {
    if (facts) {
      compactFacts = true;
      factsValidated = false;
      preparedFacts = *facts;
      if (account.isEmpty() || overview.account != account || !overview.pets.isEmpty() ||
          overview.totalPets != facts->size()) {
        status = Status::InvalidInput;
        return;
      }
    }
    if (deriveOverview) {
      overview.totalPets = overview.backpackPets = overview.normalWarehousePets = overview.eliteWarehousePets = 0;
      overview.fullyCultivatedPets = overview.improvablePets = overview.missingDetailPets = 0;
      overview.redStarMissingPets = overview.astrolabeMissingPets = overview.sacredMissingPets = overview.soulMissingPets = 0;
      overview.shopImprovablePets = 0; overview.totalCurrentPower = 0;
      overview.totalCurrentPowerKnown = false;
      overview.memoryRetention.reset();
      overviewHeapBytes = sizeof(AccountAssetOverview) + textBytes(overview.account) +
          quint64(overview.pets.capacity()) * sizeof(PetAssetRecord);
      refreshCharge();
    }
  }

  void refreshCharge() {
    timing.resultChargedBytes = resultHeapBytes + quint64(result.capacity()) * sizeof(ActionRecommendation) + overviewHeapBytes;
    timing.peakResultChargedBytes = qMax(timing.peakResultChargedBytes, timing.resultChargedBytes);
    timing.resultRowHeapBytes = resultHeapBytes;
    timing.resultRowCapacityBytes = quint64(result.capacity()) * sizeof(ActionRecommendation);
    timing.overviewCapacityBytes = deriveOverview ? quint64(overview.pets.capacity()) * sizeof(PetAssetRecord) : 0;
    timing.overviewHeapBytes = overviewHeapBytes - timing.overviewCapacityBytes;
  }

  quint64 candidateScratchBytes() const {
    return retainedCatalogRows.isEmpty() ? 0 : 64 + (quint64(retainedCatalogRows.size()) + 7) / 8;
  }
  void observeCandidate(quint64 transientBytes) {
    timing.peakCandidateChargedBytes = qMax(timing.peakCandidateChargedBytes,
        candidateScratchBytes() + transientBytes);
  }
  void reserveOutput(quint64 budget) {
    const quint64 count = quint64(compactFacts ? preparedFacts.size() : overview.pets.size());
    // A successful full overview retains P rows. Recommendations contain at
    // most one row per pet. Reserve that exact upper bound only after admission;
    // avoid Qt's byte-growth capacity overshooting P at the final allocation.
    const quint64 overviewCapacity = compactFacts ? count : quint64(overview.pets.capacity());
    const quint64 bytes = count * sizeof(ActionRecommendation) + (deriveOverview
        ? sizeof(AccountAssetOverview) + textBytes(overview.account) + overviewCapacity * sizeof(PetAssetRecord) : 0);
    if (bytes > budget) { cancel(Status::ResultBudgetExceeded); return; }
    result.reserve(qsizetype(count));
    if (compactFacts) {
      const auto before = overview.pets.capacity();
      overview.pets.reserve(qsizetype(count));
      overviewHeapBytes += quint64(overview.pets.capacity() - before) * sizeof(PetAssetRecord);
    }
    retainedCatalogRows.resize(prepared.catalog().goods().size());
    outputReserved = true;
    observeCandidate(0);
    refreshCharge();
  }

  void finishOverviewPet() {
    if (!deriveOverview) return;
    auto& pet = overview.pets[petIndex];
    pet.pet = AssetDerivation::identityFields(pet.pet);
    if (!compactFacts) pet.memoryRetention.reset();
    AssetDerivation::accumulate(&overview, pet);
    overviewHeapBytes += assetHeapBytes(pet);
    refreshCharge();
  }

  void appendResult(const ActionRecommendation& row, qsizetype catalogIndex = -1) {
    // These aliases are established by fillPresentation/finishPet, not inferred
    // from equal text. A selected good reuses exactly one prepared requirements
    // array and four account conditions. Each pet's name/gaps are the same Qt
    // values already retained by its overview row. Unique IDs, eligibility
    // reasons, identity JSON and per-row publication lease nodes remain billed.
    quint64 bytes = textBytes(row.stableId) + textBytes(row.eligibilityCondition.reason);
    if (!deriveOverview) {
      bytes += textBytes(row.petName) + quint64(row.gaps.capacity()) * sizeof(QString);
      for (const auto& gap : row.gaps) bytes += textBytes(gap);
    } else if (row.type == RecommendationType::LocalCultivation) {
      bytes += quint64(row.gaps.capacity()) * sizeof(QString);
    }
    if (catalogIndex >= 0 && !retainedCatalogRows.testBit(catalogIndex)) {
      bytes += catalogRowHeapBytes(row);
      retainedCatalogRows.setBit(catalogIndex);
    }
    const int titleIndex = typeRank(row.type);
    if (!retainedTitles[titleIndex]) { bytes += textBytes(row.title); retainedTitles[titleIndex] = true; }
    if (!retainedPetSource && !row.eligibilityCondition.source.isEmpty()) {
      bytes += textBytes(row.eligibilityCondition.source); retainedPetSource = true;
    }
    if (row.memoryRetention) bytes += 64;
    resultHeapBytes += bytes;
    result.append(row);
    refreshCharge();
  }

  void validateNextFact() {
    if (validationIndex == preparedFacts.size()) {
      validatedIdentities.clear(); validatedIdentities.squeeze();
      factsValidated = true;
      return;
    }
    const auto& facts = preparedFacts.at(validationIndex++);
    if (!validatePetAnalysisFacts(facts) || validatedIdentities.contains(facts.asset.instanceId)) {
      cancel(Status::InvalidInput); return;
    }
    validatedIdentities.insert(facts.asset.instanceId);
    observeCandidate(quint64(validatedIdentities.size()) * 64 + quint64(validatedIdentities.capacity()) * sizeof(qint64));
  }

  void beginPet() {
    PetBattlePowerState calculatedPower;
    bool cultivationDerived = false;
    if (petIndex >= (compactFacts ? preparedFacts.size() : overview.pets.size())) { readyToSort = true; return; }
    if (compactFacts) {
      const auto oldCapacity = overview.pets.capacity();
      overview.pets.append(preparedFacts.at(petIndex).asset);
      overview.pets.last().shopImprovable = false;
      overviewHeapBytes += quint64(overview.pets.capacity() - oldCapacity) * sizeof(PetAssetRecord);
      ++stats->preparedFactsReused;
      refreshCharge();
    }
    if (deriveOverview) {
      if (!compactFacts) {
        if (overview.pets.at(petIndex).detailAvailable) ++stats->rawPowerCalculations;
        const auto& seed = overview.pets.at(petIndex);
        if (seed.detailAvailable) calculatedPower = calculatePetBattlePower(seed.pet,
            petPowerMetadataFromCatalog(seed.pet, seed.metadataSlotMaxLevel,
                metadata.stargods, metadata.astrolabe, metadata.pets));
        overview.pets[petIndex] = AssetDerivation::derivePetWithPower(seed, calculatedPower);
        if (overview.pets.at(petIndex).detailAvailable) {
          derived = deriveShopPetWithPower(overview.pets.at(petIndex).pet, true, metadata, calculatedPower, stats);
          cultivationDerived = true;
          AssetDerivation::applyCultivation(&overview.pets[petIndex], derived);
        }
      }
    }
    const PetAssetRecord& pet = overview.pets.at(petIndex);
    recommendationEligible = pet.detailAvailable && !pet.fullyCultivated && pet.improvable && pet.instanceId > 0;
    const bool ownedStarAction = pet.gapKeys.contains(QStringLiteral("sg_equip")) ||
        pet.gapKeys.contains(QStringLiteral("sg_level"));
    if (!pet.detailAvailable || pet.instanceId <= 0 || (!deriveOverview && !recommendationEligible && !ownedStarAction)) {
      finishOverviewPet(); ++petIndex; return;
    }
    if (compactFacts) derived = preparedFacts.at(petIndex).eligibility;
    else if (!cultivationDerived) derived = deriveShopPet(pet.pet, pet.detailAvailable, metadata, stats);
    const auto& catalog = prepared.catalog();
    const bool petInvalidated =
        prepared.context().petFreshness == ShopConditionFreshness::Invalidated;
    const bool allPotential = derived.raceId <= 0 || petInvalidated;
    currentIndexes = allPotential ? &catalog.allGoodIndexes() : &catalog.goodsForRace(derived.raceId);
    originalIndexes = allPotential ? &empty : &catalog.goodsForRace(derived.metadataRaceId);
    currentPosition = originalPosition = 0;
    bestIndex = -1;
    candidateCount = 0;
    best = {};
    petActive = true;
  }

  bool candidatesRemain() const {
    return currentPosition < currentIndexes->size() || originalPosition < originalIndexes->size();
  }

  void evaluateOneCandidate() {
    const auto& catalog = prepared.catalog();
    const auto& context = prepared.context();
    const auto& pet = overview.pets.at(petIndex);
    const bool petInvalidated = context.petFreshness == ShopConditionFreshness::Invalidated;
    const auto& current = *currentIndexes;
    const auto& original = *originalIndexes;
    qsizetype index;
    if (originalPosition >= original.size() ||
        (currentPosition < current.size() &&
         current[currentPosition] < original[originalPosition])) {
      index = current[currentPosition++];
    } else if (currentPosition >= current.size() ||
               original[originalPosition] < current[currentPosition]) {
      index = original[originalPosition++];
    } else {
      index = current[currentPosition++];
      ++originalPosition;
    }
    if (stats) ++stats->candidatePairsVisited;
    const CompiledShopGood& compiled = catalog.goods().at(index);
    const ShopExchangeGood& good = compiled.good;
    const PreparedShopGoodConditions& accountConditions = prepared.goods().at(index);
    ShopPetEligibilitySummary eligibility = evaluateShopPetRule(compiled.petRule, derived, stats);
    const bool raceAllowed = compiled.raceIds.contains(derived.raceId) ||
        (derived.metadataRaceId > 0 && compiled.raceIds.contains(derived.metadataRaceId));
    // One H visit supplies both the cultivation/applicability overview and
    // recommendation selection. Account conditions must not hide applicability.
    if (deriveOverview && raceAllowed && eligibility.state == ShopPetEligibilityState::Usable)
      overview.pets[petIndex].shopImprovable = true;
    if (!recommendationEligible || accountConditions.excluded) return;
    ShopConditionState petState = ShopConditionState::Unknown;
    if (!raceAllowed) {
      petState = derived.raceId > 0 ? ShopConditionState::Blocked : ShopConditionState::Unknown;
      eligibility.usefulCount = 0;
      eligibility.redStarUseful = false;
    } else if (eligibility.state == ShopPetEligibilityState::Usable) {
      petState = ShopConditionState::Satisfied;
    } else if (eligibility.state == ShopPetEligibilityState::NotUsable) {
      petState = ShopConditionState::Blocked;
    }
    if (petInvalidated) petState = ShopConditionState::Unknown;
    if (petState == ShopConditionState::Blocked) return;
    ActionRecommendation candidate;
    candidate.unknownConditionCount = accountConditions.unknownConditionCount +
        (petState == ShopConditionState::Unknown ? 1 : 0);
    candidate.type = candidate.unknownConditionCount > 0
        ? RecommendationType::ConditionUnknown
        : accountConditions.account.resourcesEnough ? RecommendationType::ReadyNow
                                                     : RecommendationType::ResourceMissing;
    fillPetRanking(&candidate, pet);
    candidate.shopGoodKey = compiled.stableKey;
    candidate.remainingExchangeCount = accountConditions.account.remainingCount;
    candidate.supportedGapCount = petInvalidated ? 0 : eligibility.usefulCount;
    if (petInvalidated) {
      candidate.completionKnown = false;
      candidate.powerGapKnown = false;
    }
    candidate.resourceCoverageKnown = accountConditions.account.resourcesKnown;
    candidate.resourceCoverageMillionths = accountConditions.resourceCoverageMillionths;
    if (candidate.type == RecommendationType::ReadyNow &&
        good.provenGapCode == QStringLiteral("34") &&
        good.provenGapUnitsPerExchange == 1 && pet.stargodSlotsKnown &&
        pet.missingRedStars > 0 && eligibility.redStarUseful) {
      candidate.actionableCountKnown = true;
      candidate.actionableCount = qMin(accountConditions.account.remainingCount,
          qMin(accountConditions.resourceSupportedExchanges, pet.missingRedStars));
      candidate.remainingGapAfterAction = pet.missingRedStars - candidate.actionableCount;
      candidate.closesKnownGap = candidate.remainingGapAfterAction == 0;
    }
    ++candidateCount;
    if (stats) {
      stats->maximumLiveCandidates = qMax<qint64>(stats->maximumLiveCandidates,
                                                  bestIndex < 0 ? 1 : 2);
      if (bestIndex >= 0) ++stats->candidateComparisons;
    }
    if (bestIndex < 0 || PreparedRecommendationEngine::less(candidate, best)) {
      best = candidate;
      bestIndex = index;
    }
    observeCandidate(quint64(2 * sizeof(ActionRecommendation)) + rowHeapBytes(best) + rowHeapBytes(candidate));
  }

  void finishPet() {
    const auto& catalog = prepared.catalog();
    auto context = prepared.context();
    const auto& pet = overview.pets.at(petIndex);
    const bool ownedStarAction = pet.gapKeys.contains(QStringLiteral("sg_equip")) ||
        pet.gapKeys.contains(QStringLiteral("sg_level"));
    if (bestIndex >= 0) {
      const CompiledShopGood& compiled = catalog.goods().at(bestIndex);
      const ShopActionability conditions = describePreparedShopActionability(
          compiled, prepared.goods().at(bestIndex), derived, context);
      best.shopName = compiled.good.shopName;
      best.goodName = compiled.good.description;
      best.requirements = conditions.requirements;
      best.eligibilityCondition = conditions.eligibilityCondition;
      best.costCondition = conditions.costCondition;
      best.resourceCondition = conditions.resourceCondition;
      best.limitCondition = conditions.limitCondition;
      best.unlockCondition = conditions.unlockCondition;
      best.alternateGoodCount = candidateCount - 1;
      fillPresentation(&best, account, pet);
      appendResult(best, bestIndex);
      if (stats) ++stats->finalRowsMaterialized;
    } else if (!ownedStarAction && recommendationEligible && pet.completionKnown && pet.completionPercent >= 90 &&
               pet.completionPercent <= 100) {
      ActionRecommendation nearFull;
      nearFull.type = RecommendationType::NearFullCultivation;
      fillPetRanking(&nearFull, pet);
      fillPresentation(&nearFull, account, pet);
      appendResult(nearFull);
      if (stats) ++stats->finalRowsMaterialized;
    }
    if (pet.detailAvailable && pet.instanceId > 0 && (ownedStarAction ||
        (bestIndex < 0 && recommendationEligible && !pet.completionKnown))) {
      ActionRecommendation local;
      local.type = RecommendationType::LocalCultivation;
      fillPetRanking(&local, pet);
      fillPresentation(&local, account, pet);
      if (ownedStarAction) {
        local.gaps.clear();
        for (const auto& gap : pet.gaps)
          if (gap.contains(QStringLiteral("已有星神")) || gap.contains(QStringLiteral("星神等级")))
            local.gaps.append(gap);
      }
      appendResult(local);
      if (stats) ++stats->finalRowsMaterialized;
    }
    finishOverviewPet();
    petActive = false;
    best = {};
    ++petIndex;
  }

  void cancel(Status reason) {
    status = reason;
    result.clear();
    result.squeeze();
    if (deriveOverview) { overview.pets.clear(); overview.pets.squeeze(); }
    validatedIdentities.clear(); validatedIdentities.squeeze();
    best = {};
    timing.resultChargedBytes = 0;
  }
};

RecommendationSession::RecommendationSession(
    const QString& account, const AccountAssetOverview& overview,
    const PreparedShopConditions& prepared, const ShopPetMetadataSnapshot& metadata,
    AlgorithmPipelineStats* stats, bool deriveOverview, const QList<PetAnalysisFacts>* preparedFacts)
    : impl_(std::make_unique<Impl>(account, overview, prepared, metadata, stats, deriveOverview, preparedFacts)) {}

RecommendationSession::~RecommendationSession() = default;

RecommendationSession::Status RecommendationSession::step(
    const std::atomic_bool* cancelled, int maximumMilliseconds,
    qsizetype maximumWorkUnits, quint64 resultBudgetBytes) {
  if (impl_->status != Status::Running) return impl_->status;
  const auto isCancelled = [cancelled] {
    return cancelled && cancelled->load(std::memory_order_acquire);
  };
  QElapsedTimer slice;
  slice.start();
  ++impl_->timing.slices;
  const qint64 sliceBudget = qBound(1, maximumMilliseconds, 16) * 1000000LL;
  const qsizetype workLimit = qMax<qsizetype>(1, maximumWorkUnits);
  if (!impl_->outputReserved && !isCancelled()) {
    QElapsedTimer allocation; allocation.start();
    impl_->reserveOutput(resultBudgetBytes);
    impl_->timing.maximumAtomicWorkNanoseconds = qMax(
        impl_->timing.maximumAtomicWorkNanoseconds, allocation.nsecsElapsed());
  }
  for (qsizetype unit = 0; unit < workLimit && impl_->status == Status::Running; ++unit) {
    if (isCancelled()) { impl_->cancel(Status::Cancelled); break; }
    QElapsedTimer atomicWork;
    atomicWork.start();
    if (!impl_->factsValidated) {
      impl_->validateNextFact();
    } else if (impl_->readyToSort) {
      // Sorting is an explicitly measured non-preemptible operation. The
      // cancellation flag is checked immediately before and after it.
      std::sort(impl_->result.begin(), impl_->result.end(),
          [this](const ActionRecommendation& left, const ActionRecommendation& right) {
            ++impl_->stats->finalSortComparisons;
            return PreparedRecommendationEngine::less(left, right);
          });
      if (impl_->deriveOverview)
        std::sort(impl_->overview.pets.begin(), impl_->overview.pets.end(),
            [](const PetAssetRecord& left, const PetAssetRecord& right) {
          if (left.completionKnown != right.completionKnown) return !left.completionKnown;
          if (left.completionPercent != right.completionPercent) return left.completionPercent < right.completionPercent;
          const int name = left.name.compare(right.name, Qt::CaseSensitive);
          return name == 0 ? left.instanceId < right.instanceId : name < 0;
        });
      impl_->timing.sortNanoseconds = atomicWork.nsecsElapsed();
      impl_->observeCandidate(quint64(64 * sizeof(ActionRecommendation)));
      impl_->status = Status::Complete;
      if (isCancelled()) impl_->cancel(Status::Cancelled);
    } else if (!impl_->petActive) {
      impl_->beginPet();
    } else if (impl_->candidatesRemain()) {
      impl_->evaluateOneCandidate();
    } else {
      impl_->finishPet();
    }
    impl_->timing.maximumAtomicWorkNanoseconds = qMax(
        impl_->timing.maximumAtomicWorkNanoseconds, atomicWork.nsecsElapsed());
    if (impl_->timing.resultChargedBytes > resultBudgetBytes)
      impl_->cancel(Status::ResultBudgetExceeded);
    if (impl_->status != Status::Running || impl_->readyToSort || slice.nsecsElapsed() >= sliceBudget)
      break;
  }
  const qint64 active = slice.nsecsElapsed();
  impl_->timing.activeNanoseconds += active;
  impl_->timing.maximumSliceNanoseconds = qMax(
      impl_->timing.maximumSliceNanoseconds, active);
  return impl_->status;
}

RecommendationSession::Status RecommendationSession::status() const { return impl_->status; }

const RecommendationSliceStats& RecommendationSession::sliceStats() const { return impl_->timing; }

QList<ActionRecommendation> RecommendationSession::takeResults() {
  if (impl_->status != Status::Complete) return {};
  return std::move(impl_->result);
}

AccountAssetOverview RecommendationSession::takeOverview() {
  if (impl_->status != Status::Complete || !impl_->deriveOverview) return {};
  return std::move(impl_->overview);
}

QList<ActionRecommendation> PreparedRecommendationEngine::generatePrepared(
    const QString& account, const AccountAssetOverview& overview,
    const PreparedShopConditions& prepared,
    const ShopPetMetadataSnapshot& metadata, AlgorithmPipelineStats* stats) {
  RecommendationSession session(account, overview, prepared, metadata, stats);
  while (session.step(nullptr, 16, 4096, std::numeric_limits<quint64>::max()) ==
         RecommendationSession::Status::Running) {}
  return session.takeResults();
}

QList<ActionRecommendation> PreparedRecommendationEngine::applyFreshness(
    const QList<ActionRecommendation>& recommendations,
    bool cultivationStale, bool shopStale) {
  return applyFreshness(recommendations,cultivationStale,shopStale,{},nullptr);
}

QList<ActionRecommendation> PreparedRecommendationEngine::applyFreshness(
    const QList<ActionRecommendation>& recommendations, bool cultivationStale, bool shopStale,
    const CopyReservation& reserveCopy, bool* budgetExceeded) {
  if (budgetExceeded) *budgetExceeded = false;
  bool changes = false;
  for (const auto& row : recommendations) {
    const bool shop = shopStale && !row.shopGoodKey.isEmpty();
    if (row.cultivationStale != cultivationStale || row.shopStale != shop ||
        row.stale != (cultivationStale || shop) ||
        (!row.shopGoodKey.isEmpty() && (cultivationStale || shop))) { changes = true; break; }
  }
  if (!changes) return recommendations;
  auto reject = [&]() -> QList<ActionRecommendation> {
    if (budgetExceeded) *budgetExceeded = true;
    return {};
  };
  // A pointer key represents the same implicitly shared QList allocation, not
  // merely equal costs. Charge the bounded planning table before allocating it.
  const quint64 scratchBytes = 128 + (shopStale ? quint64(recommendations.size()) * 128 : 0);
  const auto scratchRetention = reserveCopy ? reserveCopy(scratchBytes) : std::shared_ptr<void>{};
  if (reserveCopy && !scratchRetention) return reject();
  QHash<const ResourceRequirement*, QList<ResourceRequirement>> convertedRequirements;
  quint64 copyBytes = 256 + quint64(recommendations.size()) * (sizeof(ActionRecommendation) + 64);
  if (shopStale) for (const auto& row : recommendations) {
    if (row.shopGoodKey.isEmpty() || row.requirements.isEmpty() ||
        convertedRequirements.contains(row.requirements.constData())) continue;
    convertedRequirements.insert(row.requirements.constData(),{});
    copyBytes += quint64(row.requirements.size()) * sizeof(ResourceRequirement);
  }
  const auto copyRetention = reserveCopy ? reserveCopy(copyBytes) : std::shared_ptr<void>{};
  if (reserveCopy && !copyRetention) return reject();
  struct CopyRetention {
    std::shared_ptr<void> source, copy;
  };
  QList<ActionRecommendation> result;
  result.reserve(recommendations.size());
  for (const auto& original : recommendations) {
    result.append(original);
    if (copyRetention) result.last().memoryRetention = std::make_shared<CopyRetention>(
        CopyRetention{original.memoryRetention,copyRetention});
  }
  for (ActionRecommendation& recommendation : result) {
    recommendation.cultivationStale = cultivationStale;
    recommendation.shopStale = shopStale && !recommendation.shopGoodKey.isEmpty();
    recommendation.stale = recommendation.cultivationStale || recommendation.shopStale;
    if (recommendation.shopGoodKey.isEmpty() || !recommendation.stale) continue;
    if (cultivationStale) {
      recommendation.eligibilityCondition.freshness = ShopConditionFreshness::Invalidated;
      recommendation.supportedGapCount = 0;
      recommendation.completionKnown = false;
      recommendation.powerGapKnown = false;
    }
    if (recommendation.shopStale) {
      recommendation.limitCondition.freshness = ShopConditionFreshness::Invalidated;
      recommendation.resourceCondition.freshness = ShopConditionFreshness::Invalidated;
      recommendation.unlockCondition.freshness = ShopConditionFreshness::Invalidated;
      if (!recommendation.requirements.isEmpty()) {
        auto converted = convertedRequirements.find(recommendation.requirements.constData());
        Q_ASSERT(converted != convertedRequirements.end());
        if (converted->isEmpty()) {
          converted->reserve(recommendation.requirements.size());
          for (const auto& original : std::as_const(recommendation.requirements)) {
            auto requirement = original;
            requirement.condition.freshness = ShopConditionFreshness::Invalidated;
            converted->append(std::move(requirement));
          }
        }
        recommendation.requirements = *converted;
      }
      recommendation.resourceCoverageKnown = false;
      recommendation.remainingExchangeCount = -1;
    }
    recommendation.type = RecommendationType::ConditionUnknown;
    recommendation.title = typeTitle(recommendation.type);
    recommendation.actionableCountKnown = false;
    recommendation.actionableCount = 0;
    recommendation.closesKnownGap = false;
    // Recalculate unknown dimensions without counting the aggregate resource
    // condition twice when individual balances are available.
    recommendation.unknownConditionCount = 0;
    for (const ShopCondition* condition :
         {&recommendation.eligibilityCondition, &recommendation.costCondition,
          &recommendation.limitCondition, &recommendation.unlockCondition})
      recommendation.unknownConditionCount +=
          condition->effectiveState() == ShopConditionState::Unknown;
    if (recommendation.costCondition.effectiveState() == ShopConditionState::Satisfied)
      for (const ResourceRequirement& requirement : std::as_const(recommendation.requirements))
        recommendation.unknownConditionCount +=
            requirement.condition.effectiveState() == ShopConditionState::Unknown;
  }
  std::sort(result.begin(), result.end(), less);
  return result;
}
