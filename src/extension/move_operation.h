#pragma once

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QByteArray>
#include <QPointer>
#include <QString>
#include <memory>
#include "../application/contracts/operation_types.h"

class StorageService;
class StorageWriteContext;
struct StorageSubmission;
using StorageContext = std::shared_ptr<const StorageWriteContext>;

// An operation freezes its original account directory. A persisted intent is
// Unknown after a crash and is never automatically replayed.
class MoveOperationJournal final {
public:
  MoveOperationJournal(StorageService* storage, StorageContext context);
  ~MoveOperationJournal();
  StorageSubmission begin(quint64 epoch, quint64 inventoryRevision, qint64 instanceId,
                          const QList<qint64>& sequence);
  StorageSubmission record(MoveOutcome outcome, const QString& reason = {});
  QString operationId() const { return operationId_; }
  QString path() const { return path_; }
  QString error() const { return error_; }
  quint64 revision() const { return revision_; }
  StorageContext context() const { return context_; }
  QJsonObject snapshot() const { return record_; }
  // Recovery consumes bytes read by StorageService; it never scans disk or
  // replays a pending write. Completed records return an empty object.
  static QJsonObject recoveryRecord(const QByteArray& bytes);

private:
  StorageSubmission commit();
  QPointer<StorageService> storage_;
  StorageContext context_;
  QString relativePath_;
  quint64 revision_ = 0;
  QString operationId_;
  QString path_;
  QString error_;
  QJsonObject record_;
};
