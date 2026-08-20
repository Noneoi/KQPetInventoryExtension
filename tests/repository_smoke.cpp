#include "pet_repository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  repository->handlePacket(QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

void login(PetRepository* repository, const QString& account) {
  deliver(repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                       {QStringLiteral("info"),
                        QJsonObject{{QStringLiteral("n"), account}}}});
}

QJsonObject warehousePacket(int race42 = 7152, const QString& name42 = QStringLiteral("skin-a")) {
  return {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
          {QStringLiteral("es"),
           QJsonArray{QJsonObject{{QStringLiteral("id"), 42},
                                  {QStringLiteral("ri"), race42},
                                  {QStringLiteral("fr"), race42 + 10},
                                  {QStringLiteral("n"), name42},
                                  {QStringLiteral("lv"), 100},
                                  {QStringLiteral("gd"), 100001},
                                  {QStringLiteral("zdl"), 90001},
                                  {QStringLiteral("xzdl"), 89901}},
                      QJsonObject{{QStringLiteral("id"), 43},
                                  {QStringLiteral("ri"), race42},
                                  {QStringLiteral("fr"), race42 + 10},
                                  {QStringLiteral("n"), name42},
                                  {QStringLiteral("lv"), 100},
                                  {QStringLiteral("gd"), 100002}}}},
          {QStringLiteral("ns"), QJsonArray{}},
          {QStringLiteral("rb"), QJsonArray{}}};
}

void acceptWarehouse(PetRepository* repository, quint64 generation,
                     const QJsonObject& packet) {
  repository->beginListRefresh(generation, repository->accountKey(),
                               repository->sessionGeneration());
  deliver(repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                       {QStringLiteral("pl"), QJsonArray{}},
                       {QStringLiteral("pps"), QJsonArray{}}});
  repository->expectListPart(QStringLiteral("2_1_S"), generation,
                             repository->accountKey(), repository->sessionGeneration());
  deliver(repository, packet);
}

QJsonObject detailPacket(qint64 id, int race, int face, int power) {
  return {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
          {QStringLiteral("p"),
           QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("r"), race},
                       {QStringLiteral("fr"), face},
                       {QStringLiteral("n"), QStringLiteral("detail-%1").arg(race)},
                       {QStringLiteral("lv"), 100},
                       {QStringLiteral("zdl"), power},
                       {QStringLiteral("xzdl"), power - 100}}}};
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  PetRepository repository;
  int fullListChangeCount = 0;
  int detailChangeCount = 0;
  QObject::connect(&repository, &PetRepository::dataChanged, &application,
                   [&]() { ++fullListChangeCount; });
  QObject::connect(&repository, &PetRepository::detailChanged, &application,
                   [&](qint64) { ++detailChangeCount; });
  login(&repository, QStringLiteral("account-1001"));

  deliver(&repository, warehousePacket());
  ok &= require(repository.warehousePets().isEmpty(),
                "unsolicited warehouse list must not overwrite cache");

  acceptWarehouse(&repository, 10, warehousePacket());
  ok &= require(repository.warehousePets().size() == 2,
                "manual warehouse refresh was not accepted");
  ok &= require(!repository.warehousePet(42).contains(QStringLiteral("zdl")),
                "warehouse overview power must not come from list response");

  deliver(&repository, detailPacket(42, 7152, 7162, 11111));
  ok &= require(!repository.detailFor(42).contains(QStringLiteral("zdl")),
                "unsolicited detail response must not overwrite cache");

  repository.expectDetail(42, 20, repository.accountKey(), repository.sessionGeneration());
  const int listChangesBeforeDetail = fullListChangeCount;
  deliver(&repository, detailPacket(42, 7152, 7162, 30000));
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 30000,
                "warehouse overview power must come from detail cache");
  ok &= require(fullListChangeCount == listChangesBeforeDetail && detailChangeCount == 1,
                "detail response must emit one row update instead of a full-list rebuild");

  repository.expectDetail(43, 21, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(43, 7152, 7162, 28000));
  ok &= require(repository.warehousePet(43).value(QStringLiteral("zdl")).toInt() == 28000,
                "same-race pet instances must keep separate detail power");

  const QString accountRoot = QDir(temporary.path()).filePath(QStringLiteral("accounts/account-1001"));
  const QString detail42Path = QDir(accountRoot).filePath(QStringLiteral("details/42.json"));
  const QString detail43Path = QDir(accountRoot).filePath(QStringLiteral("details/43.json"));
  ok &= require(QFile::exists(detail42Path) && QFile::exists(detail43Path),
                "duplicate-race instances were not saved to separate files");
  const QJsonObject envelope42 = QJsonDocument::fromJson([&]() {
    QFile file(detail42Path); file.open(QIODevice::ReadOnly); return file.readAll();
  }()).object();
  ok &= require(envelope42.value(QStringLiteral("schema")).toInt() == 3 &&
                    envelope42.value(QStringLiteral("account")).toString() == QStringLiteral("account-1001") &&
                    envelope42.value(QStringLiteral("instanceId")).toString() == QStringLiteral("42") &&
                    !envelope42.value(QStringLiteral("imageCacheKey")).toString().isEmpty(),
                "schema-3 detail envelope metadata is incomplete");

  acceptWarehouse(&repository, 11, warehousePacket(7250, QStringLiteral("skin-b")));
  ok &= require(repository.warehousePet(42).value(QStringLiteral("_visualMismatch")).toBool(),
                "skin/race change was not detected for the same instance");
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 30000,
                "skin change must retain local cached power until detail refresh");
  repository.expectDetail(42, 22, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(42, 7250, 7260, 31000));
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 31000 &&
                    !repository.warehousePet(42).value(QStringLiteral("_visualMismatch")).toBool(),
                "same instance did not update after skin detail refresh");

  repository.expectDetail(42, 23, repository.accountKey(), repository.sessionGeneration());
  login(&repository, QStringLiteral("account-2002"));
  deliver(&repository, detailPacket(42, 7250, 7260, 99999));
  ok &= require(repository.warehousePets().isEmpty() && repository.detailFor(42).isEmpty(),
                "old-account delayed detail response leaked into new account");

  login(&repository, QStringLiteral("account-1001"));
  ok &= require(repository.warehousePets().size() == 2 &&
                    repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 31000,
                "account-isolated cache did not reload correctly");

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: schema-3 account/instance repository cache\n");
  return 0;
}
