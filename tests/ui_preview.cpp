#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "pet_settings_dialog.h"
#include "pet_window.h"

#include <QApplication>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQPetUiPreview"));
  PetRepository repository;
  PetRefreshController refreshController(&repository);
  PetWindow window(&repository);
  QObject::connect(&window, &PetWindow::settingsRequested, &window, [&]() {
    PetSettingsDialog dialog(refreshController.timings(), &window);
    if (dialog.exec() == QDialog::Accepted)
      refreshController.setTimings(dialog.timings());
  });
  window.setWindowTitle(QStringLiteral("精灵详情界面预览（开发测试）"));
  window.show();
  return application.exec();
}
