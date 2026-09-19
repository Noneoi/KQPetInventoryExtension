#pragma once

#include <QJsonObject>
#include <QObject>
#include <memory>

class StorageService;
namespace CacheManagementInternal { struct State; }

// Explicit cache maintenance. The script, request and subprocess stay on the
// existing Storage I/O event loop; construction does not inspect or change files.
class CacheManagementService final : public QObject {
  Q_OBJECT
public:
  explicit CacheManagementService(StorageService* storage, QString clientRoot, QObject* parent = nullptr);
  ~CacheManagementService() override;
  bool request(QString action, QJsonObject options = {});
  bool busy() const;
  void close();

signals:
  void finished(QString action, QJsonObject result);
  void progress(QString message);

private:
  friend struct CacheManagementInternal::State;
  void receiveFinished(QString action, QJsonObject result);
  void receiveProgress(QString message);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
