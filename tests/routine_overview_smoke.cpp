#include "pet_repository.h"
#include "routine_overview_catalog.h"
#include "routine_overview_controller.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>

namespace {

void deliver(PetRepository* repository, const QJsonObject& packet) {
  repository->handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

bool require(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir root;
  if (!root.isValid()) return 1;
  qputenv("KQPET_DATA_ROOT", root.path().toUtf8());

  QString catalogError;
  bool ok = require(RoutineOverviewCatalog::instance().updateFromOfficialData(
                        root.path(), &catalogError),
                    qPrintable(QStringLiteral("dynamic catalog: %1").arg(catalogError)));
  ok &= require(!RoutineOverviewCatalog::instance().tasks().isEmpty(),
                "daily/weekly task catalog is empty");
  ok &= require(!RoutineOverviewCatalog::instance().activities().isEmpty(),
                "activity catalog is empty");

  PetRepository repository;
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                        {QStringLiteral("info"),
                         QJsonObject{{QStringLiteral("n"), QStringLiteral("routine-a")}}}});
  RoutineOverviewController controller(&repository);
  struct Sent { QString service; QString command; QString params; };
  QList<Sent> sent;
  controller.setSender([&sent](const QString& service, const QString& command,
                               const QString& params) {
    sent.append({service, command, params});
    return true;
  });
  ok &= require(controller.requestRefresh(), "manual refresh did not start");
  ok &= require(sent.size() == 7, "manual refresh must send exactly seven read-only requests");
  ok &= require(sent.value(0).command == QStringLiteral("1008_20170623_dt_0"),
                "daily/weekly command mismatch");
  ok &= require(sent.value(1).service == QStringLiteral("null") &&
                    sent.value(1).command == QStringLiteral("1037_0"),
                "activity red-point command mismatch");
  ok &= require(sent.value(2).command == QStringLiteral("1008_20220603_swa_0_0"),
                "opportunity command mismatch");
  ok &= require(std::none_of(sent.cbegin(), sent.cend(), [](const Sent& request) {
                  return request.command == QStringLiteral("16_24_A") ||
                         request.command == QStringLiteral("16_6_0") ||
                         request.command == QStringLiteral("100_13_0") ||
                         request.command == QStringLiteral("100_2_0");
                }),
                "arena/ranking or removed pet-park commands must never be sent");
  ok &= require(sent.value(3).command == QStringLiteral("1008_20190531_gbt_1") &&
                    sent.value(4).command == QStringLiteral("2_36_1") &&
                    sent.value(5).command == QStringLiteral("110_123_0") &&
                    sent.value(6).command == QStringLiteral("1008_20260522_nf_0") &&
                    sent.value(6).params == QStringLiteral("{\"un\":-1}"),
                "target opportunity query mapping mismatch");

  controller.handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(
          QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
                      {QStringLiteral("av"), 30}, {QStringLiteral("wav"), 300},
                      {QStringLiteral("ti"), QJsonArray{1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0}},
                      {QStringLiteral("wti"), QJsonArray{15, 6, 13, 32, 13, 5, 90, 7, 15, 7, 10, 1, 8}},
                      {QStringLiteral("bi"), QJsonArray{true, false, false, false, false}},
                      {QStringLiteral("wbi"), QJsonArray{false, false, false, false, false}}})
                            .toJson(QJsonDocument::Compact)));
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"1008_20220603_swa_0_0\",\"r\":1,\"ti\":2,\"wgt\":4}"));
  // An incomplete ArenaV3 packet must not be presented as challenge counts.
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"16_24_A\",\"r\":1,\"sweep\":3}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20190531_gbt_1\",\"r\":1,\"ti\":5}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"2_36_1\",\"t\":6,\"cclt\":0}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"110_123_0\",\"r\":1,\"rwwt\":18,\"wwt\":2,\"rdt\":4,\"rdb\":0}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20260522_nf_0\",\"pt\":8,\"rft\":16}"));
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(
          QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1037_0")},
                      {QStringLiteral("r"), 1},
                      {QStringLiteral("rs"), QStringLiteral("10011#10025")}})
                            .toJson(QJsonDocument::Compact)));
  ok &= require(controller.hasDailyPacket(), "daily packet was not stored");
  ok &= require(controller.hasRedPointPacket() &&
                    controller.activeRedPoints().contains(10011),
                "red-point packet was not stored");
  ok &= require(controller.opportunityPackets()
                        .value(QStringLiteral("1008_20220603_swa_0_0"))
                        .toObject().value(QStringLiteral("ti")).toInt() == 2 &&
                    !controller.opportunityPackets().contains(QStringLiteral("16_24_A")),
                "safe opportunity packet was not stored or arena data leaked in");

  // The game may request ArenaV3 itself when the user opens it. Observing the
  // returned challenge counters is safe even when no plugin refresh is active.
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"16_24_A\",\"r\":1,"
                     "\"zao1\":{\"curz\":102,\"ct\":2,\"bct\":0,\"cd\":99},"
                     "\"zao2\":{\"curz\":201,\"ct\":3,\"bct\":1,\"cd\":88}}"));
  ok &= require(controller.opportunityPackets()
                        .value(QStringLiteral("16_24_A")).toObject()
                        .value(QStringLiteral("zao1")).toObject()
                        .value(QStringLiteral("ct")).toInt() == 2,
                "passive ArenaV3 challenge counters were not cached");

  sent.clear();
  ok &= require(controller.requestRefresh(), "partial-failure refresh did not start");
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(
          QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("1008_20170623_dt_0")},
                      {QStringLiteral("av"), 31},
                      {QStringLiteral("ti"), QJsonArray{}},
                      {QStringLiteral("wti"), QJsonArray{}}})
                            .toJson(QJsonDocument::Compact)));
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"1008_20220603_swa_0_0\",\"r\":0}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20190531_gbt_1\",\"r\":1,\"ti\":4}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"2_36_1\",\"t\":5}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"110_123_0\",\"r\":1,\"rwwt\":17,\"rdt\":5}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1008_20260522_nf_0\",\"pt\":7,\"rft\":15}"));
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"1037_0\",\"r\":1,\"rs\":\"10026\"}"));
  ok &= require(controller.dailyPacket().value(QStringLiteral("av")).toInt() == 31 &&
                    controller.activeRedPoints().contains(10026),
                "successful sibling responses were discarded after one request failed");
  ok &= require(controller.opportunityPackets()
                        .value(QStringLiteral("1008_20220603_swa_0_0"))
                        .toObject().value(QStringLiteral("ti")).toInt() == 2,
                "failed opportunity response overwrote its old cache");

  sent.clear();
  ok &= require(controller.requestRefresh(), "second manual refresh did not start");
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                        {QStringLiteral("info"),
                         QJsonObject{{QStringLiteral("n"), QStringLiteral("routine-b")}}}});
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"1037_0\",\"r\":1,\"rs\":\"99999\"}"));
  ok &= require(!controller.activeRedPoints().contains(99999),
                "old account response leaked into the new account");
  return ok ? 0 : 2;
}
