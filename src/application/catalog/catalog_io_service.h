#pragma once

#include "storage/storage_service.h"
#include "contracts/battle_replay_catalog.h"
#include "contracts/public_dataset.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

enum class CatalogKind { Shop, Routine, PetDetail, PetSkill, BattleReplay, PublicDataset };
enum class CatalogRequestMode { Reload, OfficialUpdate };

struct CatalogIoOptions {
  // The application enables the complete-version workflow. The isolated
  // legacy importer remains available only to explicitly constructed tools/tests.
  bool publicDatasetMode = false;
  QString officialRoot;
  QString petOverlayPath;
  qint64 maximumSourceBytes = 16 * 1024 * 1024;
  int maximumScanEntries = 250000;
  int maximumPendingDirectories = 4096;
  int maximumScanMilliseconds = 15000;
};

struct CatalogIoStats {
  int pendingRequests = 0;
  int pendingWrites = 0;
  quint64 scannedEntries = 0;
  qint64 maximumSliceMicroseconds = 0;
};

namespace CatalogIoInternal { struct State; struct Candidate; }

// One Core facade and one bounded workflow on the existing Storage I/O loop.
// No per-request thread, synchronous disk API or mutable catalog crosses UI.
class CatalogIoService final : public QObject {
  Q_OBJECT
public:
  explicit CatalogIoService(StorageService* storage, CatalogIoOptions options = {}, QObject* parent = nullptr);
  ~CatalogIoService() override;
  quint64 requestReload(CatalogKind kind);
  quint64 requestOfficialUpdate(CatalogKind kind);
  void loadShopSelection();
  void saveShopSelection(const QStringList& selected, const QStringList& excludedAutomatic = {});
  bool shopSelectionEditable() const;
  QString shopSelectionMessage() const;
  CatalogIoStats stats() const;
  BattleReplayCatalogHandle battleReplaySnapshot() const;
  PublicDatasetSnapshotHandle publicDatasetSnapshot() const;
  quint64 requestPublicReload(const QString& acknowledgementAttempt = {});
  quint64 requestPublicCandidate(const QString& attempt);
  quint64 requestPublicRollback();
  void close();

signals:
  void catalogUpdated(CatalogKind kind, quint64 revision);
  void finished(quint64 jobId, CatalogKind kind, StorageStatus status, const QString& error);
  void stateChanged();
  void shopSelectionChanged();
  void publicDatasetChanged(PublicDatasetSnapshotHandle snapshot);
  void publicDatasetFinished(quint64 jobId, bool applied, const QString& message, const QJsonObject& receipt);

private:
  friend struct CatalogIoInternal::State;
  quint64 request(CatalogKind kind, CatalogRequestMode mode);
  quint64 requestPublic(int operation, const QString& attempt = {});
  void pump();
  void receive(std::shared_ptr<CatalogIoInternal::Candidate> candidate);
  void publish(const std::shared_ptr<CatalogIoInternal::Candidate>& candidate);
  void complete(StorageStatus status, const QString& error);
  void publishShopSelection();
  void publishPublicDataset(PublicDatasetSnapshotHandle snapshot);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Q_DECLARE_METATYPE(CatalogKind)
