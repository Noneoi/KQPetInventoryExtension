#include "battle_replay_empowerment.h"

#include <QtCore/QJsonArray>
#include <algorithm>

namespace BattleReplay {
namespace {

std::optional<QStringList> normalizedAttributes(const QStringList& values) {
  QStringList result;
  for (const auto& value : values) {
    const auto item = value.trimmed();
    if (item.isEmpty()) continue;
    for (const auto c : item) {
      if (c < QLatin1Char('0') || c > QLatin1Char('9')) return std::nullopt;
    }
    result.append(item);
  }
  if (result.isEmpty()) return std::nullopt;
  std::sort(result.begin(), result.end());
  return result;
}

std::optional<QStringList> nativeAttributes(const QJsonObject& pet) {
  const auto raw = pet.value(QStringLiteral("nativeAttributes"));
  if (!raw.isArray()) return std::nullopt;
  QStringList values;
  for (const auto& value : raw.toArray()) {
    if (!value.isString()) return std::nullopt;
    values.append(value.toString());
  }
  return normalizedAttributes(values);
}

QString displayName(const Fighter& fighter) {
  return fighter.formName.isEmpty() ? fighter.name : fighter.formName;
}

QString attributeNames(const QStringList& values, const QJsonObject& catalog) {
  QStringList result;
  for (const auto& value : values) {
    result.append(referenceName(catalog, QStringLiteral("attributes"), value));
  }
  return result.join(QStringLiteral("、"));
}

struct Provider {
  const Fighter* fighter = nullptr;
  QJsonObject skill;
  QString skillId;
};

struct Changed {
  const Fighter* fighter = nullptr;
  QStringList native, initial;
  QJsonObject ownSkill;
};

QString separateSelfEffect(const Changed& change, const QStringList& elementResult) {
  if (change.initial == elementResult ||
      change.ownSkill.value(QStringLiteral("transformType")).toInt(-1) != 2) return {};
  const auto& fighter = *change.fighter;
  if (fighter.clone || fighter.chaser ||
      (fighter.raw.contains(QStringLiteral("oRaceId")) &&
       valueText(fighter.raw.value(QStringLiteral("oRaceId"))) != fighter.raceId)) return {};
  for (const auto& effect : change.ownSkill.value(QStringLiteral("effectArr")).toArray()) {
    if (!effect.isString()) continue;
    const auto parts = effect.toString().split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (parts.size() != 4 || parts[0] != QStringLiteral("SELF_RACE_TYPE") ||
        parts[2] != QStringLiteral("SELF") || !parts[3].isEmpty()) continue;
    const auto ownResult = normalizedAttributes(parts[1].split(QLatin1Char(','), Qt::SkipEmptyParts));
    if (ownResult && *ownResult == change.initial) return effect.toString();
  }
  return {};
}

struct OpeningTransform {
  const Fighter* fighter = nullptr;
  QString skillId;
  QJsonObject skill;
  int kind = -1; // No transform skill when transformSkillNo is explicitly 0.
  QStringList native, initial, effectAttributes, effects;
};

std::optional<QStringList> openingAttributes(const QStringList& values) {
  const auto normalized = normalizedAttributes(values);
  if (!normalized) return std::nullopt;
  for (const auto& value : *normalized) {
    bool ok = false;
    const int id = value.toInt(&ok);
    if (!ok || id <= 0 || QString::number(id) != value) return std::nullopt;
  }
  return normalized;
}

std::optional<OpeningTransform> openingTransform(
    const Fighter& fighter, const QJsonObject& pets, const QJsonObject& skills) {
  const auto pet = pets.value(fighter.raceId).toObject();
  const auto transform = pet.value(QStringLiteral("transformSkillNo"));
  const auto id = transform.toInteger(-1);
  const auto native = nativeAttributes(pet);
  const auto initial = openingAttributes(fighter.attributes);
  if (pet.isEmpty() || !transform.isDouble() || id < 0 || transform.toDouble() != id ||
      !native || !openingAttributes(*native) || !fighter.attributesKnown || !initial) return std::nullopt;
  OpeningTransform result;
  result.fighter = &fighter;
  result.skillId = QString::number(id);
  result.native = *native;
  result.initial = *initial;
  if (id == 0) return result;

  result.skill = skills.value(result.skillId).toObject();
  const auto kind = result.skill.value(QStringLiteral("transformType"));
  result.kind = kind.toInt(-1);
  if (!kind.isDouble() || kind.toDouble() != result.kind || result.kind < 0 || result.kind > 2)
    return std::nullopt;
  const auto rawEffects = result.skill.value(QStringLiteral("effectArr"));
  if (!rawEffects.isArray() || rawEffects.toArray().isEmpty()) return std::nullopt;
  for (const auto& raw : rawEffects.toArray()) {
    if (!raw.isString()) return std::nullopt;
    const auto effect = raw.toString();
    const auto parts = effect.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (parts.size() != 4 || !parts[3].isEmpty()) return std::nullopt;
    if (result.kind == 1) {
      // The official ENERGIZE client effect is empty, and its skill type
      // affects Buffs. It is not a competing attribute conversion.
      if (effect != QStringLiteral("ENERGIZE::ALL:")) return std::nullopt;
    } else {
      const auto expectedEffect = result.kind == 2 ? QStringLiteral("SELF_RACE_TYPE") : QStringLiteral("ARR_RACE_TYPE");
      const auto expectedScope = result.kind == 2 ? QStringLiteral("SELF") : QStringLiteral("ALL");
      // Both official effect consumers pass one integer to setAttr. Do not
      // interpret a comma-separated parameter as a multi-attribute effect.
      const auto attribute = openingAttributes({parts[1]});
      if (parts[0] != expectedEffect || parts[2] != expectedScope || !attribute) return std::nullopt;
      result.effectAttributes.append(attribute->first());
    }
    result.effects.append(effect);
  }
  return result;
}

bool unambiguousSelfIdentity(const Fighter& fighter) {
  return !fighter.clone && !fighter.chaser && !fighter.key.isEmpty() &&
      fighter.team >= 0 && !fighter.player.isEmpty() &&
      (!fighter.raw.contains(QStringLiteral("oRaceId")) ||
       valueText(fighter.raw.value(QStringLiteral("oRaceId"))) == fighter.raceId) &&
      (fighter.originRaceId.isEmpty() || fighter.originRaceId == fighter.raceId) &&
      (fighter.originFighterKey.isEmpty() || fighter.originFighterKey == fighter.key);
}

} // namespace

QMap<QString, EmpowermentEvidence> identifyEmpowerments(
    const QMap<QString, Fighter>& initialState, const QJsonObject& catalog) {
  QMap<QString, EmpowermentEvidence> result;
  QMap<QString, QList<const Fighter*>> players;
  const auto pets = catalog.value(QStringLiteral("pets")).toObject();
  const auto skills = catalog.value(QStringLiteral("transformSkills")).toObject();
  for (auto it = initialState.cbegin(); it != initialState.cend(); ++it) {
    const auto& fighter = it.value();
    if (!fighter.representer) {
      players[fighterKey(fighter.team, fighter.player, QStringLiteral("@team"))].append(&fighter);
    }
  }
  for (const auto& fighters : players) {
    QList<Provider> providers;
    QList<Changed> changed;
    bool unknown = false;
    for (const auto* fighter : fighters) {
      const auto pet = pets.value(fighter->raceId).toObject();
      if (pet.isEmpty()) { unknown = true; continue; }
      const auto transform = pet.value(QStringLiteral("transformSkillNo"));
      const auto transformId = transform.toInteger(-1);
      if (!transform.isDouble() || transformId < 0 || transform.toDouble() != transformId) {
        unknown = true; continue;
      }
      const auto sid = QString::number(transformId);
      const auto skill = skills.value(sid).toObject();
      if (sid != QStringLiteral("0") && skill.isEmpty()) unknown = true;
      const int kind = skill.value(QStringLiteral("transformType")).toInt(-1);
      if (kind == 0 || kind == 1) providers.append({fighter, skill, sid});
      const auto native = nativeAttributes(pet);
      const auto initial = normalizedAttributes(fighter->attributes);
      if (!fighter->attributesKnown || !native || !initial) unknown = true;
      else if (*native != *initial) changed.append({fighter, *native, *initial, skill});
    }
    if (unknown || providers.size() != 1) continue;
    const auto& provider = providers.first();
    if (provider.skill.value(QStringLiteral("transformType")).toInt(-1) != 0) continue;
    const auto effects = provider.skill.value(QStringLiteral("effectArr")).toArray();
    if (effects.size() != 1 || !effects.first().isString()) continue;
    const auto parts = effects.first().toString().split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (parts.size() != 4 || parts[0] != QStringLiteral("ARR_RACE_TYPE") ||
        parts[2] != QStringLiteral("ALL") || !parts[3].isEmpty()) continue;
    const auto effectAttributes = normalizedAttributes(parts[1].split(QLatin1Char(','), Qt::SkipEmptyParts));
    if (!effectAttributes) continue;
    QList<Changed> candidates;
    QStringList separateSelfEvidence;
    for (const auto& change : changed) {
      // Official Formation applies self-transform first, then the selected
      // element effect. A verified self result different from the element's
      // result is independent and must not make its target ambiguous.
      const auto ownEffect = separateSelfEffect(change, *effectAttributes);
      if (!ownEffect.isEmpty()) {
        const auto ownId = QString::number(pets.value(change.fighter->raceId).toObject()
                                              .value(QStringLiteral("transformSkillNo")).toInteger());
        separateSelfEvidence.append(QStringLiteral("%1：%2（%3），自身%4→%5；官方效果 %6")
            .arg(displayName(*change.fighter), change.ownSkill.value(QStringLiteral("name")).toString(),
                 ownId, attributeNames(change.native, catalog), attributeNames(change.initial, catalog), ownEffect));
      } else candidates.append(change);
    }
    if (candidates.size() != 1) continue;
    const auto& change = candidates.first();
    const auto& target = *change.fighter;
    if (provider.fighter->key == target.key || change.initial != *effectAttributes) continue;
    if (change.ownSkill.value(QStringLiteral("transformType")).toInt(-1) == 2) continue;
    if (target.clone || target.chaser) continue;
    if (target.raw.contains(QStringLiteral("oRaceId")) &&
        valueText(target.raw.value(QStringLiteral("oRaceId"))) != target.raceId) continue;
    // This flag is only a corroborating raw observation. Self transformations
    // also carry it; the provider/effect/identity checks above remain required.
    if (!target.raw.value(QStringLiteral("ibat")).isBool() ||
        !target.raw.value(QStringLiteral("ibat")).toBool()) continue;
    EmpowermentEvidence evidence;
    evidence.source = provider.fighter->key;
    evidence.target = target.key;
    evidence.skillId = provider.skillId;
    evidence.beforeAttributes = change.native;
    evidence.afterAttributes = change.initial;
    evidence.officialEffect = effects.first().toString();
    evidence.description = QStringLiteral("官方配置与日志唯一匹配：%1 的 %2（%3）将一只精灵改为%4；"
        "%5 的当前种族原生属性为%6，开局日志为%4。")
        .arg(displayName(*provider.fighter), provider.skill.value(QStringLiteral("name")).toString(),
             provider.skillId, attributeNames(change.initial, catalog), displayName(target),
             attributeNames(change.native, catalog));
    if (separateSelfEvidence.isEmpty())
      evidence.description += QStringLiteral("同玩家仅此元能来源和符合该效果的目标，原始标记亦吻合。");
    else {
      evidence.description += QStringLiteral("排除官方效果可独立解释的自身幻元后，仅此元能来源和匹配目标，原始标记亦吻合。");
      evidence.description += QStringLiteral("独立幻元核对：%1。其结果与本次元素技结果不同。")
          .arg(separateSelfEvidence.join(QStringLiteral("；")));
    }
    result.insert(target.key, evidence);
  }
  return result;
}

QMap<QString, EmpowermentEvidence> identifySelfTransforms(
    const QMap<QString, Fighter>& initialState, const QJsonObject& catalog) {
  QMap<QString, EmpowermentEvidence> result;
  QMap<QString, QList<const Fighter*>> players;
  const auto pets = catalog.value(QStringLiteral("pets")).toObject();
  const auto skills = catalog.value(QStringLiteral("transformSkills")).toObject();
  for (const auto& fighter : initialState) {
    if (!fighter.representer)
      players[fighterKey(fighter.team, fighter.player, QStringLiteral("@team"))].append(&fighter);
  }
  for (const auto& fighters : players) {
    QList<OpeningTransform> transforms;
    bool unknown = false;
    for (const auto* fighter : fighters) {
      const auto transform = openingTransform(*fighter, pets, skills);
      if (!transform) { unknown = true; break; }
      transforms.append(*transform);
    }
    if (unknown) continue;

    QList<const OpeningTransform*> candidates;
    for (const auto& transform : transforms) {
      if (transform.kind == 2 && transform.native != transform.initial &&
          transform.initial.size() == 1 &&
          transform.effectAttributes.contains(transform.initial.first()) &&
          unambiguousSelfIdentity(*transform.fighter)) candidates.append(&transform);
    }
    // FormationBase stores one selfTransformSkillInfo per player. Two changed
    // self-skill holders cannot both be assigned that selection from the log.
    if (candidates.size() != 1) continue;
    const auto& candidate = *candidates.first();
    bool competingElement = false;
    for (const auto& transform : transforms) {
      if (transform.kind == 0 && transform.effectAttributes.contains(candidate.initial.first())) {
        competingElement = true;
        break;
      }
    }
    // TransformSkillConfig.effect applies self first and element second;
    // FormationBase also clears a previous selection on the same target.
    // Without that selection in the log, equal results do not prove the cause.
    if (competingElement) continue;

    const auto& fighter = *candidate.fighter;
    EmpowermentEvidence evidence;
    evidence.source = evidence.target = fighter.key;
    evidence.skillId = candidate.skillId;
    evidence.beforeAttributes = candidate.native;
    evidence.afterAttributes = candidate.initial;
    evidence.officialEffect = candidate.effects.at(candidate.effectAttributes.indexOf(candidate.initial.first()));
    evidence.description = QStringLiteral("开局属性核对：%1 的当前种族原生属性为%2，回包初始属性为%3，"
        "唯一吻合其自身幻元 %4（%5），官方效果 %6。同玩家未发现可形成相同结果的元能配置。"
        "这是原生属性与开局形成结果的核对，不是战斗中施放记录。")
        .arg(displayName(fighter), attributeNames(candidate.native, catalog), attributeNames(candidate.initial, catalog),
             referenceName(catalog, QStringLiteral("transformSkills"), candidate.skillId),
             candidate.skillId, evidence.officialEffect);
    result.insert(fighter.key, evidence);
  }
  return result;
}

} // namespace BattleReplay
