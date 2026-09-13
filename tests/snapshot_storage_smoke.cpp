#include "asset_analysis_settings.h"
#include "asset_snapshot_store.h"
#include "asset_snapshot_comparator.h"
#include "protocol_test_support.h"
#include "storage_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}

bool waitUntil(const std::function<bool()>& done, int timeout = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!done() && timer.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}

bool waitStore(AssetSnapshotStore& store, int timeout = 10000) {
  return waitUntil([&] { return !store.pendingTaskCount() && !store.historyLoading() && !store.instanceHistoryLoading(); }, timeout);
}

bool writeObject(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  return file.write(bytes) == bytes.size() && file.commit();
}

QJsonObject readObject(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}

void login(PetRepository& repository, const QString& account) {
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), account}}}});
  if (!waitForRepositoryIdle(&repository)) qFatal("repository fixture did not load");
}

AccountAssetOverview overviewFor(PetRepository& repository) {
  AccountAssetOverview overview;
  overview.account = repository.accountKey();
  overview.sourceVerified = repository.sessionContext().canPersist();
  overview.inputSessionEpoch = repository.sessionGeneration();
  overview.inventoryRevision = repository.inventoryRevision();
  overview.totalPets = 2;
  overview.fullyCultivatedPets = 1;
  overview.totalCurrentPower = 3000;
  for (int id = 1; id <= 2; ++id) {
    PetAssetRecord pet;
    pet.instanceId = id;
    pet.raceId = 7152;
    pet.name = QStringLiteral("snapshot-%1").arg(id);
    pet.currentPower = id * 1000;
    pet.highestPower = 2000;
    pet.completionPercent = id * 50;
    pet.detailAvailable = true;
    pet.currentPowerKnown = true;
    pet.cultivationKnown = true;
    pet.redStarKnown = true;
    pet.astrolabeKnown = true;
    pet.fullyCultivated = id == 2;
    // Production Compute publishes identity-only pet JSON. Snapshot semantics
    // must come from the derived known/value fields, not discarded raw xzdl.
    pet.pet = {{QStringLiteral("id"), id}, {QStringLiteral("n"), pet.name}};
    overview.pets.append(pet);
  }
  return overview;
}

QJsonObject historyObject(const QString& account, const QDate& date, const QJsonArray& pets,
                          int schema = 2, qint64 power = 100000) {
  QJsonObject object{{QStringLiteral("schema"), schema}, {QStringLiteral("account"), account},
      {QStringLiteral("createdAt"), QDateTime(date, QTime(12, 0), Qt::UTC).toString(Qt::ISODate)},
      {QStringLiteral("pets"), pets}};
  const QJsonObject summary{{QStringLiteral("totalPets"), pets.size()},
      {QStringLiteral("fullyCultivatedPets"), 0}, {QStringLiteral("totalCurrentPower"), QString::number(power)}};
  if (schema == 2) {
    object.insert(QStringLiteral("analysisVersion"), AssetAnalysisVersion::kCurrentAnalysis);
    object.insert(QStringLiteral("summary"), summary);
  } else {
    for (auto it = summary.begin(); it != summary.end(); ++it) object.insert(it.key(), it.value());
  }
  return object;
}

bool snapshotsAndSettings(const QString& root) {
  bool ok = true;
  qputenv("KQPET_DATA_ROOT", root.toUtf8());
  PetRepository repository;
  login(repository, QStringLiteral("snapshot-A"));
  AssetSnapshotStore store(&repository);
  bool savedSignal = false;
  QString savedAccount;
  QObject::connect(&store, &AssetSnapshotStore::writeFinished, &store,
      [&](quint64, const QString& account, quint64, quint64, int, StorageStatus status, const QString&) {
        if (status == StorageStatus::Saved) { savedSignal = true; savedAccount = account; }
      });
  const AccountAssetOverview input = overviewFor(repository);
  AccountAssetOverview weak = input;
  weak.sourceVerified = false;
  ok &= check(!store.write(input.account, weak).accepted, "weak analysis was admitted for snapshot persistence");
  AccountAssetOverview stale = input;
  ++stale.inventoryRevision;
  ok &= check(!store.write(input.account, stale).accepted, "stale analysis input was admitted");
  const StorageSubmission admission = store.write(input.account, input);
  ok &= check(admission.accepted && admission.status == StorageStatus::Queued && !savedSignal,
              "snapshot queue admission pretended the file was Saved");
  ok &= check(waitStore(store) && savedSignal && savedAccount == input.account,
              "snapshot did not report its actual saved account");
  const auto history = store.cachedHistory(input.account);
  ok &= check(history.size() == 1 && history.first().petsComplete && history.first().pets.size() == 2 &&
                  history.first().storageKey == QStringLiteral("snapshots/%1.json").arg(
                      history.first().createdAt.toLocalTime().date().toString(QStringLiteral("yyyy-MM-dd"))),
              "daily snapshot key, UTC timestamp or full-cache view was incorrect");
  ok &= check(history.size() == 1 && history.first().pets.size() == 2 &&
      history.first().totalCurrentPowerKnown &&
      history.first().pets.first().redStarKnown && history.first().pets.first().redStarComplete &&
      history.first().pets.first().astrolabeKnown && history.first().pets.first().astrolabeBreakthrough,
      "snapshot lost known cultivation completion after Compute stripped raw payload");
  AccountAssetSnapshot summary = history.first();
  summary.pets.clear();
  summary.petsComplete = false;
  const auto delta = AssetSnapshotComparator::compare(history.first(), summary);
  ok &= check(!delta.accountComparable && !delta.powerChangeKnown && delta.newPets == 0 && delta.removedPets == 0,
              "summary-only history invented a full membership comparison");
  const QString accountRoot = repository.storageContext()->directory();
  const QString settingsPath = QDir(accountRoot).filePath(QStringLiteral("asset-analysis.json"));
  ok &= check(writeObject(settingsPath, {{QStringLiteral("schema"), 1}, {QStringLiteral("account"), input.account},
      {QStringLiteral("autoSnapshot"), false}, {QStringLiteral("futureSetting"), 314}}), "settings fixture write failed");
  AssetAnalysisSettings settings(&repository);
  ok &= check(!settings.known(input.account) && settings.pending(input.account) && !settings.loadAutoSnapshot(input.account),
              "unknown settings enabled automatic snapshot persistence");
  const auto preference = settings.saveAutoSnapshot(input.account, true);
  ok &= check(preference.accepted && !settings.loadAutoSnapshot(input.account), "pending preference enabled automatic snapshots");
  ok &= check(waitUntil([&] { return !settings.pendingTaskCount() && !settings.pending(input.account); }) &&
                  settings.known(input.account) && settings.loadAutoSnapshot(input.account),
              "old disk preference overwrote a user edit or Saved never became known");
  ok &= check(readObject(settingsPath).value(QStringLiteral("futureSetting")).toInt() == 314 &&
                  readObject(settingsPath).value(QStringLiteral("autoSnapshot")).toBool(),
              "locked JSON merge discarded an unrelated setting");

  const QJsonObject wrongAccount{{QStringLiteral("schema"), 1}, {QStringLiteral("account"), QStringLiteral("another")},
      {QStringLiteral("autoSnapshot"), true}, {QStringLiteral("futureSetting"), 271}};
  ok &= check(writeObject(settingsPath, wrongAccount), "wrong-account settings fixture failed");
  settings.requestLoad(input.account);
  ok &= check(waitUntil([&] { return !settings.pendingTaskCount(); }) && !settings.known(input.account) &&
                  !settings.loadAutoSnapshot(input.account), "wrong-account settings became known");
  const auto refused = settings.saveAutoSnapshot(input.account, false);
  ok &= check(refused.accepted && waitUntil([&] { return !settings.pendingTaskCount(); }) &&
                  !settings.known(input.account) && readObject(settingsPath) == wrongAccount,
              "JSON merge overwrote another account's settings");

  // Original account writes survive a source switch, but do not populate the
  // new account's history/settings view.
  ok &= check(writeObject(settingsPath, {{QStringLiteral("schema"), 1}, {QStringLiteral("account"), input.account},
      {QStringLiteral("autoSnapshot"), false}}), "settings repair fixture failed");
  settings.requestLoad(input.account);
  waitUntil([&] { return !settings.pendingTaskCount(); });
  savedSignal = false;
  const auto pendingSnapshot = store.write(input.account, overviewFor(repository));
  const auto pendingPreference = settings.saveAutoSnapshot(input.account, true);
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("snapshot-B")}}}});
  ok &= check(pendingSnapshot.accepted && pendingPreference.accepted && waitForRepositoryIdle(&repository) &&
                  waitStore(store) && waitUntil([&] { return !settings.pendingTaskCount(); }) &&
                  store.cachedHistory(QStringLiteral("snapshot-B")).isEmpty() &&
                  !settings.loadAutoSnapshot(QStringLiteral("snapshot-B")) &&
                  readObject(settingsPath).value(QStringLiteral("autoSnapshot")).toBool(),
              "late original-account save changed the new account's view or lost its frozen target");
  return ok;
}

bool historyPressure(const QString& root) {
  bool ok = true;
  const QString account = QStringLiteral("history-365");
  const QString directory = QDir(root).filePath(QStringLiteral("accounts/history-365/snapshots"));
  QJsonArray pets;
  for (int id = 1000; id < 3000; ++id)
    pets.append(QJsonObject{{QStringLiteral("instanceId"), QString::number(id)},
        {QStringLiteral("raceId"), 7152}, {QStringLiteral("name"), QStringLiteral("history-%1").arg(id)},
        {QStringLiteral("currentPower"), id}, {QStringLiteral("highestPower"), 4000},
        {QStringLiteral("completionPercent"), 50}, {QStringLiteral("fullyCultivated"), false},
        {QStringLiteral("redStarComplete"), false}, {QStringLiteral("astrolabeBreakthrough"), false},
        {QStringLiteral("currentPowerKnown"), true}, {QStringLiteral("cultivationKnown"), true},
        {QStringLiteral("redStarKnown"), true}, {QStringLiteral("astrolabeKnown"), true}});
  const QDate first = QDate::currentDate().addDays(-365);
  qint64 fixtureBytes = 0;
  for (int day = 0; day < 365; ++day) {
    const QDate date = first.addDays(day);
    QJsonObject object = historyObject(account, date, pets, day == 0 ? 1 : 2);
    if (day == 0) {
      QJsonArray legacyPets = pets;
      for (int index = 0; index < legacyPets.size(); ++index) {
        QJsonObject pet = legacyPets.at(index).toObject();
        for (const QString& key : {QStringLiteral("currentPowerKnown"), QStringLiteral("cultivationKnown"),
                                  QStringLiteral("redStarKnown"), QStringLiteral("astrolabeKnown")}) pet.remove(key);
        legacyPets.replace(index, pet);
      }
      object.insert(QStringLiteral("pets"), legacyPets);
    }
    fixtureBytes += QJsonDocument(object).toJson(QJsonDocument::Compact).size();
    ok &= check(writeObject(QDir(directory).filePath(date.toString(QStringLiteral("yyyy-MM-dd.json"))), object),
                "history pressure fixture write failed");
  }
  QJsonArray duplicate = pets;
  duplicate.replace(1, duplicate.first());
  ok &= check(writeObject(QDir(directory).filePath(QStringLiteral("duplicate.json")),
      historyObject(account, QDate::currentDate(), duplicate)), "duplicate history fixture failed");
  QJsonArray malformed = pets;
  QJsonObject invalidPet = malformed.first().toObject();
  invalidPet.insert(QStringLiteral("currentPower"), QStringLiteral("bad"));
  malformed.replace(0, invalidPet);
  ok &= check(writeObject(QDir(directory).filePath(QStringLiteral("malformed.json")),
      historyObject(account, QDate::currentDate(), malformed)), "malformed history fixture failed");

  qputenv("KQPET_DATA_ROOT", root.toUtf8());
  PetRepository repository;
  login(repository, account);
  AssetSnapshotStore store(&repository);
  ok &= check(waitStore(store, 60000), "365-day summary scan did not finish");
  const auto summaries = store.cachedHistory(account);
  const auto stats = store.cacheStats();
  int complete = 0;
  for (const auto& snapshot : summaries) if (snapshot.petsComplete) ++complete;
  ok &= check(summaries.size() == 365 && stats.fullSnapshots <= 3 && stats.fullBytes <= 16 * 1024 * 1024 &&
                  complete <= 3 && summaries.first().schemaVersion == 1,
              "history retained unbounded full pets or accepted malformed/duplicate records");
  const QString firstKey = summaries.first().storageKey;
  store.requestSnapshotDetails(account, firstKey);
  ok &= check(waitStore(store, 10000), "selected full snapshot did not load");
  const auto selected = store.cachedHistory(account);
  ok &= check(selected.first().petsComplete && selected.first().pets.size() == 2000 &&
                  !selected.first().pets.first().cultivationKnown && !selected.first().totalCurrentPowerKnown,
              "legacy optional known fields were invented or full snapshot could not be restored");
  store.requestInstanceHistory(account, 1000);
  ok &= check(waitStore(store, 60000), "selected instance history did not finish");
  const auto instance = store.cachedInstanceHistory(account, 1000);
  ok &= check(instance.size() == 365 && instance.first().present && instance.first().membershipKnown &&
                  instance.last().pet.instanceId == 1000 && store.cacheStats().fullSnapshots <= 3 &&
                  store.cacheStats().instanceEntries == 365,
              "instance history lost dates, identities, or its bounded full-cache policy");
  std::fprintf(stdout, "history fixture: days=365 pets_per_day=2000 serialized_bytes=%lld full_cache_bytes=%lld\n",
               static_cast<long long>(fixtureBytes), static_cast<long long>(store.cacheStats().fullBytes));
  return ok;
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir root;
  bool ok = check(root.isValid(), "temporary snapshot root unavailable");
  ok &= snapshotsAndSettings(QDir(root.path()).filePath(QStringLiteral("basic")));
  ok &= historyPressure(QDir(root.path()).filePath(QStringLiteral("pressure")));
  if (ok) std::puts("PASS: asynchronous snapshots/settings, source/version fences, JSON merge and bounded 365-day history");
  return ok ? 0 : 1;
}
