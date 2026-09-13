#include "../src/application/local_stargod_statistics_service.h"
#include "../src/domain/local_stargod_count.h"
#include "../src/storage/storage_service.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>

namespace {
bool check(bool value, const char* message) { if (!value) std::fprintf(stderr,"FAIL: %s\n",message); return value; }
QJsonObject definitions() {
  const auto star = [](int quality, bool changeable) { return QJsonObject{{QStringLiteral("quality"),quality},{QStringLiteral("changeable"),changeable},{QStringLiteral("type"),1}}; };
  return {{QStringLiteral("10"),star(6,false)},{QStringLiteral("11"),star(6,false)},
      {QStringLiteral("79"),star(5,true)},{QStringLiteral("80"),star(6,true)}};
}
QJsonObject envelope(qint64 id, const QJsonObject& pet, QString account = QStringLiteral("A"), bool complete = true, int schema = 4) {
  auto body = pet; body.insert(QStringLiteral("id"),QString::number(id));
  return {{QStringLiteral("schema"),schema},{QStringLiteral("account"),account},
      {QStringLiteral("trust"),QStringLiteral("read-only-observation")},{QStringLiteral("complete"),complete},
      {QStringLiteral("instanceId"),QString::number(id)},{QStringLiteral("observedAt"),QStringLiteral("2026-09-13T00:00:00Z")},{QStringLiteral("pet"),body}};
}
bool save(const QString& path, const QJsonObject& value) {
  QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QMap<QString,QByteArray> fingerprints(const QString& root) {
  QMap<QString,QByteArray> result;
  QDirIterator files(root,QDir::Files | QDir::Hidden,QDirIterator::Subdirectories);
  while (files.hasNext()) {
    QFile file(files.next()); if (file.open(QIODevice::ReadOnly)) result.insert(file.fileName(),QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256));
  }
  return result;
}
bool until(const std::function<bool()>& predicate) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < 10000) { QCoreApplication::processEvents(); QThread::msleep(1); }
  return predicate();
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;
  const auto metadata = definitions();
  const QJsonObject first{{QStringLiteral("sgs"),QStringLiteral("10:8#11:8#10:8:80#-2:8:80")},
      {QStringLiteral("sgsp"),QJsonArray{10,10,80,79,999}},
      {QStringLiteral("sppl"),QJsonObject{{QStringLiteral("id"),99},{QStringLiteral("sgs"),QStringLiteral("10:8")},{QStringLiteral("sgsp"),QJsonArray{10,10,80}}}}};
  auto counts = countLocalStargods(first,metadata);
  ok &= check(counts.ordinaryEquipped == 2 && counts.ordinaryBackpack == 2 && counts.changeableEquipped == 2 &&
      counts.changeableBackpack == 1 && counts.unknownEntries == 1 && counts.equippedKnown && !counts.backpackKnown,
      "physical duplicates, same-type stars, mapped changeable bases or unknown definitions were miscounted");
  counts = countLocalStargods({{QStringLiteral("sgs"),QStringLiteral("10:8")}},metadata);
  ok &= check(counts.ordinaryEquipped == 1 && !counts.backpackKnown && !counts.complete(),"missing backpack was treated as a confirmed zero");
  counts = countLocalStargods({{QStringLiteral("sgs"),QStringLiteral("10:8:999#0#-1#-2")},{QStringLiteral("sgsp"),QJsonArray{QJsonObject{{QStringLiteral("id"),10}}}}},metadata);
  ok &= check(counts.ordinaryEquipped == 0 && counts.unknownEntries == 2 && !counts.complete(),"unknown changeable source or unknown backpack shape was guessed");
  counts = countLocalStargods({{QStringLiteral("sgs"),QString()},{QStringLiteral("sgsp"),QJsonArray{}}},metadata);
  ok &= check(counts.complete() && counts.ordinaryEquipped == 0 && counts.changeableBackpack == 0,"explicit empty collections lost known zero state");

  QTemporaryDir temporary;
  ok &= check(temporary.isValid(),"temporary directory unavailable");
  StorageService storage(temporary.path()); const auto context = storage.createAccountContext(QStringLiteral("A"),QStringLiteral("A"));
  const auto details = QDir(context->directory()).filePath(QStringLiteral("details"));
  ok &= save(QDir(details).filePath(QStringLiteral("1.json")),envelope(1,first));
  auto duplicate = envelope(1,{{QStringLiteral("sgs"),QString()},{QStringLiteral("sgsp"),QJsonArray{}}});
  duplicate.insert(QStringLiteral("observedAt"),QStringLiteral("2020-01-01T00:00:00Z"));
  ok &= save(QDir(details).filePath(QStringLiteral("copy-1.json")),duplicate);
  ok &= save(QDir(details).filePath(QStringLiteral("2.json")),envelope(2,{{QStringLiteral("sgs"),QStringLiteral("10:1")},{QStringLiteral("sgsp"),QJsonArray{10,10,80}}},QStringLiteral("A"),true,3));
  ok &= save(QDir(details).filePath(QStringLiteral("3.json")),envelope(3,{{QStringLiteral("sgs"),QStringLiteral("10:8")}}));
  ok &= save(QDir(details).filePath(QStringLiteral("4.json")),envelope(4,first,QStringLiteral("A"),false));
  ok &= save(QDir(details).filePath(QStringLiteral("5.json")),envelope(5,first,QStringLiteral("B")));
  QFile malformed(QDir(details).filePath(QStringLiteral("bad.json"))); malformed.open(QIODevice::WriteOnly); malformed.write("{broken"); malformed.close();
  ok &= save(QDir(details).filePath(QStringLiteral("nested/6.json")),envelope(6,first));
  ok &= save(QDir(temporary.path()).filePath(QStringLiteral("accounts/B/details/7.json")),envelope(7,first,QStringLiteral("B")));
  const auto before = fingerprints(temporary.path());
  LocalStargodStatisticsService service(&storage);
  LocalStargodStatistics result; int updates = 0;
  QObject::connect(&service,&LocalStargodStatisticsService::updated,&service,[&](const LocalStargodStatistics& value) { result = value; ++updates; });
  ok &= check(service.request(context,11,metadata) && !service.request(context,11,metadata),"local request failed or duplicate parallel scan was admitted");
  ok &= check(until([&] { return !service.busy(); }) && result.completed && result.account == QStringLiteral("A") && result.epoch == 11 && updates >= 2,
      "offline disk-only scan did not finish with its frozen account/session");
  ok &= check(result.scannedFiles == 7 && result.countedPets == 3 && result.duplicatePets == 1 && result.skippedFiles == 3 &&
      result.incompletePets == 2 && result.unknownEntries == 1 && result.ordinaryEquipped == 4 && result.ordinaryBackpack == 4 &&
      result.changeableEquipped == 2 && result.changeableBackpack == 2,
      "disk scan lost unloaded instances, included other accounts/nested pets, or counted stale duplicate files twice");
  ok &= check(before == fingerprints(temporary.path()),"read-only statistics changed cache files");
  ok &= check(service.request(context,11,metadata),"second explicit local scan was rejected"); service.cancel();
  ok &= check(until([&] { return !service.busy(); }) && result.cancelled && !result.completed,"cancel before I/O start left the statistics service busy");
  const auto other = storage.createAccountContext(QStringLiteral("B"),QStringLiteral("B"));
  ok &= check(service.request(context,11,metadata) && service.request(other,12,metadata),
      "new account could not replace the previous scan during cancellation");
  ok &= check(until([&] { return !service.busy(); }) && result.account == QStringLiteral("B") && result.epoch == 12 &&
      result.completed && result.countedPets == 1,"replaced account scan left the UI waiting or published old-account totals");
  service.close();
  ok &= check(!service.request(context,11,metadata),"closed service accepted another scan");
  storage.shutdown();
  if (ok) std::puts("PASS: offline disk-only local red-star counts, physical duplicates, changeable source, unknown fields, account isolation and cancellation");
  return ok ? 0 : 1;
}
