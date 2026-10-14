#pragma once

#include "domain/battle_replay_empowerment.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

namespace BattleReplayFixture {

inline QStringList selfTransformRuleFailures() {
  using namespace BattleReplay;
  QStringList failures;
  const auto catalog = QJsonDocument::fromJson(R"json({
    "pets": {
      "10":{"nativeAttributes":["27"],"transformSkillNo":500010},
      "20":{"nativeAttributes":["25"],"transformSkillNo":0},
      "30":{"nativeAttributes":["26"],"transformSkillNo":500030},
      "40":{"nativeAttributes":["27"],"transformSkillNo":500040}
    },
    "transformSkills": {
      "500010":{"name":"element","transformType":0,"effectArr":["ARR_RACE_TYPE:27:ALL:"]},
      "500030":{"name":"self","transformType":2,"effectArr":["SELF_RACE_TYPE:23:SELF:","SELF_RACE_TYPE:27:SELF:"]},
      "500040":{"name":"energize","transformType":1,"effectArr":["ENERGIZE::ALL:"]}
    }
  })json").object();
  auto make = [](const QString& id, const QString& race, const QString& attribute) {
    Fighter f;
    f.team = 0; f.player = QStringLiteral("p"); f.id = id; f.name = id;
    f.key = fighterKey(f.team, f.player, id); f.raceId = race;
    f.attributes = {attribute}; f.attributesKnown = true;
    f.raw = {{QStringLiteral("oRaceId"), race}, {QStringLiteral("ibat"), true}};
    return f;
  };
  const auto self = make(QStringLiteral("self"), QStringLiteral("30"), QStringLiteral("23"));
  const auto provider = make(QStringLiteral("element"), QStringLiteral("10"), QStringLiteral("27"));
  const auto target = make(QStringLiteral("target"), QStringLiteral("20"), QStringLiteral("27"));
  const QMap<QString, Fighter> initial{{self.key, self}};
  auto reject = [&](const QMap<QString, Fighter>& state, const QJsonObject& refs, const char* label) {
    if (!identifySelfTransforms(state, refs).isEmpty()) failures.append(QString::fromLatin1(label));
  };
  auto update = [](QJsonObject refs, const QString& table, const QString& id,
                   const QString& field, const QJsonValue& value) {
    auto entries = refs.value(table).toObject();
    auto entry = entries.value(id).toObject();
    if (value.isUndefined()) entry.remove(field);
    else entry.insert(field, value);
    entries.insert(id, entry); refs.insert(table, entries);
    return refs;
  };
  const auto positive = identifySelfTransforms(initial, catalog);
  const auto evidence = positive.value(self.key);
  if (positive.size() != 1 || evidence.source != self.key || evidence.target != self.key ||
      evidence.skillId != QStringLiteral("500030") ||
      evidence.beforeAttributes != QStringList{QStringLiteral("26")} ||
      evidence.afterAttributes != QStringList{QStringLiteral("23")} ||
      evidence.officialEffect != QStringLiteral("SELF_RACE_TYPE:23:SELF:") ||
      !evidence.description.contains(QStringLiteral("开局")))
    failures.append(QStringLiteral("unique self opening evidence lost its identity or exact attribute provenance"));

  auto state = initial;
  state.insert(provider.key, provider); state.insert(target.key, target);
  if (identifySelfTransforms(state, catalog).size() != 1 || identifyEmpowerments(state, catalog).size() != 1)
    failures.append(QStringLiteral("different-result self and element evidence did not coexist"));
  state[self.key].attributes = {QStringLiteral("27")};
  reject(state, catalog, "same-result element alternative was falsely called a certain self transformation");
  state.remove(target.key);
  reject(state, catalog, "absence of a second changed target erased same-result causal ambiguity");
  state = initial;
  auto second = self;
  second.id = QStringLiteral("self2"); second.key = fighterKey(second.team, second.player, second.id);
  second.attributes = {QStringLiteral("27")}; state.insert(second.key, second);
  reject(state, catalog, "multiple changed self holders were assigned one official self selection");
  second.player = QStringLiteral("p2"); second.key = fighterKey(second.team, second.player, second.id);
  state = initial; state.insert(second.key, second);
  if (identifySelfTransforms(state, catalog).size() != 2)
    failures.append(QStringLiteral("independent players incorrectly shared one self selection"));

  auto foreignProvider = provider;
  foreignProvider.player = QStringLiteral("p2");
  foreignProvider.key = fighterKey(foreignProvider.team, foreignProvider.player, foreignProvider.id);
  state = initial; state[self.key].attributes = {QStringLiteral("27")}; state.insert(foreignProvider.key, foreignProvider);
  if (identifySelfTransforms(state, catalog).size() != 1)
    failures.append(QStringLiteral("another player's element skill was treated as a competing self cause"));
  foreignProvider.team = 1; foreignProvider.player = self.player;
  foreignProvider.key = fighterKey(foreignProvider.team, foreignProvider.player, foreignProvider.id);
  state = initial; state[self.key].attributes = {QStringLiteral("27")}; state.insert(foreignProvider.key, foreignProvider);
  if (identifySelfTransforms(state, catalog).size() != 1)
    failures.append(QStringLiteral("opponent element skill was treated as a competing self cause"));

  const auto energize = make(QStringLiteral("energize"), QStringLiteral("40"), QStringLiteral("27"));
  state = initial; state[self.key].attributes = {QStringLiteral("27")}; state.insert(energize.key, energize);
  if (identifySelfTransforms(state, catalog).size() != 1 || !identifyEmpowerments(state, catalog).isEmpty())
    failures.append(QStringLiteral("known Buff-only energize either obscured self evidence or became an element assignment"));
  state = initial; state[self.key].raw.remove(QStringLiteral("ibat"));
  if (identifySelfTransforms(state, catalog).size() != 1)
    failures.append(QStringLiteral("self attribute evidence relied on an unverified meaning of ibat"));
  state = initial; state[self.key].faceId = QStringLiteral("999999");
  if (identifySelfTransforms(state, catalog).size() != 1)
    failures.append(QStringLiteral("cosmetic face identity replaced actual species self configuration"));

  state = initial; state[self.key].attributes = {QStringLiteral("26")};
  reject(state, catalog, "native opening attribute plus raw marker was called a self transformation");
  state = initial; state[self.key].attributes = {QStringLiteral("24")};
  reject(state, catalog, "an unconfigured opening attribute was called a self transformation");
  state = initial; state[self.key].attributes = {QStringLiteral("10023")};
  reject(state, catalog, "enhanced and ordinary attribute IDs were silently equated for self evidence");
  state = initial; state[self.key].attributesKnown = false;
  reject(state, catalog, "unknown opening attribute was called a self transformation");
  state = initial; state[self.key].attributes = {QStringLiteral("23"), QStringLiteral("27")};
  reject(state, catalog, "a multi-attribute opening was reduced to one chosen self alternative");
  state = initial; state[self.key].clone = true;
  reject(state, catalog, "a clone's copied form was called its own self transformation");
  state = initial; state[self.key].chaser = true;
  reject(state, catalog, "a chaser's inherited identity was called its own self transformation");
  state = initial; state[self.key].raw.insert(QStringLiteral("oRaceId"), QStringLiteral("31"));
  reject(state, catalog, "a changed original species was ignored for self evidence");
  state = initial; state[self.key].raw.insert(QStringLiteral("oRaceId"), QJsonValue(QJsonValue::Null));
  reject(state, catalog, "an explicitly unknown original species was ignored for self evidence");
  state = initial; state[self.key].originRaceId = QStringLiteral("31");
  reject(state, catalog, "preserved transformed identity was ignored for self evidence");
  state = initial; state[self.key].originFighterKey = QStringLiteral("other");
  reject(state, catalog, "another unit's original-body identity was ignored for self evidence");

  const auto pets = QStringLiteral("pets"), skills = QStringLiteral("transformSkills");
  const auto selfRace = QStringLiteral("30"), selfSkill = QStringLiteral("500030");
  reject(initial, {}, "missing official tables produced self evidence");
  reject(initial, update(catalog, pets, selfRace, QStringLiteral("nativeAttributes"), QJsonValue(QJsonValue::Undefined)),
         "missing native attributes produced self evidence");
  reject(initial, update(catalog, pets, selfRace, QStringLiteral("transformSkillNo"), 599999),
         "missing self skill definition produced self evidence");
  reject(initial, update(catalog, skills, selfSkill, QStringLiteral("transformType"), QJsonValue(QJsonValue::Undefined)),
         "missing official self kind was inferred from its name or effect string");
  reject(initial, update(catalog, skills, selfSkill, QStringLiteral("transformType"), 2.5),
         "fractional transform kind was truncated to self");
  for (const auto& effect : QStringList{QStringLiteral("SELF_RACE_TYPE:23:ALL:"),
                                      QStringLiteral("SELF_RACE_TYPE:23:SELF:extra"),
                                      QStringLiteral("SELF_RACE_TYPE:23,27:SELF:"),
                                      QStringLiteral("ARR_RACE_TYPE:23:SELF:")}) {
    reject(initial, update(catalog, skills, selfSkill, QStringLiteral("effectArr"), QJsonArray{effect}),
           "an unsupported effect signature was accepted as official self attribute evidence");
  }
  reject(initial, update(catalog, skills, selfSkill, QStringLiteral("effectArr"),
                         QJsonArray{QStringLiteral("SELF_RACE_TYPE:23:SELF:"), QStringLiteral("UNKNOWN::SELF:")}),
         "a matching effect concealed an unrecognized self configuration alternative");
  state = initial; state.insert(provider.key, provider);
  reject(state, update(catalog, pets, QStringLiteral("10"), QStringLiteral("transformSkillNo"), QJsonValue(QJsonValue::Undefined)),
         "missing teammate transform configuration was treated as no competing cause");
  reject(state, update(catalog, skills, QStringLiteral("500010"), QStringLiteral("effectArr"), QJsonArray{}),
         "missing element effect was treated as no competing cause");
  state[provider.key].attributesKnown = false;
  reject(state, catalog, "unknown formation attribute evidence was silently accepted");
  return failures;
}

// Call from the existing domain test and fail for every returned message.
inline QStringList empowermentRuleFailures() {
  using namespace BattleReplay;
  QStringList failures;
  const auto catalog = QJsonDocument::fromJson(R"json({
    "pets": {
      "10": {"nativeAttributes":["27"],"transformSkillNo":500010},
      "20": {"nativeAttributes":["25"],"transformSkillNo":0}
    },
    "transformSkills": {
      "500010": {"name":"element","type":0,"transformType":0,"effectArr":["ARR_RACE_TYPE:27:ALL:"]},
      "500020": {"name":"self","type":2,"transformType":2,"effectArr":["SELF_RACE_TYPE:27:SELF:"]}
    }
  })json").object();
  auto make = [](const QString& id, const QString& race, const QString& player = QStringLiteral("p")) {
    Fighter f;
    f.team = 0; f.player = player; f.id = id; f.name = id;
    f.key = fighterKey(f.team, player, id); f.raceId = race;
    f.attributes = {QStringLiteral("27")}; f.attributesKnown = true;
    f.raw = {{QStringLiteral("oRaceId"), race}, {QStringLiteral("ibat"), true}};
    return f;
  };
  const auto source = make(QStringLiteral("1"), QStringLiteral("10"));
  const auto target = make(QStringLiteral("2"), QStringLiteral("20"));
  const QMap<QString, Fighter> initial{{source.key, source}, {target.key, target}};
  auto reject = [&](const QMap<QString, Fighter>& state, const QJsonObject& refs, const char* label) {
    if (!identifyEmpowerments(state, refs).isEmpty()) failures.append(QString::fromLatin1(label));
  };
  const auto positive = identifyEmpowerments(initial, catalog);
  if (positive.size() != 1 || positive[target.key].source != source.key ||
      positive[target.key].skillId != QStringLiteral("500010") || positive[target.key].description.isEmpty() ||
      positive[target.key].beforeAttributes != QStringList{QStringLiteral("25")} ||
      positive[target.key].afterAttributes != QStringList{QStringLiteral("27")} ||
      positive[target.key].officialEffect != QStringLiteral("ARR_RACE_TYPE:27:ALL:"))
    failures.append(QStringLiteral("unique official element target was not identified"));
  auto state = initial;
  state[target.key].summoned = true;
  if (identifyEmpowerments(state, catalog).size() != 1)
    failures.append(QStringLiteral("summoned and empowerment classifications cannot coexist"));
  auto coexistRefs = catalog;
  auto coexistPets = coexistRefs.value(QStringLiteral("pets")).toObject();
  coexistPets.insert(QStringLiteral("30"), QJsonDocument::fromJson(
      R"json({"nativeAttributes":["26"],"transformSkillNo":500030})json").object());
  coexistRefs.insert(QStringLiteral("pets"), coexistPets);
  auto coexistSkills = coexistRefs.value(QStringLiteral("transformSkills")).toObject();
  coexistSkills.insert(QStringLiteral("500030"), QJsonDocument::fromJson(R"json({
      "type":2,"transformType":2,"effectArr":["SELF_RACE_TYPE:23:SELF:","SELF_RACE_TYPE:27:SELF:"]
  })json").object());
  coexistRefs.insert(QStringLiteral("transformSkills"), coexistSkills);
  auto selfAlly = make(QStringLiteral("3"), QStringLiteral("30"));
  selfAlly.attributes = {QStringLiteral("23")};
  state = initial; state.insert(selfAlly.key, selfAlly);
  const auto coexist = identifyEmpowerments(state, coexistRefs);
  if (coexist.size() != 1 || !coexist.contains(target.key) || coexist.contains(selfAlly.key))
    failures.append(QStringLiteral("independent self transformation obscured or became the element target"));
  state[selfAlly.key].attributes = {QStringLiteral("27")};
  reject(state, coexistRefs, "self and element changes with the same result were disambiguated without evidence");
  state[selfAlly.key].attributes = {QStringLiteral("24")};
  reject(state, coexistRefs, "a change absent from the self skill was excluded from ambiguity");
  state[selfAlly.key].attributes = {QStringLiteral("23")}; state[selfAlly.key].clone = true;
  reject(state, coexistRefs, "a transformed clone was assumed to be an independent self-transform unit");
  auto otherSource = make(QStringLiteral("3"), QStringLiteral("10"));
  state = initial; state.insert(otherSource.key, otherSource);
  reject(state, catalog, "ambiguous providers guessed an owner");
  auto otherTarget = make(QStringLiteral("4"), QStringLiteral("20"));
  state = initial; state.insert(otherTarget.key, otherTarget);
  reject(state, catalog, "ambiguous initial attribute changes guessed a target");
  state = initial; state[target.key].raw.insert(QStringLiteral("ibat"), false);
  reject(state, catalog, "attribute change alone was called empowerment");
  state = initial; state[target.key].clone = true;
  reject(state, catalog, "clone identity was called empowerment");
  state = initial; state[target.key].chaser = true;
  reject(state, catalog, "chaser identity was called empowerment");
  state = initial; state[target.key].raw.insert(QStringLiteral("oRaceId"), 21);
  reject(state, catalog, "initial transformed identity was called empowerment");
  state = initial; state[target.key].attributes = {QStringLiteral("25")};
  reject(state, catalog, "raw marker without attribute evidence was called empowerment");
  state = initial; state[target.key].attributesKnown = false;
  reject(state, catalog, "unknown initial attributes were called empowerment");
  auto selfRefs = catalog;
  auto selfPets = selfRefs.value(QStringLiteral("pets")).toObject();
  auto selfPet = selfPets.value(QStringLiteral("10")).toObject();
  selfPet.insert(QStringLiteral("nativeAttributes"), QJsonArray{QStringLiteral("25")});
  selfPets.insert(QStringLiteral("10"), selfPet); selfRefs.insert(QStringLiteral("pets"), selfPets);
  reject({{source.key, source}}, selfRefs, "provider was identified as its own nonself target");
  auto refs = catalog;
  auto pets = refs.value(QStringLiteral("pets")).toObject();
  auto pet = pets.value(QStringLiteral("20")).toObject();
  pet.insert(QStringLiteral("transformSkillNo"), 500020); pets.insert(QStringLiteral("20"), pet);
  refs.insert(QStringLiteral("pets"), pets);
  reject(initial, refs, "self transform was attributed to another unit");
  pet.remove(QStringLiteral("transformSkillNo")); pets.insert(QStringLiteral("20"), pet);
  refs.insert(QStringLiteral("pets"), pets);
  reject(initial, refs, "missing transform reference was treated as absence of a competing skill");
  refs = catalog;
  auto skills = refs.value(QStringLiteral("transformSkills")).toObject();
  auto skill = skills.value(QStringLiteral("500010")).toObject();
  skill.insert(QStringLiteral("transformType"), 1); skills.insert(QStringLiteral("500010"), skill);
  refs.insert(QStringLiteral("transformSkills"), skills);
  reject(initial, refs, "energize target was inferred from attribute changes");
  state = initial;
  const auto source2 = make(QStringLiteral("1"), QStringLiteral("10"), QStringLiteral("p2"));
  const auto target2 = make(QStringLiteral("2"), QStringLiteral("20"), QStringLiteral("p2"));
  state.insert(source2.key, source2); state.insert(target2.key, target2);
  if (identifyEmpowerments(state, catalog).size() != 2)
    failures.append(QStringLiteral("provider or target identity crossed player boundaries"));
  failures.append(selfTransformRuleFailures());
  return failures;
}

} // namespace BattleReplayFixture
