#include "application/catalog/public_dataset_store.h"
#include "application/catalog/routine_overview_catalog.h"
#include "application/catalog/pet_detail_catalog.h"
#include "application/catalog/pet_skill_catalog.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "application/catalog/catalog_io_service.h"
#include "application/catalog/catalog_legacy_compatibility.h"
#include "fixtures/battle_replay_catalog_fixture.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) {
  if (!valid) std::fprintf(stderr, "FAIL: %s\n", message);
  return valid;
}
bool checkResult(const PublicDatasetResult& value, const char* message) {
  if (!value.ok()) std::fprintf(stderr, "DETAIL: %s\n", qPrintable(value.error));
  return check(value.ok(), message);
}
QString sha(const QByteArray& value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); }
QByteArray json(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool seed(const QString& root) {
  QJsonObject pets, skills;
  for (int i = 1; i <= 1000; ++i) {
    pets.insert(QString::number(i), QJsonObject{{"name", QStringLiteral("test-%1").arg(i)}, {"slots", QJsonObject{}}});
    skills.insert(QString::number(i), QJsonObject{{"name", "test"}, {"description", "TEST ONLY"}});
  }
  const QMap<QString, QByteArray> files{
    {"pet-detail-data.json", json({{"schema", 1}, {"pets", QJsonObject{{"1", QJsonObject{{"name", "TEST ONLY"}}}}}})},
    {"pet-skill-data.json", json({{"schema", 1}, {"pets", pets}, {"skills", skills}, {"entries", QJsonObject{{"x", QJsonObject{}}}}, {"buffs", QJsonObject{{"1", QJsonObject{}}}}})},
    {"shop-exchange-data.json", json({{"protocol", QJsonObject{{"extension", "test"}, {"getInfoCommand", "test"}}},
      {"shops", QJsonArray{QJsonObject{{"shopId", 1}, {"name", "TEST ONLY"}, {"goods", QJsonArray{QJsonObject{
        {"itemServerId", 1}, {"description", "TEST ONLY"}, {"cost", "4:1:1"}, {"enhanceType", "62"},
        {"unlock", ""}, {"shelfTime", "20260101"}, {"raceIds", QJsonArray{1}}}}}}}}})},
    {"routine-overview.json", json(RoutineOverviewCatalog::embeddedRoot())},
    {"pet-image-index.json", json({{"schemaVersion", 1}, {"images", QJsonObject{}}})},
    {"public-icon-index.json", json({{"schemaVersion", 1}, {"stargods", QJsonObject{}}})},
    {"battle-replay-catalog.json", battleReplayCatalogFixtureBytes(QStringLiteral("first"))},
  };
  for (auto file = files.begin(); file != files.end(); ++file)
    if (!write(QDir(root).filePath(QStringLiteral("catalog/") + file.key()), file.value())) return false;
  return write(QDir(root).filePath(QStringLiteral("data-tools/scripts/public_dataset_store.py")), "# TEST CHECKER IDENTITY ONLY\n");
}

struct FixtureCandidate { QString attempt; QString revision; QString directory; QJsonObject value; QJsonObject report; };
FixtureCandidate update(const QString& root, const QString& marker, const QString& key = QStringLiteral("routines")) {
  FixtureCandidate fixture;
  fixture.attempt = QUuid::createUuid().toString(QUuid::Id128);
  fixture.directory = QDir(root).filePath(QStringLiteral("public/transactions/") + fixture.attempt);
  const auto currentBytes = read(QDir(root).filePath(QStringLiteral("public/current.json")));
  const auto pointer = QJsonDocument::fromJson(currentBytes).object();
  const auto previous = pointer.value("revision").toString();
  auto manifest = QJsonDocument::fromJson(read(QDir(root).filePath(QStringLiteral("public/releases/%1/manifest.json").arg(previous)))).object();
  auto refs = manifest.value("components").toObject();
  auto ref = refs.value(key).toObject();
  auto data = QJsonDocument::fromJson(read(QDir(root).filePath(QStringLiteral("public/objects/%1/data").arg(ref.value("sha256").toString())))).object();
  data.insert("testOnlyMarker", marker);
  const auto raw = json(data);
  ref.insert("sha256", sha(raw)); ref.insert("bytes", raw.size()); ref.insert("format", "json");
  ref.insert("origin", QJsonObject{{"kind", "test-only-candidate"}});
  write(QDir(root).filePath(QStringLiteral("public/objects/%1/data").arg(sha(raw))), raw);
  refs.insert(key, ref); manifest.insert("components", refs);
  const auto manifestBytes = json(manifest); fixture.revision = sha(manifestBytes);
  write(QDir(root).filePath(QStringLiteral("public/releases/%1/manifest.json").arg(fixture.revision)), manifestBytes);
  QJsonArray effective;
  if (key == QStringLiteral("routines")) effective.append(key);
  else for (const auto* component : {"pets", "skills", "shop", "images", "icons", "battleReplay"}) effective.append(QLatin1String(component));
  QJsonObject results;
  for (const auto& component : effective) results.insert(component.toString(), QJsonObject{{"status", component.toString() == key ? "updated" : "unchanged"}});
  const QJsonObject checker{{"kind", "public-dataset-python"}, {"schema", 1}, {"sha256", sha(read(QDir(root).filePath(QStringLiteral("data-tools/scripts/public_dataset_store.py"))))}};
  const QJsonObject parserFiles{{"public_dataset_store.py", checker.value("sha256")}};
  fixture.report = {{"schema", 1}, {"attempt", fixture.attempt}, {"baseRevision", previous}, {"baseCurrentSha256", sha(currentBytes)},
      {"revision", fixture.revision}, {"components", refs}, {"attachments", manifest.value("attachments")}, {"requestedComponents", QJsonArray{key}},
      {"effectiveComponents", effective}, {"results", results}, {"checks", QJsonObject{{"objects", true}, {"dependencies", true}, {"sourceStable", true}}},
      {"checker", checker}, {"parserIdentity", QJsonObject{{"files", parserFiles}, {"sha256", sha(json(parserFiles))}, {"formatSchema", 1}}},
      {"qualityReports", QJsonObject{}}, {"changedComponents", QJsonArray{key}}, {"passed", true}};
  const auto reportBytes = json(fixture.report);
  write(fixture.directory + QStringLiteral("/validation.json"), reportBytes);
  fixture.value = {{"schema", 1}, {"attempt", fixture.attempt}, {"baseRevision", previous}, {"baseCurrentSha256", sha(currentBytes)},
      {"revision", fixture.revision}, {"requestedComponents", QJsonArray{key}}, {"effectiveComponents", effective}, {"results", results},
      {"validation", QJsonObject{{"passed", true}, {"manifestSha256", fixture.revision}, {"reportSha256", sha(reportBytes)}, {"checker", checker}}}};
  write(fixture.directory + QStringLiteral("/candidate.json"), json(fixture.value));
  return fixture;
}
void updateReport(FixtureCandidate& fixture) {
  const auto raw = json(fixture.report);
  write(fixture.directory + QStringLiteral("/validation.json"), raw);
  auto validation = fixture.value.value("validation").toObject(); validation.insert("reportSha256", sha(raw));
  fixture.value.insert("validation", validation);
  write(fixture.directory + QStringLiteral("/candidate.json"), json(fixture.value));
}
void replaceManifest(const QString& root, FixtureCandidate& fixture, const QJsonObject& components) {
  const QJsonObject manifest{{"schema", 1}, {"dependencySchema", 1}, {"components", components},
      {"attachments", fixture.report.value("attachments")}};
  const auto bytes = json(manifest); fixture.revision = sha(bytes);
  write(root + QStringLiteral("/public/releases/%1/manifest.json").arg(fixture.revision), bytes);
  fixture.report.insert("revision", fixture.revision); fixture.report.insert("components", components);
  fixture.value.insert("revision", fixture.revision);
  auto validation = fixture.value.value("validation").toObject(); validation.insert("manifestSha256", fixture.revision);
  fixture.value.insert("validation", validation);
}
void qualityRecord(FixtureCandidate& fixture, const QString& component, const QJsonObject& quality) {
  const auto bytes = json(quality); const auto file = QStringLiteral("quality/%1.json").arg(component);
  write(fixture.directory + QLatin1Char('/') + file, bytes);
  auto reports = fixture.report.value("qualityReports").toObject();
  reports.insert(component, QJsonObject{{"file", file}, {"sha256", sha(bytes)}, {"bytes", bytes.size()},
      {"candidateContentSha256", quality.value("candidateContentSha256")}});
  fixture.report.insert("qualityReports", reports); updateReport(fixture);
}
QByteArray packed(const QByteArray& payload) {
  const auto compressed = qCompress(payload, 9).mid(4);
  QByteArray output(56, '\0');
  output.replace(qsizetype(0), qsizetype(8), QByteArray("KQBRPK1\n", 8));
  qToBigEndian<quint64>(quint64(payload.size()), output.data() + 8);
  qToBigEndian<quint64>(quint64(compressed.size()), output.data() + 16);
  output.replace(24, 32, QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
  output.append(compressed); return output;
}
FixtureCandidate floatingQualityCandidate(const QString& root) {
  auto fixture = update(root, QStringLiteral("canonical floating tokens"), QStringLiteral("pets"));
  auto components = fixture.report.value("components").toObject();
  auto ref = components.value("pets").toObject();
  auto bytes = read(root + QStringLiteral("/public/objects/%1/data").arg(ref.value("sha256").toString()));
  // This array uses Python's actual sorted JSON spellings. QJson would turn
  // 1.0 and -0.0 into integers, invalidating an otherwise correct report.
  bytes.replace("\"testOnlyMarker\":", "\"testFloatValues\":[1.0,0.08,1e-05,1e+16,-0.0,0.0001],\"testOnlyMarker\":");
  ref.insert("sha256", sha(bytes)); ref.insert("bytes", bytes.size()); components.insert("pets", ref);
  write(root + QStringLiteral("/public/objects/%1/data").arg(sha(bytes)), bytes);
  const QJsonObject manifest{{"schema", 1}, {"dependencySchema", 1}, {"components", components}, {"attachments", fixture.report.value("attachments")}};
  const auto manifestBytes = json(manifest); fixture.revision = sha(manifestBytes);
  write(root + QStringLiteral("/public/releases/%1/manifest.json").arg(fixture.revision), manifestBytes);
  fixture.report.insert("revision", fixture.revision); fixture.report.insert("components", components);
  fixture.value.insert("revision", fixture.revision);
  auto validation = fixture.value.value("validation").toObject(); validation.insert("manifestSha256", fixture.revision);
  fixture.value.insert("validation", validation);
  const QJsonObject quality{{"schemaVersion", 1}, {"component", "pets"}, {"passed", true},
      {"candidateContentSha256", sha(bytes)}, {"findings", QJsonArray{}},
      {"summary", QJsonObject{{"errors", 0}, {"reviewRequired", 0}}}, {"decision", "validated-not-yet-applied"}};
  const auto qualityBytes = json(quality);
  write(fixture.directory + QStringLiteral("/quality/pets.json"), qualityBytes);
  fixture.report.insert("qualityReports", QJsonObject{{"pets", QJsonObject{{"file", "quality/pets.json"}, {"sha256", sha(qualityBytes)},
      {"bytes", qualityBytes.size()}, {"candidateContentSha256", sha(bytes)}}}});
  updateReport(fixture);
  return fixture;
}

FixtureCandidate tokenQualityCandidate(const QString& root, const QByteArray& payload,
                                       const QByteArray& canonical, bool compressed = false) {
  const auto key = compressed ? QStringLiteral("battleReplay") : QStringLiteral("pets");
  auto fixture = update(root, QStringLiteral("TEST ONLY token reader boundary"), key);
  auto components = fixture.report.value("components").toObject();
  auto ref = components.value(key).toObject();
  const auto bytes = compressed ? packed(payload) : payload;
  ref.insert("sha256", sha(bytes)); ref.insert("bytes", bytes.size());
  ref.insert("format", compressed ? QStringLiteral("brpack") : QStringLiteral("json"));
  components.insert(key, ref);
  write(root + QStringLiteral("/public/objects/%1/data").arg(sha(bytes)), bytes);
  replaceManifest(root, fixture, components);
  qualityRecord(fixture, key, {{"schemaVersion", 1}, {"component", key}, {"passed", true},
      {"candidateContentSha256", sha(canonical)}, {"findings", QJsonArray{}},
      {"summary", QJsonObject{{"errors", 0}, {"reviewRequired", 0}}}, {"decision", "validated-not-yet-applied"}});
  return fixture;
}

bool canonicalQualityBoundaries() {
  QTemporaryDir directory; const auto root = directory.path();
  if (!check(seed(root), "cannot seed token-reader fixture")) return false;
  PublicDatasetStore store(root);
  const auto migration = store.prepareMigration();
  if (!checkResult(migration, "token-reader baseline failed") ||
      !checkResult(store.commitPrepared(migration.prepared), "token-reader baseline commit failed")) return false;
  const QByteArray prefix("{\"pets\":{\"1\":{\"name\":\"TEST ONLY\"}},\"schema\":1,\"unicode\":{");
  const auto bmp = QByteArray::fromHex("ee8080"), supplementary = QByteArray::fromHex("f0908080");
  const auto canonical = prefix + '"' + bmp + "\":1,\"" + supplementary + "\":2}}";
  const auto unsorted = prefix + '"' + supplementary + "\":2,\"" + bmp + "\":1}}";
  const auto unicode = tokenQualityCandidate(root, unsorted, canonical);
  bool ok = checkResult(store.prepareCandidate(unicode.attempt), "Unicode keys were sorted by UTF-16 rather than Python codepoints");
  const QList<QByteArray> invalid{
      "{\"pets\":{},\"schema\":1,}", "{\"pets\":{},\"schema\":1,\"x\":[1,]}",
      "{\"pets\":{},\"schema\":1,\"schema\":2}", "[]", "{\"pets\":{},\"schema\":1} trailing",
      "{\"pets\":{},\"schema\":1,\"x\":\"\\x\"}",
      QByteArray("{\"pets\":{},\"schema\":1,\"x\":") + QByteArray(65, '[') + '0' + QByteArray(65, ']') + '}'};
  for (const auto& bytes : invalid) {
    const auto fixture = tokenQualityCandidate(root, bytes, bytes);
    ok &= check(!store.prepareCandidate(fixture.attempt).ok(), "token-only quality validation accepted invalid JSON or excessive depth");
  }
  // Highly compressible data tests the expanded string budget, independently
  // of the physical pack limit. It fails in the token reader before any fake
  // fidelity declaration could be considered. No oversized file is retained.
  {
    const auto payload = QByteArray("{\"x\":\"") + QByteArray(96 * 1024 * 1024, 'x') + "\"}";
    const auto fixture = tokenQualityCandidate(root, payload, QByteArray("irrelevant: must fail the string budget"), true);
    const auto rejected = store.prepareCandidate(fixture.attempt);
    ok &= check(!rejected.ok() && rejected.error.contains(QStringLiteral("结构预算")),
                "expanded replay strings escaped the original 96 MiB character budget");
  }
  return ok;
}

bool packedQualityAndParserReceipts() {
  QTemporaryDir directory; const auto root = directory.path();
  if (!check(seed(root), "cannot seed packed quality fixture")) return false;
  PublicDatasetStore store(root);
  auto migration = store.prepareMigration();
  if (!checkResult(migration, "packed quality baseline failed") ||
      !checkResult(store.commitPrepared(migration.prepared), "packed quality baseline commit failed")) return false;
  auto fixture = update(root, QStringLiteral("packed regression"), QStringLiteral("battleReplay"));
  const auto replay = battleReplayCatalogFixture(QStringLiteral("packed regression"));
  // Deliberate whitespace distinguishes actual pre-pack bytes from canonical
  // JSON, so the test cannot pass by checking only its semantic content hash.
  const auto payload = QJsonDocument(replay).toJson(QJsonDocument::Indented);
  const auto pack = packed(payload);
  const auto contentSha = sha(json(replay)), payloadSha = sha(payload), packSha = sha(pack);
  auto components = fixture.report.value("components").toObject();
  components.insert("battleReplay", QJsonObject{{"sha256", packSha}, {"bytes", pack.size()}, {"format", "brpack"},
      {"origin", QJsonObject{{"kind", "synthetic-packed-regression-test"}}}});
  write(root + QStringLiteral("/public/objects/%1/data").arg(packSha), pack);
  replaceManifest(root, fixture, components);
  const QByteArray nativeBytes("TEST ONLY native identity"), runtimeBytes("TEST ONLY runtime identity");
  write(root + QStringLiteral("/data-tools/scripts/KQBattleReplayUpdateCheck.exe"), nativeBytes);
  write(root + QStringLiteral("/data-tools/scripts/Qt6Core.dll"), runtimeBytes);
  const auto source = root + QStringLiteral("/test-only-source.json");
  write(source, payload);
  const auto evidenceFile = QStringLiteral("public/source-cache/evidence/%1/data").arg(payloadSha);
  write(root + QLatin1Char('/') + evidenceFile, payload);
  const auto fixtureSet = json({{"schemaVersion", 1}, {"cases", QJsonArray{QJsonObject{{"id", "TEST PACK BINDING"}, {"sha256", payloadSha}}}}});
  write(root + QStringLiteral("/data-tools/scripts/battle-replay-regression-manifest.json"), fixtureSet);
  const QJsonObject native{{"schemaVersion", 1}, {"mode", "candidate-regression"}, {"passed", true}, {"errors", QJsonArray{}},
      {"catalogFileSha256", payloadSha}, {"catalogSha256", payloadSha}, {"fixtureSetSha256", sha(fixtureSet)}, {"caseCount", 1},
      {"cases", QJsonArray{QJsonObject{{"id", "TEST PACK BINDING"}, {"fixtureSha256", payloadSha}, {"passed", true}}}}};
  const QJsonObject regression{{"passed", true}, {"candidateContentSha256", contentSha}, {"catalogFileSha256", payloadSha},
      {"catalogSha256", payloadSha}, {"processExitCode", 0}, {"errors", QJsonArray{}}, {"nativeReport", native},
      {"fixtureSetSha256", sha(fixtureSet)}, {"checkerSha256", sha(nativeBytes)}, {"runtimeSha256", QJsonObject{{"Qt6Core.dll", sha(runtimeBytes)}}}};
  const QJsonObject fidelity{{"schemaVersion", 1}, {"passed", true}, {"candidateContentSha256", contentSha}, {"findings", QJsonArray{}},
      {"inputs", QJsonArray{QJsonObject{{"role", "test-packed-source"}, {"path", source}, {"status", "read"}, {"sha256", payloadSha}, {"bytes", payload.size()}}}}};
  QJsonObject quality{{"schemaVersion", 1}, {"component", "battleReplay"}, {"passed", true}, {"candidateContentSha256", contentSha},
      {"findings", QJsonArray{}}, {"summary", QJsonObject{{"errors", 0}, {"reviewRequired", 0}}},
      {"decision", "validated-not-yet-applied"}, {"sourceFidelity", fidelity}, {"replayRegression", regression}};
  fixture.report.insert("evidenceFiles", QJsonArray{QJsonObject{{"component", "battleReplay"}, {"role", "test-packed-source"},
      {"originalPath", source}, {"file", evidenceFile}, {"sha256", payloadSha}, {"bytes", payload.size()}}});
  qualityRecord(fixture, QStringLiteral("battleReplay"), quality);
  bool ok = check(payloadSha != packSha && payloadSha != contentSha, "packed/raw/canonical fixture identities collapsed");
  auto prepared = store.prepareCandidate(fixture.attempt);
  ok &= checkResult(prepared, "packed object rejected the exact tested pre-pack JSON bytes");
  ok &= check(prepared.snapshot && prepared.snapshot->replay && prepared.snapshot->replay->sha256 == payloadSha,
              "packed pre-read did not bind the expanded original bytes");
  auto packRegression = regression, packNative = native;
  packRegression.insert("catalogFileSha256", packSha); packNative.insert("catalogFileSha256", packSha);
  packRegression.insert("nativeReport", packNative); quality.insert("replayRegression", packRegression);
  qualityRecord(fixture, QStringLiteral("battleReplay"), quality);
  ok &= checkResult(store.prepareCandidate(fixture.attempt), "regression against the actual brpack file was rejected");

  auto unrelatedRegression = regression, unrelatedNative = native;
  unrelatedRegression.insert("catalogFileSha256", sha(QByteArray("{\"unrelated\":true}")));
  unrelatedNative.insert("catalogFileSha256", unrelatedRegression.value("catalogFileSha256"));
  unrelatedRegression.insert("nativeReport", unrelatedNative); quality.insert("replayRegression", unrelatedRegression);
  qualityRecord(fixture, QStringLiteral("battleReplay"), quality);
  ok &= check(!store.prepareCandidate(fixture.attempt).ok(), "unrelated tested JSON hash was accepted for the packed candidate");
  auto wrongPayloadRegression = packRegression, wrongPayloadNative = packNative;
  wrongPayloadRegression.insert("catalogSha256", contentSha); wrongPayloadNative.insert("catalogSha256", contentSha);
  wrongPayloadRegression.insert("nativeReport", wrongPayloadNative); quality.insert("replayRegression", wrongPayloadRegression);
  qualityRecord(fixture, QStringLiteral("battleReplay"), quality);
  ok &= check(!store.prepareCandidate(fixture.attempt).ok(), "canonical-equivalent bytes replaced the actual expanded-byte binding");
  auto disagree = regression; disagree.insert("nativeReport", packNative); quality.insert("replayRegression", disagree);
  qualityRecord(fixture, QStringLiteral("battleReplay"), quality);
  ok &= check(!store.prepareCandidate(fixture.attempt).ok(), "outer/native reports disagreeing on the tested file were accepted");

  const auto current = store.prepareCurrent().snapshot;
  for (const auto& key : {QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("battleReplay")}) {
    auto parserOnly = update(root, QStringLiteral("parser receipt only"), key);
    auto refs = current->manifest.value("components").toObject();
    auto ref = refs.value(key).toObject(), origin = ref.value("origin").toObject();
    origin.insert("parserSha256", parserOnly.report.value("parserIdentity").toObject().value("sha256"));
    origin.insert("formatSchema", 1); ref.insert("origin", origin); refs.insert(key, ref);
    replaceManifest(root, parserOnly, refs);
    parserOnly.report.insert("changedComponents", QJsonArray{}); updateReport(parserOnly);
    const auto rejected = store.prepareCandidate(parserOnly.attempt);
    ok &= check(!rejected.ok() && rejected.error.contains(QStringLiteral("没有独立质量报告")),
                "unchanged core bytes acquired a new parser proof without an independent quality report");
    if (key == QStringLiteral("pets")) {
      const auto petBytes = read(current->components.value(key).path);
      const QJsonObject petQuality{{"schemaVersion", 1}, {"component", "pets"}, {"passed", true}, {"findings", QJsonArray{}},
          {"candidateContentSha256", sha(json(QJsonDocument::fromJson(petBytes).object()))},
          {"summary", QJsonObject{{"errors", 0}, {"reviewRequired", 0}}}, {"decision", "validated-not-yet-applied"}};
      qualityRecord(parserOnly, key, petQuality);
      ok &= checkResult(store.prepareCandidate(parserOnly.attempt), "verified unchanged bytes could not record the current parser identity");
    }
  }
  return ok;
}
bool lifecycle() {
  QTemporaryDir directory;
  const auto root = directory.path();
  bool ok = check(directory.isValid() && seed(root), "cannot seed isolated legacy catalogs");
  PublicDatasetStore store(root);
  ok &= check(store.prepareCurrent().status == PublicDatasetStatus::NotFound, "missing pointer was not distinguished from corrupt pointer");
  const auto legacyReplay = read(root + QStringLiteral("/catalog/battle-replay-catalog.json"));
  auto migration = store.prepareMigration();
  if (!checkResult(migration, "legacy migration preparation failed")) return false;
  ok &= check(migration.prepared && migration.snapshot->components.size() == 7 && !migration.snapshot->replay &&
              !QFileInfo::exists(root + QStringLiteral("/public/current.json")), "migration exposed a pointer before complete preparation");
  const auto firstRevision = migration.snapshot->revision;
  auto committed = store.commitPrepared(migration.prepared);
  ok &= checkResult(committed, "migration commit failed");
  ok &= check(committed.status == PublicDatasetStatus::DiskCommitted && committed.receipt.value("sessionApplied") == QJsonValue(false),
              "disk publication falsely claimed in-process application");
  const auto created = committed.receipt.value("createdUtc").toString();
  const auto applied = store.markSessionApplied(migration.prepared->attempt, firstRevision);
  ok &= checkResult(applied, "session application receipt failed");
  ok &= check(!created.isEmpty() && applied.receipt.value("createdUtc") == created,
              "session acknowledgement changed the check's immutable creation time");
  auto legacyReceipt = applied.receipt;
  legacyReceipt.remove("createdUtc"); legacyReceipt.insert("recordedUtc", "2020-01-02T03:04:05.000Z");
  write(root + QStringLiteral("/public/transactions/%1/receipt.json").arg(migration.prepared->attempt), json(legacyReceipt));
  const auto legacyAck = store.markSessionApplied(migration.prepared->attempt, firstRevision);
  ok &= checkResult(legacyAck, "legacy session receipt did not remain compatible");
  ok &= check(legacyAck.receipt.value("createdUtc") == QStringLiteral("2020-01-02T03:04:05.000Z") &&
              legacyAck.receipt.value("recordedUtc") != legacyAck.receipt.value("createdUtc"),
              "ack promoted an old receipt to a newly started check");
  const auto rawPet = migration.snapshot->components.value(QStringLiteral("pets")).origin.value("rawObject").toObject();
  ok &= check(rawPet.value("sha256") == sha(read(root + QStringLiteral("/catalog/pet-detail-data.json"))) &&
      read(root + QStringLiteral("/public/objects/%1/data").arg(rawPet.value("sha256").toString())) == read(root + QStringLiteral("/catalog/pet-detail-data.json")),
      "migration did not retain exact original overlay bytes independently of its legacy path");
  const auto objectsBefore = QDir(root + QStringLiteral("/public/objects")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
  auto same = store.prepareMigration();
  ok &= check(same.ok() && !same.prepared && same.snapshot->revision == firstRevision &&
      QDir(root + QStringLiteral("/public/objects")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == objectsBefore &&
      read(root + QStringLiteral("/catalog/battle-replay-catalog.json")) == legacyReplay, "repeat migration copied data or mutated legacy originals");
  const auto first = store.prepareCurrent().snapshot;
  auto loaded = store.loadReplay(first);
  ok &= checkResult(loaded, "deferred replay read failed");
  ok &= check(loaded.snapshot && loaded.snapshot->replay && !first->replay && loaded.snapshot->replay->root.value("testOnlyMarker") == "first", "loading replay mutated retained snapshot");

  const auto one = update(root, QStringLiteral("second")), two = update(root, QStringLiteral("concurrent"));
  write(one.directory + QStringLiteral("/request.json"), json({{"schema", 1}, {"attempt", one.attempt},
      {"createdUtc", "2021-02-03T04:05:06.000Z"}, {"requestedComponents", QJsonArray{"routines"}}}));
  auto preparedOne = store.prepareCandidate(one.attempt), preparedTwo = store.prepareCandidate(two.attempt);
  ok &= checkResult(preparedOne, "independent routine candidate did not prepare");
  ok &= checkResult(preparedTwo, "parallel candidate did not prepare before CAS");
  if (!preparedOne.ok() || !preparedTwo.ok()) return false;
  ok &= check(preparedOne.receipt.value("createdUtc") == QStringLiteral("2021-02-03T04:05:06.000Z"),
              "receipt failed to retain the matching request's actual start time");
  ok &= checkResult(store.commitPrepared(preparedOne.prepared), "candidate commit failed");
  const auto pointerAfterCommit = read(root + QStringLiteral("/public/current.json"));
  ok &= checkResult(store.prepareCandidate(one.attempt), "repeated CLI candidate could not recover committed result");
  ok &= checkResult(store.commitPrepared(preparedOne.prepared), "repeated native commit could not recover committed result");
  ok &= check(read(root + QStringLiteral("/public/current.json")) == pointerAfterCommit,
              "repeated commit changed generation or discarded the true previous version");
  ok &= check(store.commitPrepared(preparedTwo.prepared).status == PublicDatasetStatus::Conflict, "stale candidate overwrote another publication");
  auto rollback = store.prepareRollback();
  ok &= checkResult(rollback, "rollback preparation failed");
  ok &= checkResult(store.commitPrepared(rollback.prepared), "rollback commit failed");
  ok &= check(store.prepareCurrent().snapshot->revision == firstRevision &&
      store.commitPrepared(preparedTwo.prepared).status == PublicDatasetStatus::Conflict, "ABA pointer reuse accepted a stale candidate");
  ok &= check(first->routine->root.value("testOnlyMarker").isUndefined() &&
      preparedOne.snapshot->routine->root.value("testOnlyMarker") == "second", "new publication changed retained old values");
  const auto restoreSecond = store.prepareRestoreRevision(one.revision);
  ok &= checkResult(restoreSecond, "explicit imported revision failed native restore validation");
  ok &= checkResult(store.commitPrepared(restoreSecond.prepared), "explicit restore did not use the normal publication path");
  ok &= check(store.prepareCurrent().snapshot->revision == one.revision, "explicit restore published the wrong complete release");
  const auto restoreFirst = store.prepareRestoreRevision(firstRevision);
  ok &= checkResult(restoreFirst, "restoring original release did not prepare");
  ok &= checkResult(store.commitPrepared(restoreFirst.prepared), "restoring original release did not commit");
  const auto restoreObjectPath = preparedOne.snapshot->components.value(QStringLiteral("routines")).path;
  const auto restoreObjectBytes = read(restoreObjectPath);
  const auto pointerBeforeRejectedRestore = read(root + QStringLiteral("/public/current.json"));
  write(restoreObjectPath, "{}");
  ok &= check(!store.prepareRestoreRevision(one.revision).ok() && read(root + QStringLiteral("/public/current.json")) == pointerBeforeRejectedRestore,
              "a damaged backup object changed current during restore validation");
  write(restoreObjectPath, restoreObjectBytes);
  ok &= check(store.prepareCandidate(QStringLiteral("../current")).status == PublicDatasetStatus::Invalid, "path traversal attempt accepted");
  auto missingReport = update(root, QStringLiteral("quality absent"), QStringLiteral("pets"));
  ok &= check(!store.prepareCandidate(missingReport.attempt).ok(), "changed core accepted candidate.passed without independent quality evidence");
  auto floating = floatingQualityCandidate(root);
  ok &= checkResult(store.prepareCandidate(floating.attempt), "native quality reader did not preserve Python float token identity");
  const auto extraParser = root + QStringLiteral("/data-tools/scripts/extra-unlisted.py");
  write(extraParser, "# TEST unlisted parser\n");
  ok &= check(!store.prepareCandidate(floating.attempt).ok(), "candidate omitted an actual parser file from its identity closure");
  QFile::remove(extraParser);
  auto falseDigest = floating;
  auto badReports = falseDigest.report.value("qualityReports").toObject(); auto badQuality = badReports.value("pets").toObject();
  badQuality.insert("candidateContentSha256", QString(64, QLatin1Char('0'))); badReports.insert("pets", badQuality);
  falseDigest.report.insert("qualityReports", badReports); updateReport(falseDigest);
  ok &= check(!store.prepareCandidate(falseDigest.attempt).ok(), "quality report declaration replaced actual candidate digest checking");
  auto corruptReport = update(root, QStringLiteral("report tampered"));
  write(corruptReport.directory + QStringLiteral("/validation.json"), "{}");
  ok &= check(!store.prepareCandidate(corruptReport.attempt).ok(), "changed report bytes passed their old hash");
  auto falseRange = update(root, QStringLiteral("undeclared change"));
  falseRange.report.insert("changedComponents", QJsonArray{}); updateReport(falseRange);
  ok &= check(!store.prepareCandidate(falseRange.attempt).ok(), "omitted changed component escaped validation");
  auto changedObject = update(root, QStringLiteral("tampered object"));
  auto preparedObject = store.prepareCandidate(changedObject.attempt);
  if (!checkResult(preparedObject, "object tamper fixture failed to prepare")) return false;
  const auto objectPath = preparedObject.snapshot->components.value(QStringLiteral("routines")).path;
  const auto originalObject = read(objectPath);
  write(objectPath, "{}");
  ok &= check(!store.commitPrepared(preparedObject.prepared).ok() && store.prepareCurrent().snapshot->revision == firstRevision,
              "commit accepted an immutable object changed since pre-read");
  write(objectPath, originalObject);
  QLockFile lock(root + QStringLiteral("/public/publish.lock"));
  ok &= check(lock.tryLock(0) && store.commitPrepared(preparedObject.prepared).status == PublicDatasetStatus::Busy, "publication ignored process-shared lock");
  lock.unlock();
  const auto pointerPath = root + QStringLiteral("/public/current.json");
  const auto pointerBytes = read(pointerPath);
  write(pointerPath, "{invalid");
  ok &= check(store.prepareCurrent().status == PublicDatasetStatus::Invalid && !store.prepareMigration().ok() && read(pointerPath) == "{invalid",
              "corrupt current pointer silently migrated back to old files");
  write(pointerPath, pointerBytes);
  QString inspectError; const auto inspected = store.inspect(&inspectError);
  ok &= check(inspectError.isEmpty() && inspected.value("runtimeDeletionEnabled") == QJsonValue(false) && QFileInfo::exists(objectPath),
              "inspection deleted potentially retained objects");
  return ok;
}

bool failureBoundaries() {
  bool ok = true;
  for (const auto* name : {"before-commit", "before-pointer", "after-pointer", "before-receipt"}) {
    QTemporaryDir directory; const auto root = directory.path(); seed(root);
    PublicDatasetStore store(root); const auto migration = store.prepareMigration();
    if (!checkResult(migration, "boundary migration failed")) return false;
    if (!checkResult(store.commitPrepared(migration.prepared), "boundary initial commit failed")) return false;
    const auto old = read(root + QStringLiteral("/public/current.json"));
    const auto fixture = update(root, QLatin1String(name));
    const auto prepared = store.prepareCandidate(fixture.attempt);
    if (!checkResult(prepared, "boundary candidate failed")) return false;
    PublicDatasetStoreOptions options;
    options.allowBoundary = [name](const QString& boundary) { return boundary != QLatin1String(name); };
    const auto result = PublicDatasetStore(root, options).commitPrepared(prepared.prepared);
    const bool after = QString::fromLatin1(name).startsWith(QStringLiteral("after")) || QString::fromLatin1(name) == QStringLiteral("before-receipt");
    if (after) {
      ok &= check(result.status == PublicDatasetStatus::DiskCommitted && !result.error.isEmpty(), "post-pointer failure claimed rollback of committed disk state");
      const auto recovered = store.prepareCurrent();
      ok &= check(recovered.ok() && recovered.snapshot->revision == fixture.revision && recovered.receipt.value("diskCommitted") == QJsonValue(true) &&
                  recovered.receipt.value("sessionApplied") == QJsonValue(false) && recovered.receipt.value("attempt") == fixture.attempt,
                  "startup did not recover interrupted commit receipt");
      ok &= checkResult(store.markSessionApplied(fixture.attempt, fixture.revision), "recovered candidate could not be marked applied");
    } else {
      ok &= check(!result.ok() && read(root + QStringLiteral("/public/current.json")) == old, "pre-pointer failure exposed partial new data");
    }
  }
  QTemporaryDir directory; seed(directory.path());
  std::atomic_bool cancelled{true};
  PublicDatasetStoreOptions options; options.cancelled = &cancelled;
  auto stopped = PublicDatasetStore(directory.path(), options).prepareMigration();
  ok &= check(stopped.status == PublicDatasetStatus::Cancelled && !QFileInfo::exists(directory.path() + QStringLiteral("/public/current.json")),
              "cancelled migration exposed partial state");
  return ok;
}

bool migrationCompatibility() {
  QTemporaryDir directory; const auto root = directory.path();
  if (!check(seed(root), "cannot seed compatibility migration")) return false;
  const auto petsPath = root + QStringLiteral("/catalog/pet-detail-data.json");
  auto oldPet = QJsonDocument::fromJson(read(petsPath)).object();
  const auto petSource = [](const QString& version) {
    return QJsonObject{{"petDictionaryVersion", version}, {"resources", QJsonObject{{"petDictionaryUpdate", QJsonObject{
      {"url", QStringLiteral("https://aoqi.100bt.com/play/pet/petdictionarydataupdate~%1.swf").arg(version)}}}}}};
  };
  oldPet.insert("source", petSource(QStringLiteral("20200101001")));
  auto newPet = oldPet; newPet.insert("source", petSource(QStringLiteral("20210101001")));
  newPet.insert("pets", QJsonObject{{"1", QJsonObject{{"name", "NEW TEST BASELINE"}}}});
  write(petsPath, json(oldPet));
  write(root + QStringLiteral("/data-tools/scripts/baseline/pet-detail-data.json"), json(newPet));
  const auto shopPath = root + QStringLiteral("/catalog/shop-exchange-data.json");
  auto oldShop = QJsonDocument::fromJson(read(shopPath)).object();
  const auto shopSource = [](const QString& version) {
    return QJsonObject{{"shopVersion", version}, {"resources", QJsonObject{{"shop", QJsonObject{
      {"url", CatalogLegacyCompatibility::officialShopUrl(version)}}}}}};
  };
  oldShop.insert("source", shopSource(QStringLiteral("20200101001")));
  auto newShop = oldShop; newShop.insert("source", shopSource(QStringLiteral("20210101001")));
  auto shops = newShop.value("shops").toArray(); auto shop = shops.first().toObject();
  shop.insert("name", "NEW TEST SHOP"); shops[0] = shop; newShop.insert("shops", shops);
  write(shopPath, json(oldShop));
  write(root + QStringLiteral("/data-tools/scripts/baseline/shop-exchange-data.json"), json(newShop));
  PublicDatasetStore store(root);
  const auto migrated = store.prepareMigration();
  if (!checkResult(migrated, "date-guard migration did not prepare")) return false;
  bool ok = check(migrated.snapshot->detail->root.value("pets").toObject().value("1").toObject().value("name") == "NEW TEST BASELINE" &&
      migrated.snapshot->shop->root.value("shops").toArray().first().toObject().value("name") == "NEW TEST SHOP",
      "migration reinstated an explicitly older official cache over the existing embedded-date guard");
  for (const auto& key : {QStringLiteral("pets"), QStringLiteral("shop")}) {
    const auto origin = migrated.snapshot->components.value(key).origin;
    const auto path = key == QStringLiteral("pets") ? petsPath : shopPath;
    const auto raw = origin.value("rawObject").toObject(), embedded = origin.value("embeddedObject").toObject();
    ok &= check(origin.value("kind") == "legacy-superseded-by-embedded" && raw.value("sha256") == sha(read(path)) &&
        read(root + QStringLiteral("/public/objects/%1/data").arg(raw.value("sha256").toString())) == read(path) &&
        QFileInfo::exists(root + QStringLiteral("/public/objects/%1/data").arg(embedded.value("sha256").toString())),
        "migration lost original or embedded bytes while retaining newer effective data");
  }
  // A first attempt that reached only candidatePassed may be retried; a
  // missing pointer after a recorded commit must never reset to legacy files.
  const auto retry = store.prepareMigration();
  ok &= checkResult(retry, "an interrupted first preparation prevented a safe retry");
  ok &= check(retry.snapshot->revision == migrated.snapshot->revision, "retry copied unchanged migration data into a new version");
  ok &= checkResult(store.commitPrepared(retry.prepared), "compatibility migration commit failed");
  const auto pointerPath = root + QStringLiteral("/public/current.json");
  ok &= check(QFile::remove(pointerPath), "could not simulate a lost current pointer");
  const auto lost = store.prepareMigration();
  ok &= check(lost.status == PublicDatasetStatus::Invalid && !QFileInfo::exists(pointerPath) && read(petsPath) == json(oldPet),
              "missing pointer after a recorded commit silently rebuilt from legacy catalogs");
  const auto explicitRestore = store.prepareRestoreRevision(migrated.snapshot->revision);
  ok &= checkResult(explicitRestore, "explicit restoration could not recover a root without its current pointer");
  ok &= checkResult(store.commitPrepared(explicitRestore.prepared), "explicit restoration failed its normal CAS commit");
  ok &= check(store.prepareCurrent().snapshot->revision == migrated.snapshot->revision, "explicit empty-root restore chose another release");
  return ok;
}

bool corePublication() {
  QTemporaryDir directory; const auto root = directory.path();
  if (!check(seed(root), "cannot seed Core publication fixture")) return false;
  StorageService storage(root);
  CatalogIoOptions options; options.publicDatasetMode = true;
  CatalogIoService service(&storage, options);
  bool ok = true;
  int publications = 0;
  QObject::connect(&service, &CatalogIoService::publicDatasetChanged, &service, [&](PublicDatasetSnapshotHandle value) {
    ++publications;
    ok &= check(value && service.publicDatasetSnapshot() == value &&
        PetDetailCatalog::instance().snapshot() == value->detail && PetSkillCatalog::instance().snapshot() == value->skill &&
        ShopExchangeCatalog::instance().snapshot() == value->shop && RoutineOverviewCatalog::instance().snapshot() == value->routine,
        "an observer saw individually updated compatibility catalogs instead of one complete bundle");
  });
  QJsonObject receipt;
  const auto run = [&](const std::function<quint64()>& action, bool expectedSuccess) {
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    bool received = false, success = false; quint64 expected = 0;
    const auto connection = QObject::connect(&service, &CatalogIoService::publicDatasetFinished, &loop,
        [&](quint64 id, bool applied, const QString& message, const QJsonObject& value) {
      if (id != expected) return;
      received = true; success = applied; receipt = value;
      if (expectedSuccess && !applied) std::fprintf(stderr, "CORE DETAIL: %s\n", qPrintable(message));
      loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    expected = action();
    if (expected) { timeout.start(15000); loop.exec(); }
    QObject::disconnect(connection);
    return check(expected && received && success == expectedSuccess, "Core public operation failed or never completed");
  };
  ok &= run([&] { return service.requestPublicReload(); }, true);
  const auto first = service.publicDatasetSnapshot();
  ok &= check(first && receipt.value("diskCommitted") == QJsonValue(true) && receipt.value("sessionApplied") == QJsonValue(true),
              "Core finished before persisting sessionApplied");
  if (first) {
    const auto fixture = update(root, QStringLiteral("Core second"));
    ok &= run([&] { return service.requestPublicCandidate(fixture.attempt); }, true);
    const auto second = service.publicDatasetSnapshot();
    ok &= check(second && second->revision != first->revision && second->routine->root.value("testOnlyMarker") == "Core second" &&
        first->routine->root.value("testOnlyMarker").isUndefined() && second->detail == first->detail && second->skill == first->skill,
        "Core did not retain old values or reuse unchanged handles");
    const int before = publications;
    const auto corrupted = update(root, QStringLiteral("Core corrupt"));
    write(corrupted.directory + QStringLiteral("/validation.json"), "{}");
    ok &= run([&] { return service.requestPublicCandidate(corrupted.attempt); }, false);
    ok &= check(service.publicDatasetSnapshot() == second && publications == before, "invalid candidate replaced part of the live Core bundle");
    const auto pointerPath = root + QStringLiteral("/public/current.json"); const auto pointer = read(pointerPath);
    write(pointerPath, "broken");
    ok &= run([&] { return service.requestPublicReload(); }, false);
    ok &= check(service.publicDatasetSnapshot() == second && publications == before, "invalid current pointer cleared the live bundle");
    write(pointerPath, pointer);
    ok &= run([&] { return service.requestPublicRollback(); }, true);
    ok &= check(service.publicDatasetSnapshot()->revision == first->revision && second->routine->root.value("testOnlyMarker") == "Core second",
                "Core rollback changed a retained newer snapshot");
    // Restore-to-current deliberately leaves the pointer (and its original
    // attempt) untouched. Core must acknowledge the new restore transaction,
    // rather than accidentally marking only the pointer's older attempt.
    const auto beforeRestore = read(root + QStringLiteral("/public/current.json"));
    PublicDatasetStore store(root);
    const auto sameRestore = store.prepareRestoreRevision(service.publicDatasetSnapshot()->revision);
    ok &= checkResult(sameRestore, "same-version restore did not prepare");
    if (sameRestore.prepared) {
      ok &= checkResult(store.commitPrepared(sameRestore.prepared), "same-version restore did not commit its receipt");
      const auto restoreAttempt = sameRestore.prepared->attempt;
      const auto restoreReceiptPath = root + QStringLiteral("/public/transactions/%1/receipt.json").arg(restoreAttempt);
      ok &= check(read(root + QStringLiteral("/public/current.json")) == beforeRestore &&
          QJsonDocument::fromJson(read(restoreReceiptPath)).object().value("sessionApplied") == QJsonValue(false),
          "same-version restore changed the pointer or claimed application before Core");
      ok &= run([&] { return service.requestPublicReload(restoreAttempt); }, true);
      const auto acknowledged = QJsonDocument::fromJson(read(restoreReceiptPath)).object();
      ok &= check(acknowledged.value("attempt") == restoreAttempt && acknowledged.value("sessionApplied") == QJsonValue(true) &&
          read(root + QStringLiteral("/public/current.json")) == beforeRestore,
          "Core acknowledged the old pointer attempt instead of the new same-version restore receipt");
    }
  }
  ok &= check(service.requestOfficialUpdate(CatalogKind::Shop) == 0 && service.requestOfficialUpdate(CatalogKind::Routine) == 0,
              "normal versioned mode accepted old direct-write import");
  service.close(); storage.shutdown();
  return ok;
}
}

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const bool ok = lifecycle() & failureBoundaries() & migrationCompatibility() & packedQualityAndParserReceipts() &
      canonicalQualityBoundaries() & corePublication();
  return ok ? 0 : 1;
}
