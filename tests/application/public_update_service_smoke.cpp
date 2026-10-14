#include "application/runtime/public_update_service.h"
#include "application/runtime/data_update_service.h"
#include "storage/storage_service.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QTimer>
#include <cstdio>

namespace {
bool check(bool value, const char* text) { if (!value) std::fprintf(stderr, "FAIL: %s\n", text); return value; }
QByteArray json(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
bool save(const QString& root, const QString& relative, const QByteArray& bytes) {
  const auto path = QDir(root).filePath(relative); QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool save(const QString& root, const QString& relative, const QJsonObject& value) { return save(root, relative, json(value)); }
QJsonObject read(const QString& root, const QString& relative) {
  QFile file(QDir(root).filePath(relative)); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
struct Fixture {
  QString root, revision;
  QString oldAttempt = QString(32, 'a'), nextAttempt = QString(32, 'b'), generation = QString(32, 'c');
  QJsonObject pointer;
  explicit Fixture(QString path) : root(std::move(path)) {
    QJsonObject parts;
    for (const auto* key : {"pets", "skills", "shop", "images", "icons", "routines", "battleReplay"}) parts.insert(key, QJsonObject{{"sha256", QString(64,'d')}, {"bytes", 2}, {"format", "json"}});
    const auto raw = json({{"schema", 1}, {"dependencySchema", 1}, {"components", parts}, {"attachments", QJsonObject{}}});
    revision = sha(raw);
    save(root, QStringLiteral("public/releases/%1/manifest.json").arg(revision), raw);
    pointer = {{"schema", 1}, {"revision", revision}, {"previousRevision", ""}, {"generation", generation}, {"attempt", oldAttempt}};
    save(root, "public/current.json", pointer);
  }
  void receipt(const QJsonObject& value, bool latest = true) {
    const auto id = value.value("attempt").toString();
    save(root, QStringLiteral("public/transactions/%1/receipt.json").arg(id), value);
    if (latest) save(root, "public/reports/latest.json", value);
  }
  void request(const QString& attempt, const QString& time) {
    const QJsonObject value{{"schema", 1}, {"attempt", attempt}, {"createdUtc", time}, {"requestedComponents", QJsonArray{"routines"}}};
    save(root, QStringLiteral("public/transactions/%1/request.json").arg(attempt), value);
    save(root, "public/reports/latest-request.json", value);
  }
  void candidate(const QString& attempt, const QString& target, const QJsonObject& results, bool withChanges = true) {
    QJsonObject validation{{"schema", 1}, {"attempt", attempt}, {"revision", target}, {"passed", true}};
    if (withChanges) {
      const auto changes = json({{"schema", 1}, {"attempt", attempt}, {"revision", target}, {"total", 1}, {"truncated", false},
          {"rows", QJsonArray{QJsonObject{{"component", "skills"}, {"category", QStringLiteral("技能与效果")}, {"kind", "changed"},
            {"name", QStringLiteral("测试技能")}, {"beforeText", QStringLiteral("旧官方文字")}, {"afterText", ""},
            {"technical", QJsonObject{{"id", "123"}}}}}}});
      save(root, QStringLiteral("public/transactions/%1/changes.json").arg(attempt), changes);
      validation.insert("changeReport", QJsonObject{{"file", "changes.json"}, {"sha256", sha(changes)}, {"bytes", changes.size()}});
    }
    const auto raw = json(validation);
    save(root, QStringLiteral("public/transactions/%1/validation.json").arg(attempt), raw);
    save(root, QStringLiteral("public/transactions/%1/candidate.json").arg(attempt), QJsonObject{{"schema", 1}, {"attempt", attempt},
      {"revision", target}, {"baseRevision", revision}, {"requestedComponents", QJsonArray{"routines"}},
      {"effectiveComponents", QJsonArray{"routines"}}, {"results", results},
      {"validation", QJsonObject{{"reportSha256", sha(raw)}, {"passed", true}}}});
  }
};
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv); bool ok = true; QTemporaryDir temporary; Fixture f(temporary.path());
  QJsonObject oldReceipt{{"attempt", f.oldAttempt}, {"schema", 1}, {"createdUtc", "2026-10-13T01:00:00Z"},
    {"recordedUtc", "2026-10-14T15:00:00Z"}, {"revision", f.revision}, {"diskCommitted", true}, {"sessionApplied", true},
    {"candidatePassed", true}, {"state", "sessionApplied"}};
  f.receipt(oldReceipt);
  auto state = PublicUpdateService::readState(f.root, {});
  ok &= check(state.readable && state.diskCommitted && !state.sessionApplied && state.outcome == PublicUpdateOutcome::PendingApply,
      "another process sessionApplied never describes this session");
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(state.sessionApplied && state.outcome == PublicUpdateOutcome::Applied && !state.changesRecorded,
      "actual Core revision supplies session status; legacy lack of changes is explicit");

  f.request(f.nextAttempt, "2026-10-14T09:00:00+00:00");
  QJsonObject failed{{"attempt", f.nextAttempt}, {"createdUtc", "2026-10-14T09:00:00Z"}, {"state", "failed"},
    {"error", "download unavailable at C:\\private-user\\account.txt"}, {"checked", true}, {"diskCommitted", false},
    {"results", QJsonObject{{"routines", QJsonObject{{"status", "failed"}, {"error", "TECH-ID-123"}}}}}};
  f.receipt(failed, false);
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(state.latestAttempt == f.nextAttempt && state.outcome == PublicUpdateOutcome::KeptPrevious && state.retryComponents == QStringList{"routines"},
      "older receipt acknowledged later must not replace latest failed check");
  oldReceipt.remove("createdUtc"); f.receipt(oldReceipt);
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(state.latestAttempt == f.nextAttempt, "legacy ack without immutable request time cannot replace new check");
  oldReceipt.insert("kind", "migration"); oldReceipt.insert("createdUtc", "2026-10-14T10:00:00Z"); f.receipt(oldReceipt);
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(state.latestAttempt == f.nextAttempt, "internal migration after request began cannot hide its later failure");
  ok &= check(!state.components.last().message.contains("TECH-ID"), "technical IDs never enter default component summary");
  save(f.root, "accounts/private/secret.json", QByteArray("ACCOUNT-SENTINEL"));
  save(f.root, "battle-reports/private.txt", QByteArray("BATTLE-SENTINEL"));
  const auto diagnostic = json(PublicUpdateService::publicDiagnostics(f.root, f.revision));
  ok &= check(!diagnostic.contains("ACCOUNT-SENTINEL") && !diagnostic.contains("BATTLE-SENTINEL") && !diagnostic.contains("private-user"),
      "public diagnostic whitelist omits personal files and redacts local paths");

  const QJsonObject unchanged{{"routines", QJsonObject{{"status", "unchanged"}}}};
  f.candidate(f.nextAttempt, f.revision, unchanged);
  QJsonObject committed{{"attempt", f.nextAttempt}, {"revision", f.revision}, {"candidatePassed", true}, {"diskCommitted", true},
    {"plannedGeneration", f.generation}, {"state", "diskCommitted"}, {"createdUtc", "2026-10-14T09:00:00Z"}, {"results", unchanged}};
  f.receipt(committed);
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(state.diskCommitted && state.sessionApplied && state.outcome == PublicUpdateOutcome::Unchanged && read(f.root,"public/current.json")==f.pointer,
      "unchanged candidate with older pointer attempt is acknowledged through matching generation");
  ok &= check(state.changesRecorded && state.changes.size() == 1 && state.changes.first().afterText.isEmpty(), "bound exact official change text keeps empty descriptions");
  save(f.root, QStringLiteral("public/transactions/%1/changes.json").arg(f.nextAttempt), QByteArray("{}"));
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(!state.readable && !state.changesRecorded && state.changes.isEmpty(), "tampered change report cannot be displayed as verified");

  const auto candidateRevision = QString(64,'e');
  f.candidate(f.nextAttempt, candidateRevision, unchanged, false);
  committed.insert("revision",candidateRevision); committed.insert("diskCommitted",false); committed.remove("plannedGeneration");
  f.receipt(committed);
  state = PublicUpdateService::readState(f.root, f.revision);
  ok &= check(state.candidatePassed && !state.diskCommitted && state.sessionApplied && state.outcome == PublicUpdateOutcome::PendingApply,
      "candidate awaiting publication is separate from the still applied disk version");
  committed.insert("error", "native validation rejected"); f.receipt(committed);
  state = PublicUpdateService::readState(f.root,f.revision);
  ok &= check(state.outcome == PublicUpdateOutcome::KeptPrevious, "rejected publication does not expose pending apply action");
  committed.remove("error"); committed.insert("state","cancelled"); f.receipt(committed);
  state = PublicUpdateService::readState(f.root,f.revision);
  ok &= check(state.outcome == PublicUpdateOutcome::Cancelled && !state.diskCommitted, "cancelled preparation preserves actual current version");

  QJsonObject partial{{"routines", QJsonObject{{"status","retained"}, {"retryable",true}}}};
  f.candidate(f.nextAttempt, f.revision, partial, false);
  committed.insert("revision",f.revision); committed.insert("diskCommitted",true); committed.insert("plannedGeneration",f.generation);
  committed.insert("state","diskCommitted"); committed.insert("results",partial); f.receipt(committed);
  state = PublicUpdateService::readState(f.root,f.revision);
  ok &= check(state.outcome == PublicUpdateOutcome::Partial && state.retryComponents == QStringList{"routines"}, "retained component gives actionable partial result");
  committed.insert("kind","restore"); f.receipt(committed);
  state = PublicUpdateService::readState(f.root,f.revision);
  ok &= check(state.outcome == PublicUpdateOutcome::Restored, "restored actual version has explicit outcome");

  QTemporaryDir cancellationRoot; StorageService storage(cancellationRoot.path()); DataUpdateService updater(&storage);
  QEventLoop loop; bool finished = false, applied = false;
  QObject::connect(&updater, &DataUpdateService::candidatePrepared, &loop, [&](const QString&,const QString&) { applied = true; });
  QObject::connect(&updater, &DataUpdateService::finished, &loop, [&](bool,const QStringList&,const QString&) { finished = true; loop.quit(); });
  ok &= check(!updater.canCancel() && !updater.cancelPreparation(), "idle request cannot claim cancellation");
  ok &= check(updater.requestUpdate({"routines"}) && updater.canCancel() && updater.cancelPreparation() && !updater.cancelPreparation(),
      "preparation cancellation accepted exactly once");
  QTimer::singleShot(5000,&loop,&QEventLoop::quit); loop.exec();
  state = PublicUpdateService::readState(cancellationRoot.path(),{});
  ok &= check(finished && !applied && state.outcome == PublicUpdateOutcome::Cancelled && !state.latestAttempt.isEmpty(),
      "cancel before export persists genuine request without invoking publication");
  updater.close(); storage.shutdown();

  // Keep one Auxiliary callback occupied while repeatedly requesting status.
  // The status service may queue only one read and coalesce all further calls.
  StorageService statusStorage(f.root); PublicUpdateService projection(&statusStorage);
  QSemaphore entered, release;
  ok &= check(statusStorage.postAuxiliary([&](QObject*) { entered.release(); release.acquire(); }), "admit status test barrier");
  ok &= check(entered.tryAcquire(1,2000), "status test barrier started");
  int notifications = 0; QEventLoop projected;
  QObject::connect(&projection,&PublicUpdateService::stateChanged,&projected,[&](const PublicUpdateState&) {
    if (++notifications == 2) projected.quit();
  });
  bool allAccepted = true;
  for (int i=0;i<100;++i) allAccepted &= projection.refresh();
  ok &= check(allAccepted, "repeated refreshes coalesce without exhausting Auxiliary admission");
  release.release(); QTimer::singleShot(5000,&projected,&QEventLoop::quit); projected.exec();
  ok &= check(notifications==2 && projection.state().diskRevision==f.revision, "one in-flight read plus one dirty refresh produces current state");
  ok &= check(statusStorage.postAuxiliary([](QObject*) {}), "coalesced status reads leave Auxiliary available for user work");
  projection.close(); statusStorage.shutdown();

  QTemporaryDir queuedRoot; StorageService queuedStorage(queuedRoot.path()); DataUpdateService queuedUpdater(&queuedStorage);
  QSemaphore queueEntered, queueRelease; QEventLoop queuedLoop; bool queueFinished = false;
  ok &= check(queuedStorage.postAuxiliary([&](QObject*) { queueEntered.release(); queueRelease.acquire(); }) && queueEntered.tryAcquire(1,2000),
      "user-update admission test holds first Auxiliary slot");
  ok &= check(queuedStorage.postAuxiliary([](QObject*) {}), "user-update admission test fills second Auxiliary slot");
  QObject::connect(&queuedUpdater,&DataUpdateService::finished,&queuedLoop,[&](bool,const QStringList&,const QString&) { queueFinished = true; queuedLoop.quit(); });
  ok &= check(queuedUpdater.requestUpdate({"routines"}) && queuedUpdater.busy() && !queuedUpdater.requestUpdate({"pets"}),
      "full Auxiliary accepts one bounded user update and rejects duplicate requests");
  ok &= check(queuedUpdater.cancelPreparation(), "waiting user update remains cancellable");
  queueRelease.release(); QTimer::singleShot(5000,&queuedLoop,&QEventLoop::quit); queuedLoop.exec();
  state = PublicUpdateService::readState(queuedRoot.path(),{});
  ok &= check(queueFinished && !queuedUpdater.busy() && state.outcome == PublicUpdateOutcome::Cancelled &&
      !QFileInfo::exists(QDir(queuedRoot.path()).filePath("data-tools/scripts/bootstrap-public-data.ps1")),
      "cancelled queued update records cancellation before any tool export");
  queuedUpdater.close(); queuedStorage.shutdown();
  return ok ? 0 : 1;
}
