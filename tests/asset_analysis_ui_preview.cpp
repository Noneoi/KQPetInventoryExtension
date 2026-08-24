#include "asset_analysis_controller.h"
#include "asset_analysis_window.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"

#include <QApplication>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>


int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQAssetAnalysisUiPreview"));
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());
  auto* repository = new PetRepository();
  auto deliver = [repository](const QJsonObject& packet) {
    repository->handlePacket(
        QStringLiteral("recivedata"),
        QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
  };
  deliver({{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("preview-account")}}}});
  const QString account = repository->accountKey();
  const quint64 session = repository->sessionGeneration();
  repository->beginListRefresh(1, account, session);
  const QJsonObject previewPet{
      {QStringLiteral("id"), 1001}, {QStringLiteral("r"), 7001},
      {QStringLiteral("fr"), 7001}, {QStringLiteral("n"), QStringLiteral("预览精灵")},
      {QStringLiteral("lv"), 120}, {QStringLiteral("zdl"), 5000},
      {QStringLiteral("xzdl"), 10000}, {QStringLiteral("astrolabebr"), false},
      {QStringLiteral("czdlv"), QJsonObject{{QStringLiteral("bsv"), 10}}},
      {QStringLiteral("mzdlv"), QJsonObject{{QStringLiteral("bsv"), 100}}}};
  deliver({{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"), QJsonArray{previewPet}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("1001")}},
           {QStringLiteral("ppc"), 12}});
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
      QCheckBox* autoSnapshot = window->findChild<QCheckBox*>();
      if (autoSnapshot) autoSnapshot->setChecked(true);
      if (refresh) refresh->click();
      QTableView* diagnostics =
          window->findChild<QTableView*>(QStringLiteral("KQAssetDiagnosticTable"));
      const int rowsBeforeDetail = diagnostics && diagnostics->model()
                                       ? diagnostics->model()->rowCount()
                                       : -1;
      repository->expectDetail(1001, 2, account, session);
      QJsonObject updatedPet = previewPet;
      updatedPet.insert(QStringLiteral("zdl"), 5100);
      deliver({{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
               {QStringLiteral("p"), updatedPet}});
      QCoreApplication::processEvents();
      QLabel* analysisStatus =
          window->findChild<QLabel*>(QStringLiteral("KQAssetAnalysisStatus"));
      const bool staleWasShown =
          analysisStatus && analysisStatus->text().contains(QStringLiteral("1 只详情已变化"));
      if (refresh) refresh->click();
      const bool valid =
          refresh &&
          refresh->text() == QStringLiteral("重新计算养成分析（仅本地）") &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetOverviewTable")) &&
          diagnostics && diagnostics->model() && rowsBeforeDetail == 1 &&
          diagnostics->model()->rowCount() == 1 &&
          staleWasShown && analysisStatus &&
          analysisStatus->text().contains(QStringLiteral("分析状态：最新")) &&
          controller->dirtyPetIds().isEmpty() && controller->snapshots().size() == 1 &&
          window->findChild<QTableView*>(QStringLiteral("KQAssetSnapshotTable")) &&
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
