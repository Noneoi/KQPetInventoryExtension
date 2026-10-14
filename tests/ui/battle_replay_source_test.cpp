#include "ui/battle/battle_replay_source.h"
#include "application/views/inventory_projection.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
struct Received { QString label; QByteArray bytes; QString sha; };
bool waitUntil(const std::function<bool()>& predicate, int maximumMs = 2000) {
  QElapsedTimer clock; clock.start();
  while (!predicate() && clock.elapsed() < maximumMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return predicate();
}
void observeFor(int milliseconds) {
  QElapsedTimer clock; clock.start();
  waitUntil([&] { return clock.elapsed() >= milliseconds; }, milliseconds + 100);
}
bool write(const QString& path, const QByteArray& bytes, bool atomic = false) {
  if (atomic) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
  }
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
}
QByteArray replay(int id) {
  return QJsonDocument(QJsonObject{{"br", QJsonObject{{"bi", id},
    {"or", QJsonObject{{"team", QJsonArray{
      QJsonObject{{"id", 0}, {"players", QJsonArray{QJsonObject{{"name", "synthetic-left"}, {"cts", QJsonArray{}}}}}},
      QJsonObject{{"id", 1}, {"players", QJsonArray{QJsonObject{{"name", "synthetic-right"}, {"cts", QJsonArray{}}}}}}
    }}}}, {"fr", QJsonArray{}}}}}).toJson(QJsonDocument::Compact);
}
QString login(const QJsonValue& account, const QJsonValue& nickname) {
  return QString::fromUtf8(QJsonDocument(QJsonObject{{"_cmd", "21_1"},
    {"info", QJsonObject{{"n", account}, {"nn", nickname}}}}).toJson(QJsonDocument::Compact));
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  int failures = 0;
  const auto check = [&](bool ok, const char* message) {
    if (!ok) { qCritical() << "BattleReplaySource:" << message; ++failures; }
  };
  BattleReplaySource::Options options;
  options.settleMs = 20; options.pollMs = 5;
  options.maximumReplayBytes = 4096; options.maximumPendingBytes = 8192;
  options.maximumPendingCount = 2;

  // All writes are isolated synthetic fixtures. Production only reads the
  // authorized directory and the bridge's already existing inbound signal.
  QTemporaryDir temporary;
  check(temporary.isValid(), "temporary fixture directory unavailable");
  if (!temporary.isValid()) return 1;
  const auto directory = QDir(temporary.path()).filePath(QStringLiteral("reports"));
  check(QDir().mkpath(directory), "fixture reports directory creation failed");
  const auto initial = QDir(directory).filePath(QStringLiteral("initial-win.txt"));
  check(write(initial, replay(1)), "baseline write failed");
  BattleReplaySource files(options);
  QList<Received> received;
  QObject::connect(&files, &BattleReplaySource::replayReady, &app,
    [&](const QString& label, const QByteArray& bytes, const QString& sha) { received.append({label, bytes, sha}); });
  files.replayPending();
  files.startDirectory(directory);
  observeFor(100);
  check(received.isEmpty(), "existing reports must be baseline, not new arrivals");
  check(write(initial, replay(2)), "baseline overwrite failed");
  check(waitUntil([&] { return received.size() == 1; }), "written baseline report was not delivered");
  if (!received.isEmpty()) {
    check(received[0].bytes == replay(2), "file delivery changed original bytes");
    check(received[0].label == QFileInfo(initial).absoluteFilePath(), "file provenance lost absolute path");
    check(received[0].sha == QString::fromLatin1(QCryptographicHash::hash(replay(2), QCryptographicHash::Sha256).toHex()), "SHA does not identify delivered bytes");
  }
  const auto growing = QDir(directory).filePath(QStringLiteral("new-fail.txt"));
  check(write(growing, "{\"br\":"), "partial write failed");
  observeFor(100);
  check(received.size() == 1, "truncated JSON generated a replay notification");
  check(write(growing, replay(3)), "completed write failed");
  check(waitUntil([&] { return received.size() == 2; }), "completed new report not delivered");
  const auto duplicate = QDir(directory).filePath(QStringLiteral("copy-win.json"));
  check(write(duplicate, replay(3)), "duplicate fixture write failed");
  observeFor(100);
  check(received.size() == 2, "identical renamed content bypassed SHA deduplication");
  check(write(growing, replay(4), true), "atomic replacement failed");
  check(waitUntil([&] { return received.size() == 3; }), "atomic replacement lost its file watch");
  check(write(growing, replay(5), true), "second atomic replacement failed");
  check(waitUntil([&] { return received.size() == 4; }), "watch was not re-added after atomic replacement");
  if (received.size() >= 2) check(received[1].bytes == replay(3), "queued snapshot changed when its source file was replaced");

  // Renaming the watched directory removes the native watch; recreating it
  // must be recovered via the parent watch or polling without restarting.
  check(QDir(temporary.path()).rename(QStringLiteral("reports"), QStringLiteral("old-reports")), "fixture directory rename failed");
  check(waitUntil([&] { return files.status().contains(QStringLiteral("等待样本目录")); }), "missing directory status not exposed");
  check(QDir().mkpath(directory), "replacement directory creation failed");
  check(write(QDir(directory).filePath(QStringLiteral("restored.txt")), replay(6)), "restored fixture write failed");
  check(waitUntil([&] { return received.size() == 5; }), "recreated directory was not watched");
  check(write(QDir(temporary.path()).filePath(QStringLiteral("outside.txt")), replay(7)), "outside fixture write failed");
  observeFor(100);
  check(received.size() == 5, "ancestor watch admitted a file outside the chosen directory");
  files.stopDirectory();
  check(write(QDir(directory).filePath(QStringLiteral("stopped.txt")), replay(8)), "stopped fixture write failed");
  observeFor(100);
  check(received.size() == 5, "stopped watcher still delivered files");

  const auto laterDirectory = QDir(temporary.path()).filePath(QStringLiteral("later/child"));
  files.startDirectory(laterDirectory);
  check(QDir().mkpath(laterDirectory), "late directory creation failed");
  check(write(QDir(laterDirectory).filePath(QStringLiteral("late.txt")), replay(9)), "late fixture write failed");
  check(waitUntil([&] { return received.size() == 6; }), "directory absent at startup was never discovered");
  files.stopDirectory();

#ifdef Q_OS_WIN
  // Win32 exclusive handles reproduce actual sharing violations, unlike a
  // normal QFile writer (which can still permit the observer to read).
  const auto lockedDirectory = QDir(temporary.path()).filePath(QStringLiteral("locked-reports"));
  check(QDir().mkpath(lockedDirectory), "locked fixture directory creation failed");
  files.startDirectory(lockedDirectory);
  const auto lockedA = QDir(lockedDirectory).filePath(QStringLiteral("a-locked.txt"));
  const auto lockedB = QDir(lockedDirectory).filePath(QStringLiteral("b-locked.txt"));
  check(write(lockedA, replay(20)) && write(lockedB, replay(21)), "locked fixture writes failed");
  const HANDLE lockA = CreateFileW(reinterpret_cast<LPCWSTR>(lockedA.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const HANDLE lockB = CreateFileW(reinterpret_cast<LPCWSTR>(lockedB.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(lockA != INVALID_HANDLE_VALUE && lockB != INVALID_HANDLE_VALUE, "exclusive fixture handles unavailable");
  const auto beforeLocked = received.size();
  check(write(QDir(lockedDirectory).filePath(QStringLiteral("z-ready.txt")), replay(22)), "ready fixture write failed");
  check(waitUntil([&] { return received.size() > beforeLocked; }) && received.last().bytes == replay(22),
    "two locked files consumed every scan budget and starved a later valid replay");
  files.stopDirectory();
  if (lockA != INVALID_HANDLE_VALUE) CloseHandle(lockA);
  if (lockB != INVALID_HANDLE_VALUE) CloseHandle(lockB);
#endif

  BattleReplaySource inbound(options);
  QList<Received> inboundRecords;
  int overflow = 0;
  QObject::connect(&inbound, &BattleReplaySource::replayReady, &app,
    [&](const QString& label, const QByteArray& bytes, const QString& sha) { inboundRecords.append({label, bytes, sha}); });
  QObject::connect(&inbound, &BattleReplaySource::sourceIssue, &app,
    [&](const QString&, const QString&) { ++overflow; });
  inbound.observeInbound(QStringLiteral("unrelated-channel"), QString::fromUtf8(replay(10)));
  inbound.observeInbound(QStringLiteral("recivedata"), QStringLiteral("{\"br\":"));
  inbound.observeInbound(QStringLiteral("recivedata"), QStringLiteral("{\"_cmd\":\"2_1_10\",\"pl\":[]}"));
  inbound.observeInbound(QStringLiteral("recivedata"), QStringLiteral("{\"br\":{\"or\":{\"team\":[]}}}"));
  auto grouped = QJsonDocument::fromJson(replay(11)).object();
  auto battle = grouped[QStringLiteral("br")].toObject(); battle.insert(QStringLiteral("tbt"), 1); grouped[QStringLiteral("br")] = battle;
  inbound.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(grouped).toJson()));
  check(inbound.bufferedCount() == 0, "unrelated, partial or excluded group packets admitted");
  for (int id : {10, 10, 11, 12}) inbound.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(id)));
  check(inboundRecords.isEmpty() && inbound.bufferedCount() == 2 && overflow == 1,
    "lazy-page buffering/dedup/bounded-overflow contract failed");
  inbound.replayPending();
  check(inboundRecords.size() == 2 && inboundRecords[0].bytes == replay(11) && inboundRecords[1].bytes == replay(12),
    "pending reports not delivered in arrival order");
  inbound.replayPending();
  check(inboundRecords.size() == 2, "reconnecting flushed already-delivered reports again");
  inbound.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(13)));
  check(inboundRecords.size() == 3 && inbound.bufferedCount() == 0, "connected source failed direct pending-page notification");
  inbound.suspendDelivery();
  inbound.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(14)));
  check(inboundRecords.size() == 3 && inbound.bufferedCount() == 1, "destroyed page did not restore buffering");
  inbound.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(15)) + QString(5000, QLatin1Char(' ')));
  check(inbound.bufferedCount() == 1, "oversized payload was retained");
  inbound.replayPending();
  const int beforeOtherChannels = inboundRecords.size();
  inbound.observeInbound(QStringLiteral("cutdata"), QStringLiteral("{\"br\":"));
  inbound.observeInbound(QStringLiteral("otherreturn"), QStringLiteral("{\"br\":"));
  check(inboundRecords.size() == beforeOtherChannels, "non-primary channels accepted incomplete JSON");
  inbound.observeInbound(QStringLiteral("cutdata"), QString::fromUtf8(replay(16)));
  inbound.observeInbound(QStringLiteral("otherreturn"), QString::fromUtf8(replay(17)));
  check(inboundRecords.size() == beforeOtherChannels + 2, "complete existing-channel reports were discarded");

  BattleReplaySource identity(options);
  identity.setSession(QStringLiteral("account-a"), 1, true);
  check(!identity.identityReliable(), "login account was mistaken for player nickname");
  identity.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-a"), QStringLiteral("游戏昵称 A")));
  identity.setSession(QStringLiteral("account-a"), 1, true);
  check(!identity.identityReliable(), "unaccepted login packet immediately changed identity");
  identity.setSession(QStringLiteral("account-a"), 2, true);
  check(identity.identityReliable() && identity.identityName() == QStringLiteral("游戏昵称 A"),
    "accepted matching login nickname was not made available");
  identity.setSession(QStringLiteral("account-a"), 2, true);
  check(identity.identityReliable(), "ordinary same-session publication cleared accepted nickname");
  identity.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-b"), QStringLiteral("错误的 B")));
  identity.setSession(QStringLiteral("account-a"), 3, true);
  check(!identity.identityReliable() && identity.identityName().isEmpty(), "mismatched account login leaked into an accepted session");
  identity.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-a"), QStringLiteral("旧昵称 A")));
  identity.setSession(QStringLiteral("account-b"), 4, true);
  check(!identity.identityReliable(), "account switch retained previous identity");
  identity.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-b"), QStringLiteral("游戏昵称 B")));
  identity.setSession(QStringLiteral("account-b"), 5, false);
  identity.setSession(QStringLiteral("account-b"), 6, true);
  check(!identity.identityReliable(), "inactive session retained pending identity evidence");
  identity.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-b"), QStringLiteral("游戏昵称 B")));
  identity.setSession(QStringLiteral("account-b"), 7, true);
  check(identity.identityReliable() && identity.identityName() == QStringLiteral("游戏昵称 B"), "new valid session did not recover identity");
  identity.setSession(QStringLiteral("account-b"), 7, false);
  check(!identity.identityReliable(), "logout left automatic perspective authority active");
  identity.setIdentity(QStringLiteral("nickname"), true, {});
  check(!identity.identityReliable(), "identity without an evidence description was accepted");
  BattleReplaySource numericIdentity(options);
  numericIdentity.observeInbound(QStringLiteral("recivedata"), login(123, QStringLiteral("数值账号的昵称")));
  numericIdentity.setSession(QStringLiteral("123"), 1, true);
  check(numericIdentity.identityReliable(), "checked numeric login account failed exact session matching");
  numericIdentity.observeInbound(QStringLiteral("recivedata"), login(123, QJsonValue::Null));
  numericIdentity.setSession(QStringLiteral("123"), 2, true);
  check(!numericIdentity.identityReliable(), "null nickname inherited previous identity");
  numericIdentity.observeInbound(QStringLiteral("cutdata"), login(123, QStringLiteral("wrong channel")));
  numericIdentity.setSession(QStringLiteral("123"), 3, true);
  check(!numericIdentity.identityReliable(), "non-login channel supplied nickname authority");

  // This is the exact production adapter, fed the same public snapshot that
  // InventoryPublisher produces for an accepted unverified OriginalBridge
  // login: authenticated=true, state=Uncertain, sourceVerified=false.
  BattleReplaySource production(options);
  InventoryViewSnapshot snapshot;
  snapshot.account = QStringLiteral("account-a"); snapshot.sessionEpoch = 10;
  snapshot.authenticated = true; snapshot.sessionState = SessionConnectionState::Uncertain;
  snapshot.sourceVerified = false;
  const auto publishSession = [&] {
    production.updateSession(snapshot.account, snapshot.sessionEpoch, snapshot.authenticated);
  };
  QList<Received> delivered, pagePending;
  bool identityBeforeDelivery = false;
  QObject::connect(&production, &BattleReplaySource::deliveryContextReset, &app, [&] { pagePending.clear(); });
  QObject::connect(&production, &BattleReplaySource::replayReady, &app,
    [&](const QString& label, const QByteArray& bytes, const QString& sha) {
      delivered.append({label, bytes, sha}); pagePending.append({label, bytes, sha});
      identityBeforeDelivery = production.identityReliable();
    });
  publishSession(); production.replayPending();
  check(!production.identityReliable(), "authenticated weak snapshot without a login packet fabricated nickname authority");
  production.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-a"), QStringLiteral("只读昵称 A")));
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(30)));
  check(delivered.isEmpty() && production.bufferedCount() == 1 && !production.identityReliable(),
    "same-batch login/report was delivered before the accepted session reset");
  // The workbench resets an already-created page before its later source
  // subscriber receives the projection's accountSessionChanged signal.
  pagePending.clear(); snapshot.sessionEpoch = 11; publishSession();
  check(production.identityReliable() && production.identityName() == QStringLiteral("只读昵称 A") &&
    identityBeforeDelivery && delivered.size() == 1 && pagePending.size() == 1 && production.bufferedCount() == 0 &&
    !snapshot.sourceVerified, "production weak login failed nickname matching or the following report was lost at reset");
  publishSession();
  check(production.identityReliable() && pagePending.size() == 1, "same-session publication invalidated nickname or pending replay");

  production.suspendDelivery();
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(31)));
  check(production.bufferedCount() == 1, "lazy source did not retain an old-session fixture");
  production.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-b"), QStringLiteral("只读昵称 B")));
  check(production.bufferedCount() == 0 && pagePending.isEmpty() && !production.identityReliable(),
    "observed new login left old source/page pending entries or old nickname authority");
  // The same bytes are allowed in the new scope; the prior scope's dedup must
  // not reject a report whose earlier pending entry was deliberately cleared.
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(31)));
  snapshot.account = QStringLiteral("account-b"); snapshot.sessionEpoch = 12;
  publishSession();
  check(production.identityReliable() && production.identityName() == QStringLiteral("只读昵称 B") &&
    production.bufferedCount() == 1 && delivered.size() == 1,
    "new-session identical report was suppressed or lazy delivery resumed without a page");
  production.replayPending();
  check(delivered.size() == 2 && delivered.last().bytes == replay(31) && pagePending.size() == 1,
    "new-session pending report was lost or old-account reports were replayed");
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(31)));
  check(delivered.size() == 2, "same-session duplicate escaped dedup after the new account was accepted");

  production.suspendDelivery(); production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(32)));
  snapshot.account = QStringLiteral("account-c"); snapshot.sessionEpoch = 13;
  publishSession();
  check(!production.identityReliable() && production.bufferedCount() == 0 && pagePending.isEmpty(),
    "account change without recorded login reused pending data or a previous nickname");
  production.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-c"), QJsonValue::Null));
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(33)));
  snapshot.sessionEpoch = 14; publishSession(); production.replayPending();
  check(!production.identityReliable() && delivered.last().bytes == replay(33),
    "accepted login with no nickname either invented identity or lost its following replay");

  production.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-c"), QStringLiteral("未接纳昵称")));
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(34)));
  snapshot.authenticated = false; snapshot.sessionEpoch = 15; publishSession();
  check(!production.identityReliable() && production.bufferedCount() == 0 && pagePending.isEmpty(),
    "Uncertain without authentication accepted login evidence or retained another session's pending replay");
  production.observeInbound(QStringLiteral("recivedata"), login(QStringLiteral("account-c"), QStringLiteral("捕获失效前昵称")));
  production.observeInbound(QStringLiteral("recivedata"), QString::fromUtf8(replay(35)));
  production.invalidateSession(QStringLiteral("synthetic capture failure"));
  snapshot.authenticated = true; snapshot.sessionState = SessionConnectionState::Active; snapshot.sessionEpoch = 16;
  publishSession();
  check(!production.identityReliable() && production.bufferedCount() == 0,
    "capture failure retained a login candidate and promoted it on a later session");

  if (failures == 0) qInfo() << "PASS: stable replay files, passive packets, immutable pending snapshots and accepted-session nickname identity";
  return failures == 0 ? 0 : 1;
}
