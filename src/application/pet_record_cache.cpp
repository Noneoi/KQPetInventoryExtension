#include "pet_record_cache.h"
#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>
#include <atomic>
#include <limits>

struct PetRecordCache::Ledger {
  std::atomic<quint64> bytes{0}, peak{0}, payloads{0};
  struct Lease {
    std::shared_ptr<Ledger> ledger;
    quint64 bytes = 0;
    ~Lease() {
      ledger->bytes.fetch_sub(bytes, std::memory_order_acq_rel);
      ledger->payloads.fetch_sub(1, std::memory_order_acq_rel);
    }
  };
};
namespace {
PetRecordCacheLimits bounded(PetRecordCacheLimits requested) {
  const PetRecordCacheLimits maximum;
  requested.maximumBytes = std::clamp<quint64>(requested.maximumBytes, 1024, maximum.maximumBytes);
  requested.maximumRecordBytes = std::clamp<quint64>(requested.maximumRecordBytes, 512,
      std::min(maximum.maximumRecordBytes, requested.maximumBytes));
  requested.maximumRecords = std::clamp(requested.maximumRecords, 1, maximum.maximumRecords);
  requested.maximumJsonNodes = std::clamp(requested.maximumJsonNodes, 1, maximum.maximumJsonNodes);
  requested.maximumJsonDepth = std::clamp(requested.maximumJsonDepth, 1, maximum.maximumJsonDepth);
  return requested;
}
struct Meter {
  const PetRecordCacheLimits& limits;
  quint64 bytes = 512;
  int nodes = 0;
  bool add(quint64 count) {
    if (count > limits.maximumRecordBytes || bytes > limits.maximumRecordBytes - count) return false;
    bytes += count; return true;
  }
  bool visit(const QJsonValue& value, int depth) {
    if (++nodes > limits.maximumJsonNodes || depth > limits.maximumJsonDepth || !add(64)) return false;
    if (value.isString()) return add(quint64(value.toString().size()) * sizeof(QChar) + 32);
    if (value.isArray()) {
      const auto array = value.toArray();
      if (!add(quint64(array.size()) * 24)) return false;
      for (const auto& child : array) if (!visit(child, depth + 1)) return false;
    } else if (value.isObject()) {
      const auto object = value.toObject();
      if (!add(quint64(object.size()) * 32)) return false;
      for (auto child = object.begin(); child != object.end(); ++child) {
        if (!add(quint64(child.key().size()) * sizeof(QChar) + 32) || !visit(child.value(), depth + 1)) return false;
      }
    }
    return true;
  }
};
}
PetRecordCache::PetRecordCache(const PetRecordCacheLimits& limits)
    : limits_(bounded(limits)), ledger_(std::make_shared<Ledger>()) {}
PetRecordCache::~PetRecordCache() = default;

quint64 PetRecordCache::measure(const QJsonObject& object, const PetRecordCacheLimits& requested, QString* error) {
  const auto limits = bounded(requested);
  Meter meter{limits};
  if (!meter.visit(object, 0)) {
    if (error) *error = QStringLiteral("raw detail exceeded byte, node or depth budget");
    return 0;
  }
  return meter.bytes;
}
bool PetRecordCache::makeRoom(quint64 bytes, int entries, const QSet<PetRecordKey>& targets,
                              QList<PetRecordKey>* evicted) {
  if (bytes > limits_.maximumBytes || entries > limits_.maximumRecords) return false;
  while (ledger_->bytes.load(std::memory_order_acquire) > limits_.maximumBytes - bytes ||
         entries_.size() > limits_.maximumRecords - entries) {
    auto victim = entries_.end();
    for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
      // Preserve originals awaiting an authorized save. Weak-source readings
      // are intentionally session-only: once derived, their raw pages may be
      // reclaimed so a read-only batch can continue through the inventory.
      const bool awaitingSave = candidate->record->sourceKnown && !candidate->record->persisted;
      if (targets.contains(candidate.key()) || awaitingSave || !candidate->derived) continue;
      if (victim == entries_.end() || candidate->accessed < victim->accessed) victim = candidate;
    }
    if (victim == entries_.end()) return false;
    evicted->append(victim->record->key); entries_.erase(victim); ++evictions_;
    // A GUI/Compute/IO handle may still own this payload. Its charge deliberately
    // remains; eviction alone does not pretend memory was reclaimed.
  }
  return true;
}

PetRecordAdmission PetRecordCache::reserve(const QList<RawPetRecordInput>& values) {
  PetRecordAdmission result;
  QSet<PetRecordKey> targets;
  QList<quint64> sizes;
  quint64 bytes = 0;
  int newEntries = 0;
  for (const auto& value : values) {
    const auto identity = slot(value.key);
    if (value.key.account.isEmpty() || value.key.account.size() > 1024 || value.key.instanceId <= 0 ||
        !value.key.detailMemoryRevision || value.object.isEmpty() || targets.contains(identity)) {
      result.error = QStringLiteral("invalid or duplicate raw record identity"); ++refusals_; return result;
    }
    const auto old = entries_.constFind(identity);
    if (old != entries_.cend() && old->record->key.detailMemoryRevision >= value.key.detailMemoryRevision) {
      result.error = QStringLiteral("raw record memory revision is not newer"); ++refusals_; return result;
    }
    const quint64 measured = measure(value.object, limits_, &result.error);
    if (!measured || measured > limits_.maximumBytes - bytes) { ++refusals_; return result; }
    bytes += measured; sizes.append(measured); targets.insert(identity);
    if (old == entries_.cend()) ++newEntries;
  }
  if (!makeRoom(bytes, newEntries, targets, &result.evicted)) {
    result.error = QStringLiteral("raw cache is full; retained handles or unsaved originals still hold its budget");
    refusals_ += values.size(); return result;
  }
  // Admission/allocation is Core-serialized; only releases happen elsewhere.
  // No callbacks are invoked while this reservation is being constructed.
  try {
    for (int index = 0; index < values.size(); ++index) {
      const auto& value = values[index];
      auto lease = std::make_shared<Ledger::Lease>(); lease->ledger = ledger_; lease->bytes = sizes[index];
      const quint64 total = ledger_->bytes.fetch_add(lease->bytes, std::memory_order_acq_rel) + lease->bytes;
      ledger_->payloads.fetch_add(1, std::memory_order_acq_rel);
      quint64 peak = ledger_->peak.load();
      while (total > peak && !ledger_->peak.compare_exchange_weak(peak, total)) {}
      auto payload = std::make_shared<RawPetRecordPayload>();
      payload->cacheOwner_ = ledger_;
      payload->memoryRetention = std::move(lease); payload->object = value.object; payload->chargedBytes = sizes[index];
      auto record = std::make_shared<RawPetRecord>();
      record->key = value.key; record->contentDigest = value.contentDigest;
      record->sourceKnown = value.sourceKnown; record->complete = value.complete; record->persisted = value.persisted;
      record->observedAt = value.observedAt; record->brief = value.brief; record->rawProjectionOnly = value.rawProjectionOnly;
      record->payload = std::move(payload); result.records.append(std::move(record));
    }
  } catch (...) {
    result.records.clear(); result.error = QStringLiteral("raw record allocation failed");
    refusals_ += values.size(); return result;
  }
  result.accepted = true; return result;
}
void PetRecordCache::commit(const QList<RawPetRecordHandle>& records) {
  for (const auto& record : records) {
    if (!record || !record->payload || record->payload->cacheOwner_.get() != ledger_.get()) continue;
    entries_.insert(slot(record->key), {record, false, ++access_});
  }
}
RawPetRecordHandle PetRecordCache::acquire(const QString& account, quint64 epoch, qint64 id) const {
  const auto found = entries_.find({account, epoch, id, 0});
  if (found == entries_.end()) return {};
  found->accessed = ++access_; return found->record;
}
bool PetRecordCache::contains(const QString& account, quint64 epoch, qint64 id) const {
  return entries_.contains({account, epoch, id, 0});
}
RawPetRecordHandle PetRecordCache::revise(const PetRecordKey& current, const PetRecordKey& next,
    const QJsonObject& brief, bool sourceKnown, bool rawProjectionOnly, const QDateTime& observedAt) {
  const auto found = entries_.find(slot(current));
  if (found == entries_.end() || found->record->key != current || slot(current) != slot(next) ||
      next.detailMemoryRevision < current.detailMemoryRevision) return {};
  auto replacement = std::make_shared<RawPetRecord>(*found->record);
  replacement->key = next; replacement->brief = brief; replacement->sourceKnown = sourceKnown;
  replacement->rawProjectionOnly = rawProjectionOnly;
  if (observedAt.isValid()) replacement->observedAt = observedAt;
  found->record = std::move(replacement);
  if (current != next) found->derived = false;
  found->accessed = ++access_;
  return found->record;
}
RawPetRecordHandle PetRecordCache::saved(const PetRecordKey& key, const QByteArray& digest) {
  const auto found = entries_.find(slot(key));
  if (found == entries_.end() || found->record->key != key || digest.size() != 32) return {};
  auto replacement = std::make_shared<RawPetRecord>(*found->record);
  replacement->persisted = true; replacement->contentDigest = digest;
  found->record = std::move(replacement); return found->record;
}
bool PetRecordCache::markDerived(const PetRecordKey& key) {
  const auto found = entries_.find(slot(key));
  if (found == entries_.end() || found->record->key != key) return false;
  found->derived = true; return true;
}
void PetRecordCache::clearScope() { entries_.clear(); }
void PetRecordCache::noteUntrackedExport() const { ++untrackedExports_; }
PetRecordCacheStats PetRecordCache::stats() const {
  PetRecordCacheStats result;
  result.chargedBytes = ledger_->bytes.load(); result.peakChargedBytes = ledger_->peak.load();
  result.retainedPayloads = ledger_->payloads.load(); result.residentRecords = entries_.size();
  result.evictedRecords = evictions_; result.refusedRecords = refusals_; result.untrackedExports = untrackedExports_;
  for (const auto& entry : entries_) {
    result.residentBytes += entry.record->payload->chargedBytes;
    if ((entry.record->sourceKnown && !entry.record->persisted) || !entry.derived)
      ++result.protectedRecords;
  }
  return result;
}
