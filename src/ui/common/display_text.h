#pragma once

#include <QJsonObject>
#include <QString>

// Small display-text helpers shared by several UI pages, so every page renders
// the same value the same way.

// The name the server sent for a pet, with a visible placeholder when absent.
// Deliberately not petDisplayName(), which prefers the player's own nickname:
// the pet list column and the pet window both show the server's name so that
// searching by it keeps working.
inline QString petServerName(const QJsonObject& pet) {
  const QString name = pet.value(QStringLiteral("n")).toString();
  return name.isEmpty() ? QStringLiteral("未返回名称") : name;
}

// Signed delta text: "+3", "0", "-2".
inline QString signedNumberText(qint64 value) {
  return value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
}
