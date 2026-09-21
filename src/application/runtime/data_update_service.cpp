#include "data_update_service.h"
#include "storage/storage_service.h"

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
  Job(QString root, QStringList components, std::shared_ptr<State> state, QObject* parent)
      : QObject(parent), root_(std::move(root)), requestedComponents_(std::move(components)),
        state_(std::move(state)), process_(this), timer_(this) {
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
      const QString message = !resultMessage_.isEmpty() ? resultMessage_
          : success ? QStringLiteral("数据检查完成；已有数据继续从本地读取")
          : QStringLiteral("公共数据检查未完成，原有数据已保留");
      finish(success, components_, message);
    });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] {
      elapsedMs_ += 250;
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
    if (state_->closing.load()) { finish(false, {}, QStringLiteral("数据更新已停止")); return; }
    const QString scripts = QDir(root_).filePath(QStringLiteral("data-tools/scripts"));
    if (!QDir().mkpath(QDir(scripts).filePath(QStringLiteral("baseline")))) {
      finish(false, {}, QStringLiteral("无法创建本地更新工具目录")); return;
    }
    const QStringList names{QStringLiteral("public_data_updater.py"), QStringLiteral("bootstrap-public-data.ps1"),
        QStringLiteral("public_names_updater.py"),QStringLiteral("public_icon_updater.py"),QStringLiteral("public_routine_updater.py"),
        QStringLiteral("public_activity_exchange_updater.py"),QStringLiteral("activity_evolution_selector.py"),
        QStringLiteral("generate_pet_detail_data.py"), QStringLiteral("generate_pet_skill_data.py"),
        QStringLiteral("public_skill_updater.py"), QStringLiteral("generate_shop_exchange_data.py"),
        QStringLiteral("generate_stargod_icons.py")};
    for (const QString& name : names) {
      if (!copyResource(QStringLiteral(":/kqpet/data-updater/") + name, QDir(scripts).filePath(name))) {
        finish(false, {}, QStringLiteral("无法准备内置更新工具：") + name); return;
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
        QStringLiteral("-DataRoot"), root_};
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
          {QStringLiteral("shop"),QStringLiteral("指定精灵兑换")},{QStringLiteral("images"),QStringLiteral("精灵图片")},
          {QStringLiteral("icons"),QStringLiteral("星神与属性图标")},{QStringLiteral("routines"),QStringLiteral("日常与活动")}};
      const QString error = object.value(QStringLiteral("error")).toString();
      state_->progress(labels.value(component,component) + QStringLiteral("：") +
          (!error.isEmpty() ? error : status == QStringLiteral("updated") ? QStringLiteral("已更新") :
           status == QStringLiteral("unchanged") ? QStringLiteral("已是最新") : QStringLiteral("未完成，保留已有数据")));
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
    state_->finish(success, components, message);
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
bool DataUpdateService::requestUpdate(const QStringList& components) {
  if (impl_->busy || !impl_->storage || impl_->state->closing.load()) return false;
  // Only fixed names ever reach the script's command line.
  QStringList selected;
  for (const QString& name : componentNames())
    if (components.contains(name)) selected.append(name);
  if (selected.size() != components.size()) return false;
  if (selected.size() == componentNames().size()) selected.clear();
  auto state = impl_->state;
  const QString root = impl_->storage->dataRoot();
  if (!impl_->storage->postAuxiliary([root, selected, state](QObject* ioRoot) {
        new DataUpdateInternal::Job(root, selected, state, ioRoot);
      })) return false;
  impl_->busy = true;
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
