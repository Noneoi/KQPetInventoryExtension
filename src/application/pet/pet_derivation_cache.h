#pragma once

#include "contracts/pet_derivation_types.h"
#include "storage/storage_types.h"
#include <QObject>
#include <QHashFunctions>
#include <functional>
#include <memory>

class StorageService;

enum class PetDerivationStatus {
  Queued, CacheHit, AlreadyQueued, QueueFull, BudgetExceeded, InvalidRequest,
  RawIdentityMismatch, Cancelled, Superseded, ComputeUnavailable, DeriveFailed, Closing
};
struct PetDerivationSubmission {
  quint64 taskId = 0;
  bool accepted = false;
  PetDerivationStatus status = PetDerivationStatus::InvalidRequest;
  QString error;
  PetDerivedFactsHandle facts; // CacheHit returns directly; it does not enqueue a ready event
};
struct PetDerivationRequest {
  RawPetRecordHandle raw;
  // Only scalar identity JSON is allowed here. The large calculation fields
  // are borrowed from raw/brief and projected on Compute after admission.
  PetAssetRecord seed;
  ShopPetMetadataSnapshot metadata;
  quint64 metadataRevision = 0;
  QByteArray metadataDigest;
  StorageContext storageContext;
};
struct PetDerivationCacheLimits {
  int queuedTasks = 256; // includes active task
  quint64 queuedBytes = 32ULL * 1024 * 1024;
  // One shared frozen catalog now includes all pet professions, star rules
  // and wheel nodes. Allow their retained representation and future entries.
  quint64 metadataBytes = 64ULL * 1024 * 1024;
  quint64 residentBytes = 64ULL * 1024 * 1024;
  quint64 retainedBytes = 128ULL * 1024 * 1024;
  quint64 maximumFactBytes = 256ULL * 1024;
  int residentEntries = 20000;
  quint64 indexIntentBytes = 4ULL * 1024 * 1024;
  quint64 indexRecordBytes = 64ULL * 1024;
};
struct PetDerivationCacheStats {
  int pendingTasks = 0, activeTasks = 0, residentEntries = 0;
  quint64 queuedInputBytes = 0, metadataBytes = 0;
  quint64 residentFactsBytes = 0, retainedFactsBytes = 0, reservedFactsBytes = 0;
  quint64 liveFactLeases = 0, peakInputBytes = 0, peakRetainedBytes = 0;
  quint64 cacheHits = 0, computations = 0, evictions = 0, rejected = 0, cancelled = 0;
  quint64 indexHits = 0, indexMisses = 0, indexRejected = 0, indexWrites = 0, indexWriteFailures = 0;
  quint64 indexIntentBytes = 0;
  int pendingIndexTasks = 0;
  qint64 maximumComputeMicroseconds = 0;
  qint64 completedComputeMicroseconds = 0; // executor wall spans; excludes pre-dispatch queue/IO wait and RAM hits
  bool closing = false;
  qint64 completedIndexDecodeMicroseconds = 0; // strict JSON/codec/checksum subspan of Compute
  qint64 completedRawDeriveMicroseconds = 0; // Domain factory subspan of Compute
};

namespace PetDerivationInternal { struct State; struct Completion; }

// Core owns maps/admission. Executor posts to the existing single Compute loop.
// Copies retained by UI/Analysis share the same fact lease after LRU eviction.
class PetDerivationCache final : public QObject {
  Q_OBJECT
public:
  using ComputeExecutor = std::function<bool(std::function<void()>)>;
  explicit PetDerivationCache(ComputeExecutor executor, StorageService* storage = nullptr,
      PetDerivationCacheLimits limits = {}, QObject* parent = nullptr);
  ~PetDerivationCache() override;
  static PetDerivationKey keyFor(const PetDerivationRequest& request);
  PetDerivedFactsHandle lookup(const PetDerivationKey& key);
  PetDerivationSubmission request(const PetDerivationRequest& request);
  void bindSession(const QString& account, quint64 epoch);
  void invalidateRecord(const PetRecordKey& key);
  void invalidateMetadata(quint64 revision, const QByteArray& digest);
  bool cancel(quint64 taskId);
  void clear();
  bool shutdown(); // true only if no posted work remains; never waits for Compute/IO
  PetDerivationCacheStats stats() const;

signals:
  void ready(quint64 taskId, const PetDerivationKey& key, const PetDerivedFactsHandle& facts);
  void failed(quint64 taskId, const PetDerivationKey& key, PetDerivationStatus status, const QString& error);
  void indexStatus(const PetDerivationKey& key, StorageStatus status, const QString& error);
  void stateChanged();

private:
  friend struct PetDerivationInternal::State;
  void pump();
  void budgetReleased();
  void receive(std::shared_ptr<PetDerivationInternal::Completion> completion);
  void receiveStorage(const StorageResult& result);
  void queueIndex(const PetDerivedFactsHandle& facts, const QByteArray& sourceDigest,
                  const StorageContext& context, bool eligible);
  void pumpIndexes();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Q_DECLARE_METATYPE(PetDerivationStatus)
