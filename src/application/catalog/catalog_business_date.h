#pragma once
#include <QDateTime>

// DateUtil evidence uses UTC+8. Without a reviewed server clock anchor this is
// explicitly a local-clock estimate for catalog display, never quota evidence.
inline QDate currentCatalogBusinessDate() {
  return QDateTime::currentDateTimeUtc().addSecs(8 * 60 * 60).date();
}
