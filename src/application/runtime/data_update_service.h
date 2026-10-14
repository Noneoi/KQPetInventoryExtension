#pragma once

#include <QObject>
#include "contracts/public_update_state.h"
#include <QStringList>
#include <memory>

class StorageService;
namespace DataUpdateInternal { struct State; }

// Explicitly started, one-shot public updates. Setup and QProcess live on the
// existing Storage I/O event loop; this facade never downloads on construction.
class DataUpdateService final : public QObject {
  Q_OBJECT
public:
  explicit DataUpdateService(StorageService* storage, QObject* parent = nullptr);
  ~DataUpdateService() override;
  // Public data parts that can be updated on their own.
  static QStringList componentNames();
  // Empty checks every part; otherwise only the named parts (see
  // componentNames()). Unknown names are rejected without starting anything.
  bool requestUpdate(const QStringList& components = {});
  bool busy() const;
  bool canCancel() const;
  bool cancelPreparation();
  // Called only after the complete candidate was published (or rejected) by
  // CatalogIoService. Preparing Python output is not an applied update.
  void completePublication(bool applied, const QString& message);
  void close();

signals:
  void progress(const QString& message);
  void progressChanged(const PublicUpdateProgress& progress);
  // Emitted after whole-version publication (or a terminal preparation failure).
  // Consumers receive the complete snapshot through CatalogIoService.
  void finished(bool success, const QStringList& components, const QString& message);
  void candidatePrepared(const QString& attempt, const QString& message);

private:
  friend struct DataUpdateInternal::State;
  void receiveProgress(const QString& message);
  void receiveProgressState(const PublicUpdateProgress& progress);
  void tryStart();
  void receiveFinished(bool success, const QStringList& components, const QString& message, const QString& attempt);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
