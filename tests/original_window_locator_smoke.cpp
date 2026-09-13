#include "original_window_locator.h"

#include <QApplication>
#include <QDialog>
#include <QPointer>
#include <QPushButton>
#include <QTabBar>

#include <cstdio>

class StartupDialog final : public QWidget { Q_OBJECT };

namespace {
int failures = 0;
void check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}
void settle() {
  QApplication::processEvents();
  QApplication::processEvents();
}
void show(QWidget& widget, const QString& title, const QSize& size) {
  widget.setWindowTitle(title);
  widget.resize(size);
  widget.show();
}
}

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  application.setQuitOnLastWindowClosed(false);
  QWidget unrelated;
  show(unrelated, QStringLiteral("Unrelated browser"), QSize(1800, 1400));
  QWidget splash(nullptr, Qt::SplashScreen);
  show(splash, QStringLiteral("氪奇PRO-V1.1.4"), QSize(2000, 1600));
  QDialog dialog;
  show(dialog, QStringLiteral("氪奇PRO-V1.1.4"), QSize(1700, 1300));
  StartupDialog startup;
  show(startup, QStringLiteral("氪奇PRO-V1.1.4"), QSize(800, 400));
  QWidget extension;
  extension.setObjectName(QStringLiteral("KQPetInventoryWindow"));
  show(extension, QStringLiteral("氪奇PRO 扩展"), QSize(1600, 1200));

  OriginalWindowLocator locator;
  int bindings = 0;
  int layouts = 0;
  QPointer<QPushButton> entry;
  QObject::connect(&locator, &OriginalWindowLocator::windowChanged,
      &application, [&](QWidget* window) {
        if (!window) return;
        ++bindings;
        entry = new QPushButton(QStringLiteral("Entry"), window);
        entry->setObjectName(QStringLiteral("TestExtensionEntry"));
      });
  QObject::connect(&locator, &OriginalWindowLocator::layoutNeeded,
      &application, [&] {
        ++layouts;
        if (entry) {
          entry->show();
          entry->raise();
        }
      });
  locator.start();
  settle();
  check(!locator.window() && bindings == 0, "splash, dialog or unrelated window was selected");

  auto* mainWindow = new QWidget;
  show(*mainWindow, QString(), QSize(1100, 800));
  locator.refresh();
  check(!locator.window(), "a window without client identity was selected");
  mainWindow->setWindowTitle(QStringLiteral("氪奇PRO-V1.1.4"));
  locator.refresh();
  settle();
  check(locator.window() == mainWindow && bindings == 1,
        "delayed main-window title was not discovered");
  for (int attempt = 0; attempt < 4; ++attempt) locator.refresh();
  settle();
  check(bindings == 1 && mainWindow->findChildren<QPushButton*>(
      QStringLiteral("TestExtensionEntry")).size() == 1,
        "repeated refresh duplicated extension entries");
  const int settledLayouts = layouts;
  settle();
  check(layouts == settledLayouts, "raising an entry caused an event-filter layout loop");
  auto* laterChild = new QWidget(mainWindow);
  laterChild->show();
  settle();
  check(layouts > settledLayouts, "a later native-host child did not schedule entry layout");
  mainWindow->hide();
  locator.refresh();
  settle();
  check(!mainWindow->isVisible(), "refresh forced a hidden client visible");
  mainWindow->show();

  QTabBar navigation(mainWindow);
  for (const auto* title : {"广告", "资源", "日常", "刷关", "活动", "其它"})
    navigation.addTab(QString::fromUtf8(title));
  navigation.setGeometry(600, 60, 400, 30);
  navigation.show();
  settle();
  auto geometry = OriginalWindowLocator::singleEntryGeometry(mainWindow);
  const int resourceCenter = navigation.mapTo(mainWindow, QPoint()).x() +
                             navigation.tabRect(1).center().x();
  check(qAbs(geometry.center().x() - resourceCenter) <= 1 && geometry.bottom() < navigation.y() &&
        geometry.height() >= 28,
        "single entry was not anchored above the actual resources tab");
  mainWindow->resize(320, 240);
  geometry = OriginalWindowLocator::singleEntryGeometry(mainWindow);
  check(mainWindow->rect().contains(geometry), "small-window single entry was clipped");
  // Detach this stack widget before destroying its synthetic parent.
  navigation.setParent(nullptr);
  navigation.hide();
  delete mainWindow;
  settle();
  check(!locator.window() && !entry, "destroyed main window left dangling entry ownership");
  QWidget replacement;
  replacement.setObjectName(QStringLiteral("KQProV1"));
  show(replacement, QString(), QSize(1100, 800));
  locator.refresh();
  settle();
  check(locator.window() == &replacement && bindings == 2 && entry,
        "replacement main window did not receive exactly one entry binding");
  if (failures) return 1;
  std::puts("PASS: main-window identity, delayed attachment, replacement and entry positioning");
  return 0;
}

#include "original_window_locator_smoke.moc"
