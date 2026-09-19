#pragma once

#include "storage/storage_types.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

class PetRepository;
class StorageService;

class AssetAnalysisSettings final : public QObject {
  Q_OBJECT
public:
  explicit AssetAnalysisSettings(PetRepository* repository, QObject* parent = nullptr);
  ~AssetAnalysisSettings() override;

  bool loadAutoSnapshot(const QString& account) const;
  bool known(const QString& account) const;
  bool pending(const QString& account) const;
  int pendingTaskCount() const;
  int pendingWriteCount() const {
    int count = 0;
    for (const auto& task : tasks_) if (task.write) ++count;
    return count;
  }
  StorageSubmission saveAutoSnapshot(const QString& account, bool enabled);

public slots:
  void requestLoad(const QString& account);

signals:
  void stateChanged(const QString& account, quint64 epoch, bool known,
                    bool enabled, bool pending, const QString& error);
  void writeFinished(quint64 taskId, const QString& account, quint64 epoch,
                     bool enabled, StorageStatus status, const QString& error);

private:
  struct Pending {
    QString account;
    quint64 epoch = 0;
    quint64 generation = 0;
    quint64 memoryRevision = 0;
    bool write = false;
    bool enabled = false;
  };
  void beginLoad();
  void receive(const StorageResult& result);
  void publish(const QString& error = {});
  void switchAccount(const QString& account, quint64 epoch);
  QPointer<PetRepository> repository_;
  QPointer<StorageService> storage_;
  StorageContext readContext_;
  QTimer retryTimer_;
  QHash<quint64, Pending> tasks_;
  QString account_;
  quint64 epoch_ = 0;
  quint64 generation_ = 0;
  quint64 memoryRevision_ = 0;
  quint64 loadRevision_ = 0;
  quint64 nextWriteRevision_ = 0;
  bool known_ = false;
  bool enabled_ = false;
  bool loading_ = false;
  bool saving_ = false;
  bool loadWanted_ = false;
};
