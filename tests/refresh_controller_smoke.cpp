#include "pet_refresh_controller.h"
#include "pet_repository.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 1500) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return predicate();
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  repository->handlePacket(QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

QJsonObject warehousePacket() {
  QJsonArray elite;
  for (int id = 101; id <= 103; ++id) {
    elite.append(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("ri"), 7000},
                             {QStringLiteral("n"), QStringLiteral("same-pet")},
                             {QStringLiteral("lv"), 100}});
  }
  return {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
          {QStringLiteral("ns"), QJsonArray{}},
          {QStringLiteral("rb"), QJsonArray{}},
          {QStringLiteral("es"), elite}};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  PetRepository repository;
  PetRefreshController controller(&repository);
  PetRefreshController::Timings timings;
  timings.automaticIntervalMs = 40;
  timings.listRequestGapMs = 20;
  timings.listTimeoutMs = 150;
  timings.detailRequestGapMs = 10;
  timings.detailBatchRestMs = 20;
  timings.detailTimeoutMs = 80;
  timings.detailBatchSize = 2;
  timings.detailMaxRetries = 1;
  controller.setTimings(timings);

  QElapsedTimer clock;
  clock.start();
  QList<QString> listCommands;
  QList<qint64> listCommandTimes;
  QList<qint64> detailOrder;
  int detailOutstanding = 0;
  int maxDetailOutstanding = 0;
  bool listCycleOutstanding = false;
  bool listOverlap = false;
  int warehouseResponses = 0;
  int detailResponseDelayMs = 4;

  controller.setSender([&](const QString&, const QString& command, const QString& parameters) {
    if (command == QStringLiteral("2_1_10")) {
      if (listCycleOutstanding) listOverlap = true;
      listCycleOutstanding = true;
      listCommands.append(command);
      listCommandTimes.append(clock.elapsed());
      QTimer::singleShot(4, &repository, [&repository]() {
        deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                              {QStringLiteral("pl"), QJsonArray{}},
                              {QStringLiteral("pps"), QJsonArray{}}});
      });
      return true;
    }
    if (command == QStringLiteral("2_1_S")) {
      listCommands.append(command);
      listCommandTimes.append(clock.elapsed());
      QTimer::singleShot(35, &repository, [&]() {
        ++warehouseResponses;
        listCycleOutstanding = false;
        deliver(&repository, warehousePacket());
      });
      return true;
    }
    if (command == QStringLiteral("2_2_10")) return true;
    if (command == QStringLiteral("2_1_R")) {
      const QJsonObject request = QJsonDocument::fromJson(parameters.toUtf8()).object();
      const qint64 id = request.value(QStringLiteral("pi")).toVariant().toLongLong();
      detailOrder.append(id);
      ++detailOutstanding;
      maxDetailOutstanding = qMax(maxDetailOutstanding, detailOutstanding);
      QTimer::singleShot(detailResponseDelayMs, &repository, [&, id]() {
        --detailOutstanding;
        deliver(&repository,
                {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
                 {QStringLiteral("p"),
                  QJsonObject{{QStringLiteral("id"), id},
                              {QStringLiteral("r"), 7000},
                              {QStringLiteral("n"), QStringLiteral("same-pet")},
                              {QStringLiteral("lv"), 100},
                              {QStringLiteral("zdl"), 30000 + static_cast<int>(id)},
                              {QStringLiteral("xzdl"), 29000}}}});
      });
      return true;
    }
    return false;
  });

  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                        {QStringLiteral("info"),
                         QJsonObject{{QStringLiteral("n"), QStringLiteral("account-test")}}}});
  controller.requestManualListRefresh();
  ok &= require(waitUntil([&]() { return !controller.listRefreshRunning() && warehouseResponses >= 1; }),
                "manual list refresh did not complete");
  ok &= require(listCommands.size() >= 2 &&
                    listCommands.at(0) == QStringLiteral("2_1_10") &&
                    listCommands.at(1) == QStringLiteral("2_1_S"),
                "list refresh command order is incorrect");
  ok &= require(listCommandTimes.at(1) - listCommandTimes.at(0) >= 15,
                "warehouse request was not delayed after backpack request");

  ok &= require(waitUntil([&]() { return warehouseResponses >= 2; }),
                "automatic refresh did not run after the configured interval");
  ok &= require(!listOverlap, "automatic list refresh overlapped an unfinished cycle");

  timings.automaticIntervalMs = 40;
  controller.setTimings(timings);
  detailOrder.clear();
  bool pauseIssued = false;
  QObject::connect(&controller, &PetRefreshController::detailProgressChanged,
                   &application,
                   [&](bool running, bool paused, int completed, int, int, int, qint64, int) {
    if (running && !paused && completed == 1 && !pauseIssued) {
      pauseIssued = true;
      controller.pauseWarehouseDetailRefresh();
    }
  });

  controller.startWarehouseDetailRefresh();
  const int warehouseResponsesAtBatchStart = warehouseResponses;
  QTimer::singleShot(1, &controller, [&controller]() { controller.requestSingleDetail(103); });
  ok &= require(waitUntil([&]() {
                  return controller.detailBatchPaused() && detailOrder.size() >= 2;
                }), "batch did not pause after current/priority detail requests");
  ok &= require(detailOrder.size() >= 2 && detailOrder.at(0) == 101 && detailOrder.at(1) == 103,
                "clicked detail did not jump ahead of the batch queue");
  const int pausedSendCount = detailOrder.size();
  waitUntil([&]() { return false; }, 45);
  ok &= require(detailOrder.size() == pausedSendCount,
                "paused batch continued sending ordinary queued details");
  ok &= require(warehouseResponses == warehouseResponsesAtBatchStart,
                "automatic list refresh ran while warehouse detail batch was paused");
  controller.resumeWarehouseDetailRefresh();
  ok &= require(waitUntil([&]() { return !controller.detailBatchRunning(); }),
                "resumed detail batch did not complete");
  ok &= require(detailOrder == QList<qint64>({101, 103, 102}),
                "detail batch order or deduplication is incorrect");
  ok &= require(maxDetailOutstanding == 1,
                "detail queue sent more than one request concurrently");

  const int warehouseResponsesAtBatchFinish = warehouseResponses;
  waitUntil([&]() { return false; }, 25);
  ok &= require(warehouseResponses == warehouseResponsesAtBatchFinish,
                "automatic countdown did not restart from the detail batch finish time");
  ok &= require(waitUntil([&]() {
                  return warehouseResponses > warehouseResponsesAtBatchFinish;
                }, 250),
                "automatic list refresh did not resume after detail batch completion");

  timings.automaticIntervalMs = 10000;
  controller.setTimings(timings);
  detailResponseDelayMs = 50;
  detailOrder.clear();
  controller.startWarehouseDetailRefresh();
  ok &= require(waitUntil([&]() { return !detailOrder.isEmpty(); }),
                "cancel test did not start a detail request");
  controller.cancelWarehouseDetailRefresh();
  const int sendsAtCancel = detailOrder.size();
  waitUntil([&]() { return false; }, 90);
  ok &= require(!controller.detailBatchRunning() && detailOrder.size() == sendsAtCancel,
                "cancelled batch continued sending requests");

  QFile settingsFile(QDir(temporary.path()).filePath(QStringLiteral("settings.json")));
  ok &= require(settingsFile.open(QIODevice::ReadOnly),
                "refresh settings were not persisted");
  const QJsonObject savedSettings =
      QJsonDocument::fromJson(settingsFile.readAll()).object();
  ok &= require(savedSettings.value(QStringLiteral("detailBatchSize")).toInt() == 2 &&
                    savedSettings.value(QStringLiteral("automaticIntervalMs")).toInt() == 10000,
                "persisted refresh settings do not match active timings");

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: timed list refresh and serial detail queue\n");
  return 0;
}
