#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

// Production ceilings; smaller values also permit deterministic fault tests.
struct DiagnosticLimits {
  qint64 fileBytes = 10 * 1024 * 1024;
  int runVolumes = 5;
  qint64 directoryBytes = 100 * 1024 * 1024;
  int scanEntries = 2048;
  int pendingEvents = 512;
  qint64 pendingBytes = 256 * 1024;
  int recentEvents = 64;
  qint64 recentBytes = 64 * 1024;
  qint64 batchBytes = 32 * 1024;
  int flushMilliseconds = 100;
};

struct DiagnosticStoreResult {
  bool saved = false;
  QString code;
};

// Construct, access and destroy on the StorageService I/O thread only. The
// run lock is an open OS file handle, so a crashed process releases ownership.
// No Qt object, event loop or thread is created by this component.
class DiagnosticStore final {
public:
  DiagnosticStore(QString dataRoot, QString runId, DiagnosticLimits limits = {});
  ~DiagnosticStore();
  DiagnosticStoreResult open();
  DiagnosticStoreResult append(const QByteArray& jsonLines);
  void close();
  QByteArray installationSalt() const;
  QString currentPath() const;
  QString runId() const;
  static DiagnosticLimits boundedLimits(DiagnosticLimits limits);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
