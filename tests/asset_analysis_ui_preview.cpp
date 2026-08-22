#include "asset_analysis_controller.h"
#include "asset_analysis_window.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"

#include <QApplication>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQAssetAnalysisUiPreview"));
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());
  auto* repository = new PetRepository();
  auto* shop = new ShopExchangeController(repository);
  auto* routine = new RoutineOverviewController(repository);
  auto* controller = new AssetAnalysisController(repository, shop, routine);
  auto* window = new AssetAnalysisWindow(controller);
  window->setWindowTitle(QStringLiteral("账号资产与养成分析预览（开发测试）"));
  window->show();
  if (qEnvironmentVariableIntValue("KQPET_PREVIEW_SELF_TEST") > 0) {
    QTimer::singleShot(100, &application, [&]() {
      QPushButton* refresh =
          window->findChild<QPushButton*>(QStringLiteral("KQAssetAnalysisRefresh"));
      if (refresh) refresh->click();
      const bool valid =
          refresh &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetOverviewTable")) &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetDiagnosticTable")) &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetSnapshotTable")) &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetInstanceHistoryTable"));
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
  delete controller;
  delete routine;
  delete shop;
  delete repository;
  return result;
}
