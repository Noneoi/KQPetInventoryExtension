#pragma once

#include "domain/recommendation_engine.h"

#include <QObject>
#include <QByteArray>
#include <functional>
#include <memory>

struct AnalysisVersionStamp {
  quint64 inventory = 0;
  quint64 details = 0;
  quint64 catalog = 0;
  quint64 resources = 0;
  quint64 limits = 0;
  int analysis = AssetAnalysisVersion::kCurrentAnalysis;
  quint64 routines = 0;
  quint64 routineCatalog = 0;
  quint64 catalogDay = 0;
  quint64 trust = 0;
  quint64 metadata = 0;
  quint64 quotaFreshness = 0;
  quint64 routineFreshness = 0;
  QString build;
  QString profile;
  bool operator==(const AnalysisVersionStamp& other) const;
};

struct AnalysisJobKey {
  quint64 jobId = 0;
  QString account;
  quint64 epoch = 0;
  AnalysisVersionStamp versions;
  bool operator==(const AnalysisJobKey& other) const;
};

struct AnalysisWorkInput {
  AnalysisJobKey key;
  AccountAssetOverview overview;
  PreparedShopConditions conditions;
  ShopPetMetadataSnapshot metadata;
  // If original protocol bytes are retained alongside JSON, they are charged
  // separately. The JSON value trees themselves are always charged as well.
  QList<QByteArray> retainedRawPayloads;
  bool deriveOverview = false;
  // Compact production path. overview keeps account/version/routine header
  // only, pets must be empty and totalPets must equal this frozen collection.
  bool usePreparedFacts = false;
  QList<PetAnalysisFacts> preparedFacts;
  // Production freezes these values on Core. Compilation, race indexing and
  // account-condition preparation happen incrementally on Compute.
  std::shared_ptr<const ShopCatalogSnapshot> catalogSnapshot;
  QDate catalogDate;
  QJsonObject materialDefinitions;
  QJsonObject shopPacket;
  QHash<QString, qint64> resourceCounts;
  bool resourceCountsKnown = false;
  bool sourceInvalidated = true;
  ShopConditionContext conditionContext;
};

struct AnalysisMemoryLimits {
  quint64 inputBytes = 128ULL * 1024 * 1024;
  quint64 resultBytes = 128ULL * 1024 * 1024;
  quint64 candidateBytes = 1024ULL * 1024;
  quint64 totalBytes = 512ULL * 1024 * 1024;
  int sliceMilliseconds = 16;
  qsizetype sliceWorkUnits = 256;
  // Included in totalBytes, never an additional allowance. At most two values.
  quint64 compiledCatalogBytes = 16ULL * 1024 * 1024;
};

// Conservative logical charges, including reserved input capacity and all
// retained shared results. These are not allocator or process Private Bytes.
struct AnalysisMemoryUsage {
  quint64 inputChargedBytes = 0;
  quint64 resultChargedBytes = 0;
  quint64 candidateChargedBytes = 0;
  quint64 peakChargedBytes = 0;
  quint64 peakResultChargedBytes = 0;
  quint64 peakPublicationOverlapBytes = 0;
  quint64 snapshotsCaptured = 0;
  quint64 computeSlices = 0;
  quint64 computeEventLoopTurns = 0;
  // Counter for the current or most recently executed job.
  quint64 candidatePairsVisited = 0;
  quint64 peakInputChargedBytes = 0;
  quint64 peakCandidateChargedBytes = 0;
  quint64 compiledCatalogChargedBytes = 0;
  quint64 peakCompiledCatalogChargedBytes = 0;
  quint64 compiledCatalogCacheHits = 0;
  quint64 compiledCatalogCacheMisses = 0;
};

enum class AnalysisJobOutcome {
  Published, Cancelled, Superseded, Stale, InputRejected,
  BudgetExceeded, FactoryFailed, Closed
};

struct AnalysisWorkResult {
  // Must outlive all nested payload destruction, including shared GUI copies.
  std::shared_ptr<void> memoryRetention;
  AnalysisJobKey key;
  quint64 generation = 0;
  QList<ActionRecommendation> recommendations;
  AccountAssetOverview overview;
  AlgorithmPipelineStats work;
  RecommendationSliceStats slices;
  quint64 measuredInputBytes = 0;
  quint64 retainedRawPayloadBytes = 0;
  qint64 cancellationLatencyNanoseconds = 0;
  qint64 maximumInputMeterSliceNanoseconds = 0;
  qint64 maximumPreparationSliceNanoseconds = 0;
  qint64 maximumAtomicPreparationNanoseconds = 0;
  bool catalogCacheHit = false;
  qint64 activeInputMeterNanoseconds = 0;
  qint64 catalogCompileNanoseconds = 0;
  qint64 conditionPrepareNanoseconds = 0;
  qint64 candidateComputeNanoseconds = 0;
  qint64 sortNanoseconds = 0;
  // Copied with the value, keeping memory charged until every shared result
  // reference is gone, even after the scheduler replaces its displayed result.
};

enum class AnalysisJobPhase { Preparation, Queued, Compute };

struct AnalysisJobFinished {
  AnalysisJobKey key;
  quint64 generation = 0;
  AnalysisJobOutcome outcome = AnalysisJobOutcome::Cancelled;
  qint64 cancellationLatencyNanoseconds = 0;
  RecommendationSliceStats slices;
  qint64 maximumInputMeterSliceNanoseconds = 0;
  quint64 measuredInputBytes = 0;
  bool inputMeasurementComplete = false;
  AnalysisJobPhase phase = AnalysisJobPhase::Compute;
  QString error;
};
Q_DECLARE_METATYPE(AnalysisJobFinished)

// Own on Core. Every method and callback runs on its owning thread, except
// immutable shared result destruction and the private Compute execution.
class AnalysisWorker final : public QObject {
public:
  using InputFactory = std::function<std::shared_ptr<const AnalysisWorkInput>(const AnalysisJobKey&)>;
  using PublicationGuard = std::function<bool(const AnalysisJobKey&)>;
  using ResultHandler = std::function<void(std::shared_ptr<const AnalysisWorkResult>)>;
  using FinishedHandler = std::function<void(const AnalysisJobFinished&)>;

  explicit AnalysisWorker(const AnalysisMemoryLimits& limits = {}, QObject* parent = nullptr);
  ~AnalysisWorker() override;
  void setInputFactory(InputFactory factory);
  void setPublicationGuard(PublicationGuard guard);
  void setResultHandler(ResultHandler handler);
  void setFinishedHandler(FinishedHandler handler);
  // Returns the local generation; repeated identical keys still create distinct
  // generations. Only this small descriptor is stored while Compute is busy.
  quint64 submit(const AnalysisJobKey& key);
  void cancel();
  bool busy() const;
  bool hasPendingDescriptor() const;
  std::shared_ptr<const AnalysisWorkResult> lastResult() const;
  AnalysisMemoryUsage memoryUsage() const;
  QList<ActionRecommendation> freshnessProjection(const QList<ActionRecommendation>& rows,
      bool cultivationStale, bool shopStale, bool* budgetExceeded = nullptr) const;
  // One bounded high-priority image decode, on this same Compute thread.
  // The caller owns byte reservations for the frozen image input/output.
  // Never inline; false means busy/closing. Callbacks must contain no GUI work.
  bool postPriority(std::function<void()> job);
  // At most 2 seconds. A timed-out running QThread is retained until it finishes;
  // it is never destroyed or terminated while executing.
  bool shutdown(int maximumWaitMilliseconds = 2000);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
