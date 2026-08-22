#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace PetMovePolicy {

QString restriction(const QJsonObject& pet);
QString deploymentText(const QJsonObject& pet);
QList<qint64> parseSequence(const QString& sequence);
QString serializeSequence(const QList<qint64>& ids);
QList<qint64> remove(const QList<qint64>& ids, qint64 instanceId);
QList<qint64> append(const QList<qint64>& ids, qint64 instanceId);
QList<qint64> replace(const QList<qint64>& ids, qint64 outgoingId,
                      qint64 incomingId);

}  // namespace PetMovePolicy
