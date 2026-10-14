#include "domain/battle_replay_state.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace BattleReplay {
void retainUnlinkedReference(Step& step, const UnlinkedReference& reference, QStringList& issues) {
  for (const auto& prior : step.unlinkedReferences)
    if (prior.kind == reference.kind && prior.key == reference.key && prior.path == reference.path) return;
  auto kept = reference;
  if (kept.reason.isEmpty()) kept.reason = QStringLiteral("该实例 ID 未在对应玩家的初始阵容注册；原文独立保留，不推定其精灵身份或位置");
  const auto object = kept.raw.toObject();
  for (const auto& flag : {QStringLiteral("isse"), QStringLiteral("ishit"), QStringLiteral("isbl"),
      QStringLiteral("iscr"), QStringLiteral("isIm"), QStringLiteral("issb")})
    if (object[flag].isBool()) kept.flags.insert(flag, object[flag].toBool());
  step.unlinkedReferences.append(kept);
  const auto diagnostic = kept.path + QStringLiteral("：未关联引用 [%1]；%2").arg(kept.id, kept.reason);
  if (!issues.contains(diagnostic)) issues.append(diagnostic);
  const auto note = QStringLiteral("未关联引用 [%1] 的原始目标、数值和标记单独保留，不加入阵容状态。").arg(kept.id);
  if (!step.playbackNotes.contains(note)) step.playbackNotes.append(note);
}
namespace {
std::optional<qint64> number(const QJsonValue& value) {
  if (value.isString()) {
    bool ok = false; const auto n = value.toString().toLongLong(&ok);
    if (ok && n >= -9007199254740991LL && n <= 9007199254740991LL) return n;
  } else if (value.isDouble()) {
    const auto n = value.toDouble();
    if (std::isfinite(n) && std::floor(n) == n && std::abs(n) <= 9007199254740991.0) return static_cast<qint64>(n);
  }
  return {};
}
QString shown(std::optional<qint64> n) { return n ? QString::number(*n) : QStringLiteral("待核实"); }
QStringList tokens(const QJsonValue& v) { return v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts); }
QString skillId(const QJsonValue& value) {
  const auto id = number(value);
  return id && *id > 0 ? QString::number(*id) : QString();
}
QString buffId(const QJsonValue& value) {
  const auto id = number(value);
  return id && *id >= std::numeric_limits<int>::min() && *id <= std::numeric_limits<int>::max()
    ? QString::number(*id) : QString();
}
bool isTestingForm(const Fighter& fighter, const QJsonObject& catalog) {
  // FighterInfo.isTesting checks this exact dictionary description. A name,
  // skin ID, missing dictionary item or user nickname is not evidence.
  return catalog[QStringLiteral("pets")].toObject()[fighter.raceId].toObject()[QStringLiteral("desc")].toString() == QStringLiteral("内测");
}
QString testingSkill(const QString& logged, const Fighter& fighter, const QJsonObject& catalog) {
  if (!isTestingForm(fighter, catalog) || fighter.raceId.isEmpty()) return logged;
  auto result = logged;
  const auto at = result.indexOf(fighter.raceId);
  // AS3 String.replace with a String pattern replaces the first occurrence.
  if (at >= 0) result.replace(at, fighter.raceId.size(), QStringLiteral("9999"));
  return result;
}
bool isTeamAction(const QJsonObject& action) {
  const auto type = action[QStringLiteral("orrt")];
  return !type.isUndefined() && !type.isNull() && type.toInt() != 1;
}
struct Event {
  QString field, path, buff;
  QJsonValue raw;
  qint64 group = 0;
  std::optional<qint64> delta;
  int operation = -1;
};
struct Record {
  QString key, path;
  QJsonObject num, target;
  int role = 0; // 0 source, 1 trl, 2 prl
  QList<Event> before, after;
  QList<qint64> beforeIds, afterIds;
  bool unresolvedSpecial = false;
};
struct BuffPlaybackPlan {
  QList<QList<Event>> batches;
  QList<Event> callbackTail;
  int callbackBatch = -1;
};
struct Engine {
  Step& step;
  QMap<QString, Fighter>& state;
  const QJsonObject& catalog;
  QStringList& issues;
  QList<Record> records;
  Moment pending;
  QList<KeyEvent> pendingEvents;
  QString unresolvedReason;
  bool sequence = false, effects = false, close = false, summon = false;

  bool unlinked(int team, const QString& player, const QString& id, const QString& kind,
                const QString& path, const QJsonValue& raw, const QJsonObject& numbers = {}) {
    const auto key = fighterKey(team, player, id);
    if (state.contains(key)) return false;
    UnlinkedReference reference;
    reference.kind = kind; reference.key = key; reference.team = team;
    reference.player = player; reference.id = id; reference.path = path;
    reference.raw = raw; reference.numbers = numbers;
    retainUnlinkedReference(step, reference, issues);
    return true;
  }
  void transition(const QString& kind, const QString& key) {
    if (!state.contains(key) || state.value(key).representer) return;
    // Preserve the first occurrence order inside this command boundary. A
    // later opposite crossing (zero -> restored) must survive a net-zero HP.
    for (const auto& event : pendingEvents) if (event.kind == kind && event.key == key) return;
    pendingEvents.append({kind, key, static_cast<int>(step.moments.size())});
  }

  void issue(const QString& path, const QString& message) {
    const auto text = path + QStringLiteral("：") + message;
    if (!issues.contains(text)) issues.append(text);
  }
  void playbackNote(const QString& message) {
    if (!step.playbackNotes.contains(message)) step.playbackNotes.append(message);
  }
  void detail(const QString& key, const QString& text, const QString& path, const QJsonValue& raw) {
    pending.details.append({pending.title, fighterName(state.value(key)), text, path, raw});
    if (!step.affected.contains(key)) step.affected.append(key);
  }
  QString objectName(const QString& key) const {
    const auto f = state.value(key);
    const auto name = f.formName.isEmpty() ? f.name : f.formName;
    return name.isEmpty() ? key : name;
  }
  QString actionCause() const {
    return state.value(step.actor).representer ? QStringLiteral("team") : QStringLiteral("skill");
  }
  Effect makeEffect(const QString& kind, const QString& key, const QString& cause,
                    const QString& source, const QString& id, const QString& path, const QJsonValue& raw) const {
    Effect e; e.kind = kind; e.target = key; e.cause = cause; e.source = source; e.causeId = id; e.path = path; e.raw = raw;
    if (cause == QStringLiteral("skill") || cause == QStringLiteral("target")) {
      const auto team = state.value(step.actor).representer;
      const bool knownSkill = !step.playbackSkillId.isEmpty();
      e.causeId = knownSkill ? step.playbackSkillId : team ? valueText(step.raw.toObject()[QStringLiteral("epbi")]) : step.loggedSkillId;
      e.causeName = referenceName(catalog, team && !knownSkill ? QStringLiteral("buffs") : QStringLiteral("skills"), e.causeId);
    } else if (cause == QStringLiteral("team")) {
      e.causeId = step.playbackSkillId.isEmpty() ? valueText(step.raw.toObject()[QStringLiteral("epbi")]) : step.playbackSkillId;
      e.causeName = referenceName(catalog, step.playbackSkillId.isEmpty() ? QStringLiteral("buffs") : QStringLiteral("skills"), e.causeId);
    } else if (cause == QStringLiteral("buff")) e.causeName = referenceName(catalog, QStringLiteral("buffs"), id);
    else if (cause == QStringLiteral("counter")) {
      e.causeId = step.counterSkillId;
      e.causeName = e.causeId.isEmpty() ? QStringLiteral("反击") : QStringLiteral("反击 · ") + referenceName(catalog, QStringLiteral("skills"), e.causeId);
    }
    else if (cause == QStringLiteral("calibration")) e.causeName = QStringLiteral("最终数值校准");
    else e.causeName = QStringLiteral("日志记录");
    return e;
  }
  void effect(Effect e, const QString& description) {
    if (e.kind == QStringLiteral("buff") && !e.buffSource.isEmpty())
      e.text = QStringLiteral("%1：%2；施加者：%3").arg(objectName(e.target), description,
        e.buffSourceName.isEmpty() ? e.buffSource : e.buffSourceName);
    else if (!e.source.isEmpty())
      e.text = QStringLiteral("%1 → %2：%3；%4").arg(objectName(e.source), objectName(e.target), e.causeName, description);
    else if (e.cause == QStringLiteral("calibration"))
      e.text = QStringLiteral("%1：%2；日志未说明校准差异的来源").arg(objectName(e.target), description);
    else if (e.cause == QStringLiteral("buff") && e.kind == QStringLiteral("value"))
      e.text = QStringLiteral("%1：%2 对应的%3；施加者未记录").arg(objectName(e.target), e.causeName, description);
    else e.text = QStringLiteral("%1：%2；施加者未记录").arg(objectName(e.target), description);
    e.moment = static_cast<int>(step.moments.size());
    pending.effects.append(e);
    if (e.metric == QStringLiteral("生命") && e.before && e.after) {
      if (*e.before > 0 && *e.after == 0) transition(QStringLiteral("生命归零"), e.target);
      if (*e.before == 0 && *e.after > 0) transition(QStringLiteral("生命恢复"), e.target);
    } else if (e.metric == QStringLiteral("生命上限") && e.before != e.after)
      transition(QStringLiteral("生命上限变化"), e.target);
  }
  void targetRelations(const QJsonObject& action) {
    for (const auto& list : {QStringLiteral("trl"), QStringLiteral("prl")}) {
      const bool teamTarget = list == QStringLiteral("prl");
      const auto targets = action[list].toArray();
      for (qsizetype i = 0; i < targets.size(); ++i) {
        const auto t = targets[i].toObject();
        const int team = t[QStringLiteral("tti")].toInt(-1);
        const auto player = valueText(t[QStringLiteral("tpn")]), id = targetFighterId(t[QStringLiteral("tci")]);
        const auto path = step.path + QStringLiteral("/%1/%2").arg(list).arg(i);
        if (unlinked(team, player, id, teamTarget ? QStringLiteral("team-target") : QStringLiteral("target"),
            path, t, t[QStringLiteral("tn")].toObject())) continue;
        const auto key = fighterKey(team, player, id);
        QStringList flags;
        for (const auto& pair : QList<QPair<QString, QString>>{{"isse", "技能生效"}, {"ishit", "命中"}, {"isbl", "格挡"},
          {"iscr", "暴击"}, {"isIm", "免疫"}, {"issb", "反击"}})
          if (t[pair.first].isBool()) flags.append(pair.second + (t[pair.first].toBool() ? QStringLiteral("：是") : QStringLiteral("：否")));
        auto e = makeEffect(teamTarget ? QStringLiteral("team-target") : QStringLiteral("target"), key,
          teamTarget || !state.contains(step.actor) ? QStringLiteral("record") : QStringLiteral("target"),
          teamTarget || !state.contains(step.actor) ? QString() : step.actor, {}, path, t);
        e.changed = false;
        effect(e, (teamTarget ? QStringLiteral("队伍目标记录%1") : QStringLiteral("目标记录%1"))
          .arg(flags.isEmpty() ? QStringLiteral("，未记录命中或生效标记") : QStringLiteral("；") + flags.join(QStringLiteral("；"))));
      }
    }
  }
  void begin(const QString& phase, const QString& part, const QString& title, const QString& note = {},
             int groupIndex = -1, const QString& actor = {}) {
    pending = {}; pending.title = title; pending.note = note;
    pendingEvents.clear();
    pending.phase = phase; pending.part = part; pending.groupIndex = groupIndex;
    pending.actor = actor.isEmpty() ? step.actor : actor;
  }
  void finish(bool force = false) {
    if (!force && pending.details.isEmpty() && pending.effects.isEmpty()) return;
    for (const auto& event : pendingEvents) {
      if (!step.keyChanges.contains(event.kind)) step.keyChanges.append(event.kind);
      step.events.append(event);
    }
    pending.state = state; step.effects.append(pending.effects); step.moments.append(pending);
  }
  void badEvent(Record& r, const QString& field, const QString& path, const QJsonValue& raw) {
    issue(path, QStringLiteral("记录格式无法确认；原值保留，不补零"));
    if (!state.contains(r.key)) return;
    auto& f = state[r.key];
    if (field.endsWith(QStringLiteral("shps"))) { f.shield.reset(); f.shieldPath = path; }
    else if (field.endsWith(QStringLiteral("hps"))) { f.hp.reset(); f.hpPath = path; }
    else if (field.endsWith(QStringLiteral("vgs"))) { f.power.reset(); f.powerPath = path; }
    else { f.buffsKnown = false; f.buffPath = path; }
    updatePresence(f);
    detail(r.key, QStringLiteral("未应用的记录：%1").arg(valueText(raw)), path, raw);
  }
  void readGroups(Record& r, bool before) {
    auto& events = before ? r.before : r.after;
    auto& ids = before ? r.beforeIds : r.afterIds;
    const QString prefix = before ? QStringLiteral("bb_") : QStringLiteral("ab_");
    const QString display = before ? QStringLiteral("bd_bfs") : QStringLiteral("ad_bfs");
    for (const auto& field : {prefix + QStringLiteral("hps"), prefix + QStringLiteral("shps"), prefix + QStringLiteral("vgs"), display}) {
      const auto raw = r.num[field];
      if (raw.isUndefined() || raw.isNull() || raw == QJsonValue(QString())) continue;
      if (!raw.isString()) { badEvent(r, field, r.path + QLatin1Char('/') + field, raw); continue; }
      const auto parts = tokens(raw);
      QSet<qint64> hpGroups;
      for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto values = parts[i].split(QLatin1Char('_'));
        Event e; e.field = field; e.path = r.path + QLatin1Char('/') + field; e.raw = parts[i];
        const auto group = values.size() == 3 ? number(values[2]) : std::nullopt;
        const auto first = values.size() == 3 ? number(values[0]) : std::nullopt;
        const auto second = values.size() == 3 ? number(values[1]) : std::nullopt;
        const auto identity = values.size() == 3 ? buffId(values[field == display ? 1 : 0]) : QString();
        if (!group || *group < 0 || *group > 100000 || !first || !second ||
            identity.isEmpty() || (field == display && *first != 0 && *first != 1)) {
          badEvent(r, field, e.path, e.raw); continue;
        }
        e.group = *group;
        if (field == prefix + QStringLiteral("hps")) {
          if (hpGroups.contains(e.group)) {
            const auto reason = QStringLiteral("生命变化存在重复组号 %1；官方组索引与组字典计数不一致，执行次数和顺序待核实，不重复结算").arg(e.group);
            issue(e.path, reason); playbackNote(reason);
            if (unresolvedReason.isEmpty()) unresolvedReason = reason;
          }
          hpGroups.insert(e.group);
        }
        e.buff = identity;
        if (field == display) e.operation = static_cast<int>(*first);
        else e.delta = second;
        events.append(e); if (!ids.contains(e.group)) ids.append(e.group);
      }
    }
    const auto units = r.num[QStringLiteral("rl")].toArray();
    for (qsizetype i = 0; i < units.size(); ++i) {
      const auto obj = units[i].toObject(); const auto timing = number(obj[QStringLiteral("o")]);
      const auto group = number(obj[QStringLiteral("changeset")]);
      if (!timing || (*timing != 1 && *timing != 2) || !group || *group < 0 || *group > 100000) {
        r.unresolvedSpecial = true;
        if (before) {
          const auto path = r.path + QStringLiteral("/rl/%1").arg(i);
          issue(path, QStringLiteral("特殊记录的时机或组号待核实；不推断执行位置"));
          detail(r.key, QStringLiteral("未排入顺序的特殊记录：%1").arg(valueText(obj)), path, obj);
        }
        continue;
      }
      if (*timing != (before ? 1 : 2)) continue;
      const auto type = obj[QStringLiteral("t")].toInt(-1);
      if ((type < 1 || type > 6) && type != 100 && type != 101) r.unresolvedSpecial = true;
      Event e; e.field = QStringLiteral("rl"); e.group = *group; e.raw = obj;
      e.path = r.path + QStringLiteral("/rl/%1").arg(i); events.append(e);
      if (!ids.contains(e.group)) ids.append(e.group);
    }
    std::sort(ids.begin(), ids.end());
  }
  void addRecord(const QString& key, const QString& path, const QJsonObject& num, int role, const QJsonObject& target = {}) {
    // SkillNumResult's group indices exist even when getFighterById fails.
    // Keep those read-only scheduler inputs, while every state writer below
    // requires an actually registered fighter.
    Record r; r.key = key; r.path = path; r.num = num; r.role = role; r.target = target;
    const auto units = num[QStringLiteral("rl")].toArray();
    const auto host = state.value(key);
    for (qsizetype i = 0; i < units.size(); ++i) {
      const auto unit = units[i].toObject(); const auto type = unit[QStringLiteral("t")].toInt(-1);
      const auto unitPath = path + QStringLiteral("/rl/%1").arg(i);
      if (type == 3) unlinked(unit[QStringLiteral("ti")].toInt(-1), valueText(unit[QStringLiteral("pn")]),
        valueText(unit[QStringLiteral("ci")]), QStringLiteral("record"), unitPath, unit, num);
      else if ((type == 4 || type == 5) && state.contains(key)) unlinked(host.team, host.player,
        type == 4 ? QStringLiteral("@team") : valueText(unit[QStringLiteral("ci")]), QStringLiteral("record"), unitPath, unit, num);
    }
    readGroups(r, true); readGroups(r, false); records.append(r);
  }
  void delta(const Record& r, const Event& e, const QString& metric,
             const QString& cause = QStringLiteral("buff"), const QString& source = {}) {
    if (!state.contains(r.key)) return;
    auto& f = state[r.key];
    if (f.representer) {
      detail(r.key, QStringLiteral("队伍对象数值记录 %1：%2").arg(metric, valueText(e.raw)), e.path, e.raw); return;
    }
    auto& value = metric == QStringLiteral("生命") ? f.hp : metric == QStringLiteral("气势") ? f.power : f.shield;
    auto& path = metric == QStringLiteral("生命") ? f.hpPath : metric == QStringLiteral("气势") ? f.powerPath : f.shieldPath;
    const auto old = value;
    if (value && e.delta) {
      const auto sum = *value + *e.delta;
      if (sum < -9007199254740991LL || sum > 9007199254740991LL) value.reset();
      else value = metric == QStringLiteral("生命") ? std::max<qint64>(0, sum) : sum;
    } else value.reset();
    path = e.path;
    if (metric == QStringLiteral("生命")) updatePresence(f);
    detail(r.key, QStringLiteral("%1 %2 → %3（变化 %4%5）").arg(metric, shown(old), shown(value), shown(e.delta),
      e.buff.isEmpty() ? QString() : QStringLiteral("；%1").arg(referenceName(catalog, QStringLiteral("buffs"), e.buff))), e.path, e.raw);
    auto change = makeEffect(QStringLiteral("value"), r.key, cause, source, e.buff, e.path, e.raw);
    change.metric = metric; change.before = old; change.after = value; change.delta = e.delta;
    change.changed = !old || !value || old != value || (e.delta && *e.delta != 0);
    effect(change, QStringLiteral("%1 %2 → %3（记录变化 %4）").arg(metric, shown(old), shown(value), shown(e.delta)));
  }
  void buff(const Record& r, const Event& e, int playbackBatch = -1, bool afterCallback = false) {
    if (!state.contains(r.key)) return;
    auto& f = state[r.key]; const auto old = f.buffs.count(e.buff);
    f.buffMembers = recordedBuffMembers(f);
    // The official display tuple contains operation, ID and changeset only.
    // Neither its holder nor this action's actor identifies the applier.
    BuffMember member{e.buff, {}, {}, e.path, -1};
    if (e.operation == 1) {
      f.buffs.append(e.buff); f.buffMembers.append(member);
    } else {
      const auto index = f.buffs.indexOf(e.buff);
      if (index >= 0) {
        member = f.buffMembers[index];
        f.buffs.removeAt(index); f.buffMembers.removeAt(index);
      }
    }
    f.buffPath = e.path;
    const auto config = catalog[QStringLiteral("buffs")].toObject()[e.buff].toObject();
    const bool restoresDisplay = e.operation == 1 && number(config[QStringLiteral("buffWaitType")]).value_or(0) > 0 &&
        (f.summoned || (f.clone && f.cloneCalled));
    // summonAppear only restores child display layers. It does not set the
    // summon-called flag, establish a fresh entry or recover any HP.
    if (restoresDisplay) {
      f.displayLayersRestored = true; f.displayLayersPath = e.path;
      updatePresence(f);
    }
    const auto operationText = e.operation == 1 ? QStringLiteral("获得") :
      old == 0 ? QStringLiteral("移除记录（无匹配项）") : QStringLiteral("移除一条");
    detail(r.key, QStringLiteral("%1 %2%3 · 记录数 %4 → %5").arg(
      operationText, referenceName(catalog, QStringLiteral("buffs"), e.buff),
      number(config[QStringLiteral("hd")]) == 1 ? QStringLiteral("（隐藏 Buff）") : QString()).arg(old).arg(f.buffs.count(e.buff)), e.path, e.raw);
    auto change = makeEffect(QStringLiteral("buff"), r.key, QStringLiteral("buff"), {}, e.buff, e.path, e.raw);
    change.buffOperation = e.operation;
    change.restoresDisplayLayers = restoresDisplay;
    const auto waitType = number(config[QStringLiteral("buffWaitType")]).value_or(0);
    change.buffWaitType = config.isEmpty() ? -1 : e.operation == 1 ? static_cast<int>(waitType) : 0;
    change.buffPlaybackBatch = playbackBatch; change.afterWaitCallback = afterCallback;
    if (!config.isEmpty()) change.buffWaitCondition = QStringLiteral("none");
    if (e.operation == 1 && waitType > 0) {
      if (waitType == 3) {
        change.visibilityCommand = QStringLiteral("show-up");
        change.buffWaitCondition = QStringLiteral("force-show");
        playbackNote(QStringLiteral("显现型等待 Buff 会确保模型显现后等待；显现不表示生命恢复。"));
      } else {
        change.buffWaitCondition = QStringLiteral("visible-or-skip");
        playbackNote(QStringLiteral("等待 Buff 按配置队列展示；官方在模型透明度为 0 时先回调再无等待处理。战报未记录透明度，实际等待时机未确认，不能用生命或待召唤状态代替。"));
      }
    }
    change.buffSource = member.source; change.buffSourceName = member.sourceName;
    change.buffSourceTeam = member.sourceTeam; change.buffSourcePath = member.path;
    change.metric = QStringLiteral("Buff 记录数"); change.before = old; change.after = f.buffs.count(e.buff);
    change.delta = *change.after - *change.before; change.changed = change.before != change.after;
    effect(change, QStringLiteral("%1 %2%3；记录数 %4 → %5").arg(
      operationText, change.causeName,
      number(config[QStringLiteral("hd")]) == 1 ? QStringLiteral("（隐藏 Buff）") : QString()).arg(old).arg(f.buffs.count(e.buff)));
  }
  QList<Event> eventsAt(const Record& r, bool before, int index) const {
    const auto& ids = before ? r.beforeIds : r.afterIds;
    if (index >= ids.size()) return {};
    QList<Event> found; for (const auto& e : before ? r.before : r.after) if (e.group == ids[index]) found.append(e);
    return found;
  }
  BuffPlaybackPlan planBuffPlayback(const QList<Event>& events) const {
    BuffPlaybackPlan plan; QList<Event> changes;
    for (const auto& e : events) if (e.operation >= 0) changes.append(e);
    const auto waitType = [&](const Event& e) {
      if (e.operation != 1) return qint64(0);
      const auto cfg = catalog[QStringLiteral("buffs")].toObject()[e.buff].toObject();
      return number(cfg[QStringLiteral("buffWaitType")]).value_or(0);
    };
    int batch = 0;
    for (qsizetype i = 0; i < changes.size(); ++i) {
      while (plan.batches.size() <= batch) plan.batches.append(QList<Event>{});
      plan.batches[batch].append(changes[i]);
      const auto type = waitType(changes[i]);
      if (type == 2) {
        // Flash BattleUtil.addWaitBuffOneByOne: its completion starts the
        // remaining list; even a non-wait tail precedes the outer callback.
        ++batch;
      } else if (type > 0) {
        bool anotherWait = false;
        for (qsizetype j = i + 1; j < changes.size(); ++j) anotherWait |= waitType(changes[j]) > 0;
        if (!anotherWait) {
          // addWaitBuffAllAtOnce notifies the outer command BEFORE processing
          // the remaining non-wait changes. Preserve that separate boundary.
          plan.callbackBatch = batch + 1;
          for (++i; i < changes.size(); ++i) plan.callbackTail.append(changes[i]);
          break;
        }
      }
    }
    return plan;
  }
  void appear(const QString& key, const QString& path, const QString& reason, const QJsonValue& raw,
              const QString& cause = QStringLiteral("record"), const QString& source = {}, const QString& causeId = {}) {
    if (!state.contains(key)) return;
    auto& f = state[key];
    const bool changed = !f.entered || !f.entryKnown;
    const bool newlyEntered = !f.entered;
    f.entered = true; f.entryKnown = true; f.presencePath = path; updatePresence(f);
    if (changed) {
      detail(key, QStringLiteral("登场：%1；%2").arg(reason, f.presence), path, raw);
      effect(makeEffect(QStringLiteral("appearance"), key, cause, source, causeId, path, raw), QStringLiteral("登场；%1").arg(reason));
      if (newlyEntered) transition(QStringLiteral("登场"), key);
    }
  }
  QString attributes(const QStringList& ids) const {
    QStringList names; for (const auto& id : ids) names.append(referenceName(catalog, QStringLiteral("attributes"), id));
    return names.isEmpty() ? QStringLiteral("未记录") : names.join(QStringLiteral("、"));
  }
  void special(const Record& r, const Event& e) {
    if (!state.contains(r.key)) return;
    const auto obj = e.raw.toObject(); const auto type = obj[QStringLiteral("t")].toInt(-1);
    auto key = r.key; auto text = QString();
    if (type == 2) {
      auto& f = state[key]; const auto hp = f.hp, max = f.maxHp;
      f.hp = number(obj[QStringLiteral("new_curv")]); f.maxHp = number(obj[QStringLiteral("new_maxv")]);
      if (f.hp) f.hp = std::max<qint64>(0, *f.hp);
      f.hpPath = e.path + QStringLiteral("/new_curv"); f.maxHpPath = e.path + QStringLiteral("/new_maxv"); f.lastUnit = obj;
      updatePresence(f);
      text = QStringLiteral("生命重设 %1 → %2；上限 %3 → %4").arg(shown(hp), shown(f.hp), shown(max), shown(f.maxHp));
      const auto addValue = [&](const QString& metric, std::optional<qint64> before, std::optional<qint64> after,
                                const QString& oldField, const QString& newField) {
        auto change = makeEffect(QStringLiteral("value"), key, QStringLiteral("record"), {}, {}, e.path, obj);
        change.metric = metric; change.before = before; change.after = after;
        if (before && after) change.delta = *after - *before;
        change.recordedBefore = number(obj[oldField]); change.recordedAfter = number(obj[newField]);
        if (change.recordedBefore && change.recordedAfter) change.recordedDelta = *change.recordedAfter - *change.recordedBefore;
        change.changed = !before || !after || before != after;
        auto description = QStringLiteral("%1重设 %2 → %3").arg(metric, shown(before), shown(after));
        if (change.recordedDelta) description += QStringLiteral("；回包变化 %1（%2 → %3）")
          .arg(shown(change.recordedDelta), shown(change.recordedBefore), shown(change.recordedAfter));
        effect(change, description);
      };
      addValue(QStringLiteral("生命"), hp, f.hp, QStringLiteral("old_curv"), QStringLiteral("new_curv"));
      addValue(QStringLiteral("生命上限"), max, f.maxHp, QStringLiteral("old_maxv"), QStringLiteral("new_maxv"));
    } else if (type == 3) {
      key = fighterKey(obj[QStringLiteral("ti")].toInt(-1), valueText(obj[QStringLiteral("pn")]), valueText(obj[QStringLiteral("ci")]));
      if (!state.contains(key)) return;
      state[key].lastUnit = obj;
      text = QStringLiteral("召唤记录：此记录不单独改变登场状态；召唤技能的实际执行阶段另记，等待型 Buff 只恢复显示层");
      auto change = makeEffect(QStringLiteral("observation"), key, QStringLiteral("record"), {}, {}, e.path, obj);
      change.changed = false; effect(change, text);
    } else if (type == 5) {
      const auto host = state.value(r.key);
      key = fighterKey(host.team, host.player, valueText(obj[QStringLiteral("ci")]));
      if (!state.contains(key)) return;
      auto& f = state[key]; f.clone = f.cloneCalled = true; f.lastUnit = obj;
      appear(key, e.path, QStringLiteral("分身登场指令"), obj);
      text = QStringLiteral("分身已呼出；归属于本数值记录的玩家 %1").arg(host.player);
    } else if (type == 4) {
      const auto f = state.value(key); key = fighterKey(f.team, f.player, QStringLiteral("@team"));
      if (!state.contains(key)) return;
      state[key].lastUnit = obj;
      TeamProgress progress; progress.buffId = buffId(obj[QStringLiteral("pbid")]); progress.path = e.path;
      progress.current = number(obj[QStringLiteral("proa")]); progress.maximum = number(obj[QStringLiteral("mp")]);
      progress.previous = number(obj[QStringLiteral("prob")]); progress.delta = number(obj[QStringLiteral("vc")]);
      if (obj[QStringLiteral("isse")].isBool()) progress.skillEffected = obj[QStringLiteral("isse")].toBool();
      progress.raw = obj;
      if (progress.buffId.isEmpty())
        issue(e.path + QStringLiteral("/pbid"), QStringLiteral("进度 Buff 编号无法确认；事件和原始数值保留，不合并到任意进度条"));
      else state[key].teamProgress.insert(progress.buffId, progress);
      const auto progressName = progress.buffId.isEmpty() ? QStringLiteral("Buff 编号待核实") : referenceName(catalog, QStringLiteral("buffs"), progress.buffId);
      text = QStringLiteral("队伍进度 %1：%2 → %3 / %4").arg(progressName,
        valueText(obj[QStringLiteral("prob")]), valueText(obj[QStringLiteral("proa")]), valueText(obj[QStringLiteral("mp")]));
      auto change = makeEffect(QStringLiteral("progress"), key, QStringLiteral("record"), {}, progress.buffId, e.path, obj);
      change.metric = QStringLiteral("队伍进度"); change.causeName = progressName;
      change.before = number(obj[QStringLiteral("prob")]); change.after = number(obj[QStringLiteral("proa")]);
      change.delta = number(obj[QStringLiteral("vc")]);
      change.changed = !change.before || !change.after || change.before != change.after;
      effect(change, text);
    } else if (type == 1 || type == 6) {
      auto& f = state[key]; f.lastUnit = obj;
      const auto oldAttributes = f.attributes;
      bool formChanged = false;
      f.attributes = tokens(obj[QStringLiteral("ca")]); f.attributesKnown = obj[QStringLiteral("ca")].isString();
      f.attributesPath = e.path;
      text = QStringLiteral("属性 %1 → %2").arg(attributes(oldAttributes), f.attributesKnown ? attributes(f.attributes) : QStringLiteral("待核实"));
      bool changed = oldAttributes != f.attributes || !f.attributesKnown;
      if (type == 1) {
        const auto oldRace = f.raceId, oldNormal = f.normalSkill, oldUnique = f.uniqueSkill;
        const auto oldFace = f.faceId; const auto oldMaxPower = f.maxPower;
        // getShowRaceIdWithOrigin returns the existing origin, otherwise the
        // raw current race (raceIdIgnoreTesting), before changing the form.
        if (f.originRaceId.toLongLong() <= 0 && oldRace.toLongLong() > 0) {
          f.originRaceId = oldRace; f.originFighterKey = key;
          f.originRelation = QStringLiteral("transform"); f.originPath = f.formPath;
        }
        const auto idValue = [&](const char* field) { const auto n = number(obj[QLatin1String(field)]); return n ? QString::number(*n) : QString(); };
        f.raceId = idValue("cr"); f.faceId = idValue("cf"); f.normalSkill = idValue("cn");
        const auto unique = number(obj[QStringLiteral("cu")].toString().section(QLatin1Char(','), 0, 0));
        // BattleNumUnitUtil writes uniqueSkill only. It does not replace
        // FighterInfo.superUniqueSkill; scu is the fallback for cu[0] == 0.
        f.uniqueSkill = unique && *unique == 0 ? idValue("scu") : unique ? QString::number(*unique) : QString();
        f.formPath = f.skillsPath = e.path; updateFormReference(f, catalog);
        formChanged = (!f.raceId.isEmpty() && f.raceId != oldRace) || (!f.faceId.isEmpty() && f.faceId != oldFace);
        changed |= oldRace != f.raceId || oldFace != f.faceId || oldNormal != f.normalSkill || oldUnique != f.uniqueSkill || oldMaxPower != f.maxPower;
        if (f.raceId.isEmpty() || f.faceId.isEmpty() || f.normalSkill.isEmpty() || f.uniqueSkill.isEmpty() || !f.maxPower)
          issue(e.path, QStringLiteral("变身字段或官方形态字典未齐全；缺项保持待核实"));
        text = QStringLiteral("形态 %1 → %2；外观 %3 → %4；%5；普通技能 %6 → %7；当前超杀 %8 → %9；满气势 %10 → %11")
          .arg(referenceName(catalog, QStringLiteral("pets"), oldRace), referenceName(catalog, QStringLiteral("pets"), f.raceId), oldFace, f.faceId, text,
            referenceName(catalog, QStringLiteral("skills"), oldNormal), referenceName(catalog, QStringLiteral("skills"), f.normalSkill),
            referenceName(catalog, QStringLiteral("skills"), oldUnique), referenceName(catalog, QStringLiteral("skills"), f.uniqueSkill), shown(oldMaxPower), shown(f.maxPower));
      }
      auto change = makeEffect(type == 1 ? QStringLiteral("transform") : QStringLiteral("attributes"), key, QStringLiteral("record"), {}, {}, e.path, obj);
      change.changed = changed; effect(change, text);
      if (formChanged) transition(QStringLiteral("变身"), key);
      if (f.attributesKnown && f.attributes != oldAttributes) transition(QStringLiteral("属性变化"), key);
    } else if (type == 100 || type == 101) {
      text = QStringLiteral("官方空操作记录 t=%1").arg(type);
    } else {
      issue(e.path, QStringLiteral("未实现特殊记录 t=%1").arg(type));
      text = QStringLiteral("特殊记录 t=%1 尚未解释，完整保留").arg(type);
    }
    detail(key, text, e.path, e.raw);
  }
  void finalNumbers(const Record& r) {
    if (!state.contains(r.key) || r.role == 2 || state.value(r.key).representer) return;
    auto& f = state[r.key];
    for (const auto& field : {QStringLiteral("ahp"), QStringLiteral("avg")}) {
      if (!r.num.contains(field)) continue;
      auto& value = field == QStringLiteral("ahp") ? f.hp : f.power;
      auto& path = field == QStringLiteral("ahp") ? f.hpPath : f.powerPath;
      const auto old = value; value = number(r.num[field]);
      if (field == QStringLiteral("ahp") && value) value = std::max<qint64>(0, *value);
      path = r.path + QLatin1Char('/') + field;
      if (field == QStringLiteral("ahp")) updatePresence(f);
      if (!value) issue(path, QStringLiteral("最终数值无效；不补零"));
      detail(r.key, QStringLiteral("%1最终校准 %2 → %3").arg(field == QStringLiteral("ahp") ? QStringLiteral("生命") : QStringLiteral("气势"), shown(old), shown(value)), path, r.num[field]);
      auto change = makeEffect(QStringLiteral("observation"), r.key, QStringLiteral("calibration"), {}, {}, path, r.num[field]);
      change.metric = field == QStringLiteral("ahp") ? QStringLiteral("生命") : QStringLiteral("气势");
      change.before = old; change.after = value; if (old && value) change.delta = *value - *old;
      change.changed = !old || !value || old != value;
      effect(change, QStringLiteral("%1最终校准 %2 → %3").arg(change.metric, shown(old), shown(value)));
    }
  }
  void grouped(bool before) {
    int count = 0;
    for (auto& r : records) {
      auto& ids = before ? r.beforeIds : r.afterIds;
      if (sequence) {
        const auto beforeMax = r.beforeIds.isEmpty() ? 0 : r.beforeIds.last();
        const auto afterMax = r.afterIds.isEmpty() ? 0 : r.afterIds.last();
        const auto limit = before ? beforeMax + 1 : std::max(beforeMax, afterMax);
        // SkillNumResult fills before inclusively, after exclusively.
        for (qint64 id = 0; id < limit; ++id) if (!ids.contains(id)) ids.append(id);
        std::sort(ids.begin(), ids.end());
      }
      count = std::max(count, static_cast<int>(ids.size()));
    }
    const auto phase = before ? QStringLiteral("before") : QStringLiteral("after");
    const auto phaseTitle = before ? QStringLiteral("技能前") : QStringLiteral("技能后");
    for (int index = 0; index < count; ++index) {
      bool wait = false, unknownWait = false, any = false;
      QStringList groups;
      for (const auto& r : records) {
        const auto events = eventsAt(r, before, index); any |= !events.isEmpty();
        if (!events.isEmpty()) groups.append(QStringLiteral("%1：组 %2").arg(objectName(r.key)).arg(events.first().group));
        for (const auto& e : events) if (e.operation == 1) {
          const auto cfg = catalog[QStringLiteral("buffs")].toObject()[e.buff].toObject();
          if (cfg.isEmpty()) unknownWait = true;
          else wait |= number(cfg[QStringLiteral("buffWaitType")]).value_or(0) > 0;
        }
      }
      if (sequence && !any) continue;
      const auto title = QStringLiteral("%1 · 第 %2 组").arg(phaseTitle).arg(index + 1);
      auto note = groups.join(QStringLiteral("；"));
      if (wait) note += QStringLiteral("；等待队列依据配置，模型透明度未记录；可见性条件另见 Buff 详情");
      QList<BuffPlaybackPlan> buffPlans;
      if (unknownWait) {
        issue(step.path, QStringLiteral("有 Buff 缺少官方等待配置；该组只提供组末快照，不能确定组内 Buff 时机"));
        begin(phase, QStringLiteral("group-end"), title + QStringLiteral(" · 组末"), note + QStringLiteral("；Buff 时机待核实"), index);
      } else if (wait) {
        int batchCount = 0;
        for (const auto& r : records) {
          buffPlans.append(planBuffPlayback(eventsAt(r, before, index)));
          batchCount = std::max(batchCount, static_cast<int>(buffPlans.last().batches.size()));
        }
        for (int batch = 0; batch < batchCount; ++batch) {
          const auto batchNote = note + QStringLiteral("；各对象局部批次 %1，互相并行；跨对象完成先后未记录").arg(batch + 1);
          begin(phase, QStringLiteral("wait-buff"), title + QStringLiteral(" · 等待型 Buff"), batchNote, index);
          for (qsizetype r = 0; r < records.size(); ++r) if (batch < buffPlans[r].batches.size())
            for (const auto& e : buffPlans[r].batches[batch]) buff(records[r], e, batch);
          finish();
        }
      }
      if (!unknownWait) begin(phase, QStringLiteral("values"), title + QStringLiteral(" · 生命 / 护盾"), note, index);
      for (const auto& r : records) {
        // Before/AfterAttackCommand skip target HP/SP when it is also source.
        if (r.role != 0 && r.key == step.actor) continue;
        for (const auto& suffix : {QStringLiteral("hps"), QStringLiteral("shps")})
          for (const auto& e : eventsAt(r, before, index)) if (e.field.endsWith(QLatin1Char('_') + suffix)) delta(r, e, suffix == QStringLiteral("hps") ? QStringLiteral("生命") : QStringLiteral("护盾"));
      }
      if (!unknownWait) { finish(); begin(phase, QStringLiteral("power-buff"), title + QStringLiteral(" · 气势 / Buff"), note, index); }
      // Source power+buff, all trl powers then trl buffs, then prl likewise.
      for (int role = 0; role < 3; ++role) {
        for (const auto& r : records) if (r.role == role)
          for (const auto& e : eventsAt(r, before, index)) if (e.field.endsWith(QStringLiteral("vgs"))) delta(r, e, QStringLiteral("气势"));
        if (!wait || unknownWait) for (const auto& r : records) if (r.role == role)
          for (const auto& e : eventsAt(r, before, index)) if (e.operation >= 0) buff(r, e);
      }
      finish();
      if (wait && !unknownWait) {
        bool hasTail = false; for (const auto& plan : buffPlans) hasTail |= !plan.callbackTail.isEmpty();
        if (hasTail) {
          begin(phase, QStringLiteral("wait-tail"), title + QStringLiteral(" · Buff 回调后续"),
            note + QStringLiteral("；尾项在该对象等待回调之后；与其他对象、数值动画完成的精确先后未记录"), index);
          for (qsizetype r = 0; r < records.size(); ++r) for (const auto& e : buffPlans[r].callbackTail)
            buff(records[r], e, buffPlans[r].callbackBatch, true);
          finish();
        }
      }
      // Official AfterAttackCommand calibrates trl then source, BEFORE objects.
      if (!before) {
        begin(phase, QStringLiteral("calibration"), title + QStringLiteral(" · 最终数值校准"), {}, index);
        for (int role : {1, 0}) for (const auto& r : records)
          if (r.role == role && index >= r.afterIds.size() - 1) finalNumbers(r);
        finish();
      }
      begin(phase, QStringLiteral("objects"), title + QStringLiteral(" · 特殊记录"), note, index);
      for (const auto& r : records) for (const auto& e : eventsAt(r, before, index)) if (e.field == QStringLiteral("rl")) special(r, e);
      finish();
    }
  }
  void attackRecord(const Record& r, const QString& prefix, bool source, const QString& actingKey) {
    for (const auto& pair : QList<QPair<QString, QString>>{{"hps", "生命"}, {"shps", "护盾"}, {"vgs", "气势"}}) {
      const auto field = prefix + QLatin1Char('_') + pair.first;
      const auto raw = r.num[field];
      if (raw.isUndefined() || raw.isNull() || raw == QJsonValue(QString())) continue;
      auto rCopy = r;
      if (!raw.isString()) { badEvent(rCopy, field, r.path + QLatin1Char('/') + field, raw); continue; }
      Event combined; combined.path = r.path + QLatin1Char('/') + field; combined.raw = raw; combined.delta = 0;
      for (const auto& token : tokens(raw)) {
        Event e; e.path = combined.path; e.raw = token; e.delta = number(token);
        if (!e.delta) { badEvent(rCopy, field, e.path, e.raw); combined.delta.reset(); continue; }
        if (source) delta(r, e, pair.second, prefix == QStringLiteral("sb") ? QStringLiteral("counter") : actionCause(), actingKey);
        else if (combined.delta) {
          const auto sum = *combined.delta + *e.delta;
          if (std::abs(static_cast<double>(sum)) > 9007199254740991.0) combined.delta.reset(); else combined.delta = sum;
        }
      }
      if (!source) delta(r, combined, pair.second, prefix == QStringLiteral("sb") ? QStringLiteral("counter") : actionCause(), actingKey);
    }
  }
  bool configure(const QJsonObject& action) {
    const bool team = isTeamAction(action);
    step.loggedSkillId = skillId(action[QStringLiteral("si")]);
    step.playbackSkillId.clear(); step.counterSkillId.clear();
    step.testingSkillId.clear(); step.lastSkillStatus = QStringLiteral("not-applicable");
    step.playbackKind = QStringLiteral("unresolved");
    const auto executionCode = team ? QStringLiteral("1") : valueText(action[QStringLiteral("ses")]);
    const auto execution = catalog[QStringLiteral("execution")].toObject()[executionCode].toObject();
    step.executionName = execution[QStringLiteral("nm")].toString();
    if (team) {
      if (!catalog.contains(QStringLiteral("bufferExecution"))) return false;
      const auto cfg = catalog[QStringLiteral("bufferExecution")].toObject()[valueText(action[QStringLiteral("epbi")])].toObject();
      // getPlayerBuffType(null) is PLAYER_BUFF_NORMAL, a simple command.
      effects = number(cfg[QStringLiteral("actionResId")]).value_or(0) > 0;
      step.playbackKind = effects ? QStringLiteral("team-skill") : QStringLiteral("team-simple");
      if (effects) {
        auto field = QStringLiteral("skillId"); bool confirmed = true;
        const auto dragonIds = action[QStringLiteral("eps")];
        const auto dragons = dragonIds.toArray();
        if (!dragonIds.isUndefined() && !dragonIds.isNull() && !dragonIds.isArray()) {
          confirmed = false;
          issue(step.path + QStringLiteral("/eps"), QStringLiteral("队伍行动的小神龙列表格式无效，播放技能保持待核实"));
        }
        if (!dragons.isEmpty()) {
          confirmed = false;
          const auto dragonId = number(dragons.first());
          for (const auto& entry : state.value(step.actor).raw[QStringLiteral("drs")].toArray()) {
            const auto dragon = entry.toObject();
            if (!dragonId || number(dragon[QStringLiteral("di")]) != dragonId) continue;
            const auto gene = dragon[QStringLiteral("ge")];
            if (!gene.isUndefined() && !gene.isNull() && !gene.isString()) break;
            // Official aqlib modules 426/775: absent gene becomes "";
            // PetDragonGene.isGlittery checks the first TWO characters == 'a'.
            if (gene.toString().startsWith(QStringLiteral("aa"))) field = QStringLiteral("lightedSkillId");
            confirmed = true; break;
          }
          if (!confirmed) issue(step.path + QStringLiteral("/eps"), QStringLiteral("队伍行动的小神龙或基因记录未确认，播放技能保持待核实"));
        }
        if (confirmed) step.playbackSkillId = skillId(cfg[field]);
        if (confirmed && step.playbackSkillId.isEmpty())
          issue(step.path + QStringLiteral("/epbi"), QStringLiteral("队伍执行配置未给出有效播放技能 ID"));
      }
      step.schedule = QStringLiteral("队伍行动：按各对象变化组的序号同步推进"); return true;
    }
    const auto source = state.value(step.actor);
    auto skill = testingSkill(step.loggedSkillId, source, catalog);
    if (skill != step.loggedSkillId) {
      step.testingSkillId = skill;
      playbackNote(QStringLiteral("官方字典标为内测：日志技能 %1 按种族 %2 → 9999 替换为 %3，原始 si 保留。")
        .arg(step.loggedSkillId, source.raceId, skill));
    }
    bool comboReplaced = false;
    for (const auto& v : catalog[QStringLiteral("combos")].toArray()) {
      const auto c = v.toObject(); if (skillId(c[QStringLiteral("skillId")]) != skill) continue;
      const auto buffs = tokens(c[QStringLiteral("buffs")]), targets = tokens(c[QStringLiteral("targetSkillIds")]);
      for (qsizetype i = buffs.size(); i-- > 0;) if (source.buffs.contains(buffs[i])) {
        const auto replacement = skillId(targets.value(i));
        if (!replacement.isEmpty()) { skill = replacement; comboReplaced = true; }
        break;
      }
      break;
    }
    auto cfg = catalog[QStringLiteral("skills")].toObject()[skill].toObject();
    if (cfg.isEmpty() || (!cfg.contains(QStringLiteral("action")) && !cfg.contains(QStringLiteral("playOrderType"))) || execution.isEmpty()) return false;
    // BattleFighter obtains the combo definition with its returned ID, then
    // stores that ID on RoundResult; later getter calls apply testing again.
    step.playbackSkillId = comboReplaced ? testingSkill(skill, source, catalog) : skill;
    if (step.playbackSkillId != skill) {
      step.testingSkillId = step.playbackSkillId;
      playbackNote(QStringLiteral("组合技能写回后，官方内测 getter 将 %1 替换为 %2。")
        .arg(skill, step.playbackSkillId));
    }
    // Official fillWithMaxChangeSetId precedes the last-effect substitution.
    sequence = number(cfg[QStringLiteral("playOrderType")]).value_or(0) == 1;
    const bool disabled = number(execution[QStringLiteral("type")]) == 2;
    if (!disabled && step.isLastRound && number(cfg[QStringLiteral("last")]) == 1) {
      const auto last = catalog[QStringLiteral("skillLastEffects")].toObject()[step.playbackSkillId].toObject();
      if (last.isEmpty() || (!last.contains(QStringLiteral("action")) && !last.contains(QStringLiteral("playOrderType")))) {
        step.lastSkillStatus = QStringLiteral("missing");
        playbackNote(QStringLiteral("本段末次行动无反击，技能 %1 要求末击专用配置，但当前官方资料未提供；保留最终数值与原始记录，动作内部顺序待核实。")
          .arg(step.playbackSkillId));
        return false;
      }
      cfg = last; step.lastSkillStatus = QStringLiteral("applied");
      playbackNote(QStringLiteral("本段末次行动无反击，已使用技能 %1 的官方末击配置；补齐变化组仍依据普通配置。")
        .arg(step.playbackSkillId));
    }
    close = cfg[QStringLiteral("attackType")].toString() == QStringLiteral("近身");
    const auto baseType = cfg[QStringLiteral("skillBaseType")].toString();
    const auto type2 = QStringList{QStringLiteral("召唤技"), QStringLiteral("超杀技"), QStringLiteral("超必杀技")}.contains(baseType) ? baseType : cfg[QStringLiteral("skillType")].toString();
    summon = !disabled && !close && type2 == QStringLiteral("召唤技");
    effects = !disabled && !summon;
    step.playbackKind = disabled ? QStringLiteral("disabled") : summon ? QStringLiteral("summon") : QStringLiteral("normal");
    step.schedule = sequence ? QStringLiteral("顺序型技能：按官方规则补齐空组后同步推进") : QStringLiteral("普通技能：按各对象变化组的序号同步推进，组号不跨对象排序");
    if (comboReplaced) step.schedule += QStringLiteral("；当前 Buff 对应播放技能 %1").arg(step.playbackSkillId);
    if (!step.testingSkillId.isEmpty()) step.schedule += QStringLiteral("；官方内测技能替换 %1").arg(step.testingSkillId);
    return true;
  }
  void invalidateUnresolved() {
    for (const auto& r : records) if (r.unresolvedSpecial && state.contains(r.key)) {
      auto& f = state[r.key]; f.hp.reset(); f.power.reset(); f.shield.reset(); f.buffsKnown = false;
      f.raceId.clear(); f.faceId.clear(); f.maxPower.reset(); f.formName.clear();
      f.normalSkill.clear(); f.uniqueSkill.clear(); f.attributesKnown = false; f.entryKnown = false; updatePresence(f);
      f.hpPath = f.powerPath = f.shieldPath = f.buffPath = r.path + QStringLiteral("/rl");
      detail(r.key, QStringLiteral("存在无法定位或解释的特殊记录；保留已知变化，但行动末状态标为待核实"), r.path + QStringLiteral("/rl"), r.num[QStringLiteral("rl")]);
    }
  }
  void run() {
    const auto action = step.raw.toObject();
    const bool representer = isTeamAction(action);
    if (!state.contains(step.actor)) {
      unlinked(action[QStringLiteral("cti")].toInt(-1), valueText(action[QStringLiteral("cpn")]),
        representer ? QStringLiteral("@team") : valueText(action[QStringLiteral("cci")]),
        QStringLiteral("actor"), step.path, action, action[QStringLiteral("cn")].toObject());
      unresolvedReason = QStringLiteral("行动者未关联到初始阵容，不能确认其动作执行；保留已关联对象的最终记录");
    }
    bool configured = configure(action);
    begin(QStringLiteral("setup"), QStringLiteral("initial"), QStringLiteral("行动开始")); targetRelations(action); finish(true);
    begin(QStringLiteral("setup"), QStringLiteral("checks"), QStringLiteral("记录检查"));
    // RoundResult creates an empty source SkillNumResult for team actions.
    if (!representer && action[QStringLiteral("cn")].isObject()) addRecord(step.actor, step.path + QStringLiteral("/cn"), action[QStringLiteral("cn")].toObject(), 0);
    for (const auto& list : {QStringLiteral("trl"), QStringLiteral("prl")}) {
      const auto targets = action[list].toArray();
      for (qsizetype i = 0; i < targets.size(); ++i) {
        const auto t = targets[i].toObject(); if (!t[QStringLiteral("tn")].isObject()) continue;
        const auto key = fighterKey(t[QStringLiteral("tti")].toInt(-1), valueText(t[QStringLiteral("tpn")]),
          targetFighterId(t[QStringLiteral("tci")]));
        addRecord(key, step.path + QStringLiteral("/%1/%2/tn").arg(list).arg(i), t[QStringLiteral("tn")].toObject(), list == QStringLiteral("trl") ? 1 : 2, t);
      }
    }
    finish();
    if (configured && effects && close) {
      int effectiveTargets = 0; bool unlinkedEffective = false;
      for (const auto& target : action[QStringLiteral("trl")].toArray()) {
        const auto t = target.toObject();
        if (!t[QStringLiteral("isse")].toBool()) continue;
        ++effectiveTargets;
        unlinkedEffective |= !state.contains(fighterKey(t[QStringLiteral("tti")].toInt(-1),
          valueText(t[QStringLiteral("tpn")]), targetFighterId(t[QStringLiteral("tci")])));
      }
      if (effectiveTargets > 1) {
        const auto reason = QStringLiteral("近身技能记录了 %1 个生效目标，官方 ApproachCommand 不接受多生效目标；不推定逐目标攻击顺序").arg(effectiveTargets);
        issue(step.path + QStringLiteral("/trl"), reason); playbackNote(reason);
        if (unresolvedReason.isEmpty()) unresolvedReason = reason;
      } else if (unlinkedEffective) {
        const auto reason = QStringLiteral("近身技能的生效目标未关联，官方 ApproachCommand 无法定位该目标；不假定攻击已执行");
        issue(step.path + QStringLiteral("/trl"), reason); playbackNote(reason);
        if (unresolvedReason.isEmpty()) unresolvedReason = reason;
      }
    }
    if (!unresolvedReason.isEmpty()) { configured = false; step.playbackKind = QStringLiteral("unresolved"); }
    if (!configured) {
      step.schedule = !unresolvedReason.isEmpty() ? unresolvedReason : step.lastSkillStatus == QStringLiteral("missing")
        ? QStringLiteral("末击专用配置未收录，动作内部顺序待核实；保留最终记录观察")
        : QStringLiteral("缺少官方技能或执行配置，动作内部顺序待核实");
      issue(step.path, step.schedule);
      begin(QStringLiteral("unresolved"), QStringLiteral("final"), QStringLiteral("最终记录观察"), step.schedule);
      for (auto& r : records) if (!r.num[QStringLiteral("rl")].toArray().isEmpty()) r.unresolvedSpecial = true;
      for (int role : {1, 0, 2}) for (const auto& r : records) if (r.role == role) {
        if (!state.contains(r.key)) continue;
        state[r.key].lastNumbers = r.num;
        finalNumbers(r);
        for (auto it = r.num.begin(); it != r.num.end(); ++it) if (!it.value().toString().isEmpty()) {
          if (it.key().endsWith(QStringLiteral("shps"))) { state[r.key].shield.reset(); state[r.key].shieldPath = r.path + QLatin1Char('/') + it.key(); }
          else if (!unresolvedReason.isEmpty() && !state[r.key].representer &&
                   it.key().endsWith(QStringLiteral("hps")) && !r.num.contains(QStringLiteral("ahp"))) {
            state[r.key].hp.reset(); state[r.key].hpPath = r.path + QLatin1Char('/') + it.key(); updatePresence(state[r.key]);
          } else if (!unresolvedReason.isEmpty() && !state[r.key].representer &&
                     it.key().endsWith(QStringLiteral("vgs")) && !r.num.contains(QStringLiteral("avg"))) {
            state[r.key].power.reset(); state[r.key].powerPath = r.path + QLatin1Char('/') + it.key();
          }
          if (it.key() == QStringLiteral("bd_bfs") || it.key() == QStringLiteral("ad_bfs")) state[r.key].buffsKnown = false;
        }
      }
      invalidateUnresolved(); finish(true); return;
    }
    grouped(true);
    QList<int> effective;
    for (int i = 0; i < records.size(); ++i) if (records[i].role == 1 && records[i].target[QStringLiteral("isse")].toBool()) effective.append(i);
    if (summon && !effective.isEmpty()) {
      begin(QStringLiteral("attack"), QStringLiteral("result"), QStringLiteral("召唤登场"), QStringLiteral("按官方召唤技能阶段展示，不推测目标之间的动画帧顺序。"));
      for (const auto i : effective) appear(records[i].key, records[i].path.chopped(3), QStringLiteral("召唤技能生效"), records[i].target, actionCause(), step.actor);
      finish();
    }
    if (effects && !effective.isEmpty()) {
      begin(QStringLiteral("attack"), QStringLiteral("result"), QStringLiteral("技能结算"), QStringLiteral("同阶段的目标合并展示；日志数值不等于动画命中段数，不补造帧间顺序。"));
      for (const auto& r : records) if (r.role == 0) attackRecord(r, QStringLiteral("att"), true, step.actor);
      for (const auto i : effective) attackRecord(records[i], QStringLiteral("att"), false, step.actor);
      // handleHpAndPowerChanges still runs client hints for an effective
      // target whose tn has no numeric fields. Keep its actual attack boundary.
      finish(true);
      if (close && effective.size() == 1 && records[effective.first()].target[QStringLiteral("issb")].toBool()) {
        const auto counterSource = records[effective.first()].key;
        // CounterCommand passes the current normal skill AFTER before-effects;
        // counterAttack does not run the normal action's combo substitution.
        const auto counterFighter = state.value(counterSource);
        step.counterSkillId = isTestingForm(counterFighter, catalog) ? QStringLiteral("109999") : skillId(counterFighter.normalSkill);
        begin(QStringLiteral("counter"), QStringLiteral("result"), QStringLiteral("反击结算"), {}, -1, counterSource);
        attackRecord(records[effective.first()], QStringLiteral("sb"), true, counterSource);
        for (const auto& r : records) if (r.role == 0) attackRecord(r, QStringLiteral("sb"), false, counterSource);
        finish(true);
      }
    }
    grouped(false);
    begin(QStringLiteral("settle"), QStringLiteral("final"), QStringLiteral("行动结束"), QStringLiteral("护盾由初始值及已执行变化累计；ashp 保留原文，不覆盖余额。"));
    for (const auto& r : records) {
      if (!state.contains(r.key)) continue;
      state[r.key].lastNumbers = r.num;
      for (const auto& field : {QStringLiteral("ba_bfs"), QStringLiteral("bc_bfs"), QStringLiteral("aa_bfs"), QStringLiteral("ac_bfs")}) {
        if (r.num[field].toString().isEmpty()) continue;
        issue(r.path + QLatin1Char('/') + field, QStringLiteral("旧 Buff 增删字段在当前官方命令中无调用；仅保留，不与显示变更重复计算"));
        detail(r.key, QStringLiteral("原始 Buff 记录（未重复应用）：%1").arg(valueText(r.num[field])), r.path + QLatin1Char('/') + field, r.num[field]);
      }
    }
    invalidateUnresolved(); finish(true);
  }
};
}
void advanceAction(Step& step, QMap<QString, Fighter>& state, const QJsonObject& catalog, QStringList& issues) {
  Engine engine{step, state, catalog, issues}; engine.run();
}
}
