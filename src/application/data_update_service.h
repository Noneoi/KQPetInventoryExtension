#pragma once

#include <QObject>
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
  bool requestUpdate();
  bool busy() const;
  void close();

signals:
  void progress(const QString& message);
  // Successful components include unchanged components: callers can reload
  // cached defaults that were installed on a first check without a download.
  void finished(bool success, const QStringList& components, const QString& message);

private:
  friend struct DataUpdateInternal::State;
  void receiveProgress(const QString& message);
  void receiveFinished(bool success, const QStringList& components, const QString& message);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
