#pragma once

#include "contracts/public_update_state.h"
#include <QObject>
#include <memory>

class StorageService;

// Persistent public update projection and diagnostics. No account context or
// battle report is accepted by this service; all filesystem reads are queued.
class PublicUpdateService final : public QObject {
  Q_OBJECT
public:
  explicit PublicUpdateService(StorageService* storage, QObject* parent = nullptr);
  ~PublicUpdateService() override;
  PublicUpdateState state() const;
  bool refresh();
  void setSessionRevision(const QString& revision);
  void setBusy(bool busy, bool canCancel = false);
  void observeProgress(const PublicUpdateProgress& progress);
  void observePictures(qint64 completed, qint64 total);
  bool exportDiagnostics(const QString& destination);
  void close();

  // Pure/read-only seams for storage tests; never called on the GUI thread.
  static PublicUpdateState readState(const QString& dataRoot, const QString& sessionRevision);
  static QJsonObject publicDiagnostics(const QString& dataRoot, const QString& sessionRevision);

signals:
  void stateChanged(const PublicUpdateState& state);
  void diagnosticsExported(bool success, const QString& path, const QString& message);

private:
  void acceptState(PublicUpdateState state, quint64 request);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
