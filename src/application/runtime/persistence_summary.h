#pragma once
#include "runtime_types.h"
#include <QHash>

// Core-owned aggregate; GUI delivery may coalesce any number of record events
// without losing a failed record when a different record later saves.
class PersistenceSummary final {
public:
  RuntimePersistenceState update(const QString& currentAccount, quint64 currentEpoch,
      const QString& recordAccount, quint64 recordEpoch, const QString& record,
      quint64 revision, StorageStatus status, const QString& error, int pending) {
    if (currentAccount != account_ || currentEpoch != epoch_) {
      account_ = currentAccount; epoch_ = currentEpoch;
      failures_.clear(); overflow_ = false; saved_ = false; lastRecord_.clear();
    }
    if (recordAccount == account_ && recordEpoch == epoch_ && !record.isEmpty()) {
      lastRecord_ = record.left(256);
      if (status == StorageStatus::Saved) {
        const auto failed = failures_.constFind(record);
        if (failed != failures_.constEnd() && revision >= failed->revision) failures_.remove(record);
        saved_ = true;
      } else if (status != StorageStatus::Queued && status != StorageStatus::Superseded) {
        if (failures_.contains(record) || failures_.size() < 1024) {
          auto& failed = failures_[record];
          if (revision >= failed.revision) failed = {revision, error.left(512)};
        }
        else overflow_ = true; // Retain an explicit unknown remainder, never mark it Saved.
      }
    }
    RuntimePersistenceState result;
    result.account = account_; result.epoch = epoch_; result.record = lastRecord_; result.revision = revision;
    result.pendingWrites = qMax(0, pending); result.hasSaved = saved_;
    result.failedRecords = failures_.size(); result.failuresTruncated = overflow_;
    if (!failures_.isEmpty() || overflow_) {
      result.status = StorageStatus::WriteFailed;
      auto names = failures_.keys(); names.sort(Qt::CaseSensitive);
      QStringList notes;
      for (const auto& name : names.mid(0, 8))
        notes.append(name.left(128) + QStringLiteral("：") + failures_.value(name).error);
      if (names.size() > 8 || overflow_) notes.append(QStringLiteral("仅展开前 8 条；其余失败仍计入状态"));
      result.error = notes.join(QLatin1Char('\n'));
    } else if (pending > 0) result.status = StorageStatus::Queued;
    else result.status = saved_ ? StorageStatus::Saved : StorageStatus::Superseded;
    return result;
  }
private:
  QString account_, lastRecord_;
  quint64 epoch_ = 0;
  struct FailedRecord { quint64 revision = 0; QString error; };
  QHash<QString, FailedRecord> failures_;
  bool overflow_ = false;
  bool saved_ = false;
};
