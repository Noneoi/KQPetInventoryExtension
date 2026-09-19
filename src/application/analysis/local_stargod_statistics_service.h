#pragma once
#include "contracts/local_stargod_statistics.h"
#include "storage/storage_write_context.h"
#include <QJsonObject>
#include <QObject>
#include <memory>

class StorageService;
namespace LocalStargodStatisticsInternal { struct State; }

class LocalStargodStatisticsService final : public QObject {
  Q_OBJECT
public:
  explicit LocalStargodStatisticsService(StorageService* storage, QObject* parent = nullptr);
  ~LocalStargodStatisticsService() override;
  bool request(StorageContext context, quint64 epoch, QJsonObject stargods);
  bool busy() const;
  void cancel();
  void close();
signals:
  void updated(const LocalStargodStatistics& result);
private:
  friend struct LocalStargodStatisticsInternal::State;
  void receive(quint64 generation, LocalStargodStatistics result);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
