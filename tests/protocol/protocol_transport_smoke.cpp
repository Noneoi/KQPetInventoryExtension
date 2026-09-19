#include "protocol/protocol_transport.h"
#include "application/pet/pet_refresh_controller.h"
#include "application/pet/pet_repository.h"
#include "support/protocol_test_support.h"
#include "domain/pet_move_policy.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <thread>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
bool waitUntil(const std::function<bool()>& predicate, int timeout = 2500) {
  QElapsedTimer clock; clock.start();
  while (!predicate() && clock.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return predicate();
}
SessionSourceEvidence source() {
  return {QStringLiteral("fixture-account"), QStringLiteral("fixture-page"), 1, true, true,
          QStringLiteral("synthetic ordered host source")};
}
OutboundIntent request(bool write = false) {
  OutboundIntent intent;
  intent.ticket = {nextTransportTaskId(), source().account, 1,
      write ? QStringLiteral("2_1_11") : QStringLiteral("2_1_10"), 0, 5000, 500,
      PacketCorrelationStrength::CommandObserved};
  intent.source = source();
  intent.extension = QStringLiteral("PJXExtension");
  intent.parameters = write ? QStringLiteral("{\"pps\":\"1#2\",\"ppt\":0}") : QStringLiteral("{}");
  intent.write = write; intent.preflightRevision = write ? 7 : 0;
  return intent;
}
ActualSendSource actual() { return {source(), 1, 7, false, true, true}; }

bool primitiveTests() {
  bool ok = true;
  OutboundQueue queue(2, 4096);
  auto first = request(), second = request(), third = request();
  ok &= check(queue.tryPush(first) && queue.tryPush(second) && !queue.tryPush(third), "outbound queue exceeded admission capacity");
  auto taken = queue.takeNext();
  ok &= check(taken && queue.outstanding() == 2 && !queue.tryPush(third), "taken intent escaped capacity accounting");
  std::thread revoker([&] { queue.revokeAll(); }); revoker.join();
  int calls = 0;
  const auto submit = [&](const OutboundIntent&) { ++calls; return SubmissionOutcome::Submitted; };
  const auto cancelled = executeIntent(*taken, actual(), 1000, submit);
  ok &= check(cancelled.outcome == SubmissionOutcome::DefinitelyNotSubmitted && calls == 0,
              "independent cancellation did not revoke taken-but-unclaimed intent");
  queue.complete(first.ticket.taskId); queue.complete(second.ticket.taskId);
  ok &= check(queue.outstanding() == 0 && queue.tryPush(third), "completed admission was not released");
  queue.revokeAll(); queue.complete(third.ticket.taskId);

  auto write = request(true);
  const auto sent = executeIntent(write, actual(), 1200, [&](const OutboundIntent& active) {
    ++calls;
    ok &= check(active.permit.revoke() == SubmissionOutcome::Unknown,
                "claimed write was incorrectly confirmed not submitted");
    return SubmissionOutcome::Submitted;
  });
  ok &= check(sent.dispatchedAtMs == 1200 && sent.responseDeadlineMs == 1700 &&
              remainingResponseMs(sent, 1900) == 0, "late receipt extended the actual response deadline");
  const int beforeDuplicate = calls;
  ok &= check(executeIntent(write, actual(), 1300, submit).outcome == SubmissionOutcome::Unknown && calls == beforeDuplicate,
              "duplicate execution bypassed atomic permit claim");
  for (int fault = 0; fault < 6; ++fault) {
    auto intent = request(true); auto evidence = actual();
    if (fault == 0) evidence.source = {};
    if (fault == 1) ++evidence.sessionEpoch;
    if (fault == 2) ++evidence.preflightRevision;
    if (fault == 3) evidence.closing = true;
    if (fault == 4) evidence.orderedSubmission = false;
    if (fault == 5) intent.write = false;
    const int before = calls;
    ok &= check(executeIntent(intent, evidence, 1000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted && calls == before,
                "unverified/changed source, stale preflight or forged write flag reached host");
  }
  auto expired = request();
  ActualSendSource unverifiedHost;
  unverifiedHost.allowUnverifiedRead = true;
  auto readObservation = request(); readObservation.source = {};
  const int beforeRead = calls;
  const auto observed = executeIntent(readObservation, unverifiedHost, 1000, submit);
  ok &= check(observed.outcome == SubmissionOutcome::Submitted && calls == beforeRead + 1 &&
                  !readObservation.source.verified(), "explicit weak read did not preserve unverified provenance");
  auto noDowngrade = request();
  ok &= check(executeIntent(noDowngrade, unverifiedHost, 1000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted,
              "verified request silently downgraded to weak source");
  auto weakWrite = request(true); weakWrite.source = {};
  ok &= check(executeIntent(weakWrite, unverifiedHost, 1000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted,
              "weak read capability authorized a write");
  // v1.3-level move: an explicit write capability for a source-free intent
  // that Core preflighted. It cannot stand in for verified evidence, and a
  // write without a preflight revision is still refused.
  ActualSendSource moveHost = unverifiedHost; moveHost.allowUnverifiedWrite = true;
  const int beforeWeakWrite = calls;
  auto preflightedWrite = request(true); preflightedWrite.source = {};
  ok &= check(executeIntent(preflightedWrite, moveHost, 1000, submit).outcome == SubmissionOutcome::Submitted &&
                  calls == beforeWeakWrite + 1,
              "explicit read-continuity write capability did not submit a preflighted move");
  auto unpreflighted = request(true); unpreflighted.source = {}; unpreflighted.preflightRevision = 0;
  ok &= check(executeIntent(unpreflighted, moveHost, 1000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted,
              "a write without a Core preflight revision reached the host");
  auto verifiedWrite = request(true);
  ok &= check(executeIntent(verifiedWrite, moveHost, 1000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted,
              "a verified-source write was downgraded to the read-continuity level");
  auto weakReadViaMoveHost = request(); weakReadViaMoveHost.source = {};
  weakReadViaMoveHost.write = true;
  ok &= check(executeIntent(weakReadViaMoveHost, moveHost, 1000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted &&
                  calls == beforeWeakWrite + 1,
              "a read command forged as a write passed the write capability");
  ok &= check(executeIntent(expired, actual(), 5000, submit).outcome == SubmissionOutcome::DefinitelyNotSubmitted,
              "expired pending intent reached host");
  auto queued = request();
  ok &= check(!pollSendReceipt(queued, 4000), "response timeout started while intent was only queued");
  const auto lateDispatch = executeIntent(queued, actual(), 4100, submit);
  ok &= check(lateDispatch.responseDeadlineMs == 4600 && remainingResponseMs(lateDispatch, 4700) == 0,
              "dispatch-based deadline did not survive delayed delivery");
  auto racing = request(true);
  std::atomic_bool go{false}; std::atomic<int> winners{0};
  const auto concurrent = [&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    executeIntent(racing, actual(), 1000, [&](const OutboundIntent&) {
      winners.fetch_add(1); return SubmissionOutcome::Submitted;
    });
  };
  std::thread a(concurrent), b(concurrent);
  go.store(true, std::memory_order_release); a.join(); b.join();
  ok &= check(winners.load() == 1, "concurrent executors submitted the same intent twice");
  return ok;
}

bool controllerTests() {
  QTemporaryDir directory;
  qputenv("KQPET_DATA_ROOT", directory.path().toUtf8());
  PetRepository repository;
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("transport-fixture")}}}});
  PetRefreshController controller(&repository);
  QString lastMoveStatus;
  QObject::connect(&controller,&PetRefreshController::statusChanged,&controller,
      [&](const QString& status) { lastMoveStatus = status; });
  PetRefreshController::Timings timing;
  timing.automaticIntervalMs = 100000;
  timing.listRequestGapMs = 1; timing.listTimeoutMs = 1000;
  timing.detailRequestGapMs = 0; timing.detailTimeoutMs = 1000; timing.detailMaxRetries = 0;
  timing.moveRequestTimeoutMs = 250;
  controller.setTimings(timing);
  QList<OutboundIntent> held;
  controller.setAsyncSender([&](const OutboundIntent& intent) { held.append(intent); return true; });
  controller.requestManualListRefresh();
  bool ok = check(held.size() == 1 && held.first().ticket.command == QStringLiteral("2_1_10"),
                  "warehouse gap ran before actual backpack dispatch");
  if (held.isEmpty()) return false;
  const auto evidence = [&repository] {
    return ActualSendSource{repository.sessionContext().source, repository.sessionGeneration(),
                             repository.inventoryRevision(), false, true, true};
  };
  auto late = executeIntent(held.takeFirst(), evidence(), transportMonotonicMs() - 2000,
      [](const OutboundIntent&) { return SubmissionOutcome::Submitted; });
  controller.handleSendReceipt(late);
  ok &= check(waitUntil([&] { return held.size() == 1; }), "actual dispatch did not schedule warehouse gap");
  if (!held.isEmpty()) {
    late = executeIntent(held.takeFirst(), evidence(), transportMonotonicMs() - 2000,
        [](const OutboundIntent&) { return SubmissionOutcome::Submitted; });
    controller.handleSendReceipt(late);
  }
  ok &= check(!controller.listRefreshRunning(), "late list receipt restarted a full response timeout");

  QList<qint64> backpack{1, 2}; QSet<qint64> warehouse{3};
  const auto pet = [](qint64 id) { return QJsonObject{{QStringLiteral("id"), QString::number(id)},
      {QStringLiteral("ri"), 7000 + int(id)}, {QStringLiteral("lv"), 100}, {QStringLiteral("n"), QString::number(id)}}; };
  std::optional<OutboundIntent> pendingWrite;
  std::optional<SendReceipt> ambiguousReceipt;
  bool claimAmbiguousAtDispatch = false;
  int ambiguousSubmissions = 0;
  int socketWrites = 0;
  controller.setAsyncSender([&](const OutboundIntent& intent) {
    if (intent.write) {
      if (claimAmbiguousAtDispatch) {
        // Claim while this preflight is current. Deliver the receipt later;
        // unrelated cache reads may change the inventory revision meanwhile.
        ambiguousReceipt = executeIntent(intent,evidence(),transportMonotonicMs(),[&](const OutboundIntent& active) {
          ++ambiguousSubmissions;
          if (active.ticket.command != QStringLiteral("batchpet")) ++socketWrites;
          return SubmissionOutcome::Unknown;
        });
      } else pendingWrite = intent;
      return true;
    }
    const auto receipt = executeIntent(intent, evidence(), transportMonotonicMs(), [&](const OutboundIntent& active) {
      QJsonArray pets;
      if (active.ticket.command == QStringLiteral("2_1_10")) {
        for (qint64 id : backpack) pets.append(pet(id));
        deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), active.ticket.command},
            {QStringLiteral("pl"), pets}, {QStringLiteral("pps"), QJsonArray{PetMovePolicy::serializeSequence(backpack)}},
            {QStringLiteral("ppc"), 12}});
      } else if (active.ticket.command == QStringLiteral("2_1_S")) {
        for (qint64 id : warehouse) pets.append(pet(id));
        deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), active.ticket.command},
            {QStringLiteral("ns"), pets}, {QStringLiteral("rb"), QJsonArray{}}, {QStringLiteral("es"), QJsonArray{}}});
      }
      return SubmissionOutcome::Submitted;
    });
    controller.handleSendReceipt(receipt);
    return true;
  });
  controller.requestMoveToWarehouse(2);
  ok &= check(waitUntil([&] { return pendingWrite.has_value(); }), "async move never reached WaitingDispatch after saved intent");
  if (!pendingWrite) return false;
  const quint64 task = controller.currentMoveTaskId();
  controller.cancelMove();
  const auto revoked = executeIntent(*pendingWrite, evidence(), transportMonotonicMs(), [&](const OutboundIntent&) {
    ++socketWrites; return SubmissionOutcome::Submitted;
  });
  ok &= check(task != 0 && controller.currentMoveTaskId() == 0 &&
              revoked.outcome == SubmissionOutcome::DefinitelyNotSubmitted && socketWrites == 0 &&
              controller.lastMoveOutcome() == MoveOutcome::NotSent,
              "cancelled unclaimed move was sent or retained dialog task identity");
  pendingWrite.reset();
  ok &= check(waitForRepositoryIdle(&repository), "cancelled move fixture storage did not settle");
  controller.requestMoveToWarehouse(2);
  ok &= check(waitUntil([&] { return pendingWrite.has_value(); }), "second async move did not reach dispatch");
  if (!pendingWrite) return false;
  ok &= check(controller.currentMoveTaskId() > task, "same-account new move reused the prior dialog identity");
  pendingWrite->permit.claim(transportMonotonicMs());
  controller.cancelMove();
  ok &= check(controller.lastMoveOutcome() == MoveOutcome::Unknown && socketWrites == 0,
              "claimed pending move was incorrectly reported NotSent");
  pendingWrite.reset();
  ok &= check(waitForRepositoryIdle(&repository), "claimed move fixture storage did not settle");
  claimAmbiguousAtDispatch = true;
  controller.requestMoveToWarehouse(2);
  ok &= check(waitUntil([&] { return ambiguousReceipt.has_value(); }), "unknown async move did not reach dispatch");
  if (!ambiguousReceipt) {
    std::fprintf(stderr,"MOVE: %s\n",lastMoveStatus.toUtf8().constData());
    return false;
  }
  const auto ambiguous = *ambiguousReceipt;
  controller.handleSendReceipt(ambiguous);
  ok &= check(waitUntil([&] { return !controller.moveRunning(); }) && !pendingWrite &&
              ambiguous.outcome == SubmissionOutcome::Unknown && ambiguousSubmissions == 1 &&
              controller.lastMoveOutcome() == MoveOutcome::Unknown && socketWrites == 0,
              "Unknown async outcome retried fallback or skipped read-only reconciliation");
  if (!ok) std::fprintf(stderr,"MOVE: outcome=%d receipt=%d pending=%d running=%d status=%s\n",
      int(controller.lastMoveOutcome()),int(ambiguous.outcome),pendingWrite.has_value(),controller.moveRunning(),
      lastMoveStatus.toUtf8().constData());
  return ok;
}

bool repeatedWeakLoginTests() {
  QTemporaryDir directory;
  qputenv("KQPET_DATA_ROOT", directory.path().toUtf8());
  PetRepository repository;
  PetRefreshController controller(&repository);
  PetRefreshController::Timings timing;
  timing.automaticIntervalMs = 100000;
  timing.listRequestGapMs = 0; timing.listTimeoutMs = 1000;
  timing.detailRequestGapMs = 0; timing.detailTimeoutMs = 1000; timing.detailMaxRetries = 0;
  controller.setTimings(timing); // a local shared preference, never an account fact
  const QJsonObject login{{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("weak-live-login")}}}};
  const auto deliverWeak = [&repository](const QJsonObject& packet) {
    InboundEnvelope envelope;
    envelope.receiveSequence = repository.lastInboundSequence() + 1;
    envelope.receivedMonotonicMs = transportMonotonicMs();
    envelope.method = QStringLiteral("recivedata");
    envelope.payload = QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
    // Exactly like OriginalBridge: no captured account, epoch, source or order
    // is borrowed from the currently selected Core account.
    repository.handleEnvelope(envelope);
  };
  // Real accounts keep far more than one 12-pet UI page in the backpack; the
  // field failure that a 1-pet fixture hid was a 78-pet move sequence.
  QList<qint64> backpackIds{1};
  for (qint64 id = 1001; id <= 1076; ++id) backpackIds.append(id);
  const auto backpackPacket = [](const QList<qint64>& ids) {
    QJsonArray pets;
    for (qint64 id : ids)
      pets.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("r"), 7000 + (id % 900)},
                              {QStringLiteral("lv"), 100},
                              {QStringLiteral("n"), id == 2 ? QStringLiteral("observed warehouse")
                                                            : QStringLiteral("observed backpack")}});
    return QJsonObject{{QStringLiteral("_cmd"), QStringLiteral("2_1_10")}, {QStringLiteral("pl"), pets},
        {QStringLiteral("pps"), QJsonArray{PetMovePolicy::serializeSequence(ids), QJsonValue::Null}},
        {QStringLiteral("ppc"), 100}};
  };
  const QJsonObject backpack = backpackPacket(backpackIds);
  const QJsonObject warehouse{{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
      {QStringLiteral("ns"), QJsonArray{QJsonObject{{QStringLiteral("id"), 2},
          {QStringLiteral("ri"), 7002}, {QStringLiteral("lv"), 100}, {QStringLiteral("n"), QStringLiteral("observed warehouse")}}}},
      {QStringLiteral("rb"), QJsonArray{}}, {QStringLiteral("es"), QJsonArray{}}};

  deliverWeak(login);
  const quint64 firstEpoch = repository.sessionGeneration();
  bool ok = check(repository.isAuthenticated() && firstEpoch != 0 && !repository.sessionContext().canPersist(),
                  "initial real-shaped unverified login did not establish a RAM-only read session");
  // Both notifications may already be in the same GUI capture batch. No event
  // pumping or projected epoch round-trip occurs between these two packets.
  deliverWeak(login);
  const quint64 batchEpoch = repository.sessionGeneration();
  ok &= check(repository.isAuthenticated() && batchEpoch > firstEpoch &&
                  !repository.sessionContext().source.verified() && !repository.sessionContext().canPersist(),
              "consecutive source-free logins captured at epoch zero disabled RAM-only reading");
  QList<OutboundIntent> oldIntents;
  controller.setAsyncSender([&](const OutboundIntent& intent) { oldIntents.append(intent); return true; });
  controller.requestManualListRefresh();
  ok &= check(oldIntents.size() == 1, "first weak login could not queue manual read");
  deliverWeak(backpack);
  ok &= check(repository.backpackPets().size() == 77, "first weak observation was not visible before renewal");
  deliverWeak(login);
  const quint64 secondEpoch = repository.sessionGeneration();
  ok &= check(repository.isAuthenticated() && secondEpoch > batchEpoch &&
                  repository.accountKey() == QStringLiteral("weak-live-login") &&
                  repository.sessionContext().state == SessionConnectionState::Uncertain &&
                  !repository.sessionContext().source.verified() && !repository.sessionContext().canPersist() &&
                  repository.backpackPets().isEmpty() && !controller.listRefreshRunning(),
              "same-account duplicate weak login revoked reading or promoted previous weak facts");
  ActualSendSource weakHost; weakHost.allowUnverifiedRead = true;
  int oldHostCalls = 0;
  for (const auto& intent : oldIntents)
    executeIntent(intent, weakHost, transportMonotonicMs(), [&](const OutboundIntent&) {
      ++oldHostCalls; return SubmissionOutcome::Submitted;
    });
  ok &= check(oldHostCalls == 0, "new local weak epoch did not revoke the prior queued intent");
  deliverWeak(backpack);
  ok &= check(repository.backpackPets().isEmpty(), "renewed weak epoch retained an old list expectation");

  int listReads = 0, detailReads = 0, writes = 0;
  bool moved = false;  // the simulated server state after an accepted move
  const QList<qint64> movedIds = PetMovePolicy::append(backpackIds, 2);
  const QJsonObject movedBackpack = backpackPacket(movedIds);
  const QJsonObject movedWarehouse{{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
      {QStringLiteral("ns"), QJsonArray{}}, {QStringLiteral("rb"), QJsonArray{}}, {QStringLiteral("es"), QJsonArray{}}};
  bool detailFinished = false, detailSucceeded = false, repositorySaved = false;
  QObject::connect(&repository, &PetRepository::detailResponseAccepted, &repository,
                   [&](qint64, quint64) { repositorySaved = true; });
  QObject::connect(&controller, &PetRefreshController::detailRequestFinished, &controller,
                   [&](qint64 id, bool succeeded, const QString&) { if (id == 2) { detailFinished = true; detailSucceeded = succeeded; } });
  controller.setAsyncSender([&](const OutboundIntent& intent) {
    ok &= check(!intent.source.verified() && intent.ticket.sessionEpoch == repository.sessionGeneration(),
                "weak controller intent acquired source evidence or used an old local epoch");
    const auto receipt = executeIntent(intent, weakHost, transportMonotonicMs(), [&](const OutboundIntent& active) {
      if (active.write) {
        ++writes; moved = true;
        deliverWeak({{QStringLiteral("_cmd"), QStringLiteral("2_1_11")}, {QStringLiteral("r"), 1}});
        return SubmissionOutcome::Submitted;
      }
      if (active.ticket.command == QStringLiteral("2_1_10")) { ++listReads; deliverWeak(moved ? movedBackpack : backpack); }
      else if (active.ticket.command == QStringLiteral("2_1_S")) { ++listReads; deliverWeak(moved ? movedWarehouse : warehouse); }
      else if (active.ticket.command == QStringLiteral("2_1_R")) {
        ++detailReads;
        deliverWeak({{QStringLiteral("_cmd"), active.ticket.command},
            {QStringLiteral("p"), QJsonObject{{QStringLiteral("id"), 2}, {QStringLiteral("r"), 7002},
                {QStringLiteral("lv"), 100}, {QStringLiteral("zdl"), 12345}}}});
      }
      return SubmissionOutcome::Submitted;
    });
    controller.handleSendReceipt(receipt);
    return true;
  });
  controller.requestManualListRefresh();
  ok &= check(waitUntil([&] { return listReads == 2 && !controller.listRefreshRunning(); }) &&
                  repository.backpackPets().size() == 77 && repository.warehousePets().size() == 1 &&
                  repository.backpackPet(1).value(QStringLiteral("_unverifiedObservation")).toBool() &&
                  repository.warehousePet(2).value(QStringLiteral("_unverifiedObservation")).toBool(),
              "two weak logins did not allow Controller -> Transport -> Repository manual inventory reading");
  controller.requestSingleDetail(2);
  ok &= check(waitUntil([&] { return detailFinished; }) && waitForRepositoryIdle(&repository) &&
                  detailReads == 1 && detailSucceeded && repositorySaved &&
                  repository.detailFor(2).value(QStringLiteral("zdl")).toInt() == 12345 &&
                  repository.detailFor(2).value(QStringLiteral("_unverifiedObservation")).toBool() &&
                  repository.isDetailPersisted(2),
              "read-only detail was invisible, retried after valid observation, or not durably cached");
  // Moving at the v1.3 level: the forced preflight re-reads both lists
  // through this uninterrupted stream, which is enough for Core to queue the
  // write. The host still refuses it unless it grants the explicit write
  // capability that production grants to a compatible, healthy client.
  bool moveSucceeded = false; QString moveMessage; int moveFinishedCount = 0;
  QObject::connect(&controller, &PetRefreshController::moveFinished, &controller,
                   [&](bool succeeded, const QString& message) {
                     ++moveFinishedCount; moveSucceeded = succeeded; moveMessage = message; });
  controller.requestMoveToBackpack(2);
  ok &= check(waitUntil([&] { return moveFinishedCount == 1; }) && !moveSucceeded && writes == 0 &&
                  !moved && repository.readContinuityWriteAllowed() && !repository.sessionContext().canPersist(),
              "a host without the explicit write capability submitted a move");
  weakHost.allowUnverifiedWrite = true;
  controller.requestMoveToBackpack(2);
  ok &= check(waitUntil([&] { return moveFinishedCount == 2; }) && moveSucceeded && writes == 1 &&
                  movedIds.size() == 78 && repository.backpackIds() == movedIds && repository.warehousePet(2).isEmpty() &&
                  !repository.sessionContext().canPersist(),
              qPrintable(QStringLiteral("unverified but uninterrupted session did not complete exactly one "
                                        "verified move: %1").arg(moveMessage)));
  ok &= check(waitForRepositoryIdle(&repository), "weak read test did not drain actual repository I/O");
  QDirIterator accountFiles(QDir(directory.path()).filePath(QStringLiteral("accounts")),
                           QStringList{QStringLiteral("*.json")},
                           QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
  int savedObservations = 0, moveJournals = 0;
  while (accountFiles.hasNext()) {
    const QString path = accountFiles.next();
    QFile cached(path);
    ok &= check(cached.open(QIODevice::ReadOnly), "read-only cache could not be read back");
    const auto envelope = QJsonDocument::fromJson(cached.readAll()).object();
    if (QFileInfo(path).dir().dirName() == QStringLiteral("operations")) {
      // Move intent journals, one per attempted move; not observation caches.
      ok &= check(envelope.value(QStringLiteral("schema")).toInt() == 1 &&
                      envelope.value(QStringLiteral("account")).toString() == QStringLiteral("weak-live-login"),
                  "move journal was written for another account or schema");
      ++moveJournals;
      continue;
    }
    ok &= check(envelope.value(QStringLiteral("schema")).toInt() == 4 &&
                    envelope.value(QStringLiteral("account")).toString() == QStringLiteral("weak-live-login") &&
                    envelope.value(QStringLiteral("trust")).toString() == QStringLiteral("read-only-observation"),
                "cached read-only data was promoted to a verified observation");
    ++savedObservations;
  }
  ok &= check(moveJournals == 2, "each attempted read-continuity move did not leave exactly one intent journal");
  ok &= check(savedObservations >= 2 && QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("last-account.txt"))),
              "read-only observations or offline account hint were not saved");

  QJsonObject different = login;
  different.insert(QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("late-other-account")}});
  deliverWeak(different);
  ok &= check(!repository.isAuthenticated() && repository.accountKey() == QStringLiteral("weak-live-login") &&
                  repository.sessionGeneration() == secondEpoch,
              "different unverified login switched the account or renewed reading");
  deliverWeak(login);
  ok &= check(!repository.isAuthenticated() && repository.sessionGeneration() == secondEpoch,
              "same-account login recovered a stream invalidated by a conflicting account");

  // Hard failures are independent of the Uncertain label used by a valid weak
  // view. Neither a post-login loss nor an initial queue failure can recover
  // through another source-free login packet.
  for (const auto state : {SessionConnectionState::Disconnected, SessionConnectionState::Uncertain, SessionConnectionState::Closing}) {
    PetRepository interrupted;
    interrupted.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact)));
    const quint64 epoch = interrupted.sessionGeneration();
    interrupted.setConnectionState(state, QStringLiteral("synthetic disconnect / input overflow / shutdown"));
    interrupted.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact)));
    ok &= check(!interrupted.isAuthenticated() && interrupted.sessionGeneration() == epoch && !interrupted.sessionContext().canPersist(),
                "a hard input interruption was recovered by a repeated weak login");
  }
  PetRepository initiallyInterrupted;
  initiallyInterrupted.setConnectionState(SessionConnectionState::Disconnected, QStringLiteral("capture lost before login"));
  initiallyInterrupted.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact)));
  ok &= check(!initiallyInterrupted.isAuthenticated() && initiallyInterrupted.sessionGeneration() == 0,
              "an initial stream failure was mistaken for pristine weak login bootstrap");
  PetRepository verified;
  deliverVerifiedFixture(&verified, login);
  const quint64 verifiedEpoch = verified.sessionGeneration();
  verified.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact)));
  ok &= check(!verified.isAuthenticated() && verified.sessionGeneration() == verifiedEpoch && !verified.sessionContext().canPersist(),
              "source-free duplicate login silently downgraded an established verified source");
  return ok;
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const bool ok = primitiveTests() && controllerTests() && repeatedWeakLoginTests();
  if (ok) std::puts("PASS: bounded intents, atomic revocation, real-dispatch deadlines and asynchronous move ownership");
  return ok ? 0 : 1;
}
