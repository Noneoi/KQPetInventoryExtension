#include "move_operation.h"

#include "storage_service.h"
#include "packet_contract.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>
#include <limits>

MoveOperationJournal::MoveOperationJournal(StorageService* storage, StorageContext context)
    : storage_(storage), context_(std::move(context)) {}
MoveOperationJournal::~MoveOperationJournal() = default;

StorageSubmission MoveOperationJournal::begin(quint64 epoch, quint64 inventoryRevision,
    qint64 instanceId, const QList<qint64>& sequence) {
  QSet<qint64> unique;
  for (qint64 id : sequence) {
    if (id <= 0 || unique.contains(id)) {
      error_ = QStringLiteral("invalid or duplicate target sequence instance");
      return {0, false, StorageStatus::InvalidRequest, error_};
    }
    unique.insert(id);
  }
  if (!path_.isEmpty() || !context_ || context_->isShared() || context_->account().isEmpty() ||
      !epoch || !inventoryRevision || instanceId <= 0 || sequence.isEmpty() || sequence.size() > 12) {
    error_ = QStringLiteral("invalid operation intent/context");
    return {0, false, StorageStatus::InvalidRequest, error_};
  }
  operationId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  relativePath_ = QStringLiteral("operations/%1.json").arg(operationId_);
  path_ = QDir(context_->directory()).filePath(relativePath_);
  QJsonArray ids;
  for (qint64 id : sequence) ids.append(QString::number(id));
  record_ = {{QStringLiteral("schema"), 1},
             {QStringLiteral("operationId"), operationId_},
             {QStringLiteral("account"), context_->account()},
             {QStringLiteral("sessionEpoch"), QString::number(epoch)},
             {QStringLiteral("inventoryRevision"), QString::number(inventoryRevision)},
             {QStringLiteral("instanceId"), QString::number(instanceId)},
             {QStringLiteral("targetSequence"), ids},
             {QStringLiteral("submissionIntent"), true},
             {QStringLiteral("outcome"), QStringLiteral("Unknown")},
             {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
  return commit();
}

StorageSubmission MoveOperationJournal::record(MoveOutcome outcome, const QString& reason) {
  if (path_.isEmpty()) return {0, false, StorageStatus::InvalidRequest, QStringLiteral("intent not created")};
  record_.insert(QStringLiteral("outcome"), moveOutcomeName(outcome));
  record_.insert(QStringLiteral("reason"), reason);
  record_.insert(QStringLiteral("observedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  return commit();
}

StorageSubmission MoveOperationJournal::commit() {
  if (!storage_ || !context_) {
    error_ = QStringLiteral("operation storage service is unavailable");
    return {0, false, StorageStatus::Closing, error_};
  }
  if (revision_ == std::numeric_limits<quint64>::max())
    return {0, false, StorageStatus::InvalidRequest, QStringLiteral("operation revision overflow")};
  record_.insert(QStringLiteral("recordRevision"), QString::number(++revision_));
  const StorageSubmission admission = storage_->submitJsonWrite({context_, relativePath_, revision_,
      record_, 1024 * 1024, true});
  error_ = admission.error;
  return admission;
}

QJsonObject MoveOperationJournal::recoveryRecord(const QByteArray& bytes) {
  if (bytes.size() > 1024 * 1024) return {};
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
  QJsonObject record = document.object();
  const QString outcome = record.value(QStringLiteral("outcome")).toString();
  const QString id = record.value(QStringLiteral("operationId")).toString();
  qint64 schema = 0, instanceId = 0;
  if (!PacketContracts::checkedInteger(record.value(QStringLiteral("schema")), &schema, 1, 1) ||
      QUuid(id).isNull() || !record.value(QStringLiteral("account")).isString() ||
      record.value(QStringLiteral("account")).toString().isEmpty() ||
      !PacketContracts::checkedInteger(record.value(QStringLiteral("instanceId")), &instanceId, 1) ||
      !record.value(QStringLiteral("submissionIntent")).isBool() ||
      !record.value(QStringLiteral("submissionIntent")).toBool() ||
      !record.value(QStringLiteral("targetSequence")).isArray() ||
      (outcome != QStringLiteral("Submitted") && outcome != QStringLiteral("Unknown"))) return {};
  const QJsonArray sequence = record.value(QStringLiteral("targetSequence")).toArray();
  if (sequence.isEmpty() || sequence.size() > 12) return {};
  QSet<qint64> unique;
  for (const auto& value : sequence) {
    qint64 instance = 0;
    if (!PacketContracts::checkedInteger(value, &instance, 1) || unique.contains(instance)) return {};
    unique.insert(instance);
  }
  record.insert(QStringLiteral("outcome"), QStringLiteral("Unknown"));
  return record;
}
