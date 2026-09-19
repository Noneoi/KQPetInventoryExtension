#include "diagnostics/diagnostic_logger.h"
#include "storage/diagnostic_store.h"
#include "storage/storage_service.h"
#include "diagnostics/target_compatibility_guard.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <windows.h>

namespace {
bool check(bool ok, const char* why) { if (!ok) std::fprintf(stderr, "FAIL: %s\n", why); return ok; }
bool until(const std::function<bool()>& predicate, int ms = 3000) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1); }
  return predicate();
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
qint64 total(const QString& path) {
  qint64 bytes = 0;
  QDirIterator iterator(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
  while (iterator.hasNext()) { iterator.next(); bytes += iterator.fileInfo().size(); }
  return bytes;
}
bool probe(const QString& path) {
  QProcess child; child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--probe"), path});
  return child.waitForFinished(3000) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
}
bool rotation(const QString& root) {
  DiagnosticLimits limits; limits.fileBytes = 1024; limits.directoryBytes = 64 * 1024;
  DiagnosticStore store(root, QStringLiteral("run-rotation-one"), limits);
  bool ok = check(store.open().saved, "open rotation store");
  for (int i = 0; i < 12; ++i) ok &= check(store.append(QByteArray(899, 'x') + '\n').saved, "append rotating volume");
  const QString directory = QDir(root).filePath(QStringLiteral("logs/run-rotation-one"));
  const auto files = QDir(directory).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files);
  ok &= check(files.size() == 5, "retain exactly five closed/current volumes");
  for (const auto& file : files) ok &= check(file.size() <= 1024, "single volume hard byte cap");
  const QString lock = QDir(directory).filePath(QStringLiteral(".active.lock"));
  ok &= check(!probe(lock), "another process cannot own live run lock");
  DiagnosticStore collision(root, QStringLiteral("run-rotation-one"), limits);
  ok &= check(!collision.open().saved, "run identity never reopens an existing run");
  const auto index = QJsonDocument::fromJson(read(QDir(root).filePath(QStringLiteral("logs/latest.json")))).object();
  ok &= check(index.value(QStringLiteral("runId")).toString() == store.runId(), "latest is a run index");
  ok &= check(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("logs/latest.log"))), "no shared latest log writer");
  store.close();
  ok &= check(probe(lock), "run lock released on close");
  return ok;
}
bool directoryBudget(const QString& root) {
  DiagnosticLimits limits; limits.fileBytes = 1024; limits.directoryBytes = 5500;
  DiagnosticStore first(root, QStringLiteral("run-active-first"), limits), second(root, QStringLiteral("run-active-second"), limits);
  bool ok = check(first.open().saved, "open first run");
  for (int i = 0; i < 3; ++i) ok &= check(first.append(QByteArray(899, 'a') + '\n').saved, "fill first run");
  ok &= check(second.open().saved, "open concurrent run");
  ok &= check(first.installationSalt().size() == 32 && first.installationSalt() == second.installationSalt(), "installation salt stable between independent runs");
  const QByteArray kept = read(first.currentPath());
  bool full = false;
  for (int i = 0; i < 8; ++i) if (!second.append(QByteArray(899, 'b') + '\n').saved) { full = true; break; }
  ok &= check(full && total(QDir(root).filePath(QStringLiteral("logs"))) <= limits.directoryBytes, "active runs cannot exceed total directory budget");
  ok &= check(read(first.currentPath()) == kept, "capacity refusal preserves other active log");
  first.close();
  ok &= check(second.append(QByteArray(899, 'c') + '\n').saved, "ended run reclaimed to admit bounded write");
  ok &= check(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("logs/run-active-first"))), "only ended run removed");
  ok &= check(QFileInfo::exists(second.currentPath()), "current file retained through cleanup");
  return ok;
}
bool closeRejected() {
  QTemporaryDir root;
  QObject owner;
  StorageService storage(root.path());
  auto gate = std::make_shared<QSemaphore>();
  bool ok = check(storage.postAuxiliary([gate](QObject*) { gate->acquire(); }) &&
      storage.postAuxiliary([](QObject*) {}), "occupy final flush admission slots");
  ok &= check(DiagnosticLogger::attachStorage(&storage, &owner), "attach while IO admission full");
  DiagnosticLogger::info(QStringLiteral("shutdown"), QStringLiteral("cannot promise this event is saved"));
  ok &= check(!DiagnosticLogger::closeStorage(&storage), "full queue rejects close without false success");
  const auto status = DiagnosticLogger::status();
  ok &= check(status.closing && status.pendingEvents > 0 && status.lastCode == QStringLiteral("log_close_not_queued"),
      "unwritten closing events retained with explicit failure status");
  gate->release();
  ok &= check(storage.shutdown(2000), "shutdown after rejected log admission");
  return ok;
}
bool pathAndScan(const QString& root) {
  DiagnosticLimits limits; limits.scanEntries = 8;
  QDir().mkpath(QDir(root).filePath(QStringLiteral("logs")));
  for (int i = 0; i < 10; ++i) {
    QFile file(QDir(root).filePath(QStringLiteral("logs/unrecognized-%1").arg(i)));
    file.open(QIODevice::WriteOnly); file.write("keep");
  }
  DiagnosticStore store(root, QStringLiteral("run-scan-refusal"), limits);
  const auto rejected = store.open();
  bool ok = check(!rejected.saved && rejected.code == QStringLiteral("log_scan_capacity"), "bounded scan refuses excess layout");
  ok &= check(read(QDir(root).filePath(QStringLiteral("logs/unrecognized-0"))) == QByteArrayLiteral("keep"), "unknown files preserved on refusal");
  DiagnosticStore traversal(root, QStringLiteral("run-../../escape"), limits);
  ok &= check(!traversal.open().saved, "run identity cannot escape log scope");
  return ok;
}
bool logger(const QString& root) {
  bool ok = true;
  ok &= check(DiagnosticLogger::maskedAccount(QStringLiteral("12345678")) == QStringLiteral("account-correlation-unavailable"), "no unsalted account fallback");
  auto gate = std::make_shared<QSemaphore>();
  auto entered = std::make_shared<std::atomic<bool>>(false);
  QObject owner;
  StorageService storage(root);
  ok &= check(storage.postAuxiliary([gate, entered](QObject*) { entered->store(true); gate->acquire(); }), "IO blocker admitted");
  ok &= check(storage.postAuxiliary([](QObject*) {}), "second bounded auxiliary slot occupied");
  DiagnosticLimits limits; limits.pendingEvents = 32; limits.pendingBytes = 16 * 1024; limits.flushMilliseconds = 10;
  limits.fileBytes = 4096; limits.directoryBytes = 32 * 1024;
  ok &= check(DiagnosticLogger::attachStorage(&storage, &owner, limits), "Core pump attached without disk wait");
  ok &= check(until([&] { return entered->load(); }), "known IO callback executing");
  for (int i = 0; i < 2000; ++i) DiagnosticLogger::info(QStringLiteral("capacity"), QStringLiteral("bounded event"));
  auto status = DiagnosticLogger::status();
  ok &= check(status.pendingEvents <= 32 && status.pendingBytes <= 16 * 1024 && status.dropped > 0 && status.admissionFailures > 0,
      "all queued/in-flight events charged while shared IO queue is full");
  gate->release();
  ok &= check(until([] { const auto s = DiagnosticLogger::status(); return s.pendingEvents == 0 && s.saltAvailable; }), "bounded queue drains on IO");
  const auto digest = DiagnosticLogger::maskedAccount(QStringLiteral("12345678"));
  ok &= check(digest.startsWith(QStringLiteral("account-")) && !digest.contains(QStringLiteral("12345678")) &&
      digest == DiagnosticLogger::maskedAccount(QStringLiteral("12345678")) && digest != DiagnosticLogger::maskedAccount(QStringLiteral("87654321")), "salted stable account correlation");
  DiagnosticLogger::event({QStringLiteral("write_failed"), QStringLiteral("storage"), 987,
      QStringLiteral("commit"), QStringLiteral("account=12345678 token=topsecret C:\\Users\\Alice\\file.json"), QStringLiteral("retry manually")}, true);
  DiagnosticLogger::info(QStringLiteral("privacy"), QStringLiteral("raw={\"secret\":\"do-not-keep\"}"));
  DiagnosticLogger::info(QStringLiteral("privacy"), QStringLiteral("account=%1 action=check").arg(digest));
  ok &= check(until([] { return DiagnosticLogger::status().pendingEvents == 0; }), "privacy events persisted");
  const auto exported = DiagnosticLogger::diagnosticText();
  ok &= check(exported.contains(digest) && exported.contains(QStringLiteral("KQPet Compatibility Report")),
      "salted correlation and compatibility report remain useful after redaction");
  ok &= check(!exported.contains(QStringLiteral("12345678")) && !exported.contains(QStringLiteral("topsecret")) &&
      !exported.contains(QStringLiteral("Alice")) && !exported.contains(QStringLiteral("do-not-keep")), "memory export removes accounts, secrets, paths and raw payloads");
  QByteArray all;
  QDirIterator files(QDir(root).filePath(QStringLiteral("logs")), {QStringLiteral("*.jsonl")}, QDir::Files, QDirIterator::Subdirectories);
  bool structure = false;
  while (files.hasNext()) { all += read(files.next()); }
  for (const auto& line : all.split('\n')) {
    if (line.isEmpty()) continue;
    const auto obj = QJsonDocument::fromJson(line).object();
    ok &= check(obj.contains(QStringLiteral("code")) && obj.contains(QStringLiteral("module")) && obj.contains(QStringLiteral("taskId")) &&
        obj.contains(QStringLiteral("stage")) && obj.contains(QStringLiteral("observedAt")) && obj.contains(QStringLiteral("suggestedAction")), "each log line has structured diagnostic identity");
    if (obj.value(QStringLiteral("code")) == QStringLiteral("write_failed")) structure = obj.value(QStringLiteral("taskId")) == QStringLiteral("987");
  }
  ok &= check(structure && !all.contains("topsecret") && !all.contains("12345678") && !all.contains("Alice") && !all.contains("do-not-keep"), "stored bytes also redact sensitive content");
  // Keep the IO worker blocked through the common deadline, then prove it can
  // finish safely even after the caller stopped waiting.
  entered->store(false);
  ok &= check(storage.postAuxiliary([gate, entered](QObject*) { entered->store(true); gate->acquire(); }), "exit blocker admitted");
  ok &= check(until([&] { return entered->load(); }), "exit blocker is live");
  DiagnosticLogger::info(QStringLiteral("shutdown"), QStringLiteral("last bounded event"));
  ok &= check(DiagnosticLogger::closeStorage(&storage), "final bounded flush admitted");
  QElapsedTimer deadline; deadline.start();
  ok &= check(!storage.shutdown(40) && deadline.elapsed() < 500, "caller deadline does not destroy blocked IO");
  DiagnosticLogger::info(QStringLiteral("shutdown"), QStringLiteral("after closing"));
  ok &= check(DiagnosticLogger::status().closing, "closing is visible");
  gate->release();
  ok &= check(storage.shutdown(2000), "same worker safely drains after caller timeout");
  ok &= check(DiagnosticLogger::status().pendingEvents == 0, "accepted final batch drained");
  return ok;
}
}
int main(int argc, char** argv) {
  // This call precedes QCoreApplication, as real compatibility initialization does.
  TargetCompatibilityReport report; report.supported = false; report.failureReason = L"C:\\Users\\Alice\\private.exe";
  DiagnosticLogger::initialize(report);
  QCoreApplication app(argc, argv);
  if (app.arguments().value(1) == QStringLiteral("--close-rejected")) return closeRejected() ? 0 : 1;
  if (app.arguments().value(1) == QStringLiteral("--probe")) {
    const auto path = app.arguments().value(2);
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 2;
    CloseHandle(handle); return 0;
  }
  QTemporaryDir directory;
  bool ok = check(directory.isValid(), "temporary fixture directory");
  ok &= rotation(directory.filePath(QStringLiteral("rotation")));
  ok &= directoryBudget(directory.filePath(QStringLiteral("budget")));
  ok &= pathAndScan(directory.filePath(QStringLiteral("scan")));
  QProcess closeChild;
  closeChild.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--close-rejected")});
  ok &= check(closeChild.waitForFinished(5000) && closeChild.exitStatus() == QProcess::NormalExit && closeChild.exitCode() == 0,
      "independent final-admission rejection test");
  if (closeChild.exitCode()) std::fputs(closeChild.readAllStandardError().constData(), stderr);
  ok &= logger(directory.filePath(QStringLiteral("logger")));
  if (ok) std::puts("PASS: diagnostics rotation, live-run locking, bounded queue, privacy and shutdown");
  return ok ? 0 : 1;
}
