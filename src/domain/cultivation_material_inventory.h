#pragma once
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <optional>

inline QString cultivationMaterialKey(int type, int id, int extra = 0) {
  const auto key = QStringLiteral("%1:%2").arg(type).arg(id);
  return extra > 0 ? key + QLatin1Char(':') + QString::number(extra) : key;
}

// Account-scoped observations. Absence is zero only for fully decoded groups.
struct MaterialInventorySnapshot {
  QHash<QString,qint64> counts;
  QSet<int> knownTypes;
  QSet<QString> knownExtraGroups;
  QHash<int,QDateTime> observedTimes;
  QDateTime observedAt;
  bool running = false;
};

inline std::optional<qint64> cultivationMaterialOwned(const MaterialInventorySnapshot& stock, int type, int id, int extra) {
  if (type <= 0 || id <= 0 || extra < 0) return {};
  const auto entry = stock.counts.constFind(cultivationMaterialKey(type,id,extra));
  if (entry != stock.counts.cend()) return entry.value() >= 0 ? std::optional<qint64>(entry.value()) : std::nullopt;
  const bool complete = extra == 0 ? stock.knownTypes.contains(type)
      : stock.knownExtraGroups.contains(QStringLiteral("%1:%2").arg(type).arg(extra));
  return complete ? std::optional<qint64>(0) : std::nullopt;
}
