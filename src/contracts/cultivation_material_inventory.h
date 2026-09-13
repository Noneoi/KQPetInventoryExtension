#pragma once
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>

inline QString cultivationMaterialKey(int type, int id, int extra = 0) {
  const auto key = QStringLiteral("%1:%2").arg(type).arg(id);
  return extra > 0 ? key + QLatin1Char(':') + QString::number(extra) : key;
}

// Read-only, account-scoped observations. An explicit count remains useful
// offline; missing keys mean zero only for completely decoded inventory groups.
struct MaterialInventorySnapshot {
  QHash<QString, qint64> counts;
  QSet<int> knownTypes;
  QSet<QString> knownExtraGroups; // Explicitly confirmed variants, e.g. "24:1".
  QHash<int, QDateTime> observedTimes;
  QDateTime observedAt;
  bool running = false;
};
