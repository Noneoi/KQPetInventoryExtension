#include "protocol_test_support.h"
#include "asset_analysis_controller.h"
#include "asset_analysis_filter_proxy_model.h"
#include "asset_analysis_model.h"
#include "asset_analysis_window.h"
#include "pet_repository.h"
#include "routine_overview_controller.h"
#include "shop_exchange_controller.h"
#include "performance_dataset.h"
#include "asset_derivation.h"
#include "storage_service.h"
#include "build_info.h"
#include "pet_derivation_cache.h"
#include "pet_record_cache.h"
#include "analysis_environment.h"
#include "shop_exchange_catalog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QHeaderView>
#include <QFontDatabase>
#include <QImage>
#include <QJsonParseError>
#include <QPointer>
#include <QTableView>
#include <QTimer>
#include <QSysInfo>
#include <atomic>
#include <chrono>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
}

AccountAssetOverview analyze(AssetAnalysisController& controller) {
  controller.requestAnalysis();
  QElapsedTimer timer; timer.start();
  while (controller.analysisRunning() && timer.elapsed() < 15000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
  }
  return controller.overview();
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

int legacySmoke(QApplication& application) {
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
  controller.setCompatibilityIdentity(QStringLiteral("fixture-build"), QStringLiteral("fixture-profile"), true);

  QElapsedTimer timer;
  loadPets(&repository, 1000, 1, account, session);
  timer.start();
  const AccountAssetOverview overview1000 = analyze(controller);
  const qint64 analyze1000Ms = timer.elapsed();
  ok &= require(overview1000.totalPets == 1000 && analyze1000Ms < 3000,
                "1000-pet full analysis regressed");

  loadPets(&repository, 2000, 2, account, session);
  timer.restart();
  const AccountAssetOverview overview2000 = analyze(controller);
  const qint64 analyze2000Ms = timer.elapsed();
  ok &= require(overview2000.totalPets == 2000 && analyze2000Ms < 6000,
                "2000-pet full analysis regressed");
  ok &= require(analyze2000Ms <= analyze1000Ms * 4 + 100,
                "analysis scaling suggests quadratic growth");
  const QList<AccountAssetSnapshot> emptySnapshots = controller.snapshots();
  ok &= require(emptySnapshots.isEmpty(), "unexpected snapshot before recording");

  AssetAnalysisWindow window(&controller);
  auto* model = window.findChild<AssetAnalysisModel*>();
  // The window fills its model through the event loop, so that first population
  // arrives after the constructor returns. Counting resets from here would
  // measure the window's own startup rather than what the detail changes below
  // trigger, which is what this check is about. Drain until the model has been
  // quiet for three passes, with a cap so a genuine reset storm still fails.
  {
    int settleResets = 0;
    const QMetaObject::Connection settleWatch = model
        ? QObject::connect(model, &QAbstractItemModel::modelReset, &application,
                           [&settleResets]() { ++settleResets; })
        : QMetaObject::Connection();
    QElapsedTimer settle;
    settle.start();
    for (int quiet = 0; quiet < 3 && settle.elapsed() < 5000;) {
      const int before = settleResets;
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
      quiet = settleResets == before ? quiet + 1 : 0;
    }
    if (settleWatch) QObject::disconnect(settleWatch);
  }
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
  // Four independent conditions: without the observed values a failure here
  // says nothing about whether an analysis actually ran or the model merely
  // finished populating late.
  const quint64 runsAfterDetails = controller.analysisRunCount();
  const int modelRows = model ? model->rowCount() : -1;
  const bool backgroundQuiet = runsAfterDetails == runsBeforeDetails &&
      modelResets == 0 && model && modelRows == 2000;
  if (!backgroundQuiet) {
    std::fprintf(stderr, "DIAG: analysisRuns %llu -> %llu, modelResets %d, rows %d\n",
                 static_cast<unsigned long long>(runsBeforeDetails),
                 static_cast<unsigned long long>(runsAfterDetails), modelResets, modelRows);
  }
  ok &= require(backgroundQuiet,
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

  // This legacy check times snapshot generation itself. Complete the fixture's
  // new per-instance original writes before admitting the snapshot; queue-full
  // behavior is covered separately and is not a false Saved result here.
  ok &= require(waitForRepositoryIdle(&repository, 20000), "legacy fixture originals did not finish IO");
  analyze(controller);
  timer.restart();
  const bool snapshotSaved = controller.recordSnapshot();
  while (controller.persistencePendingTaskCount() > 0 && timer.elapsed() < 10000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
  }
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

namespace {
qint64 nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
quint64 currentPrivateBytes() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    return counters.PrivateUsage;
#endif
  return 0;
}
void writeJsonLine(const QJsonObject& object) {
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
  std::fputc('\n', stdout); std::fflush(stdout);
}
QString outcomeName(AnalysisJobOutcome outcome) {
  switch (outcome) {
    case AnalysisJobOutcome::Published: return QStringLiteral("Published");
    case AnalysisJobOutcome::Cancelled: return QStringLiteral("Cancelled");
    case AnalysisJobOutcome::Superseded: return QStringLiteral("Superseded");
    case AnalysisJobOutcome::Stale: return QStringLiteral("Stale");
    case AnalysisJobOutcome::InputRejected: return QStringLiteral("InputRejected");
    case AnalysisJobOutcome::BudgetExceeded: return QStringLiteral("BudgetExceeded");
    case AnalysisJobOutcome::FactoryFailed: return QStringLiteral("FactoryFailed");
    case AnalysisJobOutcome::Closed: return QStringLiteral("Closed");
  }
  return QStringLiteral("Unknown");
}
QString storageStatusName(StorageStatus status) {
  switch (status) {
    case StorageStatus::Queued: return QStringLiteral("Queued");
    case StorageStatus::Saved: return QStringLiteral("Saved");
    case StorageStatus::Loaded: return QStringLiteral("Loaded");
    case StorageStatus::Scanned: return QStringLiteral("Scanned");
    case StorageStatus::Superseded: return QStringLiteral("Superseded");
    case StorageStatus::Cancelled: return QStringLiteral("Cancelled");
    case StorageStatus::QueueFull: return QStringLiteral("QueueFull");
    case StorageStatus::InvalidRequest: return QStringLiteral("InvalidRequest");
    case StorageStatus::PathRejected: return QStringLiteral("PathRejected");
    case StorageStatus::LockUnavailable: return QStringLiteral("LockUnavailable");
    case StorageStatus::WriteFailed: return QStringLiteral("WriteFailed");
    case StorageStatus::ReadFailed: return QStringLiteral("ReadFailed");
    case StorageStatus::NotFound: return QStringLiteral("NotFound");
    case StorageStatus::Closing: return QStringLiteral("Closing");
  }
  return QStringLiteral("Unknown");
}
QJsonObject memoryJson(const AnalysisMemoryUsage& m) {
  return {{QStringLiteral("inputChargedBytes"),qint64(m.inputChargedBytes)},
      {QStringLiteral("resultChargedBytes"),qint64(m.resultChargedBytes)},
      {QStringLiteral("peakChargedBytesLifetime"),qint64(m.peakChargedBytes)},
      {QStringLiteral("peakInputChargedBytesLifetime"),qint64(m.peakInputChargedBytes)},
      {QStringLiteral("peakResultChargedBytesLifetime"),qint64(m.peakResultChargedBytes)},
      {QStringLiteral("peakCandidateChargedBytesLifetime"),qint64(m.peakCandidateChargedBytes)},
      {QStringLiteral("peakPublicationOverlapBytesLifetime"),qint64(m.peakPublicationOverlapBytes)},
      {QStringLiteral("computeSlicesLifetime"),qint64(m.computeSlices)},
      {QStringLiteral("computeEventLoopTurnsLifetime"),qint64(m.computeEventLoopTurns)},
      {QStringLiteral("compiledCatalogChargedBytes"),qint64(m.compiledCatalogChargedBytes)},
      {QStringLiteral("peakCompiledCatalogChargedBytesLifetime"),qint64(m.peakCompiledCatalogChargedBytes)},
      {QStringLiteral("compiledCatalogCacheHitsLifetime"),qint64(m.compiledCatalogCacheHits)},
      {QStringLiteral("compiledCatalogCacheMissesLifetime"),qint64(m.compiledCatalogCacheMisses)},
      {QStringLiteral("snapshotsCapturedLifetime"),qint64(m.snapshotsCaptured)}};
}
struct BenchmarkOptions {
  PerformanceCase spec;
  int warmup = 1;
  int samples = 3;
  int timeoutMs = 180000;
  int cancelAfterMs = -1;
  QString cancelPhase = QStringLiteral("worker");
  QString cacheMode = QStringLiteral("warm");
  bool probes = true;
  bool save = true;
};

#include "performance_controller_core.h"

class BenchmarkThread final : public QThread {
public:
  QString root;
  std::shared_ptr<PerformanceDataset> data;
  BenchmarkOptions options;
  QObject* gui = nullptr;
  BenchmarkCore::Ready ready;
  BenchmarkCore::Result result;
  BenchmarkCore::Completed completed;
  std::atomic<BenchmarkCore*> core{nullptr};
  std::atomic_bool clean{false};
protected:
  void run() override {
    auto* value = new BenchmarkCore(root,data,options,gui,ready,result,completed);
    core.store(value);
    QTimer::singleShot(0,value,[value] { value->prepare(); });
    exec();
    core.store(nullptr);
    if (clean.load()) delete value;
  }
};

class BenchmarkRunner final : public QObject {
public:
  explicit BenchmarkRunner(BenchmarkOptions options) : options_(std::move(options)), model_(), proxy_(), view_() {
    proxy_.setSourceModel(&model_); view_.setModel(&proxy_);
    view_.setFont(QFont(QStringLiteral("Microsoft YaHei"),9));
    view_.resize(1100,650); view_.setAlternatingRowColors(true);
    view_.setSortingEnabled(true); view_.horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view_.setColumnWidth(0,260); view_.setColumnWidth(5,290); view_.show();
    sampler_.setInterval(5); sampler_.setTimerType(Qt::PreciseTimer);
    connect(&sampler_, &QTimer::timeout, this, [this] {
      const auto time = nowNs();
      if (pulseAt_) guiGapNs_ = qMax(guiGapNs_,time-pulseAt_);
      pulseAt_ = time; privatePeak_ = qMax(privatePeak_,currentPrivateBytes());
    });
  }
  int run() {
    if (!temporary_.isValid()) { writeJsonLine({{QStringLiteral("kind"),QStringLiteral("fatal")},{QStringLiteral("error"),QStringLiteral("temporary directory unavailable")}}); return 2; }
    qputenv("KQPET_DATA_ROOT",temporary_.path().toUtf8());
    const qint64 fixtureStart = nowNs();
    processBaseline_ = currentPrivateBytes();
    auto data = createPerformanceDataset(options_.spec,QDir(temporary_.path()).filePath(QStringLiteral("performance-input")));
    if (!data->error.isEmpty()) { writeJsonLine({{QStringLiteral("kind"),QStringLiteral("fatal")},{QStringLiteral("error"),data->error}}); return 2; }
    thread_ = new BenchmarkThread;
    thread_->root = temporary_.path(); thread_->data = data; thread_->options = options_; thread_->gui = this;
    thread_->ready = [this,fixtureStart](const QJsonObject& description) {
      QJsonObject row = description;
      row.insert(QStringLiteral("kind"),QStringLiteral("dataset"));
      row.insert(QStringLiteral("fixturePreparationNsExcluded"),nowNs()-fixtureStart);
      row.insert(QStringLiteral("processId"),QCoreApplication::applicationPid());
      row.insert(QStringLiteral("qtVersion"),QString::fromLatin1(qVersion()));
      row.insert(QStringLiteral("binaryBuildVersion"),BuildInfo::version());
      row.insert(QStringLiteral("binaryBuildTimeUtc"),BuildInfo::buildTimeUtc());
      row.insert(QStringLiteral("binaryGitHeadNotSourceDigest"),BuildInfo::gitCommit());
#ifdef _MSC_FULL_VER
      row.insert(QStringLiteral("msvcFullVersion"),qint64(_MSC_FULL_VER));
#endif
#ifdef NDEBUG
      row.insert(QStringLiteral("releaseAssertionsDisabled"),true);
#else
      row.insert(QStringLiteral("releaseAssertionsDisabled"),false);
#endif
      row.insert(QStringLiteral("architecture"),QSysInfo::currentCpuArchitecture());
      row.insert(QStringLiteral("guiDpiPercent"),qRound(view_.devicePixelRatioF()*100));
      row.insert(QStringLiteral("guiLogicalDpi"),view_.logicalDpiY());
      row.insert(QStringLiteral("warmupRequested"),options_.warmup);
      row.insert(QStringLiteral("samplesRequested"),options_.samples);
      row.insert(QStringLiteral("cacheMode"),options_.cacheMode);
      row.insert(QStringLiteral("operatingSystem"),QSysInfo::prettyProductName());
      row.insert(QStringLiteral("measurementScope"),QStringLiteral("production-AssetAnalysisController-Repository-RawCache-DerivationCache-Worker-to-AssetAnalysisModel-visible-viewport"));
      row.insert(QStringLiteral("computeResponsivenessProbe"),QStringLiteral("at most one priority no-op pending; attempted every 5 ms on Core"));
      row.insert(QStringLiteral("sourceCachePolicy"),QStringLiteral("production default RawCache/DerivationCache and persisted indexes; initial click attaches an empty cache; all reads and derivation remain inside click timing"));
      writeJsonLine(row);
      if (!row.value(QStringLiteral("error")).toString().isEmpty()) { failed_ = true; loop_.quit(); return; }
      next();
    };
    thread_->result = [this](QJsonObject row,std::shared_ptr<const AnalysisWorkResult> result) { visible(std::move(row),std::move(result)); };
    thread_->completed = [this](QJsonObject row) { completed(std::move(row)); };
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
      writeJsonLine({{QStringLiteral("kind"),QStringLiteral("timeout")},{QStringLiteral("caseId"),options_.spec.id()},
          {QStringLiteral("sampleIndex"),runNumber_},{QStringLiteral("timeoutMs"),options_.timeoutMs}});
      failed_ = true; loop_.quit();
    });
    deadline_.start(options_.timeoutMs); thread_->start(); loop_.exec(); sampler_.stop(); deadline_.stop();
    const auto thread = thread_;
    if (auto* core = thread->core.load()) QMetaObject::invokeMethod(core,[thread,core] {
      thread->clean.store(core->close()); thread->quit();
    },Qt::QueuedConnection);
    QEventLoop stopping;
    connect(thread,&QThread::finished,&stopping,&QEventLoop::quit);
    QTimer::singleShot(2500,&stopping,&QEventLoop::quit);
    if (thread->isRunning()) stopping.exec();
    const bool stopped = !thread->isRunning() && thread->clean.load();
    temporary_.setAutoRemove(stopped);
    writeJsonLine({{QStringLiteral("kind"),QStringLiteral("complete")},{QStringLiteral("caseId"),options_.spec.id()},
        {QStringLiteral("measuredSamples"),qMax(0,runNumber_-options_.warmup)},
        {QStringLiteral("warmupRequested"),options_.warmup},{QStringLiteral("samplesRequested"),options_.samples},
        {QStringLiteral("shutdownClean"),stopped},{QStringLiteral("allSamplesCorrect"),!failed_}});
    if (!thread->isRunning()) delete thread; // A live runtime is retained until this isolated process exits.
    return failed_ || !stopped ? 2 : 0;
  }
private:
  void next() {
    if (runNumber_ >= options_.warmup + options_.samples) { loop_.quit(); return; }
    privateBaseline_ = currentPrivateBytes(); privatePeak_ = privateBaseline_;
    guiGapNs_ = 0; pulseAt_ = nowNs(); sampler_.start();
    deadline_.start(options_.timeoutMs);
    const bool warmup = runNumber_ < options_.warmup;
    const int index = warmup ? runNumber_ : runNumber_ - options_.warmup;
    const qint64 clickedAt = nowNs();
    if (auto* core = thread_->core.load()) QMetaObject::invokeMethod(core,[core,index,warmup,clickedAt] { core->start(index,warmup,clickedAt); },Qt::QueuedConnection);
  }
  void visible(QJsonObject row,std::shared_ptr<const AnalysisWorkResult> result) {
    if (result) {
      proxy_.setQuery({}); proxy_.setAssetFilter(PetAssetFilter::All);
      QElapsedTimer timer; timer.start(); model_.setOverview(result->overview);
      row.insert(QStringLiteral("guiModelCommitNs"),timer.nsecsElapsed());
      row.insert(QStringLiteral("guiCommittedRows"),model_.rowCount());
      row.insert(QStringLiteral("correct"),row.value(QStringLiteral("correct")).toBool() && model_.rowCount()==result->overview.pets.size());
      timer.restart();
      QImage painted(view_.size(),QImage::Format_ARGB32_Premultiplied);
      painted.fill(Qt::transparent); view_.render(&painted);
      row.insert(QStringLiteral("guiPaintNs"),timer.nsecsElapsed());
      row.insert(QStringLiteral("clickToVisibleNs"),nowNs()-row.value(QStringLiteral("clickMonotonicNs")).toInteger());
      row.insert(QStringLiteral("visibleRows"),view_.viewport()->height()/qMax(1,view_.verticalHeader()->defaultSectionSize()));
      timer.restart();
      proxy_.setQuery(QStringLiteral("性能精灵1")); proxy_.sort(AssetAnalysisModel::CurrentPower,Qt::DescendingOrder);
      const int rows = proxy_.rowCount();
      row.insert(QStringLiteral("guiSearchFilterSortNs"),timer.nsecsElapsed());
      row.insert(QStringLiteral("filteredRows"),rows);
      row.insert(QStringLiteral("searchDebounceNsExcluded"),qint64(150000000));
    }
    row.insert(QStringLiteral("guiEventLoopMaximumGapNs"),qMax(guiGapNs_,nowNs()-pulseAt_));
    row.insert(QStringLiteral("privateBytesAtGuiCompletion"),qint64(currentPrivateBytes()));
    if (result) {
      row.insert(QStringLiteral("privateBytesAtVisible"),qint64(currentPrivateBytes()));
      row.insert(QStringLiteral("privateBytesSampledPeakAtVisible"),qint64(qMax(privatePeak_,currentPrivateBytes())));
    }
    if (auto* core = thread_->core.load()) QMetaObject::invokeMethod(core,[core,row] { core->finalize(row); },Qt::QueuedConnection);
  }
  void completed(QJsonObject row) {
    sampler_.stop(); deadline_.stop();
    privatePeak_ = qMax(privatePeak_,currentPrivateBytes());
    row.insert(QStringLiteral("privateBytesBaseline"),qint64(privateBaseline_));
    row.insert(QStringLiteral("privateBytesSampledPeak"),qint64(privatePeak_));
    row.insert(QStringLiteral("privateBytesSampledDelta"),qint64(privatePeak_-privateBaseline_));
    row.insert(QStringLiteral("privateBytesSamplingIntervalMs"),5);
    row.insert(QStringLiteral("privateBytesBaselineBeforeDataset"),qint64(processBaseline_));
    row.insert(QStringLiteral("privateBytesIncrementFromBeforeDataset"),qint64(privatePeak_ > processBaseline_ ? privatePeak_-processBaseline_ : 0));
    row.insert(QStringLiteral("processPeakIsSampledNotExact"),true);
    if (!row.value(QStringLiteral("correct")).toBool() ||
        (row.contains(QStringLiteral("snapshotSaved")) && !row.value(QStringLiteral("snapshotSaved")).toBool())) failed_ = true;
    writeJsonLine(row); ++runNumber_; QTimer::singleShot(0,this,[this] { next(); });
  }
  BenchmarkOptions options_;
  QTemporaryDir temporary_;
  AssetAnalysisModel model_; AssetAnalysisFilterProxyModel proxy_; QTableView view_;
  QTimer sampler_,deadline_; QEventLoop loop_;
  BenchmarkThread* thread_ = nullptr;
  int runNumber_ = 0;
  bool failed_ = false;
  qint64 pulseAt_ = 0,guiGapNs_ = 0;
  quint64 privateBaseline_ = 0,privatePeak_ = 0,processBaseline_ = 0;
};
}

int main(int argc,char* argv[]) {
  qputenv("QT_QPA_PLATFORM","offscreen");
  qputenv("QT_SCALE_FACTOR","1");
  QApplication application(argc,argv);
  // Isolated offscreen QPA does not enumerate Windows fonts automatically.
  // Load the same installed Chinese font before timing any rendering work.
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  if (argc == 1) return legacySmoke(application);
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("v2 deterministic offline performance harness; no game/network requests"));
  parser.addHelpOption();
  const auto option = [&](const QString& name,const QString& text,const QString& value = {}) {
    parser.addOption(QCommandLineOption(name,text,value.isEmpty()?QString():QStringLiteral("value"),value));
  };
  option(QStringLiteral("list-cases"),QStringLiteral("Print the full planned matrix as JSONL"));
  option(QStringLiteral("case"),QStringLiteral("Select one case ID from --list-cases"),QStringLiteral("custom"));
  option(QStringLiteral("pets"),QStringLiteral("Requested pet count"),QStringLiteral("2000"));
  option(QStringLiteral("goods"),QStringLiteral("Requested goods count or actual"),QStringLiteral("200"));
  option(QStringLiteral("match"),QStringLiteral("zero/sparse/normal/full"),QStringLiteral("normal"));
  option(QStringLiteral("detail-percent"),QStringLiteral("0/50/100"),QStringLiteral("100"));
  option(QStringLiteral("detail-kib"),QStringLiteral("8/32; actual bytes are reported"),QStringLiteral("8"));
  option(QStringLiteral("cost-items"),QStringLiteral("1/3/10; actual catalog is preserved"),QStringLiteral("3"));
  option(QStringLiteral("cultivation-items"),QStringLiteral("1/3/10; the tenth distinct code is unknown"),QStringLiteral("3"));
  option(QStringLiteral("seed"),QStringLiteral("Deterministic fixture seed"),QStringLiteral("20260909"));
  option(QStringLiteral("warmup"),QStringLiteral("Excluded warmup iterations"),QStringLiteral("1"));
  option(QStringLiteral("samples"),QStringLiteral("Measured iterations"),QStringLiteral("3"));
  option(QStringLiteral("cache"),QStringLiteral("warm/cold; cold requires a new process and one sample"),QStringLiteral("warm"));
  option(QStringLiteral("timeout-ms"),QStringLiteral("Per phase/run observation deadline"),QStringLiteral("180000"));
  option(QStringLiteral("cancel-after-ms"),QStringLiteral("Cancel this many ms after Worker submission; -1 disables"),QStringLiteral("-1"));
  option(QStringLiteral("cancel-phase"),QStringLiteral("preparation/worker; choose the actual production stage to cancel"),QStringLiteral("worker"));
  option(QStringLiteral("no-phase-probes"),QStringLiteral("Skip separate pure-API timing probes"));
  option(QStringLiteral("no-save"),QStringLiteral("Skip the separately measured snapshot write"));
  parser.process(application);
  for (const QString& name : {QStringLiteral("pets"),QStringLiteral("detail-percent"),QStringLiteral("detail-kib"),
      QStringLiteral("cost-items"),QStringLiteral("cultivation-items"),QStringLiteral("warmup"),QStringLiteral("samples"),
      QStringLiteral("timeout-ms"),QStringLiteral("cancel-after-ms"),QStringLiteral("seed")}) {
    bool numeric = false;
    if (name == QStringLiteral("seed")) parser.value(name).toUInt(&numeric);
    else parser.value(name).toInt(&numeric);
    if (!numeric) { writeJsonLine({{QStringLiteral("kind"),QStringLiteral("fatal")},
        {QStringLiteral("error"),QStringLiteral("non-integer option: %1").arg(name)}}); return 2; }
  }
  if (parser.isSet(QStringLiteral("list-cases"))) {
    for (const auto& entry : performanceMatrix()) writeJsonLine(entry.json());
    return 0;
  }
  BenchmarkOptions options;
  options.spec.pets = parser.value(QStringLiteral("pets")).toInt();
  const QString goods = parser.value(QStringLiteral("goods")); options.spec.actualCatalog = goods == QStringLiteral("actual");
  options.spec.goods = options.spec.actualCatalog ? 1 : goods.toInt(); options.spec.match = parser.value(QStringLiteral("match"));
  options.spec.detailPercent = parser.value(QStringLiteral("detail-percent")).toInt();
  options.spec.detailKiB = parser.value(QStringLiteral("detail-kib")).toInt();
  options.spec.costItems = parser.value(QStringLiteral("cost-items")).toInt();
  options.spec.cultivationItems = parser.value(QStringLiteral("cultivation-items")).toInt();
  options.spec.seed = parser.value(QStringLiteral("seed")).toUInt();
  if (parser.value(QStringLiteral("case")) != QStringLiteral("custom")) {
    bool found = false;
    for (const auto& entry : performanceMatrix()) if (entry.id() == parser.value(QStringLiteral("case"))) { options.spec=entry; found=true; break; }
    if (!found) { writeJsonLine({{QStringLiteral("kind"),QStringLiteral("fatal")},{QStringLiteral("error"),QStringLiteral("unknown case ID")}}); return 2; }
  }
  options.spec.seed = parser.value(QStringLiteral("seed")).toUInt();
  options.warmup = parser.value(QStringLiteral("warmup")).toInt(); options.samples = parser.value(QStringLiteral("samples")).toInt();
  options.cacheMode = parser.value(QStringLiteral("cache")); options.timeoutMs = parser.value(QStringLiteral("timeout-ms")).toInt();
  options.cancelAfterMs = parser.value(QStringLiteral("cancel-after-ms")).toInt();
  options.cancelPhase = parser.value(QStringLiteral("cancel-phase"));
  options.probes = !parser.isSet(QStringLiteral("no-phase-probes")); options.save = !parser.isSet(QStringLiteral("no-save"));
  const QString error = options.spec.validationError();
  if (!error.isEmpty() || options.warmup < 0 || options.warmup > 100 || options.samples < 1 || options.samples > 1000 ||
      options.timeoutMs < 100 || options.timeoutMs > 3600000 ||
      options.cancelAfterMs < -1 || options.cancelAfterMs > options.timeoutMs ||
      (options.cancelPhase != QStringLiteral("worker") && options.cancelPhase != QStringLiteral("preparation")) ||
      (options.cacheMode != QStringLiteral("warm") && options.cacheMode != QStringLiteral("cold")) ||
      (options.cacheMode == QStringLiteral("cold") && (options.warmup != 0 || options.samples != 1))) {
    writeJsonLine({{QStringLiteral("kind"),QStringLiteral("fatal")},{QStringLiteral("error"),error.isEmpty()?QStringLiteral("invalid run/cache controls"):error}}); return 2;
  }
  BenchmarkRunner runner(options);
  return runner.run();
}
