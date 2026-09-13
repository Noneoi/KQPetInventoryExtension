#pragma once

#include "storage_service.h"
#include <QObject>
#include <QString>
#include <memory>

enum class CatalogKind { Shop, Routine, PetDetail };
enum class CatalogRequestMode { Reload, OfficialUpdate };

struct CatalogIoOptions {
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
  CatalogIoStats stats() const;
  void close();

signals:
  void catalogUpdated(CatalogKind kind, quint64 revision);
  void finished(quint64 jobId, CatalogKind kind, StorageStatus status, const QString& error);
  void stateChanged();

private:
  friend struct CatalogIoInternal::State;
  quint64 request(CatalogKind kind, CatalogRequestMode mode);
  void pump();
  void receive(std::shared_ptr<CatalogIoInternal::Candidate> candidate);
  void publish(const std::shared_ptr<CatalogIoInternal::Candidate>& candidate);
  void complete(StorageStatus status, const QString& error);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Q_DECLARE_METATYPE(CatalogKind)
