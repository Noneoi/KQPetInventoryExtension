#include "pet_repository.h"
#include "routine_overview_catalog.h"
#include "routine_overview_controller.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

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
  ok &= require(sent.size() == 6, "manual refresh must send exactly six requests");
  ok &= require(sent.value(0).command == QStringLiteral("1008_20170623_dt_0"),
                "daily/weekly command mismatch");
  ok &= require(sent.value(1).service == QStringLiteral("null") &&
                    sent.value(1).command == QStringLiteral("1037_0"),
                "activity red-point command mismatch");
  ok &= require(sent.value(2).command == QStringLiteral("1008_20220603_swa_0_0"),
                "opportunity command mismatch");
  ok &= require(sent.value(3).service == QStringLiteral("null") &&
                    sent.value(3).command == QStringLiteral("16_24_A"),
                "arena opportunity command mismatch");
  ok &= require(sent.value(4).service == QStringLiteral("PetParkExtension") &&
                    sent.value(4).command == QStringLiteral("100_13_0"),
                "pet-park fusion command mismatch");
  ok &= require(sent.value(5).command == QStringLiteral("100_2_0") &&
                    sent.value(5).params.contains(QStringLiteral("routine-a")),
                "pet-park feed command/account mismatch");

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
  controller.handlePacket(
      QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"16_24_A\",\"r\":1,\"sweep\":3,"
                     "\"zao1\":{\"curz\":1,\"ct\":5,\"bct\":1},"
                     "\"zao2\":{\"curz\":2,\"ct\":1,\"bct\":0}}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"100_13_0\",\"r\":1,\"pt\":1}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"100_2_0\",\"r\":1,\"rfc\":4}"));
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
                    controller.opportunityPackets()
                        .value(QStringLiteral("16_24_A"))
                        .toObject().value(QStringLiteral("sweep")).toInt() == 3,
                "real opportunity packet was not stored");

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
                          QStringLiteral("{\"_cmd\":\"16_24_A\",\"r\":1,\"sweep\":4}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"100_13_0\",\"r\":1,\"pt\":2}"));
  controller.handlePacket(QStringLiteral("recivedata"),
                          QStringLiteral("{\"_cmd\":\"100_2_0\",\"r\":1,\"rfc\":3}"));
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
