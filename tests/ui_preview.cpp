#include "pet_filter_proxy_model.h"
#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "pet_settings_dialog.h"
#include "pet_window.h"
#include "preview_inventory_support.h"

#include <QApplication>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextBrowser>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQPetUiPreview"));
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());
  PreviewInventory::Fixture fixture(dataRoot.path(),QStringLiteral("synthetic-pet-preview"));
  if (!fixture.initialized) return 1;
  const auto backpackPet = PreviewInventory::pagedPet(10001,7115,QStringLiteral("离线分页样例 A"),100,22000);
  const auto warehousePet = PreviewInventory::pagedPet(10002,7185,QStringLiteral("离线分页样例 B"),100,20000);
  const auto elitePet = PreviewInventory::pagedPet(10003,7115,QStringLiteral("离线分页样例 C"),80,18000);
  if (!fixture.publishLists({backpackPet},{PreviewInventory::brief(warehousePet)},
      {PreviewInventory::brief(elitePet)},{warehousePet,elitePet}) || !fixture.factsReady()) return 1;
  auto* repository = &fixture.repository;
  auto* refreshController = new PetRefreshController(repository);
  auto* window = new PetWindow(&fixture.inventory);
  int gameRequests = 0;
  QObject::connect(window,&PetWindow::detailRequested,window,[&](qint64) { ++gameRequests; });
  QObject::connect(window,&PetWindow::listRefreshRequested,window,[&] { ++gameRequests; });
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
      bool valid = backpack && warehouse && elite &&
                         qobject_cast<PetFilterProxyModel*>(warehouse->model()) &&
                         qobject_cast<PetFilterProxyModel*>(elite->model()) &&
                         backpack->selectionBehavior() == QAbstractItemView::SelectRows &&
                         warehouse->selectionBehavior() == QAbstractItemView::SelectRows &&
                         elite->selectionBehavior() == QAbstractItemView::SelectRows &&
                         backpack->styleSheet().contains(QStringLiteral("#2563eb")) &&
                         warehouse->styleSheet().contains(QStringLiteral("#2563eb")) &&
                         elite->styleSheet().contains(QStringLiteral("#2563eb"));
      window->focusPet(10001);
      auto* detail = window->findChild<QTextBrowser*>(QStringLiteral("KQPetPreparedDetail"));
      valid &= fixture.preparedVisible(detail,0,10001);
      const int requestsBeforePages = gameRequests;
      const auto computations = fixture.derivations.stats().computations;
      valid &= fixture.followPage(detail,0,10001,DetailSection::StargodBackpack,1) &&
          fixture.followPage(detail,0,10001,DetailSection::Overview,0) &&
          gameRequests==requestsBeforePages && fixture.derivations.stats().computations==computations;
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
  return fixture.close() ? result : 3;
}
