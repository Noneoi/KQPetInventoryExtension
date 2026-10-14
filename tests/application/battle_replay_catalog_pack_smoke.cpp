#include "application/catalog/battle_replay_catalog_pack.h"
#include "application/catalog/battle_replay_catalog_validation.h"
#include "application/catalog/catalog_io_service.h"
#include "fixtures/battle_replay_catalog_fixture.h"
#include "support/catalog_test_support.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonParseError>
#include <QList>
#include <QMap>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <utility>

namespace {
using BattleReplayCatalogPack::Decoder;
using BattleReplayCatalogPack::StepResult;

bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}

QByteArray sha256(const QByteArray& bytes) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(sha256(bytes).toHex());
}

// Fixture encoding is deliberately independent of the production decoder.
// qCompress prefixes four size bytes; the remaining bytes are an RFC1950 stream.
void setBigEndian64(QByteArray& bytes, qsizetype offset, quint64 value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes[offset++] = static_cast<char>((value >> shift) & 0xff);
}

QByteArray packWithBody(const QByteArray& raw, const QByteArray& body) {
  QByteArray pack(BattleReplayCatalogPack::HeaderBytes, '\0');
  pack.replace(0, 8, QByteArrayLiteral("KQBRPK1\n"));
  setBigEndian64(pack, 8, static_cast<quint64>(raw.size()));
  setBigEndian64(pack, 16, static_cast<quint64>(body.size()));
  pack.replace(24, 32, sha256(raw));
  pack.append(body);
  return pack;
}

QByteArray makePack(const QByteArray& raw) {
  return packWithBody(raw, qCompress(raw, 9).mid(4));
}

void matchBodyLength(QByteArray& pack) {
  setBigEndian64(pack, 16,
      static_cast<quint64>(pack.size() - BattleReplayCatalogPack::HeaderBytes));
}

bool roundTrip(const QByteArray& raw, const char* label) {
  Decoder decoder(makePack(raw));
  bool ok = check(decoder.takeJson().isEmpty() && decoder.sha256().isEmpty(),
                  "decoder exposed content before its first step");
  StepResult result = StepResult::More;
  int steps = 0;
  while (result == StepResult::More && steps < 8192) {
    result = decoder.step();
    ++steps;
    if (result == StepResult::More)
      ok &= check(decoder.takeJson().isEmpty() && decoder.sha256().isEmpty(),
                  "decoder exposed unverified partial content between steps");
  }
  const qint64 minimumSteps = (raw.size() + BattleReplayCatalogPack::OutputChunkBytes - 1) /
      BattleReplayCatalogPack::OutputChunkBytes;
  const bool matches = result == StepResult::Done && decoder.error().isEmpty() &&
      steps >= minimumSteps && decoder.sha256() == sha256(raw) && decoder.takeJson() == raw;
  if (!matches)
    std::fprintf(stderr, "round trip %s: steps=%d error=%s\n",
        label, steps, decoder.error().toUtf8().constData());
  ok &= check(matches, label);
  ok &= check(decoder.step() == StepResult::Done && decoder.sha256() == sha256(raw),
              "successful decoder did not retain its terminal state and raw digest");
  return ok;
}

bool rejected(QByteArray pack, const char* label) {
  Decoder decoder(std::move(pack));
  StepResult result = StepResult::More;
  for (int i = 0; result == StepResult::More && i < 8192; ++i) result = decoder.step();
  const bool failed = result == StepResult::Failed && !decoder.error().isEmpty() &&
      decoder.takeJson().isEmpty() && decoder.sha256().isEmpty() &&
      decoder.step() == StepResult::Failed;
  if (!failed) std::fprintf(stderr, "invalid container accepted or not safely rejected: %s\n", label);
  return check(failed, label);
}

bool decoderCases() {
  bool ok = true;
  const QByteArray raw = QByteArrayLiteral("{\"testOnly\":\"lossless bytes\",\"value\":7}\n");
  const QByteArray valid = makePack(raw);
  ok &= roundTrip(raw, "small container did not round-trip exactly");

  // Highly compressible input exercises the OUTPUT budget even when the whole
  // compressed stream fits in one read. Bytes need not be JSON at this layer.
  const QByteArray many(BattleReplayCatalogPack::OutputChunkBytes * 3 + 17, 'x');
  ok &= roundTrip(many, "large expansion bypassed the per-step output budget");
  QByteArray noisy(BattleReplayCatalogPack::OutputChunkBytes * 3 + 257, '\0');
  quint32 state = 0x6d2b79f5u;
  for (qsizetype i = 0; i < noisy.size(); ++i) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    noisy[i] = static_cast<char>(state & 0xff);
  }
  ok &= check(makePack(noisy).size() > BattleReplayCatalogPack::OutputChunkBytes * 2,
              "multi-read fixture compressed too small to exercise input boundaries");
  ok &= roundTrip(noisy, "multi-read binary content was truncated or changed");
  {
    Decoder cancelled(makePack(many));
    ok &= check(cancelled.step() == StepResult::More && cancelled.takeJson().isEmpty(),
                "large decoder could not yield for cancellation");
    // Destruction between steps is the owner's cancellation operation.
  }
  ok &= roundTrip(raw, "a cancelled decoder affected a subsequent decoder");

  ok &= rejected({}, "empty file");
  ok &= rejected(valid.left(BattleReplayCatalogPack::HeaderBytes - 1), "incomplete header");
  QByteArray changed = valid;
  changed[6] = '2';
  ok &= rejected(changed, "unsupported container version");

  for (const auto declared : {quint64(0), static_cast<quint64>(raw.size() - 1),
       static_cast<quint64>(raw.size() + 1),
       static_cast<quint64>(BattleReplayCatalogPack::MaximumJsonBytes) + 1,
       std::numeric_limits<quint64>::max()}) {
    changed = valid; setBigEndian64(changed, 8, declared);
    ok &= rejected(changed, "false, zero or over-budget raw length");
  }
  const quint64 bodyBytes = static_cast<quint64>(valid.size() - BattleReplayCatalogPack::HeaderBytes);
  for (const auto declared : {quint64(0), bodyBytes - 1, bodyBytes + 1,
       std::numeric_limits<quint64>::max()}) {
    changed = valid; setBigEndian64(changed, 16, declared);
    ok &= rejected(changed, "false, zero or overflowing compressed length");
  }
  changed = valid; changed[24] = static_cast<char>(changed.at(24) ^ 1);
  ok &= rejected(changed, "intact zlib stream with a false raw SHA256");
  changed = valid;
  changed[BattleReplayCatalogPack::HeaderBytes] = '\0';
  ok &= rejected(changed, "invalid zlib stream header");
  changed = valid; changed[changed.size() - 1] = static_cast<char>(changed.back() ^ 1);
  ok &= rejected(changed, "corrupt zlib stream checksum");
  changed = valid.left(valid.size() - 1); matchBodyLength(changed);
  ok &= rejected(changed, "truncated zlib footer with a matching container length");
  changed = valid.left(BattleReplayCatalogPack::HeaderBytes + bodyBytes / 2);
  matchBodyLength(changed);
  ok &= rejected(changed, "truncated deflate data with a matching container length");
  changed = valid; changed.append('x');
  ok &= rejected(changed, "undeclared trailing bytes");
  matchBodyLength(changed);
  ok &= rejected(changed, "declared trailing bytes after the only zlib stream");
  changed = valid; changed.append(valid.mid(BattleReplayCatalogPack::HeaderBytes));
  matchBodyLength(changed);
  ok &= rejected(changed, "a second concatenated zlib stream");

  const auto body = valid.mid(BattleReplayCatalogPack::HeaderBytes);
  ok &= rejected(packWithBody(raw, body.mid(2, body.size() - 6)), "raw deflate is not RFC1950");
  // Generated independently with Python zlib.compressobj(level=9,
  // zdict=b'dictionary-prefix:'). This is a valid preset-dictionary stream.
  const QByteArray dictionaryRaw("dictionary-prefix:dictionary-prefix:payload");
  const QByteArray dictionaryBody = QByteArray::fromHex("78f94592072c4b2142a420b132273f3105007c701141");
  ok &= rejected(packWithBody(dictionaryRaw, dictionaryBody), "preset dictionary is unsupported");
  {
    QByteArray oversized(BattleReplayCatalogPack::MaximumPackBytes + 1, '\0');
    oversized.replace(0, BattleReplayCatalogPack::HeaderBytes,
                      valid.left(BattleReplayCatalogPack::HeaderBytes));
    matchBodyLength(oversized);
    ok &= rejected(std::move(oversized), "actual file size exceeds the compressed budget");
  }
  return ok;
}

QString jsonPath(const QString& root) {
  return QDir(root).filePath(QStringLiteral("catalog/battle-replay-catalog.json"));
}

QString packPath(const QString& root) {
  return QDir(root).filePath(QStringLiteral("catalog/battle-replay-catalog.brpack"));
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QString marker(const BattleReplayCatalogHandle& snapshot) {
  return snapshot ? snapshot->root.value(QStringLiteral("testOnlyMarker")).toString() : QString{};
}

bool reload(CatalogIoService& service, QString* error = nullptr, int timeout = 10000) {
  return runCatalogRequest(&service, CatalogKind::BattleReplay, CatalogRequestMode::Reload, error, timeout);
}

bool packageSelectionAndRetention(const QString& root) {
  StorageService storage(root);
  CatalogIoService service(&storage);
  const QString compressedPath = packPath(root), legacyPath = jsonPath(root);
  const QByteArray firstRaw = battleReplayCatalogFixtureBytes(QStringLiteral("package-first"));
  const QByteArray firstPack = makePack(firstRaw);
  const QByteArray legacyRaw = battleReplayCatalogFixtureBytes(QStringLiteral("legacy-json"));
  int publications = 0;
  QObject::connect(&service, &CatalogIoService::catalogUpdated, &service,
      [&](CatalogKind kind, quint64) { if (kind == CatalogKind::BattleReplay) ++publications; });
  bool ok = check(writeCatalogFixture(legacyPath, legacyRaw) &&
      writeCatalogFixture(compressedPath, firstPack) && reload(service), "valid local package did not load");
  const auto first = service.battleReplaySnapshot();
  if (!check(first != nullptr, "local package did not publish a catalog")) {
    service.close(); storage.shutdown(); return false;
  }
  ok &= check(!first->embedded && marker(first) == QStringLiteral("package-first") &&
      QDir::cleanPath(first->sourcePath) == QDir::cleanPath(compressedPath) &&
      first->sha256 == digest(firstRaw) && first->sha256 != digest(firstPack) &&
      first->revision > 0 && first->loadedAt.isValid() && first->warning.isEmpty() && publications == 1,
      "package did not outrank legacy JSON or did not retain the raw-content identity");
  ok &= check(reload(service) && service.battleReplaySnapshot() == first && publications == 1,
              "identical package caused a replacement snapshot or publication");

  QByteArray corrupt = firstPack;
  corrupt[24] = static_cast<char>(corrupt.at(24) ^ 1);
  auto wrongSchema = battleReplayCatalogFixture(QStringLiteral("wrong-schema"));
  wrongSchema.remove(QStringLiteral("petMechanics"));
  const QList<QByteArray> badPackages{
      corrupt, QByteArray("not a replay package"), makePack(QByteArray("{\"format\":3,")),
      makePack(QJsonDocument(wrongSchema).toJson(QJsonDocument::Compact))};
  for (const auto& bad : badPackages) {
    QString error;
    ok &= check(writeCatalogFixture(compressedPath, bad) && !reload(service, &error) && !error.isEmpty(),
                "invalid package was accepted or failed without a reason");
    ok &= check(service.battleReplaySnapshot() == first && publications == 1 &&
        readFile(compressedPath) == bad && readFile(legacyPath) == legacyRaw,
        "invalid package replaced the good snapshot, used stale JSON, or rewrote either source");
  }
  {
    QFile oversized(compressedPath);
    const qint64 tooLarge = BattleReplayCatalogPack::MaximumPackBytes + 1;
    const bool madeOversized = oversized.open(QIODevice::WriteOnly) && oversized.resize(tooLarge);
    oversized.close();
    QString error;
    ok &= check(madeOversized && !reload(service, &error) && !error.isEmpty() &&
        service.battleReplaySnapshot() == first && publications == 1 &&
        QFileInfo(compressedPath).size() == tooLarge && readFile(legacyPath) == legacyRaw,
        "oversized external package bypassed its budget or damaged retained data");
  }
  const QByteArray secondRaw = battleReplayCatalogFixtureBytes(QStringLiteral("package-second"));
  ok &= check(writeCatalogFixture(compressedPath, makePack(secondRaw)) && reload(service),
              "a valid package could not recover after failed reads");
  const auto second = service.battleReplaySnapshot();
  ok &= check(second && second != first && marker(second) == QStringLiteral("package-second") &&
      second->sha256 == digest(secondRaw) && second->revision > first->revision && publications == 2 &&
      marker(first) == QStringLiteral("package-first") && first->sha256 == digest(firstRaw),
      "package replacement mutated a retained catalog or failed to advance its revision");
  ok &= check(QFile::remove(compressedPath) && reload(service), "legacy JSON did not load without a package");
  const auto legacy = service.battleReplaySnapshot();
  ok &= check(legacy && marker(legacy) == QStringLiteral("legacy-json") && !legacy->embedded &&
      QDir::cleanPath(legacy->sourcePath) == QDir::cleanPath(legacyPath) &&
      legacy->sha256 == digest(legacyRaw) && publications == 3 &&
      !QFileInfo::exists(compressedPath) && readFile(legacyPath) == legacyRaw,
      "legacy JSON compatibility changed its bytes or created a package");
  service.close();
  ok &= check(storage.shutdown(), "package selection storage did not drain");
  return ok;
}

struct CatalogShape {
  QStringList keys;
  QMap<QString, qsizetype> collectionSizes;
  QMap<QString, int> types;
};

CatalogShape catalogShape(const QJsonObject& root) {
  CatalogShape result;
  result.keys = root.keys();
  const auto record = [&](const QString& path, const QJsonValue& value) {
    result.types.insert(path, static_cast<int>(value.type()));
    if (value.isObject()) result.collectionSizes.insert(path, value.toObject().size());
    if (value.isArray()) result.collectionSizes.insert(path, value.toArray().size());
  };
  for (auto it = root.begin(); it != root.end(); ++it) record(it.key(), it.value());
  // These contain additional named tables, rather than one row per pet/skill.
  for (const auto* key : {"replayCultivation", "replaySupplements"}) {
    const auto tables = root.value(QLatin1String(key)).toObject();
    for (auto it = tables.begin(); it != tables.end(); ++it)
      record(QString::fromLatin1(key) + QLatin1Char('/') + it.key(), it.value());
  }
  return result;
}

struct SourceExpectation {
  QString sha256;
  qint64 bytes = 0;
  CatalogShape shape;
};

bool inspectJsonBytes(const QByteArray& bytes, SourceExpectation* expected) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  const auto root = document.object();
  if (!check(error.error == QJsonParseError::NoError && document.isObject() &&
      root.value(QStringLiteral("format")).toInt() == 3 &&
      root.value(QStringLiteral("skills")).toObject().size() > 20000 &&
      root.value(QStringLiteral("buffs")).toObject().size() > 30000,
      "real source JSON is incomplete or is a synthetic test fixture")) return false;
  expected->sha256 = digest(bytes);
  expected->bytes = bytes.size();
  expected->shape = catalogShape(root);
  // Only hashes and collection counts survive this call. Do not retain another
  // complete 94 MB JSON document while CatalogIoService prepares its snapshot.
  return true;
}

bool inspectSource(const QString& path, SourceExpectation* expected) {
  QFile source(path);
  if (!check(source.open(QIODevice::ReadOnly) && source.size() > 32 * 1024 * 1024 &&
      source.size() <= BattleReplayCatalogPack::MaximumJsonBytes, "real source JSON is missing or outside its expected budget"))
    return false;
  const auto bytes = source.readAll();
  return check(bytes.size() == source.size() && source.error() == QFileDevice::NoError,
               "real source JSON could not be read completely") && inspectJsonBytes(bytes, expected);
}

bool inspectUpdatedPack(const QString& path, SourceExpectation* expected) {
  QFile source(path);
  if (!check(source.open(QIODevice::ReadOnly) && source.size() > BattleReplayCatalogPack::HeaderBytes &&
      source.size() <= BattleReplayCatalogPack::MaximumPackBytes, "updated real package is unavailable or oversized")) return false;
  Decoder decoder(source.readAll());
  StepResult result;
  do { result = decoder.step(); } while (result == StepResult::More);
  return check(result == StepResult::Done, "updated real package failed container verification") &&
      inspectJsonBytes(decoder.takeJson(), expected);
}

bool matchesSource(const BattleReplayCatalogHandle& snapshot, const SourceExpectation& expected) {
  if (!snapshot) return false;
  const auto actual = catalogShape(snapshot->root);
  return snapshot->sha256 == expected.sha256 &&
      snapshot->root.value(QStringLiteral("format")).toInt() == 3 &&
      actual.keys == expected.shape.keys && actual.types == expected.shape.types &&
      actual.collectionSizes == expected.shape.collectionSizes;
}

bool exportedEmbeddedBaseline(const QString& root, const SourceExpectation& expected) {
  const QString output = QDir(root).filePath(QStringLiteral("baseline/battle-replay-catalog.brpack"));
  const QByteArray previous = makePack(battleReplayCatalogFixtureBytes(QStringLiteral("previous-baseline")));
  if (!check(writeCatalogFixture(output, previous), "prior baseline fixture could not be written")) return false;
  std::atomic_bool cancelled{true};
  QString error = BattleReplayCatalogPack::exportEmbeddedBaseline(output, &cancelled);
  bool ok = check(!error.isEmpty() && readFile(output) == previous,
                  "pre-cancelled baseline export changed an existing target or reported success");
  const QString absentDirectory = QDir(root).filePath(QStringLiteral("cancelled-new/nested"));
  error = BattleReplayCatalogPack::exportEmbeddedBaseline(
      QDir(absentDirectory).filePath(QStringLiteral("battle-replay-catalog.brpack")), &cancelled);
  ok &= check(!error.isEmpty() && !QFileInfo::exists(QDir(root).filePath(QStringLiteral("cancelled-new"))),
              "pre-cancelled baseline export created its previously absent output directory");

  cancelled.store(false);
  QElapsedTimer elapsed; elapsed.start();
  error = BattleReplayCatalogPack::exportEmbeddedBaseline(output, &cancelled);
  const qint64 exportMilliseconds = elapsed.elapsed();
  if (!error.isEmpty()) std::fprintf(stderr, "embedded baseline export: %s\n", error.toUtf8().constData());
  const QByteArray packed = readFile(output);
  if (!check(error.isEmpty() && packed.size() > BattleReplayCatalogPack::HeaderBytes &&
      packed.size() <= BattleReplayCatalogPack::MaximumPackBytes && packed != previous,
      "real embedded baseline did not replace the old target with a bounded package")) return false;
  Decoder decoder(packed);
  StepResult result = StepResult::More;
  int steps = 0;
  while (result == StepResult::More && steps < 8192) { result = decoder.step(); ++steps; }
  const QByteArray decoded = decoder.takeJson();
  const bool verified = result == StepResult::Done && decoder.error().isEmpty() &&
      decoded.size() == expected.bytes &&
      QString::fromLatin1(decoder.sha256().toHex()) == expected.sha256 && digest(decoded) == expected.sha256;
  if (!verified) std::fprintf(stderr, "embedded baseline decode: %s\n", decoder.error().toUtf8().constData());
  ok &= check(verified, "exported embedded baseline did not decode to the exact source bytes and SHA256");
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(decoded, &parseError);
  const auto catalog = document.object();
  const QString validationError = validateBattleReplayCatalog(catalog);
  const auto shape = catalogShape(catalog);
  if (!validationError.isEmpty())
    std::fprintf(stderr, "embedded baseline format: %s\n", validationError.toUtf8().constData());
  ok &= check(parseError.error == QJsonParseError::NoError && document.isObject() && validationError.isEmpty() &&
      shape.keys == expected.shape.keys && shape.types == expected.shape.types &&
      shape.collectionSizes == expected.shape.collectionSizes,
      "exported embedded baseline failed complete format 3 validation or lost catalog tables");

  cancelled.store(true);
  error = BattleReplayCatalogPack::exportEmbeddedBaseline(output, &cancelled);
  ok &= check(!error.isEmpty() && readFile(output) == packed,
              "pre-cancelled re-export changed the successfully generated baseline");
  QStringList files;
  QDirIterator entries(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
  while (entries.hasNext()) files.append(QDir(root).relativeFilePath(entries.next()));
  files.sort();
  ok &= check(files == QStringList{QStringLiteral("baseline/battle-replay-catalog.brpack")},
              "baseline export left expanded JSON, a cancelled target or temporary files on disk");
  std::printf("replay embedded baseline: packed_bytes=%lld raw_bytes=%lld sha256=%s export_ms=%lld decode_steps=%d\n",
      static_cast<long long>(packed.size()), static_cast<long long>(decoded.size()),
      expected.sha256.toLatin1().constData(), static_cast<long long>(exportMilliseconds), steps);
  return ok;
}

bool realPythonPackage(const QString& root, const QString& sourcePack, const SourceExpectation& expected) {
  const QString destination = packPath(root);
  if (!check(QFileInfo(sourcePack).isFile() && QFileInfo(sourcePack).size() > 0 &&
      QFileInfo(sourcePack).size() <= BattleReplayCatalogPack::MaximumPackBytes &&
      QDir().mkpath(QFileInfo(destination).absolutePath()) && QFile::copy(sourcePack, destination),
      "real Python-generated package could not be copied to the temporary data root")) return false;
  const QByteArray originalPack = readFile(destination);
  StorageService storage(root);
  CatalogIoService service(&storage);
  int heartbeats = 0;
  qint64 maximumHeartbeatGap = 0, lastHeartbeat = 0;
  QElapsedTimer elapsed; elapsed.start();
  QTimer heartbeat;
  heartbeat.setInterval(1);
  QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
    const qint64 now = elapsed.elapsed();
    maximumHeartbeatGap = std::max(maximumHeartbeatGap, now - lastHeartbeat);
    lastHeartbeat = now; ++heartbeats;
  });
  heartbeat.start();
  QString error;
  const bool loaded = reload(service, &error, 60000);
  const qint64 loadMilliseconds = elapsed.elapsed();
  maximumHeartbeatGap = std::max(maximumHeartbeatGap, loadMilliseconds - lastHeartbeat);
  heartbeat.stop();
  if (!loaded) std::fprintf(stderr, "real package load: %s\n", error.toUtf8().constData());
  const auto snapshot = service.battleReplaySnapshot();
  bool ok = check(loaded && matchesSource(snapshot, expected) && !snapshot->embedded &&
      snapshot->warning.isEmpty() && snapshot->revision > 0 && snapshot->loadedAt.isValid() &&
      QDir::cleanPath(snapshot->sourcePath) == QDir::cleanPath(destination) && heartbeats > 1,
      "real package did not preserve the complete catalog/hash or blocked the caller event loop");
  ok &= check(readFile(destination) == originalPack && !QFileInfo::exists(jsonPath(root)),
              "real package loading rewrote the package or materialized a JSON file");
  const auto count = [&](const char* key) { return expected.shape.collectionSizes.value(QLatin1String(key)); };
  std::printf("replay package: packed_bytes=%lld raw_bytes=%lld sha256=%s top_keys=%lld "
      "pets=%lld skills=%lld buffs=%lld elapsed_ms=%lld gui_heartbeat_pulses=%d "
      "max_gui_heartbeat_gap_ms=%lld max_io_slice_us=%lld\n",
      static_cast<long long>(originalPack.size()), static_cast<long long>(expected.bytes),
      expected.sha256.toLatin1().constData(), static_cast<long long>(expected.shape.keys.size()),
      static_cast<long long>(count("pets")), static_cast<long long>(count("skills")),
      static_cast<long long>(count("buffs")), static_cast<long long>(loadMilliseconds), heartbeats,
      static_cast<long long>(maximumHeartbeatGap), static_cast<long long>(service.stats().maximumSliceMicroseconds));
  service.close();
  ok &= check(storage.shutdown(), "real package storage did not drain");
  return ok;
}

bool initialBadPackageFallback(const QString& root, const SourceExpectation& expected) {
  StorageService storage(root);
  CatalogIoService service(&storage);
  const QByteArray legacyRaw = battleReplayCatalogFixtureBytes(QStringLiteral("must-not-use-stale-json"));
  QByteArray bad = makePack(battleReplayCatalogFixtureBytes(QStringLiteral("broken-package")));
  bad[24] = static_cast<char>(bad.at(24) ^ 1);
  bool ok = check(writeCatalogFixture(packPath(root), bad) && writeCatalogFixture(jsonPath(root), legacyRaw),
                  "initial broken package fixtures could not be written");
  QString error;
  const bool loaded = reload(service, &error, 60000);
  if (!loaded) std::fprintf(stderr, "initial bad-package fallback: %s\n", error.toUtf8().constData());
  const auto snapshot = service.battleReplaySnapshot();
  ok &= check(loaded && matchesSource(snapshot, expected) && snapshot->embedded &&
      snapshot->sourcePath.startsWith(QStringLiteral(":/kqpet/")) && !snapshot->warning.isEmpty() &&
      marker(snapshot).isEmpty() && readFile(packPath(root)) == bad && readFile(jsonPath(root)) == legacyRaw,
      "first bad package did not visibly fall back to complete embedded data or used stale JSON");
  service.close();
  ok &= check(storage.shutdown(), "initial fallback storage did not drain");
  return ok;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  if (application.arguments().size() != 3 && application.arguments().size() != 4) {
    std::fprintf(stderr, "usage: KQBattleReplayCatalogPackSmoke <generated.brpack> <source.json> [updated.brpack]\n");
    return 2;
  }
  QTemporaryDir root;
  if (!check(root.isValid(), "temporary package test root unavailable")) return 1;
  bool ok = decoderCases();
  ok &= packageSelectionAndRetention(root.filePath(QStringLiteral("selection")));
  SourceExpectation expected;
  const bool inspected = inspectSource(application.arguments().at(2), &expected);
  ok &= inspected;
  if (inspected) {
    ok &= exportedEmbeddedBaseline(root.filePath(QStringLiteral("export")), expected);
    ok &= realPythonPackage(root.filePath(QStringLiteral("real")), application.arguments().at(1), expected);
    ok &= initialBadPackageFallback(root.filePath(QStringLiteral("fallback")), expected);
  }
  if (application.arguments().size() == 4) {
    SourceExpectation updated;
    const QString path = application.arguments().at(3);
    const bool valid = inspectUpdatedPack(path, &updated);
    ok &= valid;
    if (valid) ok &= realPythonPackage(root.filePath(QStringLiteral("updated-real")), path, updated);
  }
  if (ok) std::puts("PASS: replay package bounds, integrity, yielding, atomic embedded export, priority and full real catalog");
  return ok ? 0 : 1;
}
