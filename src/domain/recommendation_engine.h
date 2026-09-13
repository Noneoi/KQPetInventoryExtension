#pragma once

#include "account_resource_view.h"
#include "asset_analysis_types.h"
#include "recommendation_types.h"
#include "catalog_types.h"
#include "prepared_shop_conditions.h"
#include "pet_analysis_facts.h"
#include <atomic>
#include <memory>
#include <functional>

struct RecommendationSliceStats {
  quint64 slices = 0;
  qint64 maximumSliceNanoseconds = 0;
  qint64 maximumAtomicWorkNanoseconds = 0;
  qint64 sortNanoseconds = 0;
  quint64 resultChargedBytes = 0;
  quint64 peakResultChargedBytes = 0;
  quint64 peakCandidateChargedBytes = 0;
  qint64 activeNanoseconds = 0;
  quint64 resultRowHeapBytes = 0;
  quint64 resultRowCapacityBytes = 0;
  quint64 overviewHeapBytes = 0;
  quint64 overviewCapacityBytes = 0;
};

// A value-only resumable calculation. The owner calls step() on one thread;
// cancellation may be set from another thread without queued delivery.
class RecommendationSession final {
public:
  enum class Status { Running, Complete, Cancelled, ResultBudgetExceeded, InvalidInput };
  RecommendationSession(const QString& account, const AccountAssetOverview& overview,
      const PreparedShopConditions& prepared, const ShopPetMetadataSnapshot& metadata,
      AlgorithmPipelineStats* stats = nullptr, bool deriveOverview = false,
      const QList<PetAnalysisFacts>* preparedFacts = nullptr);
  ~RecommendationSession();
  RecommendationSession(const RecommendationSession&) = delete;
  RecommendationSession& operator=(const RecommendationSession&) = delete;

  Status step(const std::atomic_bool* cancelled = nullptr, int maximumMilliseconds = 16,
              qsizetype maximumWorkUnits = 256,
              quint64 resultBudgetBytes = 128ULL * 1024 * 1024);
  Status status() const;
  const RecommendationSliceStats& sliceStats() const;
  QList<ActionRecommendation> takeResults();
  AccountAssetOverview takeOverview();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class PreparedRecommendationEngine final {
public:
  static QList<ActionRecommendation> generatePrepared(
      const QString& account, const AccountAssetOverview& overview,
      const PreparedShopConditions& prepared,
      const ShopPetMetadataSnapshot& metadata,
      AlgorithmPipelineStats* stats = nullptr);
  // Shared deterministic ranking for per-pet selection and final ordering.
  static bool less(const ActionRecommendation& left,
                   const ActionRecommendation& right);
  static QList<ActionRecommendation> applyFreshness(
      const QList<ActionRecommendation>& recommendations,
      bool cultivationStale, bool shopStale);
  // The callback reserves additional copy/scratch allocations before mutation.
  // Every returned row retains the copy lease. Rejection is explicit and never
  // returns the original actionable rows as an invalidated view.
  using CopyReservation = std::function<std::shared_ptr<void>(quint64)>;
  static QList<ActionRecommendation> applyFreshness(
      const QList<ActionRecommendation>& recommendations,
      bool cultivationStale, bool shopStale,
      const CopyReservation& reserveCopy, bool* budgetExceeded);
};
