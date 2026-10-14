#pragma once

#include "contracts/public_dataset.h"

#include <atomic>
#include <functional>

enum class PublicDatasetStatus {
  Ready, NotFound, Invalid, Conflict, Cancelled, Busy, IoError,
  DiskCommitted, SessionApplied
};

struct PublicDatasetPrepared {
  PublicDatasetSnapshotHandle snapshot;
  QString attempt;
  QString baseRevision;
  QString baseCurrentSha256;
  QString kind; // update, migration, rollback, restore
  QJsonObject validation;
  QJsonObject candidate;
};
using PublicDatasetPreparedHandle = std::shared_ptr<const PublicDatasetPrepared>;

struct PublicDatasetResult {
  PublicDatasetStatus status = PublicDatasetStatus::Invalid;
  QString error;
  PublicDatasetSnapshotHandle snapshot;
  PublicDatasetPreparedHandle prepared;
  QJsonObject receipt;
  bool ok() const {
    return status == PublicDatasetStatus::Ready || status == PublicDatasetStatus::DiskCommitted ||
           status == PublicDatasetStatus::SessionApplied;
  }
};

struct PublicDatasetStoreOptions {
  const std::atomic_bool* cancelled = nullptr;
  // Returning false injects a storage boundary failure. Empty in production.
  std::function<bool(const QString&)> allowBoundary;
};

// Construct and call only on Storage I/O (or the standalone native checker).
// Preparation retains no lock. Commit takes the process-shared publication
// lock and compares the exact current-pointer bytes, preventing ABA overwrite.
class PublicDatasetStore final {
public:
  explicit PublicDatasetStore(QString dataRoot, PublicDatasetStoreOptions options = {});
  PublicDatasetResult prepareCurrent(bool loadReplay = false) const;
  PublicDatasetResult prepareCandidate(const QString& attempt) const;
  PublicDatasetResult prepareMigration() const;
  PublicDatasetResult prepareRollback() const;
  PublicDatasetResult prepareRestoreRevision(const QString& revision) const;
  PublicDatasetResult commitPrepared(PublicDatasetPreparedHandle prepared) const;
  PublicDatasetResult markSessionApplied(const QString& attempt, const QString& revision) const;
  PublicDatasetResult loadReplay(PublicDatasetSnapshotHandle snapshot) const;
  QJsonObject inspect(QString* error = nullptr) const;
  static QString statusName(PublicDatasetStatus status);
  static QStringList componentKeys();
  static QStringList attachmentNames();

private:
  QString root_;
  PublicDatasetStoreOptions options_;
};
