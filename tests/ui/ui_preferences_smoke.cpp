#include "ui/common/ui_preferences.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

struct Widgets {
  QComboBox sort;
  QTabWidget tabs;
  QSplitter splitter;
  Widgets() {
    sort.addItem(QStringLiteral("默认排序"), 0);
    sort.addItem(QStringLiteral("战斗力"), 1);
    sort.addItem(QStringLiteral("获得时间"), 4);
    tabs.addTab(new QLabel, QStringLiteral("a"));
    tabs.addTab(new QLabel, QStringLiteral("b"));
    tabs.addTab(new QLabel, QStringLiteral("c"));
    splitter.addWidget(new QLabel);
    splitter.addWidget(new QLabel);
    splitter.resize(400, 100);
  }
  void bind() {
    UiPreferences::bindComboBox(&sort, QStringLiteral("test/sort"));
    UiPreferences::bindTabWidget(&tabs, QStringLiteral("test/tab"));
    UiPreferences::bindSplitter(&splitter, QStringLiteral("test/splitter"));
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  bool ok = true;

  // Disabled store: a no-op that returns fallbacks and leaves widgets alone.
  UiPreferences::setFilePath({});
  {
    Widgets widgets;
    widgets.bind();
    widgets.sort.setCurrentIndex(2);
    ok &= require(!UiPreferences::enabled() &&
                  UiPreferences::value(QStringLiteral("test/sort"), 7).toInt() == 7,
                  "a disabled store returned or kept a value");
  }

  QTemporaryDir directory;
  const QString path = QDir(directory.path()).filePath(QStringLiteral("ui-preferences.ini"));
  UiPreferences::setFilePath(path);
  {
    Widgets widgets;
    widgets.bind();
    widgets.sort.setCurrentIndex(2);
    widgets.tabs.setCurrentIndex(1);
    UiPreferences::setIntList(QStringLiteral("test/splitter"), {150, 250});
  }
  UiPreferences::setFilePath({});  // flushes the file
  UiPreferences::setFilePath(path);
  {
    Widgets widgets;
    widgets.bind();
    ok &= require(widgets.sort.currentData().toInt() == 4, "combo box choice was not restored by item data");
    ok &= require(widgets.tabs.currentIndex() == 1, "tab index was not restored");
    ok &= require(UiPreferences::intList(QStringLiteral("test/splitter")) == QList<int>({150, 250}),
                  "splitter sizes did not round-trip");
  }
  // Stale or foreign values must not select anything unexpected.
  UiPreferences::setValue(QStringLiteral("test/tab"), 99);
  UiPreferences::setValue(QStringLiteral("test/sort"), QStringLiteral("unknown"));
  UiPreferences::setValue(QStringLiteral("test/splitter"), QStringLiteral("1,x"));
  {
    Widgets widgets;
    widgets.bind();
    ok &= require(widgets.tabs.currentIndex() == 0 && widgets.sort.currentIndex() == 0,
                  "out-of-range preferences changed the default selection");
    ok &= require(UiPreferences::intList(QStringLiteral("test/splitter")).isEmpty(),
                  "malformed splitter sizes were accepted");
  }
  UiPreferences::setFilePath({});
  return ok ? 0 : 1;
}
