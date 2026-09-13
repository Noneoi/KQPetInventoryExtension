#include "analysis_worker.h"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <limits>
#include <optional>

bool AnalysisVersionStamp::operator==(const AnalysisVersionStamp& other) const {
  return inventory == other.inventory && details == other.details &&
      catalog == other.catalog && resources == other.resources &&
      limits == other.limits && analysis == other.analysis && routines == other.routines &&
      routineCatalog == other.routineCatalog && catalogDay == other.catalogDay &&
      trust == other.trust && metadata == other.metadata && quotaFreshness == other.quotaFreshness &&
      routineFreshness == other.routineFreshness && build == other.build && profile == other.profile;
}

bool AnalysisJobKey::operator==(const AnalysisJobKey& other) const {
  return jobId == other.jobId && account == other.account &&
      epoch == other.epoch && versions == other.versions;
}

namespace {

qint64 monotonicNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

void atomicMaximum(std::atomic<quint64>& target, quint64 value) {
  quint64 previous = target.load(std::memory_order_relaxed);
  while (previous < value && !target.compare_exchange_weak(
      previous, value, std::memory_order_relaxed)) {}
}

struct Cancellation {
  std::atomic_bool requested{false};
  std::atomic<qint64> requestedAt{0};
  void cancel() {
    qint64 empty = 0;
    requestedAt.compare_exchange_strong(empty, monotonicNanoseconds());
    requested.store(true, std::memory_order_release);
  }
  qint64 latency() const {
    const qint64 at = requestedAt.load(std::memory_order_acquire);
    return at > 0 ? qMax<qint64>(0, monotonicNanoseconds() - at) : 0;
  }
};

struct PriorityState {
  std::atomic_bool busy{false};
  std::atomic_bool closing{false};
};
class PriorityEvent final : public QEvent {
public:
  static QEvent::Type eventType() {
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
  }
  PriorityEvent(std::shared_ptr<PriorityState> state, std::function<void()> job)
      : QEvent(eventType()), state_(std::move(state)), job_(std::move(job)) {}
  ~PriorityEvent() override { state_->busy.store(false, std::memory_order_release); }
  void execute() {
    if (state_->closing.load(std::memory_order_acquire)) return;
    try { job_(); } catch (...) { /* Keep the host Compute event loop alive. */ }
  }
private:
  std::shared_ptr<PriorityState> state_;
  std::function<void()> job_;
};

struct MemoryLedger {
  std::atomic<quint64> input{0}, results{0}, candidates{0}, compiled{0};
  std::atomic<quint64> peak{0}, peakResults{0}, overlap{0};
  std::atomic<quint64> peakInput{0}, peakCandidates{0};
  std::atomic<quint64> captured{0}, slices{0}, turns{0}, pairs{0};
  std::atomic<quint64> peakCompiled{0}, cacheHits{0}, cacheMisses{0};

  quint64 total() const {
    return input.load() + results.load() + candidates.load() + compiled.load();
  }
  void observe() {
    atomicMaximum(peak, total());
    atomicMaximum(peakResults, results.load());
    atomicMaximum(peakInput, input.load());
    atomicMaximum(peakCandidates, candidates.load());
    atomicMaximum(peakCompiled, compiled.load());
  }
  AnalysisMemoryUsage snapshot() const {
    return {input.load(), results.load(), candidates.load(), peak.load(),
            peakResults.load(), overlap.load(), captured.load(), slices.load(), turns.load(), pairs.load(),
            peakInput.load(), peakCandidates.load(), compiled.load(), peakCompiled.load(),
            cacheHits.load(), cacheMisses.load()};
  }
};

class MemoryLease final {
public:
  enum class Kind { Input, Result, Candidate, CompiledCatalog };
  MemoryLease(std::shared_ptr<MemoryLedger> ledger, Kind kind, quint64 bytes = 0)
      : ledger_(std::move(ledger)), kind_(kind) { resize(bytes); }
  ~MemoryLease() { resize(0); }
  void resize(quint64 bytes) {
    auto& counter = kind_ == Kind::Input ? ledger_->input
        : kind_ == Kind::Result ? ledger_->results
        : kind_ == Kind::Candidate ? ledger_->candidates : ledger_->compiled;
    if (bytes >= bytes_) counter.fetch_add(bytes - bytes_);
    else counter.fetch_sub(bytes_ - bytes);
    bytes_ = bytes;
    ledger_->observe();
  }
  quint64 bytes() const { return bytes_; }
private:
  std::shared_ptr<MemoryLedger> ledger_;
  Kind kind_;
  quint64 bytes_ = 0;
};

struct CombinedResultRetention {
  std::shared_ptr<void> inherited;
  std::shared_ptr<MemoryLease> result;
};
std::shared_ptr<void> retainResultWithSource(const std::shared_ptr<void>& inherited,
                                           const std::shared_ptr<MemoryLease>& result) {
  if (!inherited) return result;
  return std::make_shared<CombinedResultRetention>(CombinedResultRetention{inherited,result});
}

quint64 stringCharge(const QString& text) {
  return text.isEmpty() ? 0 : 64 + quint64(text.size()) * sizeof(QChar);
}

quint64 stringsCharge(const QStringList& strings) {
  quint64 bytes = quint64(strings.capacity()) * sizeof(QString);
  for (const QString& value : strings) bytes += stringCharge(value);
  return bytes;
}

quint64 conditionCharge(const ShopCondition& value) {
  return stringCharge(value.reason) + stringCharge(value.source);
}

quint64 requirementsCharge(const QList<ResourceRequirement>& values) {
  quint64 bytes = quint64(values.capacity()) * sizeof(ResourceRequirement);
  for (const auto& value : values)
    bytes += stringCharge(value.resourceKey) + stringCharge(value.resourceName) +
             conditionCharge(value.condition);
  return bytes;
}

quint64 rawGoodCharge(const ShopExchangeGood& good) {
  return sizeof(ShopExchangeGood) + stringCharge(good.shopName) + stringCharge(good.description) +
      stringCharge(good.cost) + stringCharge(good.enhanceType) + stringCharge(good.unlock) +
      stringCharge(good.limitKey) + stringCharge(good.limitText) + stringCharge(good.limitLabel) +
      stringCharge(good.tag) + stringCharge(good.provenGapCode) + quint64(good.raceIds.capacity()) * sizeof(int);
}
quint64 preparedCharge(const PreparedShopGoodConditions& good) {
  quint64 bytes = requirementsCharge(good.account.requirements);
  for (const ShopCondition* value : {&good.account.costCondition, &good.account.resourceCondition,
      &good.account.limitCondition, &good.account.unlockCondition}) bytes += conditionCharge(*value);
  return bytes;
}

struct CompiledCatalogEntry {
  // Declared first so the lease outlives all retained Qt values.
  std::shared_ptr<MemoryLease> retention;
  std::weak_ptr<const ShopCatalogSnapshot> source;
  quint64 sourceRevision = 0, catalogRevision = 0, metadataRevision = 0;
  int analysisVersion = 0;
  QDate date;
  QJsonObject materials;
  CompiledShopCatalog catalog;
  bool matches(const AnalysisWorkInput& input) const {
    const auto live = source.lock();
    return live && live.get() == input.catalogSnapshot.get() &&
        !source.owner_before(input.catalogSnapshot) && !input.catalogSnapshot.owner_before(source) &&
        sourceRevision == input.catalogSnapshot->revision && catalogRevision == input.key.versions.catalog &&
        metadataRevision == input.key.versions.metadata && analysisVersion == input.key.versions.analysis &&
        date == input.catalogDate && materials == input.materialDefinitions;
  }
};

// Owned and mutated exclusively by Compute. Source identity is weak: caching
// compiled rules must not pin the unmetered source JSON after an input releases.
class CompiledCatalogCache final {
public:
  explicit CompiledCatalogCache(std::shared_ptr<MemoryLedger> ledger) : ledger_(std::move(ledger)) {}
  std::shared_ptr<const CompiledCatalogEntry> find(const AnalysisWorkInput& input) {
    for (qsizetype index = entries_.size(); index-- > 0;) {
      if (entries_.at(index)->source.expired()) { entries_.removeAt(index); continue; }
      if (!entries_.at(index)->matches(input)) continue;
      auto hit = entries_.takeAt(index);
      entries_.append(hit);
      ++ledger_->cacheHits;
      return hit;
    }
    ++ledger_->cacheMisses;
    return {};
  }
  void trim(quint64 maximumBytes, qsizetype maximumEntries = 2) {
    while (!entries_.isEmpty() &&
        (ledger_->compiled.load() > maximumBytes || entries_.size() > maximumEntries))
      entries_.removeFirst();
  }
  void insert(std::shared_ptr<const CompiledCatalogEntry> entry) { entries_.append(std::move(entry)); }
private:
  std::shared_ptr<MemoryLedger> ledger_;
  QList<std::shared_ptr<const CompiledCatalogEntry>> entries_;
};

// The phase owns only frozen values and advances one material/good at a time.
// No singleton, clock read, repository callback or filesystem access occurs.
class CatalogPreparation final {
public:
  CatalogPreparation(const AnalysisWorkInput& input, AlgorithmPipelineStats* stats,
                     const CompiledShopCatalog* cached = nullptr)
      : input_(input), stats_(stats), resources_(input.resourceCounts, input.resourceCountsKnown,
          {}, input.conditionContext.shopSource, input.sourceInvalidated) {
    scratchBytes_ = sizeof(CatalogPreparation);
    if (cached) {
      compiled_ = *cached;
      externallyCharged_ = true;
      materialSection_ = 2;
      shop_ = input_.catalogSnapshot->allShops.size();
      stats_->goodsReused = cached->goods().size();
      ++stats_->compiledCatalogCacheHits;
    } else {
      names_.insert(QStringLiteral("134:1"), QStringLiteral("个人贡献币"));
      names_.insert(QStringLiteral("134:2"), QStringLiteral("联盟资金"));
      scratchBytes_ += 512;
    }
  }
  void step(const Cancellation& cancellation, int milliseconds, qsizetype workUnits, quint64 maximumBytes) {
    QElapsedTimer slice; slice.start();
    for (qsizetype unit = 0; unit < qMax<qsizetype>(1, workUnits); ++unit) {
      if (cancellation.requested.load(std::memory_order_acquire) || complete || rejected) break;
      QElapsedTimer atomic; atomic.start();
      const bool compiling = materialSection_ < 2 || shop_ < input_.catalogSnapshot->allShops.size();
      if (materialSection_ < 2) {
        const QString section = materialSection_ == 0 ? QStringLiteral("items") : QStringLiteral("money");
        const auto definitions = input_.materialDefinitions.value(section).toObject();
        if (materialIndex_ >= definitions.size()) { ++materialSection_; materialIndex_ = 0; }
        else {
          const auto entry = definitions.constBegin() + materialIndex_++;
          const QString key = (materialSection_ == 0 ? QStringLiteral("4:") : QStringLiteral("8:")) + entry.key();
          const QString name = entry.value().toObject().value(QStringLiteral("name")).toString();
          if (!name.isEmpty()) { names_.insert(key, name); scratchBytes_ += 128 + stringCharge(key) + stringCharge(name); }
        }
      } else if (shop_ < input_.catalogSnapshot->allShops.size()) {
        const auto& shop = input_.catalogSnapshot->allShops.at(shop_);
        if (good_ >= shop.goods.size()) { ++shop_; good_ = 0; }
        else {
          const auto& good = shop.goods.at(good_++);
          if (good.cost.size() > 65536 || good.enhanceType.size() > 65536 || good.raceIds.size() > 65536) rejected = true;
          else if (good.isOnlineOn(input_.catalogDate)) {
            compiled_.appendGood(good, names_, stats_);
          }
        }
      } else if (!preparing_) {
        const quint64 reserveBytes = quint64(compiled_.goods().size()) * sizeof(PreparedShopGoodConditions);
        if (bytes() > maximumBytes || reserveBytes > maximumBytes - bytes()) rejected = true;
        else {
          prepared.begin(compiled_, input_.conditionContext);
          scratchBytes_ += quint64(prepared.goods().capacity()) * sizeof(PreparedShopGoodConditions);
          preparing_ = true;
        }
      } else if (prepared.appendNext(input_.shopPacket, resources_, stats_)) {
        scratchBytes_ += preparedCharge(prepared.goods().last());
      } else complete = true;
      const qint64 elapsed = atomic.nsecsElapsed();
      if (compiling) compileNanoseconds += elapsed;
      else prepareNanoseconds += elapsed;
      maximumAtomicNanoseconds = qMax(maximumAtomicNanoseconds, elapsed);
      if (bytes() > maximumBytes) { rejected = true; break; }
      if (slice.nsecsElapsed() >= qBound(1, milliseconds, 16) * 1000000LL) break;
    }
    maximumSliceNanoseconds = qMax(maximumSliceNanoseconds, slice.nsecsElapsed());
  }
  PreparedShopConditions prepared;
  quint64 bytes() const { return scratchBytes_ + (externallyCharged_ ? 0 : compiled_.retainedBytes()); }
  const CompiledShopCatalog& compiled() const { return compiled_; }
  void chargedByCache() { externallyCharged_ = true; }
  bool complete = false, rejected = false;
  qint64 maximumSliceNanoseconds = 0, maximumAtomicNanoseconds = 0;
  qint64 compileNanoseconds = 0, prepareNanoseconds = 0;
private:
  const AnalysisWorkInput& input_;
  AlgorithmPipelineStats* stats_;
  AccountResourceView resources_;
  QHash<QString, QString> names_;
  CompiledShopCatalog compiled_;
  qsizetype materialIndex_ = 0, shop_ = 0, good_ = 0;
  int materialSection_ = 0;
  bool preparing_ = false;
  bool externallyCharged_ = false;
  quint64 scratchBytes_ = 0;
};

// Walk retained JSON values incrementally, charging 64 bytes per value plus
// UTF-16 strings and object keys. No full-document serialization or scan blocks
// the event loop; a large single pet may span many input-meter slices.
class InputMeter final {
public:
  explicit InputMeter(const AnalysisWorkInput& input) : input_(input), resource_(input.resourceCounts.cbegin()) {
    bytes = sizeof(AnalysisWorkInput) + stringCharge(input.key.account) +
        stringCharge(input.key.versions.build) + stringCharge(input.key.versions.profile) +
        stringCharge(input.overview.account) +
        quint64(input.overview.pets.capacity()) * sizeof(PetAssetRecord) +
        quint64(input.preparedFacts.capacity()) * sizeof(PetAnalysisFacts) +
        quint64(input.retainedRawPayloads.capacity()) * sizeof(QByteArray);
    if (!input.catalogSnapshot) bytes += input.conditions.catalog().retainedBytes() +
        quint64(input.conditions.goods().capacity()) * sizeof(PreparedShopGoodConditions);
  }

  bool step(const Cancellation& cancellation, int milliseconds, qsizetype workUnits) {
    QElapsedTimer elapsed;
    elapsed.start();
    for (qsizetype unit = 0; unit < qMax<qsizetype>(1, workUnits); ++unit) {
      if (cancellation.requested.load(std::memory_order_acquire)) break;
      if (!json_.isEmpty()) {
        walkJson();
      } else if (fact_ < input_.preparedFacts.size()) {
        const auto& fact = input_.preparedFacts.at(fact_++);
        if (!validatePetAnalysisFacts(fact) || factIdentities_.contains(fact.asset.instanceId)) {
          invalid = true;
          break;
        }
        factIdentities_.insert(fact.asset.instanceId);
        bytes += petAnalysisFactsRetainedBytes(fact) - sizeof(PetAnalysisFacts);
      } else if (pet_ < input_.overview.pets.size()) {
        const auto& pet = input_.overview.pets.at(pet_++);
        bytes += stringCharge(pet.name) + stringCharge(pet.location) +
                 stringsCharge(pet.gapKeys) + stringsCharge(pet.gaps);
        pushJson(pet.pet);
      } else if (input_.catalogSnapshot && shop_ < input_.catalogSnapshot->allShops.size()) {
        const auto& shop = input_.catalogSnapshot->allShops.at(shop_);
        if (rawGood_ < shop.goods.size()) bytes += rawGoodCharge(shop.goods.at(rawGood_++));
        else { bytes += sizeof(ShopExchangeShop) + stringCharge(shop.name) + stringCharge(shop.siKey); ++shop_; rawGood_ = 0; }
      } else if (good_ < input_.conditions.catalog().goods().size()) {
        const auto& prepared = input_.conditions.goods().at(good_++);
        bytes += preparedCharge(prepared);
      } else if (!metadata_) {
        metadata_ = true;
        const auto& context = input_.catalogSnapshot ? input_.conditionContext : input_.conditions.context();
        bytes += stringCharge(context.shopSource) + stringCharge(context.petSource);
        for (auto it = context.verifiedUnlockFacts.cbegin(); it != context.verifiedUnlockFacts.cend(); ++it)
          bytes += 128 + stringCharge(it.key()) + conditionCharge(it.value());
        for (auto it = context.quotaValidity.cbegin(); it != context.quotaValidity.cend(); ++it)
          bytes += 128 + stringCharge(it.key()) + conditionCharge(it.value());
        pushJson(input_.metadata.stargods);
        pushJson(input_.metadata.astrolabe);
        pushJson(input_.metadata.pets);
        pushJson(input_.metadata.sacredStarPlans);
        pushJson(input_.metadata.sacredStagePlans);
        pushJson(input_.metadata.badges);
      } else if (rawInputs_ < 3) {
        if (rawInputs_ == 0) {
          materialsStartBytes_ = bytes;
          pushJson(input_.materialDefinitions);
        } else if (rawInputs_ == 1) {
          materialDefinitionBytes = bytes - materialsStartBytes_;
          pushJson(input_.shopPacket);
        } else pushJson(input_.catalogSnapshot ? QJsonValue(input_.catalogSnapshot->root) : QJsonValue());
        ++rawInputs_;
      } else if (resource_ != input_.resourceCounts.cend()) {
        bytes += 128 + stringCharge(resource_.key()); ++resource_;
      } else if (raw_ < input_.retainedRawPayloads.size()) {
        const quint64 payloadBytes = input_.retainedRawPayloads.at(raw_++).size();
        bytes += payloadBytes;
        rawPayloadBytes += payloadBytes;
      } else {
        complete = true;
        json_.squeeze();
        factIdentities_.clear(); factIdentities_.squeeze();
        break;
      }
      if (elapsed.nsecsElapsed() >= qBound(1, milliseconds, 16) * 1000000LL) break;
    }
    const qint64 active = elapsed.nsecsElapsed();
    activeNanoseconds += active;
    maximumSliceNanoseconds = qMax(maximumSliceNanoseconds, active);
    return complete;
  }

  quint64 bytes = 0;
  quint64 rawPayloadBytes = 0;
  quint64 materialDefinitionBytes = 0;
  bool complete = false;
  bool invalid = false;
  qint64 maximumSliceNanoseconds = 0;
  qint64 activeNanoseconds = 0;
  quint64 chargedBytes() const {
    return bytes + quint64(json_.capacity()) * sizeof(Frame) + quint64(factIdentities_.size()) * 64 +
        quint64(factIdentities_.capacity()) * sizeof(qint64);
  }

private:
  struct Frame {
    QJsonValue value;
    QJsonObject object;
    QJsonArray array;
    qsizetype index = -1;
  };
  void pushJson(const QJsonValue& value) { Frame frame; frame.value = value; json_.append(frame); }
  void walkJson() {
    Frame& frame = json_.last();
    if (frame.index < 0) {
      bytes += 64;
      frame.index = 0;
      if (frame.value.isString()) {
        bytes += stringCharge(frame.value.toString());
        json_.removeLast();
      } else if (frame.value.isObject()) {
        frame.object = frame.value.toObject();
      } else if (frame.value.isArray()) {
        frame.array = frame.value.toArray();
      } else json_.removeLast();
      return;
    }
    if (frame.value.isObject()) {
      if (frame.index >= frame.object.size()) { json_.removeLast(); return; }
      // Qt 6 iterators retain the object's address. Rebuild after stack growth
      // instead of retaining an iterator to a QList frame that can move.
      const auto iterator = frame.object.constBegin() + frame.index++;
      const QString key = iterator.key();
      const QJsonValue child = iterator.value();
      bytes += 32 + stringCharge(key);
      pushJson(child);
    } else {
      if (frame.index >= frame.array.size()) { json_.removeLast(); return; }
      const QJsonValue child = frame.array.at(frame.index++);
      pushJson(child);
    }
  }

  const AnalysisWorkInput& input_;
  qsizetype pet_ = 0, fact_ = 0, good_ = 0, raw_ = 0, shop_ = 0, rawGood_ = 0;
  bool metadata_ = false;
  int rawInputs_ = 0;
  quint64 materialsStartBytes_ = 0;
  QHash<QString, qint64>::const_iterator resource_;
  QList<Frame> json_;
  QSet<qint64> factIdentities_;
};

}  // namespace

struct AnalysisCompletion {
  AnalysisJobFinished finished;
  std::shared_ptr<const AnalysisWorkResult> result;
};
Q_DECLARE_METATYPE(AnalysisCompletion)

class AnalysisComputeObject final : public QObject {
public:
  using Completion = std::function<void(AnalysisCompletion)>;
  AnalysisComputeObject(const AnalysisMemoryLimits& limits,
                        std::shared_ptr<MemoryLedger> ledger, Completion completion)
      : limits_(limits), ledger_(std::move(ledger)), completion_(std::move(completion)), cache_(ledger_) {
    // This object and its timer are constructed by QThread::run in Compute.
    pulse_ = new QTimer(this);
    pulse_->setInterval(16);
    QObject::connect(pulse_, &QTimer::timeout, this, [this] { ++ledger_->turns; });
  }
  ~AnalysisComputeObject() override {
    // quit() can end the event loop between slices. Preserve the same input
    // release ordering and report closure without relying on another tick.
    if (input_) finish(AnalysisJobOutcome::Closed);
  }
  bool event(QEvent* event) override {
    if (event->type() == PriorityEvent::eventType()) {
      static_cast<PriorityEvent*>(event)->execute();
      return true;
    }
    return QObject::event(event);
  }

  void start(const AnalysisJobKey& key, quint64 generation,
             std::shared_ptr<const AnalysisWorkInput> input,
             std::shared_ptr<Cancellation> cancellation,
             std::shared_ptr<MemoryLease> inputLease) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!input_);
    key_ = key;
    generation_ = generation;
    input_ = std::move(input);
    cancellation_ = std::move(cancellation);
    inputLease_ = std::move(inputLease);
    try {
      resultLease_ = std::make_shared<MemoryLease>(ledger_, MemoryLease::Kind::Result);
      candidateLease_ = std::make_shared<MemoryLease>(ledger_, MemoryLease::Kind::Candidate);
      stats_ = {};
      ledger_->pairs.store(0);
      meter_ = std::make_unique<InputMeter>(*input_);
      pulse_->start();
      schedule();
    } catch (...) { finish(AnalysisJobOutcome::InputRejected); }
  }

private:
  void schedule() { QTimer::singleShot(0, this, [this] { tick(); }); }

  void tick() {
    if (!input_) return;
    ++ledger_->slices;
    try {
      if (cancellation_->requested.load(std::memory_order_acquire)) {
        finish(AnalysisJobOutcome::Cancelled); return;
      }
      if (!meter_->complete) {
        meter_->step(*cancellation_, limits_.sliceMilliseconds, limits_.sliceWorkUnits);
        if (meter_->invalid) { finish(AnalysisJobOutcome::InputRejected); return; }
        if (meter_->chargedBytes() > limits_.inputBytes) {
          finish(AnalysisJobOutcome::BudgetExceeded); return;
        }
        if (cancellation_->requested.load(std::memory_order_acquire)) {
          finish(AnalysisJobOutcome::Cancelled); return;
        }
        if (meter_->complete) {
          inputLease_->resize(meter_->bytes);
          if (input_->catalogSnapshot) {
            if (limits_.compiledCatalogBytes) activeCatalog_ = cache_.find(*input_);
            preparation_ = std::make_unique<CatalogPreparation>(*input_, &stats_,
                activeCatalog_ ? &activeCatalog_->catalog : nullptr);
          }
          else session_ = std::make_unique<RecommendationSession>(key_.account, input_->overview,
              input_->conditions, input_->metadata, &stats_, input_->deriveOverview,
              input_->usePreparedFacts ? &input_->preparedFacts : nullptr);
        }
        schedule();
        return;
      }
      if (preparation_ && !preparation_->complete) {
        const quint64 otherBytes = ledger_->total() - inputLease_->bytes();
        const quint64 jobBudget = otherBytes < limits_.totalBytes
            ? qMin(limits_.inputBytes, limits_.totalBytes - otherBytes) : 0;
        if (meter_->bytes >= jobBudget) { finish(AnalysisJobOutcome::BudgetExceeded); return; }
        preparation_->step(*cancellation_, limits_.sliceMilliseconds, limits_.sliceWorkUnits, jobBudget - meter_->bytes);
        const quint64 charged = meter_->bytes + preparation_->bytes();
        inputLease_->resize(charged);
        if (preparation_->rejected || charged > limits_.inputBytes || ledger_->total() > limits_.totalBytes) {
          finish(AnalysisJobOutcome::BudgetExceeded); return;
        }
        if (cancellation_->requested.load(std::memory_order_acquire)) { finish(AnalysisJobOutcome::Cancelled); return; }
        if (preparation_->complete && preparation_->prepared.goods().size() != preparation_->prepared.catalog().goods().size()) {
          finish(AnalysisJobOutcome::InputRejected); return;
        }
        if (preparation_->complete) {
          if (!activeCatalog_) retainCompiledCatalog();
          session_ = std::make_unique<RecommendationSession>(key_.account, input_->overview,
              preparation_->prepared, input_->metadata, &stats_, input_->deriveOverview,
              input_->usePreparedFacts ? &input_->preparedFacts : nullptr);
        }
        schedule(); return;
      }
      const quint64 previousResults = ledger_->results.load() - resultLease_->bytes();
      const quint64 reserved = inputLease_->bytes() + previousResults + limits_.candidateBytes + ledger_->compiled.load();
      const quint64 available = reserved < limits_.totalBytes ? limits_.totalBytes - reserved : 0;
      const auto status = session_->step(&cancellation_->requested, limits_.sliceMilliseconds,
          limits_.sliceWorkUnits, qMin(limits_.resultBytes, available));
      const auto& slices = session_->sliceStats();
      ledger_->pairs.store(stats_.candidatePairsVisited);
      // Include transient rows even when the same slice discovers a budget
      // violation and clears its partial result before returning.
      atomicMaximum(ledger_->peakResults, previousResults + slices.peakResultChargedBytes);
      atomicMaximum(ledger_->peak, inputLease_->bytes() + previousResults + ledger_->compiled.load() +
          slices.peakResultChargedBytes + slices.peakCandidateChargedBytes);
      resultLease_->resize(slices.resultChargedBytes);
      candidateLease_->resize(slices.peakCandidateChargedBytes);
      if (slices.peakCandidateChargedBytes > limits_.candidateBytes ||
          ledger_->total() > limits_.totalBytes ||
          status == RecommendationSession::Status::ResultBudgetExceeded) {
        finish(AnalysisJobOutcome::BudgetExceeded); return;
      }
      if (status == RecommendationSession::Status::Cancelled) {
        finish(AnalysisJobOutcome::Cancelled); return;
      }
      if (status == RecommendationSession::Status::InvalidInput) {
        finish(AnalysisJobOutcome::InputRejected); return;
      }
      if (status == RecommendationSession::Status::Complete) {
        finish(AnalysisJobOutcome::Published); return;
      }
      schedule();
    } catch (...) {
      finish(AnalysisJobOutcome::InputRejected);
    }
  }

  void retainCompiledCatalog() {
    const quint64 compiledBytes = preparation_->compiled().retainedBytes();
    const quint64 bytes = sizeof(CompiledCatalogEntry) + 128 + compiledBytes + meter_->materialDefinitionBytes;
    const quint64 maximum = qMin(limits_.compiledCatalogBytes, 16ULL * 1024 * 1024);
    if (bytes > maximum) return;
    // Only completed catalog/account preparation is eligible. No partial
    // compiler or cancelled source is published into the reusable cache.
    cache_.trim(maximum - bytes, 1);
    const quint64 afterTransfer = ledger_->total() - compiledBytes + bytes;
    if (afterTransfer > limits_.totalBytes) return;
    auto entry = std::make_shared<CompiledCatalogEntry>();
    entry->retention = std::make_shared<MemoryLease>(ledger_, MemoryLease::Kind::CompiledCatalog);
    entry->source = input_->catalogSnapshot;
    entry->sourceRevision = input_->catalogSnapshot->revision;
    entry->catalogRevision = input_->key.versions.catalog;
    entry->metadataRevision = input_->key.versions.metadata;
    entry->analysisVersion = input_->key.versions.analysis;
    entry->date = input_->catalogDate;
    entry->materials = input_->materialDefinitions;
    entry->catalog = preparation_->compiled();
    preparation_->chargedByCache();
    inputLease_->resize(meter_->bytes + preparation_->bytes());
    entry->retention->resize(bytes);
    activeCatalog_ = entry;
    cache_.insert(std::move(entry));
  }

  void finish(AnalysisJobOutcome outcome) {
    pulse_->stop();
    const RecommendationSliceStats sliceStats = session_ ? session_->sliceStats()
                                                         : RecommendationSliceStats{};
    const qint64 meterSlice = meter_ ? meter_->maximumSliceNanoseconds : 0;
    const quint64 measuredBytes = meter_ ? meter_->bytes + (preparation_ ? preparation_->bytes() : 0) : 0;
    const bool measurementComplete = meter_ && meter_->complete;
    std::shared_ptr<AnalysisWorkResult> result;
    if (outcome == AnalysisJobOutcome::Published) {
      const quint64 wrapperBytes = sizeof(AnalysisWorkResult) + stringCharge(key_.account);
      if (resultLease_->bytes() + wrapperBytes > limits_.resultBytes ||
          ledger_->total() + wrapperBytes > limits_.totalBytes) {
        outcome = AnalysisJobOutcome::BudgetExceeded;
      } else {
        resultLease_->resize(resultLease_->bytes() + wrapperBytes);
        result = std::make_shared<AnalysisWorkResult>();
        result->key = key_;
        result->generation = generation_;
        result->recommendations = session_->takeResults();
        for (auto& recommendation : result->recommendations)
          recommendation.memoryRetention = retainResultWithSource(recommendation.memoryRetention,resultLease_);
        if (input_->deriveOverview || input_->usePreparedFacts) {
          result->overview = session_->takeOverview();
          result->overview.memoryRetention = resultLease_;
          for (auto& pet : result->overview.pets)
            pet.memoryRetention = retainResultWithSource(pet.memoryRetention,resultLease_);
        }
        result->work = stats_;
        result->slices = session_->sliceStats();
        result->measuredInputBytes = measuredBytes;
        result->maximumPreparationSliceNanoseconds = preparation_ ? preparation_->maximumSliceNanoseconds : 0;
        result->maximumAtomicPreparationNanoseconds = preparation_ ? preparation_->maximumAtomicNanoseconds : 0;
        result->retainedRawPayloadBytes = meter_->rawPayloadBytes;
        result->maximumInputMeterSliceNanoseconds = meter_->maximumSliceNanoseconds;
        result->catalogCacheHit = stats_.compiledCatalogCacheHits > 0;
        result->activeInputMeterNanoseconds = meter_->activeNanoseconds;
        result->catalogCompileNanoseconds = preparation_ ? preparation_->compileNanoseconds : 0;
        result->conditionPrepareNanoseconds = preparation_ ? preparation_->prepareNanoseconds : 0;
        result->candidateComputeNanoseconds = sliceStats.activeNanoseconds - sliceStats.sortNanoseconds;
        result->sortNanoseconds = sliceStats.sortNanoseconds;
        result->memoryRetention = resultLease_;
      }
    }
    const auto cancellation = cancellation_;
    // This is the release barrier: no completion is sent while any Compute
    // input/session/JSON-walker reference remains. Core may capture next only
    // after receiving the completion below.
    session_.reset();
    preparation_.reset();
    activeCatalog_.reset();
    meter_.reset();
    input_.reset();
    inputLease_.reset();
    candidateLease_.reset();
    resultLease_.reset();
    cancellation_.reset();
    // Reserve room for the next full input capture while idle. Eviction occurs
    // after this job's final compiled reference releases; all in-flight leases
    // remain charged even when their cache entry has been removed.
    const quint64 reserved = limits_.inputBytes + ledger_->results.load();
    cache_.trim(reserved < limits_.totalBytes
        ? qMin(limits_.compiledCatalogBytes, limits_.totalBytes - reserved) : 0);
    const qint64 latency = cancellation ? cancellation->latency() : 0;
    if (result) result->cancellationLatencyNanoseconds = latency;
    completion_({{key_, generation_, outcome, latency, sliceStats,
                  meterSlice, measuredBytes, measurementComplete}, result});
  }

  AnalysisMemoryLimits limits_;
  std::shared_ptr<MemoryLedger> ledger_;
  Completion completion_;
  CompiledCatalogCache cache_;
  std::shared_ptr<const CompiledCatalogEntry> activeCatalog_;
  QTimer* pulse_ = nullptr;
  AnalysisJobKey key_;
  quint64 generation_ = 0;
  std::shared_ptr<const AnalysisWorkInput> input_;
  std::shared_ptr<Cancellation> cancellation_;
  std::unique_ptr<CatalogPreparation> preparation_;
  std::shared_ptr<MemoryLease> inputLease_, resultLease_, candidateLease_;
  std::unique_ptr<InputMeter> meter_;
  std::unique_ptr<RecommendationSession> session_;
  AlgorithmPipelineStats stats_;
};

class AnalysisComputeThread final : public QThread {
  Q_OBJECT
public:
  AnalysisComputeThread(const AnalysisMemoryLimits& limits, std::shared_ptr<MemoryLedger> ledger)
      : limits_(limits), ledger_(std::move(ledger)) {}
signals:
  void initialized(AnalysisComputeObject* object);
  void calculationFinished(AnalysisCompletion completion);
protected:
  void run() override {
    AnalysisComputeObject worker(limits_, ledger_,
        [this](AnalysisCompletion result) { emit calculationFinished(std::move(result)); });
    emit initialized(&worker);
    exec();
  }
private:
  AnalysisMemoryLimits limits_;
  std::shared_ptr<MemoryLedger> ledger_;
};

struct AnalysisWorker::Impl {
  AnalysisWorker* owner;
  AnalysisMemoryLimits limits;
  std::shared_ptr<MemoryLedger> ledger = std::make_shared<MemoryLedger>();
  AnalysisComputeThread* thread = nullptr;
  AnalysisComputeObject* compute = nullptr;
  InputFactory factory;
  PublicationGuard guard;
  ResultHandler resultHandler;
  FinishedHandler finishedHandler;
  struct Descriptor { AnalysisJobKey key; quint64 generation = 0; };
  std::optional<Descriptor> latest;
  std::optional<Descriptor> active;
  std::shared_ptr<Cancellation> cancellation;
  std::shared_ptr<const AnalysisWorkResult> displayed;
  QList<ActionRecommendation> projectionSource, projection;
  bool projectionValid = false, projectionCultivationStale = false, projectionShopStale = false;
  quint64 generation = 0;
  bool closing = false;
  bool pumpQueued = false;
  std::shared_ptr<PriorityState> priority = std::make_shared<PriorityState>();
  std::unique_ptr<PriorityEvent> pendingPriority;

  Impl(AnalysisWorker* object, const AnalysisMemoryLimits& policy) : owner(object), limits(policy) {
    limits.sliceMilliseconds = qBound(1, limits.sliceMilliseconds, 16);
    limits.sliceWorkUnits = qMax<qsizetype>(1, limits.sliceWorkUnits);
    qRegisterMetaType<AnalysisCompletion>();
    thread = new AnalysisComputeThread(limits, ledger);
    QObject::connect(thread, &AnalysisComputeThread::initialized, owner,
        [this](AnalysisComputeObject* worker) {
          if (closing) return;
          compute = worker;
          if (pendingPriority) QCoreApplication::postEvent(compute, pendingPriority.release(), Qt::HighEventPriority);
          schedulePump();
        }, Qt::QueuedConnection);
    QObject::connect(thread, &AnalysisComputeThread::calculationFinished, owner,
        [this](AnalysisCompletion completion) { receive(std::move(completion)); },
        Qt::QueuedConnection);
    QObject::connect(thread, &QThread::finished, owner, [this] { compute = nullptr; },
                     Qt::QueuedConnection);
    thread->start();
  }

  void assertCore() const { Q_ASSERT(QThread::currentThread() == owner->thread()); }

  void schedulePump() {
    if (pumpQueued || closing) return;
    pumpQueued = true;
    QTimer::singleShot(0, owner, [this] { pumpQueued = false; pump(); });
  }

  bool notify(const Descriptor& descriptor, AnalysisJobOutcome outcome, qint64 latency = 0,
              const AnalysisJobFinished* diagnostics = nullptr) {
    QPointer<AnalysisWorker> alive(owner);
    const FinishedHandler handler = finishedHandler;
    AnalysisJobFinished finished = diagnostics ? *diagnostics : AnalysisJobFinished{};
    finished.key = descriptor.key;
    finished.generation = descriptor.generation;
    finished.outcome = outcome;
    finished.cancellationLatencyNanoseconds = latency;
    if (handler) handler(finished);
    return !alive.isNull();
  }

  bool validLimits() const {
    constexpr quint64 maximum = 1ULL << 40;
    return limits.inputBytes > 0 && limits.resultBytes > 0 && limits.candidateBytes > 0 &&
        limits.inputBytes <= maximum && limits.resultBytes <= maximum &&
        limits.candidateBytes <= maximum && limits.totalBytes <= maximum &&
        limits.totalBytes >= limits.inputBytes;
  }

  void pump() {
    assertCore();
    if (closing || active || !latest || !compute) return;
    const Descriptor descriptor = *latest;
    latest.reset();
    if (!validLimits() || ledger->total() > limits.totalBytes ||
        limits.inputBytes > limits.totalBytes - ledger->total()) {
      notify(descriptor, AnalysisJobOutcome::BudgetExceeded);
      return;
    }
    auto inputLease = std::make_shared<MemoryLease>(ledger, MemoryLease::Kind::Input,
                                                   limits.inputBytes);
    active = descriptor;
    cancellation = std::make_shared<Cancellation>();
    const auto token = cancellation;
    std::shared_ptr<const AnalysisWorkInput> input;
    QPointer<AnalysisWorker> alive(owner);
    const InputFactory capture = factory;
    try {
      if (capture) input = capture(descriptor.key);
    } catch (...) {
      if (!alive) return;
      active.reset();
      cancellation.reset();
      inputLease.reset();
      if (!notify(descriptor, AnalysisJobOutcome::FactoryFailed)) return;
      schedulePump();
      return;
    }
    if (!alive) return;
    if (input) ++ledger->captured;
    if (closing || generation != descriptor.generation || token->requested.load()) {
      input.reset();
      inputLease.reset();
      active.reset();
      cancellation.reset();
      if (!notify(descriptor, closing ? AnalysisJobOutcome::Closed : AnalysisJobOutcome::Superseded,
                  token->latency())) return;
      schedulePump();
      return;
    }
    if (!input || (!input->catalogSnapshot && input->conditions.goods().size() != input->conditions.catalog().goods().size()) ||
        (input->usePreparedFacts && (!input->overview.pets.isEmpty() || !input->retainedRawPayloads.isEmpty() ||
            input->overview.totalPets != input->preparedFacts.size())) ||
        (!input->usePreparedFacts && !input->preparedFacts.isEmpty()) ||
        !(input->key == descriptor.key) ||
        input->overview.account != descriptor.key.account ||
        input->overview.inputSessionEpoch != descriptor.key.epoch ||
        input->overview.inventoryRevision != descriptor.key.versions.inventory ||
        input->overview.analysisVersion != descriptor.key.versions.analysis) {
      input.reset();
      inputLease.reset();
      active.reset();
      cancellation.reset();
      if (!notify(descriptor, AnalysisJobOutcome::InputRejected)) return;
      schedulePump();
      return;
    }
    const bool queued = QMetaObject::invokeMethod(compute,
        [worker = compute, key = descriptor.key, version = descriptor.generation,
         input = std::move(input), token, inputLease = std::move(inputLease)]() mutable {
          worker->start(key, version, std::move(input), token, std::move(inputLease));
        }, Qt::QueuedConnection);
    if (!queued) {
      active.reset();
      cancellation.reset();
      if (!notify(descriptor, AnalysisJobOutcome::InputRejected)) return;
      schedulePump();
    }
  }

  void receive(AnalysisCompletion completion) {
    assertCore();
    if (!active || active->generation != completion.finished.generation ||
        !(active->key == completion.finished.key)) return;
    const Descriptor descriptor = *active;
    const auto token = cancellation;
    auto outcome = completion.finished.outcome;
    if (closing) outcome = AnalysisJobOutcome::Closed;
    else if (generation != descriptor.generation) outcome = AnalysisJobOutcome::Superseded;
    else if (token && token->requested.load()) outcome = AnalysisJobOutcome::Cancelled;
    else if (outcome == AnalysisJobOutcome::Published) {
      // The guard is deliberately fail-closed until Core installs its current
      // session/version/freshness predicate. Recheck after a reentrant guard.
      QPointer<AnalysisWorker> alive(owner);
      const PublicationGuard check = guard;
      bool current = false;
      try { current = check && check(descriptor.key); }
      catch (...) { current = false; }
      if (!alive) return;
      if (closing) outcome = AnalysisJobOutcome::Closed;
      else if (generation != descriptor.generation) outcome = AnalysisJobOutcome::Superseded;
      else if (token && token->requested.load()) outcome = AnalysisJobOutcome::Cancelled;
      else if (!current) outcome = AnalysisJobOutcome::Stale;
      else {
        atomicMaximum(ledger->overlap, ledger->results.load());
        displayed = completion.result;
        const ResultHandler handler = resultHandler;
        if (handler) handler(displayed);
        if (!alive) return;
      }
    }
    active.reset();
    cancellation.reset();
    if (!notify(descriptor, outcome, completion.finished.cancellationLatencyNanoseconds,
                &completion.finished)) return;
    schedulePump();
  }

  bool shutdown(int maximumMilliseconds) {
    assertCore();
    closing = true;
    priority->closing.store(true, std::memory_order_release);
    pendingPriority.reset();
    latest.reset();
    if (cancellation) cancellation->cancel();
    if (!thread) return true;
    auto* stopping = thread;
    thread = nullptr;
    compute = nullptr;
    // Install before waiting: finished can race with a zero-duration wait.
    QObject::connect(stopping, &QThread::finished, stopping, &QObject::deleteLater,
                     Qt::QueuedConnection);
    stopping->quit();
    const bool finished = stopping->wait(qBound(0, maximumMilliseconds, 2000));
    if (finished) delete stopping;
    return finished;
  }
};

AnalysisWorker::AnalysisWorker(const AnalysisMemoryLimits& limits, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, limits)) {}

AnalysisWorker::~AnalysisWorker() { impl_->shutdown(2000); }

bool AnalysisWorker::postPriority(std::function<void()> job) {
  impl_->assertCore();
  if (!job || impl_->closing || impl_->priority->busy.exchange(true, std::memory_order_acq_rel)) return false;
  try {
    auto event = std::make_unique<PriorityEvent>(impl_->priority, std::move(job));
    if (impl_->compute) QCoreApplication::postEvent(impl_->compute, event.release(), Qt::HighEventPriority);
    else impl_->pendingPriority = std::move(event);
  } catch (...) {
    impl_->priority->busy.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void AnalysisWorker::setInputFactory(InputFactory factory) {
  impl_->assertCore(); impl_->factory = std::move(factory);
}
void AnalysisWorker::setPublicationGuard(PublicationGuard guard) {
  impl_->assertCore(); impl_->guard = std::move(guard);
}
void AnalysisWorker::setResultHandler(ResultHandler handler) {
  impl_->assertCore(); impl_->resultHandler = std::move(handler);
}
void AnalysisWorker::setFinishedHandler(FinishedHandler handler) {
  impl_->assertCore(); impl_->finishedHandler = std::move(handler);
}

quint64 AnalysisWorker::submit(const AnalysisJobKey& key) {
  impl_->assertCore();
  if (impl_->closing || key.jobId == 0 || key.account.isEmpty() || key.epoch == 0 ||
      key.versions.analysis != AssetAnalysisVersion::kCurrentAnalysis ||
      impl_->generation == std::numeric_limits<quint64>::max()) return 0;
  ++impl_->generation;
  impl_->latest = Impl::Descriptor{key, impl_->generation};
  if (impl_->cancellation) impl_->cancellation->cancel();
  impl_->schedulePump();
  return impl_->generation;
}

void AnalysisWorker::cancel() {
  impl_->assertCore();
  impl_->latest.reset();
  if (impl_->cancellation) impl_->cancellation->cancel();
}

bool AnalysisWorker::busy() const { impl_->assertCore(); return bool(impl_->active); }
bool AnalysisWorker::hasPendingDescriptor() const { impl_->assertCore(); return bool(impl_->latest); }

std::shared_ptr<const AnalysisWorkResult> AnalysisWorker::lastResult() const {
  impl_->assertCore(); return impl_->displayed;
}

AnalysisMemoryUsage AnalysisWorker::memoryUsage() const {
  impl_->assertCore(); return impl_->ledger->snapshot();
}

QList<ActionRecommendation> AnalysisWorker::freshnessProjection(
    const QList<ActionRecommendation>& rows, bool cultivationStale, bool shopStale, bool* budgetExceeded) const {
  impl_->assertCore();
  if (budgetExceeded) *budgetExceeded = false;
  if (impl_->projectionValid && impl_->projectionSource.constData() == rows.constData() &&
      impl_->projectionSource.size() == rows.size() &&
      impl_->projectionCultivationStale == cultivationStale && impl_->projectionShopStale == shopStale)
    return impl_->projection;
  impl_->projectionValid = false;
  impl_->projection.clear(); impl_->projectionSource.clear();
  quint64 reservedForView = 0;
  bool exceeded = false;
  const auto reserve = [&](quint64 bytes) -> std::shared_ptr<void> {
    // New projections are Core work. Never race a Compute step's available
    // result allowance: existing/fresh projections still return while busy.
    if (impl_->closing || impl_->active || impl_->latest || !impl_->validLimits() ||
        reservedForView > impl_->limits.resultBytes || bytes > impl_->limits.resultBytes - reservedForView ||
        impl_->ledger->total() > impl_->limits.totalBytes || bytes > impl_->limits.totalBytes - impl_->ledger->total())
      return {};
    auto lease = std::make_shared<MemoryLease>(impl_->ledger,MemoryLease::Kind::Result,bytes);
    reservedForView += bytes;
    return lease;
  };
  QList<ActionRecommendation> projected;
  try {
    projected = PreparedRecommendationEngine::applyFreshness(rows,cultivationStale,shopStale,reserve,&exceeded);
  } catch (...) { exceeded = true; projected.clear(); }
  if (budgetExceeded) *budgetExceeded = exceeded;
  if (exceeded) return {};
  impl_->projectionSource = rows; impl_->projection = projected;
  impl_->projectionCultivationStale = cultivationStale; impl_->projectionShopStale = shopStale;
  impl_->projectionValid = true;
  return projected;
}

bool AnalysisWorker::shutdown(int maximumWaitMilliseconds) {
  return impl_->shutdown(maximumWaitMilliseconds);
}

#include "analysis_worker.moc"
