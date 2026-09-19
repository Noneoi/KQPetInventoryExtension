#include "cache_management_service.h"
#include "storage/storage_service.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <atomic>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace CacheManagementInternal {
struct State {
  QMutex mutex;
  CacheManagementService* receiver = nullptr;
  std::atomic_bool closing{false};
  void progress(QString message) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, message = std::move(message)] { target->receiveProgress(message); }, Qt::QueuedConnection);
  }
  void finish(QString action, QJsonObject result) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    auto target = receiver;
    QMetaObject::invokeMethod(target, [target, action = std::move(action), result = std::move(result)] {
      target->receiveFinished(action, result);
    }, Qt::QueuedConnection);
  }
};

namespace {
constexpr qsizetype kMaximumRequestBytes = 4 * 1024 * 1024;
constexpr qsizetype kMaximumResultBytes = 8 * 1024 * 1024;

bool writeAtomic(const QString& path, const QByteArray& bytes) {
  QSaveFile output(path);
  return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
QJsonObject failure(const QString& message) {
  return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), message}};
}
QString startingMessage(const QString& action) {
  if (action == QStringLiteral("inspect")) return QStringLiteral("正在统计本地缓存");
  if (action == QStringLiteral("backup")) return QStringLiteral("正在备份本地缓存");
  if (action == QStringLiteral("restore")) return QStringLiteral("正在恢复本地缓存");
  if (action == QStringLiteral("schedule-root")) return QStringLiteral("正在保存缓存目录设置");
  if (action == QStringLiteral("cleanup-legacy")) return QStringLiteral("正在整理旧版本重复缓存");
  return QStringLiteral("正在处理所选缓存");
}

class Job final : public QObject {
public:
  Job(QString action, QJsonObject request, std::shared_ptr<State> state, QObject* parent)
      : QObject(parent), action_(std::move(action)), request_(std::move(request)), state_(std::move(state)),
        process_(this), timer_(this) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process_.setProcessEnvironment(environment);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) finish(failure(QStringLiteral("无法启动缓存管理工具：") + process_.errorString()));
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus status) {
      readOutput();
      if (overflow_) { finish(failure(QStringLiteral("缓存管理结果过大，未能读取结果"))); return; }
      if (!writeAtomic(resultPath_, output_)) { finish(failure(QStringLiteral("无法保存缓存管理结果"))); return; }
      // The helper writes one compact JSON result. Accept a trailing JSON line
      // as well, so a PowerShell host notice cannot hide the actual outcome.
      QJsonObject result = QJsonDocument::fromJson(output_.trimmed()).object();
      if (!result.contains(QStringLiteral("ok"))) {
        const auto lines = output_.split('\n');
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
          auto candidate = QJsonDocument::fromJson(it->trimmed()).object();
          if (candidate.contains(QStringLiteral("ok"))) { result = std::move(candidate); break; }
        }
      }
      if (!result.contains(QStringLiteral("ok"))) result = failure(QStringLiteral("缓存管理未返回有效结果"));
      if (exitCode != 0 || status != QProcess::NormalExit) {
        result.insert(QStringLiteral("ok"), false);
        if (result.value(QStringLiteral("message")).toString().isEmpty()) result.insert(QStringLiteral("message"), QStringLiteral("缓存管理未完成"));
      }
      finish(std::move(result));
    });
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] {
      if (state_->closing.load()) process_.kill();
    });
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
      arguments->flags |= CREATE_NO_WINDOW;
    });
    connect(&process_, &QProcess::started, this, [this] {
      job_ = CreateJobObjectW(nullptr, nullptr);
      if (!job_) return;
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
      HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process_.processId()));
      if (child) { AssignProcessToJobObject(job_, child); CloseHandle(child); }
    });
#endif
    QTimer::singleShot(0, this, [this] { start(); });
  }
  ~Job() override {
#ifdef Q_OS_WIN
    if (job_) CloseHandle(job_);
#endif
    cleanup();
  }
private:
  void start() {
    if (state_->closing.load()) { finish(failure(QStringLiteral("缓存管理已停止"))); return; }
    const QString root = request_.value(QStringLiteral("root")).toString();
    const QString tools = QDir(root).filePath(QStringLiteral("data-tools"));
    const QString token = QUuid::createUuid().toString(QUuid::Id128);
    temporaryDirectory_ = QDir(root).filePath(QStringLiteral(".maintenance/cache-") + token);
    if (!QDir().mkpath(tools) || !QDir().mkpath(temporaryDirectory_)) {
      finish(failure(QStringLiteral("无法创建缓存管理工作目录"))); return;
    }
    scriptPath_ = QDir(tools).filePath(QStringLiteral("cache-manager.ps1"));
    requestPath_ = QDir(temporaryDirectory_).filePath(QStringLiteral("request.json"));
    resultPath_ = QDir(temporaryDirectory_).filePath(QStringLiteral("result.json"));
    QFile script(QStringLiteral(":/kqpet/cache-manager.ps1"));
    if (!script.open(QIODevice::ReadOnly) || script.size() > 2 * 1024 * 1024) {
      finish(failure(QStringLiteral("缺少内置缓存管理工具"))); return;
    }
    QByteArray scriptBytes = script.readAll();
    // Windows PowerShell 5 reads a BOM-less script using the system ANSI code
    // page. Keep the trusted Chinese text intact without changing host policy.
    if (!scriptBytes.startsWith("\xef\xbb\xbf")) scriptBytes.prepend("\xef\xbb\xbf");
    const auto requestBytes = QJsonDocument(request_).toJson(QJsonDocument::Compact);
    if (requestBytes.size() > kMaximumRequestBytes || !writeAtomic(scriptPath_, scriptBytes) || !writeAtomic(requestPath_, requestBytes)) {
      finish(failure(QStringLiteral("无法准备缓存管理请求"))); return;
    }
#ifdef Q_OS_WIN
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) { finish(failure(QStringLiteral("无法定位系统脚本运行器"))); return; }
    process_.setProgram(QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe")));
#else
    process_.setProgram(QStringLiteral("pwsh"));
#endif
    process_.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath_, QStringLiteral("-RequestFile"), requestPath_});
    process_.setWorkingDirectory(tools);
    state_->progress(startingMessage(action_));
    timer_.start();
    process_.start();
  }
  void readOutput() {
    const QByteArray bytes = process_.readAllStandardOutput();
    if (output_.size() + bytes.size() > kMaximumResultBytes) {
      overflow_ = true;
      process_.kill();
      return;
    }
    output_.append(bytes);
  }
  void cleanup() {
    // Delete only the two filenames created by this job, then its empty UUID
    // directory. Never recursively remove a user-supplied path.
    if (!requestPath_.isEmpty()) QFile::remove(requestPath_);
    if (!resultPath_.isEmpty()) QFile::remove(resultPath_);
    if (!temporaryDirectory_.isEmpty()) QDir().rmdir(temporaryDirectory_);
  }
  void finish(QJsonObject result) {
    if (done_) return;
    done_ = true;
    timer_.stop();
    result.insert(QStringLiteral("mode"), action_);
    cleanup();
    state_->finish(action_, std::move(result));
    deleteLater();
  }
  QString action_;
  QJsonObject request_;
  std::shared_ptr<State> state_;
  QProcess process_;
  QTimer timer_;
  QString temporaryDirectory_, requestPath_, resultPath_, scriptPath_;
  QByteArray output_;
  bool overflow_ = false, done_ = false;
#ifdef Q_OS_WIN
  HANDLE job_ = nullptr;
#endif
};
} // namespace
} // namespace CacheManagementInternal

struct CacheManagementService::Impl {
  QPointer<StorageService> storage;
  QString clientRoot;
  std::shared_ptr<CacheManagementInternal::State> state = std::make_shared<CacheManagementInternal::State>();
  bool busy = false;
};
CacheManagementService::CacheManagementService(StorageService* storage, QString clientRoot, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->storage = storage;
  impl_->clientRoot = std::move(clientRoot);
  impl_->state->receiver = this;
}
CacheManagementService::~CacheManagementService() { close(); }
bool CacheManagementService::busy() const { return impl_->busy; }
bool CacheManagementService::request(QString action, QJsonObject options) {
  static const QSet<QString> allowed{QStringLiteral("inspect"), QStringLiteral("clear-account"), QStringLiteral("clear-detail"),
      QStringLiteral("clear-image"), QStringLiteral("clear-images"), QStringLiteral("clear-all"), QStringLiteral("cleanup-legacy"),
      QStringLiteral("backup"), QStringLiteral("restore"), QStringLiteral("schedule-root")};
  if (!allowed.contains(action) || impl_->busy || !impl_->storage || impl_->state->closing.load()) return false;
  options.insert(QStringLiteral("mode"), action);
  options.insert(QStringLiteral("root"), impl_->storage->dataRoot());
  options.insert(QStringLiteral("clientRoot"), impl_->clientRoot);
  auto state = impl_->state;
  if (!impl_->storage->postAuxiliary([action, options = std::move(options), state](QObject* ioRoot) {
    new CacheManagementInternal::Job(action, options, state, ioRoot);
  })) return false;
  impl_->busy = true;
  return true;
}
void CacheManagementService::close() {
  impl_->state->closing.store(true);
  QMutexLocker guard(&impl_->state->mutex);
  impl_->state->receiver = nullptr;
}
void CacheManagementService::receiveFinished(QString action, QJsonObject result) {
  impl_->busy = false;
  emit finished(std::move(action), std::move(result));
}
void CacheManagementService::receiveProgress(QString message) { emit progress(std::move(message)); }
