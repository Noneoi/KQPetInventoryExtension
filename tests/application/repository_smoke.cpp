#include "application/pet/pet_repository.h"
#include "support/protocol_test_support.h"
#include "protocol/packet_contract.h"
#include "storage/storage_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QSemaphore>
#include <atomic>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

void deliver(PetRepository* repository, const QJsonObject& packet) {
  deliverVerifiedFixture(repository, packet);
  if (!waitForRepositoryIdle(repository))
    qFatal("repository storage did not complete within the test budget");
}

void login(PetRepository* repository, const QString& account) {
  deliver(repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
                       {QStringLiteral("info"),
                        QJsonObject{{QStringLiteral("n"), account}}}});
}

QJsonObject warehousePacket(int race42 = 7152, const QString& name42 = QStringLiteral("skin-a")) {
  return {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
          {QStringLiteral("es"),
           QJsonArray{QJsonObject{{QStringLiteral("id"), 42},
                                  {QStringLiteral("ri"), race42},
                                  {QStringLiteral("fr"), race42 + 10},
                                  {QStringLiteral("n"), name42},
                                  {QStringLiteral("lv"), 100},
                                  {QStringLiteral("gd"), 100001},
                                  {QStringLiteral("zdl"), 90001},
                                  {QStringLiteral("xzdl"), 89901}},
                      QJsonObject{{QStringLiteral("id"), 43},
                                  {QStringLiteral("ri"), race42},
                                  {QStringLiteral("fr"), race42 + 10},
                                  {QStringLiteral("n"), name42},
                                  {QStringLiteral("lv"), 100},
                                  {QStringLiteral("gd"), 100002}}}},
          {QStringLiteral("ns"), QJsonArray{}},
          {QStringLiteral("rb"), QJsonArray{}}};
}

void acceptWarehouse(PetRepository* repository, quint64 generation,
                     const QJsonObject& packet) {
  repository->beginListRefresh(generation, repository->accountKey(),
                               repository->sessionGeneration());
  deliver(repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                       {QStringLiteral("pl"), QJsonArray{}},
                       {QStringLiteral("pps"), QJsonArray{}}});
  repository->expectListPart(QStringLiteral("2_1_S"), generation,
                             repository->accountKey(), repository->sessionGeneration());
  deliver(repository, packet);
}

QJsonObject detailPacket(qint64 id, int race, int face, int power) {
  return {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
          {QStringLiteral("p"),
           QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("r"), race},
                       {QStringLiteral("fr"), face},
                       {QStringLiteral("n"), QStringLiteral("detail-%1").arg(race)},
                       {QStringLiteral("lv"), 100},
                       {QStringLiteral("zdl"), power},
                       {QStringLiteral("xzdl"), power - 100}}}};
}

bool protocolRegressions(const QString& root) {
  bool ok = true;
  qint64 integer = 0;
  ok &= require(!PacketContracts::checkedInteger(1.5, &integer) &&
                    !PacketContracts::checkedInteger(QJsonValue::Null, &integer) &&
                    !PacketContracts::checkedInteger(true, &integer) &&
                    !PacketContracts::checkedInteger(QStringLiteral("1e3"), &integer) &&
                    !PacketContracts::checkedInteger(QStringLiteral(" 12"), &integer) &&
                    !PacketContracts::checkedInteger(9007199254740992.0, &integer) &&
                    PacketContracts::checkedInteger(QStringLiteral("9223372036854775807"), &integer) &&
                    integer == std::numeric_limits<qint64>::max() &&
                    !PacketContracts::checkedInteger(QStringLiteral("9223372036854775808"), &integer),
                "integer decoder accepted lossy, fractional, or malformed integers");
  ok &= require(!PacketContracts::checkedAdd(std::numeric_limits<qint64>::max(), 1, &integer) &&
                    !PacketContracts::checkedMultiply(std::numeric_limits<qint64>::min(), -1, &integer) &&
                    !PacketContracts::checkedMultiply(std::numeric_limits<qint64>::max(), 2, &integer) &&
                    PacketContracts::checkedMultiply(-7, -8, &integer) && integer == 56,
                "checked arithmetic overflow handling failed");
  ok &= require(PacketContracts::decodePetList(QJsonValue::Undefined).state == PacketFieldState::Missing &&
                    PacketContracts::decodePetList(QJsonValue::Null).state == PacketFieldState::Invalid &&
                    PacketContracts::decodePetList(QJsonArray{}).state == PacketFieldState::Empty &&
                    PacketContracts::decodePetList(QJsonArray{false}).state == PacketFieldState::Invalid,
                "missing, malformed, and explicit empty lists were conflated");

  qputenv("KQPET_DATA_ROOT", QDir(root).filePath(QStringLiteral("protocol")).toUtf8());
  PetRepository repository;
  bool sawLegacyProbe = false;
  bool legacyProbeContained = true;
  QObject::connect(repository.storageService(), &StorageService::completed, &repository,
                   [&](const StorageResult& result) {
    if (QFileInfo(result.absolutePath).fileName() == QStringLiteral("cache-v1.json")) {
      sawLegacyProbe = true;
      legacyProbeContained &= result.absolutePath.startsWith(
          QDir(repository.dataRoot()).filePath(QStringLiteral("legacy-import")), Qt::CaseInsensitive);
    }
  });
  login(&repository, QStringLiteral("protocol-account"));
  const auto expectBackpack = [&](quint64 generation) {
    repository.beginListRefresh(generation, repository.accountKey(), repository.sessionGeneration());
  };
  const QJsonObject pet{{QStringLiteral("id"), 51}, {QStringLiteral("r"), 7152}, {QStringLiteral("lv"), 100}};
  const QJsonObject backpack{{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                             {QStringLiteral("pl"), QJsonArray{pet}},
                             {QStringLiteral("pps"), QJsonArray{QStringLiteral("51")}},
                             {QStringLiteral("ppc"), 12}};
  expectBackpack(101);
  deliver(&repository, backpack);
  const quint64 oldRevision = repository.inventoryRevision();
  const QDateTime previousObservedAt = repository.backpackObservation().observedAt;
  QJsonObject invalid = backpack;
  invalid.insert(QStringLiteral("pps"), QJsonArray{QStringLiteral("51#51")});
  expectBackpack(102);
  deliver(&repository, invalid);
  ok &= require(repository.inventoryRevision() == oldRevision && repository.backpackIds() == QList<qint64>{51} &&
                    repository.backpackObservation().observedAt == previousObservedAt,
                "invalid sequence changed list, sequence, revision, or observation time");
  invalid = backpack;
  invalid.insert(QStringLiteral("ppc"), 0.5);
  deliver(&repository, invalid);
  ok &= require(repository.backpackCapacity() == 12 && repository.inventoryRevision() == oldRevision,
                "invalid capacity partially changed repository state");
  invalid = backpack;
  invalid.insert(QStringLiteral("pl"), QJsonArray{pet, QJsonObject{{QStringLiteral("id"), 1.5}}});
  deliver(&repository, invalid);
  ok &= require(repository.backpackIds() == QList<qint64>{51} && repository.inventoryRevision() == oldRevision,
                "one malformed pet truncated a previously valid inventory");

  repository.expectListPart(QStringLiteral("2_1_S"), 102, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, warehousePacket());
  repository.expectListPart(QStringLiteral("2_1_S"), 103, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
                        {QStringLiteral("ns"), QJsonArray{QJsonObject{{QStringLiteral("id"), 61}}}}});
  ok &= require(repository.warehousePets().size() == 3 && !repository.warehousePet(42).isEmpty() &&
                    !repository.listObservationsAuthoritativeForWrite(),
                "partial warehouse group erased unobserved groups or authorized writes");
  repository.expectListPart(QStringLiteral("2_1_S"), 104, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
                        {QStringLiteral("ns"), QJsonValue::Null}, {QStringLiteral("es"), QJsonArray{}}});
  ok &= require(repository.warehousePets().size() == 1 && !repository.warehousePet(61).isEmpty(),
                "malformed group replaced its cache or valid explicit-empty sibling was lost");
  repository.expectListPart(QStringLiteral("2_1_S"), 105, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")}, {QStringLiteral("ns"), QJsonArray{}}});
  ok &= require(repository.warehousePets().isEmpty(), "explicit empty group did not clear the known group");

  expectBackpack(106);
  deliver(&repository, backpack);
  repository.expectListPart(QStringLiteral("2_1_S"), 106, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, warehousePacket());
  ok &= require(repository.listObservationsAuthoritativeForWrite(),
                "complete synthetic verified ordered list observations did not authorize fixture preflight");
  const quint64 factsBeforePreservation = repository.inventoryRevision();
  const quint64 preservation = repository.preserveBackpackDetailAsync(51);
  ok &= require(preservation != 0 && waitForRepositoryPersistence(&repository) &&
                    repository.isDetailPersisted(51) && repository.inventoryRevision() == factsBeforePreservation,
                "saving unchanged backpack data invalidated the move's fact revision");
  expectBackpack(107);
  ok &= require(!repository.listObservationsAuthoritativeForWrite(),
                "starting a new preflight inherited previous observation authority");
  deliver(&repository, backpack);
  repository.expectListPart(QStringLiteral("2_1_S"), 107, repository.accountKey(), repository.sessionGeneration());
  InboundEnvelope unordered = verifiedFixtureEnvelope(&repository, warehousePacket());
  unordered.orderEvidenceToken.clear();
  repository.handleEnvelope(unordered);
  ok &= require(!repository.listObservationsAuthoritativeForWrite(),
                "sequence and local generation fabricated a missing host-order proof");

  expectBackpack(108);
  deliver(&repository, backpack);
  repository.expectListPart(QStringLiteral("2_1_S"), 108, repository.accountKey(), repository.sessionGeneration());
  QJsonObject contradictory = warehousePacket();
  contradictory.insert(QStringLiteral("ns"), QJsonArray{pet});
  deliver(&repository, contradictory);
  ok &= require(!repository.listObservationsAuthoritativeForWrite(),
                "the same instance in backpack and warehouse authorized a write");
  repository.expectListPart(QStringLiteral("2_1_S"), 109, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, warehousePacket());

  PacketCorrelationStrength correlation = PacketCorrelationStrength::ServerCorrelated;
  QObject::connect(&repository, &PetRepository::detailObserved, &repository,
                   [&](qint64, PacketCorrelationStrength observed, quint64, bool) { correlation = observed; });
  repository.expectDetail(42, 201, repository.accountKey(), repository.sessionGeneration());
  repository.cancelDetailRequest(42, 201);
  repository.expectDetail(42, 202, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(42, 7152, 7162, 30000));
  ok &= require(correlation == PacketCorrelationStrength::EntityCorrelated,
                "same-instance retry observation masqueraded as a server-correlated response");
  InboundEnvelope old = verifiedFixtureEnvelope(&repository, detailPacket(42, 7152, 7162, 99999));
  login(&repository, QStringLiteral("new-protocol-account"));
  acceptWarehouse(&repository, 203, warehousePacket());
  repository.expectDetail(42, 204, repository.accountKey(), repository.sessionGeneration());
  old.receiveSequence = repository.lastInboundSequence() + 1;
  repository.handleEnvelope(old);
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() != 99999,
                "old captured source committed into a new matching expectation");
  const quint64 beforeUnknown = repository.inventoryRevision();
  repository.handlePacket(QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(detailPacket(42, 7152, 7162, 99999)).toJson(QJsonDocument::Compact)));
  ok &= require(repository.sessionContext().state == SessionConnectionState::Uncertain &&
                    !repository.isAuthenticated() && repository.inventoryRevision() == beforeUnknown &&
                    !repository.listObservationsAuthoritativeForWrite(),
                "unknown source inherited a verified session's trust");
  repository.handlePacket(QStringLiteral("recivedata"),
      QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"untrusted-switch\"}}"));
  ok &= require(repository.accountKey() == QStringLiteral("new-protocol-account"),
                "untrusted login reassigned the current account");

  qputenv("KQPET_DATA_ROOT", QDir(root).filePath(QStringLiteral("unknown-source")).toUtf8());
  PetRepository weak;
  weak.handlePacket(QStringLiteral("recivedata"),
                    QStringLiteral("{\"_cmd\":\"21_1\",\"info\":{\"n\":\"weak\"}}"));
  weak.beginListRefresh(1, weak.accountKey(), weak.sessionGeneration());
  weak.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(backpack).toJson(QJsonDocument::Compact)));
  ok &= require(waitForRepositoryIdle(&weak) && weak.backpackPets().size() == 1 && QFile::exists(weak.cachePath()) &&
                    !weak.listObservationsAuthoritativeForWrite(),
                "weak read-only list was not persisted or became write-authoritative");
  weak.expectDetail(51, 2, weak.accountKey(), weak.sessionGeneration());
  bool weakSaved = false;
  QObject::connect(&weak, &PetRepository::detailResponseAccepted, &weak,
                   [&](qint64, quint64) { weakSaved = true; });
  weak.handlePacket(QStringLiteral("recivedata"),
      QString::fromUtf8(QJsonDocument(detailPacket(51, 7152, 7162, 12345)).toJson(QJsonDocument::Compact)));
  ok &= require(waitForRepositoryIdle(&weak) && weak.detailFor(51).value(QStringLiteral("zdl")).toInt() == 12345 &&
                    weak.detailFor(51).value(QStringLiteral("_unverifiedObservation")).toBool() &&
                    weak.isDetailPersisted(51) && weakSaved && QFile::exists(weak.cachePath()) &&
                    !weak.listObservationsAuthoritativeForWrite(),
                "valid weak detail was not saved or gained write authority");
  const QDir weakDetails(QDir(QFileInfo(weak.cachePath()).absolutePath()).filePath(QStringLiteral("details")));
  const auto detailBytes = [&] {
    QFile file(weakDetails.filePath(QStringLiteral("51.json")));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
  };
  const auto originalBytes = detailBytes();
  weak.expectDetail(51, 3, weak.accountKey(), weak.sessionGeneration());
  QJsonObject incomplete = detailPacket(51, 7152, 7162, 99999);
  auto incompletePet = incomplete.value(QStringLiteral("p")).toObject();
  incompletePet.remove(QStringLiteral("lv")); incomplete.insert(QStringLiteral("p"), incompletePet);
  weak.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(incomplete).toJson(QJsonDocument::Compact)));
  ok &= require(waitForRepositoryIdle(&weak) && detailBytes() == originalBytes,
      "incomplete observed response overwrote the valid local original");
  weak.expectDetail(51, 4, weak.accountKey(), weak.sessionGeneration());
  QJsonObject latest = detailPacket(51, 7152, 7162, 23456);
  auto latestPet = latest.value(QStringLiteral("p")).toObject();
  latestPet.insert(QStringLiteral("lv"), 120); latest.insert(QStringLiteral("p"), latestPet);
  weak.handlePacket(QStringLiteral("recivedata"), QString::fromUtf8(QJsonDocument(latest).toJson(QJsonDocument::Compact)));
  ok &= require(waitForRepositoryIdle(&weak) && detailBytes() != originalBytes &&
      weakDetails.entryList({QStringLiteral("*.json")}, QDir::Files).size() == 1 &&
      weak.detailFor(51).value(QStringLiteral("lv")).toInt() == 120,
      "latest observed detail did not replace one file or an old list field hid the updated level");
  PetRepository offline;
  ok &= require(waitForRepositoryIdle(&offline) && offline.accountKey() == QStringLiteral("weak") &&
      offline.backpackPets().size() == 1 && offline.isDetailPersisted(51) && !offline.isAuthenticated() &&
      !offline.recordVersion(51).sourceKnown && !offline.listObservationsAuthoritativeForWrite() &&
      offline.detailFor(51).value(QStringLiteral("zdl")).toInt() == 23456 &&
      offline.detailFor(51).value(QStringLiteral("lv")).toInt() == 120,
      "offline startup did not restore observed account/list/detail or promoted their trust");
  login(&weak, QStringLiteral("weak"));
  ok &= require(weak.sessionContext().canPersist() && weak.backpackPets().size() == 1 &&
                    !weak.recordVersion(51).sourceKnown &&
                    !weak.isOnlineData() && !weak.listObservationsAuthoritativeForWrite(),
                "verified login promoted cached weak observations into current source/write evidence");
  ok &= require(sawLegacyProbe && legacyProbeContained,
                "an isolated data-root override probed the user's personal legacy directory");

  // v1.3-level move basis on an unverified host stream: complete lists read in
  // order through the current, never-interrupted stream.
  qputenv("KQPET_DATA_ROOT", QDir(root).filePath(QStringLiteral("read-continuity")).toUtf8());
  PetRepository movable;
  const auto weakDeliver = [&movable](const QJsonObject& packet) {
    movable.handlePacket(QStringLiteral("recivedata"),
                         QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact)));
  };
  weakDeliver({{QStringLiteral("_cmd"), QStringLiteral("21_1")},
               {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("movable")}}}});
  const auto refreshBoth = [&](quint64 generation, const QJsonObject& warehouse) {
    movable.beginListRefresh(generation, movable.accountKey(), movable.sessionGeneration());
    weakDeliver(backpack);
    movable.expectListPart(QStringLiteral("2_1_S"), generation, movable.accountKey(), movable.sessionGeneration());
    weakDeliver(warehouse);
  };
  refreshBoth(1, warehousePacket());
  ok &= require(movable.readContinuityWriteAllowed() && !movable.sessionContext().canPersist() &&
                    movable.listObservationsAuthoritativeForWrite(),
                "fresh complete in-order lists on an uninterrupted stream did not allow a v1.3-level move");
  movable.beginListRefresh(2, movable.accountKey(), movable.sessionGeneration());
  ok &= require(!movable.listObservationsAuthoritativeForWrite(),
                "a new read-continuity preflight inherited the previous observations");
  movable.expectListPart(QStringLiteral("2_1_S"), 2, movable.accountKey(), movable.sessionGeneration());
  weakDeliver(warehousePacket());
  ok &= require(!movable.listObservationsAuthoritativeForWrite(),
                "a warehouse read without a fresh backpack read allowed a move");
  refreshBoth(3, {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")}, {QStringLiteral("ns"), QJsonArray{}}});
  ok &= require(!movable.listObservationsAuthoritativeForWrite(),
                "a partial warehouse read allowed a read-continuity move");
  QJsonObject duplicated = warehousePacket();
  duplicated.insert(QStringLiteral("ns"), QJsonArray{pet});
  refreshBoth(4, duplicated);
  ok &= require(!movable.listObservationsAuthoritativeForWrite(),
                "the same instance in backpack and warehouse allowed a read-continuity move");
  refreshBoth(5, warehousePacket());
  ok &= require(movable.listObservationsAuthoritativeForWrite(),
                "a clean re-read on the same stream did not restore the move basis");
  movable.setConnectionState(SessionConnectionState::Uncertain, QStringLiteral("synthetic input overflow"));
  ok &= require(!movable.readContinuityWriteAllowed() && !movable.listObservationsAuthoritativeForWrite(),
                "an interrupted stream kept its move basis");
  refreshBoth(6, warehousePacket());
  ok &= require(!movable.readContinuityWriteAllowed() && !movable.listObservationsAuthoritativeForWrite(),
                "lists read after a stream interruption regained a move basis");
  ok &= require(waitForRepositoryIdle(&movable), "read-continuity repository did not drain its I/O");
  return ok;
}

bool asynchronousPersistenceRegression(const QString& root) {
  bool ok = true;
  QSemaphore entered, release;
  std::atomic<bool> blocked{false};
  StorageService storage(QDir(root).filePath(QStringLiteral("async-storage")), {},
      [&](const QString& path, const QByteArray& bytes) {
        if (QFileInfo(path).fileName() == QStringLiteral("42.json") && !blocked.exchange(true)) {
          entered.release();
          release.acquire();
        }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
        const qint64 written = file.write(bytes);
        if (written != bytes.size() || !file.commit()) return StorageWriteAttempt{false, written, file.errorString()};
        return StorageWriteAttempt{true, written, {}};
      });
  PetRepository repository(nullptr, &storage);
  login(&repository, QStringLiteral("async-original"));
  acceptWarehouse(&repository, 300, warehousePacket());
  int oldDetailAccepted = 0;
  QObject::connect(&repository, &PetRepository::detailResponseAccepted, &repository,
                   [&](qint64 id, quint64 generation) {
                     if (id == 42 && generation == 301) ++oldDetailAccepted;
                   });
  repository.expectDetail(42, 301, repository.accountKey(), repository.sessionGeneration());
  deliverVerifiedFixture(&repository, detailPacket(42, 7152, 7162, 32100));
  ok &= require(entered.tryAcquire(1, 1500) && oldDetailAccepted == 0 &&
                    repository.hasCachedDetail(42) && !repository.isDetailPersisted(42) &&
                    repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 32100,
                "detail admission was reported as Saved before actual I/O completion");
  const QString originalPath = QDir(QFileInfo(repository.cachePath()).absolutePath())
                                   .filePath(QStringLiteral("details/42.json"));
  deliverVerifiedFixture(&repository,
      {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
       {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), QStringLiteral("async-new")}}}});
  release.release();
  ok &= require(waitForRepositoryPersistence(&repository) && QFile::exists(originalPath) &&
                    repository.detailFor(42).isEmpty() && oldDetailAccepted == 0,
                "old-session write changed the new UI, disappeared, or notified the new session");
  QFile originalFile(originalPath);
  ok &= require(originalFile.open(QIODevice::ReadOnly) &&
                    QJsonDocument::fromJson(originalFile.readAll()).object()
                        .value(QStringLiteral("account")).toString() == QStringLiteral("async-original"),
                "queued detail did not retain its original account envelope");

  acceptWarehouse(&repository, 302, warehousePacket());
  const QString invalidTarget = QDir(QFileInfo(repository.cachePath()).absolutePath())
                                    .filePath(QStringLiteral("details/43.json"));
  QDir().mkpath(invalidTarget);
  bool rejected = false;
  bool saveFailureVisible = false;
  QObject::connect(&repository, &PetRepository::detailResponseRejected, &repository,
                   [&](qint64 id, quint64 generation, const QString&) {
                     if (id == 43 && generation == 303) rejected = true;
                   });
  QObject::connect(&repository, &PetRepository::persistenceChanged, &repository,
                   [&](const QString&, const QString& path, quint64, StorageStatus status, const QString&) {
                     if (path == QStringLiteral("details/43.json") && status == StorageStatus::WriteFailed)
                       saveFailureVisible = true;
                   });
  repository.expectDetail(43, 303, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(43, 7152, 7162, 31900));
  ok &= require(rejected && saveFailureVisible && repository.hasCachedDetail(43) &&
                    !repository.isDetailPersisted(43) &&
                    repository.warehousePet(43).value(QStringLiteral("zdl")).toInt() == 31900,
                "real detail I/O failure was hidden or memory update was misreported as saved");
  return ok;
}

bool malformedDetailRegression(const QString& root) {
  StorageService storage(QDir(root).filePath(QStringLiteral("malformed-detail")));
  PetRepository repository(nullptr, &storage);
  login(&repository, QStringLiteral("malformed-detail"));
  acceptWarehouse(&repository, 700, warehousePacket());
  repository.expectDetail(42, 701, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(42, 7152, 7162, 31500));
  const auto original = repository.rawRecordHandle(42);
  int rejected = 0;
  quint64 rejectedGeneration = 0;
  QString reason;
  QObject::connect(&repository, &PetRepository::detailResponseRejected, &repository,
      [&](qint64 id, quint64 generation, const QString& message) {
    if (id == 42) { ++rejected; rejectedGeneration = generation; reason = message; }
  });
  repository.expectDetail(42, 702, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")}});
  bool ok = require(rejected == 1 && rejectedGeneration == 702 && reason.contains(QStringLiteral("数据对象")) &&
      repository.rawRecordHandle(42) == original,
      "missing detail object waited for timeout or discarded the existing cache");
  repository.expectDetail(42, 703, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_R")},
      {QStringLiteral("p"), QJsonObject{{QStringLiteral("id"), QStringLiteral("invalid-id")}}}});
  ok &= require(rejected == 2 && rejectedGeneration == 703 && reason.contains(QStringLiteral("ID")) &&
      repository.rawRecordHandle(42) == original,
      "invalid detail identity waited for timeout or discarded the existing cache");
  repository.expectDetail(42, 704, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(43, 7152, 7162, 39000));
  ok &= require(rejected == 2 && repository.rawRecordHandle(42) == original && !repository.hasCachedDetail(43),
      "a different instance response ended the active request or updated another pet");
  deliver(&repository, detailPacket(42, 7152, 7162, 41000));
  ok &= require(rejected == 2 && repository.detailFor(42).value(QStringLiteral("zdl")).toInt() == 41000 &&
      repository.isDetailPersisted(42), "correct detail could not complete after an ignored other-instance reply");
  return ok;
}

bool persistenceAdmissionRegression(const QString& root) {
  bool ok = true;
  QSemaphore entered, release;
  StorageLimits limits;
  limits.maximumOutstandingTasks = 1;
  StorageService storage(QDir(root).filePath(QStringLiteral("admission-storage")), limits,
      [&](const QString& path, const QByteArray& bytes) {
        if (QFileInfo(path).fileName() == QStringLiteral("hold.json")) {
          entered.release(); release.acquire();
        }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
        const qint64 written = file.write(bytes);
        if (written != bytes.size() || !file.commit()) return StorageWriteAttempt{false, written, file.errorString()};
        return StorageWriteAttempt{true, written, {}};
      });
  PetRepository repository(nullptr, &storage);
  login(&repository, QStringLiteral("admission"));
  acceptWarehouse(&repository, 400, warehousePacket());
  const auto held = storage.submitWrite({repository.storageContext(), QStringLiteral("hold.json"),
                                        1, QByteArray("held"), true});
  ok &= require(held.accepted && entered.tryAcquire(1, 1500), "admission fixture did not fill the queue");
  QStringList events;
  QObject::connect(&repository, &PetRepository::detailPersistenceQueued, &repository,
                   [&](qint64 id, quint64 generation, quint64 taskId) {
                     if (id == 42 && generation == 401 && taskId == 0) events.append(QStringLiteral("received"));
                   });
  QObject::connect(&repository, &PetRepository::detailResponseRejected, &repository,
                   [&](qint64 id, quint64 generation, const QString&) {
                     if (id == 42 && generation == 401) events.append(QStringLiteral("rejected"));
                   });
  repository.expectDetail(42, 401, repository.accountKey(), repository.sessionGeneration());
  deliverVerifiedFixture(&repository, detailPacket(42, 7152, 7162, 31500));
  ok &= require(events == QStringList{QStringLiteral("received"), QStringLiteral("rejected")} &&
                    repository.hasCachedDetail(42) && !repository.isDetailPersisted(42),
                "valid detail admission failure was indistinguishable from a network failure");
  release.release();
  ok &= require(storage.shutdown(), "admission fixture I/O did not drain");
  return ok;
}

bool writeCacheFixture(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  return file.write(bytes) == bytes.size() && file.commit();
}

bool asynchronousReadRegression(const QString& root, bool switchAccount) {
  bool ok = true;
  const QString dataRoot = QDir(root).filePath(switchAccount ? QStringLiteral("read-cross-account") : QStringLiteral("read-revision"));
  const QString originalAccount = QStringLiteral("disk-A");
  const QString targetAccount = switchAccount ? QStringLiteral("disk-B") : originalAccount;
  const QString accountPath = QDir(dataRoot).filePath(QStringLiteral("accounts/disk-A"));
  QJsonObject oldBrief{{QStringLiteral("id"), 42}, {QStringLiteral("ri"), 7152},
                       {QStringLiteral("lv"), 100}, {QStringLiteral("n"), QStringLiteral("old")},
                       {QStringLiteral("_location"), QStringLiteral("warehouse")},
                       {QStringLiteral("_warehouseGroup"), QStringLiteral("elite")}};
  QJsonObject ghost = oldBrief;
  ghost.insert(QStringLiteral("id"), 99);
  ok &= require(writeCacheFixture(QDir(accountPath).filePath(QStringLiteral("inventory.json")),
      {{QStringLiteral("schema"), 3}, {QStringLiteral("account"), originalAccount},
       {QStringLiteral("backpack"), QJsonArray{}}, {QStringLiteral("warehouse"), QJsonArray{oldBrief, ghost}}}),
      "disk inventory fixture could not be created");
  ok &= require(writeCacheFixture(QDir(accountPath).filePath(QStringLiteral("details/42.json")),
      {{QStringLiteral("schema"), 3}, {QStringLiteral("account"), originalAccount},
       {QStringLiteral("instanceId"), QStringLiteral("42")},
       {QStringLiteral("pet"), detailPacket(42, 7152, 7162, 1000).value(QStringLiteral("p"))}}),
      "disk detail fixture could not be created");
  QSemaphore firstEntered, firstRelease, barrierEntered, barrierRelease;
  StorageService storage(dataRoot, {}, [&](const QString& path, const QByteArray& bytes) {
    if (QFileInfo(path).fileName() == QStringLiteral("hold.json")) {
      firstEntered.release(); firstRelease.acquire();
    } else if (QFileInfo(path).fileName() == QStringLiteral("read-barrier.json")) {
      barrierEntered.release(); barrierRelease.acquire();
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return StorageWriteAttempt{false, 0, file.errorString()};
    const qint64 written = file.write(bytes);
    if (written != bytes.size() || !file.commit()) return StorageWriteAttempt{false, written, file.errorString()};
    return StorageWriteAttempt{true, written, {}};
  });
  const auto holder = storage.createAccountContext(originalAccount, originalAccount);
  storage.submitWrite({holder, QStringLiteral("hold.json"), 1, QByteArray("hold"), true});
  ok &= require(firstEntered.tryAcquire(1, 1500), "read regression could not hold I/O");
  PetRepository repository(nullptr, &storage, QDir(root).filePath(QStringLiteral("no-legacy-source")));
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
      {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), originalAccount}}}});
  ok &= require(repository.requestCachedDetail(42), "selected cache read was rejected");
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  storage.submitWrite({holder, QStringLiteral("read-barrier.json"), 1, QByteArray("barrier"), false});
  firstRelease.release();
  ok &= require(barrierEntered.tryAcquire(1, 1500), "selected disk reads did not precede the normal-write barrier");
  // I/O has read the old bytes, but their Core completions are still queued.
  if (switchAccount) deliverVerifiedFixture(&repository,
      {{QStringLiteral("_cmd"), QStringLiteral("21_1")},
       {QStringLiteral("info"), QJsonObject{{QStringLiteral("n"), targetAccount}}}});
  repository.beginListRefresh(500, repository.accountKey(), repository.sessionGeneration());
  deliverVerifiedFixture(&repository, {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
      {QStringLiteral("pl"), QJsonArray{QJsonObject{{QStringLiteral("id"), 51},
          {QStringLiteral("r"), 7152}, {QStringLiteral("lv"), 100}}}},
      {QStringLiteral("pps"), QJsonArray{QStringLiteral("51")}}, {QStringLiteral("ppc"), 12}});
  repository.expectListPart(QStringLiteral("2_1_S"), 500, repository.accountKey(), repository.sessionGeneration());
  deliverVerifiedFixture(&repository, warehousePacket());
  repository.expectDetail(42, 501, repository.accountKey(), repository.sessionGeneration());
  deliverVerifiedFixture(&repository, detailPacket(42, 7152, 7162, 99000));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  ok &= require(repository.accountKey() == targetAccount && repository.backpackIds() == QList<qint64>{51} &&
                    repository.warehousePet(99).isEmpty() &&
                    repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 99000,
                "late disk completion overwrote a newer network record or another account");
  barrierRelease.release();
  ok &= require(waitForRepositoryIdle(&repository), "read regression did not drain real I/O");
  return ok;
}

// T5-A: a partial list observation must not overwrite the original that an
// earlier verified session left on disk, so the repository reloads that
// original (pet_repository.cpp: "Load that original now while keeping the newly
// observed compact list fields"). The reload republishes exactly the brief, the
// raw record and the merged view the roster already holds; bumping the fact
// revision for it invalidates a move preflight whose intent write is still
// queued, so the confirmed move ends as "list or session changed" although
// nothing observable changed.
bool diskDetailReloadRegression(const QString& root) {
  bool ok = true;
  const QString dataRoot = QDir(root).filePath(QStringLiteral("disk-detail-reload"));
  const QString account = QStringLiteral("disk-reload-A");
  const qint64 instance = 44;
  const QJsonObject partialPet{{QStringLiteral("id"), instance},
                               {QStringLiteral("r"), 7152},
                               {QStringLiteral("lv"), 100}};
  StorageService storage(dataRoot);
  PetRepository repository(nullptr, &storage,
                           QDir(root).filePath(QStringLiteral("no-legacy-source")));
  login(&repository, account);
  const auto deliverLists = [&](quint64 generation) {
    repository.beginListRefresh(generation, repository.accountKey(),
                                repository.sessionGeneration());
    deliverVerifiedFixture(&repository,
                           {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
                            {QStringLiteral("pl"), QJsonArray{partialPet}},
                            {QStringLiteral("pps"), QJsonArray{QStringLiteral("44")}},
                            {QStringLiteral("ppc"), 12}});
    repository.expectListPart(QStringLiteral("2_1_S"), generation,
                              repository.accountKey(),
                              repository.sessionGeneration());
    deliverVerifiedFixture(&repository,
                           {{QStringLiteral("_cmd"), QStringLiteral("2_1_S")},
                            {QStringLiteral("es"), QJsonArray{}},
                            {QStringLiteral("ns"), QJsonArray{}},
                            {QStringLiteral("rb"), QJsonArray{}}});
  };
  // First observation is persisted as the account's only-if-missing original.
  deliverLists(700);
  ok &= require(waitForRepositoryIdle(&repository) && repository.hasCachedDetail(instance),
                "disk detail reload fixture could not persist the first observation");
  const QJsonObject petBeforeReload = repository.backpackPet(instance);
  const quint64 revisionBeforeReload = repository.inventoryRevision();
  // The same partial observation now meets that original, so the write is
  // superseded and the repository reloads the file instead.
  deliverLists(701);
  const quint64 revisionAfterLists = repository.inventoryRevision();
  ok &= require(repository.backpackIds() == QList<qint64>{instance} &&
                    !petBeforeReload.isEmpty(),
                "disk detail reload fixture did not keep the instance in the backpack");
  ok &= require(waitForRepositoryIdle(&repository),
                "disk detail reload did not drain real I/O");
  const QJsonObject petAfterReload = repository.backpackPet(instance);
  const quint64 revisionAfterReload = repository.inventoryRevision();
  std::fprintf(stderr,
               "DISK-RELOAD id=%lld loaded=%d unchanged=%d revisionBeforeReload=%llu "
               "revisionAfterLists=%llu revisionAfterReload=%llu\n",
               static_cast<long long>(instance), int(repository.hasCachedDetail(instance)),
               int(petBeforeReload == petAfterReload),
               static_cast<unsigned long long>(revisionBeforeReload),
               static_cast<unsigned long long>(revisionAfterLists),
               static_cast<unsigned long long>(revisionAfterReload));
  ok &= require(repository.hasCachedDetail(instance),
                "the persisted original was not loaded after a partial list observation");
  ok &= require(petBeforeReload == petAfterReload,
                "the superseded-write reload changed the details it was supposed to republish");
  ok &= require(revisionAfterReload == revisionAfterLists,
                "identical disk detail facts unnecessarily invalidated the fact revision");
  ok &= require(revisionAfterReload >= revisionBeforeReload, "fact revision moved backwards");
  return ok;
}

bool pagedMigrationRegression(const QString& root) {
  bool ok = true;
  const QString dataRoot = QDir(root).filePath(QStringLiteral("migration-target"));
  const QString legacyRoot = QDir(root).filePath(QStringLiteral("migration-source"));
  const QString account = QStringLiteral("legacy-A");
  QJsonArray warehouse;
  QJsonObject details;
  for (int id = 1000; id < 1300; ++id) {
    QJsonObject pet = detailPacket(id, 7152, 7162, id).value(QStringLiteral("p")).toObject();
    QJsonObject brief = pet;
    brief.insert(QStringLiteral("_location"), QStringLiteral("warehouse"));
    brief.insert(QStringLiteral("_warehouseGroup"), QStringLiteral("normal"));
    warehouse.append(brief);
    details.insert(QString::number(id), pet);
  }
  const QJsonObject profile{{QStringLiteral("backpack"), QJsonArray{}},
      {QStringLiteral("warehouse"), warehouse}, {QStringLiteral("details"), details},
      {QStringLiteral("savedAt"), QStringLiteral("2020-01-01T00:00:00Z")}};
  const QJsonObject legacy{{QStringLiteral("schema"), 1}, {QStringLiteral("lastAccount"), account},
      {QStringLiteral("profiles"), QJsonObject{{account, profile}}}};
  ok &= require(writeCacheFixture(QDir(legacyRoot).filePath(QStringLiteral("cache-v1.json")), legacy),
                "legacy migration fixture could not be created");
  const QString firstDetail = QDir(dataRoot).filePath(QStringLiteral("accounts/legacy-A/details/1000.json"));
  ok &= require(writeCacheFixture(firstDetail, {{QStringLiteral("schema"), 3},
      {QStringLiteral("account"), account}, {QStringLiteral("instanceId"), QStringLiteral("1000")},
      {QStringLiteral("pet"), detailPacket(1000, 7152, 7162, 77777).value(QStringLiteral("p"))}}),
      "newer existing detail fixture could not be created");
  StorageLimits limits;
  limits.maximumOutstandingTasks = 1;
  StorageService storage(dataRoot, limits);
  PetRepository repository(nullptr, &storage, legacyRoot);
  ok &= require(waitForRepositoryIdle(&repository, 60000), "paged migration failed to respect queue backpressure");
  const QDir detailDirectory(QDir(dataRoot).filePath(QStringLiteral("accounts/legacy-A/details")));
  ok &= require(repository.accountKey() == account && !repository.isAuthenticated() &&
                    repository.warehousePets().size() == 300 &&
                    detailDirectory.entryList({QStringLiteral("*.json")}, QDir::Files).size() == 300 &&
                    repository.hasCachedDetail(1299) && repository.isDetailPersisted(1299) &&
                    repository.detailFor(1000).value(QStringLiteral("zdl")).toInt() == 77777 &&
                    QFile::exists(QDir(dataRoot).filePath(QStringLiteral("migration-v1-completed.json"))),
                "migration lost unqueued details, overwrote a newer record, or published incomplete completion");
  return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporary;
  bool ok = require(temporary.isValid(), "temporary directory unavailable");
  qputenv("KQPET_DATA_ROOT", temporary.path().toUtf8());

  PetRepository repository;
  int fullListChangeCount = 0;
  int detailChangeCount = 0;
  QObject::connect(&repository, &PetRepository::dataChanged, &application,
                   [&]() { ++fullListChangeCount; });
  QObject::connect(&repository, &PetRepository::detailChanged, &application,
                   [&](qint64) { ++detailChangeCount; });
  login(&repository, QStringLiteral("account-1001"));

  repository.beginListRefresh(5, repository.accountKey(),
                              repository.sessionGeneration());
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_1_10")},
           {QStringLiteral("pl"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 42},
                                   {QStringLiteral("r"), 7152},
                                   {QStringLiteral("lv"), 100}},
                       QJsonObject{{QStringLiteral("id"), 44},
                                   {QStringLiteral("r"), 7153},
                                   {QStringLiteral("lv"), 100}}}},
           {QStringLiteral("pps"), QJsonArray{QStringLiteral("42#44")}},
           {QStringLiteral("ppc"), 12}});
  const quint64 beforeDetailFactRevision = repository.inventoryRevision();
  repository.expectDetail(44, 6, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(44, 7153, 7163, 32000));
  ok &= require(repository.inventoryRevision() > beforeDetailFactRevision,
                "new detail facts did not invalidate the analysis/move input revision");
  const quint64 unchangedDetailRevision = repository.inventoryRevision();
  repository.expectDetail(44, 7, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(44, 7153, 7163, 32000));
  ok &= require(repository.inventoryRevision() == unchangedDetailRevision,
                "identical detail facts unnecessarily changed the fact revision");
  ok &= require(repository.backpackPet(44).value(QStringLiteral("zdl")).toInt() == 32000 &&
                    repository.backpackPet(44).value(QStringLiteral("_location")).toString() ==
                        QStringLiteral("backpack") &&
                    repository.hasCachedDetail(44),
                "instance detail refresh did not update and cache a backpack pet");
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_2_10")},
           {QStringLiteral("fis"),
            QJsonObject{{QStringLiteral("cfid"), 1},
                        {QStringLiteral("fl"),
                         QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                                {QStringLiteral("p"), 0},
                                                {QStringLiteral("ps"),
                                                 QStringLiteral("42#-1#0")}}}}}}});
  ok &= require(repository.formationKnown() && repository.isDeployed(42) &&
                    !repository.isDeployed(44) &&
                    repository.backpackPet(42)
                        .value(QStringLiteral("_inFormation"))
                        .toBool() &&
                    !repository.backpackPet(44)
                         .value(QStringLiteral("_inFormation"))
                         .toBool(),
                "current formation did not decorate backpack instances");
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_2_0")},
           {QStringLiteral("fl"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                   {QStringLiteral("p"), 0},
                                   {QStringLiteral("ps"),
                                    QStringLiteral("44#0#-1")}}}}});
  ok &= require(!repository.isDeployed(42) && repository.isDeployed(44),
                "formation position push did not update deployed instances");
  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_2_14")},
           {QStringLiteral("r"), 1},
           {QStringLiteral("fl"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                   {QStringLiteral("p"), 0},
                                   {QStringLiteral("ps"), QStringLiteral("42")}}}}});
  ok &= require(repository.isDeployed(42) && !repository.isDeployed(44),
                "formation save response did not update deployed instances");

  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_2_10")},
           {QStringLiteral("fis"),
            QJsonObject{{QStringLiteral("cfid"), 16},
                        {QStringLiteral("fl"),
                         QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                                {QStringLiteral("p"), 0},
                                                {QStringLiteral("ps"),
                                                 QStringLiteral("44")}}}}}},
           {QStringLiteral("plan17"),
            QJsonObject{{QStringLiteral("cpid"), 2},
                        {QStringLiteral("fl"),
                         QJsonArray{QJsonObject{{QStringLiteral("id"), 17},
                                                {QStringLiteral("p"), 2},
                                                {QStringLiteral("ps"),
                                                 QStringLiteral("42#0")}}}}}}});
  ok &= require(repository.isDeployed(42) && !repository.isDeployed(44),
                "diverse current formation alias was not resolved");

  deliver(&repository,
          {{QStringLiteral("_cmd"), QStringLiteral("2_2_10")},
           {QStringLiteral("fis"),
            QJsonObject{{QStringLiteral("cfid"), 17},
                        {QStringLiteral("fl"),
                         QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                                {QStringLiteral("p"), 0},
                                                {QStringLiteral("ps"),
                                                 QStringLiteral("-1#0#-1")}}}}}},
           {QStringLiteral("plan17"),
            QJsonObject{{QStringLiteral("cpid"), 5},
                        {QStringLiteral("fl"),
                         QJsonArray{
                             QJsonObject{{QStringLiteral("id"), 17},
                                         {QStringLiteral("p"), 0},
                                         {QStringLiteral("ps"), QStringLiteral("44")}},
                             QJsonObject{{QStringLiteral("id"), 17},
                                         {QStringLiteral("p"), 5},
                                         {QStringLiteral("ps"),
                                          QStringLiteral("42#0#44")}}}}}}});
  ok &= require(repository.isDeployed(42) && repository.isDeployed(44),
                "official cfid=17/plan17.cpid=5 current formation was not resolved");

  deliver(&repository, warehousePacket());
  ok &= require(repository.warehousePets().isEmpty(),
                "unsolicited warehouse list must not overwrite cache");

  acceptWarehouse(&repository, 10, warehousePacket());
  ok &= require(repository.warehousePets().size() == 2,
                "manual warehouse refresh was not accepted");
  ok &= require(!repository.warehousePet(42).contains(QStringLiteral("zdl")),
                "warehouse overview power must not come from list response");

  deliver(&repository, detailPacket(42, 7152, 7162, 11111));
  ok &= require(!repository.detailFor(42).contains(QStringLiteral("zdl")),
                "unsolicited detail response must not overwrite cache");

  repository.expectDetail(42, 20, repository.accountKey(), repository.sessionGeneration());
  const int listChangesBeforeDetail = fullListChangeCount;
  const int detailChangesBeforeWarehouseDetail = detailChangeCount;
  deliver(&repository, detailPacket(42, 7152, 7162, 30000));
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 30000,
                "warehouse overview power must come from detail cache");
  ok &= require(fullListChangeCount == listChangesBeforeDetail &&
                    detailChangeCount == detailChangesBeforeWarehouseDetail + 1,
                "detail response must emit one row update instead of a full-list rebuild");

  repository.expectDetail(43, 21, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(43, 7152, 7162, 28000));
  ok &= require(repository.warehousePet(43).value(QStringLiteral("zdl")).toInt() == 28000,
                "same-race pet instances must keep separate detail power");

  const QString accountRoot = QDir(temporary.path()).filePath(QStringLiteral("accounts/account-1001"));
  const QString detail42Path = QDir(accountRoot).filePath(QStringLiteral("details/42.json"));
  const QString detail43Path = QDir(accountRoot).filePath(QStringLiteral("details/43.json"));
  ok &= require(QFile::exists(detail42Path) && QFile::exists(detail43Path),
                "duplicate-race instances were not saved to separate files");
  const QJsonObject envelope42 = QJsonDocument::fromJson([&]() {
    QFile file(detail42Path); file.open(QIODevice::ReadOnly); return file.readAll();
  }()).object();
  ok &= require(envelope42.value(QStringLiteral("schema")).toInt() == 4 &&
                    envelope42.value(QStringLiteral("trust")).toString() == QStringLiteral("verified-observation") &&
                    envelope42.value(QStringLiteral("account")).toString() == QStringLiteral("account-1001") &&
                    envelope42.value(QStringLiteral("instanceId")).toString() == QStringLiteral("42") &&
                    !envelope42.value(QStringLiteral("imageCacheKey")).toString().isEmpty(),
                "schema-4 detail envelope metadata is incomplete");

  const QString originalBeforeSkin = repository.warehousePet(42)
                                         .value(QStringLiteral("_metaOriginalName"))
                                         .toString();
  const QString attributesBeforeSkin = repository.warehousePet(42)
                                           .value(QStringLiteral("_metaAttributes"))
                                           .toString();
  acceptWarehouse(&repository, 11, warehousePacket(990001, QStringLiteral("future-skin-b")));
  ok &= require(repository.warehousePet(42).value(QStringLiteral("_visualMismatch")).toBool(),
                "skin/race change was not detected for the same instance");
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 30000,
                "skin change must retain local cached power until detail refresh");
  repository.expectDetail(42, 22, repository.accountKey(), repository.sessionGeneration());
  deliver(&repository, detailPacket(42, 990001, 990002, 31000));
  ok &= require(repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 31000 &&
                    !repository.warehousePet(42).value(QStringLiteral("_visualMismatch")).toBool(),
                "same instance did not update after skin detail refresh");
  ok &= require(!originalBeforeSkin.isEmpty() && !attributesBeforeSkin.isEmpty() &&
                    repository.warehousePet(42)
                            .value(QStringLiteral("_metaOriginalName"))
                            .toString() == originalBeforeSkin &&
                    repository.warehousePet(42)
                            .value(QStringLiteral("_metaAttributes"))
                            .toString() == attributesBeforeSkin,
                "unknown future skin lost original-name or attribute metadata");

  repository.expectDetail(42, 23, repository.accountKey(), repository.sessionGeneration());
  login(&repository, QStringLiteral("account-2002"));
  ok &= require(!repository.formationKnown(),
                "account switch must clear runtime formation state");
  deliver(&repository, detailPacket(42, 990001, 990002, 99999));
  ok &= require(repository.warehousePets().isEmpty() && repository.detailFor(42).isEmpty(),
                "old-account delayed detail response leaked into new account");

  login(&repository, QStringLiteral("account-1001"));
  ok &= require(repository.warehousePets().size() == 2 &&
                    repository.warehousePet(42).value(QStringLiteral("zdl")).toInt() == 31000,
                "account-isolated cache did not reload correctly");

  ok &= protocolRegressions(temporary.path());
  ok &= asynchronousPersistenceRegression(temporary.path());
  ok &= malformedDetailRegression(temporary.path());
  ok &= persistenceAdmissionRegression(temporary.path());
  ok &= asynchronousReadRegression(temporary.path(), false);
  ok &= asynchronousReadRegression(temporary.path(), true);
  ok &= diskDetailReloadRegression(temporary.path());
  ok &= pagedMigrationRegression(temporary.path());
  if (!ok) return 1;
  std::fprintf(stdout, "PASS: account/instance cache with durable observation trust\n");
  return 0;
}
