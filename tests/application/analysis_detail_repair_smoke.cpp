#include "support/protocol_test_support.h"
#include "application/analysis/asset_analysis_controller.h"
#include "application/pet/pet_derivation_cache.h"
#include "application/pet/pet_refresh_controller.h"
#include "application/pet/pet_repository.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
bool until(const std::function<bool()>& predicate, int timeout = 15000) {
  QElapsedTimer timer; timer.start();
  while (timer.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents,5);
    if (predicate()) return true;
    QThread::msleep(1);
  }
  return false;
}
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr,"FAIL: %s\n",message);
  return value;
}
QJsonObject detail(qint64 id) {
  return {{"id",QString::number(id)},{"r",7001},{"n","fixture"},{"lv",120},
      {"sgs","66:8#67:8#70:8"},{"sgsp",QJsonArray{}},{"badge",""},{"shenjue",""},{"astrolabe",""},
      {"zdl",7000},{"xzdl",8000},{"czdlv",QJsonObject{{"lv",7000}}},{"mzdlv",QJsonObject{{"lv",8000}}}};
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); QTemporaryDir root;
  qputenv("KQPET_DATA_ROOT",root.path().toUtf8());
  PetRepository repository; bool ok = waitForRepositoryIdle(&repository);
  deliverVerifiedFixture(&repository,{{"_cmd","21_1"},{"info",QJsonObject{{"n","analysis-repair"}}}});
  ok &= waitForRepositoryIdle(&repository);
  const auto account = repository.accountKey(); const auto epoch = repository.sessionGeneration();
  repository.beginListRefresh(1,account,epoch);
  deliverVerifiedFixture(&repository,{{"_cmd","2_1_10"},{"pl",QJsonArray{detail(1001)}},{"pps",QJsonArray{"1001"}},{"ppc",12}});
  repository.expectListPart("2_1_S",1,account,epoch);
  deliverVerifiedFixture(&repository,{{"_cmd","2_1_S"},{"ns",QJsonArray{
      QJsonObject{{"id","2001"},{"ri",7001},{"lv",120}},QJsonObject{{"id","2002"},{"ri",7001},{"lv",120}}}},
      {"es",QJsonArray{}},{"rb",QJsonArray{}}});
  ok &= waitForRepositoryIdle(&repository);
  AssetAnalysisController analysis(&repository,nullptr,nullptr);
  PetDerivationCache cache([&](std::function<void()> work) { return analysis.postPriorityCompute(std::move(work)); }, repository.storageService());
  analysis.setDerivationCache(&cache);
  PetRefreshController refresh(&repository);
  auto timings = refresh.timings(); timings.detailRequestGapMs = 0; timings.detailTimeoutMs = 30;
  timings.detailMaxRetries = 2; refresh.setTimings(timings);
  analysis.setDetailRefreshController(&refresh);
  QHash<qint64,int> sends; bool failSecond = true;
  refresh.setSender([&](const QString&, const QString& command, const QString& parameters) {
    if (command != "2_1_R") return false;
    const auto id = QJsonDocument::fromJson(parameters.toUtf8()).object().value("pi").toInteger();
    ++sends[id];
    if (failSecond && id == 2002) return true;
    QTimer::singleShot(5,&repository,[&,id] {
      deliverVerifiedFixture(&repository,{{"_cmd","2_1_R"},{"p",detail(id)}});
    });
    return true;
  });
  QString status;
  QObject::connect(&analysis,&AssetAnalysisController::statusChanged,[&](const QString& text) { status = text; });
  analysis.requestAnalysis();
  ok &= check(until([&] { return !analysis.analysisRunning(); }),"automatic missing-detail repair finishes");
  ok &= check(analysis.hasAnalysis() && analysis.overview().totalPets == 3 && analysis.overview().missingDetailPets == 1,
      "successful missing detail is included, failure remains explicitly unknown");
  ok &= check(sends.value(1001) == 0 && sends.value(2001) == 1 && sends.value(2002) == 1,
      "only missing instances are queried once, regardless of normal manual retry setting");
  if (!ok) std::fprintf(stderr,"status=%s; sends=%d,%d,%d\n",qPrintable(status),sends.value(1001),sends.value(2001),sends.value(2002));
  failSecond = false; analysis.requestAnalysis();
  ok &= check(until([&] { return !analysis.analysisRunning(); }) && analysis.overview().missingDetailPets == 0,
      "a later explicit analysis may repair a prior failure");
  ok &= check(sends.value(2001) == 1 && sends.value(2002) == 2,"cached success is never fetched again");
  analysis.requestAnalysis(); analysis.cancelAnalysis();
  ok &= check(!analysis.analysisRunning(),"cancel stops analysis repair scheduling");
  analysis.shutdownAnalysis();
  return ok ? 0 : 1;
}
