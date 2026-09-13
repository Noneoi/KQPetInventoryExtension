#pragma once

#include "asset_analysis_types.h"
#include "storage_types.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <deque>

class PetRepository;
class StorageService;

struct SnapshotStorageLimits {
  int maximumSummaries = 1024;
  int maximumFullSnapshots = 3;
  qint64 maximumFullCacheBytes = 16 * 1024 * 1024;
  qint64 maximumSnapshotBytes = 8 * 1024 * 1024;
  int maximumConcurrentReads = 2;
};

struct SnapshotCacheStats {
  int summaries = 0;
  int fullSnapshots = 0;
  qint64 fullBytes = 0;
  int instanceEntries = 0;
  int pendingTasks = 0;
  bool hasOlderHistory = false;
};

class AssetSnapshotStore final : public QObject {
  Q_OBJECT
public:
  explicit AssetSnapshotStore(PetRepository* repository, QObject* parent = nullptr,
                              const SnapshotStorageLimits& limits = {});
  ~AssetSnapshotStore() override;

  StorageSubmission write(const QString& account, const AccountAssetOverview& overview,
                          QString* admissionStatus = nullptr);
  QList<AccountAssetSnapshot> cachedHistory(const QString& account) const;
  QList<SnapshotInstanceHistoryEntry> cachedInstanceHistory(const QString& account, qint64 instanceId) const;
  int pendingTaskCount() const;
  int pendingWriteCount() const { return static_cast<int>(pendingWrites_.size()); }
  bool historyLoading() const { return historyLoading_; }
  bool instanceHistoryLoading() const { return instanceLoading_; }
  SnapshotCacheStats cacheStats() const;

public slots:
  void requestHistory(const QString& account, const QDateTime& before = {});
  void requestSnapshotDetails(const QString& account, const QString& storageKey);
  void requestInstanceHistory(const QString& account, qint64 instanceId);

signals:
  void writeStateChanged(quint64 taskId, const QString& account, quint64 epoch,
                         const QString& key, StorageStatus status, const QString& error);
  void writeFinished(quint64 taskId, const QString& account, quint64 epoch,
                     quint64 inventoryRevision, int analysisVersion,
                     StorageStatus status, const QString& error);
  void historyChanged(const QString& account, quint64 epoch);
  void historyLoadingChanged(const QString& account, quint64 epoch, bool loading, const QString& error);
  void instanceHistoryChanged(const QString& account, quint64 epoch, qint64 instanceId,
                              bool loading, const QString& error);
  void snapshotDetailsChanged(const QString& account, quint64 epoch, const QString& storageKey,
                              bool available, const QString& error);

private:
  enum class ReadKind { Scan, Summary, Full, Instance };
  struct PendingRead {
    ReadKind kind = ReadKind::Summary;
    StorageContext context;
    QString account;
    QString key;
    quint64 epoch = 0;
    quint64 generation = 0;
    quint64 recordRevision = 0;
    qint64 instanceId = 0;
    QString cursor;
  };
  struct PendingWrite {
    QString account;
    QString key;
    quint64 epoch = 0;
    quint64 inputRevision = 0;
    quint64 storeRevision = 0;
    AccountAssetSnapshot snapshot;
  };
  struct FullCache {
    AccountAssetSnapshot snapshot;
    qint64 bytes = 0;
    quint64 touch = 0;
  };
  void switchAccount(const QString& account, quint64 epoch);
  StorageContext newReadContext() const;
  void pump();
  void receive(const StorageResult& result);
  void acceptSnapshot(const QString& key, AccountAssetSnapshot snapshot, bool cacheFull);
  void cacheFull(const QString& key, const AccountAssetSnapshot& snapshot);
  void startInstanceReads();
  void finishLoadingIfDone();
  bool readCurrent(const PendingRead& pending) const;
  void trimSummaries();

  QPointer<PetRepository> repository_;
  QPointer<StorageService> storage_;
  SnapshotStorageLimits limits_;
  QString account_;
  quint64 epoch_ = 0;
  quint64 historyGeneration_ = 0;
  quint64 instanceGeneration_ = 0;
  quint64 nextStoreRevision_ = 0;
  quint64 cacheTouch_ = 0;
  StorageContext historyContext_;
  StorageContext instanceContext_;
  QTimer pumpTimer_;
  QHash<quint64, PendingRead> pendingReads_;
  QHash<quint64, PendingWrite> pendingWrites_;
  std::deque<PendingRead> queuedReads_;
  QStringList historyNames_;
  QStringList instanceKeys_;
  QString scanCursor_;
  bool scanWanted_ = false;
  bool scanInFlight_ = false;
  bool historyLoading_ = false;
  bool instanceLoading_ = false;
  bool instanceWaitingForHistory_ = false;
  bool historyHasOlder_ = false;
  qint64 selectedInstance_ = 0;
  QDateTime before_;
  QList<AccountAssetSnapshot> summaries_;
  QHash<QString, FullCache> fullCache_;
  qint64 fullCacheBytes_ = 0;
  QHash<QString, quint64> pathRevisions_;
  QSet<QString> requestedFullKeys_;
  QString selectedFullKey_;
  QString historyError_;
  QString instanceError_;
  QList<SnapshotInstanceHistoryEntry> instanceHistory_;
};
