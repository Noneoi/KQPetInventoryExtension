#include "data_update_service.h"
#include "application/catalog/battle_replay_catalog_pack.h"
#include "storage/storage_service.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <atomic>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace DataUpdateInternal {
struct State {
  QMutex mutex;
  DataUpdateService* receiver = nullptr;
  std::atomic_bool closing{false};
  std::atomic_bool cancelRequested{false};
  std::atomic_bool publishing{false};
  void progress(const QString& message) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, message] { target->receiveProgress(message); }, Qt::QueuedConnection);
  }
  void progressState(const PublicUpdateProgress& progress) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, progress] { target->receiveProgressState(progress); }, Qt::QueuedConnection);
  }
  void finish(bool success, const QStringList& components, const QString& message, const QString& attempt = {}) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, success, components, message, attempt] {
      target->receiveFinished(success, components, message, attempt);
    }, Qt::QueuedConnection);
  }
};

namespace {
bool saveJson(const QString& path, const QJsonObject& object) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QJsonObject readJson(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024 * 1024 ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
void recordStopped(const QString& root, const QString& attempt, bool cancelled, const QString& error) {
  const auto path = QDir(root).filePath(QStringLiteral("public/transactions/%1/receipt.json").arg(attempt));
  auto value = readJson(path);
  if (value.value("attempt") != attempt || value.value("diskCommitted").toBool()) return;
  value.insert("state", cancelled ? "cancelled" : "failed");
  value.insert("checkedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  value.insert("error", error);
  saveJson(path, value);
}
bool copyResource(const QString& resource, const QString& output) {
  QFile input(resource);
  if (!input.open(QIODevice::ReadOnly) || input.size() > 32 * 1024 * 1024) return false;
  const QByteArray bytes = input.readAll();
  QFile existing(output);
  if (existing.open(QIODevice::ReadOnly) && existing.size() == bytes.size() && existing.readAll() == bytes) return true;
  existing.close();
  QSaveFile file(output);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

class Job final : public QObject {
public:
  Job(QString root, QStringList components, QString attempt, std::shared_ptr<State> state, QObject* parent)
      : QObject(parent), root_(std::move(root)), requestedComponents_(std::move(components)),
        state_(std::move(state)), process_(this), timer_(this), attempt_(std::move(attempt)) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    process_.setProcessEnvironment(environment);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) finish(false, {}, QStringLiteral("无法启动数据更新工具：") + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus status) {
      readOutput();
      if (!tail_.trimmed().isEmpty()) parse(tail_);
      const bool success = hasResult_ && resultSuccess_ && code == 0 && status == QProcess::NormalExit;
      if (!hasResult_ || status != QProcess::NormalExit || (code != 0 && code != 2)) candidateAttempt_.clear();
      const QString message = !resultMessage_.isEmpty() ? resultMessage_
          : success ? QStringLiteral("数据检查完成；已有数据继续从本地读取")
          : QStringLiteral("公共数据检查未完成，原有数据已保留");
      finish(success, components_, message);
    });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] {
      elapsedMs_ += 250;
      if (state_->closing.load() || state_->cancelRequested.load() || elapsedMs_ >= 15 * 60 * 1000) {
        resultMessage_ = state_->closing.load() || state_->cancelRequested.load() ? QStringLiteral("检查已取消，原有资料已保留") : QStringLiteral("数据更新超时，原有数据已保留");
#ifdef Q_OS_WIN
        if (job_) TerminateJobObject(job_, 1);
#endif
        process_.kill();
      }
    });
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
      arguments->flags |= CREATE_NO_WINDOW;
    });
    connect(&process_, &QProcess::started, this, [this] {
      // Closing the job also stops a Python/Java child, including application
      // exit while a public download is underway. No helper remains resident.
      job_ = CreateJobObjectW(nullptr, nullptr);
      if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process_.processId()));
        if (child) { AssignProcessToJobObject(job_, child); CloseHandle(child); }
      }
    });
#endif
    QTimer::singleShot(0, this, [this] { start(); });
  }
  ~Job() override {
#ifdef Q_OS_WIN
    if (job_) CloseHandle(job_);
#endif
  }
private:
  void start() {
    const QString directory = QDir(root_).filePath(QStringLiteral("public/transactions/%1").arg(attempt_));
    if (QFileInfo::exists(directory)) { finish(false, {}, QStringLiteral("检查请求发生冲突，请重试")); return; }
    const auto selected = requestedComponents_.isEmpty() ? DataUpdateService::componentNames() : requestedComponents_;
    const QJsonObject request{{"schema", 1}, {"attempt", attempt_}, {"requestedComponents", QJsonArray::fromStringList(selected)},
        {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    auto receipt = request;
    receipt.insert("state", "preparing"); receipt.insert("checked", false); receipt.insert("candidatePassed", false);
    receipt.insert("diskCommitted", false); receipt.insert("sessionApplied", false);
    if (!saveJson(QDir(directory).filePath(QStringLiteral("request.json")), request) ||
        !saveJson(QDir(directory).filePath(QStringLiteral("receipt.json")), receipt) ||
        !saveJson(QDir(root_).filePath(QStringLiteral("public/reports/latest-request.json")), request)) {
      finish(false, {}, QStringLiteral("无法保存检查请求，原有资料已保留")); return;
    }
    requestOwned_ = true;
    state_->progressState({PublicUpdatePhase::PreparingTools, attempt_, {}, QStringLiteral("正在准备更新工具")});
    if (state_->closing.load() || state_->cancelRequested.load()) { finish(false, {}, QStringLiteral("检查已取消，原有资料已保留")); return; }
    const QString scripts = QDir(root_).filePath(QStringLiteral("data-tools/scripts"));
    if (!QDir().mkpath(QDir(scripts).filePath(QStringLiteral("baseline")))) {
      finish(false, {}, QStringLiteral("无法创建本地更新工具目录")); return;
    }
    const QStringList names{QStringLiteral("public_data_updater.py"), QStringLiteral("public_dataset_store.py"), QStringLiteral("bootstrap-public-data.ps1"),
        QStringLiteral("public_resource_session.py"), QStringLiteral("public_resource_plan.py"),
        QStringLiteral("public_catalog_quality.py"),
        QStringLiteral("public_skill_fidelity.py"),
        QStringLiteral("public_cultivation_supplement.py"), QStringLiteral("battle_replay/fetch_reference.py"),
        QStringLiteral("public_names_updater.py"),QStringLiteral("public_icon_updater.py"),QStringLiteral("public_routine_updater.py"),
        QStringLiteral("public_routine_rules.py"),
        QStringLiteral("public_activity_exchange_updater.py"),QStringLiteral("activity_evolution_selector.py"),
        QStringLiteral("activity_reward_structures.py"),QStringLiteral("activity_periods.py"),QStringLiteral("activity_trade_facts.py"),
        QStringLiteral("generate_pet_detail_data.py"), QStringLiteral("generate_pet_skill_data.py"),
        QStringLiteral("public_skill_updater.py"), QStringLiteral("generate_shop_exchange_data.py"),
        QStringLiteral("generate_stargod_icons.py")};
    for (const QString& name : names) {
      if (state_->closing.load() || state_->cancelRequested.load()) { finish(false, {}, QStringLiteral("检查已取消，原有资料已保留")); return; }
      const QString output = QDir(scripts).filePath(name);
      if (!QDir().mkpath(QFileInfo(output).absolutePath()) ||
          !copyResource(QStringLiteral(":/kqpet/data-updater/") + name, output)) {
        finish(false, {}, QStringLiteral("无法准备内置更新工具：") + name); return;
      }
    }
    // A selected update may expand to its declared dependencies; preparation
    // always exports the complete, mutually compatible updater tool set.
    {
      const QStringList replayNames{
          QStringLiteral("public_battle_replay_updater.py"),
          QStringLiteral("public_replay_fidelity.py"), QStringLiteral("public_replay_regression.py"),
          QStringLiteral("battle-replay-regression-manifest.json"),
          QStringLiteral("KQBattleReplayUpdateCheck.exe"), QStringLiteral("KQPublicDatasetCheck.exe"), QStringLiteral("Qt6Core.dll"),
          QStringLiteral("msvcp140.dll"), QStringLiteral("msvcp140_1.dll"),
          QStringLiteral("vcruntime140.dll"), QStringLiteral("vcruntime140_1.dll"),
          QStringLiteral("battle_replay/build_catalog.py"), QStringLiteral("battle_replay/catalog_pack.py"),
          QStringLiteral("battle_replay/fetch_pet_reference.py"),
          QStringLiteral("battle_replay/fetch_formation_reference.py"), QStringLiteral("battle_replay/extract_buff_tables.py"),
          QStringLiteral("battle_replay/extract_skill_tables.py"), QStringLiteral("battle_replay/extract_skill_embed_effect.py"),
          QStringLiteral("battle_replay/extract_entry_tables.py"), QStringLiteral("battle_replay/extract_pet_mechanisms.py"),
          QStringLiteral("battle_replay/extract_pet_identity.py"), QStringLiteral("battle_replay/extract_presentation_config.py"),
          QStringLiteral("battle_replay/extract_pet_dragons.py"), QStringLiteral("battle_replay/extract_replay_supplements.py"),
          QStringLiteral("battle_replay/extract_replay_cultivation.py"), QStringLiteral("battle_replay/link_battle_tables.py"),
          QStringLiteral("battle_replay/audit_reference_coverage.py"), QStringLiteral("battle_replay/update_generated_evidence.py"),
          QStringLiteral("battle_replay/audit_source_coverage.py"),
          QStringLiteral("battle_replay/update_source_bundle.py"), QStringLiteral("battle_replay/update-sources.zip")};
      for (const QString& name : replayNames) {
        if (state_->closing.load() || state_->cancelRequested.load()) { finish(false, {}, QStringLiteral("检查已取消，原有资料已保留")); return; }
        const QString output = QDir(scripts).filePath(name);
        if (!QDir().mkpath(QFileInfo(output).absolutePath()) ||
            !copyResource(QStringLiteral(":/kqpet/data-updater/") + name, output)) {
          finish(false, {}, QStringLiteral("无法准备内置回放更新工具：") + name); return;
        }
      }
      // Reuse the existing embedded JSON as a streamed compressed baseline;
      // never put a second full JSON copy in resources or the tools directory.
      const QString error = BattleReplayCatalogPack::exportEmbeddedBaseline(
          QDir(scripts).filePath(QStringLiteral("baseline/battle-replay-catalog.brpack")), &state_->cancelRequested);
      if (!error.isEmpty()) {
        finish(false, {}, QStringLiteral("无法准备内置回放资料：") + error); return;
      }
    }
    for (const QString& name : {QStringLiteral("pet-detail-data.json"), QStringLiteral("pet-skill-data.json"),
         QStringLiteral("shop-exchange-data.json"),QStringLiteral("activity-exchange-data.json")}) {
      if (!copyResource(QStringLiteral(":/kqpet/") + name, QDir(scripts).filePath(QStringLiteral("baseline/") + name))) {
        finish(false, {}, QStringLiteral("无法准备内置公共数据")); return;
      }
    }
    const QString iconBaseline = QDir(scripts).filePath(QStringLiteral("baseline/stargod-icons"));
    if (!QDir().mkpath(iconBaseline) || !copyResource(QStringLiteral(":/kqpet/stargod-icons/sources.json"),
        QDir(iconBaseline).filePath(QStringLiteral("sources.json")))) {
      finish(false, {}, QStringLiteral("无法准备内置图标来源记录")); return;
    }
    QFile catalog(QStringLiteral(":/kqpet/pet-detail-data.json"));
    if (!catalog.open(QIODevice::ReadOnly)) { finish(false, {}, QStringLiteral("无法读取内置图标目录")); return; }
    const auto starDefinitions = QJsonDocument::fromJson(catalog.readAll()).object().value(QStringLiteral("stargods")).toObject();
    for (auto star = starDefinitions.begin(); star != starDefinitions.end(); ++star) {
      bool valid = false; const int id = star.key().toInt(&valid);
      if (!valid || id <= 0) continue;
      const QString name = QString::number(id) + QStringLiteral(".png");
      const QString resource = QStringLiteral(":/kqpet/stargod-icons/") + name;
      if (QFile::exists(resource) && !copyResource(resource,QDir(iconBaseline).filePath(name))) {
        finish(false, {}, QStringLiteral("无法准备内置星神图标")); return;
      }
    }
#ifdef Q_OS_WIN
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) { finish(false, {}, QStringLiteral("无法定位系统脚本运行器")); return; }
    process_.setProgram(QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe")));
#else
    process_.setProgram(QStringLiteral("pwsh"));
#endif
    QStringList arguments{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"), QStringLiteral("-File"), QDir(scripts).filePath(QStringLiteral("bootstrap-public-data.ps1")),
        QStringLiteral("-DataRoot"), root_, QStringLiteral("-PrepareOnly"), QStringLiteral("-AttemptId"), attempt_};
    if (!requestedComponents_.isEmpty())
      arguments << QStringLiteral("-Components") << requestedComponents_.join(QLatin1Char(','));
    process_.setArguments(arguments);
    process_.setWorkingDirectory(scripts);
    state_->progress(requestedComponents_.isEmpty()
        ? QStringLiteral("正在检查全部官方数据；首次运行会准备可重复使用的本地解析工具")
        : QStringLiteral("正在检查所选官方数据；首次运行会准备可重复使用的本地解析工具"));
    timer_.start();
    process_.start();
  }
  void parse(const QByteArray& line) {
    const auto object = QJsonDocument::fromJson(line).object();
    const QString event = object.value(QStringLiteral("event")).toString();
    if (event == QStringLiteral("candidate")) {
      const QString attempt = object.value(QStringLiteral("attempt")).toString();
      static const QRegularExpression valid(QStringLiteral("^[a-f0-9]{32}$"));
      if (valid.match(attempt).hasMatch() && attempt == attempt_) candidateAttempt_ = attempt;
    }
    if (event == QStringLiteral("stage") && object.value("attempt") == attempt_) {
      static const QHash<QString, PublicUpdatePhase> phases{{"preparingTools", PublicUpdatePhase::PreparingTools},
          {"fetchingResources", PublicUpdatePhase::FetchingResources}, {"parsing", PublicUpdatePhase::Parsing},
          {"validating", PublicUpdatePhase::Validating}};
      const auto phase = object.value("phase").toString();
      if (phases.contains(phase)) {
        PublicUpdateProgress progress;
        progress.phase = phases.value(phase); progress.attempt = attempt_;
        const auto component = object.value("component").toString();
        if (DataUpdateService::componentNames().contains(component)) progress.component = component;
        const auto completed = object.value("completed").toInteger(-1), total = object.value("total").toInteger(-1);
        if (completed >= 0 && total >= completed) { progress.completed = completed; progress.total = total; }
        state_->progressState(progress);
      }
    }
    if (event == QStringLiteral("progress")) {
      QString message = object.value(QStringLiteral("message")).toString();
      if (message == QStringLiteral("Preparing local data update tools (Python)")) message = QStringLiteral("首次准备本地数据更新工具");
      // Free-form extractor output is diagnostic text. User stage/counts come
      // only from the structured event above, never from guessing its wording.
    }
    if (event == QStringLiteral("component")) {
      const QString status = object.value(QStringLiteral("status")).toString();
      const QString component = object.value(QStringLiteral("component")).toString();
      if ((status == QStringLiteral("updated") || status == QStringLiteral("unchanged")) && !components_.contains(component)) components_.append(component);
      static const QHash<QString,QString> labels{{QStringLiteral("pets"),QStringLiteral("精灵与养成资料")},
          {QStringLiteral("skills"),QStringLiteral("精灵技能资料")},
          {QStringLiteral("battleReplay"),QStringLiteral("战斗回放资料")},
          {QStringLiteral("shop"),QStringLiteral("兑换商店")},{QStringLiteral("images"),QStringLiteral("精灵图片")},
          {QStringLiteral("icons"),QStringLiteral("星神与属性图标")},{QStringLiteral("routines"),QStringLiteral("日常与活动")}};
      const QString error = object.value(QStringLiteral("error")).toString();
      state_->progress(labels.value(component,component) + QStringLiteral("：") +
          (!error.isEmpty() ? QStringLiteral("部分内容未完成，已保留可用资料；可在更新结果查看详情") : status == QStringLiteral("updated") ? QStringLiteral("候选资料已准备") :
           status == QStringLiteral("unchanged") ? QStringLiteral("内容未变化") : QStringLiteral("未完成，保留已有数据")));
    }
    if (event == QStringLiteral("finished")) {
      hasResult_ = true;
      resultSuccess_ = object.value(QStringLiteral("success")).toBool() && object.value(QStringLiteral("complete")).toBool(true);
      resultMessage_ = object.value(QStringLiteral("error")).toString(object.value(QStringLiteral("message")).toString());
      const auto values = object.value(QStringLiteral("components")).toObject();
      QStringList errors;
      for (auto it = values.begin(); it != values.end(); ++it) {
        const auto item = it.value().toObject();
        const auto error = item.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) errors.append(error);
        if (!error.isEmpty() || item.value(QStringLiteral("status")).toString() == QStringLiteral("retained") ||
            item.value(QStringLiteral("status")).toString() == QStringLiteral("failed")) resultSuccess_ = false;
      }
      // Phase text such as "waiting to apply" must not survive the later
      // successful Core publication. Only durable outcome details are kept.
      if (!candidateAttempt_.isEmpty()) resultMessage_ = errors.join(QStringLiteral("；"));
      else if (!errors.isEmpty()) resultMessage_ = errors.join(QStringLiteral("；"));
      const auto shopCounts = values.value(QStringLiteral("shop")).toObject().value(QStringLiteral("scanCounts")).toObject();
      if (!shopCounts.isEmpty()) resultMessage_.prepend(
          QStringLiteral("兑换商店扫描：%1 个商店，自动添加 %2 项，%3 项可手动补充。 ")
              .arg(shopCounts.value(QStringLiteral("shops")).toInt())
              .arg(shopCounts.value(QStringLiteral("automaticGoods")).toInt())
              .arg(shopCounts.value(QStringLiteral("manualGoods")).toInt()));
    }
  }
  void readOutput() {
    tail_ += process_.readAllStandardOutput();
    if (tail_.size() > 256 * 1024) tail_ = tail_.right(128 * 1024);
    qsizetype newline;
    while ((newline = tail_.indexOf('\n')) >= 0) {
      parse(tail_.left(newline).trimmed());
      tail_.remove(0, newline + 1);
    }
  }
  void finish(bool success, const QStringList& components, const QString& message) {
    if (done_) return;
    done_ = true;
    timer_.stop();
    const bool cancelled = state_->closing.load() || state_->cancelRequested.load();
    if (requestOwned_ && (candidateAttempt_.isEmpty() || cancelled)) recordStopped(root_, attempt_, cancelled, message);
    state_->finish(cancelled ? false : success, cancelled ? QStringList{} : components,
        cancelled ? QStringLiteral("检查已取消，原有资料已保留") : !success && candidateAttempt_.isEmpty()
          ? QStringLiteral("检查未完成，原有资料已保留；可重试或查看诊断") : message,
        cancelled ? QString{} : candidateAttempt_);
    deleteLater();
  }
  QString root_;
  QStringList requestedComponents_;
  std::shared_ptr<State> state_;
  QProcess process_;
  QTimer timer_;
  QByteArray tail_;
  QStringList components_;
  QString resultMessage_;
  QString candidateAttempt_;
  QString attempt_;
  int elapsedMs_ = 0;
  bool hasResult_ = false, resultSuccess_ = false, done_ = false;
  bool requestOwned_ = false;
#ifdef Q_OS_WIN
  HANDLE job_ = nullptr;
#endif
};
} // namespace
} // namespace DataUpdateInternal

struct DataUpdateService::Impl {
  QPointer<StorageService> storage;
  std::shared_ptr<DataUpdateInternal::State> state = std::make_shared<DataUpdateInternal::State>();
  bool busy = false;
  bool candidateComplete = false;
  bool pendingStart = false;
  QStringList preparedComponents;
  QStringList pendingComponents;
  QString attempt;
};
DataUpdateService::DataUpdateService(StorageService* storage, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->storage = storage;
  impl_->state->receiver = this;
  qRegisterMetaType<PublicUpdateProgress>();
}
DataUpdateService::~DataUpdateService() { close(); }
bool DataUpdateService::busy() const { return impl_->busy; }
bool DataUpdateService::canCancel() const { return impl_->busy && !impl_->state->publishing.load() && !impl_->state->cancelRequested.load(); }
bool DataUpdateService::cancelPreparation() {
  if (!canCancel()) return false;
  impl_->state->cancelRequested.store(true);
  emit progress(QStringLiteral("正在取消检查，原有资料保持可用"));
  return true;
}
QStringList DataUpdateService::componentNames() {
  return {QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("battleReplay"), QStringLiteral("shop"), QStringLiteral("images"),
          QStringLiteral("icons"), QStringLiteral("routines")};
}
bool DataUpdateService::requestUpdate(const QStringList& components) {
  if (impl_->busy || !impl_->storage || impl_->state->closing.load()) return false;
  // Only fixed names ever reach the script's command line.
  QStringList selected;
  for (const QString& name : componentNames())
    if (components.contains(name)) selected.append(name);
  if (selected.size() != components.size()) return false;
  if (selected.size() == componentNames().size()) selected.clear();
  auto state = impl_->state;
  state->cancelRequested.store(false); state->publishing.store(false);
  impl_->attempt = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
  impl_->pendingComponents = selected;
  impl_->pendingStart = true;
  impl_->busy = true;
  emit progressChanged({PublicUpdatePhase::PreparingTools, impl_->attempt, {}, QStringLiteral("正在安排公共资料检查")});
  tryStart();
  return true;
}
void DataUpdateService::tryStart() {
  if (!impl_->pendingStart || !impl_->busy || impl_->state->closing.load()) return;
  if (!impl_->storage) {
    impl_->pendingStart = false; impl_->busy = false;
    emit finished(false, {}, QStringLiteral("本地资料服务已停止，原有资料已保留"));
    return;
  }
  const auto state = impl_->state;
  const auto selected = impl_->pendingComponents;
  const auto attempt = impl_->attempt;
  const QString root = impl_->storage->dataRoot();
  if (!impl_->storage->postAuxiliary([root, selected, attempt, state](QObject* ioRoot) {
        new DataUpdateInternal::Job(root, selected, attempt, state, ioRoot);
      })) {
    // One explicit request is owned by this facade. Background status reads
    // may briefly occupy Auxiliary, but must not reject the user's request.
    QTimer::singleShot(150, this, [this] { tryStart(); });
    return;
  }
  impl_->pendingStart = false;
  impl_->pendingComponents.clear();
}
void DataUpdateService::close() {
  impl_->state->closing.store(true);
  impl_->state->cancelRequested.store(true);
  impl_->pendingStart = false;
  QMutexLocker guard(&impl_->state->mutex);
  impl_->state->receiver = nullptr;
}
void DataUpdateService::receiveProgress(const QString& message) { emit progress(message); }
void DataUpdateService::receiveProgressState(const PublicUpdateProgress& progress) { emit progressChanged(progress); }
void DataUpdateService::receiveFinished(bool success, const QStringList& components, const QString& message, const QString& attempt) {
  if (!attempt.isEmpty()) {
    if (impl_->state->cancelRequested.load()) {
      const auto root = impl_->storage->dataRoot();
      const auto state = impl_->state;
      const bool queued = impl_->storage->postAuxiliary([root, attempt, state](QObject*) {
        DataUpdateInternal::recordStopped(root, attempt, true, QStringLiteral("检查已取消，原有资料已保留"));
        state->finish(false, {}, QStringLiteral("检查已取消，原有资料已保留"));
      });
      if (!queued) QTimer::singleShot(100, this, [this, success, components, message, attempt] {
        if (!impl_->state->closing.load()) receiveFinished(success, components, message, attempt);
      });
      return;
    }
    impl_->state->publishing.store(true);
    impl_->candidateComplete = success;
    impl_->preparedComponents = components;
    emit progress(QStringLiteral("候选资料已准备，正在检查并应用完整版本"));
    emit progressChanged({PublicUpdatePhase::Applying, attempt, {}, QStringLiteral("正在应用完整资料版本")});
    emit candidatePrepared(attempt, message);
    return;
  }
  impl_->busy = false;
  emit finished(success, components, message);
}
void DataUpdateService::completePublication(bool applied, const QString& message) {
  if (!impl_->busy || impl_->state->closing.load()) return;
  const auto components = applied ? impl_->preparedComponents : QStringList{};
  const bool success = applied && impl_->candidateComplete;
  impl_->busy = false;
  impl_->preparedComponents.clear();
  emit finished(success, components, message);
}
