#include "domain/battle_replay_presentation.h"

#include <QtCore/QJsonArray>
#include <QtCore/QSet>
#include <cmath>
#include <limits>

namespace BattleReplay {
namespace {

std::optional<double> numeric(const QJsonValue& value) {
  if (value.isDouble() && std::isfinite(value.toDouble())) return value.toDouble();
  if (value.isString()) {
    bool ok = false;
    const double n = value.toString().toDouble(&ok);
    if (ok && std::isfinite(n)) return n;
  }
  return {};
}

QString identifier(const QJsonValue& value) {
  const auto n = numeric(value);
  if (!n || std::floor(*n) != *n || *n < 0 || *n > 2147483647.0) return {};
  return QString::number(static_cast<qint64>(*n));
}

std::optional<qint64> attackHp(const QJsonObject& numbers, bool counter) {
  const auto value = numbers[counter ? QStringLiteral("sb_hps") : QStringLiteral("att_hps")];
  if (value.isNull() || value.isUndefined()) return 0;
  if (!value.isString()) return {};
  if (value.toString().isEmpty()) return 0;
  qint64 total = 0;
  for (const auto& token : value.toString().split(QLatin1Char(','))) {
    bool ok = false;
    const auto n = token.toLongLong(&ok);
    if (!ok || std::abs(static_cast<double>(n)) > 9007199254740991.0 ||
        std::abs(static_cast<double>(total) + static_cast<double>(n)) > 9007199254740991.0) return {};
    total += n;
  }
  return total;
}

std::optional<bool> ignoresRestraint(const QJsonObject& numbers,
                                   const QJsonObject& config, bool counter) {
  // SkillNumResult.reverseToCounterNumResult clears beforeDisplayBuff. The
  // original attack's ignore marker must not leak into the reversed attack.
  if (counter) return false;
  if (!config[QStringLiteral("ignoreRestraintBuffIds")].isArray()) return {};
  const auto raw = numbers[QStringLiteral("bd_bfs")];
  if (raw.isNull() || raw.isUndefined()) return false;
  if (!raw.isString()) return {};
  const auto text = raw.toString();
  if (text.isEmpty()) return false;
  QSet<QString> ignored;
  for (const auto& id : config[QStringLiteral("ignoreRestraintBuffIds")].toArray()) {
    const auto key = identifier(id);
    if (key.isEmpty()) return {};
    ignored.insert(key);
  }
  bool result = false;
  for (const auto& token : text.split(QLatin1Char(','))) {
    const auto fields = token.split(QLatin1Char('_'));
    if (fields.size() < 2 || identifier(fields[1]).isEmpty()) return {};
    // Both add and clear entries are tested by the official consumer.
    result |= ignored.contains(identifier(fields[1]));
  }
  return result;
}

struct Attribute {
  QString id, name;
  double priority = 0;
  QJsonObject definition;
};

std::optional<Attribute> selectedAttribute(const Fighter& fighter,
                                         const QJsonObject& definitions) {
  if (!fighter.attributesKnown || fighter.attributes.isEmpty()) return {};
  Attribute selected;
  for (const auto& id : fighter.attributes) {
    const auto definition = definitions[id].toObject();
    const auto priority = numeric(definition[QStringLiteral("pri")]);
    if (definition.isEmpty() || !priority) return {};
    // BattleUtil uses >, so a priority tie keeps the FIRST recorded attribute.
    if (*priority > selected.priority)
      selected = {id, definition[QStringLiteral("nm")].toString(), *priority, definition};
  }
  if (selected.id.isEmpty()) return {};
  return selected;
}

std::optional<double> coefficient(const Attribute& source, const Attribute& target,
                                  const QJsonObject& config) {
  const auto index = numeric(config[QStringLiteral("attributeIndex")].toObject()[target.id]);
  if (!index || std::floor(*index) != *index || *index < 0 || *index > 10000) return {};
  const auto values = source.definition[QStringLiteral("rcs")].toString().split(QLatin1Char(','));
  if (*index >= values.size()) return {};
  return numeric(values[static_cast<qsizetype>(*index)]);
}

std::optional<bool> godJob(const Fighter& fighter, const QJsonObject& config) {
  // FighterInfo initializes AND changes jobTypeId from the CURRENT race's
  // dictionary primary job, never its skin name, original race or second job.
  const auto pet = config[QStringLiteral("pets")].toObject()[fighter.raceId].toObject();
  const auto jobId = identifier(pet[QStringLiteral("jobId")]);
  const auto job = config[QStringLiteral("jobs")].toObject()[jobId].toObject();
  if (jobId.isEmpty() || !job[QStringLiteral("god")].isBool()) return {};
  return job[QStringLiteral("god")].toBool();
}

QString resultTarget(const QJsonObject& result) {
  const auto team = numeric(result[QStringLiteral("tti")]);
  if (!team || std::floor(*team) != *team || *team < 0 || *team > 1 ||
      !result[QStringLiteral("tpn")].isString()) return {};
  const auto character = result[QStringLiteral("tci")];
  const auto id = character.isUndefined() || character.isNull() ? QStringLiteral("@team") : identifier(character);
  if (id.isEmpty()) return {};
  return fighterKey(static_cast<int>(*team), result[QStringLiteral("tpn")].toString(), id);
}

}  // namespace

QList<ClientPresentationCue> clientPresentationCues(
    const Step& step, const Moment& moment,
    const QMap<QString, Fighter>& phaseBefore, const QJsonObject& catalog) {
  QList<ClientPresentationCue> cues;
  const bool counter = moment.phase == QStringLiteral("counter");
  if ((!counter && moment.phase != QStringLiteral("attack")) || moment.part != QStringLiteral("result")) return cues;
  const bool representer = step.playbackKind == QStringLiteral("team-skill");
  if (step.playbackKind != QStringLiteral("normal") && !representer) return cues;
  if (counter && representer) return cues;
  const auto config = catalog[QStringLiteral("clientPresentation")].toObject();
  if (config[QStringLiteral("schemaVersion")].toInt() != 1) return cues;
  const auto sourceKey = moment.actor;
  if (sourceKey.isEmpty() || !phaseBefore.contains(sourceKey)) return cues;
  const auto source = phaseBefore[sourceKey];
  const auto raw = step.raw.toObject();
  if (raw.isEmpty()) return cues;
  const auto definitions = catalog[QStringLiteral("attributes")].toObject();
  const auto sourceAttribute = selectedAttribute(source, definitions);
  struct Target { QString key, path; QJsonObject numbers; };
  QList<Target> targets;
  if (counter) {
    // A reversed result has exactly the original actor as target. Counter
    // judgement flags are reset by SkillResult.genCounterResult.
    int effective = 0;
    bool foundCounter = false;
    for (const auto& value : raw[QStringLiteral("trl")].toArray()) {
      const auto result = value.toObject();
      if (!result[QStringLiteral("isse")].toBool()) continue;
      ++effective;
      foundCounter |= result[QStringLiteral("issb")].toBool() && resultTarget(result) == sourceKey;
    }
    const auto originalSkills = catalog[step.lastSkillStatus == QStringLiteral("applied")
      ? QStringLiteral("skillLastEffects") : QStringLiteral("skills")].toObject();
    const auto originalSkill = originalSkills[step.playbackSkillId].toObject();
    const auto counterSkill = catalog[QStringLiteral("skills")].toObject()[step.counterSkillId].toObject();
    if (effective != 1 || !foundCounter || originalSkill[QStringLiteral("attackType")].toString() != QStringLiteral("近身") ||
        counterSkill.isEmpty() || !raw[QStringLiteral("cn")].isObject() || step.actor == sourceKey) return cues;
    targets.append({step.actor, step.path + QStringLiteral("/cn"), raw[QStringLiteral("cn")].toObject()});
  } else {
    if (sourceKey != step.actor) return cues;
    const auto results = raw[QStringLiteral("trl")].toArray();
    for (qsizetype i = 0; i < results.size(); ++i) {
      const auto result = results[i].toObject();
      // onEffectFrame filters effect, then handleHpAndPowerChanges runs BEFORE
      // the miss/immunity branch. Those flags must not suppress these cues.
      if (!result[QStringLiteral("isse")].toBool() || !result[QStringLiteral("tn")].isObject()) continue;
      const auto key = resultTarget(result);
      if (!key.isEmpty()) targets.append({key, step.path + QStringLiteral("/trl/%1/tn").arg(i), result[QStringLiteral("tn")].toObject()});
    }
  }
  const auto weak = numeric(config[QStringLiteral("weakThreshold")]);
  const auto strong = numeric(config[QStringLiteral("strongThreshold")]);
  QSet<QString> added;
  for (const auto& result : targets) {
    if (!phaseBefore.contains(result.key) || !moment.state.contains(result.key)) continue;
    const auto target = phaseBefore[result.key];
    const auto after = moment.state[result.key];
    auto append = [&](const QString& kind, const QString& recipient, const QString& label,
                      const QString& explanation, const QStringList& evidence, const QString& attribute = QString()) {
      const auto identity = kind + QLatin1Char('|') + recipient + QLatin1Char('|') + result.key + QLatin1Char('|') + attribute;
      if (added.contains(identity)) return;
      added.insert(identity);
      cues.append({kind, recipient, sourceKey, result.key, label,
        QStringLiteral("官方客户端提示：") + explanation + QStringLiteral("；不是战报直接判定或服务端伤害计算。"), evidence, attribute});
    };
    const auto ignore = ignoresRestraint(result.numbers, config, counter);
    const auto targetAttribute = selectedAttribute(target, definitions);
    bool specialStrong = false;
    if (ignore && !*ignore && sourceAttribute && targetAttribute && weak && strong && *weak < *strong) {
      const auto rate = coefficient(*sourceAttribute, *targetAttribute, config);
      const auto evidence = QStringList{source.attributesPath, target.attributesPath, result.path + QStringLiteral("/bd_bfs"),
        QStringLiteral("catalog:/clientPresentation/attributeIndex"),
        QStringLiteral("PlayEffectCommand.as:359-385"), QStringLiteral("BattleUtil.as:383-407")};
      if (rate && *rate <= *weak) {
        append(QStringLiteral("weak"), result.key, QStringLiteral("弱效"),
          QStringLiteral("按当前属性 %1 → %2 与官方弱效阈值显示").arg(sourceAttribute->name, targetAttribute->name), evidence);
      } else if (rate && *rate >= *strong) {
        const auto priorities = config[representer ? QStringLiteral("representerGodPriorities") : QStringLiteral("godPriorities")];
        // An incomplete priority policy cannot establish the exact strong cue.
        if (priorities.isArray()) {
          for (const auto& attribute : source.attributes) {
            const auto definition = definitions[attribute].toObject();
            const auto priority = numeric(definition[QStringLiteral("pri")]);
            bool special = false;
            for (const auto& value : priorities.toArray()) special |= priority && numeric(value) == priority;
            if (!special) continue;
            specialStrong = true;
            append(QStringLiteral("strong"), result.key, QStringLiteral("强效·%1").arg(definition[QStringLiteral("nm")].toString()),
              QStringLiteral("按当前属性 %1 → %2 显示 %3 属性强效").arg(sourceAttribute->name, targetAttribute->name, definition[QStringLiteral("nm")].toString()), evidence, attribute);
          }
          if (!specialStrong) append(QStringLiteral("strong"), result.key, QStringLiteral("强效"),
            QStringLiteral("按当前属性 %1 → %2 与官方强效阈值显示").arg(sourceAttribute->name, targetAttribute->name), evidence);
        }
      }
    }
    // PlayRepresenterEffectCommand intentionally lacks job and TianQi cues.
    if (representer) continue;
    const auto sourceGod = godJob(source, config), targetGod = godJob(target, config);
    if (sourceGod && targetGod && *sourceGod != *targetGod) {
      const auto evidence = QStringList{source.formPath, target.formPath, QStringLiteral("FighterInfo.as:135-140,235-240"),
        QStringLiteral("PetJob.as:isGodJob"), QStringLiteral("PlayEffectCommand.as:388-399")};
      append(*sourceGod ? QStringLiteral("god-job-attack") : QStringLiteral("god-job-defend"),
        *sourceGod ? sourceKey : result.key, *sourceGod ? QStringLiteral("神职攻击") : QStringLiteral("神职受击"),
        *sourceGod ? QStringLiteral("当前攻击者是神职、目标不是神职，显示神职攻击提示") :
                     QStringLiteral("当前目标是神职、攻击者不是神职，显示神职受击提示"), evidence);
    }
    // TianQi uses info.raceId (the testing getter maps to 9999). jobTypeId
    // above instead retains the job cached from _nRaceId's real dictionary.
    const bool testing = catalog[QStringLiteral("pets")].toObject()[source.raceId].toObject()[QStringLiteral("desc")].toString() == QStringLiteral("内测");
    const auto pet = config[QStringLiteral("pets")].toObject()[testing ? QStringLiteral("9999") : source.raceId].toObject();
    const auto delta = attackHp(result.numbers, counter);
    if (specialStrong && pet[QStringLiteral("isTianQi")].isBool() && pet[QStringLiteral("isTianQi")].toBool() &&
        target.hp && after.hp && delta && *after.hp <= 0 &&
        static_cast<double>(*target.hp) + static_cast<double>(*delta) <= 0) {
      append(QStringLiteral("tianqi"), sourceKey, QStringLiteral("天启"),
        QStringLiteral("当前原型带官方“天启”标记，本次符合属性专属强效且攻击数值使目标生命不大于 0"),
        {source.formPath, target.hpPath, result.path + (counter ? QStringLiteral("/sb_hps") : QStringLiteral("/att_hps")),
          QStringLiteral("PetDictionaryDataItem.as:isTianQi"), QStringLiteral("PlayEffectCommand.as:412-415")},
        source.attributes.value(0));
    }
  }
  return cues;
}

}  // namespace BattleReplay
