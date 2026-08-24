#include "pet_filter_proxy_model.h"
#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "pet_settings_dialog.h"
#include "pet_window.h"

#include <QApplication>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQPetUiPreview"));
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());
  auto* repository = new PetRepository();
  auto* refreshController = new PetRefreshController(repository);
  auto* window = new PetWindow(repository);
  QObject::connect(window, &PetWindow::settingsRequested, window, [=]() {
    PetSettingsDialog dialog(refreshController->timings(), window);
    if (dialog.exec() == QDialog::Accepted)
      refreshController->setTimings(dialog.timings());
  });
  window->setWindowTitle(QStringLiteral("精灵详情界面预览（开发测试）"));
  window->show();
  if (qEnvironmentVariableIntValue("KQPET_PREVIEW_SELF_TEST") > 0) {
    QTimer::singleShot(100, &application, [&]() {
      auto* backpack = window->findChild<QTableWidget*>(
          QStringLiteral("KQPetBackpackTable"));
      auto* warehouse = window->findChild<QTableView*>(
          QStringLiteral("KQPetWarehouseTable"));
      auto* elite = window->findChild<QTableView*>(
          QStringLiteral("KQPetEliteWarehouseTable"));
      const bool valid = backpack && warehouse && elite &&
                         qobject_cast<PetFilterProxyModel*>(warehouse->model()) &&
                         qobject_cast<PetFilterProxyModel*>(elite->model()) &&
                         backpack->selectionBehavior() == QAbstractItemView::SelectRows &&
                         warehouse->selectionBehavior() == QAbstractItemView::SelectRows &&
                         elite->selectionBehavior() == QAbstractItemView::SelectRows &&
                         backpack->styleSheet().contains(QStringLiteral("#2563eb")) &&
                         warehouse->styleSheet().contains(QStringLiteral("#2563eb")) &&
                         elite->styleSheet().contains(QStringLiteral("#2563eb"));
      application.exit(valid ? 0 : 2);
    });
  }
  bool validExitDelay = false;
  const int exitDelayMs = qEnvironmentVariableIntValue("KQPET_PREVIEW_EXIT_MS",
                                                        &validExitDelay);
  if (validExitDelay && exitDelayMs > 0)
    QTimer::singleShot(exitDelayMs, &application, &QCoreApplication::quit);
  const int result = application.exec();
  delete window;
  delete refreshController;
  delete repository;
  return result;
}
