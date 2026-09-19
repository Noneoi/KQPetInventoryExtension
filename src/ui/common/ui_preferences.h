#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariant>

class QComboBox;
class QSplitter;
class QTabWidget;

// Remembers purely presentational choices (sort, filters, tabs, splitters,
// window size) across restarts in <data-root>/ui-preferences.ini.
//
// GUI thread only. Until setFilePath() receives a path every call is a no-op
// and value() returns the fallback, so previews and tests stay deterministic.
// Nothing account-specific or game-derived belongs here.
namespace UiPreferences {

void setFilePath(const QString& path);
bool enabled();
QVariant value(const QString& key, const QVariant& fallback = {});
void setValue(const QString& key, const QVariant& value);
void sync();

// Restores the saved index (by item data when present, otherwise by text) and
// saves every later user change. Items must already be populated.
void bindComboBox(QComboBox* box, const QString& key);
// Restores the saved tab index and saves later changes.
void bindTabWidget(QTabWidget* tabs, const QString& key);
// Restores saved sizes when the pane count matches; saves after the user drags.
void bindSplitter(QSplitter* splitter, const QString& key);

QList<int> intList(const QString& key);
void setIntList(const QString& key, const QList<int>& values);

}  // namespace UiPreferences
