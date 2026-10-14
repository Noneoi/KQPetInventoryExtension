#include "application/catalog/battle_replay_catalog_pack.h"
#include "application/catalog/battle_replay_catalog_validation.h"
#include "domain/battle_replay.h"
#include "domain/battle_replay_presentation.h"
#include "domain/battle_replay_state.h"
#include "tools/battle_replay_update_audit.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <algorithm>
#include <cstdio>

// This executable uses the same QtCore domain and catalog readers as the
// workbench. It never loads a GUI or executes downloaded game code. Golden
// observations belong to the reviewed fixture manifest, never the candidate.
namespace {
QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray compact(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QJsonValue optional(std::optional<qint64> value) { return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null); }
QJsonValue optional(std::optional<bool> value) { return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null); }
QJsonArray strings(const QStringList& list) { return QJsonArray::fromStringList(list); }

bool read(const QString& path, qint64 limit, QByteArray& bytes, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) {
    error = QStringLiteral("Cannot read input or input exceeds its size limit"); return false;
  }
  bytes = file.readAll();
  if (bytes.size() != file.size()) { error = QStringLiteral("Incomplete input read"); return false; }
  return true;
}
bool object(const QByteArray& bytes, QJsonObject& result, QString& error) {
  QJsonParseError parsed;
  const auto doc = QJsonDocument::fromJson(bytes, &parsed);
  if (parsed.error != QJsonParseError::NoError || !doc.isObject()) {
    error = QStringLiteral("Input is not a JSON object"); return false;
  }
  result = doc.object(); return true;
}

QJsonObject stateProjection(const QMap<QString, BattleReplay::Fighter>& state) {
  QJsonObject result;
  for (const auto& f : state) {
    QJsonArray members; QJsonObject progress;
    for (const auto& m : BattleReplay::recordedBuffMembers(f))
      members.append(QJsonArray{m.id, m.source, m.sourceTeam, m.path});
    for (const auto& p : f.teamProgress)
      progress[p.buffId] = QJsonObject{{"current", optional(p.current)}, {"maximum", optional(p.maximum)},
        {"previous", optional(p.previous)}, {"delta", optional(p.delta)}, {"skillEffected", optional(p.skillEffected)},
        {"path", p.path}, {"raw", p.raw}};
    // Names/descriptions are official mutable content. Their exact attachment
    // is checked by source-fidelity validation, not a historical text freeze.
    result[f.key] = QJsonObject{{"key", f.key}, {"player", f.player}, {"id", f.id}, {"team", f.team},
      {"x", f.x}, {"y", f.y}, {"playerX", f.playerX}, {"playerY", f.playerY}, {"representer", f.representer},
      {"raceId", f.raceId}, {"faceId", f.faceId}, {"originRaceId", f.originRaceId},
      {"originFighterKey", f.originFighterKey}, {"originRelation", f.originRelation},
      {"normalSkill", f.normalSkill}, {"uniqueSkill", f.uniqueSkill}, {"superUniqueSkill", f.superUniqueSkill},
      {"attributes", strings(f.attributes)}, {"attributesKnown", f.attributesKnown},
      {"summoned", f.summoned}, {"clone", f.clone}, {"cloneCalled", f.cloneCalled}, {"chaser", f.chaser},
      {"entered", f.entered}, {"entryKnown", f.entryKnown}, {"playerRemoved", f.playerRemoved},
      {"hp", optional(f.hp)}, {"maxHp", optional(f.maxHp)}, {"shield", optional(f.shield)},
      {"power", optional(f.power)}, {"maxPower", optional(f.maxPower)},
      {"buffs", strings(f.buffs)}, {"initialBuffs", strings(f.initialBuffs)}, {"buffMembers", members},
      {"buffsKnown", f.buffsKnown}, {"teamProgress", progress}};
  }
  return result;
}
QJsonObject effectProjection(const BattleReplay::Effect& e) {
  return {{"kind", e.kind}, {"source", e.source}, {"target", e.target}, {"cause", e.cause},
    {"causeId", e.causeId}, {"metric", e.metric}, {"path", e.path}, {"raw", e.raw},
    {"before", optional(e.before)}, {"after", optional(e.after)}, {"delta", optional(e.delta)},
    {"recordedBefore", optional(e.recordedBefore)}, {"recordedAfter", optional(e.recordedAfter)},
    {"recordedDelta", optional(e.recordedDelta)}, {"changed", e.changed}, {"moment", e.moment},
    {"buffSource", e.buffSource}, {"buffSourceTeam", e.buffSourceTeam}, {"buffSourcePath", e.buffSourcePath},
    {"buffWaitType", e.buffWaitType}, {"buffPlaybackBatch", e.buffPlaybackBatch},
    {"afterWaitCallback", e.afterWaitCallback}, {"buffWaitCondition", e.buffWaitCondition},
    {"visibilityCommand", e.visibilityCommand}, {"restoresDisplayLayers", e.restoresDisplayLayers},
    {"buffOperation", e.buffOperation}};
}
QJsonObject actionProjection(const BattleReplay::Step& step) {
  QJsonArray moments, events, references; QJsonObject queues, queuePlayers, rounds;
  for (auto it = step.actionQueues.cbegin(); it != step.actionQueues.cend(); ++it)
    queues[QString::number(it.key())] = strings(it.value());
  for (auto it = step.actionQueuePlayers.cbegin(); it != step.actionQueuePlayers.cend(); ++it)
    queuePlayers[QString::number(it.key())] = it.value();
  for (auto it = step.playerRounds.cbegin(); it != step.playerRounds.cend(); ++it)
    rounds[QString::number(it.key())] = it.value();
  for (const auto& e : step.events) events.append(QJsonArray{e.kind, e.key, e.moment});
  for (const auto& r : step.unlinkedReferences)
    references.append(QJsonObject{{"kind", r.kind}, {"key", r.key}, {"player", r.player},
      {"id", r.id}, {"team", r.team}, {"path", r.path}, {"raw", r.raw}, {"numbers", r.numbers}});
  for (const auto& m : step.moments) {
    QJsonArray effects;
    for (const auto& e : m.effects) effects.append(effectProjection(e));
    moments.append(QJsonObject{{"phase", m.phase}, {"part", m.part}, {"actor", m.actor},
      {"groupIndex", m.groupIndex}, {"effects", effects}, {"state", stateProjection(m.state)}});
  }
  return {{"path", step.path}, {"actor", step.actor}, {"round", step.round}, {"schedule", step.schedule},
    {"loggedSkillId", step.loggedSkillId}, {"playbackSkillId", step.playbackSkillId},
    {"counterSkillId", step.counterSkillId}, {"playbackKind", step.playbackKind},
    {"testingSkillId", step.testingSkillId}, {"lastSkillStatus", step.lastSkillStatus},
    {"isLastRound", step.isLastRound}, {"moments", moments}, {"events", events},
    {"state", stateProjection(step.state)}, {"unlinkedReferences", references},
    {"actionQueues", queues}, {"actionQueuePlayers", queuePlayers}, {"playerRounds", rounds},
    {"fromNormalQueue", optional(step.fromNormalQueue)}, {"overRoundLimit", optional(step.overRoundLimit)},
    {"batchId", step.batchId}, {"lineIndex", step.lineIndex}, {"parallelBatch", step.parallelBatch}};
}
QJsonArray cueProjection(const BattleReplay::Step& step, const QJsonObject& catalog) {
  QJsonArray result;
  for (qsizetype i = 0; i < step.moments.size(); ++i) {
    const auto& moment = step.moments[i];
    const auto& before = i > 0 ? step.moments[i - 1].state : moment.state;
    for (const auto& cue : BattleReplay::clientPresentationCues(step, moment, before, catalog))
      result.append(QJsonArray{i, cue.kind, cue.recipient, cue.source, cue.target, cue.attributeId, strings(cue.evidence)});
  }
  return result;
}

struct Observation {
  QJsonObject golden, detail, consumed;
  QStringList errors;
};
Observation observe(const QByteArray& bytes, const QJsonObject& catalog) {
  Observation out;
  const auto replay = BattleReplay::parse(bytes, catalog);
  if (!replay.error.isEmpty()) { out.errors.append(replay.error); return out; }
  if (replay.original != bytes || replay.root != QJsonDocument::fromJson(bytes).object())
    out.errors.append(QStringLiteral("Original fixture or raw report fields were not retained exactly"));
  const auto audit = auditBattleReplayEffects(replay);
  if (audit.errors() != 0) out.errors.append(QStringLiteral("Domain effects fail their raw-value/state invariant audit"));
  QCryptographicHash ordered(QCryptographicHash::Sha256), cues(QCryptographicHash::Sha256), seeks(QCryptographicHash::Sha256);
  QJsonArray checkpoints; QJsonObject coverage; QSet<QString> skills, buffs, pets;
  auto count = [&](const QString& key, qint64 n = 1) { coverage[key] = coverage[key].toInteger() + n; };
  qint64 moments = 0, rebuilds = 0;
  if (!replay.steps.isEmpty()) for (const auto& f : replay.steps.first().state) {
    if (f.summoned) count(QStringLiteral("summonedFighters"));
    if (f.clone) count(QStringLiteral("cloneFighters"));
    if (f.summoned && !f.entered) count(QStringLiteral("waitingSummonedFighters"));
  }
  QList<QByteArray> snapshots;
  for (const auto& step : replay.steps) {
    const auto projection = compact(actionProjection(step));
    ordered.addData(projection); ordered.addData(QByteArrayLiteral("\n"));
    const auto cueBytes = QJsonDocument(cueProjection(step, catalog)).toJson(QJsonDocument::Compact);
    cues.addData(step.path.toUtf8()); cues.addData(QByteArrayLiteral("\n")); cues.addData(cueBytes); cues.addData(QByteArrayLiteral("\n"));
    const auto stateBytes = compact(stateProjection(step.state)); snapshots.append(stateBytes);
    checkpoints.append(QJsonObject{{"path", step.path}, {"sha256", digest(projection)}, {"stateSha256", digest(stateBytes)}});
    for (const auto& f : step.state) {
      pets.insert(f.raceId); pets.insert(f.originRaceId);
      skills.insert(f.normalSkill); skills.insert(f.uniqueSkill); skills.insert(f.superUniqueSkill);
      for (const auto& id : f.buffs) buffs.insert(id);
    }
    skills.insert(step.loggedSkillId); skills.insert(step.playbackSkillId); skills.insert(step.counterSkillId);
    if (step.playbackKind == QStringLiteral("normal")) count(QStringLiteral("normalActions"));
    if (step.playbackKind == QStringLiteral("summon")) count(QStringLiteral("summonSkillActions"));
    if (step.raw.toObject()[QStringLiteral("trl")].toArray().size() > 1) count(QStringLiteral("multiTargetActions"));
    if (BattleReplay::valueText(step.raw.toObject()[QStringLiteral("epbi")]) == QStringLiteral("200195")) {
      count(QStringLiteral("execution200195"));
      if (step.playbackKind != QStringLiteral("team-simple")) out.errors.append(QStringLiteral("Recorded execution 200195 lost its verified team-simple playback"));
    }
    moments += step.moments.size();
    for (const auto& e : step.effects) {
      if (e.kind == QStringLiteral("buff")) buffs.insert(e.causeId);
      if (e.restoresDisplayLayers && e.causeId == QStringLiteral("30098")) count(QStringLiteral("restoreDisplay30098"));
      if (e.kind == QStringLiteral("appearance")) count(QStringLiteral("appearances"));
      if (e.kind == QStringLiteral("appearance") && e.cause == QStringLiteral("buff"))
        out.errors.append(QStringLiteral("Display-only Buff was incorrectly promoted into an entry event"));
      if (e.kind == QStringLiteral("transform")) count(QStringLiteral("transforms"));
      if (e.kind == QStringLiteral("observation") && e.cause == QStringLiteral("record") &&
          e.raw.isObject() && e.raw.toObject()[QStringLiteral("t")].toInt() == 3)
        count(QStringLiteral("recordedSummons"));
      if (e.kind == QStringLiteral("value") && e.delta && *e.delta != 0) {
        if (e.metric == QStringLiteral("生命")) count(*e.delta < 0 ? QStringLiteral("hpDecreases") : QStringLiteral("hpIncreases"));
        if (e.metric == QStringLiteral("护盾")) count(*e.delta < 0 ? QStringLiteral("shieldDecreases") : QStringLiteral("shieldIncreases"));
        if (e.metric == QStringLiteral("气势")) count(*e.delta < 0 ? QStringLiteral("powerDecreases") : QStringLiteral("powerIncreases"));
      }
    }
    for (const auto& e : step.events) {
      if (e.kind == QStringLiteral("生命归零")) count(QStringLiteral("zeroHpEvents"));
      if (e.kind == QStringLiteral("生命恢复")) count(QStringLiteral("hpRestoredEvents"));
    }
    // Reconstruct every independent action again from its actual initial
    // observation. This exercises the same path used to rebuild command
    // boundaries without treating a final snapshot as an initial state.
    if (!step.moments.isEmpty()) {
      auto fresh = step;
      auto state = step.moments.first().state;
      fresh.moments.clear(); fresh.effects.clear(); fresh.events.clear(); fresh.keyChanges.clear();
      fresh.affected.clear(); fresh.details.clear(); fresh.unlinkedReferences.clear();
      QStringList issues;
      BattleReplay::advanceAction(fresh, state, catalog, issues);
      ++rebuilds;
      // advanceAction does not own Step::state (the parse composition does).
      fresh.state = state;
      if (compact(actionProjection(fresh)) != projection)
        out.errors.append(QStringLiteral("Rebuilding an action from its initial observation changed the ordered projection at ") + step.path);
    }
  }
  // Seeking the frozen domain snapshots is intentionally independent of GUI
  // animation. Forward/backward/random access must return the same snapshots.
  for (qsizetype i = snapshots.size(); i-- > 0;) {
    const auto actual = compact(stateProjection(replay.steps[i].state));
    if (actual != snapshots[i]) out.errors.append(QStringLiteral("Backward snapshot access changed state"));
    seeks.addData(actual); seeks.addData(QByteArrayLiteral("\n"));
  }
  for (qsizetype i = 0; i < snapshots.size(); i += 2) {
    if (compact(stateProjection(replay.steps[i].state)) != snapshots[i]) out.errors.append(QStringLiteral("Random snapshot access changed state"));
  }
  auto retain = [&](const QString& table, const QSet<QString>& ids, const QStringList& fields) {
    QJsonObject values;
    for (const auto& id : ids) {
      if (id.isEmpty() || id == QStringLiteral("0")) continue;
      const auto row = catalog[table].toObject()[id].toObject(); QJsonObject selected;
      for (const auto& field : fields) if (row.contains(field)) selected[field] = row[field];
      values[id] = QJsonObject{{"present", !row.isEmpty()}, {"values", selected}};
    }
    out.consumed[table] = values;
  };
  retain(QStringLiteral("skills"), skills, {QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("attackType"), QStringLiteral("playbackAvailable")});
  retain(QStringLiteral("buffs"), buffs, {QStringLiteral("nm"), QStringLiteral("mess"), QStringLiteral("beffect"), QStringLiteral("buffWaitType")});
  retain(QStringLiteral("pets"), pets, {QStringLiteral("name"), QStringLiteral("identity"), QStringLiteral("maxPower"), QStringLiteral("desc")});
  out.golden = {{"actions", replay.actions}, {"steps", replay.steps.size()}, {"moments", moments}, {"rawFields", replay.fields.size()},
    {"effectsSha256", audit.effectsSha256}, {"orderedProjectionSha256", QString::fromLatin1(ordered.result().toHex())},
    {"clientCuesSha256", QString::fromLatin1(cues.result().toHex())},
    {"backwardSnapshotsSha256", QString::fromLatin1(seeks.result().toHex())},
    {"finalStateSha256", snapshots.isEmpty() ? QString() : digest(snapshots.last())}, {"coverage", coverage}};
  out.detail = {{"rawInvariantAudit", audit.report()}, {"actionRebuilds", rebuilds},
    {"snapshotAccessChecks", snapshots.size() + (snapshots.size() + 1) / 2},
    {"issueCount", replay.issues.size()}, {"checkpoints", checkpoints}};
  return out;
}

bool writeReport(const QString& path, const QJsonObject& report) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  return file.write(bytes) == bytes.size() && file.commit();
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  const auto args = application.arguments();
  auto arg = [&](const QString& name) { const auto i = args.indexOf(name); return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString(); };
  const auto catalogPath = arg(QStringLiteral("--catalog")), reportPath = arg(QStringLiteral("--report"));
  const auto external = arg(QStringLiteral("--fixtures"));
  const bool observeOnly = args.contains(QStringLiteral("--observe-baseline"));
  if (catalogPath.isEmpty() || reportPath.isEmpty() || (observeOnly && external.isEmpty())) {
    std::fputs("usage: KQBattleReplayUpdateCheck --catalog candidate.brpack|json --report result.json [--fixtures manifest.json] [--observe-baseline]\n", stderr); return 2;
  }
  QElapsedTimer elapsed; elapsed.start();
  QJsonObject report{{"schemaVersion", 1}, {"passed", false}, {"mode", observeOnly ? "baseline-observation" : "candidate-regression"}};
  QJsonArray errors, cases; QString error; QByteArray raw, manifestBytes;
  auto fail = [&](const QString& message) { errors.append(message); report["errors"] = errors; report["cases"] = cases;
    report["elapsedMs"] = elapsed.elapsed(); writeReport(reportPath, report); std::fputs("Replay regression rejected; see the validation report.\n", stderr); return 1; };
  if (!read(catalogPath, BattleReplayCatalogPack::MaximumJsonBytes, raw, error)) return fail(error);
  report["catalogFileSha256"] = digest(raw);
  if (raw.startsWith(QByteArrayLiteral("KQBRPK1\n"))) {
    BattleReplayCatalogPack::Decoder decoder(std::move(raw));
    auto status = BattleReplayCatalogPack::StepResult::More;
    while (status == BattleReplayCatalogPack::StepResult::More) status = decoder.step();
    if (status != BattleReplayCatalogPack::StepResult::Done) return fail(decoder.error());
    raw = decoder.takeJson();
  }
  report["catalogSha256"] = digest(raw);
  QJsonObject catalog;
  if (!object(raw, catalog, error)) return fail(error);
  const auto validation = validateBattleReplayCatalog(catalog);
  if (!validation.isEmpty()) return fail(validation);
  raw.clear(); raw.squeeze();
  const auto manifestPath = external.isEmpty() ? QStringLiteral(":/kqpet/replay-regression/manifest.json") : external;
  if (!read(manifestPath, 2 * 1024 * 1024, manifestBytes, error)) return fail(error);
  report["fixtureSetSha256"] = digest(manifestBytes);
  QJsonObject manifest;
  if (!object(manifestBytes, manifest, error) || manifest["schemaVersion"].toInt() != 1 ||
      manifest["projectionSchema"].toString() != QStringLiteral("qtcore-domain-v1") || manifest["cases"].toArray().isEmpty())
    return fail(QStringLiteral("Unsupported or empty regression manifest"));
  const auto entries = manifest["cases"].toArray();
  report["fixtureVersion"] = manifest["fixtureVersion"];
  report["caseCount"] = entries.size();
  report["scope"] = manifest["scope"];
  if (entries.size() > 12) return fail(QStringLiteral("Regression fixture count exceeds its bound"));
  for (const auto& value : entries) {
    const auto item = value.toObject(); const auto file = item["file"].toString();
    if (file.isEmpty() || QFileInfo(file).fileName() != file || file.contains(QLatin1Char(':')))
      return fail(QStringLiteral("Fixture file must be a basename within the manifest directory"));
    const auto path = external.isEmpty() ? QStringLiteral(":/kqpet/replay-regression/") + file : QFileInfo(external).dir().filePath(file);
    QByteArray fixture;
    if (!read(path, 2 * 1024 * 1024, fixture, error)) return fail(error);
    if (digest(fixture) != item["sha256"].toString()) return fail(QStringLiteral("Fixture bytes do not match the reviewed manifest"));
    const auto observed = observe(fixture, catalog);
    QJsonArray differences;
    const auto expected = item["expected"].toObject();
    if (!observeOnly) {
      if (expected.isEmpty()) differences.append(QStringLiteral("Missing reviewed expectations"));
      for (auto it = observed.golden.begin(); it != observed.golden.end(); ++it)
        if (expected[it.key()] != it.value()) differences.append(it.key());
      if (observed.golden.isEmpty()) differences.append(QStringLiteral("No domain observation"));
    }
    const bool passed = observed.errors.isEmpty() && differences.isEmpty();
    QJsonObject result{{"id", item["id"]}, {"fixtureSha256", digest(fixture)}, {"passed", passed},
      {"observed", observed.golden}, {"differences", differences}, {"errors", strings(observed.errors)},
      {"detail", observed.detail}, {"consumedDefinitionValues", observed.consumed},
      {"consumedDefinitionValuesSha256", digest(compact(observed.consumed))}};
    cases.append(result);
    if (!passed) errors.append(QStringLiteral("Regression case failed: ") + item["id"].toString());
  }
  report["cases"] = cases; report["errors"] = errors; report["elapsedMs"] = elapsed.elapsed();
  report["passed"] = !observeOnly && errors.isEmpty();
  report["observationComplete"] = errors.isEmpty();
  if (!writeReport(reportPath, report)) { std::fputs("Cannot persist replay regression report.\n", stderr); return 1; }
  std::printf("Replay regression: %lld cases, %lld ms, %s\n", static_cast<long long>(cases.size()),
      static_cast<long long>(elapsed.elapsed()), errors.isEmpty() ? (observeOnly ? "observed (not approved)" : "passed") : "rejected");
  return errors.isEmpty() ? 0 : 1;
}
