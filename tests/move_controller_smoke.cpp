#include "pet_move_policy.h"
#include "pet_refresh_controller.h"
#include "pet_repository.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <algorithm>
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
  repository->handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

QJsonObject pet(qint64 id) {
  return {{QStringLiteral("id"), id},
          {QStringLiteral("ri"), 7000 + static_cast<int>(id)},
          {QStringLiteral("n"), QStringLiteral("pet-%1").arg(id)},
          {QStringLiteral("lv"), 100},
          {QStringLiteral("zdl"), 30000 + static_cast<int>(id)},
          {QStringLiteral("xzdl"), 30000}};
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
  timings.automaticIntervalMs = 100000;
  timings.listRequestGapMs = 2;
  timings.listTimeoutMs = 100;
  timings.detailRequestGapMs = 2;
  timings.detailTimeoutMs = 50;
  timings.moveRequestTimeoutMs = 25;
  controller.setTimings(timings);

  QList<qint64> pack{1, 2};
  QSet<qint64> warehouse{3};
  QHash<qint64, QJsonObject> pets{{1, pet(1)}, {2, pet(2)}, {3, pet(3)}};
  int capacity = 2;
  int writes = 0;
  int detailRequests = 0;
  bool acknowledgeWrites = true;
  bool deliveringMoveAck = false;
  bool embeddedAckApplied = true;
  int reentrantVerificationSends = 0;

  auto backpackPacket = [&]() {
    QJsonArray list;
    for (qint64 id : pack) list.append(pets.value(id));
    return QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                       {QStringLiteral("pl"), list},
                       {QStringLiteral("pps"),
                        QJsonArray{PetMovePolicy::serializeSequence(pack)}},
                       {QStringLiteral("ppc"), capacity}};
  };
  auto warehousePacket = [&]() {
    QJsonArray normal;
    QList<qint64> ids = warehouse.values();
    std::sort(ids.begin(), ids.end());
    for (qint64 id : ids) normal.append(pets.value(id));
    return QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
                       {QStringLiteral("ns"), normal},
                       {QStringLiteral("rb"), QJsonArray{}},
                       {QStringLiteral("es"), QJsonArray{}}};
  };

  auto applySequenceWrite = [&](const QList<qint64>& next) {
    ++writes;
    for (qint64 id : pack)
      if (!next.contains(id)) warehouse.insert(id);
    for (qint64 id : next) warehouse.remove(id);
    pack = next;
    if (acknowledgeWrites) {
      QTimer::singleShot(1, &repository, [&]() {
        QJsonObject response = backpackPacket();
        response.insert(QStringLiteral("_cmd"), QStringLiteral("2_1_11"));
        response.insert(QStringLiteral("r"), 1);
        deliveringMoveAck = true;
        deliver(&repository, response);
        embeddedAckApplied =
            embeddedAckApplied && repository.backpackIds(0) == pack;
        deliveringMoveAck = false;
      });
    }
  };
  int flashMoves = 0;
  controller.setFlashInvoker([&](const QString& method, const QString& argument) {
    if (method != QStringLiteral("batchpet")) return false;
    ++flashMoves;
    applySequenceWrite(PetMovePolicy::parseSequence(argument));
    return true;
  });
  controller.setSender([&](const QString&, const QString& command,
                           const QString& parameters) {
    if (command == QStringLiteral("2_1_10")) {
      if (deliveringMoveAck) ++reentrantVerificationSends;
      QTimer::singleShot(1, &repository,
                         [&]() { deliver(&repository, backpackPacket()); });
      return true;
    }
    if (command == QStringLiteral("2_1_S")) {
      if (deliveringMoveAck) ++reentrantVerificationSends;
      QTimer::singleShot(1, &repository,
                         [&]() { deliver(&repository, warehousePacket()); });
      return true;
    }
    if (command == QStringLiteral("2_2_10")) return true;
    if (command == QStringLiteral("2_1_R")) {
      ++detailRequests;
      const qint64 id = QJsonDocument::fromJson(parameters.toUtf8())
                            .object().value(QStringLiteral("pi")).toVariant().toLongLong();
      QJsonObject detail = pets.value(id);
      detail.insert(QStringLiteral("r"), detail.value(QStringLiteral("ri")));
      QTimer::singleShot(1, &repository, [&, detail]() {
        deliver(&repository,
                {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
                 {QStringLiteral("p"), detail}});
      });
      return true;
    }
    if (command == QStringLiteral("2_1_11")) {
      const QJsonObject request =
          QJsonDocument::fromJson(parameters.toUtf8()).object();
      applySequenceWrite(PetMovePolicy::parseSequence(
          request.value(QStringLiteral("pps")).toString()));
      return true;
    }
    return false;
  });

  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("move-test")}}}});

  const QStringList relationFields = {
      QStringLiteral("srpi"), QStringLiteral("sepi"), QStringLiteral("sdpi"),
      QStringLiteral("cepi"), QStringLiteral("crpis"), QStringLiteral("asps"),
      QStringLiteral("acps"), QStringLiteral("sppl"), QStringLiteral("cppl")};
  for (const QString& field : relationFields) {
    QJsonObject related = pet(90);
    related.insert(field, 99);
    ok &= require(PetMovePolicy::restriction(related).isEmpty(),
                  "summon/carry/divine relation was still blocked");
  }
  ok &= require(PetMovePolicy::deploymentText(QJsonObject{}) ==
                    QStringLiteral("—") &&
                    PetMovePolicy::deploymentText(
                        QJsonObject{{QStringLiteral("inFormation"), false}}) ==
                        QStringLiteral("否") &&
                    PetMovePolicy::deploymentText(
                        QJsonObject{{QStringLiteral("inTeam"), QStringLiteral("1")}}) ==
                        QStringLiteral("是") &&
                    PetMovePolicy::deploymentText(
                        QJsonObject{{QStringLiteral("isTeamPet"),
                                     QStringLiteral("false")}}) ==
                        QStringLiteral("否"),
                "deployment overview text did not handle tri-state flags");

  bool finished = false;
  bool succeeded = false;
  QString result;
  QObject::connect(&controller, &PetRefreshController::moveFinished, &application,
                   [&](bool success, const QString& message) {
                     finished = true;
                     succeeded = success;
                     result = message;
                   });
  QObject::connect(
      &controller, &PetRefreshController::replacementRequired, &application,
      [&](qint64, const QList<qint64>& eligible) {
        if (eligible.contains(3))
          controller.chooseMoveReplacement(3);
        else if (eligible.contains(2))
          controller.chooseMoveReplacement(2);
      });

  controller.requestMoveToWarehouse(2);
  ok &= require(waitUntil([&]() { return finished; }),
                "move-to-warehouse did not finish");
  ok &= require(succeeded && pack == QList<qint64>({1}) && warehouse.contains(2),
                "move-to-warehouse result is incorrect");
  ok &= require(repository.hasCachedDetail(2),
                "outgoing backpack detail was not preserved");
  ok &= require(embeddedAckApplied,
                "embedded 2_1_11 backpack data was not applied immediately");
  ok &= require(reentrantVerificationSends == 0,
                "verification list refresh was sent inside the 2_1_11 response stack");
  ok &= require(flashMoves > 0,
                "move did not go through official Flash batchpet path");

  finished = false;
  controller.requestSingleDetail(3);
  controller.requestMoveToBackpack(3);
  ok &= require(waitUntil([&]() { return finished; }),
                "move-to-backpack did not finish");
  ok &= require(succeeded && pack == QList<qint64>({1, 3}) &&
                    !warehouse.contains(3),
                "move-to-backpack result is incorrect");
  ok &= require(detailRequests == 1 && repository.hasCachedDetail(3),
                "clicked instance detail did not finish before move preflight");

  finished = false;
  controller.requestMoveToBackpack(2);
  ok &= require(waitUntil([&]() { return finished; }),
                "full-pack replacement did not finish");
  ok &= require(succeeded && pack == QList<qint64>({1, 2}) &&
                    warehouse.contains(3) && !warehouse.contains(2),
                "full-pack replacement result is incorrect");
  ok &= require(repository.hasCachedDetail(3),
                "replaced backpack detail was not preserved");

  pets[1].insert(QStringLiteral("srpi"), 99);
  finished = false;
  const int writesBeforeRestriction = writes;
  controller.requestMoveToWarehouse(1);
  ok &= require(waitUntil([&]() { return finished; }),
                "related-pet move did not finish");
  ok &= require(succeeded && writes == writesBeforeRestriction + 1 &&
                    pack == QList<qint64>({2}) && warehouse.contains(1),
                "summon/carry/divine relation was not allowed to move");
  pets[1].remove(QStringLiteral("srpi"));

  pets[2].insert(QStringLiteral("inFormation"), true);
  finished = false;
  const int writesBeforeFormationBlock = writes;
  controller.requestMoveToWarehouse(2);
  ok &= require(waitUntil([&]() { return finished; }),
                "formation restriction did not finish");
  ok &= require(!succeeded && writes == writesBeforeFormationBlock &&
                    result.contains(QStringLiteral("阵型")),
                "active formation pet was not blocked");
  pets[2].remove(QStringLiteral("inFormation"));

  acknowledgeWrites = false;
  finished = false;
  const int writesBeforeTimeout = writes;
  controller.requestMoveToBackpack(3);
  ok &= require(waitUntil([&]() { return finished; }, 2000),
                "write-timeout reconciliation did not finish");
  ok &= require(succeeded && writes == writesBeforeTimeout + 1,
                "timed-out write was resent or not reconciled by reads");

  if (!ok) return 1;
  std::fprintf(stdout,
               "PASS: safe pet movement, async verification, relations and deployment state\n");
  return 0;
}
