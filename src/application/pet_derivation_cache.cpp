#include "pet_derivation_cache.h"
#include "../domain/asset_derivation.h"
#include "../domain/checked_json_numbers.h"
#include "../domain/pet_identity.h"
#include "protocol_transport.h"
#include "storage_service.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <deque>
#include <list>
#include <limits>

namespace PetDerivationInternal {
struct State {
  QMutex delivery;
  PetDerivationCache* receiver = nullptr;
  std::atomic_bool closing{false}, notificationQueued{false};
  std::atomic<quint64> inputBytes{0}, metadataBytes{0}, factsBytes{0}, reservedBytes{0}, liveLeases{0};
  std::atomic<quint64> peakInput{0}, peakFacts{0};
  std::atomic<int> posted{0};
  std::atomic<qint64> completedComputeUs{0};
  std::atomic<qint64> completedIndexDecodeUs{0}, completedRawDeriveUs{0};
  quintptr ownerThreadId = 0;
  static void peak(std::atomic<quint64>& destination, quint64 value) {
    quint64 old = destination.load();
    while (old < value && !destination.compare_exchange_weak(old, value)) {}
  }
  void notify() {
    if (closing.load() || notificationQueued.exchange(true)) return;
    QMutexLocker guard(&delivery);
    if (!receiver) { notificationQueued.store(false); return; }
    auto* target = receiver;
    QMetaObject::invokeMethod(target, [target] { target->budgetReleased(); }, Qt::QueuedConnection);
  }
  void deliver(std::shared_ptr<Completion> completion);
};
struct Metadata {
  std::shared_ptr<State> state;
  ShopPetMetadataSnapshot value;
  quint64 revision = 0, bytes = 0;
  QByteArray digest;
  ~Metadata() { value = {}; state->metadataBytes.fetch_sub(bytes); state->notify(); }
};
struct Input {
  std::shared_ptr<State> state;
  RawPetRecordHandle raw;
  PetAssetRecord seed;
  std::shared_ptr<const Metadata> metadata;
  quint64 bytes = 0;
  ~Input() {
    raw.reset(); seed = {}; metadata.reset();
    state->inputBytes.fetch_sub(bytes); state->notify();
  }
};
struct Reservation {
  std::shared_ptr<State> state;
  std::atomic<quint64> bytes{0};
  void release() { state->reservedBytes.fetch_sub(bytes.exchange(0)); state->notify(); }
  ~Reservation() { release(); }
};
struct FactLease {
  std::shared_ptr<State> state;
  quint64 bytes = 0;
  ~FactLease() {
    state->factsBytes.fetch_sub(bytes); state->liveLeases.fetch_sub(1); state->notify();
  }
};
struct Posted {
  std::shared_ptr<State> state;
  ~Posted() { state->posted.fetch_sub(1); state->notify(); }
};
struct IndexScratch {
  std::shared_ptr<State> state;
  quint64 bytes = 0;
  ~IndexScratch() { state->inputBytes.fetch_sub(bytes); state->notify(); }
};
struct Work {
  quint64 id = 0;
  PetDerivationKey key;
  std::shared_ptr<Input> input;
  std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
  // Digest may arrive after computation started without changing memRev.
  QByteArray sourceDigest;
  bool persistable = false;
  StorageContext storageContext;
  bool indexAttempted = false, computePosted = false;
  quint64 readId = 0;
  QByteArray indexBytes;
  std::shared_ptr<IndexScratch> indexScratch;
  std::shared_ptr<Reservation> reservation;
  ~Work() { indexBytes.clear(); indexScratch.reset(); }
};
struct Completion {
  quint64 id = 0;
  PetDerivationKey key;
  PetDerivationStatus status = PetDerivationStatus::DeriveFailed;
  QString error;
  std::optional<PetAnalysisFacts> facts;
  std::shared_ptr<Reservation> reservation;
  qint64 elapsedUs = 0;
  QByteArray factsDigest;
  QByteArray indexSourceDigest;
  bool derived = false, indexHit = false, indexRejected = false;
};
struct ComputeSubspan {
  std::atomic<qint64>& total;
  QElapsedTimer timer;
  explicit ComputeSubspan(std::atomic<qint64>& destination) : total(destination) { timer.start(); }
  ~ComputeSubspan() { total.fetch_add(timer.nsecsElapsed() / 1000); }
};
void State::deliver(std::shared_ptr<Completion> completion) {
  QMutexLocker guard(&delivery);
  if (!receiver || closing.load()) return;
  auto* target = receiver;
  QMetaObject::invokeMethod(target, [target, completion = std::move(completion)] { target->receive(completion); }, Qt::QueuedConnection);
}

namespace {
bool validKey(const PetDerivationKey& key) {
  // Epoch zero is the local last-account snapshot before login. Its exact key
  // still isolates callbacks from a later authenticated epoch.
  return !key.record.account.isEmpty() && key.record.account.size() <= 1024 &&
      key.record.instanceId > 0 && key.record.detailMemoryRevision > 0 && key.metadataRevision > 0 &&
      (key.metadataDigest.isEmpty() || key.metadataDigest.size() == 32) &&
      key.analysisVersion == AssetAnalysisVersion::kCurrentAnalysis;
}
bool add(quint64* sum, quint64 amount, quint64 maximum) {
  if (*sum > maximum || amount > maximum - *sum) return false;
  *sum += amount; return true;
}
quint64 textBytes(const QString& text) { return 64 + quint64(qMax(text.size(), text.capacity())) * sizeof(QChar); }
quint64 recordOverhead(const PetDerivationKey& key) {
  return sizeof(PetDerivedFactsRecord) + 256 + textBytes(key.record.account) +
      quint64(qMax(key.metadataDigest.size(), key.metadataDigest.capacity()));
}
bool valueBytes(const QJsonValue& value, quint64* bytes, quint64 maximum, int depth = 0) {
  if (depth > 32 || !add(bytes, 256, maximum)) return false;
  if (value.isString()) return add(bytes, textBytes(value.toString()), maximum);
  if (value.isObject()) {
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
      if (!add(bytes, textBytes(it.key()), maximum) || !valueBytes(it.value(), bytes, maximum, depth + 1)) return false;
  } else if (value.isArray()) {
    const auto array = value.toArray();
    for (const auto& child : array) if (!valueBytes(child, bytes, maximum, depth + 1)) return false;
  }
  return true;
}
bool rawIdentity(const RawPetRecord& raw, QString* error) {
  const auto validId = [&](const QJsonObject& object, bool required) {
    if (!object.contains(QStringLiteral("id"))) return !required;
    qint64 value = 0;
    return DomainNumeric::checkedInteger(object.value(QStringLiteral("id")), &value, 1) && value == raw.key.instanceId;
  };
  if (!validId(raw.object(), raw.complete) || !validId(raw.brief, false)) {
    *error = QStringLiteral("raw/brief instance identity contradicts its frozen record key"); return false;
  }
  return true;
}
bool sameCalculationSeed(const PetAssetRecord& a, const PetAssetRecord& b) {
  if (a.instanceId != b.instanceId || a.raceId != b.raceId || a.metadataSlotMaxLevel != b.metadataSlotMaxLevel ||
      a.detailAvailable != b.detailAvailable) return false;
  for (const QString& key : {QStringLiteral("id"), QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("fr"),
       QStringLiteral("lv"), QStringLiteral("_metaRaceId")})
    if (a.pet.value(key) != b.pet.value(key)) return false;
  return true;
}
QJsonObject calculationIdentity(const QJsonObject& identity) {
  QJsonObject result;
  for (const QString& key : {QStringLiteral("id"), QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("fr"),
       QStringLiteral("lv"), QStringLiteral("_metaRaceId")})
    if (identity.contains(key)) result.insert(key, identity.value(key));
  return result;
}
PetAssetRecord smallSeed(const PetAssetRecord& source, bool verified) {
  PetAssetRecord result;
  result.instanceId = source.instanceId; result.raceId = source.raceId;
  result.metadataSlotMaxLevel = source.metadataSlotMaxLevel;
  result.detailAvailable = source.detailAvailable; result.observationVerified = verified;
  result.name = source.name; result.location = source.location;
  result.pet = source.pet;
  return result;
}
QString indexPath(qint64 instanceId) { return QStringLiteral("derived/pets/%1.json").arg(instanceId); }
bool indexContextValid(StorageService* storage, const PetDerivationKey& key, const StorageContext& context) {
  return storage && context && !context->isShared() && !context->isReadOnly() &&
      context->account() == key.record.account && context->dataRoot() == storage->dataRoot();
}
std::optional<PetAnalysisFacts> readIndex(const QByteArray& bytes, const PetDerivationKey& key,
    const QByteArray& sourceDigest, const PetAssetRecord& seed, QString* error) {
  const auto fail = [error](const QString& reason) -> std::optional<PetAnalysisFacts> { *error = reason; return {}; };
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(QStringLiteral("invalid index JSON"));
  const auto envelope = document.object();
  qint64 schema = 0, algorithm = 0, instance = 0;
  if (!DomainNumeric::checkedInteger(envelope.value(QStringLiteral("schema")), &schema, 1, 1) ||
      !DomainNumeric::checkedInteger(envelope.value(QStringLiteral("algorithm")), &algorithm, key.analysisVersion, key.analysisVersion) ||
      !DomainNumeric::checkedInteger(envelope.value(QStringLiteral("instanceId")), &instance, 1) || instance != key.record.instanceId ||
      envelope.value(QStringLiteral("account")).toString() != key.record.account ||
      envelope.value(QStringLiteral("sourceProjection")).toString() != QStringLiteral("raw") ||
      envelope.value(QStringLiteral("metadataDigest")).toString() != QString::fromLatin1(key.metadataDigest.toHex()) ||
      envelope.value(QStringLiteral("sourceDigest")).toString() != QString::fromLatin1(sourceDigest.toHex()) ||
      !envelope.value(QStringLiteral("facts")).isObject()) return fail(QStringLiteral("index identity, schema or dependency digest differs"));
  const auto object = envelope.value(QStringLiteral("facts")).toObject();
  const auto digest = QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  if (envelope.value(QStringLiteral("factsDigest")).toString() != QString::fromLatin1(digest.toHex()))
    return fail(QStringLiteral("index fact checksum differs"));
  auto facts = petAnalysisFactsFromJson(object, error);
  if (!facts || facts->asset.instanceId != seed.instanceId || facts->asset.raceId != seed.raceId ||
      facts->asset.metadataSlotMaxLevel != seed.metadataSlotMaxLevel || facts->asset.detailAvailable != seed.detailAvailable ||
      !facts->asset.observationVerified) return fail(QStringLiteral("index facts differ from current source identity/knownness"));
  // r/ri are two representations of the already checked effective race.
  for (const QString& field : {QStringLiteral("fr"), QStringLiteral("lv"), QStringLiteral("_metaRaceId")})
    if (facts->asset.pet.value(field) != seed.pet.value(field)) return fail(QStringLiteral("index calculation identity differs"));
  // Names/location are presentation state, intentionally not a raw fact rev.
  facts->asset.name = seed.name; facts->asset.location = seed.location;
  for (const QString& field : {QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("n"), QStringLiteral("customName"), QStringLiteral("_location"),
       QStringLiteral("_warehouseGroup"), QStringLiteral("_position"), QStringLiteral("_metaOriginalName"),
       QStringLiteral("_metaAttributes"), QStringLiteral("_metaJobs"), QStringLiteral("_metaEra")}) {
    if (seed.pet.contains(field)) facts->asset.pet.insert(field, seed.pet.value(field));
    else facts->asset.pet.remove(field);
  }
  return facts;
}
void calculate(const std::shared_ptr<State>& state, const std::shared_ptr<Work>& work,
               const std::shared_ptr<Reservation>& reservation, QByteArray indexBytes,
               QByteArray sourceDigest, quint64 maximumIndexBytes) {
  auto result = std::make_shared<Completion>();
  result->id = work->id; result->key = work->key; result->reservation = reservation;
  QElapsedTimer elapsed; elapsed.start();
  try {
    if (state->closing.load() || work->cancelled->load()) result->status = PetDerivationStatus::Cancelled;
    else if (reinterpret_cast<quintptr>(QThread::currentThreadId()) == state->ownerThreadId) {
      result->status = PetDerivationStatus::ComputeUnavailable;
      result->error = QStringLiteral("derivation executor ran on Core instead of the existing Compute thread");
    }
    else if (!rawIdentity(*work->input->raw, &result->error)) result->status = PetDerivationStatus::RawIdentityMismatch;
    else {
      // No large JSON projection is made or retained on Core.
      PetAssetRecord seed = work->input->seed;
      const auto& raw = *work->input->raw;
      const int rawRace = petRaceId(raw.object());
      if (seed.detailAvailable && rawRace > 0 && rawRace != seed.raceId) {
        result->status = PetDerivationStatus::RawIdentityMismatch;
        result->error = QStringLiteral("complete raw detail race differs from the requested current race");
      } else {
        seed.pet = AssetDerivation::analysisInputFields(raw.brief, raw.object());
        // Caller identity is frozen on Core from the same record/meta version.
        const auto identity = work->input->seed.pet;
        for (auto field = identity.begin(); field != identity.end(); ++field) seed.pet.insert(field.key(), field.value());
        if (!seed.pet.contains(QStringLiteral("id"))) seed.pet.insert(QStringLiteral("id"), seed.instanceId);
        if (!seed.pet.contains(QStringLiteral("r")) && !seed.pet.contains(QStringLiteral("ri")) && seed.raceId > 0)
          seed.pet.insert(QStringLiteral("r"), seed.raceId);
        if (!seed.detailAvailable) seed.pet = AssetDerivation::identityFields(seed.pet);
        quint64 identityBytes = 0;
        if (!valueBytes(AssetDerivation::identityFields(seed.pet), &identityBytes, reservation->bytes.load() / 2)) {
          result->status = PetDerivationStatus::BudgetExceeded;
          result->error = QStringLiteral("compact identity exceeds the per-fact budget");
        } else {
          if (!indexBytes.isEmpty()) {
            ComputeSubspan decodeElapsed(state->completedIndexDecodeUs);
            result->facts = readIndex(indexBytes, work->key, sourceDigest, seed, &result->error);
            result->indexHit = result->facts.has_value(); result->indexRejected = !result->indexHit;
            if (result->indexHit) result->indexSourceDigest = sourceDigest;
          }
          if (!result->facts) {
            result->error.clear(); result->derived = true;
            ComputeSubspan deriveElapsed(state->completedRawDeriveUs);
            result->facts = derivePetAnalysisFacts(seed, work->input->metadata->value, nullptr, &result->error);
          }
          result->status = result->facts ? PetDerivationStatus::CacheHit : PetDerivationStatus::DeriveFailed;
          if (result->facts && petAnalysisFactsRetainedBytes(*result->facts) + recordOverhead(work->key) > reservation->bytes.load()) {
            result->facts.reset(); result->status = PetDerivationStatus::BudgetExceeded;
            result->error = QStringLiteral("derived fact exceeds its reserved output budget");
          }
          if (result->facts && work->key.metadataDigest.size() == 32) {
            const auto bytes = QJsonDocument(petAnalysisFactsToJson(*result->facts)).toJson(QJsonDocument::Compact);
            if (quint64(bytes.size()) <= maximumIndexBytes / 2)
              result->factsDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
          }
        }
      }
    }
  } catch (...) { result->facts.reset(); result->status = PetDerivationStatus::DeriveFailed; result->error = QStringLiteral("derivation raised an exception"); }
  if (state->closing.load() || work->cancelled->load()) { result->facts.reset(); result->status = PetDerivationStatus::Cancelled; }
  result->elapsedUs = elapsed.nsecsElapsed() / 1000;
  state->completedComputeUs.fetch_add(result->elapsedUs);
  state->deliver(std::move(result));
}
}
} // namespace PetDerivationInternal

using namespace PetDerivationInternal;
struct PetDerivationCache::Impl {
  ComputeExecutor executor;
  QPointer<StorageService> storage;
  PetDerivationCacheLimits limits;
  std::shared_ptr<State> state = std::make_shared<State>();
  QString account;
  quint64 epoch = 0, metadataRevision = 0;
  QByteArray metadataDigest;
  std::shared_ptr<Metadata> metadata;
  QTimer retry;
  std::list<std::shared_ptr<Work>> queue;
  QHash<quint64, std::list<std::shared_ptr<Work>>::iterator> queuedPositions;
  QHash<quint64, std::shared_ptr<Work>> pending;
  QHash<PetDerivationKey, quint64> pendingKeys;
  QHash<qint64, QSet<quint64>> pendingById;
  QHash<qint64, quint64> minimumRevisions;
  std::shared_ptr<Work> active;
  struct Entry {
    PetDerivedFactsHandle value;
    PetAssetRecord seed;
    std::list<PetDerivationKey>::iterator lru;
    QByteArray factsDigest, indexedDigest, writingDigest;
  };
  QHash<PetDerivationKey, Entry> entries;
  QHash<qint64, PetDerivationKey> residentById;
  std::list<PetDerivationKey> lru;
  quint64 residentBytes = 0;
  struct Read { std::weak_ptr<Work> work; StorageContext context; std::shared_ptr<IndexScratch> scratch; };
  struct Write { PetDerivationKey key; QByteArray sourceDigest; };
  struct Intent {
    std::weak_ptr<const PetDerivedFactsRecord> facts;
    QByteArray sourceDigest, factsDigest;
    StorageContext context;
    quint64 bytes = 0;
  };
  QHash<quint64, Read> reads;
  QHash<quint64, Write> writes;
  QHash<PetDerivationKey, Intent> intents;
  QHash<qint64, PetDerivationKey> intentById;
  quint64 intentBytes = 0;
  PetDerivationCacheStats counters;
  bool current(const PetDerivationKey& key) const {
    return !state->closing.load() && key.record.account == account && key.record.epoch == epoch &&
        key.metadataRevision == metadataRevision && key.metadataDigest == metadataDigest &&
        key.record.detailMemoryRevision >= minimumRevisions.value(key.record.instanceId);
  }
  void remove(const PetDerivationKey& key) {
    auto entry = entries.find(key);
    if (entry == entries.end()) return;
    residentBytes -= entry->value->chargedBytes;
    if (residentById.value(key.record.instanceId) == key) residentById.remove(key.record.instanceId);
    lru.erase(entry->lru); entries.erase(entry); ++counters.evictions;
  }
  void forgetPending(quint64 id) {
    auto work = pending.find(id); if (work == pending.end()) return;
    const auto key = work.value()->key;
    if (pendingKeys.value(key) == id) pendingKeys.remove(key);
    auto slot = pendingById.find(key.record.instanceId);
    if (slot != pendingById.end()) { slot->remove(id); if (slot->isEmpty()) pendingById.erase(slot); }
    pending.erase(work);
  }
  void removeQueued(quint64 id) {
    auto position = queuedPositions.find(id); if (position == queuedPositions.end()) return;
    queue.erase(position.value()); queuedPositions.erase(position);
  }
  void forgetIntent(const PetDerivationKey& key) {
    auto intent = intents.find(key); if (intent == intents.end()) return;
    intentBytes -= intent->bytes;
    if (intentById.value(key.record.instanceId) == key) intentById.remove(key.record.instanceId);
    intents.erase(intent);
  }
  void trim(quint64 requested = 0) {
    while (!lru.empty() && (residentBytes > limits.residentBytes || entries.size() > limits.residentEntries ||
        state->factsBytes.load() + state->reservedBytes.load() > limits.retainedBytes - requested)) remove(lru.back());
  }
};

PetDerivationCache::PetDerivationCache(ComputeExecutor executor, StorageService* storage,
    PetDerivationCacheLimits limits, QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->executor = std::move(executor); impl_->storage = storage;
  limits.queuedTasks = std::max(1, limits.queuedTasks); limits.residentEntries = std::max(1, limits.residentEntries);
  limits.queuedBytes = std::max<quint64>(1, limits.queuedBytes); limits.metadataBytes = std::max<quint64>(1, limits.metadataBytes);
  limits.retainedBytes = std::max<quint64>(1, limits.retainedBytes);
  limits.residentBytes = std::min(limits.residentBytes, limits.retainedBytes);
  limits.maximumFactBytes = std::max<quint64>(1, std::min(limits.maximumFactBytes, limits.retainedBytes));
  limits.indexRecordBytes = std::clamp<quint64>(limits.indexRecordBytes, 1024, 256 * 1024);
  limits.indexIntentBytes = std::max<quint64>(1, limits.indexIntentBytes);
  impl_->limits = limits; impl_->state->receiver = this;
  impl_->state->ownerThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
  impl_->retry.setSingleShot(true);
  connect(&impl_->retry, &QTimer::timeout, this, [this] { pump(); });
  if (storage) connect(storage, &StorageService::completed, this, &PetDerivationCache::receiveStorage);
  qRegisterMetaType<PetDerivationKey>(); qRegisterMetaType<PetDerivedFactsHandle>(); qRegisterMetaType<PetDerivationStatus>();
}
PetDerivationCache::~PetDerivationCache() { shutdown(); }
PetDerivationKey PetDerivationCache::keyFor(const PetDerivationRequest& request) {
  return {request.raw ? request.raw->key : PetRecordKey{}, request.metadataRevision, request.metadataDigest};
}
PetDerivedFactsHandle PetDerivationCache::lookup(const PetDerivationKey& key) {
  if (!impl_->current(key)) return {};
  auto entry = impl_->entries.find(key);
  if (entry == impl_->entries.end()) return {};
  impl_->lru.splice(impl_->lru.begin(), impl_->lru, entry->lru);
  ++impl_->counters.cacheHits;
  return entry->value;
}
PetDerivationSubmission PetDerivationCache::request(const PetDerivationRequest& request) {
  PetDerivationSubmission result;
  const auto reject = [&](PetDerivationStatus status, const QString& error) {
    ++impl_->counters.rejected; result.status = status; result.error = error; return result;
  };
  if (impl_->state->closing.load()) return reject(PetDerivationStatus::Closing, QStringLiteral("derivation cache is closing"));
  const auto key = keyFor(request);
  if (!request.raw || !validKey(key) || key.record.account != impl_->account || key.record.epoch != impl_->epoch ||
      request.seed.instanceId != key.record.instanceId || request.seed.raceId < 0 || request.seed.metadataSlotMaxLevel < 0 ||
      (request.raw->complete && !request.raw->payload) || (request.seed.detailAvailable && !request.raw->complete) ||
      request.seed.pet != AssetDerivation::identityFields(request.seed.pet) ||
      (!request.raw->contentDigest.isEmpty() && request.raw->contentDigest.size() != 32))
    return reject(PetDerivationStatus::InvalidRequest, QStringLiteral("invalid/frozen raw, identity, session or digest request"));
  if (!impl_->metadataRevision) { impl_->metadataRevision = key.metadataRevision; impl_->metadataDigest = key.metadataDigest; }
  if (!impl_->current(key)) return reject(PetDerivationStatus::Superseded, QStringLiteral("request belongs to another metadata version"));
  QString identityError;
  if (!rawIdentity(*request.raw, &identityError) ||
      (request.seed.detailAvailable && petRaceId(request.raw->object()) > 0 && petRaceId(request.raw->object()) != request.seed.raceId))
    return reject(PetDerivationStatus::RawIdentityMismatch, identityError.isEmpty() ? QStringLiteral("raw race differs from the complete seed") : identityError);
  if (impl_->metadata && (impl_->metadata->value.stargods != request.metadata.stargods ||
      impl_->metadata->value.astrolabe != request.metadata.astrolabe || impl_->metadata->value.pets != request.metadata.pets ||
      impl_->metadata->value.sacredStarPlans != request.metadata.sacredStarPlans ||
      impl_->metadata->value.sacredStagePlans != request.metadata.sacredStagePlans ||
      impl_->metadata->value.badges != request.metadata.badges))
    return reject(PetDerivationStatus::InvalidRequest, QStringLiteral("one metadata key cannot describe different content"));
  auto existing = impl_->entries.find(key);
  if (existing != impl_->entries.end()) {
    if (!sameCalculationSeed(existing->seed, request.seed) || existing->seed.observationVerified != request.raw->sourceKnown)
      return reject(PetDerivationStatus::InvalidRequest, QStringLiteral("one derivation key was reused with different calculation seed"));
    result.facts = lookup(key); result.accepted = true; result.status = PetDerivationStatus::CacheHit;
    const QPointer<PetDerivationCache> alive(this);
    queueIndex(result.facts, request.raw->contentDigest, request.storageContext,
        request.raw->sourceKnown && request.raw->complete && request.raw->persisted && request.raw->rawProjectionOnly && request.seed.detailAvailable);
    if (!alive || !impl_->current(key)) { result.facts.reset(); result.accepted = false; result.status = PetDerivationStatus::Superseded; }
    return result;
  }
  const auto queuedId = impl_->pendingKeys.value(key);
  if (queuedId) {
    auto work = impl_->pending.value(queuedId);
    if (work && !work->cancelled->load()) {
      if (!sameCalculationSeed(work->input->seed, request.seed) || work->input->seed.observationVerified != request.raw->sourceKnown)
        return reject(PetDerivationStatus::InvalidRequest, QStringLiteral("pending key was reused with a different seed"));
      if (!request.raw->contentDigest.isEmpty()) work->sourceDigest = request.raw->contentDigest;
      work->persistable = request.raw->sourceKnown && request.raw->complete && request.raw->persisted && request.raw->rawProjectionOnly && request.seed.detailAvailable;
      if (request.storageContext) work->storageContext = request.storageContext;
      result.taskId = queuedId; result.accepted = true; result.status = PetDerivationStatus::AlreadyQueued; return result;
    }
  }
  if (impl_->pending.size() >= impl_->limits.queuedTasks)
    return reject(PetDerivationStatus::QueueFull, QStringLiteral("bounded derivation task queue is full"));
  quint64 inputBytes = sizeof(Work) + sizeof(Input) + sizeof(RawPetRecord) + textBytes(key.record.account) + 512;
  const quint64 rawBytes = request.raw->payload ? request.raw->payload->chargedBytes : 0;
  // Reserve both the borrowed raw representation and its temporary projection.
  if (!add(&inputBytes, rawBytes, impl_->limits.queuedBytes) || !add(&inputBytes, rawBytes, impl_->limits.queuedBytes) ||
      !add(&inputBytes, textBytes(request.seed.name) + textBytes(request.seed.location), impl_->limits.queuedBytes) ||
      !valueBytes(request.seed.pet, &inputBytes, impl_->limits.queuedBytes) ||
      !valueBytes(request.raw->brief, &inputBytes, impl_->limits.queuedBytes) ||
      impl_->state->inputBytes.load() > impl_->limits.queuedBytes - inputBytes)
    return reject(PetDerivationStatus::QueueFull, QStringLiteral("bounded derivation input byte queue is full"));
  if (!impl_->metadata) {
    quint64 bytes = sizeof(Metadata) + 128;
    if (!valueBytes(request.metadata.stargods, &bytes, impl_->limits.metadataBytes) ||
        !valueBytes(request.metadata.astrolabe, &bytes, impl_->limits.metadataBytes) ||
        !valueBytes(request.metadata.pets, &bytes, impl_->limits.metadataBytes) ||
        !valueBytes(request.metadata.sacredStarPlans, &bytes, impl_->limits.metadataBytes) ||
        !valueBytes(request.metadata.sacredStagePlans, &bytes, impl_->limits.metadataBytes) ||
        !valueBytes(request.metadata.badges, &bytes, impl_->limits.metadataBytes) ||
        impl_->state->metadataBytes.load() > impl_->limits.metadataBytes - bytes)
      return reject(PetDerivationStatus::BudgetExceeded, QStringLiteral("frozen derivation metadata exceeds its budget"));
    auto metadata = std::make_shared<Metadata>(); metadata->state = impl_->state;
    metadata->value = request.metadata; metadata->revision = key.metadataRevision; metadata->digest = key.metadataDigest;
    metadata->bytes = bytes; impl_->state->metadataBytes.fetch_add(bytes); impl_->metadata = std::move(metadata);
  }
  auto input = std::make_shared<Input>(); input->state = impl_->state; input->raw = request.raw;
  input->seed = smallSeed(request.seed, request.raw->sourceKnown); input->metadata = impl_->metadata;
  input->bytes = inputBytes; impl_->state->inputBytes.fetch_add(inputBytes);
  State::peak(impl_->state->peakInput, impl_->state->inputBytes.load());
  auto work = std::make_shared<Work>(); work->id = nextTransportTaskId(); work->key = key; work->input = std::move(input);
  work->sourceDigest = request.raw->contentDigest;
  work->persistable = request.raw->sourceKnown && request.raw->complete && request.raw->persisted && request.raw->rawProjectionOnly && request.seed.detailAvailable;
  work->storageContext = request.storageContext;
  impl_->pending.insert(work->id, work); impl_->pendingKeys.insert(key, work->id); impl_->queue.push_back(work);
  impl_->queuedPositions.insert(work->id, std::prev(impl_->queue.end()));
  impl_->pendingById[key.record.instanceId].insert(work->id);
  impl_->retry.start(0);
  result.taskId = work->id; result.accepted = true; result.status = PetDerivationStatus::Queued;
  emit stateChanged(); return result;
}
void PetDerivationCache::pump() {
  if (impl_->state->closing.load()) return;
  // A completion can reach Core before the Compute functor has unwound. Do
  // not post a second callback until that first dispatch token is released.
  if (!impl_->active && impl_->state->posted.load() != 0) return;
  while (!impl_->queue.empty() && impl_->queue.front()->cancelled->load()) impl_->removeQueued(impl_->queue.front()->id);
  if (!impl_->active && impl_->queue.empty()) { pumpIndexes(); return; }
  if (!impl_->active) {
    const auto work = impl_->queue.front();
    impl_->trim(impl_->limits.maximumFactBytes);
    if (impl_->state->factsBytes.load() + impl_->state->reservedBytes.load() > impl_->limits.retainedBytes - impl_->limits.maximumFactBytes) {
      impl_->removeQueued(work->id); impl_->forgetPending(work->id);
      ++impl_->counters.rejected; impl_->retry.start(0);
      emit failed(work->id, work->key, PetDerivationStatus::BudgetExceeded, QStringLiteral("derived facts retained by consumers exhausted the budget"));
      return;
    }
    auto reservation = std::make_shared<Reservation>(); reservation->state = impl_->state;
    reservation->bytes = impl_->limits.maximumFactBytes;
    impl_->state->reservedBytes.fetch_add(reservation->bytes.load());
    State::peak(impl_->state->peakFacts, impl_->state->factsBytes.load() + impl_->state->reservedBytes.load());
    work->reservation = std::move(reservation);
    impl_->removeQueued(work->id); impl_->active = work;
  }
  const auto work = impl_->active;
  if (work->computePosted || work->readId) return;
  if (work->cancelled->load()) {
    auto cancelled = std::make_shared<Completion>(); cancelled->id = work->id; cancelled->key = work->key;
    cancelled->status = PetDerivationStatus::Cancelled; cancelled->reservation = work->reservation;
    receive(cancelled); return;
  }
  if (!work->indexAttempted) {
    work->indexAttempted = true;
    const bool eligible = work->persistable && work->sourceDigest.size() == 32 && work->key.metadataDigest.size() == 32 &&
        indexContextValid(impl_->storage, work->key, work->storageContext);
    const quint64 scratchBytes = impl_->limits.indexRecordBytes * 65;
    if (eligible && scratchBytes <= impl_->limits.queuedBytes &&
        impl_->state->inputBytes.load() <= impl_->limits.queuedBytes - scratchBytes) {
      auto scratch = std::make_shared<IndexScratch>(); scratch->state = impl_->state; scratch->bytes = scratchBytes;
      impl_->state->inputBytes.fetch_add(scratchBytes);
      State::peak(impl_->state->peakInput, impl_->state->inputBytes.load());
      const auto context = impl_->storage->createReadOnlyContext(work->storageContext->directory());
      const auto read = impl_->storage->submitRead({context, indexPath(work->key.record.instanceId),
          work->key.record.detailMemoryRevision, static_cast<qint64>(impl_->limits.indexRecordBytes), false});
      if (read.accepted) {
        work->readId = read.taskId; work->indexScratch = scratch;
        impl_->reads.insert(read.taskId, {work, context, scratch}); emit stateChanged(); return;
      }
    }
    ++impl_->counters.indexMisses;
  }
  if (!impl_->executor) {
    auto failed = std::make_shared<Completion>(); failed->id = work->id; failed->key = work->key;
    failed->status = PetDerivationStatus::ComputeUnavailable; failed->error = QStringLiteral("Compute executor is unavailable");
    failed->reservation = work->reservation; receive(failed); return;
  }
  auto posted = std::make_shared<Posted>(); posted->state = impl_->state; impl_->state->posted.fetch_add(1);
  const auto state = impl_->state; const auto reservation = work->reservation;
  const auto bytes = work->indexBytes; const auto digest = work->sourceDigest;
  const auto maximumIndexBytes = impl_->limits.indexRecordBytes;
  bool accepted = false;
  try { accepted = impl_->executor([state, work, reservation, posted, bytes, digest, maximumIndexBytes] {
    calculate(state, work, reservation, bytes, digest, maximumIndexBytes);
  }); } catch (...) {}
  if (!accepted) { impl_->retry.start(10); return; }
  work->computePosted = true;
  emit stateChanged();
}

void PetDerivationCache::receive(std::shared_ptr<Completion> completion) {
  if (impl_->state->closing.load() || !impl_->active || impl_->active->id != completion->id) return;
  const auto work = impl_->active;
  impl_->active.reset(); impl_->forgetPending(work->id);
  impl_->counters.maximumComputeMicroseconds = std::max(impl_->counters.maximumComputeMicroseconds, completion->elapsedUs);
  impl_->counters.computations += completion->derived;
  impl_->counters.indexHits += completion->indexHit;
  impl_->counters.indexRejected += completion->indexRejected;
  PetDerivedFactsHandle handle;
  auto status = completion->status;
  if (work->cancelled->load() || !impl_->current(work->key)) status = PetDerivationStatus::Cancelled;
  if (status == PetDerivationStatus::CacheHit && completion->facts) {
    const quint64 bytes = petAnalysisFactsRetainedBytes(*completion->facts) + recordOverhead(work->key);
    auto lease = std::make_shared<FactLease>(); lease->state = impl_->state; lease->bytes = bytes;
    impl_->state->factsBytes.fetch_add(bytes); impl_->state->liveLeases.fetch_add(1);
    auto value = std::make_shared<PetDerivedFactsRecord>(); value->key = work->key; value->chargedBytes = bytes;
    value->memoryRetention = lease;
    value->facts = std::move(*completion->facts); value->facts.asset.memoryRetention = lease;
    // Convert the active result reservation to its measured immutable lease.
    completion->facts.reset(); completion->reservation->release(); completion->reservation.reset();
    handle = value;
    if (impl_->residentById.contains(work->key.record.instanceId)) impl_->remove(impl_->residentById.value(work->key.record.instanceId));
    impl_->lru.push_front(work->key);
    impl_->entries.insert(work->key, {value, work->input->seed, impl_->lru.begin()});
    impl_->residentById.insert(work->key.record.instanceId, work->key);
    impl_->entries[work->key].factsDigest = completion->factsDigest;
    if (completion->indexHit) impl_->entries[work->key].indexedDigest = completion->indexSourceDigest;
    // The cache never retains a calculation seed's source/lease or raw JSON.
    impl_->entries[work->key].seed.pet = calculationIdentity(work->input->seed.pet);
    impl_->entries[work->key].seed.memoryRetention.reset();
    impl_->residentBytes += bytes; impl_->trim();
    State::peak(impl_->state->peakFacts, impl_->state->factsBytes.load() + impl_->state->reservedBytes.load());
    const QPointer<PetDerivationCache> alive(this);
    queueIndex(handle, work->sourceDigest, work->storageContext, work->persistable);
    if (!alive) return;
    if (!impl_->current(work->key)) { handle.reset(); status = PetDerivationStatus::Cancelled; }
  } else {
    if (completion->reservation) completion->reservation->release();
    if (status == PetDerivationStatus::Cancelled) ++impl_->counters.cancelled;
  }
  impl_->retry.start(0);
  const QPointer<PetDerivationCache> alive(this);
  if (handle) emit ready(work->id, work->key, handle);
  else emit failed(work->id, work->key, status, completion->error);
  if (alive) emit stateChanged();
}
void PetDerivationCache::budgetReleased() {
  impl_->state->notificationQueued.store(false);
  if (impl_->state->closing.load()) return;
  if (!impl_->active && (!impl_->queue.empty() || !impl_->intents.isEmpty())) impl_->retry.start(0);
  emit stateChanged();
}
void PetDerivationCache::receiveStorage(const StorageResult& result) {
  auto read = impl_->reads.find(result.taskId);
  if (read != impl_->reads.end()) {
    const auto task = read.value(); impl_->reads.erase(read);
    const auto work = task.work.lock();
    if (work) work->readId = 0;
    if (!work || impl_->state->closing.load()) return;
    if (result.status == StorageStatus::Loaded && !work->cancelled->load()) work->indexBytes = result.content;
    else { ++impl_->counters.indexMisses; work->indexScratch.reset(); }
    impl_->retry.start(0); emit stateChanged(); return;
  }
  auto write = impl_->writes.find(result.taskId);
  if (write == impl_->writes.end()) return;
  const auto task = write.value(); impl_->writes.erase(write);
  if (result.status != StorageStatus::Saved && result.status != StorageStatus::Superseded) ++impl_->counters.indexWriteFailures;
  auto entry = impl_->entries.find(task.key);
  if (entry != impl_->entries.end() && entry->writingDigest == task.sourceDigest) {
    entry->writingDigest.clear();
    if (result.status == StorageStatus::Saved) entry->indexedDigest = task.sourceDigest;
  }
  if (impl_->state->closing.load()) return;
  impl_->retry.start(0);
  const QPointer<PetDerivationCache> alive(this);
  emit indexStatus(task.key, result.status, result.error);
  if (alive) emit stateChanged();
}
void PetDerivationCache::queueIndex(const PetDerivedFactsHandle& facts, const QByteArray& sourceDigest,
                                   const StorageContext& context, bool eligible) {
  if (!facts || !eligible || facts->key.record.epoch == 0 || !impl_->current(facts->key) || sourceDigest.size() != 32 || facts->key.metadataDigest.size() != 32 ||
      !indexContextValid(impl_->storage, facts->key, context)) return;
  const auto entry = impl_->entries.constFind(facts->key);
  if (entry == impl_->entries.cend() || entry->factsDigest.size() != 32 ||
      entry->indexedDigest == sourceDigest || entry->writingDigest == sourceDigest) return;
  const quint64 bytes = sizeof(Impl::Intent) + sizeof(PetDerivationKey) + textBytes(facts->key.record.account) + 256;
  auto previous = impl_->intents.find(facts->key);
  if (previous != impl_->intents.end()) {
    if (previous->sourceDigest == sourceDigest) return;
    impl_->forgetIntent(facts->key);
  }
  if (impl_->intentById.contains(facts->key.record.instanceId)) impl_->forgetIntent(impl_->intentById.value(facts->key.record.instanceId));
  if (impl_->intents.size() >= impl_->limits.residentEntries || bytes > impl_->limits.indexIntentBytes ||
      impl_->intentBytes > impl_->limits.indexIntentBytes - bytes) {
    ++impl_->counters.indexWriteFailures;
    emit indexStatus(facts->key, StorageStatus::QueueFull, QStringLiteral("bounded disposable index-intent queue is full"));
    return;
  }
  impl_->intents.insert(facts->key, {facts, sourceDigest, entry->factsDigest, context, bytes});
  impl_->intentById.insert(facts->key.record.instanceId, facts->key);
  impl_->intentBytes += bytes;
  if (!impl_->active && impl_->queue.empty()) impl_->retry.start(0);
}
void PetDerivationCache::pumpIndexes() {
  constexpr int maximumInFlightWrites = 2;
  if (!impl_->storage || impl_->state->closing.load() || impl_->active || !impl_->queue.empty() ||
      impl_->writes.size() >= maximumInFlightWrites) return;
  // Optional index writes wait behind the bounded raw-derivation batch, so
  // thousands of first-use fsyncs do not serialize the analysis hot path.
  for (int count = 0; count < 8 && !impl_->intents.isEmpty() && impl_->writes.size() < maximumInFlightWrites; ++count) {
    auto iterator = impl_->intents.begin();
    const auto key = iterator.key(); const auto intent = iterator.value();
    const auto facts = intent.facts.lock();
    if (!facts || !impl_->current(key)) {
      impl_->forgetIntent(key); continue;
    }
    const auto object = petAnalysisFactsToJson(facts->facts);
    QJsonObject envelope{{QStringLiteral("schema"), 1}, {QStringLiteral("algorithm"), key.analysisVersion},
        {QStringLiteral("account"), key.record.account}, {QStringLiteral("instanceId"), QString::number(key.record.instanceId)},
        {QStringLiteral("sourceProjection"), QStringLiteral("raw")},
        {QStringLiteral("metadataDigest"), QString::fromLatin1(key.metadataDigest.toHex())},
        {QStringLiteral("sourceDigest"), QString::fromLatin1(intent.sourceDigest.toHex())},
        {QStringLiteral("factsDigest"), QString::fromLatin1(intent.factsDigest.toHex())}, {QStringLiteral("facts"), object}};
    const auto write = impl_->storage->submitJsonWrite({intent.context, indexPath(key.record.instanceId), nextTransportTaskId(),
        envelope, static_cast<qint64>(impl_->limits.indexRecordBytes), false});
    if (!write.accepted && write.status == StorageStatus::QueueFull) { impl_->retry.start(10); return; }
    impl_->forgetIntent(key);
    if (write.accepted) {
      impl_->writes.insert(write.taskId, {key, intent.sourceDigest}); ++impl_->counters.indexWrites;
      auto entry = impl_->entries.find(key); if (entry != impl_->entries.end()) entry->writingDigest = intent.sourceDigest;
    } else ++impl_->counters.indexWriteFailures;
    const QPointer<PetDerivationCache> alive(this);
    emit indexStatus(key, write.status, write.error);
    if (!alive) return;
    if (impl_->state->closing.load() || impl_->active || !impl_->queue.empty()) return;
  }
  if (!impl_->intents.isEmpty()) impl_->retry.start(0);
}
void PetDerivationCache::bindSession(const QString& account, quint64 epoch) {
  if (impl_->account == account && impl_->epoch == epoch) return;
  impl_->account = account; impl_->epoch = epoch; impl_->minimumRevisions.clear(); clear();
}
void PetDerivationCache::invalidateRecord(const PetRecordKey& key) {
  const QPointer<PetDerivationCache> alive(this);
  if (key.account == impl_->account && key.epoch == impl_->epoch)
    impl_->minimumRevisions.insert(key.instanceId, std::max(key.detailMemoryRevision, impl_->minimumRevisions.value(key.instanceId)));
  if (key.account != impl_->account || key.epoch != impl_->epoch) return;
  const auto cancelIds = impl_->pendingById.value(key.instanceId);
  for (auto id : cancelIds) { cancel(id); if (!alive) return; }
  if (impl_->residentById.contains(key.instanceId)) impl_->remove(impl_->residentById.value(key.instanceId));
  if (impl_->intentById.contains(key.instanceId)) impl_->forgetIntent(impl_->intentById.value(key.instanceId));
  emit stateChanged();
}
void PetDerivationCache::invalidateMetadata(quint64 revision, const QByteArray& digest) {
  if (impl_->metadataRevision == revision && impl_->metadataDigest == digest) return;
  impl_->metadata.reset(); impl_->metadataRevision = revision; impl_->metadataDigest = digest; clear();
}
bool PetDerivationCache::cancel(quint64 taskId) {
  const auto work = impl_->pending.value(taskId);
  if (!work || work->cancelled->exchange(true)) return false;
  if (impl_->pendingKeys.value(work->key) == taskId) impl_->pendingKeys.remove(work->key);
  if (impl_->active && impl_->active->id == taskId) {
    if (work->readId && impl_->storage) impl_->storage->cancelReads(impl_->reads.value(work->readId).context);
    if (!work->computePosted && !work->readId) impl_->retry.start(0);
    return true;
  }
  impl_->forgetPending(taskId);
  impl_->removeQueued(taskId);
  ++impl_->counters.cancelled;
  emit failed(taskId, work->key, PetDerivationStatus::Cancelled, QStringLiteral("derivation request was cancelled before Compute"));
  return true;
}
void PetDerivationCache::clear() {
  QList<std::pair<quint64, PetDerivationKey>> cancelled;
  const auto pending = impl_->pending;
  for (auto work = pending.begin(); work != pending.end(); ++work) {
    work.value()->cancelled->store(true);
    if (work.value()->readId && impl_->storage) impl_->storage->cancelReads(impl_->reads.value(work.value()->readId).context);
    if (impl_->active && impl_->active->id == work.key()) continue;
    cancelled.append({work.key(), work.value()->key});
    impl_->forgetPending(work.key()); ++impl_->counters.cancelled;
  }
  impl_->queue.clear(); impl_->queuedPositions.clear(); impl_->pendingKeys.clear();
  impl_->intents.clear(); impl_->intentById.clear(); impl_->intentBytes = 0;
  impl_->entries.clear(); impl_->residentById.clear(); impl_->lru.clear(); impl_->residentBytes = 0;
  const QPointer<PetDerivationCache> alive(this);
  for (const auto& item : cancelled) {
    emit failed(item.first, item.second, PetDerivationStatus::Cancelled, QStringLiteral("cache generation was cleared before Compute"));
    if (!alive) return;
  }
  emit stateChanged();
}
bool PetDerivationCache::shutdown() {
  if (!impl_->state->closing.exchange(true)) {
    impl_->retry.stop();
    { QMutexLocker guard(&impl_->state->delivery); impl_->state->receiver = nullptr; }
    for (const auto& work : impl_->pending) work->cancelled->store(true);
    if (impl_->storage) for (const auto& read : impl_->reads) impl_->storage->cancelReads(read.context);
    impl_->queue.clear(); impl_->queuedPositions.clear(); impl_->pending.clear(); impl_->pendingKeys.clear(); impl_->pendingById.clear(); impl_->active.reset();
    impl_->entries.clear(); impl_->residentById.clear(); impl_->lru.clear(); impl_->residentBytes = 0; impl_->metadata.reset();
    impl_->intents.clear(); impl_->intentById.clear(); impl_->intentBytes = 0;
  }
  return impl_->state->posted.load() == 0 && impl_->reads.isEmpty() && impl_->writes.isEmpty();
}
PetDerivationCacheStats PetDerivationCache::stats() const {
  auto result = impl_->counters;
  result.pendingTasks = impl_->pending.size(); result.activeTasks = impl_->state->posted.load();
  result.residentEntries = impl_->entries.size(); result.residentFactsBytes = impl_->residentBytes;
  result.queuedInputBytes = impl_->state->inputBytes.load(); result.metadataBytes = impl_->state->metadataBytes.load();
  result.retainedFactsBytes = impl_->state->factsBytes.load(); result.reservedFactsBytes = impl_->state->reservedBytes.load();
  result.liveFactLeases = impl_->state->liveLeases.load(); result.peakInputBytes = impl_->state->peakInput.load();
  result.peakRetainedBytes = impl_->state->peakFacts.load(); result.closing = impl_->state->closing.load();
  result.completedComputeMicroseconds = impl_->state->completedComputeUs.load();
  result.completedIndexDecodeMicroseconds = impl_->state->completedIndexDecodeUs.load();
  result.completedRawDeriveMicroseconds = impl_->state->completedRawDeriveUs.load();
  result.pendingIndexTasks = impl_->reads.size() + impl_->writes.size() + impl_->intents.size();
  result.indexIntentBytes = impl_->intentBytes;
  return result;
}
