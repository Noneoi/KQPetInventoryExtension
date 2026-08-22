#include "routine_overview_catalog.h"
#include "routine_overview_window.h"

#include <QApplication>
#include <QJsonArray>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQRoutineUiPreview"));
  QTemporaryDir dataRoot;
  RoutineOverviewCatalog::instance().updateFromOfficialData(dataRoot.path());

  auto* window = new RoutineOverviewWindow();
  window->setData(
      {{QStringLiteral("av"), 85}, {QStringLiteral("wav"), 574},
       {QStringLiteral("bi"), QJsonArray{true, true, false, false, false}},
       {QStringLiteral("wbi"), QJsonArray{true, false, false, false, false}},
       {QStringLiteral("ti"), QJsonArray{1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1}},
      {QStringLiteral("wti"), QJsonArray{7, 6, 12, 25, 12, 5, 90, 6, 13, 7, 9, 1, 0}}},
      true, QSet<int>{10011, 10025, 10037}, true,
      {{QStringLiteral("1008_20220603_swa_0_0"),
        QJsonObject{{QStringLiteral("ti"), 2}, {QStringLiteral("wgt"), 4}}},
       {QStringLiteral("16_24_A"),
        QJsonObject{{QStringLiteral("sweep"), 3},
                    {QStringLiteral("zao1"),
                     QJsonObject{{QStringLiteral("curz"), 1}, {QStringLiteral("ct"), 5},
                                 {QStringLiteral("bct"), 1}}},
                    {QStringLiteral("zao2"),
                     QJsonObject{{QStringLiteral("curz"), 2}, {QStringLiteral("ct"), 1},
                                 {QStringLiteral("bct"), 0}}}}},
       {QStringLiteral("100_13_0"), QJsonObject{{QStringLiteral("pt"), 1}}},
       {QStringLiteral("100_2_0"), QJsonObject{{QStringLiteral("rfc"), 4}}}});
  window->setStatus(QStringLiteral("界面预览：全部数据仅为模拟，不会发送网络请求"));
  window->show();

  if (qEnvironmentVariableIntValue("KQPET_PREVIEW_SELF_TEST") > 0) {
    QTimer::singleShot(0, &application, [&]() {
      const QList<QTableWidget*> tables = window->findChildren<QTableWidget*>();
      bool valid = tables.size() == 4;
      for (QTableWidget* table : tables) valid = valid && table->rowCount() > 0;
      auto* daily = window->findChild<QTableWidget*>(QStringLiteral("KQRoutineDailyTable"));
      auto* weekly = window->findChild<QTableWidget*>(QStringLiteral("KQRoutineWeeklyTable"));
      auto* activity = window->findChild<QTableWidget*>(QStringLiteral("KQRoutineActivityTable"));
      auto* opportunity = window->findChild<QTableWidget*>(QStringLiteral("KQRoutineOpportunityTable"));
      valid = valid && daily && weekly && daily->columnCount() == 6 &&
              weekly->columnCount() == 6 && activity && activity->columnCount() == 6 &&
              opportunity && opportunity->columnCount() == 7 &&
              opportunity->rowCount() == 8 &&
              opportunity->item(0, 2) &&
              opportunity->item(0, 2)->text() == QStringLiteral("2") &&
              daily->item(0, 4) &&
              daily->item(0, 4)->text() == QStringLiteral("0");
      application.exit(valid ? 0 : 2);
    });
  }
  bool validDelay = false;
  const int delay = qEnvironmentVariableIntValue("KQPET_PREVIEW_EXIT_MS", &validDelay);
  if (validDelay && delay > 0)
    QTimer::singleShot(delay, &application, &QCoreApplication::quit);
  const int result = application.exec();
  delete window;
  return result;
}
