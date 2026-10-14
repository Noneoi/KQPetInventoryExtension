#include "application/catalog/public_dataset_store.h"
#include "domain/pet_metadata_view.h"
#include "domain/public_data_view.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#endif

namespace {
struct ProcessSample {
  quint64 cpu100ns = 0;
  quint64 workingSet = 0, peakWorkingSet = 0, privateBytes = 0;
  quint64 readBytes = 0, writeBytes = 0, readCalls = 0, writeCalls = 0;
  bool available = false;
};
ProcessSample processSample() {
  ProcessSample result;
#ifdef Q_OS_WIN
  const auto process = GetCurrentProcess();
  FILETIME created{}, ended{}, kernel{}, user{};
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  IO_COUNTERS io{};
  if (!GetProcessTimes(process, &created, &ended, &kernel, &user) ||
      !GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) ||
      !GetProcessIoCounters(process, &io)) return result;
  const auto ticks = [](FILETIME value) { return (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
  result.cpu100ns = ticks(kernel) + ticks(user);
  result.workingSet = memory.WorkingSetSize; result.peakWorkingSet = memory.PeakWorkingSetSize;
  result.privateBytes = memory.PrivateUsage;
  result.readBytes = io.ReadTransferCount; result.writeBytes = io.WriteTransferCount;
  result.readCalls = io.ReadOperationCount; result.writeCalls = io.WriteOperationCount;
  result.available = true;
#endif
  return result;
}
double milliseconds(qint64 ns) { return double(ns) / 1000000.0; }
QString argument(const QStringList& arguments, const QString& key, const QString& fallback = {}) {
  const auto index = arguments.indexOf(key);
  return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : fallback;
}
QString query(PublicDatasetSnapshotHandle snapshot, int count, quint64* lookups) {
  PublicDataView skills(PublicDataViewKind::PetSkills, snapshot->skill->root);
  PublicDataView replay(PublicDataViewKind::BattleReplay, snapshot->replay->root);
  PetMetadataView pets(snapshot->detail);
  const auto petIds = snapshot->detail->root.value("pets").toObject().keys();
  const auto skillIds = snapshot->skill->root.value("skills").toObject().keys();
  const auto buffIds = snapshot->replay->root.value("buffs").toObject().keys();
  const auto replayIds = snapshot->replay->root.value("skills").toObject().keys();
  if (petIds.isEmpty() || skillIds.isEmpty() || buffIds.isEmpty() || replayIds.isEmpty()) return {};
  QCryptographicHash digest(QCryptographicHash::Sha256);
  for (int i = 0; i < count; ++i) {
    const QString texts[] = {
      pets.originalName(petIds.at(i % petIds.size()).toInt()),
      skills.object(QStringLiteral("skills"), skillIds.at(i % skillIds.size())).value("description").toString(),
      replay.object(QStringLiteral("buffs"), buffIds.at(i % buffIds.size())).value("nm").toString(),
      replay.object(QStringLiteral("skills"), replayIds.at(i % replayIds.size())).value("name").toString()};
    for (const auto& text : texts) { digest.addData(text.toUtf8()); digest.addData(QByteArrayView("\0", 1)); }
  }
  *lookups += quint64(count) * 4;
  return QString::fromLatin1(digest.result().toHex());
}
}

// Dedicated measurement adapter over the production store. This executable
// was not part of the installed second-round release. All callers must provide
// an isolated fixture root; it is intentionally not a normal CTest operation.
int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const auto args = application.arguments();
  const auto root = argument(args, QStringLiteral("--root"));
  const auto operation = argument(args, QStringLiteral("--operation"));
  const auto output = argument(args, QStringLiteral("--report"));
  bool valid = false;
  const int iterations = argument(args, QStringLiteral("--iterations"), QStringLiteral("1")).toInt(&valid);
  if (root.isEmpty() || output.isEmpty() || !valid || iterations < 1 || iterations > 100 ||
      !QStringList{"current", "replay", "query", "restore", "candidate"}.contains(operation) ||
      !args.contains(QStringLiteral("--isolated-fixture"))) {
    std::fprintf(stderr, "Require --isolated-fixture --root <isolatedRoot> --operation current|replay|query|restore|candidate --report <JSON> [--iterations N] [--attempt ID] [--revision SHA] [--commit] [--hold-replay]\n");
    return 2;
  }
  const int queryRounds = std::clamp(argument(args, QStringLiteral("--query-rounds"), QStringLiteral("10000")).toInt(), 1, 1000000);
  auto phase = std::make_shared<std::atomic_int>(0);
  QJsonObject report;
  std::vector<qint64> heartbeats;
  QElapsedTimer pulseClock; pulseClock.start(); qint64 lastPulse = 0;
  QTimer heartbeat; heartbeat.setTimerType(Qt::PreciseTimer); heartbeat.setInterval(5);
  QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] {
    const auto now = pulseClock.nsecsElapsed();
    if (phase->load() == 1) heartbeats.push_back(now - lastPulse);
    lastPulse = now;
  });
  auto* worker = QThread::create([&] {
    PublicDatasetStore store(root);
    PublicDatasetSnapshotHandle held;
    PublicDatasetResult result;
    QElapsedTimer setup; setup.start();
    if (operation != QStringLiteral("current")) {
      result = store.prepareCurrent(false);
      if (result.ok()) held = result.snapshot;
      if (held && (operation == QStringLiteral("query") || args.contains(QStringLiteral("--hold-replay")))) {
        result = store.loadReplay(held); if (result.ok()) held = result.snapshot;
      }
      if (!result.ok()) {
        report = {{"passed", false}, {"phase", "setup"}, {"error", result.error}}; return;
      }
    }
    const auto setupNs = setup.nsecsElapsed();
    const auto before = processSample();
    QElapsedTimer measured; measured.start(); phase->store(1);
    QJsonArray samples;
    QString queryDigest;
    quint64 lookupCount = 0;
    for (int i = 0; i < iterations; ++i) {
      QElapsedTimer iteration; iteration.start();
      double prepareMs = 0, commitMs = 0;
      if (operation == QStringLiteral("current")) result = store.prepareCurrent(false);
      else if (operation == QStringLiteral("replay")) result = store.loadReplay(held);
      else if (operation == QStringLiteral("query")) {
        queryDigest = query(held, queryRounds, &lookupCount);
        result.status = queryDigest.isEmpty() ? PublicDatasetStatus::Invalid : PublicDatasetStatus::Ready;
        result.snapshot = held;
      } else {
        result = operation == QStringLiteral("restore")
            ? store.prepareRestoreRevision(argument(args, QStringLiteral("--revision"), held->revision))
            : store.prepareCandidate(argument(args, QStringLiteral("--attempt")));
        prepareMs = milliseconds(iteration.nsecsElapsed());
        if (result.ok() && result.prepared && (operation == QStringLiteral("restore") || args.contains(QStringLiteral("--commit")))) {
          QElapsedTimer committing; committing.start(); result = store.commitPrepared(result.prepared);
          commitMs = milliseconds(committing.nsecsElapsed());
        }
      }
      samples.append(QJsonObject{{"wallMs", milliseconds(iteration.nsecsElapsed())}, {"prepareMs", prepareMs}, {"commitMs", commitMs},
          {"status", PublicDatasetStore::statusName(result.status)}, {"revision", result.snapshot ? result.snapshot->revision : QString{}}});
      if (!result.ok()) break;
    }
    const auto elapsed = measured.nsecsElapsed(); phase->store(2);
    const auto after = processSample();
    report = {{"schema", 1}, {"passed", result.ok()}, {"operation", operation}, {"iterationsRequested", iterations},
        {"iterationsCompleted", samples.size()}, {"samples", samples}, {"setupWallMsExcluded", milliseconds(setupNs)},
        {"wallMs", milliseconds(elapsed)}, {"error", result.error}, {"revision", result.snapshot ? result.snapshot->revision : QString{}},
        {"queryDigest", queryDigest}, {"lookups", qint64(lookupCount)}, {"measurementKind", "dedicated-native-store-adapter"},
        {"measurementRoot", root}, {"networkPerformed", false}, {"corePublicationPerformed", false},
        {"processCountersAvailable", before.available && after.available}};
    if (before.available && after.available) {
      report.insert("cpuMs", double(after.cpu100ns - before.cpu100ns) / 10000.0);
      report.insert("workingSetBeforeBytes", qint64(before.workingSet)); report.insert("workingSetAfterBytes", qint64(after.workingSet));
      report.insert("peakWorkingSetBytesWholeProcess", qint64(after.peakWorkingSet));
      report.insert("peakWorkingSetIncreaseDuringOperationBytes", qint64(after.peakWorkingSet - before.peakWorkingSet));
      report.insert("privateBytesBefore", qint64(before.privateBytes)); report.insert("privateBytesAfter", qint64(after.privateBytes));
      report.insert("logicalIoReadBytes", qint64(after.readBytes - before.readBytes));
      report.insert("logicalIoWriteBytes", qint64(after.writeBytes - before.writeBytes));
      report.insert("logicalIoReadCalls", qint64(after.readCalls - before.readCalls));
      report.insert("logicalIoWriteCalls", qint64(after.writeCalls - before.writeCalls));
    }
  });
  QObject::connect(worker, &QThread::finished, &application, &QCoreApplication::quit);
  heartbeat.start(); worker->start(); application.exec(); worker->wait(); delete worker;
  std::sort(heartbeats.begin(), heartbeats.end());
  report.insert("mainLoopHeartbeat", QJsonObject{{"proxyOnly", true}, {"targetIntervalMs", 5}, {"samples", qsizetype(heartbeats.size())},
      {"maxGapMs", heartbeats.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(milliseconds(heartbeats.back()))},
      {"p95GapMs", heartbeats.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(milliseconds(heartbeats[(heartbeats.size() - 1) * 95 / 100]))},
      {"scope", "Qt main event loop while native I/O runs on a worker; not a live game UI measurement"}});
  QSaveFile file(output);
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
    std::fprintf(stderr, "Cannot write benchmark report\n"); return 3;
  }
  std::printf("%s: %s, wall=%.3f ms\n", qPrintable(operation), report.value("passed").toBool() ? "passed" : "failed", report.value("wallMs").toDouble());
  return report.value("passed").toBool() ? 0 : 1;
}
