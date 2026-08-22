#include "pet_repository.h"
#include "shop_window.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTableWidget>
#include <QHeaderView>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>

namespace {

void deliver(PetRepository* repository, const QJsonObject& packet) {
  repository->handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("KQShopUiPreview"));
  QTemporaryDir dataRoot;
  qputenv("KQPET_DATA_ROOT", dataRoot.path().toUtf8());

  auto* repository = new PetRepository();
  deliver(repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                       {QStringLiteral("info"),
                        QJsonObject{{QStringLiteral("n"),
                                     QStringLiteral("preview-account")}}}});
  repository->beginListRefresh(1, repository->accountKey(),
                               repository->sessionGeneration());
  deliver(repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 10001},
                                   {QStringLiteral("r"), 7115},
                                   {QStringLiteral("n"), QStringLiteral("预览精灵 A")},
                                   {QStringLiteral("lv"), 100},
                                   {QStringLiteral("zdl"), 22000},
                                   {QStringLiteral("xzdl"), 24000}},
                       QJsonObject{{QStringLiteral("id"), 10003},
                                   {QStringLiteral("r"), 7115},
                                   {QStringLiteral("n"), QStringLiteral("预览精灵 C")},
                                   {QStringLiteral("lv"), 80},
                                   {QStringLiteral("zdl"), 18000},
                                   {QStringLiteral("xzdl"), 23000}}}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("10001")}}});
  repository->expectListPart(QStringLiteral("2_1_S"), 1,
                             repository->accountKey(), repository->sessionGeneration());
  deliver(repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
           {QStringLiteral("ns"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 10002},
                                   {QStringLiteral("ri"), 7185},
                                   {QStringLiteral("n"), QStringLiteral("预览精灵 B")},
                                   {QStringLiteral("lv"), 100}}}},
           {QStringLiteral("es"), QJsonArray{}},
           {QStringLiteral("rb"), QJsonArray{}}});

  auto* window = new ShopWindow(repository);
  window->setPacket(
      {{QStringLiteral("si1"),
        QJsonObject{{QStringLiteral("b4"), QJsonObject{{QStringLiteral("pl"), 0}}},
                    {QStringLiteral("b6"), QJsonObject{{QStringLiteral("pl"), 1}}}}},
       {QStringLiteral("si4"),
        QJsonObject{{QStringLiteral("b4"), QJsonObject{{QStringLiteral("pl"), 0}}}}}},
      true);
  window->setStatus(QStringLiteral("界面预览：已载入模拟兑换次数"));
  window->setMaterialCounts({{QStringLiteral("4:3237"), 1888},
                             {QStringLiteral("4:3238"), 66},
                             {QStringLiteral("4:3241"), 777},
                             {QStringLiteral("4:3247"), 999},
                             {QStringLiteral("4:1344"), 1234},
                             {QStringLiteral("4:3189"), 3168},
                             {QStringLiteral("134:1"), 8800}}, true);
  window->show();

  if (qEnvironmentVariableIntValue("KQPET_PREVIEW_SELF_TEST") > 0) {
    QTimer::singleShot(0, &application, [&]() {
      auto* goods = window->findChild<QTableWidget*>(QStringLiteral("KQShopGoodsTable-1"));
      auto* pets = window->findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
      auto* detail = window->findChild<QTextBrowser*>();
      if (!goods || !pets || !detail || goods->rowCount() <= 0) {
        application.exit(2);
        return;
      }
      const QPoint goodsTop = goods->mapToGlobal(QPoint(0, 0));
      const QPoint petsTop = pets->mapToGlobal(QPoint(0, 0));
      const QPoint detailTop = detail->mapToGlobal(QPoint(0, 0));
      if (petsTop.y() <= goodsTop.y() || detailTop.x() <= petsTop.x()) {
        application.exit(7);
        return;
      }
      QMetaObject::invokeMethod(goods, "cellClicked", Qt::DirectConnection,
                                Q_ARG(int, 0), Q_ARG(int, 0));
      if (!goods->item(0, 0) || !goods->item(0, 0)->font().bold()) {
        application.exit(8);
        return;
      }
      if (pets->rowCount() <= 0) {
        application.exit(3);
        return;
      }
      if (pets->rowCount() < 2) {
        application.exit(5);
        return;
      } else {
        const auto verifyTwoWaySort = [pets](int column) {
          QMetaObject::invokeMethod(pets->horizontalHeader(), "sectionClicked",
                                    Qt::DirectConnection, Q_ARG(int, column));
          const int firstAscending = pets->item(0, column)->text().toInt();
          const int lastAscending = pets->item(pets->rowCount() - 1, column)->text().toInt();
          QMetaObject::invokeMethod(pets->horizontalHeader(), "sectionClicked",
                                    Qt::DirectConnection, Q_ARG(int, column));
          const int firstDescending = pets->item(0, column)->text().toInt();
          const int lastDescending = pets->item(pets->rowCount() - 1, column)->text().toInt();
          return firstAscending <= lastAscending && firstDescending >= lastDescending;
        };
        if (!verifyTwoWaySort(1) || !verifyTwoWaySort(5) || !verifyTwoWaySort(6)) {
          application.exit(6);
          return;
        }
      }
      QMetaObject::invokeMethod(pets, "cellClicked", Qt::DirectConnection,
                                Q_ARG(int, 0), Q_ARG(int, 0));

      repository->beginListRefresh(2, repository->accountKey(),
                                   repository->sessionGeneration());
      deliver(repository,
              {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
               {QStringLiteral("pl"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), 10001},
                                       {QStringLiteral("r"), 7115},
                                       {QStringLiteral("n"), QStringLiteral("预览精灵 A2")},
                                       {QStringLiteral("lv"), 100}}}},
               {QStringLiteral("pps"), QJsonArray{QStringLiteral("10001")}}});
      repository->expectListPart(QStringLiteral("2_1_S"), 2,
                                 repository->accountKey(),
                                 repository->sessionGeneration());
      deliver(repository,
              {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
               {QStringLiteral("ns"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), 10002},
                                       {QStringLiteral("ri"), 7185},
                                       {QStringLiteral("n"), QStringLiteral("预览精灵 B2")},
                                       {QStringLiteral("lv"), 100}}}},
               {QStringLiteral("es"), QJsonArray{}},
               {QStringLiteral("rb"), QJsonArray{}}});
      pets = window->findChild<QTableWidget*>(QStringLiteral("KQShopPetTable"));
      const bool preserved = pets && pets->rowCount() > 0 &&
                             pets->currentRow() >= 0 && detail &&
                             detail->toPlainText().contains(QStringLiteral("预览精灵 A2"));
      application.exit(preserved ? 0 : 4);
    });
  }

  bool validExitDelay = false;
  const int exitDelayMs =
      qEnvironmentVariableIntValue("KQPET_PREVIEW_EXIT_MS", &validExitDelay);
  if (validExitDelay && exitDelayMs > 0)
    QTimer::singleShot(exitDelayMs, &application, &QCoreApplication::quit);
  const int result = application.exec();
  delete window;
  delete repository;
  return result;
}
