#pragma once

#include <QMetaType>
#include <QString>
#include <QSize>

enum class WorkbenchPage { Pets = 0, Shop, Routine, Assets };
Q_DECLARE_METATYPE(WorkbenchPage)

struct NavigationTarget {
  WorkbenchPage page = WorkbenchPage::Pets;
  QString account;
  quint64 epoch = 0;
  qint64 petInstanceId = 0;
  QString goodKey;
};
Q_DECLARE_METATYPE(NavigationTarget)

struct WorkbenchSession {
  QString account;
  quint64 epoch = 0;
  QString source;
  QString clientVersion;
  bool sourceVerified = false;
};
Q_DECLARE_METATYPE(WorkbenchSession)

struct WorkbenchTask {
  QString text;
  bool running = false;
  int completed = 0;
  int total = 0;
};
Q_DECLARE_METATYPE(WorkbenchTask)

enum class PersistenceState { Unknown, Pending, Saved, Failed };

struct WorkbenchPersistence {
  PersistenceState state = PersistenceState::Unknown;
  QString detail;
};
Q_DECLARE_METATYPE(WorkbenchPersistence)

struct WorkbenchOptions {
  // Empty uses the current screen. Preview supplies logical geometry so DPI
  // tests do not depend on the offscreen platform's synthetic screen size.
  QSize availableLogicalSize;
  QSize preferredSize{1280, 800};
};
