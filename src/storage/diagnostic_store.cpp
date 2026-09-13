#include "diagnostic_store.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <QVector>
#include <algorithm>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace {
constexpr qint64 IndexAllowance = 1024;
class FileLease final {
public:
  explicit FileLease(const QString& path) {
    handle_ = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
  }
  ~FileLease() { if (valid()) CloseHandle(handle_); }
  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool safePath(QString path) {
  if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)) return false;
  path = QDir::cleanPath(path);
  while (!path.isEmpty()) {
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    const QString parent = QFileInfo(path).absolutePath();
    if (parent == path) break;
    path = parent;
  }
  return true;
}

bool validRun(const QString& run) {
  if (!run.startsWith(QStringLiteral("run-")) || run.size() < 8 || run.size() > 96) return false;
  for (QChar ch : run)
    if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
          (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) || ch == QLatin1Char('-'))) return false;
  return true;
}
bool volumeName(const QString& name) {
  if (!name.startsWith(QStringLiteral("volume-")) || !name.endsWith(QStringLiteral(".jsonl"))) return false;
  const QString digits = name.mid(7, name.size() - 13);
  if (digits.isEmpty() || digits.size() > 10) return false;
  for (QChar ch : digits) if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
  return true;
}
bool atomicSave(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { file.cancelWriting(); return false; }
  return file.commit();
}
struct RunFiles { QString directory; QStringList files; qint64 bytes = 0; QDateTime modified; bool recognized = true; };
struct Inventory { qint64 bytes = 0; QVector<RunFiles> runs; QString error; };
Inventory inspect(const QString& root, int maximum) {
  Inventory result;
  int entries = 0;
  QDirIterator roots(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  while (roots.hasNext()) {
    roots.next();
    if (++entries > maximum) { result.error = QStringLiteral("log_scan_capacity"); return result; }
    const QFileInfo info = roots.fileInfo();
    if (!safePath(info.absoluteFilePath())) { result.error = QStringLiteral("log_reparse_path"); return result; }
    if (!info.isDir()) { result.bytes += info.size(); continue; }
    if (!validRun(info.fileName())) { result.error = QStringLiteral("log_unknown_directory"); return result; }
    RunFiles run{info.absoluteFilePath(), {}, 0, info.lastModified(), true};
    QDirIterator files(run.directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (files.hasNext()) {
      files.next();
      if (++entries > maximum) { result.error = QStringLiteral("log_scan_capacity"); return result; }
      const QFileInfo entry = files.fileInfo();
      if (!safePath(entry.absoluteFilePath()) || entry.isDir()) {
        result.error = QStringLiteral("log_unknown_run_layout"); return result;
      }
      run.recognized = run.recognized && (volumeName(entry.fileName()) || entry.fileName() == QStringLiteral(".active.lock"));
      run.files.append(entry.absoluteFilePath());
      run.bytes += entry.size();
      run.modified = std::max(run.modified, entry.lastModified());
    }
    result.bytes += run.bytes;
    result.runs.append(std::move(run));
  }
  std::sort(result.runs.begin(), result.runs.end(), [](const RunFiles& a, const RunFiles& b) {
    return a.modified == b.modified ? a.directory < b.directory : a.modified < b.modified;
  });
  return result;
}
}

struct DiagnosticStore::Impl {
  QString root, run, directory, path;
  DiagnosticLimits limits;
  std::unique_ptr<FileLease> lease;
  QByteArray salt;
  quint64 sequence = 0;
  QThread* owner = QThread::currentThread();
  bool closed = false;

  bool saveIndex() const {
    const QByteArray index = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("runId"), run}, {QStringLiteral("relativeDirectory"), run},
        {QStringLiteral("relativeFile"), path.isEmpty() ? QString() : run + QLatin1Char('/') + QFileInfo(path).fileName()},
        {QStringLiteral("observedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}).toJson(QJsonDocument::Compact);
    return index.size() < IndexAllowance && atomicSave(QDir(root).filePath(QStringLiteral("latest.json")), index);
  }

  DiagnosticStoreResult ensureSpace(qint64 added) {
    Inventory inventory = inspect(root, limits.scanEntries);
    if (!inventory.error.isEmpty()) return {false, inventory.error};
    for (const auto& old : inventory.runs) {
      if (inventory.bytes <= limits.directoryBytes - added) break;
      if (old.directory == directory || !old.recognized) continue;
      // Global directory lease is held by the caller. A live run cannot be
      // removed: it owns this same exclusive handle for its entire lifetime.
      bool removed = true;
      {
        FileLease candidate(QDir(old.directory).filePath(QStringLiteral(".active.lock")));
        if (!candidate.valid()) continue;
        for (const QString& file : old.files) {
          if (QFileInfo(file).fileName() == QStringLiteral(".active.lock")) continue;
          if (!QFile::remove(file)) { removed = false; break; }
        }
      }
      if (removed) {
        const QString lock = QDir(old.directory).filePath(QStringLiteral(".active.lock"));
        removed = QFile::remove(lock) && QDir().rmdir(old.directory);
      }
      // Recount partial removal instead of assuming the complete run vanished.
      if (!removed) {
        inventory = inspect(root, limits.scanEntries);
        if (!inventory.error.isEmpty()) return {false, inventory.error};
        return inventory.bytes <= limits.directoryBytes - added
            ? DiagnosticStoreResult{true, {}} : DiagnosticStoreResult{false, QStringLiteral("log_cleanup_failed")};
      }
      inventory.bytes -= old.bytes;
    }
    return inventory.bytes <= limits.directoryBytes - added
        ? DiagnosticStoreResult{true, {}} : DiagnosticStoreResult{false, QStringLiteral("log_directory_capacity")};
  }
};

DiagnosticLimits DiagnosticStore::boundedLimits(DiagnosticLimits value) {
  const DiagnosticLimits maximum;
  value.fileBytes = std::clamp<qint64>(value.fileBytes, 256, maximum.fileBytes);
  value.runVolumes = std::clamp(value.runVolumes, 1, maximum.runVolumes);
  value.directoryBytes = std::clamp<qint64>(value.directoryBytes, 2048, maximum.directoryBytes);
  value.scanEntries = std::clamp(value.scanEntries, 8, maximum.scanEntries);
  value.pendingEvents = std::clamp(value.pendingEvents, 1, maximum.pendingEvents);
  value.pendingBytes = std::clamp<qint64>(value.pendingBytes, 512, maximum.pendingBytes);
  value.recentEvents = std::clamp(value.recentEvents, 1, maximum.recentEvents);
  value.recentBytes = std::clamp<qint64>(value.recentBytes, 512, maximum.recentBytes);
  value.batchBytes = std::clamp<qint64>(value.batchBytes, 256, std::min(maximum.batchBytes, value.fileBytes));
  value.flushMilliseconds = std::clamp(value.flushMilliseconds, 10, 1000);
  return value;
}

DiagnosticStore::DiagnosticStore(QString dataRoot, QString runId, DiagnosticLimits limits)
    : impl_(std::make_unique<Impl>()) {
  impl_->root = QDir(QDir::cleanPath(dataRoot)).filePath(QStringLiteral("logs"));
  impl_->run = std::move(runId);
  impl_->limits = boundedLimits(limits);
  impl_->directory = QDir(impl_->root).filePath(impl_->run);
}
DiagnosticStore::~DiagnosticStore() = default;

DiagnosticStoreResult DiagnosticStore::open() {
  auto& state = *impl_;
  if (QThread::currentThread() != state.owner || state.closed || !validRun(state.run) || !safePath(state.root))
    return {false, QStringLiteral("log_invalid_context")};
  if (state.lease) return {true, {}};
  if (!QDir().mkpath(state.root) || !safePath(state.root)) return {false, QStringLiteral("log_directory_failed")};
  FileLease directoryLock(QDir(state.root).filePath(QStringLiteral(".directory.lock")));
  if (!directoryLock.valid()) return {false, QStringLiteral("log_directory_busy")};
  // Never reopen somebody else's run, even if that process already exited.
  if (QFileInfo::exists(state.directory)) return {false, QStringLiteral("log_run_collision")};
  auto room = state.ensureSpace(IndexAllowance + 32);
  if (!room.saved) return room;
  if (!QDir().mkdir(state.directory)) return {false, QStringLiteral("log_run_create_failed")};
  auto lease = std::make_unique<FileLease>(QDir(state.directory).filePath(QStringLiteral(".active.lock")));
  if (!lease->valid()) return {false, QStringLiteral("log_run_lock_failed")};
  const QString saltPath = QDir(state.root).filePath(QStringLiteral(".installation-salt"));
  if (!safePath(saltPath)) return {false, QStringLiteral("log_salt_path_invalid")};
  if (QFileInfo::exists(saltPath)) {
    QFile file(saltPath);
    if (file.size() == 32 && file.open(QIODevice::ReadOnly)) state.salt = file.read(33);
  } else {
    QByteArray generated(32, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(generated.data()), static_cast<ULONG>(generated.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 && atomicSave(saltPath, generated)) state.salt = generated;
  }
  if (state.salt.size() != 32) state.salt.clear();
  if (!state.saveIndex())
    return {false, QStringLiteral("log_index_failed")};
  state.lease = std::move(lease);
  return {true, state.salt.isEmpty() ? QStringLiteral("log_salt_unavailable") : QString()};
}

DiagnosticStoreResult DiagnosticStore::append(const QByteArray& lines) {
  auto& state = *impl_;
  if (QThread::currentThread() != state.owner || !state.lease || state.closed)
    return {false, QStringLiteral("log_not_open")};
  if (lines.isEmpty()) return {true, {}};
  if (lines.size() > state.limits.fileBytes) return {false, QStringLiteral("log_batch_capacity")};
  if (!safePath(state.directory)) return {false, QStringLiteral("log_reparse_path")};
  FileLease directoryLock(QDir(state.root).filePath(QStringLiteral(".directory.lock")));
  if (!directoryLock.valid()) return {false, QStringLiteral("log_directory_busy")};
  if (state.path.isEmpty() || QFileInfo(state.path).size() > state.limits.fileBytes - lines.size()) {
    state.path = QDir(state.directory).filePath(QStringLiteral("volume-%1.jsonl").arg(++state.sequence, 10, 10, QLatin1Char('0')));
    const QStringList old = QDir(state.directory).entryList({QStringLiteral("volume-*.jsonl")}, QDir::Files, QDir::Name);
    for (int i = 0; i <= old.size() - state.limits.runVolumes; ++i)
      if (!QFile::remove(QDir(state.directory).filePath(old[i]))) return {false, QStringLiteral("log_rotation_failed")};
  }
  // Reserve the complete replacement index, including its temporary QSaveFile
  // copy. A successful append also repairs an index for a reclaimed old run.
  auto room = state.ensureSpace(lines.size() + IndexAllowance);
  if (!room.saved) return room;
  QFile file(state.path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return {false, QStringLiteral("log_open_failed")};
  const qint64 previous = file.size();
  if (file.write(lines) != lines.size() || !file.flush()) {
    // Best effort removes a partial JSON line; failure remains visible either way.
    file.resize(previous);
    return {false, QStringLiteral("log_write_failed")};
  }
  return {true, state.saveIndex() ? QString() : QStringLiteral("log_index_failed")};
}
void DiagnosticStore::close() { impl_->closed = true; impl_->lease.reset(); }
QByteArray DiagnosticStore::installationSalt() const { return impl_->salt; }
QString DiagnosticStore::currentPath() const { return impl_->path; }
QString DiagnosticStore::runId() const { return impl_->run; }
