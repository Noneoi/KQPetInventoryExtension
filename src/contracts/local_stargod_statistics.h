#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>

struct LocalStargodStatistics {
  QString account;
  quint64 epoch = 0;
  bool running = false;
  bool completed = false;
  bool cancelled = false;
  QString error;
  qint64 ordinaryEquipped = 0;
  qint64 ordinaryBackpack = 0;
  qint64 changeableEquipped = 0;
  qint64 changeableBackpack = 0;
  qint64 scannedFiles = 0;
  qint64 countedPets = 0;
  qint64 incompletePets = 0;
  qint64 skippedFiles = 0;
  qint64 duplicatePets = 0;
  qint64 unknownEntries = 0;
  QDateTime finishedAt;
};
Q_DECLARE_METATYPE(LocalStargodStatistics)
