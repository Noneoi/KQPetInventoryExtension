#include "domain/battle_replay_presentation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

using namespace BattleReplay;

namespace {
const QString sourceKey = fighterKey(0, QStringLiteral("own"), QStringLiteral("7"));
const QString targetKey = fighterKey(1, QStringLiteral("enemy"), QStringLiteral("7"));

struct Fixture {
  Step step;
  Moment moment;
  QMap<QString, Fighter> before;
  QJsonObject catalog;
};

Fixture fixture() {
  Fixture f;
  const auto attr = [](const char* name, int priority, const char* coefficients) {
    return QJsonObject{{"nm", QString::fromUtf8(name)}, {"pri", priority}, {"rcs", QString::fromLatin1(coefficients)}};
  };
  f.catalog = {
    {"attributes", QJsonObject{
      {"1", attr("普", 1, "1,1.2,0.8,1,1")},
      {"2", attr("草", 1, "1.2,1,1.3,1,1")},
      {"21", attr("神草", 2, "1.3,1.3,1,1.5,1")},
      {"22", attr("神水", 2, "1.3,1.3,0.5,1,1")},
      {"10021", attr("神草增强", 3, "1.5,1.5,1,0.5,1")}}},
    {"skills", QJsonObject{{"100", QJsonObject{{"attackType", QStringLiteral("近身")}}},
                          {"200", QJsonObject{{"attackType", QStringLiteral("远程")}}}}},
    {"clientPresentation", QJsonObject{
      {"schemaVersion", 1}, {"weakThreshold", 0.8}, {"strongThreshold", 1.2},
      {"attributeIndex", QJsonObject{{"1", 0}, {"2", 1}, {"21", 2}, {"22", 3}, {"10021", 4}}},
      {"godPriorities", QJsonArray{2, 3}}, {"representerGodPriorities", QJsonArray{2}},
      {"ignoreRestraintBuffIds", QJsonArray{4467, 5257, 5655}},
      {"jobs", QJsonObject{{"1", QJsonObject{{"god", false}}}, {"16", QJsonObject{{"god", true}}}}},
      {"pets", QJsonObject{{"10", QJsonObject{{"jobId", 1}, {"isTianQi", false}}},
                           {"20", QJsonObject{{"jobId", 16}, {"isTianQi", false}}},
                           {"30", QJsonObject{{"jobId", 16}, {"isTianQi", true}}}}}}}
  };
  Fighter source; source.key = sourceKey; source.team = 0; source.player = QStringLiteral("own"); source.id = QStringLiteral("7");
  source.raceId = QStringLiteral("30"); source.attributes = {QStringLiteral("21")}; source.attributesKnown = true;
  source.attributesPath = QStringLiteral("/source/attr"); source.formPath = QStringLiteral("/source/raceId"); source.hp = 200;
  Fighter target; target.key = targetKey; target.team = 1; target.player = QStringLiteral("enemy"); target.id = QStringLiteral("7");
  target.raceId = QStringLiteral("10"); target.attributes = {QStringLiteral("22")}; target.attributesKnown = true;
  target.attributesPath = QStringLiteral("/target/attr"); target.formPath = QStringLiteral("/target/raceId"); target.hpPath = QStringLiteral("/target/hp"); target.hp = 100;
  f.before = {{sourceKey, source}, {targetKey, target}};
  f.step.actor = sourceKey; f.step.path = QStringLiteral("/fr/0/rl/0");
  f.step.playbackKind = QStringLiteral("normal"); f.step.playbackSkillId = QStringLiteral("100"); f.step.counterSkillId = QStringLiteral("200");
  f.step.raw = QJsonObject{{"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", QStringLiteral("enemy")}, {"tci", 7},
    {"isse", true}, {"ishit", true}, {"issb", true}, {"tn", QJsonObject{{"att_hps", QStringLiteral("-150")}}}}}},
    {"cn", QJsonObject{{"sb_hps", QStringLiteral("-15")}}}};
  f.moment.phase = QStringLiteral("attack"); f.moment.part = QStringLiteral("result"); f.moment.actor = sourceKey;
  f.moment.state = f.before; f.moment.state[targetKey].hp = 0;
  return f;
}

QList<ClientPresentationCue> evaluate(const Fixture& f) {
  return clientPresentationCues(f.step, f.moment, f.before, f.catalog);
}

bool has(const QList<ClientPresentationCue>& cues, const QString& kind, const QString& recipient = {}) {
  for (const auto& cue : cues) if (cue.kind == kind && (recipient.isEmpty() || cue.recipient == recipient)) return true;
  return false;
}

void setTargetField(Fixture& f, const QString& name, const QJsonValue& value) {
  auto raw = f.step.raw.toObject(); auto list = raw["trl"].toArray(); auto target = list[0].toObject();
  target[name] = value; list[0] = target; raw["trl"] = list; f.step.raw = raw;
}

void setConfigField(Fixture& f, const QString& name, const QJsonValue& value) {
  auto config = f.catalog["clientPresentation"].toObject(); config[name] = value; f.catalog["clientPresentation"] = config;
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  int failures = 0;
  const auto check = [&](bool ok, const char* message) { if (!ok) { qCritical() << message; ++failures; } };
  {
    auto f = fixture(); const auto original = QJsonDocument(f.step.raw.toObject()).toJson();
    const auto cues = evaluate(f);
    check(has(cues, QStringLiteral("strong"), targetKey) && has(cues, QStringLiteral("god-job-attack"), sourceKey) &&
      has(cues, QStringLiteral("tianqi"), sourceKey), "recorded lethal attack lacks official client cues");
    check(f.before[targetKey].hp == 100 && f.moment.state[targetKey].hp == 0 && QJsonDocument(f.step.raw.toObject()).toJson() == original,
      "presentation must not mutate the ledger, moment or raw packet");
    for (const auto& cue : cues) check(cue.explanation.startsWith(QStringLiteral("官方客户端提示")) && !cue.evidence.isEmpty(),
      "derived display cues must remain distinct from logged judgement");
  }
  {
    auto f = fixture(); f.before[sourceKey].attributes = {QStringLiteral("1")}; f.before[targetKey].attributes = {QStringLiteral("2")};
    check(has(evaluate(f), QStringLiteral("strong")), "strong threshold equality must use explicit column map, not attribute ID");
    f.before[targetKey].attributes = {QStringLiteral("21")};
    check(has(evaluate(f), QStringLiteral("weak")), "weak threshold equality is inclusive");
    f.before[sourceKey].attributes = {QStringLiteral("1"), QStringLiteral("2")};
    check(has(evaluate(f), QStringLiteral("weak")), "priority ties must retain first recorded attribute");
    f.before[sourceKey].attributes = {QStringLiteral("2"), QStringLiteral("1")};
    check(has(evaluate(f), QStringLiteral("strong")), "priority ties must follow recorded order, not sorted IDs");
  }
  {
    auto f = fixture(); f.before[sourceKey].attributes = {QStringLiteral("21"), QStringLiteral("10021")};
    check(has(evaluate(f), QStringLiteral("weak")) && !has(evaluate(f), QStringLiteral("strong")), "current enhanced attribute must take higher priority");
    f.step.state = f.before; f.step.state[sourceKey].attributes = {QStringLiteral("21")};
    check(has(evaluate(f), QStringLiteral("weak")), "action-final attributes must not leak backwards into current stage");
    f.before[targetKey].attributes = {QStringLiteral("999999")};
    check(!has(evaluate(f), QStringLiteral("weak")) && has(evaluate(f), QStringLiteral("god-job-attack")), "unknown attribute suppresses only unsupported attribute cue");
    f.before[targetKey].attributes = {QStringLiteral("22")}; f.before[sourceKey].attributesKnown = false;
    check(!has(evaluate(f), QStringLiteral("weak")), "unknown current attribute state cannot fall back to dictionary attributes");
  }
  {
    auto f = fixture(); setConfigField(f, QStringLiteral("attributeIndex"), QJsonObject{});
    check(!has(evaluate(f), QStringLiteral("strong")), "missing official ID-to-column map cannot be guessed");
    f = fixture(); setConfigField(f, QStringLiteral("strongThreshold"), QJsonValue());
    check(!has(evaluate(f), QStringLiteral("strong")), "missing threshold cannot use hardcoded AS defaults");
  }
  {
    auto f = fixture(); setTargetField(f, QStringLiteral("tn"), QJsonObject{{"att_hps", QStringLiteral("-150")}, {"bd_bfs", QStringLiteral("0_4467_9")}});
    auto cues = evaluate(f);
    check(!has(cues, QStringLiteral("strong")) && !has(cues, QStringLiteral("tianqi")) && has(cues, QStringLiteral("god-job-attack")),
      "ignore marker must apply even for clear operation, without suppressing unrelated job cue");
    setTargetField(f, QStringLiteral("tn"), QJsonObject{{"att_hps", QStringLiteral("-150")}, {"bd_bfs", QStringLiteral("broken")}});
    check(!has(evaluate(f), QStringLiteral("strong")), "malformed before-display-Buff data must not assume no ignore marker");
    setTargetField(f, QStringLiteral("tn"), QJsonObject{{"att_hps", QStringLiteral("-150")}, {"ad_bfs", QStringLiteral("1_4467_9")}});
    check(has(evaluate(f), QStringLiteral("strong")), "after marker must not retroactively suppress attack hint");
  }
  {
    auto f = fixture(); setTargetField(f, QStringLiteral("ishit"), false); setTargetField(f, QStringLiteral("isIm"), true);
    check(has(evaluate(f), QStringLiteral("strong")), "official handleHpAndPowerChanges precedes miss/immunity branch");
    setTargetField(f, QStringLiteral("isse"), false);
    check(evaluate(f).isEmpty(), "non-effective target cannot get attack hints");
    f = fixture(); setTargetField(f, QStringLiteral("tn"), QJsonValue());
    check(evaluate(f).isEmpty(), "null numerical result returns before any official hints");
    f = fixture(); auto raw = f.step.raw.toObject(); raw["prl"] = raw["trl"]; raw["trl"] = QJsonArray{}; f.step.raw = raw;
    check(evaluate(f).isEmpty(), "passive prl results are not PlayEffect targets");
  }
  {
    for (const auto& kind : {QStringLiteral("disabled"), QStringLiteral("summon"), QStringLiteral("team-simple"), QStringLiteral("unresolved")}) {
      auto f = fixture(); f.step.playbackKind = kind; check(evaluate(f).isEmpty(), "non-attack command gained attack-only client cues");
    }
    for (const auto& phase : {QStringLiteral("before"), QStringLiteral("after"), QStringLiteral("settle"), QStringLiteral("setup")}) {
      auto f = fixture(); f.moment.phase = phase; check(evaluate(f).isEmpty(), "wrong phase gained future/duplicate attack cue");
    }
    auto f = fixture(); f.moment.part = QStringLiteral("objects"); check(evaluate(f).isEmpty(), "special object moment is not attack result");
  }
  {
    auto f = fixture(); f.before[sourceKey].raceId = QStringLiteral("10"); f.before[targetKey].raceId = QStringLiteral("20");
    check(has(evaluate(f), QStringLiteral("god-job-defend"), targetKey), "god defender cue belongs to defender card");
    f.before[sourceKey].raceId = QStringLiteral("20");
    check(!has(evaluate(f), QStringLiteral("god-job-defend")) && !has(evaluate(f), QStringLiteral("god-job-attack")), "equal god status has no job differential cue");
    f.before[sourceKey].raceId = QStringLiteral("99999"); f.before[sourceKey].raw["job"] = 1;
    check(!has(evaluate(f), QStringLiteral("god-job-defend")), "unknown current dictionary job must not be guessed from unconsumed raw field");
  }
  {
    auto f = fixture(); f.before[sourceKey].raceId = QStringLiteral("20");
    check(!has(evaluate(f), QStringLiteral("tianqi")), "name or god status cannot imply TianQi sign");
    f = fixture(); f.moment.state[targetKey].hp = 40;
    check(!has(evaluate(f), QStringLiteral("tianqi")), "TianQi cue requires current-stage observed nonpositive HP");
    f = fixture(); setTargetField(f, QStringLiteral("tn"), QJsonObject{{"att_hps", QStringLiteral("-10")}});
    check(!has(evaluate(f), QStringLiteral("tianqi")), "final dead snapshot alone must not invent lethal attack delta");
    f = fixture(); f.before[targetKey].hp.reset();
    check(!has(evaluate(f), QStringLiteral("tianqi")), "unknown pre-hit life cannot establish TianQi condition");
    f = fixture(); f.catalog["pets"] = QJsonObject{{"30", QJsonObject{{"desc", QStringLiteral("内测")}}}};
    check(!has(evaluate(f), QStringLiteral("tianqi")) && has(evaluate(f), QStringLiteral("god-job-attack")),
      "testing race getter changes TianQi dictionary lookup but not cached primary job");
  }
  {
    auto f = fixture(); f.moment.phase = QStringLiteral("counter"); f.moment.actor = targetKey;
    f.moment.state = f.before; f.moment.state[sourceKey].hp = 185;
    auto raw = f.step.raw.toObject(); raw["cn"] = QJsonObject{{"sb_hps", QStringLiteral("-15")}, {"bd_bfs", QStringLiteral("1_4467_0")}}; f.step.raw = raw;
    const auto cues = evaluate(f);
    check(has(cues, QStringLiteral("weak"), sourceKey) && has(cues, QStringLiteral("god-job-defend"), sourceKey),
      "counter must reverse source/target and clear original before-display ignore list");
    for (const auto& cue : cues) check(cue.source == targetKey && cue.target == sourceKey, "counter cue used original attacking side");
    f.step.playbackSkillId = QStringLiteral("200");
    check(evaluate(f).isEmpty(), "remote original attack cannot acquire close-only counter semantics");
    f.step.lastSkillStatus = QStringLiteral("applied");
    f.catalog["skillLastEffects"] = QJsonObject{{"200", QJsonObject{{"attackType", QStringLiteral("近身")}}}};
    check(has(evaluate(f), QStringLiteral("weak"), sourceKey), "applied last-effect attack type must govern counter support");
  }
  {
    auto f = fixture(); f.step.playbackKind = QStringLiteral("team-skill");
    const auto cues = evaluate(f);
    check(has(cues, QStringLiteral("strong")) && !has(cues, QStringLiteral("god-job-attack")) && !has(cues, QStringLiteral("tianqi")),
      "representer effect branch has attribute cues but no job/TianQi branch");
    f.before[sourceKey].attributes = {QStringLiteral("10021")}; f.before[targetKey].attributes = {QStringLiteral("1")};
    const auto enhanced = evaluate(f);
    check(has(enhanced, QStringLiteral("strong")), "representer enhanced strong case lost ordinary strong cue");
    for (const auto& cue : enhanced) if (cue.kind == QStringLiteral("strong")) check(cue.attributeId.isEmpty(), "representer must not inherit fighter-only TRANSFORM icon branch");
  }
  qInfo() << "Client presentation tests:" << failures << "failures";
  return failures ? 1 : 0;
}
