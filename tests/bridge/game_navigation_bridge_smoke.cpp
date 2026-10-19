#include "bridge/original_bridge.h"
#include "diagnostics/target_compatibility_guard.h"

#include <QApplication>
#include <QFile>
#include <QLibrary>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>
#include <cstdio>
#include <memory>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "navigation bridge failure line %d\n", __LINE__); return 1; } } while (false)

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  CHECK(argc == 2);
  QTemporaryDir temp;
  CHECK(temp.isValid());
  const QString fixturePath = temp.filePath(QStringLiteral("QCefView.dll"));
  CHECK(QFile::copy(app.arguments()[1], fixturePath));
  QLibrary fixture(fixturePath);
  CHECK(fixture.load());
  auto create = reinterpret_cast<QWidget*(*)()>(fixture.resolve("createNavigationFixture"));
  CHECK(create);
  TargetCompatibilityReport compatibility;
  compatibility.supported = true;
  compatibility.qcefViewExecuteJavascript = 1;  // Only checked for availability here.
  TargetCompatibilityGuard::setLastReport(compatibility);
  {
    std::unique_ptr<QWidget> mainView(create()), otherView(create());
    mainView->resize(900, 700); mainView->show();
    otherView->resize(400, 300); otherView->show();
    OriginalBridge bridge;
    QSignalSpy finished(&bridge, &OriginalBridge::gameNavigationFinished);
    const QString route = QStringLiteral("btnNewAct_storeexchangeframework_showMainPanel_4");
    const auto count = [](QWidget* view) { return view->property("calls").toInt(); };
    const auto reply = [](QWidget* view, const QString& value, QString context = {}) {
      if (context.isEmpty()) context = view->property("context").toString();
      return QMetaObject::invokeMethod(view, "reportJavascriptResult", Qt::DirectConnection,
          Q_ARG(int, 1), Q_ARG(qint64, 12), Q_ARG(QString, context), Q_ARG(QVariant, QVariant(value)));
    };
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Submitted);
    const QString firstContext = mainView->property("context").toString();
    CHECK(count(mainView.get()) == 1 && count(otherView.get()) == 0);
    CHECK(mainView->property("frame").toLongLong() == 0);
    CHECK(mainView->property("script").toString().startsWith(QStringLiteral("return ")));
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::DefinitelyNotSubmitted);
    CHECK(count(mainView.get()) == 1);
    CHECK(reply(mainView.get(), QStringLiteral("submitted-flash"), QStringLiteral("stale-context")));
    CHECK(finished.isEmpty());
    CHECK(reply(mainView.get(), QStringLiteral("missing-game-runtime")));
    CHECK(count(otherView.get()) == 1 && finished.isEmpty());
    const QString secondContext = otherView->property("context").toString();
    CHECK(secondContext != firstContext);
    // Correct context from the wrong view cannot acknowledge the request.
    CHECK(reply(mainView.get(), QStringLiteral("submitted-flash"), secondContext));
    CHECK(finished.isEmpty());
    CHECK(reply(otherView.get(), QStringLiteral("submitted-h5")));
    CHECK(finished.size() == 1 && finished.last()[0].toBool());
    CHECK(!finished.last()[1].toString().contains(QStringLiteral("已打开")));

    finished.clear();
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Submitted);
    CHECK(reply(mainView.get(), QStringLiteral("submitted-flash")));
    CHECK(finished.size() == 1 && count(otherView.get()) == 1);  // No broadcast.
    CHECK(reply(mainView.get(), QStringLiteral("submitted-flash"), firstContext));
    CHECK(finished.size() == 1);  // Late result ignored.

    for (const QString value : {QString(), QStringLiteral("error:partial-call"), QStringLiteral("opened")}) {
      finished.clear();
      CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Submitted);
      CHECK(reply(mainView.get(), value));
      CHECK(finished.size() == 1 && !finished.last()[0].toBool() && count(otherView.get()) == 1);
    }
    finished.clear();
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Submitted);
    CHECK(reply(mainView.get(), QStringLiteral("flash-not-ready")));
    CHECK(reply(otherView.get(), QStringLiteral("missing-game-runtime")));
    CHECK(finished.size() == 1 && !finished.last()[0].toBool());
    CHECK(finished.last()[1].toString().contains(QStringLiteral("Flash")));

    mainView->setProperty("rejectExecution", true);
    const int otherCount = count(otherView.get());
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Unknown);
    CHECK(count(otherView.get()) == otherCount);
    mainView->setProperty("rejectExecution", false);
    finished.clear();
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Submitted);
    QTest::qWait(2800);
    CHECK(finished.size() == 1 && !finished.last()[0].toBool());
    CHECK(count(otherView.get()) == otherCount);  // Timeout must not resubmit.
    CHECK(bridge.openGameNavigation(QStringLiteral("javascript:alert(1)")) == SubmissionOutcome::DefinitelyNotSubmitted);

    finished.clear();
    CHECK(bridge.openGameNavigation(route) == SubmissionOutcome::Submitted);
    mainView.reset();
    QTest::qWait(2800);
    CHECK(finished.size() == 1 && !finished.last()[0].toBool());
    CHECK(count(otherView.get()) == otherCount);  // Deleted view cannot be retried.
  }
  CHECK(fixture.unload());
  std::puts("navigation sequential dispatch, receipts, late results, timeout and destroyed-view checks passed");
  return 0;
}
