#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace PetMovePolicy {

// Sanity bound for a backpack sequence in a move request or intent journal.
// The real limit is the server-reported capacity (ppc), which the move
// preflight enforces; real backpacks hold far more than the 12 pets one UI
// page shows, so this only rejects obviously malformed input.
constexpr int kMaxSequenceInstances = 1000;

QString restriction(const QJsonObject& pet);
QString deploymentText(const QJsonObject& pet);
QList<qint64> parseSequence(const QString& sequence);
QString serializeSequence(const QList<qint64>& ids);
QList<qint64> remove(const QList<qint64>& ids, qint64 instanceId);
QList<qint64> append(const QList<qint64>& ids, qint64 instanceId);
QList<qint64> replace(const QList<qint64>& ids, qint64 outgoingId,
                      qint64 incomingId);

}  // namespace PetMovePolicy
