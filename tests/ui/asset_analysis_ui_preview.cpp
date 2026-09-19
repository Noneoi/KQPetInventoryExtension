#include "support/protocol_test_support.h"
#include "application/analysis/asset_analysis_controller.h"
#include "ui/analysis/asset_analysis_window.h"
#include "ui/analysis/asset_analysis_model.h"
#include "domain/asset_derivation.h"
#include "application/pet/pet_repository.h"
#include "application/routine/routine_overview_controller.h"
#include "application/shop/shop_exchange_controller.h"

#include <QApplication>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>


int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQAssetAnalysisUiPreview"));
  AssetAnalysisModel unknownModel;
  AccountAssetOverview unknownOverview;
  PetAssetRecord incomplete; incomplete.instanceId = 1; incomplete.detailAvailable = true;
  unknownOverview.pets = {incomplete}; unknownModel.setOverview(unknownOverview);
  if (unknownModel.data(unknownModel.index(0, AssetAnalysisModel::CurrentPower)).toString() != QStringLiteral("—") ||
      unknownModel.data(unknownModel.index(0, AssetAnalysisModel::Completion)).toString() != QStringLiteral("—") ||
      unknownModel.data(unknownModel.index(0, AssetAnalysisModel::Completion), Qt::ForegroundRole).isValid() ||
      unknownModel.data(unknownModel.index(0, AssetAnalysisModel::CurrentPower), AssetAnalysisModel::SortRole).toInt() != -1) {
    std::fprintf(stderr, "FAIL: existing detail with unknown values was displayed as known zero/completion\n"); return 9;
  }
  unknownOverview.pets[0].currentPowerKnown = true;
  unknownModel.setOverview(unknownOverview);
  if (unknownModel.data(unknownModel.index(0, AssetAnalysisModel::CurrentPower)).toString() != QStringLiteral("0")) {
    std::fprintf(stderr, "FAIL: explicitly known zero power was hidden\n"); return 9;
  }
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());
  auto* repository = new PetRepository();
  auto deliver = [repository](const QJsonObject& packet) {
    deliverVerifiedFixture(repository, packet);
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
  controller->setCompatibilityIdentity(QStringLiteral("isolated-preview-build"), QStringLiteral("v114-fixture"), true);
  auto* window = new AssetAnalysisWindow(controller);
  window->setWindowTitle(QStringLiteral("账号资产与养成分析预览（开发测试）"));
  window->show();
  if (qEnvironmentVariableIntValue("KQPET_PREVIEW_SELF_TEST") > 0) {
    QTimer::singleShot(100, &application, [&]() {
      QPushButton* refresh =
          window->findChild<QPushButton*>(QStringLiteral("KQAssetAnalysisRefresh"));
      const auto waitUntil = [](const std::function<bool()>& done) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < 3000) {
          QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1);
        }
        return done();
      };
      if (!waitUntil([&] { return controller->autoSnapshotSettingKnown(); })) { application.exit(6); return; }
      QPushButton* localStars = window->findChild<QPushButton*>(QStringLiteral("KQLocalStargodStatistics"));
      QPushButton* cancelLocalStars = window->findChild<QPushButton*>(QStringLiteral("KQLocalStargodStatisticsCancel"));
      QLabel* localStarSummary = window->findChild<QLabel*>(QStringLiteral("KQLocalStargodStatisticsSummary"));
      QObject localStatsProbe;
      int localStatsRequests = 0, localStatsCancellations = 0;
      QObject::connect(window, &AssetAnalysisWindow::localStargodStatisticsRequested, &localStatsProbe,
          [&] { ++localStatsRequests; });
      QObject::connect(window, &AssetAnalysisWindow::localStargodStatisticsCancelled, &localStatsProbe,
          [&] { ++localStatsCancellations; });
      if (!localStars || !cancelLocalStars || !localStarSummary || !cancelLocalStars->isHidden()) {
        std::fprintf(stderr,"FAIL: local red-star controls are missing or idle cancel is visible\n"); application.exit(10); return;
      }
      localStars->click();
      LocalStargodStatistics localStats;
      localStats.account = account; localStats.epoch = session; localStats.running = true;
      localStats.ordinaryEquipped = 4; localStats.ordinaryBackpack = 6;
      localStats.changeableEquipped = 2; localStats.changeableBackpack = 1;
      localStats.scannedFiles = 12; localStats.countedPets = 10;
      window->setLocalStargodStatistics(localStats);
      const bool localStatsRunning = localStatsRequests == 1 && !localStars->isEnabled() &&
          cancelLocalStars->isVisible() &&
          localStarSummary->text().contains(QStringLiteral("普通红星：已确认 10 颗（已装备 4，本宠背包 6）")) &&
          localStarSummary->text().contains(QStringLiteral("万变红星：已确认 3 颗（已装备 2，本宠背包 1）"));
      cancelLocalStars->click();
      localStats.running = false; localStats.completed = true;
      window->setLocalStargodStatistics(localStats);
      if (!localStatsRunning || localStatsCancellations != 1 || !cancelLocalStars->isHidden() || !localStars->isEnabled() ||
          !localStarSummary->text().contains(QStringLiteral("普通红星：共 10 颗")) ||
          !localStarSummary->text().contains(QStringLiteral("万变红星：共 3 颗"))) {
        std::fprintf(stderr,"FAIL: local red-star request, counts or running-only cancel state is incorrect\n"); application.exit(10); return;
      }
      QCheckBox* autoSnapshot = window->findChild<QCheckBox*>(QStringLiteral("KQAutoSnapshot"));
      if (autoSnapshot) autoSnapshot->setChecked(true);
      if (!autoSnapshot || !waitUntil([&] { return controller->autoSnapshotSettingKnown() && controller->autoSnapshotEnabled(); })) {
        application.exit(7); return;
      }
      const auto analyzeAndWait = [&]() {
        if (!refresh) return false;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        bool completed = false;
        QObject::connect(controller, &AssetAnalysisController::analysisCompleted, &loop, [&] {
          completed = true; loop.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(3000);
        refresh->click();
        if (!completed) loop.exec();
        return completed;
      };
      if (!waitForRepositoryIdle(repository) || !analyzeAndWait()) { application.exit(3); return; }
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
      if (!waitForRepositoryIdle(repository)) { application.exit(4); return; }
      QLabel* analysisStatus =
          window->findChild<QLabel*>(QStringLiteral("KQAssetAnalysisStatus"));
      const bool staleWasShown =
          analysisStatus && analysisStatus->text().contains(QStringLiteral("1 只详情已变化"));
      if (!analyzeAndWait()) { application.exit(5); return; }
      if (!waitUntil([&] { return !controller->persistencePendingTaskCount() && !controller->snapshotHistoryLoading(); })) {
        application.exit(8); return;
      }
      // The overview summary recomputes pet-derived counts once per analysis
      // result. A routine-only change must rewrite only its two cells.
      QTableWidget* overviewTable =
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetOverviewTable"));
      const auto overviewValue = [&](const QString& label) -> QString {
        if (!overviewTable) return {};
        for (int row = 0; row < overviewTable->rowCount(); ++row)
          if (overviewTable->item(row, 0) && overviewTable->item(row, 0)->text() == label)
            return overviewTable->item(row, 1) ? overviewTable->item(row, 1)->text() : QString{};
        return {};
      };
      const quint64 scansAfterAnalysis = window->overviewPetScanCount();
      const int overviewRows = overviewTable ? overviewTable->rowCount() : 0;
      const QVariant firstAction =
          overviewTable && overviewTable->item(0, 0) ? overviewTable->item(0, 0)->data(Qt::UserRole) : QVariant{};
      const QString upgradeCount = overviewValue(QStringLiteral("星神等级待提升"));
      const QString routineDaily = overviewValue(QStringLiteral("今日任务 / 玩法剩余"));
      QMetaObject::invokeMethod(window, "refreshRoutineSummary", Qt::DirectConnection);
      int expectedUpgrade = 0;
      for (const PetAssetRecord& record : controller->overview().pets)
        if (AssetDerivation::matchesFilter(record, PetAssetFilter::StargodUpgradeNeeded)) ++expectedUpgrade;
      const bool routineOnly = overviewTable && overviewRows > 0 &&
          window->overviewPetScanCount() == scansAfterAnalysis &&
          overviewTable->rowCount() == overviewRows &&
          overviewTable->item(0, 0)->data(Qt::UserRole) == firstAction &&
          overviewValue(QStringLiteral("星神等级待提升")) == upgradeCount &&
          overviewValue(QStringLiteral("今日任务 / 玩法剩余")) == routineDaily &&
          upgradeCount == QString::number(expectedUpgrade);
      // A new analysis result invalidates the cache and rescans exactly once.
      const quint64 scansBeforeThird = window->overviewPetScanCount();
      const bool thirdAnalysis = analyzeAndWait() &&
          window->overviewPetScanCount() == scansBeforeThird + 1;
      // A session reset clears the rows and must not let a routine update
      // resurrect them before the next analysis.
      window->resetSessionContext();
      const bool clearedRows = overviewTable && overviewTable->rowCount() == 0;
      QMetaObject::invokeMethod(window, "refreshRoutineSummary", Qt::DirectConnection);
      const bool resetKept = clearedRows && overviewTable && overviewTable->rowCount() == 0;
      const bool restored = resetKept && analyzeAndWait() && overviewTable &&
          overviewTable->rowCount() == overviewRows;
      const bool overviewLocalUpdate = routineOnly && thirdAnalysis && restored;
      if (!overviewLocalUpdate) {
        std::fprintf(stderr,
            "FAIL: overview scans=%llu rows=%d upgrade=%s expected=%d routineOnly=%d third=%d restored=%d\n",
            static_cast<unsigned long long>(window->overviewPetScanCount()), overviewRows,
            qPrintable(upgradeCount), expectedUpgrade, int(routineOnly), int(thirdAnalysis),
            int(restored));
      }
      QTabWidget* recommendationTabs =
          window->findChild<QTabWidget*>(QStringLiteral("KQRecommendationTabs"));      QTableView* readyRecommendations =
          window->findChild<QTableView*>(QStringLiteral("KQReadyRecommendationTable"));
      QTableView* missingRecommendations =
          window->findChild<QTableView*>(QStringLiteral("KQMissingRecommendationTable"));
      QTableView* nearFullRecommendations =
          window->findChild<QTableView*>(QStringLiteral("KQNearFullRecommendationTable"));
      const bool valid =
          refresh &&
          refresh->text() == QStringLiteral("重新计算养成分析（仅本地）") &&
          overviewLocalUpdate &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetOverviewTable")) &&
          diagnostics && diagnostics->model() && rowsBeforeDetail == 1 &&
          diagnostics->model()->rowCount() == 1 &&
          staleWasShown && analysisStatus &&
          analysisStatus->text().contains(QStringLiteral("分析状态：最新")) &&
          controller->dirtyPetIds().isEmpty() && controller->snapshots().size() == 1 &&
          window->findChild<QTableView*>(QStringLiteral("KQAssetSnapshotTable")) &&
          recommendationTabs && recommendationTabs->count() == 3 &&
          readyRecommendations && missingRecommendations &&
          nearFullRecommendations &&
          readyRecommendations->horizontalScrollBarPolicy() ==
              Qt::ScrollBarAlwaysOff &&
          missingRecommendations->horizontalScrollBarPolicy() ==
              Qt::ScrollBarAlwaysOff &&
          nearFullRecommendations->horizontalScrollBarPolicy() ==
              Qt::ScrollBarAlwaysOff &&
          window->findChild<QCheckBox*>(QStringLiteral("KQShowAllRecommendations")) &&
          window->findChild<QTableWidget*>(QStringLiteral("KQAssetInstanceHistoryTable"));
      application.exit(valid ? 0 : 2);
      if (!valid) std::fprintf(stderr, "FAIL: rows=%d stale=%d dirty=%lld history=%lld status=%s\n",
          rowsBeforeDetail, staleWasShown, static_cast<long long>(controller->dirtyPetIds().size()),
          static_cast<long long>(controller->snapshots().size()), analysisStatus ? analysisStatus->text().toUtf8().constData() : "missing");
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
