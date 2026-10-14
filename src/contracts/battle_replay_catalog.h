#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <memory>

// Immutable after publication. A page retains the handle used to parse its
// current battle even when the shared data service publishes a newer version.
struct BattleReplayCatalogSnapshot {
  QJsonObject root;
  quint64 revision = 0;
  QString sha256;
  QString sourcePath;
  QString sourceLabel;
  QString warning;
  QDateTime loadedAt;
  bool embedded = false;
};
using BattleReplayCatalogHandle = std::shared_ptr<const BattleReplayCatalogSnapshot>;
Q_DECLARE_METATYPE(BattleReplayCatalogHandle)
