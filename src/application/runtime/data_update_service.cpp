#include "data_update_service.h"
#include "storage/storage_service.h"
#include "storage/public_catalog_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QJsonArray>
#include <QCoreApplication>
#include <QLockFile>
#include <QTemporaryDir>
#include <atomic>
#include <stdexcept>

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
  std::atomic_bool cancelling{false};
  void report(const QJsonObject& value) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, value] { target->receiveReport(value); }, Qt::QueuedConnection);
  }
  void progress(const QString& message) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, message] { target->receiveProgress(message); }, Qt::QueuedConnection);
  }
  void finish(bool success, const QStringList& components, const QString& message) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, success, components, message] {
      target->receiveFinished(success, components, message);
    }, Qt::QueuedConnection);
  }
};

namespace {
QJsonObject fullReceipt(const QString& root, const QJsonObject& incoming) {
  const QString attempt = incoming.value("attempt").toString();
  const auto path = PublicCatalogStore::safePath(root, "catalog/update-tasks/" + attempt + "/result.json");
  auto result = QFileInfo::exists(path) ? PublicCatalogStore::object(path) : QJsonObject{};
  if (!result.isEmpty() && result.value("attempt").toString() != attempt) throw std::runtime_error("更新记录身份不一致");
  // Progress messages deliberately abbreviate change lists and omit operation
  // logs. Native acknowledgement must preserve the full diagnostic on disk.
  for (auto it = incoming.begin(); it != incoming.end(); ++it) {
    if (it.key() == "components") {
      auto rows = result.value(it.key()).toObject(); const auto updates = it.value().toObject();
      for (auto row = updates.begin(); row != updates.end(); ++row) {
        auto stored = rows.value(row.key()).toObject(); const auto values = row.value().toObject();
        for (auto field = values.begin(); field != values.end(); ++field) {
          if (stored.contains(field.key()) && ((field.key() == "changes" && !field.value().toObject().isEmpty()) ||
              ((field.key() == "pendingActivities" || field.key() == "notices") && !field.value().toArray().isEmpty()))) continue;
          stored.insert(field.key(), field.value());
        }
        rows.insert(row.key(), stored);
      }
      result.insert(it.key(), rows);
    } else if (it.key() == "timings") {
      auto timings = result.value(it.key()).toObject(); const auto values = it.value().toObject();
      for (auto field = values.begin(); field != values.end(); ++field) timings.insert(field.key(), field.value());
      result.insert(it.key(), timings);
    } else result.insert(it.key(), it.value());
  }
  return result;
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

QJsonObject prunePublicData(const QString& root) {
  try {
    QLockFile publication(PublicCatalogStore::safePath(root, "catalog/publication.lock"));
    if (!publication.tryLock(0)) return {{"ok", false}, {"error", "其他任务正在应用公共资料，稍后自动重试清理"}};
    const auto pointer = QJsonDocument::fromJson(PublicCatalogStore::pointerBytes(root)).object();
    const QString python = PublicCatalogStore::safePath(root, "data-tools/python/python.exe");
    if (!QFileInfo::exists(python)) return {{"ok", false}, {"error", "更新工具尚未就绪，下次更新成功后清理"}};
    // Private helpers avoid overwriting scripts used by another in-flight
    // updater. Python also takes data-update.lock before scanning/removal.
    const QString maintenance = PublicCatalogStore::safePath(root, ".maintenance");
    if (!QDir().mkpath(maintenance)) throw std::runtime_error("无法创建公共资料清理目录");
    QTemporaryDir helpers(QDir(maintenance).filePath("public-retention-XXXXXX"));
    if (!helpers.isValid()) throw std::runtime_error("无法准备公共资料清理工具");
    for (const auto& name : {QStringLiteral("public_data_retention.py"), QStringLiteral("public_update_store.py"), QStringLiteral("public_update_support.py")})
      if (!copyResource(QStringLiteral(":/kqpet/data-updater/") + name, helpers.filePath(name)))
        throw std::runtime_error("公共资料清理工具不完整");
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
    process.start(python, {"-B", "-X", "utf8", helpers.filePath("public_data_retention.py"),
        "--data-root", root, "--revision", pointer.value("revision").toString(),
        "--publication-owner", QString::number(QCoreApplication::applicationPid())});
    if (!process.waitForStarted(5000)) throw std::runtime_error("公共资料清理工具启动失败");
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
      HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process.processId()));
      if (child) { AssignProcessToJobObject(job, child); CloseHandle(child); }
    }
#endif
    const bool ended = process.waitForFinished(30000);
    if (!ended) { process.kill(); process.waitForFinished(1000); }
#ifdef Q_OS_WIN
    if (job) CloseHandle(job);
#endif
    const auto output = process.readAllStandardOutput();
    auto result = output.size() <= 1024 * 1024 ? QJsonDocument::fromJson(output).object() : QJsonObject{};
    if (!ended || !result.contains("ok")) return {{"ok", false}, {"error", "公共资料清理未完成，下次更新后重试"},
        {"diagnostic", QString::fromUtf8(process.readAllStandardError().left(2048))}};
    if (process.exitCode() != 0 || process.exitStatus() != QProcess::NormalExit) result.insert("ok", false);
    return result;
  } catch (const std::exception& error) {
    return {{"ok", false}, {"error", QString::fromUtf8(error.what())}};
  }
}

class Job final : public QObject {
public:
  Job(QString root, QStringList components, QString attempt, bool verifyAll, std::shared_ptr<State> state, QObject* parent)
      : QObject(parent), root_(std::move(root)), requestedComponents_(std::move(components)),
        attempt_(std::move(attempt)), verifyAll_(verifyAll), state_(std::move(state)), process_(this), timer_(this) {
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
      if (report_.value("state").toString() == "prepared" && code == 0 && status == QProcess::NormalExit) {
        done_ = true; timer_.stop(); state_->report(report_); deleteLater(); return;
      }
      const bool success = hasResult_ && resultSuccess_ && code == 0 && status == QProcess::NormalExit;
      const QString message = !resultMessage_.isEmpty() ? resultMessage_
          : success ? QStringLiteral("数据检查完成；已有数据继续从本地读取")
          : QStringLiteral("公共数据检查未完成，原有数据已保留");
      finish(success, components_, message);
    });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] {
      elapsedMs_ += 250;
      if (elapsedMs_ % 1000 == 0 && report_.value("state").toString() != "prepared") {
        auto value = report_; value.insert("attempt", attempt_); value.insert("wallSeconds", elapsedMs_ / 1000.0);
        if (!value.contains("state")) value.insert("state", "preparing");
        state_->report(value);
      }
      if (state_->cancelling.load() && report_.value("state").toString() != "applying") {
        if (!cancelStarted_) {
          cancelStarted_ = elapsedMs_;
          try { PublicCatalogStore::save(PublicCatalogStore::safePath(root_, "catalog/update-tasks/" + attempt_ + "/cancel"), {}); }
          catch (const std::exception&) { /* Killing the helper also cannot publish a candidate. */ }
          state_->progress(QStringLiteral("正在取消；保留现用资料"));
        }
        if (elapsedMs_ - cancelStarted_ > 1500) {
#ifdef Q_OS_WIN
          if (job_) TerminateJobObject(job_, 1);
#endif
          process_.kill();
        }
      }
      if (state_->closing.load() || elapsedMs_ >= 15 * 60 * 1000) {
        resultMessage_ = state_->closing.load() ? QStringLiteral("数据更新已停止") : QStringLiteral("数据更新超时，原有数据已保留");
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
    try {
      const auto latest = PublicCatalogStore::safePath(root_, "catalog/public-update-status.json");
      try { if (QFileInfo::exists(latest)) report_ = PublicCatalogStore::object(latest); }
      catch (const std::exception&) { report_ = {}; }
      report_.insert("schema", 2); report_.insert("attempt", attempt_); report_.insert("state", "preparing");
      report_.insert("createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
      report_.insert("requested", QJsonArray::fromStringList(requestedComponents_.isEmpty() ? DataUpdateService::componentNames() : requestedComponents_));
      report_.insert("diskCommitted", false); report_.insert("sessionApplied", false); report_.insert("canCancel", true);
      for (const auto& key : {"error", "receiptError", "recovered", "timings", "action", "phase", "component", "wallSeconds", "downloadedBytes", "extractions", "reusedExtractions"}) report_.remove(key);
      PublicCatalogStore::save(latest, report_);
      state_->report(report_);
    } catch (const std::exception& error) { finish(false, {}, QString::fromUtf8(error.what())); return; }
    if (state_->closing.load() || state_->cancelling.load()) { finish(false, {}, QStringLiteral("数据更新已停止")); return; }
    const QString scripts = QDir(root_).filePath(QStringLiteral("data-tools/scripts"));
    if (!QDir().mkpath(QDir(scripts).filePath(QStringLiteral("baseline")))) {
      finish(false, {}, QStringLiteral("无法创建本地更新工具目录")); return;
    }
    const QStringList names{QStringLiteral("public_data_updater.py"), QStringLiteral("bootstrap-public-data.ps1"),
        QStringLiteral("public_update_support.py"), QStringLiteral("public_update_store.py"), QStringLiteral("public_data_quality.py"), QStringLiteral("public_data_retention.py"),
        QStringLiteral("public_names_updater.py"),QStringLiteral("public_icon_updater.py"),QStringLiteral("public_routine_updater.py"),
        QStringLiteral("public_activity_exchange_updater.py"),QStringLiteral("activity_evolution_selector.py"),
        QStringLiteral("activity_reward_structures.py"),QStringLiteral("activity_call_rewards.py"),QStringLiteral("activity_periods.py"),QStringLiteral("activity_navigation.py"),QStringLiteral("activity_trade_facts.py"),
        QStringLiteral("activity_constructed_offers.py"),QStringLiteral("activity_price_rules.py"),QStringLiteral("activity_service_discovery.py"),
        QStringLiteral("generate_pet_detail_data.py"), QStringLiteral("generate_pet_skill_data.py"),
        QStringLiteral("public_pet_relations.py"),
        QStringLiteral("public_skill_updater.py"), QStringLiteral("generate_shop_exchange_data.py"),
        QStringLiteral("generate_stargod_icons.py")};
    for (const QString& name : names) {
      if (!copyResource(QStringLiteral(":/kqpet/data-updater/") + name, QDir(scripts).filePath(name))) {
        finish(false, {}, QStringLiteral("无法准备内置更新工具：") + name); return;
      }
    }
    for (const QString& name : {QStringLiteral("pet-detail-data.json"), QStringLiteral("pet-skill-data.json"),
         QStringLiteral("shop-exchange-data.json"),QStringLiteral("activity-exchange-data.json"),
         QStringLiteral("routine-overview.json"),QStringLiteral("pet-image-index.json"),
         QStringLiteral("public-icon-index.json"),QStringLiteral("activity-evolution-data.json"),QStringLiteral("official-versions.json")}) {
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
        QStringLiteral("-DataRoot"), root_, QStringLiteral("-Attempt"), attempt_, QStringLiteral("-PrepareOnly")};
    if (verifyAll_) arguments << QStringLiteral("-VerifyAll");
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
    if (event == "report") {
      const auto value = object.value("report").toObject();
      if (value.value("attempt").toString() == attempt_) {
        report_ = value;
        // Preparation is acknowledged only after the helper exits normally.
        if (report_.value("state").toString() != "prepared") state_->report(report_);
      }
    }
    if (event == "phase" || event == "operation") {
      auto value = report_; value.insert("attempt", attempt_);
      if (event == "phase") {
        value.insert("phase", object.value("phase")); value.insert("resource", object.value("resource"));
        if (!object.value("component").toString().isEmpty()) value.insert("component", object.value("component"));
      } else {
        value.insert("wallSeconds", object.value("wallSeconds"));
        const auto operation = object.value("operation").toObject();
        if (operation.value("kind").toString() == "download") downloadedBytes_ += operation.value("bytes").toInteger();
        if (operation.value("kind").toString() == "decompile" || operation.value("kind").toString() == "extractCache") {
          ++extractions_; if (operation.value("reused").toBool()) ++reusedExtractions_;
        }
      }
      value.insert("downloadedBytes", downloadedBytes_); value.insert("extractions", extractions_);
      value.insert("reusedExtractions", reusedExtractions_); report_ = value;
      if (value.value("state").toString() != "prepared") state_->report(value);
    }
    if (event == QStringLiteral("progress")) {
      QString message = object.value(QStringLiteral("message")).toString();
      if (message == QStringLiteral("Preparing local data update tools (Python)")) message = QStringLiteral("首次准备本地数据更新工具");
      state_->progress(message);
    }
    if (event == QStringLiteral("component")) {
      const QString status = object.value(QStringLiteral("status")).toString();
      const QString component = object.value(QStringLiteral("component")).toString();
      if ((status == QStringLiteral("updated") || status == QStringLiteral("unchanged")) && !components_.contains(component)) components_.append(component);
      static const QHash<QString,QString> labels{{QStringLiteral("pets"),QStringLiteral("精灵与养成资料")},
          {QStringLiteral("skills"),QStringLiteral("精灵技能资料")},
          {QStringLiteral("shop"),QStringLiteral("兑换商店")},{QStringLiteral("images"),QStringLiteral("精灵图片")},
          {QStringLiteral("icons"),QStringLiteral("星神与属性图标")},{QStringLiteral("routines"),QStringLiteral("日常与活动")}};
      const QString error = object.value(QStringLiteral("error")).toString();
      state_->progress(labels.value(component,component) + QStringLiteral("：") +
          (!error.isEmpty() ? error : status == QStringLiteral("updated") ? QStringLiteral("已整理，等待校验和载入") :
           status == QStringLiteral("unchanged") ? QStringLiteral("来源无变化，等待载入确认") : QStringLiteral("未完成，保留已有数据")));
    }
    if (event == QStringLiteral("finished")) {
      hasResult_ = true;
      resultSuccess_ = object.value(QStringLiteral("success")).toBool();
      resultMessage_ = object.value(QStringLiteral("error")).toString(object.value(QStringLiteral("message")).toString());
      const auto values = object.value(QStringLiteral("components")).toObject();
      QStringList errors;
      for (auto it = values.begin(); it != values.end(); ++it) {
        const auto item = it.value().toObject();
        const auto error = item.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) errors.append(error);
      }
      if (!errors.isEmpty()) resultMessage_ = errors.join(QStringLiteral("；"));
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
    auto value = report_;
    value.insert("attempt", attempt_);
    value.insert("state", state_->cancelling.load() ? "cancelled" : "failed");
    value.insert("error", state_->cancelling.load() ? QStringLiteral("检查已取消，现用资料保留") : message);
    value.insert("canCancel", false);
    value.insert("diskCommitted", false); value.insert("sessionApplied", false);
    auto rows = value.value("components").toObject();
    for (const auto& key : requestedComponents_.isEmpty() ? DataUpdateService::componentNames() : requestedComponents_) {
      auto row = rows.value(key).toObject(); row.insert("status", state_->cancelling.load() ? "cancelled" : "failed");
      row.insert("retryable", true); row.insert("error", value.value("error")); row.insert("changes", QJsonObject{});
      row.insert("checkedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)); rows.insert(key, row);
    }
    value.insert("components", rows);
    try {
      value = fullReceipt(root_, value);
      PublicCatalogStore::save(PublicCatalogStore::safePath(root_, "catalog/update-tasks/" + attempt_ + "/result.json"), value);
      PublicCatalogStore::save(PublicCatalogStore::safePath(root_, "catalog/public-update-status.json"), value);
    } catch (const std::exception& error) { value.insert("receiptError", QString::fromUtf8(error.what())); }
    state_->report(value);
    state_->finish(success, components, message);
    deleteLater();
  }
  QString root_;
  QStringList requestedComponents_;
  QString attempt_;
  bool verifyAll_ = false;
  QJsonObject report_;
  int cancelStarted_ = 0, extractions_ = 0, reusedExtractions_ = 0;
  qint64 downloadedBytes_ = 0;
  std::shared_ptr<State> state_;
  QProcess process_;
  QTimer timer_;
  QByteArray tail_;
  QStringList components_;
  QString resultMessage_;
  int elapsedMs_ = 0;
  bool hasResult_ = false, resultSuccess_ = false, done_ = false;
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
  QString attempt;
  QJsonObject report;
};
DataUpdateService::DataUpdateService(StorageService* storage, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->storage = storage;
  impl_->state->receiver = this;
}
DataUpdateService::~DataUpdateService() { close(); }
bool DataUpdateService::busy() const { return impl_->busy; }
QStringList DataUpdateService::componentNames() {
  return {QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("shop"), QStringLiteral("images"),
          QStringLiteral("icons"), QStringLiteral("routines")};
}
bool DataUpdateService::requestUpdate(const QStringList& components, bool verifyAll) {
  if (impl_->busy || !impl_->storage || impl_->state->closing.load()) return false;
  // Only fixed names ever reach the script's command line.
  QStringList selected;
  for (const QString& name : componentNames())
    if (components.contains(name)) selected.append(name);
  if (selected.size() != components.size()) return false;
  if (selected.size() == componentNames().size()) selected.clear();
  auto state = impl_->state;
  const QString root = impl_->storage->dataRoot();
  const QString attempt = QUuid::createUuid().toString(QUuid::Id128);
  state->cancelling.store(false);
  if (!impl_->storage->postAuxiliary([root, selected, attempt, verifyAll, state](QObject* ioRoot) {
        new DataUpdateInternal::Job(root, selected, attempt, verifyAll, state, ioRoot);
      })) return false;
  impl_->busy = true;
  impl_->attempt = attempt;
  impl_->report.insert("attempt", attempt); impl_->report.insert("state", "accepted");
  impl_->report.insert("canCancel", true); impl_->report.remove("error");
  impl_->report.remove("recovered"); impl_->report.remove("component");
  impl_->report.insert("phase", "prepare"); impl_->report.insert("wallSeconds", 0);
  impl_->report.insert("downloadedBytes", 0); impl_->report.insert("extractions", 0); impl_->report.insert("reusedExtractions", 0);
  emit reportChanged(impl_->report);
  emit progress(QStringLiteral("已受理数据更新，正在准备检查"));
  return true;
}
void DataUpdateService::close() {
  impl_->state->closing.store(true);
  QMutexLocker guard(&impl_->state->mutex);
  impl_->state->receiver = nullptr;
}
void DataUpdateService::receiveProgress(const QString& message) { emit progress(message); }
void DataUpdateService::receiveFinished(bool success, const QStringList& components, const QString& message) {
  impl_->busy = false;
  emit finished(success, components, message);
}

void DataUpdateService::receiveReport(const QJsonObject& report) {
  if (impl_->busy && report.value("attempt").toString() != impl_->attempt) return;
  impl_->report = report;
  emit reportChanged(report);
  if (impl_->busy && report.value("state").toString() == "prepared") emit prepared(report);
}

void DataUpdateService::requestStatus() {
  if (impl_->busy) { emit reportChanged(impl_->report); return; }
  if (!impl_->storage) return;
  auto state = impl_->state; const QString root = impl_->storage->dataRoot();
  impl_->storage->postAuxiliary([state, root](QObject*) {
    try {
      const auto path = PublicCatalogStore::safePath(root, "catalog/public-update-status.json");
      if (!QFileInfo::exists(path)) { state->report({}); return; }
      auto value = PublicCatalogStore::object(path);
      value.insert("recovered", true);
      const auto pointer = QJsonDocument::fromJson(PublicCatalogStore::pointerBytes(root)).object();
      if (pointer.value("attempt") == value.value("attempt") && pointer.value("revision") == value.value("revision") &&
          !value.value("sessionApplied").toBool()) {
        value.insert("diskCommitted", true); value.insert("state", "diskCommitted");
      }
      const QString phase = value.value("state").toString();
      if (QStringList{"accepted", "preparing", "running", "applying"}.contains(phase)) {
        value.insert("state", "interrupted"); value.insert("error", "上次检查中断，现用资料保留，可重新检查"); value.insert("canCancel", false);
      }
      state->report(value);
    } catch (const std::exception& error) { state->report({{"state", "failed"}, {"error", QString::fromUtf8(error.what())}}); }
  });
}

bool DataUpdateService::requestAction(const QString& action) {
  if (action == "cancel") {
    if (!impl_->busy || impl_->report.value("state").toString() == "applying" || impl_->report.value("state").toString() == "prepared") return false;
    impl_->state->cancelling.store(true); return true;
  }
  if (action == "status") { requestStatus(); return true; }
  if (action == "prune") {
    if (impl_->busy || !impl_->storage) return false;
    const auto state = impl_->state; const QString root = impl_->storage->dataRoot();
    const bool accepted = impl_->storage->postAuxiliary([state, root](QObject*) {
      const auto result = DataUpdateInternal::prunePublicData(root);
      const bool ok = result.value("ok").toBool();
      state->finish(ok, {}, ok ? QStringLiteral("公共资料已整理，只保留当前版和上一版") : result.value("error").toString());
    });
    if (accepted) impl_->busy = true;
    return accepted;
  }
  if (action == "verify") return requestUpdate({}, true);
  if (action == "retry") {
    QStringList failed;
    const auto rows = impl_->report.value("components").toObject();
    for (auto it = rows.begin(); it != rows.end(); ++it) if (it.value().toObject().value("retryable").toBool()) failed.append(it.key());
    if (failed.isEmpty() && QStringList{"failed", "cancelled", "interrupted"}.contains(impl_->report.value("state").toString()))
      failed = impl_->report.value("requested").toVariant().toStringList();
    if (failed.isEmpty()) return false;
    return requestUpdate(failed);
  }
  if ((action != "reload" && action != "rollback") || impl_->busy || !impl_->storage) return false;
  const auto state = impl_->state; const QString root = impl_->storage->dataRoot();
  const QString attempt = QUuid::createUuid().toString(QUuid::Id128);
  const auto previous = impl_->report;
  const bool accepted = impl_->storage->postAuxiliary([state, root, attempt, action, previous](QObject*) {
    auto report = previous; report.insert("attempt", attempt); report.insert("canCancel", false);
    report.insert("sessionApplied", false); report.insert("action", action); report.remove("error");
    report.remove("recovered");
    try {
      const auto bytes = PublicCatalogStore::pointerBytes(root);
      const auto current = QJsonDocument::fromJson(bytes).object();
      if (action == "reload" && previous.value("state").toString() == "applyFailed" && !previous.value("diskCommitted").toBool())
        throw std::runtime_error("候选尚未提交为现用资料，请重新检查；原资料仍在使用");
      QString revision = action == "rollback" ? current.value("previousRevision").toString() : current.value("revision").toString();
      if (action == "reload" && previous.value("state").toString() == "prepared") {
        if (PublicCatalogStore::digest(bytes) != previous.value("expectedPointerSha256").toString() && current.value("revision") != previous.value("revision"))
          throw std::runtime_error("候选生成后现用资料已变化，请重新检查");
        revision = previous.value("revision").toString();
      }
      PublicCatalogStore::version(root, revision);
      report.insert("revision", revision); report.insert("expectedPointerSha256", PublicCatalogStore::digest(bytes));
      report.insert("previousRevision", action == "rollback" ? current.value("revision") : current.value("previousRevision"));
      report.insert("state", "prepared"); report.insert("complete", action == "rollback" || previous.value("complete").toBool());
      if (action == "rollback") {
        QJsonObject rows;
        for (const auto& key : DataUpdateService::componentNames()) rows.insert(key, QJsonObject{{"status", "restored"}});
        report.insert("components", rows);
      }
      state->report(report);
    } catch (const std::exception& error) {
      report.insert("state", "applyFailed"); report.insert("error", QString::fromUtf8(error.what())); state->report(report);
      state->finish(false, {}, QString::fromUtf8(error.what()));
    }
  });
  if (accepted) { impl_->busy = true; impl_->attempt = attempt; emit progress(QStringLiteral("正在校验并载入已保存资料")); }
  return accepted;
}

void DataUpdateService::applicationFinished(const QJsonObject& incoming) {
  if (incoming.value("attempt").toString() != impl_->attempt || !impl_->storage) return;
  const auto state = impl_->state; const auto root = impl_->storage->dataRoot();
  const auto persist = [state, root, incoming](QObject*) {
    auto report = incoming;
    try {
      report = DataUpdateInternal::fullReceipt(root, report);
      PublicCatalogStore::save(PublicCatalogStore::safePath(root, "catalog/update-tasks/" + report.value("attempt").toString() + "/result.json"), report);
      PublicCatalogStore::save(PublicCatalogStore::safePath(root, "catalog/public-update-status.json"), report);
    } catch (const std::exception& error) { report.insert("receiptError", QString::fromUtf8(error.what())); }
    const bool applied = report.value("sessionApplied").toBool();
    if (applied && !report.contains("receiptError")) {
      report.insert("retention", DataUpdateInternal::prunePublicData(root));
      // Cleanup has its own public-retention.json receipt. Do not overwrite a
      // newer updater's status after releasing the publication lock.
    }
    const QString action = report.value("action").toString();
    QString message = !applied ? QStringLiteral("资料载入未完成：") + report.value("error").toString()
        : action == "rollback" ? QStringLiteral("已恢复上一版公共资料；账号资料与商品选择保留")
        : action == "reload" ? QStringLiteral("已重新载入已保存资料")
        : report.value("complete").toBool() ? QStringLiteral("检查完成，工作台已载入资料") : QStringLiteral("可用资料已载入；仍有未完成项目，请查看各类结果");
    const auto rows = report.value("components").toObject();
    bool hasNotices = false;
    for (auto row = rows.begin(); row != rows.end(); ++row)
      hasNotices |= !row.value().toObject().value("notices").toArray().isEmpty();
    if (applied && hasNotices) message += QStringLiteral("；另有资料提示，具体活动和商品见变化与诊断");
    if (applied && report.value("retention").isObject() && !report.value("retention").toObject().value("ok").toBool())
      message += QStringLiteral("；历史资料清理未完成，下次更新后重试");
    state->report(report); state->finish(applied, {}, message);
  };
  if (!impl_->storage->postAuxiliary(persist)) {
    impl_->busy = false;
    auto report = incoming; report.insert("receiptError", "结果保存队列不可用"); receiveReport(report);
    emit finished(false, {}, QStringLiteral("载入结果未能保存，请查看诊断"));
  }
}
