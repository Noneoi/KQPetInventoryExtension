#include "pet_repository.h"
#include "protocol_test_support.h"
#include "storage_service.h"
#include "../src/domain/pet_identity.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTemporaryDir>
#include <cstdio>

namespace {
bool check(bool value, const char* message) { if (!value) std::fprintf(stderr, "FAIL: %s\n", message); return value; }
QJsonObject pet(qint64 id, bool complete, int level = 100) {
  QJsonObject result{{QStringLiteral("id"), QString::number(id)}, {QStringLiteral("r"), 990000 + int(id)},
      {QStringLiteral("n"), QStringLiteral("raw-%1").arg(id)}, {QStringLiteral("lv"), level},
      {QStringLiteral("zdl"), 1000}, {QStringLiteral("xzdl"), 2000},
      {QStringLiteral("originalUnknownField"), QString(6000, QLatin1Char('x'))}};
  if (complete) {
    result.insert(QStringLiteral("czdlv"), QJsonObject{{QStringLiteral("lv"), 1}, {QStringLiteral("sgv"), 1}});
    result.insert(QStringLiteral("mzdlv"), QJsonObject{{QStringLiteral("lv"), 2}, {QStringLiteral("sgv"), 2}});
  }
  return result;
}
bool idle(PetRepository& repo) { return waitForRepositoryIdle(&repo, 7000); }
void login(PetRepository& repo, const QString& account) {
  deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), account}}}});
}
bool backpack(PetRepository& repo, const QJsonArray& pets, quint64 task) {
  repo.beginListRefresh(task, repo.accountKey(), repo.sessionGeneration());
  QStringList ids; for (const auto& value : pets) ids.append(value.toObject().value(QStringLiteral("id")).toString());
  deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")}, {QStringLiteral("pl"), pets},
      {QStringLiteral("pps"), QJsonArray{ids.join(QLatin1Char('#'))}}, {QStringLiteral("ppc"), 12}});
  return idle(repo);
}
bool warehouse(PetRepository& repo, int level1, quint64 task) {
  QJsonArray entries;
  for (int id = 1; id <= 6; ++id)
    entries.append(QJsonObject{{QStringLiteral("id"), QString::number(id)}, {QStringLiteral("ri"), 990000 + id},
        {QStringLiteral("n"), QStringLiteral("raw-%1").arg(id)}, {QStringLiteral("lv"), id == 1 ? level1 : 100}});
  repo.expectListPart(QStringLiteral("2_1_S"), task, repo.accountKey(), repo.sessionGeneration());
  deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")}, {QStringLiteral("ns"), entries},
      {QStringLiteral("rb"), QJsonArray{}}, {QStringLiteral("es"), QJsonArray{}}});
  return idle(repo);
}
bool detail(PetRepository& repo, qint64 id, quint64 task) {
  repo.expectDetail(id, task, repo.accountKey(), repo.sessionGeneration());
  deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")}, {QStringLiteral("p"), pet(id, true)}});
  return idle(repo);
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }

bool evictionAndRestore() {
  QTemporaryDir root;
  StorageService storage(root.path());
  PetRecordCacheLimits limits; limits.maximumBytes = 32 * 1024; limits.maximumRecordBytes = 24 * 1024;
  PetRepository repo(nullptr, &storage, root.filePath(QStringLiteral("legacy")), limits);
  // Simulate the separate, tested fact consumer acknowledging each frozen key.
  QObject::connect(&repo, &PetRepository::rawRecordAvailable, &repo, [&](const RawPetRecordHandle& raw) {
    repo.markRecordDerived(raw->key);
  });
  login(repo, QStringLiteral("raw-cache"));
  bool ok = check(idle(repo) && backpack(repo, {}, 1) && warehouse(repo, 100, 1), "raw fixture roster");
  const auto missing = repo.rawRecordHandle(4);
  ok &= check(missing && !missing->complete && !missing->payload && missing->key.detailMemoryRevision != 0,
      "never-complete member did not expose a versioned unknown handle");
  ok &= check(detail(repo, 1, 11) && detail(repo, 2, 12) && detail(repo, 3, 13), "three durable raw observations");
  const auto version = repo.recordVersion(1);
  ok &= check(version.complete && version.persisted && version.rawProjectionOnly && !repo.rawRecordResident(1) && !repo.rawRecordHandle(1) &&
      repo.hasCachedDetail(1) && repo.isDetailPersisted(1), "LRU eviction erased knowledge or durable identity");
  const auto exports = repo.rawCacheStats().untrackedExports;
  ok &= check(!repo.briefFor(3).contains(QStringLiteral("originalUnknownField")) &&
      !repo.briefFor(3).contains(QStringLiteral("czdlv")) && repo.warehouseBriefs().size() == 6 &&
      repo.currentInstanceIds().size() == 6 && repo.rawCacheStats().untrackedExports == exports,
      "compact read paths copied full raw data or performed a legacy export");
  PetRecordKey finished; bool loaded = false;
  QObject::connect(&repo, &PetRepository::rawRecordLoadFinished, &repo,
      [&](const PetRecordKey& key, bool success, const QString&) { finished = key; loaded = success; });
  ok &= check(repo.requestCachedDetail(1) && idle(repo) && loaded && finished == version.key &&
      repo.rawRecordResident(1) && repo.detailMemoryRevision(1) == version.key.detailMemoryRevision,
      "known evicted detail did not restore by its frozen read/memory key");
  ok &= check(repo.rawRecordHandle(1)->object().value(QStringLiteral("originalUnknownField")).toString().size() == 6000,
      "restored original lost an unsupported field");
  const auto summaryKey = repo.recordVersion(4).key;
  ok &= check(repo.requestCachedDetail(4) && idle(repo) && !loaded && finished == summaryKey && !repo.recordVersion(4).complete,
      "missing file did not terminate the exact unknown read without inventing completeness");
  const QString path = root.filePath(QStringLiteral("accounts/raw-cache/details/1.json"));
  ok &= check(QFile::remove(path) && detail(repo, 4, 14) && detail(repo, 5, 15) && !repo.rawRecordResident(1),
      "missing-file recovery fixture did not evict its original");
  const auto previousMemory = repo.detailMemoryRevision(1);
  ok &= check(warehouse(repo, 101, 2) && repo.detailMemoryRevision(1) > previousMemory &&
      !repo.recordVersion(1).rawProjectionOnly && repo.recordVersion(1).contentDigest == version.contentDigest,
      "brief-only calculation input reused an old memory/index key");
  const auto changedKey = repo.recordVersion(1).key;
  ok &= check(repo.requestCachedDetail(1) && idle(repo) && !loaded && finished == changedKey && repo.recordVersion(1).complete,
      "missing formerly complete raw was downgraded to Unknown knowledge");
  ok &= check(repo.rawCacheStats().chargedBytes <= limits.maximumBytes, "raw payload budget exceeded under restoration");
  return ok;
}

bool partialBackpack() {
  QTemporaryDir root;
  const QString account = QStringLiteral("partial");
  bool ok = true;
  {
    StorageService storage(root.path());
    PetRepository repo(nullptr, &storage, root.filePath(QStringLiteral("legacy")));
    login(repo, account); ok &= check(idle(repo) && backpack(repo, {pet(20, false)}, 1), "partial observation fixture");
    auto partial = repo.rawRecordHandle(20);
    ok &= check(partial && !partial->complete && partial->persisted && repo.hasCachedDetail(20) &&
        repo.isDetailPersisted(20) && !repo.recordVersion(20).complete, "saving partial raw invented complete cultivation");
    const QString path = root.filePath(QStringLiteral("accounts/partial/details/20.json"));
    const auto file = QJsonDocument::fromJson(read(path)).object();
    ok &= check(file.value(QStringLiteral("complete")).isBool() && !file.value(QStringLiteral("complete")).toBool() &&
        file.value(QStringLiteral("pet")).toObject().contains(QStringLiteral("originalUnknownField")),
        "partial original or explicit false completeness was not independently preserved");
  }
  {
    StorageService storage(root.path());
    PetRepository repo(nullptr, &storage, root.filePath(QStringLiteral("legacy")));
    login(repo, account); ok &= check(idle(repo) && !repo.recordVersion(20).complete && repo.rawRecordResident(20),
        "reload promoted a saved partial original to complete");
    ok &= check(backpack(repo, {pet(20, true)}, 2), "complete replacement observation");
    const auto complete = repo.rawRecordHandle(20);
    const QString path = root.filePath(QStringLiteral("accounts/partial/details/20.json"));
    const auto saved = read(path);
    QJsonObject summary = pet(20, false, 101); summary.remove(QStringLiteral("originalUnknownField"));
    ok &= check(backpack(repo, {summary}, 3) && repo.recordVersion(20).complete &&
        repo.rawRecordHandle(20)->payload == complete->payload && read(path) == saved &&
        repo.briefFor(20).value(QStringLiteral("lv")).toInt() == 101,
        "partial list overwrote an old complete original or its file");
    summary.insert(QStringLiteral("newUnknownArray"), QJsonArray{1, 2});
    backpack(repo, {summary}, 4);
    ok &= check(repo.sessionContext().state == SessionConnectionState::Uncertain &&
        repo.rawRecordHandle(20)->payload == complete->payload && read(path) == saved,
        "unrepresentable partial field was silently lost or merged into a false complete record");
  }
  return ok;
}

bool partialBeforeFirstLocalRead() {
  QTemporaryDir root;
  const QString account = QStringLiteral("early-partial");
  const QString path = root.filePath(QStringLiteral("accounts/early-partial/details/20.json"));
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile originalFile(path);
  const auto originalBytes = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 3},
      {QStringLiteral("account"), account}, {QStringLiteral("instanceId"), QStringLiteral("20")},
      {QStringLiteral("complete"), true}, {QStringLiteral("pet"), pet(20, true)}}).toJson();
  bool ok = check(originalFile.open(QIODevice::WriteOnly) && originalFile.write(originalBytes) == originalBytes.size(),
      "early-partial fixture could not create its old original");
  originalFile.close();
  StorageService storage(root.path());
  QSemaphore entered, release;
  ok &= check(storage.postAuxiliary([&](QObject*) { entered.release(); release.acquire(); }) && entered.tryAcquire(1, 2000),
      "early-partial fixture did not hold IO");
  PetRepository repo(nullptr, &storage, root.filePath(QStringLiteral("legacy")));
  login(repo, account);
  repo.beginListRefresh(1, account, repo.sessionGeneration());
  deliverVerifiedFixture(&repo, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
      {QStringLiteral("pl"), QJsonArray{pet(20, false, 101)}},
      {QStringLiteral("pps"), QJsonArray{QStringLiteral("20")}}, {QStringLiteral("ppc"), 12}});
  release.release();
  ok &= check(idle(repo) && read(path) == originalBytes && repo.recordVersion(20).complete &&
      repo.rawRecordHandle(20)->object().contains(QStringLiteral("czdlv")) &&
      repo.briefFor(20).value(QStringLiteral("lv")).toInt() == 101,
      "a partial list arriving before local load replaced the complete disk original or hid the new summary");
  return ok;
}

bool historyDoesNotBlockCurrentInventory() {
  QTemporaryDir root;
  const QString account = QStringLiteral("history-preload");
  const QString directory = root.filePath(QStringLiteral("accounts/") + account);
  const auto write = [](const QString& path, const QJsonObject& object) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  bool ok = check(write(QDir(directory).filePath(QStringLiteral("inventory.json")),
      {{QStringLiteral("schema"),3},{QStringLiteral("account"),account},{QStringLiteral("backpack"),QJsonArray{}},
       {QStringLiteral("warehouse"),QJsonArray{QJsonObject{{QStringLiteral("id"),QStringLiteral("99")},{QStringLiteral("ri"),990099},{QStringLiteral("n"),QStringLiteral("current")},{QStringLiteral("lv"),100}}}}}),"history fixture inventory");
  QHash<qint64,QByteArray> originals;
  for (qint64 id : {1,2,99}) {
    const QString path = QDir(directory).filePath(QStringLiteral("details/%1.json").arg(id));
    ok &= check(write(path,{{QStringLiteral("schema"),3},{QStringLiteral("account"),account},{QStringLiteral("instanceId"),QString::number(id)},
        {QStringLiteral("complete"),true},{QStringLiteral("pet"),pet(id,true)}}),"history fixture raw detail");
    originals.insert(id,read(path));
  }
  StorageService storage(root.path());
  PetRecordCacheLimits limits; limits.maximumRecords = 2; limits.maximumBytes = 64 * 1024; limits.maximumRecordBytes = 32 * 1024;
  PetRepository repo(nullptr,&storage,root.filePath(QStringLiteral("legacy")),limits);
  QSet<qint64> loaded;
  QObject::connect(&repo,&PetRepository::rawRecordAvailable,&repo,[&](const RawPetRecordHandle& raw) {
    if (!raw->payload) return;
    loaded.insert(raw->key.instanceId);
    // Models the Controller's identity prerequisite for a successful derived fact.
    if (petInstanceId(raw->brief) == raw->key.instanceId) repo.markRecordDerived(raw->key);
  });
  login(repo,account);
  const bool startupIdle = waitForRepositoryIdle(&repo,1200);
  ok &= check(startupIdle && loaded == QSet<qint64>{99} && repo.rawRecordResident(99) &&
      repo.rawCacheStats().protectedRecords == 0 && repo.ioMetrics().rawJsonDecodeCalls == 1,
      "startup loaded unlisted history and pinned raw entries instead of completing current-inventory preload");
  // Skip the explicit-load half if the old implementation is still stuck.
  if (!startupIdle) return false;
  const auto currentVersion = repo.recordVersion(99);
  ok &= check(repo.requestCachedDetail(1) && idle(repo),"explicit history lookup was refused");
  {
    const auto historical = repo.rawRecordHandle(1);
    ok &= check(historical && petInstanceId(historical->brief) == 1 && petRaceId(historical->brief) == 990001 &&
        historical->object().value(QStringLiteral("originalUnknownField")).toString().size() == 6000 &&
        repo.rawCacheStats().protectedRecords == 0 && repo.currentInstanceIds() == QList<qint64>{99},
        "explicit history lost its derivable identity/original fields or became a current member");
  }
  ok &= check(repo.requestCachedDetail(2) && idle(repo) && !repo.rawRecordResident(99) && repo.recordVersion(99).complete &&
      repo.requestCachedDetail(99) && idle(repo) && repo.rawRecordResident(99) && repo.recordVersion(99).key == currentVersion.key,
      "explicit history lookup prevented a known current raw record from being evicted/restored");
  for (qint64 id : {1,2,99}) ok &= check(read(QDir(directory).filePath(QStringLiteral("details/%1.json").arg(id))) == originals.value(id),
      "preload filtering or explicit lookup changed a historical source file");
  return ok;
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const bool ok = evictionAndRestore() && partialBackpack() && partialBeforeFirstLocalRead() && historyDoesNotBlockCurrentInventory();
  if (ok) std::puts("PASS: compact repository, raw LRU/restore, versioned load failures and partial-source preservation");
  return ok ? 0 : 1;
}
