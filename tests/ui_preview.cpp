#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "pet_settings_dialog.h"
#include "pet_window.h"

#include <QApplication>
#include <QTimer>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQPetUiPreview"));
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
