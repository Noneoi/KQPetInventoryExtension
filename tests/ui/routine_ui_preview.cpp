#include "application/catalog/routine_overview_catalog.h"
#include "ui/routine/routine_overview_window.h"
#include "support/catalog_test_support.h"

#include <QApplication>
#include <QJsonArray>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QLabel>
#include <QBrush>
#include <limits>
#include <cstdio>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQRoutineUiPreview"));
  QTemporaryDir dataRoot;
  QTemporaryDir officialRoot;
  if (!writeRoutineOfficialFixture(officialRoot.path())) return 1;
  StorageService storage(dataRoot.path());
  CatalogIoOptions options;
  options.officialRoot = officialRoot.path();
  CatalogIoService catalogIo(&storage, options);
  if (!runCatalogRequest(&catalogIo, CatalogKind::Routine, CatalogRequestMode::OfficialUpdate)) return 1;

  auto* window = new RoutineOverviewWindow();
  window->setCatalogSnapshot(RoutineOverviewCatalog::instance().snapshot());
  window->setData(
      {{QStringLiteral("av"), 85}, {QStringLiteral("wav"), 574},
       {QStringLiteral("bi"), QJsonArray{true, true, false, false, false}},
       {QStringLiteral("wbi"), QJsonArray{true, false, false, false, false}},
       {QStringLiteral("ti"), QJsonArray{1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1}},
      {QStringLiteral("wti"), QJsonArray{7, 6, 12, 25, 12, 5, 90, 6, 13, 7, 9, 1, 0}}},
      true, QSet<int>{10011, 10025, 10037}, true,
      {{QStringLiteral("1008_20220603_swa_0_0"),
        QJsonObject{{QStringLiteral("ti"), 2}, {QStringLiteral("wgt"), 4}}},
       {QStringLiteral("1008_20190531_gbt_1"), QJsonObject{{QStringLiteral("ti"), 5}}},
       {QStringLiteral("2_36_1"), QJsonObject{{QStringLiteral("t"), 6}}},
       {QStringLiteral("110_123_0"),
        QJsonObject{{QStringLiteral("rwwt"), 18}, {QStringLiteral("wwt"), 2},
                    {QStringLiteral("rdt"), 4}, {QStringLiteral("rdb"), 0}}},
       {QStringLiteral("1008_20260522_nf_0"),
        QJsonObject{{QStringLiteral("pt"), 8}, {QStringLiteral("rft"), 16}}},
       {QStringLiteral("16_24_A"),
        QJsonObject{{QStringLiteral("zao1"),
                     QJsonObject{{QStringLiteral("ct"), 2}, {QStringLiteral("bct"), 0}}},
                    {QStringLiteral("zao2"),
                     QJsonObject{{QStringLiteral("ct"), 3}, {QStringLiteral("bct"), 1}}}}}});
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
              opportunity->rowCount() == 11 &&
              opportunity->item(0, 2) &&
              opportunity->item(0, 2)->text() == QStringLiteral("2") &&
              opportunity->item(9, 0) &&
              opportunity->item(9, 0)->text().contains(QStringLiteral("经典竞技场")) &&
              opportunity->item(9, 2)->text() == QStringLiteral("6") &&
              opportunity->item(10, 0)->text().contains(QStringLiteral("传奇竞技场")) &&
              opportunity->item(10, 2)->text() == QStringLiteral("6") &&
              daily->item(0, 4) &&
              daily->item(0, 4)->text() == QStringLiteral("0");
      const auto check = [&](bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
        valid = valid && condition;
      };
      if (!valid) { application.exit(2); return; }
      check(daily->item(0, 1)->text().contains(QStringLiteral("周期未确认")) &&
            daily->item(0, 1)->foreground().style() == Qt::NoBrush &&
            opportunity->item(0, 2)->foreground().style() == Qt::NoBrush,
            "unproved periods displayed green actionable/completed state");
      ObservationValidity current;
      current.state = ObservationValidityState::Current;
      current.periodId = QStringLiteral("synthetic-ui-window");
      current.evidenceReference = QStringLiteral("isolated display fixture, not server evidence");
      current.observedAtUtc = QDateTime::fromString(QStringLiteral("2026-09-12T00:00:00Z"), Qt::ISODate);
      QHash<QString, ObservationValidity> validity;
      for (const QString& key : {QStringLiteral("ti:daily"), QStringLiteral("av:daily"), QStringLiteral("bi:daily"),
          QStringLiteral("wti:weekly"), QStringLiteral("wav:weekly"), QStringLiteral("wbi:weekly"), QStringLiteral("rs:activity"),
          QStringLiteral("110_123_0:activity"), QStringLiteral("1008_20220603_swa_0_0:activity"),
          QStringLiteral("1008_20260522_nf_0:activity"), QStringLiteral("16_24_A:zao1:activity"), QStringLiteral("16_24_A:zao2:activity")})
        validity.insert(key, current);
      window->setPeriodValidity(validity);
      check(daily->item(0, 1)->text() == QStringLiteral("已完成") &&
            daily->item(0, 1)->foreground().color() == QColor(QStringLiteral("#087a43")),
            "verified current progress did not preserve completed display");
      auto expired = current; expired.state = ObservationValidityState::Invalidated;
      expired.reason = QStringLiteral("fixture invalidation"); validity[QStringLiteral("ti:daily")] = expired;
      window->setPeriodValidity(validity);
      check(daily->item(0, 1)->text().contains(QStringLiteral("已失效")) &&
            daily->item(0, 1)->foreground().style() == Qt::NoBrush,
            "invalidated progress retained its completed color");
      window->setData({{QStringLiteral("av"), 100}, {QStringLiteral("ti"), QJsonArray{0.5}},
                       {QStringLiteral("wti"), QJsonArray{QStringLiteral("bad")}}}, true, {}, true,
          {{QStringLiteral("110_123_0"), QJsonObject{{QStringLiteral("rdt"), 1},
              {QStringLiteral("rdb"), std::numeric_limits<int>::max()}, {QStringLiteral("rwwt"), 4}}},
           {QStringLiteral("1008_20220603_swa_0_0"), QJsonObject{{QStringLiteral("ti"), 2}, {QStringLiteral("wgt"), QStringLiteral("bad")}}},
           {QStringLiteral("1008_20260522_nf_0"), QJsonObject{{QStringLiteral("pt"), 5}}},
           {QStringLiteral("16_24_A"), QJsonObject{{QStringLiteral("zao1"), QJsonObject{{QStringLiteral("ct"), 1},
               {QStringLiteral("bct"), std::numeric_limits<int>::max()}}},
               {QStringLiteral("zao2"), QJsonObject{{QStringLiteral("ct"), 1}, {QStringLiteral("bct"), 0}}}}}});
      check(daily->item(0, 2)->text() == QStringLiteral("—") && daily->item(0, 4)->text() == QStringLiteral("—") &&
            weekly->item(0, 2)->text() == QStringLiteral("—"), "invalid/missing task counters became numeric zero");
      auto* summary = window->findChild<QLabel*>(QStringLiteral("KQRoutineDailySummary"));
      check(summary && !summary->text().contains(QStringLiteral("可领取")) && summary->text().contains(QStringLiteral("领取状态未知")),
            "missing claim flags were treated as unclaimed rewards");
      check(opportunity->rowCount() == 11 && opportunity->item(4, 2)->text() == QStringLiteral("—") &&
            opportunity->item(5, 2)->text() == QStringLiteral("4") && opportunity->item(0, 2)->text() == QStringLiteral("2") &&
            opportunity->item(1, 2)->text() == QStringLiteral("—") && opportunity->item(6, 2)->text() == QStringLiteral("5") &&
            opportunity->item(7, 2)->text() == QStringLiteral("—") && opportunity->item(9, 2)->text() == QStringLiteral("—") &&
            opportunity->item(10, 2)->text() == QStringLiteral("7"),
            "overflow or unrelated missing fields corrupted independent opportunity observations");
      check(activity->item(0, 1)->foreground().style() == Qt::NoBrush &&
            !activity->item(0, 1)->text().contains(QStringLiteral("已完成")), "unlit red points were shown as completed");
      window->resetSessionContext();
      check(daily->item(0, 2)->text() == QStringLiteral("—") && opportunity->item(0, 2)->text() == QStringLiteral("—"),
            "session reset retained old routine counters");
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
