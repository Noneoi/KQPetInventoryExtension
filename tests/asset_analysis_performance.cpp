#include "asset_analysis_controller.h"
#include "asset_analysis_filter_proxy_model.h"
#include "asset_analysis_model.h"
#include "asset_analysis_window.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  repository->handlePacket(
      QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
}

QJsonObject pet(int index) {
  const bool full = index % 5 == 0;
  return {{QStringLiteral("id"), 100000 + index},
          {QStringLiteral("r"), 7000 + index},
          {QStringLiteral("fr"), 7000 + index},
          {QStringLiteral("n"), QStringLiteral("性能精灵%1").arg(index)},
          {QStringLiteral("lv"), 120},
          {QStringLiteral("zdl"), full ? 11350 : 5000 + index},
          {QStringLiteral("xzdl"), 10000},
          {QStringLiteral("astrolabebr"), full},
          {QStringLiteral("czdlv"),
           QJsonObject{{QStringLiteral("sgv"), full ? 1200 : 0},
                       {QStringLiteral("asv"), full ? 150 : 0},
                       {QStringLiteral("bsv"), full ? 100 : 10},
                       {QStringLiteral("sjv"), full ? 50 : 5}}},
          {QStringLiteral("mzdlv"),
           QJsonObject{{QStringLiteral("sgv"), 0},
                       {QStringLiteral("asv"), full ? 0 : 100},
                       {QStringLiteral("bsv"), 100},
                       {QStringLiteral("sjv"), 50}}}};
}

void loadPets(PetRepository* repository, int count, quint64 generation,
              const QString& account, quint64 session) {
  QJsonArray pets;
  QStringList ids;
  ids.reserve(count);
  for (int index = 0; index < count; ++index) {
    pets.append(pet(index));
    ids.append(QString::number(100000 + index));
  }
  repository->beginListRefresh(generation, account, session);
  deliver(repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"), pets},
           {QStringLiteral("pps"), QJsonArray{ids.join(QLatin1Char('#'))}},
           {QStringLiteral("ppc"), count}});
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  PetRepository repository;
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
           {QStringLiteral("info"),
            QJsonObject{{QStringLiteral("n"), QStringLiteral("perf-account")}}}});
  const QString account = repository.accountKey();
  const quint64 session = repository.sessionGeneration();
  ShopExchangeController shop(&repository);
  RoutineOverviewController routine(&repository);
  AssetAnalysisController controller(&repository, &shop, &routine);

  QElapsedTimer timer;
  loadPets(&repository, 1000, 1, account, session);
  timer.start();
  const AccountAssetOverview overview1000 = controller.recalculateOverview();
  const qint64 analyze1000Ms = timer.elapsed();
  ok &= require(overview1000.totalPets == 1000 && analyze1000Ms < 3000,
                "1000-pet full analysis regressed");

  loadPets(&repository, 2000, 2, account, session);
  timer.restart();
  const AccountAssetOverview overview2000 = controller.recalculateOverview();
  const qint64 analyze2000Ms = timer.elapsed();
  ok &= require(overview2000.totalPets == 2000 && analyze2000Ms < 6000,
                "2000-pet full analysis regressed");
  ok &= require(analyze2000Ms <= analyze1000Ms * 4 + 100,
                "analysis scaling suggests quadratic growth");
  const QList<AccountAssetSnapshot> emptySnapshots = controller.snapshots();
  ok &= require(emptySnapshots.isEmpty(), "unexpected snapshot before recording");

  AssetAnalysisWindow window(&controller);
  auto* model = window.findChild<AssetAnalysisModel*>();
  int modelResets = 0;
  if (model)
    QObject::connect(model, &QAbstractItemModel::modelReset, &application,
                     [&modelResets]() { ++modelResets; });
  const quint64 runsBeforeDetails = controller.analysisRunCount();
  int detailSignals = 0;
  QObject::connect(&controller, &AssetAnalysisController::petDetailChanged,
                   &application, [&detailSignals](qint64) { ++detailSignals; });
  timer.restart();
  for (int index = 0; index < 1000; ++index)
    repository.detailChanged(100000 + index);
  for (int index = 0; index < 1000; ++index)
    repository.detailChanged(100000 + index);
  const qint64 dirtyMs = timer.elapsed();
  QCoreApplication::processEvents();
  ok &= require(controller.dirtyPetIds().size() == 1000 &&
                    detailSignals == 1000 && dirtyMs < 1000,
                "dirty ID insertion or duplicate suppression regressed");
  ok &= require(controller.analysisRunCount() == runsBeforeDetails &&
                    modelResets == 0 && model && model->rowCount() == 2000,
                "background details triggered analysis or table model rebuild");

  AssetAnalysisModel analysisModel;
  AssetAnalysisFilterProxyModel proxy;
  proxy.setSourceModel(&analysisModel);
  timer.restart();
  analysisModel.setOverview(overview2000);
  proxy.setAssetFilter(PetAssetFilter::Improvable);
  proxy.setQuery(QStringLiteral("性能精灵1"));
  proxy.sort(AssetAnalysisModel::CurrentPower, Qt::DescendingOrder);
  const qint64 modelMs = timer.elapsed();
  ok &= require(proxy.rowCount() > 0 && modelMs < 2000,
                "2000-row filtering or sorting regressed");

  controller.recalculateOverview();
  timer.restart();
  const bool snapshotSaved = controller.recordSnapshot();
  const qint64 snapshotMs = timer.elapsed();
  const QList<AccountAssetSnapshot> snapshots = controller.snapshots();
  ok &= require(snapshotSaved && snapshotMs < 6000 && snapshots.size() == 1 &&
                    snapshots.constFirst().pets.size() == 2000,
                "2000-pet snapshot generation regressed");

  if (!ok) return 1;
  std::fprintf(stdout,
               "PASS: perf 1000=%lldms 2000=%lldms dirty=%lldms model=%lldms snapshot=%lldms\n",
               analyze1000Ms, analyze2000Ms, dirtyMs, modelMs, snapshotMs);
  return 0;
}
