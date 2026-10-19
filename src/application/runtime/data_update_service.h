#pragma once

#include <QObject>
#include <QStringList>
#include <QJsonObject>
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
  bool requestUpdate(const QStringList& components = {}, bool verifyAll = false);
  void requestStatus();
  bool requestAction(const QString& action);
  void applicationFinished(const QJsonObject& report);
  bool busy() const;
  void close();

signals:
  void progress(const QString& message);
  void reportChanged(const QJsonObject& report);
  void prepared(const QJsonObject& report);
  // Successful components include unchanged components: callers can reload
  // cached defaults that were installed on a first check without a download.
  void finished(bool success, const QStringList& components, const QString& message);

private:
  friend struct DataUpdateInternal::State;
  void receiveProgress(const QString& message);
  void receiveFinished(bool success, const QStringList& components, const QString& message);
  void receiveReport(const QJsonObject& report);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
