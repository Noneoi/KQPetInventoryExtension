#pragma once
#include "../contracts/pet_record_types.h"
#include <QHash>
#include <QList>
#include <QSet>
#include <memory>

struct PetRecordCacheLimits {
  quint64 maximumBytes = 64ULL * 1024 * 1024;
  quint64 maximumRecordBytes = 32ULL * 1024 * 1024;
  int maximumRecords = 16384;
  int maximumJsonNodes = 200000;
  int maximumJsonDepth = 64;
};
struct RawPetRecordInput {
  PetRecordKey key;
  QJsonObject object;
  QJsonObject brief;
  QByteArray contentDigest;
  bool sourceKnown = false;
  bool complete = false;
  bool persisted = false;
  bool rawProjectionOnly = true;
  QDateTime observedAt;
};
struct PetRecordAdmission {
  bool accepted = false;
  QList<RawPetRecordHandle> records;
  QList<PetRecordKey> evicted;
  QString error;
};

// Core-owned LRU; only payload leases cross threads. A lease is released only
// after the last handle drops it, even after replacement/eviction/clearScope.
class PetRecordCache final {
public:
  explicit PetRecordCache(const PetRecordCacheLimits& limits = {});
  ~PetRecordCache();
  PetRecordCache(const PetRecordCache&) = delete;
  PetRecordCache& operator=(const PetRecordCache&) = delete;

  // Reserve every payload before replacing any target entry. Call commit only
  // after the producer accepts the complete observation. Rejected reservation
  // leaves existing target records intact; unrelated reclaimable LRU may retire.
  PetRecordAdmission reserve(const QList<RawPetRecordInput>& records);
  void commit(const QList<RawPetRecordHandle>& records);
  RawPetRecordHandle acquire(const QString& account, quint64 epoch, qint64 id) const;
  RawPetRecordHandle revise(const PetRecordKey& current, const PetRecordKey& next,
      const QJsonObject& brief, bool sourceKnown, bool rawProjectionOnly, const QDateTime& observedAt = {});
  RawPetRecordHandle saved(const PetRecordKey& key, const QByteArray& digest);
  bool markDerived(const PetRecordKey& key);
  bool contains(const QString& account, quint64 epoch, qint64 id) const;
  void clearScope();
  PetRecordCacheStats stats() const;
  void noteUntrackedExport() const;
  static quint64 measure(const QJsonObject& object, const PetRecordCacheLimits& limits,
                         QString* error = nullptr);

private:
  struct Ledger;
  struct Entry { RawPetRecordHandle record; bool derived = false; quint64 accessed = 0; };
  static PetRecordKey slot(PetRecordKey key) { key.detailMemoryRevision = 0; return key; }
  bool makeRoom(quint64 bytes, int entries, const QSet<PetRecordKey>& protectedTargets,
                QList<PetRecordKey>* evicted);
  const PetRecordCacheLimits limits_;
  std::shared_ptr<Ledger> ledger_;
  mutable QHash<PetRecordKey, Entry> entries_;
  mutable quint64 access_ = 0;
  quint64 evictions_ = 0, refusals_ = 0;
  mutable quint64 untrackedExports_ = 0;
};
