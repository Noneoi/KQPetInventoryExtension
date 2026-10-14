#include "domain/battle_replay.h"
#include "domain/battle_replay_flow.h"
#include "domain/battle_replay_state.h"
#include "domain/public_data_view.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace BattleReplay {
QList<BuffMember> recordedBuffMembers(const Fighter& fighter) {
  bool aligned = fighter.buffMembers.size() == fighter.buffs.size();
  for (qsizetype i = 0; aligned && i < fighter.buffs.size(); ++i)
    aligned = fighter.buffMembers[i].id == fighter.buffs[i];
  if (aligned) return fighter.buffMembers;
  QList<BuffMember> unknown;
  for (const auto& id : fighter.buffs) unknown.append({id, {}, {}, fighter.buffPath, -1});
  return unknown;
}

QString valueText(const QJsonValue& value) {
  if (value.isString()) return value.toString();
  if (value.isUndefined()) return QStringLiteral("未记录");
  if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}
QString fighterKey(int team, const QString& player, const QString& id) {
  return QString::number(team) + QLatin1Char(':') + QString::number(player.size()) + QLatin1Char(':') + player + QLatin1Char(':') + id;
}
QString targetFighterId(const QJsonValue& id) {
  if (id.isUndefined() || id.isNull() ||
      (id.isDouble() && id.toDouble() == 2147483647.0) ||
      (id.isString() && id.toString() == QStringLiteral("2147483647"))) return QStringLiteral("@team");
  return valueText(id);
}
QString petName(const QJsonObject& catalog, const QString& raceId) {
  const auto pet = PublicDataView(PublicDataViewKind::BattleReplay, catalog).object(QStringLiteral("pets"), raceId);
  const auto identity = pet[QStringLiteral("identity")].toObject();
  // A legacy name field may itself be a skin name. Only the verified identity
  // projection supplies an original name, even when a display name is present.
  const auto status = identity[QStringLiteral("status")].toString();
  const auto name = status == QStringLiteral("native") || status == QStringLiteral("skin")
    ? identity[QStringLiteral("canonicalName")].toString() : QString();
  return name.isEmpty() ? QStringLiteral("原名待核实 [%1]").arg(raceId) : name;
}
QString fighterName(const Fighter& f) {
  return f.formName.isEmpty() ? f.name : f.formName;
}
QString referenceName(const QJsonObject& catalog, const QString& section, const QString& id) {
  if (section == QStringLiteral("pets")) return petName(catalog, id) + QStringLiteral("（种族 %1）").arg(id);
  const auto entry = PublicDataView(PublicDataViewKind::BattleReplay, catalog).object(section, id);
  auto name = entry[section == QStringLiteral("skills") || section == QStringLiteral("pets") ? QStringLiteral("name") : QStringLiteral("nm")].toString();
  if (name.isEmpty()) name = (entry.isEmpty() ? QStringLiteral("未收录") : QStringLiteral("无名称")) +
    (section == QStringLiteral("skills") ? QStringLiteral("技能") : section == QStringLiteral("pets") ? QStringLiteral("形态") : section == QStringLiteral("attributes") ? QStringLiteral("属性") : QStringLiteral(" Buff"));
  return name + QStringLiteral(" [%1]").arg(id);
}
void updatePresence(Fighter& f) {
  if (f.playerRemoved) f.presence = QStringLiteral("玩家已移除");
  else if (f.representer) f.presence = QStringLiteral("队伍效果");
  else if (!f.entryKnown && !f.displayLayersRestored) f.presence = QStringLiteral("登场状态待核实");
  else if (!presentedOnField(f)) f.presence = f.clone ? QStringLiteral("待分身登场") : f.summoned ? QStringLiteral("待召唤") : QStringLiteral("未登场");
  else if (!f.hp) f.presence = QStringLiteral("生命状态待核实");
  else if (*f.hp <= 0) f.presence = QStringLiteral("已退场（生命为 0）");
  else f.presence = f.clone ? QStringLiteral("分身在场") : f.summoned ? QStringLiteral("召唤单位在场") : QStringLiteral("在场");
  if (f.chaser) f.presence += QStringLiteral(" · 追击位");
}
void updateFormReference(Fighter& f, const QJsonObject& catalog) {
  const auto pet = catalog[QStringLiteral("pets")].toObject()[f.raceId].toObject();
  f.formName = f.raceId.isEmpty() ? QString() : petName(catalog, f.raceId);
  f.maxPower.reset();
  const auto power = pet[QStringLiteral("maxPower")];
  if (power.isDouble() && power.toInt(-1) > 0) f.maxPower = power.toInt();
}
namespace {
QString str(const QJsonObject& object, const char* key) { return valueText(object[QLatin1String(key)]); }
QString child(const QString& base, QString key) {
  key.replace(QLatin1Char('~'), QStringLiteral("~0")).replace(QLatin1Char('/'), QStringLiteral("~1"));
  return base + QLatin1Char('/') + key;
}
std::optional<qint64> integer(const QJsonValue& value) {
  if (value.isDouble()) {
    const double n = value.toDouble();
    if (std::isfinite(n) && std::abs(n) <= 9007199254740991.0 && std::floor(n) == n) return static_cast<qint64>(n);
  } else if (value.isString()) {
    bool ok = false;
    const auto number = value.toString().toLongLong(&ok);
    if (ok) return number;
  }
  return {};
}
QString buffId(const QJsonValue& value) {
  const auto id = integer(value);
  // The official Buff parsers use int IDs. Normalize valid recorded integers
  // across initial membership, display changes and progress observations;
  // malformed/out-of-range evidence never becomes a fabricated zero ID.
  return id && *id >= std::numeric_limits<int>::min() && *id <= std::numeric_limits<int>::max()
    ? QString::number(*id) : QString();
}
QString shown(std::optional<qint64> n) { return n ? QString::number(*n) : QStringLiteral("未确认"); }
bool isTeamAction(const QJsonObject& action) {
  const auto type = action[QStringLiteral("orrt")];
  return !type.isUndefined() && !type.isNull() && type.toInt() != 1;
}
// Scoped labels: identical short keys have different meanings in different objects.
QString fieldLabel(const QString& path) {
  static const QRegularExpression indices(QStringLiteral("/[0-9]+(?=/|$)"));
  static const QRegularExpression numbersPath(QStringLiteral("/(cn|tn)/"));
  QString p = path;
  if (p.startsWith(QStringLiteral("/br/"))) p.remove(0, 3);
  p.replace(indices, QStringLiteral("/*"));
  if (p.startsWith(QStringLiteral("/brs/*/"))) p.remove(0, 6);
  const auto key = p.section(QLatin1Char('/'), -1);
  const bool fighter = p.startsWith(QStringLiteral("/or/team/*/players/*/cts/*/"));
  const bool numbers = p.contains(numbersPath);
  const bool unit = numbers && p.contains(QStringLiteral("/rl/*/"));
  static const QMap<QString, QString> fighters{
    {"id", "战斗单位 ID"}, {"name", "原始名称"}, {"customName", "自定义名称"},
    {"hp", "初始生命"}, {"maxHp", "初始生命上限"}, {"shp", "初始护盾"}, {"vigour", "初始气势"},
    {"x", "阵位上下（0 上 / 1 中 / 2 下）"}, {"y", "阵位纵深（0 前排 / 1 中排 / 2 后排）"}, {"raceId", "种族 ID"}, {"faceId", "外观 ID"},
    {"oRaceId", "原种族 ID"}, {"oFaceId", "原外观 ID"}, {"lvl", "等级"}, {"bfs", "初始 Buff 列表"},
    {"attr", "属性 ID 列表"}, {"nskill", "普通技能 ID"}, {"uskills", "超杀技能 ID"}, {"suskill", "额外超杀技能 ID"},
    {"ics", "召唤单位标记"}, {"is", "召唤登场标记"}, {"ias", "分身标记"}, {"ibs", "分身登场标记"}, {"icr", "追击单位标记"}};
  if (fighter) return fighters.value(key);
  if (p.startsWith(QStringLiteral("/or/team/*/players/*/"))) {
    static const QMap<QString, QString> labels{{"name", "玩家名称"}, {"x", "整套阵容的上下位置"}, {"y", "整套阵容的前后位置"},
      {"cts", "玩家精灵记录"}, {"fm", "阵型 ID"}, {"fl", "阵型等级"}, {"fp", "战斗力"}, {"sp", "速度"}};
    if (labels.contains(key)) return labels.value(key);
  }
  if (p.startsWith(QStringLiteral("/fr/*/pr/"))) {
    static const QMap<QString, QString> labels{{"player", "被移动或移除的玩家"}, {"teamId", "玩家所属队伍"},
      {"x_to", "整套阵容移到的上下位置"}, {"y_to", "整套阵容移到的前后位置"}, {"ml", "整套阵容移位列表"}, {"dl", "玩家移除列表"}};
    return labels.value(key);
  }
  if (unit) {
    static const QMap<QString, QString> labels{{"t", "特殊记录类型"}, {"o", "时机：1 技能前 / 2 技能后"},
      {"changeset", "对象内的变化组号"}, {"ci", "关联单位 ID"}, {"ti", "关联队伍 ID"}, {"pn", "关联玩家"},
      {"pbid", "队伍进度 Buff ID"}, {"prob", "进度原值"}, {"proa", "进度新值"}, {"vc", "进度变化值"}, {"mp", "进度上限"},
      {"isse", "技能生效标记"}, {"new_curv", "新生命值"}, {"old_curv", "原生命值"}, {"new_maxv", "新生命上限"}, {"old_maxv", "原生命上限"},
      {"cr", "变化后种族 ID"}, {"cf", "变化后外观 ID"}, {"cn", "变化后普通技能"}, {"cu", "变化后超杀技能列表"}, {"ca", "变化后属性"}, {"scu", "变化后额外超杀技能"}};
    return labels.value(key);
  }
  if (numbers) {
    if (key == QStringLiteral("ahp")) return QStringLiteral("记录中的最终生命");
    if (key == QStringLiteral("avg")) return QStringLiteral("记录中的最终气势");
    if (key == QStringLiteral("ashp")) return QStringLiteral("finalSp 原值（不能直接当作护盾余额）");
    static const QMap<QString, QString> phases{{"bb", "技能前 Buff"}, {"ab", "技能后 Buff"}, {"att", "技能"}, {"sb", "反击"}};
    static const QMap<QString, QString> metrics{{"hps", "生命变化"}, {"vgs", "气势变化"}, {"shps", "护盾变化"}};
    const auto parts = key.split(QLatin1Char('_'));
    if (parts.size() == 2 && phases.contains(parts[0]) && metrics.contains(parts[1])) return phases[parts[0]] + metrics[parts[1]];
    static const QMap<QString, QString> buffs{{"bd_bfs", "技能前 Buff 显示变更"}, {"ad_bfs", "技能后 Buff 显示变更"},
      {"ba_bfs", "技能前新增 Buff 记录"}, {"aa_bfs", "技能后新增 Buff 记录"}, {"bc_bfs", "技能前删除 Buff 记录"}, {"ac_bfs", "技能后删除 Buff 记录"}};
    return buffs.value(key);
  }
  if (p.startsWith(QStringLiteral("/fr/*/rl/*/"))) {
    if (p.endsWith(QStringLiteral("/seqa/*"))) return QStringLiteral("队伍 0 行动队列单位 ID");
    if (p.endsWith(QStringLiteral("/seqb/*"))) return QStringLiteral("队伍 1 行动队列单位 ID");
    static const QMap<QString, QString> labels{{"cti", "行动队伍 ID"}, {"cpn", "行动玩家"}, {"cci", "行动单位 ID"},
      {"csn", "战斗回合"}, {"pr", "玩家回合"}, {"si", "技能 ID"}, {"ses", "执行结果代码"},
      {"orrt", "行动类型：1 或空为普通 / 其他为队伍行动"}, {"epbi", "执行 Buff ID"}, {"eps", "执行龙 ID 列表"},
      {"seqa", "队伍 0 行动队列"}, {"seqb", "队伍 1 行动队列"},
      {"isor", "超出回合限制标记"}, {"fbs", "普通行动队列标记"}, {"tti", "目标队伍 ID"}, {"tpn", "目标玩家"},
      {"tci", "目标单位 ID"}, {"ishit", "命中标记"}, {"isbl", "格挡标记"}, {"iscr", "暴击标记"},
      {"issb", "反击标记"}, {"isse", "技能生效标记"}, {"isIm", "免疫标记"}};
    return labels.value(key);
  }
  if (p == QStringLiteral("/win")) return QStringLiteral("胜方队伍 ID；-1 为未分胜负");
  if (p == QStringLiteral("/bi")) return QStringLiteral("战斗 ID");
  if (p == QStringLiteral("/ver")) return QStringLiteral("战斗数据版本");
  if (p == QStringLiteral("/pve")) return QStringLiteral("官方 replay 字段（不能用来判断 PVE / PVP）");
  if (p == QStringLiteral("/abs")) return QStringLiteral("所需 Buff ID 列表");
  return {};
}
void flatten(const QJsonValue& value, const QString& path, const QString& schemaPath,
             QList<Field>& fields, QMap<QString, QString>& labels) {
  // Include containers too: empty lists/objects and future fields are never lost.
  if (!labels.contains(schemaPath)) labels.insert(schemaPath, fieldLabel(schemaPath));
  const auto label = labels.value(schemaPath);
  fields.append({path, label.isEmpty() ? QStringLiteral("含义待核实") : label, value, !label.isEmpty()});
  if (value.isObject()) {
    const auto obj = value.toObject();
    for (auto it = obj.begin(); it != obj.end(); ++it) flatten(it.value(), child(path, it.key()), child(schemaPath, it.key()), fields, labels);
  } else if (value.isArray()) {
    const auto array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) flatten(array[i], child(path, QString::number(i)), child(schemaPath, QStringLiteral("*")), fields, labels);
  }
}
struct Builder {
  Replay out;
  const QJsonObject& catalog;
  QMap<QString, Fighter> state;
  void issue(const QString& text) { if (!out.issues.contains(text)) out.issues.append(text); }
  QStringList initialBuffIds(const QJsonValue& raw, const QString& path, bool& known) {
    QStringList ids;
    if (raw.isUndefined() || raw.isNull() || raw == QJsonValue(QString())) return ids;
    if (!raw.isString()) {
      known = false; issue(path + QStringLiteral("：初始 Buff 列表格式无法确认；原值保留，不当作空集合"));
      return ids;
    }
    for (const auto& token : raw.toString().split(QLatin1Char(','), Qt::KeepEmptyParts)) {
      const auto id = buffId(token);
      if (id.isEmpty()) {
        known = false; issue(path + QStringLiteral("：存在无法确认的 Buff 编号；原值保留，不补零"));
      } else ids.append(id);
    }
    return ids;
  }
  QString name(const QString& key) const { return state.contains(key) ? fighterName(state[key]) : key; }
  QString resolve(Step& step, int team, const QString& player, const QString& id, const QString& path,
                  const QString& kind, const QJsonValue& raw, const QJsonObject& numbers = {}) {
    auto key = fighterKey(team, player, id);
    if (!state.contains(key)) {
      UnlinkedReference reference;
      reference.kind = kind; reference.key = key; reference.team = team;
      reference.player = player; reference.id = id; reference.path = path;
      reference.raw = raw; reference.numbers = numbers;
      retainUnlinkedReference(step, reference, out.issues);
    }
    return key;
  }
  void detail(Step& step, const QString& phase, const QString& key, const QString& text, const QString& path, const QJsonValue& raw) {
    step.details.append({phase, name(key), text, path, raw});
    if (!key.isEmpty() && !step.affected.contains(key)) step.affected.append(key);
  }
  void numbers(Step& step, const QString& key, const QJsonObject& num, const QString& path) {
    // Raw values remain inspectable independently of the command scheduler.
    // Resolve identities here; ONLY advanceAction mutates numeric/Buff state.
    for (auto it = num.begin(); it != num.end(); ++it) {
      if (it.key() == QStringLiteral("rl")) continue;
      const auto label = fieldLabel(child(path, it.key()));
      detail(step, QStringLiteral("记录值"), key,
        (label.isEmpty() ? QStringLiteral("未解释字段 %1").arg(it.key()) : label) + QStringLiteral("：") + valueText(it.value()),
        child(path, it.key()), it.value());
    }
    const auto units = num[QStringLiteral("rl")].toArray();
    for (qsizetype i = 0; i < units.size(); ++i) {
      const auto obj = units[i].toObject(); const auto type = obj[QStringLiteral("t")].toInt(-1);
      const auto p = path + QStringLiteral("/rl/%1").arg(i);
      if (type == 3) resolve(step, obj[QStringLiteral("ti")].toInt(-1), str(obj, "pn"), str(obj, "ci"), p,
        QStringLiteral("record"), obj, num);
      else if (type == 5 && state.contains(key)) { const auto f = state.value(key);
        resolve(step, f.team, f.player, str(obj, "ci"), p, QStringLiteral("record"), obj, num); }
      else if (type == 4 && state.contains(key)) { const auto f = state.value(key);
        resolve(step, f.team, f.player, QStringLiteral("@team"), p, QStringLiteral("record"), obj, num); }
    }
  }

};

// Compare battle observations separately from the raw-record cache. Reading a
// target alone is not a write; conversely a zero-delta calibration still is an
// absolute write and is added to the write set by its Effect below.
bool sameObservation(const Fighter& a, const Fighter& b) {
  if (std::tie(a.hp, a.maxHp, a.power, a.maxPower, a.shield, a.raceId, a.faceId, a.formName,
        a.normalSkill, a.uniqueSkill, a.superUniqueSkill, a.attributes, a.attributesKnown,
        a.summoned, a.clone, a.cloneCalled, a.entered, a.entryKnown, a.displayLayersRestored, a.chaser, a.playerRemoved,
        a.x, a.y, a.playerX, a.playerY, a.buffs, a.buffsKnown, a.originRaceId, a.originFighterKey, a.originRelation) !=
      std::tie(b.hp, b.maxHp, b.power, b.maxPower, b.shield, b.raceId, b.faceId, b.formName,
        b.normalSkill, b.uniqueSkill, b.superUniqueSkill, b.attributes, b.attributesKnown,
        b.summoned, b.clone, b.cloneCalled, b.entered, b.entryKnown, b.displayLayersRestored, b.chaser, b.playerRemoved,
        b.x, b.y, b.playerX, b.playerY, b.buffs, b.buffsKnown, b.originRaceId, b.originFighterKey, b.originRelation)) return false;
  if (a.buffMembers.size() != b.buffMembers.size() || a.teamProgress.keys() != b.teamProgress.keys()) return false;
  for (qsizetype i = 0; i < a.buffMembers.size(); ++i) {
    const auto& x = a.buffMembers[i]; const auto& y = b.buffMembers[i];
    if (std::tie(x.id, x.source, x.sourceName, x.sourceTeam) != std::tie(y.id, y.source, y.sourceName, y.sourceTeam)) return false;
  }
  for (auto it = a.teamProgress.begin(); it != a.teamProgress.end(); ++it) {
    const auto& x = it.value(); const auto& y = b.teamProgress[it.key()];
    if (std::tie(x.current, x.maximum, x.previous, x.delta, x.skillEffected) !=
        std::tie(y.current, y.maximum, y.previous, y.delta, y.skillEffected)) return false;
  }
  return true;
}
bool sameEvidence(const Fighter& a, const Fighter& b) {
  if (!sameObservation(a, b) || std::tie(a.hpPath, a.maxHpPath, a.powerPath, a.shieldPath, a.buffPath,
      a.formPath, a.attributesPath, a.skillsPath, a.presencePath, a.displayLayersPath, a.lastNumbers, a.lastUnit, a.originPath) !=
      std::tie(b.hpPath, b.maxHpPath, b.powerPath, b.shieldPath, b.buffPath,
      b.formPath, b.attributesPath, b.skillsPath, b.presencePath, b.displayLayersPath, b.lastNumbers, b.lastUnit, b.originPath)) return false;
  for (qsizetype i = 0; i < a.buffMembers.size(); ++i)
    if (a.buffMembers[i].path != b.buffMembers[i].path) return false;
  for (auto it = a.teamProgress.begin(); it != a.teamProgress.end(); ++it) {
    const auto& other = b.teamProgress[it.key()];
    if (it->path != other.path || it->raw != other.raw) return false;
  }
  return true;
}
void invalidateParallelObservation(Fighter& f, const QString& path) {
  f.hp.reset(); f.maxHp.reset(); f.power.reset(); f.maxPower.reset(); f.shield.reset();
  f.buffs.clear(); f.buffMembers.clear(); f.buffsKnown = false;
  f.raceId.clear(); f.faceId.clear(); f.formName.clear();
  f.normalSkill.clear(); f.uniqueSkill.clear(); f.superUniqueSkill.clear();
  f.attributes.clear(); f.attributesKnown = false; f.entryKnown = false;
  f.displayLayersRestored = false; f.displayLayersPath = path;
  f.hpPath = f.maxHpPath = f.powerPath = f.shieldPath = f.buffPath = path;
  f.formPath = f.attributesPath = f.skillsPath = f.presencePath = path;
  f.lastNumbers = {}; f.lastUnit = {};
  for (auto& progress : f.teamProgress) {
    progress.current.reset(); progress.maximum.reset(); progress.previous.reset(); progress.delta.reset();
    progress.skillEffected.reset(); progress.path = path; progress.raw = {};
  }
  updatePresence(f);
}
} // namespace

Replay parse(const QByteArray& bytes, const QJsonObject& officialCatalog) {
  Builder b{{}, officialCatalog, {}};
  b.out.original = bytes;
  b.out.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  QJsonParseError error;
  const auto doc = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    b.out.error = QStringLiteral("无法读取 JSON：%1（字节 %2）").arg(error.errorString()).arg(error.offset); return b.out;
  }
  b.out.root = doc.object();
  QMap<QString, QString> fieldLabels;
  flatten(b.out.root, {}, {}, b.out.fields, fieldLabels);
  const bool wrapped = b.out.root.contains(QStringLiteral("br"));
  auto battle = wrapped ? b.out.root[QStringLiteral("br")].toObject() : b.out.root;
  QString base = wrapped ? QStringLiteral("/br") : QString();
  const auto teamBattle = battle[QStringLiteral("tbt")];
  if (!teamBattle.isUndefined() && !teamBattle.isNull()) {
    b.out.containerKind = QStringLiteral("team");
    if (!battle[QStringLiteral("brs")].isArray()) {
      b.out.error = QStringLiteral("组队战报容器 brs 必须是数组"); return b.out;
    }
    const auto members = battle[QStringLiteral("brs")].toArray();
    b.out.containerBattleCount = static_cast<int>(members.size());
    // BattleBaseTeam.setMyBattle uses the last member with battleInfo (or),
    // rather than treating every brs member as a consecutive battle.
    for (qsizetype i = 0; i < members.size(); ++i)
      if (members[i].toObject()[QStringLiteral("or")].isObject()) b.out.selectedBattleIndex = static_cast<int>(i);
    if (b.out.selectedBattleIndex < 0) {
      b.out.error = QStringLiteral("组队战报没有包含初始阵容 or 的可播放成员"); return b.out;
    }
    base += QStringLiteral("/brs/%1").arg(b.out.selectedBattleIndex);
    battle = members[b.out.selectedBattleIndex].toObject();
  }
  b.out.battlePath = base;
  if (!battle[QStringLiteral("or")].isObject() || !battle[QStringLiteral("fr")].isArray()) {
    b.out.error = b.out.containerKind == QStringLiteral("team")
      ? QStringLiteral("组队战报中官方选中的第 %1/%2 项缺少初始信息 or 或行动列表 fr")
          .arg(b.out.selectedBattleIndex + 1).arg(b.out.containerBattleCount)
      : QStringLiteral("缺少战斗初始信息 or 或行动列表 fr");
    return b.out;
  }
  const auto teams = battle[QStringLiteral("or")].toObject()[QStringLiteral("team")].toArray();
  for (qsizetype t = 0; t < teams.size(); ++t) {
    const auto team = teams[t].toObject();
    const int tid = team[QStringLiteral("id")].toInt(-1);
    QStringList names;
    const auto players = team[QStringLiteral("players")].toArray();
    for (qsizetype p = 0; p < players.size(); ++p) {
      const auto player = players[p].toObject();
      const QString pn = str(player, "name");
      names.append(pn);
      const auto path = base + QStringLiteral("/or/team/%1/players/%2").arg(t).arg(p);
      Fighter representer;
      representer.key = fighterKey(tid, pn, QStringLiteral("@team")); representer.team = tid;
      representer.player = pn; representer.id = QStringLiteral("@team"); representer.representer = true;
      representer.name = pn + QStringLiteral(" · 队伍效果"); representer.path = path;
      representer.raw = player; representer.raw.remove(QStringLiteral("cts"));
      representer.playerX = player[QStringLiteral("x")].toInt(-1); representer.playerY = player[QStringLiteral("y")].toInt(-1);
      representer.buffs = b.initialBuffIds(player[QStringLiteral("pcbfs")], path + QStringLiteral("/pcbfs"), representer.buffsKnown);
      for (const auto& id : representer.buffs)
        representer.buffMembers.append({id, {}, {}, path + QStringLiteral("/pcbfs"), -1});
      representer.lastUnit = player[QStringLiteral("ppbf")].toObject();
      if (representer.lastUnit.contains(QStringLiteral("pbid"))) {
        const auto id = buffId(representer.lastUnit[QStringLiteral("pbid")]);
        if (id.isEmpty()) {
          representer.buffsKnown = false;
          b.issue(path + QStringLiteral("/ppbf/pbid：初始进度 Buff 编号无法确认；原值保留，不建立虚构编号"));
        } else {
          representer.buffs.prepend(id);
          representer.buffMembers.prepend({id, {}, {}, path + QStringLiteral("/ppbf/pbid"), -1});
          TeamProgress progress; progress.buffId = id; progress.path = path + QStringLiteral("/ppbf");
          progress.current = integer(representer.lastUnit[QStringLiteral("cp")]);
          progress.maximum = integer(representer.lastUnit[QStringLiteral("mp")]);
          progress.raw = representer.lastUnit;
          representer.teamProgress.insert(id, progress);
        }
      }
      representer.initialBuffs = representer.buffs; representer.buffPath = path;
      b.state.insert(representer.key, representer);
      const auto fighters = player[QStringLiteral("cts")].toArray();
      QStringList playerFighterKeys;
      for (qsizetype f = 0; f < fighters.size(); ++f) {
        const auto raw = fighters[f].toObject();
        Fighter unit;
        unit.team = tid; unit.player = pn; unit.id = str(raw, "id"); unit.key = fighterKey(tid, pn, unit.id);
        unit.name = raw[QStringLiteral("customName")].toString();
        if (unit.name.isEmpty()) unit.name = raw[QStringLiteral("name")].toString();
        if (unit.name.isEmpty()) unit.name = QStringLiteral("单位 [%1]").arg(unit.id);
        unit.raw = raw; unit.path = path + QStringLiteral("/cts/%1").arg(f);
        unit.playerX = representer.playerX; unit.playerY = representer.playerY;
        unit.x = raw[QStringLiteral("x")].toInt(-1); unit.y = raw[QStringLiteral("y")].toInt(-1);
        unit.hp = integer(raw[QStringLiteral("hp")]); unit.maxHp = integer(raw[QStringLiteral("maxHp")]);
        unit.power = integer(raw[QStringLiteral("vigour")]);
        // FighterInfo defaults only an absent property to zero. Explicit
        // null/invalid evidence stays unknown instead of becoming a balance.
        unit.shield = raw.contains(QStringLiteral("shp")) ? integer(raw[QStringLiteral("shp")]) : std::optional<qint64>(0);
        const auto idValue = [&](const char* field) { const auto n = integer(raw[QLatin1String(field)]); return n ? QString::number(*n) : QString(); };
        unit.raceId = idValue("raceId"); unit.faceId = idValue("faceId");
        unit.normalSkill = idValue("nskill"); unit.uniqueSkill = idValue("uskills"); unit.superUniqueSkill = idValue("suskill");
        unit.attributesKnown = raw[QStringLiteral("attr")].isString();
        unit.attributes = raw[QStringLiteral("attr")].toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        unit.formPath = unit.skillsPath = unit.path; unit.attributesPath = unit.path + QStringLiteral("/attr");
        updateFormReference(unit, officialCatalog);
        // Initial copied forms retain the original pet separately; never use
        // faceId, a nickname or a fighter instance ID as a species lookup.
        auto originalRace = idValue("oRaceId");
        if (originalRace.toLongLong() <= 0) originalRace = unit.raceId;
        if (!originalRace.isEmpty()) unit.name = petName(officialCatalog, originalRace);
        unit.hpPath = unit.path + QStringLiteral("/hp"); unit.maxHpPath = unit.path + QStringLiteral("/maxHp");
        unit.powerPath = unit.path + QStringLiteral("/vigour"); unit.shieldPath = unit.path + QStringLiteral("/shp");
        unit.buffs = b.initialBuffIds(raw[QStringLiteral("bfs")], unit.path + QStringLiteral("/bfs"), unit.buffsKnown);
        unit.initialBuffs = unit.buffs; unit.buffPath = unit.path + QStringLiteral("/bfs");
        for (const auto& id : unit.buffs) unit.buffMembers.append({id, {}, {}, unit.buffPath, -1});
        unit.summoned = raw[QStringLiteral("ics")].toBool(); unit.clone = raw[QStringLiteral("ias")].toBool();
        unit.cloneCalled = raw[QStringLiteral("ibs")].toBool(); unit.chaser = raw[QStringLiteral("icr")].toBool();
        unit.entered = (!unit.summoned || raw[QStringLiteral("is")].toBool()) && (!unit.clone || unit.cloneCalled);
        unit.presencePath = unit.path; updatePresence(unit);
        if (b.state.contains(unit.key)) {
          b.issue(unit.path + QStringLiteral("：重复身份，作为独立记录保留")); unit.key += QStringLiteral("#%1").arg(f);
        }
        b.state.insert(unit.key, unit);
        playerFighterKeys.append(unit.key);
      }
      // SetupFighterCommand uses input order, the same player and INITIAL
      // coordinates. Map order, another team's cell or oRaceId is not a match.
      for (const auto& key : playerFighterKeys) {
        auto& chaser = b.state[key];
        if (!chaser.chaser || chaser.x < 0 || chaser.y < 0) continue;
        for (const auto& sourceKey : playerFighterKeys) {
          const auto& source = b.state[sourceKey];
          if (source.chaser || source.x != chaser.x || source.y != chaser.y) continue;
          const auto pet = officialCatalog[QStringLiteral("pets")].toObject()[source.raceId].toObject();
          const auto race = pet[QStringLiteral("desc")].toString() == QStringLiteral("内测") ? QStringLiteral("9999") : source.raceId;
          if (race.toLongLong() > 0) {
            chaser.originRaceId = race; chaser.originFighterKey = sourceKey;
            chaser.originRelation = QStringLiteral("chaser"); chaser.originPath = source.path + QStringLiteral("/raceId");
          }
          break;
        }
      }
    }
    b.out.teams[tid] = names.join(QStringLiteral(" / "));
  }
  Step initial; initial.title = QStringLiteral("初始阵容"); initial.path = base + QStringLiteral("/or");
  initial.raw = battle[QStringLiteral("or")]; initial.state = b.state;
  if (b.out.containerKind == QStringLiteral("team")) {
    const auto selection = QStringLiteral("组队战报：官方选中第 %1/%2 项").arg(b.out.selectedBattleIndex + 1).arg(b.out.containerBattleCount);
    initial.title += QStringLiteral(" · ") + selection;
    initial.details.append({QStringLiteral("战报容器"), {}, selection + QStringLiteral("；全部成员原文保留在字段与原始数据中"), base, battle});
  }
  b.out.steps.append(initial);
  const auto flow = battle[QStringLiteral("fr")].toArray();
  QSet<int> nonemptyLines;
  bool invalidLine = false;
  for (const auto& value : flow) {
    const auto block = value.toObject();
    if (block[QStringLiteral("rl")].toArray().isEmpty()) continue;
    const auto rawLine = block[QStringLiteral("bfn")];
    const auto line = rawLine.isUndefined() || rawLine.isNull() ? std::optional<qint64>(0) : integer(rawLine);
    if (!line || *line < 0 || *line > 2) invalidLine = true;
    else nonemptyLines.insert(static_cast<int>(*line));
  }
  // Existing one-line reports retain exactly their previous steps/moments. A
  // second empty lane does not turn an ordinary report into parallel combat.
  const bool parallelFlow = invalidLine || nonemptyLines.size() > 1;
  BattleFlowPlan plan;
  if (parallelFlow) {
    plan = planBattleFlow(flow, base + QStringLiteral("/fr"));
    for (const auto& issue : plan.issues) b.issue(issue);
    if (!plan.complete) {
      b.out.error = QStringLiteral("多战线记录无法按官方三槽同步规则完整排程；原文与字段已保留，不以串行顺序代替");
      return b.out;
    }
  }
  QMap<int, int> playerRounds;
  QMap<int, QMap<int, int>> linePlayerRounds;
  QMap<int, Step> segments;
  QMap<int, QMap<int, QStringList>> unitQueues;
  QMap<int, QSet<int>> unitKnownQueues;
  const auto appendSegment = [&](int block, int lineIndex, int batchId) {
    const auto flowObject = flow[block].toObject();
    Step segment;
    segment.lineIndex = lineIndex; segment.batchId = batchId;
    segment.title = QStringLiteral("战斗段 %1 · %2 / %3").arg(block + 1).arg(str(flowObject, "pan"), str(flowObject, "pbn"));
    segment.path = base + QStringLiteral("/fr/%1").arg(block); segment.raw = flowObject;
    for (const auto* field : {"pan", "pbn"}) if (flowObject[QLatin1String(field)].isString()) segment.participants.append(flowObject[QLatin1String(field)].toString());
    for (int side = 0; side < 2; ++side) {
      const auto field = side == 0 ? QStringLiteral("pan") : QStringLiteral("pbn");
      const auto player = flowObject[field];
      if (player.isString() && !player.toString().isEmpty()) segment.actionQueuePlayers[side] = player.toString();
    }
    auto header = flowObject; header.remove(QStringLiteral("rl"));
    segment.raw = header;
    segment.details.append({QStringLiteral("战斗段"), {}, QStringLiteral("当前对战玩家：%1；%2").arg(segment.participants.join(QStringLiteral(" / ")),
      parallelFlow ? QStringLiteral("按官方战线插入屏障排列；位移完成后，同批战线一起推进") : QStringLiteral("按日志战斗段浏览，未补造多条战线之间的动画时刻")), segment.path, header});
    const auto shifts = flowObject[QStringLiteral("pr")].toObject();
    const auto removed = shifts[QStringLiteral("dl")].toArray();
    for (qsizetype d = 0; d < removed.size(); ++d) {
      const auto removal = removed[d].toObject();
      const auto path = segment.path + QStringLiteral("/pr/dl/%1").arg(d);
      const int team = removal[QStringLiteral("teamId")].toInt(-1);
      const auto player = str(removal, "player");
      for (auto it = b.state.begin(); it != b.state.end(); ++it) {
        if (it->team == team && it->player == player) {
          it->playerRemoved = true; it->presencePath = path; updatePresence(it.value());
          segment.affected.append(it.key());
          segment.events.append({QStringLiteral("玩家移除"), it.key(), -1});
        }
      }
      segment.details.append({QStringLiteral("战斗段"), player, QStringLiteral("队伍 %1 的玩家被移除；保留最后阵容供查看").arg(team), path, removal});
      if (!segment.keyChanges.contains(QStringLiteral("玩家移除"))) segment.keyChanges.append(QStringLiteral("玩家移除"));
    }
    const auto moves = shifts[QStringLiteral("ml")].toArray();
    for (qsizetype m = 0; m < moves.size(); ++m) {
      const auto move = moves[m].toObject(); const auto player = str(move, "player");
      const int team = move[QStringLiteral("teamId")].toInt(-1);
      const auto nx = integer(move[QStringLiteral("x_to")]), ny = integer(move[QStringLiteral("y_to")]);
      const auto path = segment.path + QStringLiteral("/pr/ml/%1").arg(m);
      QString old; bool found = false;
      for (auto it = b.state.begin(); it != b.state.end(); ++it) if (it->team == team && it->player == player) {
        old = QStringLiteral("(%1,%2)").arg(it->playerX).arg(it->playerY); found = true;
        it->playerX = nx && *nx >= 0 && *nx <= 2 ? int(*nx) : -1;
        it->playerY = ny && *ny >= 0 && *ny <= 2 ? int(*ny) : -1;
        segment.affected.append(it.key());
        segment.events.append({QStringLiteral("玩家换位"), it.key(), -1});
      }
      if (!found || !nx || !ny) b.issue(path + QStringLiteral("：玩家移位目标或坐标未确认"));
      segment.details.append({QStringLiteral("战斗段"), player,
        QStringLiteral("整套阵容位置 %1 → (%2,%3)；精灵在阵容内的九宫格位置保持原记录").arg(old, shown(nx), shown(ny)), path, move});
      if (!segment.keyChanges.contains(QStringLiteral("玩家换位"))) segment.keyChanges.append(QStringLiteral("玩家换位"));
    }
    segment.state = b.state;
    segment.playerRounds = parallelFlow ? linePlayerRounds.value(lineIndex) : playerRounds;
    segments[block] = segment;
    b.out.steps.append(segment);
  };
  const auto appendAction = [&](int block, int a, int lineIndex, int batchId) {
      const auto segment = segments.value(block);
      const auto actions = flow[block].toObject()[QStringLiteral("rl")].toArray();
      auto& actionQueues = unitQueues[block];
      auto& knownActionQueues = unitKnownQueues[block];
      auto& rounds = parallelFlow ? linePlayerRounds[lineIndex] : playerRounds;
      ++b.out.actions;
      const auto action = actions[a].toObject();
      Step step; step.path = segment.path + QStringLiteral("/rl/%1").arg(a); step.raw = actions[a];
      step.lineIndex = lineIndex; step.batchId = batchId;
      step.participants = segment.participants;
      step.actionQueuePlayers = segment.actionQueuePlayers;
      step.round = action[QStringLiteral("csn")].toInt(-1);
      const bool representer = isTeamAction(action);
      if (!representer && a == actions.size() - 1) {
        step.isLastRound = true;
        for (const auto& target : action[QStringLiteral("trl")].toArray())
          if (target.toObject()[QStringLiteral("issb")].toBool()) step.isLastRound = false;
      }
      step.actor = b.resolve(step, action[QStringLiteral("cti")].toInt(-1), str(action, "cpn"),
        representer ? QStringLiteral("@team") : str(action, "cci"), step.path, QStringLiteral("actor"), action,
        action[QStringLiteral("cn")].toObject());
      // RoundQueue inherits the prior action's queues inside this BattleUnit.
      // Missing is not an explicit empty queue; invalid input is not inherited.
      for (int side = 0; side < 2; ++side) {
        const auto field = side == 0 ? QStringLiteral("seqa") : QStringLiteral("seqb");
        if (!action.contains(field)) continue;
        bool valid = action[field].isArray(); QStringList queue;
        for (const auto& entry : action[field].toArray()) {
          const auto id = integer(entry);
          if (!id) { valid = false; break; }
          queue.append(QString::number(*id));
        }
        if (valid) { actionQueues[side] = queue; knownActionQueues.insert(side); }
        else {
          actionQueues.remove(side); knownActionQueues.remove(side);
          b.issue(step.path + QLatin1Char('/') + field + QStringLiteral("：行动队列格式无效，保持待核实"));
        }
      }
      step.actionQueues = actionQueues; step.knownActionQueues = knownActionQueues;
      const auto playerRound = integer(action[QStringLiteral("pr")]);
      const int sourceTeam = action[QStringLiteral("cti")].toInt(-1);
      // BattleRoundUI keeps the previous visible round for missing/zero values.
      if (sourceTeam >= 0 && playerRound && *playerRound > 0 && *playerRound <= std::numeric_limits<int>::max())
        rounds[sourceTeam] = static_cast<int>(*playerRound);
      step.playerRounds = rounds;
      if (!representer && action[QStringLiteral("fbs")].isBool()) step.fromNormalQueue = action[QStringLiteral("fbs")].toBool();
      if (action[QStringLiteral("isor")].isBool()) step.overRoundLimit = action[QStringLiteral("isor")].toBool();
      const auto skill = representer ? referenceName(officialCatalog, QStringLiteral("buffs"), str(action, "epbi")) : referenceName(officialCatalog, QStringLiteral("skills"), str(action, "si"));
      step.title = b.name(step.actor) + (representer ? QStringLiteral(" · 触发 ") : QStringLiteral(" · ")) + skill;
      step.affected.append(step.actor);
      if (action.contains(QStringLiteral("ses"))) {
        const auto code = str(action, "ses");
        const auto execution = officialCatalog[QStringLiteral("execution")].toObject()[code].toObject()[QStringLiteral("nm")].toString();
        b.detail(step, QStringLiteral("行动"), step.actor,
          QStringLiteral("%1（执行代码 %2）").arg(execution.isEmpty() ? QStringLiteral("含义待核实") : execution, code),
          step.path + QStringLiteral("/ses"), action[QStringLiteral("ses")]);
      }
      if (action[QStringLiteral("cn")].isObject()) b.numbers(step, step.actor, action[QStringLiteral("cn")].toObject(), step.path + QStringLiteral("/cn"));
      for (const auto& listName : {QStringLiteral("trl"), QStringLiteral("prl")}) {
        const auto targets = action[listName].toArray();
        for (qsizetype n = 0; n < targets.size(); ++n) {
          const auto target = targets[n].toObject(); const auto path = step.path + QStringLiteral("/%1/%2").arg(listName).arg(n);
          const auto key = b.resolve(step, target[QStringLiteral("tti")].toInt(-1), str(target, "tpn"),
            targetFighterId(target[QStringLiteral("tci")]), path,
            listName == QStringLiteral("trl") ? QStringLiteral("target") : QStringLiteral("team-target"),
            target, target[QStringLiteral("tn")].toObject());
          QStringList flags;
          for (const auto& field : {QStringLiteral("ishit"), QStringLiteral("isbl"), QStringLiteral("iscr"), QStringLiteral("issb"), QStringLiteral("isIm"), QStringLiteral("isse")}) {
            if (target.contains(field)) flags.append(fieldLabel(child(path, field)) + QStringLiteral("=") + (target[field].toBool() ? QStringLiteral("是") : QStringLiteral("否")));
          }
          b.detail(step, QStringLiteral("目标"), key, flags.isEmpty() ? QStringLiteral("目标记录（没有命中标记）") : flags.join(QStringLiteral("；")), path, target);
          // isse=false still carries side effects; do not drop its tn.
          if (target[QStringLiteral("tn")].isObject()) b.numbers(step, key, target[QStringLiteral("tn")].toObject(), path + QStringLiteral("/tn"));
        }
      }
      advanceAction(step, b.state, officialCatalog, b.out.issues);
      step.state = b.state; b.out.steps.append(step);
  };
  if (!parallelFlow) {
    for (qsizetype block = 0; block < flow.size(); ++block) {
      appendSegment(static_cast<int>(block), -1, -1);
      const auto actions = flow[block].toObject()[QStringLiteral("rl")].toArray();
      for (qsizetype a = 0; a < actions.size(); ++a)
        appendAction(static_cast<int>(block), static_cast<int>(a), -1, -1);
    }
  } else {
    for (qsizetype cursor = 0; cursor < plan.items.size();) {
      const auto item = plan.items[cursor];
      if (item.kind == FlowPlanItemKind::SegmentBarrier) {
        appendSegment(item.frIndex, item.lineIndex, item.batchId); ++cursor; continue;
      }
      const auto start = b.state;
      const int batchId = item.batchId;
      QList<int> stepIndices;
      QList<QSet<QString>> reads, writes, evidenceChanges;
      while (cursor < plan.items.size() && plan.items[cursor].kind == FlowPlanItemKind::Action && plan.items[cursor].batchId == batchId) {
        const auto actionItem = plan.items[cursor++];
        b.state = start;
        appendAction(actionItem.frIndex, actionItem.rlIndex, actionItem.lineIndex, actionItem.batchId);
        auto& step = b.out.steps.last();
        step.parallelBatch = true; step.batchStartState = start;
        stepIndices.append(static_cast<int>(b.out.steps.size() - 1));
        QSet<QString> read, write, changed;
        for (const auto& key : step.affected) if (!key.isEmpty()) read.insert(key);
        if (!step.actor.isEmpty()) read.insert(step.actor);
        for (const auto& effect : step.effects) {
          if (!effect.source.isEmpty()) read.insert(effect.source);
          if (!effect.target.isEmpty()) read.insert(effect.target);
          if (!effect.target.isEmpty() && (effect.kind == QStringLiteral("value") || effect.kind == QStringLiteral("buff") ||
              effect.kind == QStringLiteral("progress") || effect.kind == QStringLiteral("transform") || effect.kind == QStringLiteral("attributes") ||
              effect.kind == QStringLiteral("appearance") || effect.cause == QStringLiteral("calibration"))) write.insert(effect.target);
        }
        for (auto it = b.state.begin(); it != b.state.end(); ++it) {
          if (!start.contains(it.key()) || !sameObservation(start[it.key()], it.value())) write.insert(it.key());
          if (!start.contains(it.key()) || !sameEvidence(start[it.key()], it.value())) changed.insert(it.key());
        }
        reads.append(read); writes.append(write); evidenceChanges.append(changed);
      }
      QSet<QString> conflicts;
      for (qsizetype i = 0; i < stepIndices.size(); ++i)
        for (qsizetype j = 0; j < stepIndices.size(); ++j) if (i != j) {
          auto overlap = writes[i]; overlap.intersect(reads[j] | writes[j]); conflicts.unite(overlap);
        }
      auto merged = start;
      QSet<QString> observedKeys;
      for (const auto& changed : evidenceChanges) observedKeys.unite(changed);
      for (const auto& changed : writes) observedKeys.unite(changed);
      for (const auto& key : observedKeys) {
        int writer = -1, observer = -1, observers = 0;
        for (qsizetype i = 0; i < stepIndices.size(); ++i) {
          if (writes[i].contains(key)) writer = static_cast<int>(i);
          if (evidenceChanges[i].contains(key)) { observer = static_cast<int>(i); ++observers; }
        }
        // Multiple read-only cache observations have no authoritative "last";
        // every raw value remains in its own line's Step instead.
        if (!conflicts.contains(key)) {
          const int selected = writer >= 0 ? writer : observers == 1 ? observer : -1;
          if (selected >= 0) merged[key] = b.out.steps[stepIndices[selected]].state.value(key);
        } else {
          if (!merged.contains(key)) merged[key] = b.out.steps[stepIndices[writer >= 0 ? writer : observer]].state.value(key);
          // Keep the identities of every recorded progress Buff even when its
          // balance cannot be ordered. The per-line records keep their values.
          for (const auto index : stepIndices) {
            const auto progress = b.out.steps[index].state.value(key).teamProgress;
            for (auto it = progress.begin(); it != progress.end(); ++it)
              if (!merged[key].teamProgress.contains(it.key())) merged[key].teamProgress.insert(it.key(), it.value());
          }
          invalidateParallelObservation(merged[key], base + QStringLiteral("/fr"));
        }
      }
      QStringList conflictKeys = conflicts.values(); conflictKeys.sort();
      Step settled; settled.title = QStringLiteral("同批 %1 结算").arg(batchId + 1);
      settled.path = base + QStringLiteral("/fr"); settled.batchId = batchId;
      settled.parallelBatch = true; settled.batchStartState = start; settled.state = merged;
      settled.parallelConflictKeys = conflictKeys; settled.keyChanges.append(QStringLiteral("同批结算"));
      QJsonArray batchRaw;
      for (const auto index : stepIndices) {
        auto& step = b.out.steps[index]; step.parallelConflictKeys = conflictKeys;
        batchRaw.append(step.raw);
        for (const auto& player : step.participants) if (!settled.participants.contains(player)) settled.participants.append(player);
        for (const auto& key : step.affected) if (!settled.affected.contains(key)) settled.affected.append(key);
        settled.details.append({QStringLiteral("同批战线"), {}, QStringLiteral("战线 %1：%2；从共同批次起点分线查看，未规定跨线完成先后")
          .arg(step.lineIndex + 1).arg(step.title), step.path, step.raw});
      }
      settled.raw = batchRaw;
      settled.schedule = QStringLiteral("本批各战线全部完成后推进下一批；仅合并互不冲突的记录，不把分线浏览当作先后顺序");
      for (const auto& key : conflictKeys) {
        const auto text = QStringLiteral("%1：同批不同战线读写同一单位，跨线先后未记录；批末当前状态待核实").arg(fighterName(merged[key]));
        b.issue(settled.path + QStringLiteral(" · 同批 %1 · ").arg(batchId + 1) + text);
        settled.details.append({QStringLiteral("并行顺序待核实"), fighterName(merged[key]), text, settled.path, {}});
      }
      b.state = merged; b.out.steps.append(settled);
    }
  }
  Step end; end.path = base + QStringLiteral("/win"); end.raw = battle[QStringLiteral("win")]; end.state = b.state;
  end.playerRounds = playerRounds;
  const auto win = integer(battle[QStringLiteral("win")]);
  end.title = !win ? QStringLiteral("战斗结束 · 结果未记录") : *win == -1 ? QStringLiteral("战斗结束 · 未分胜负") :
      QStringLiteral("战斗结束 · 队伍 %1 获胜（%2）").arg(*win).arg(b.out.teams.value(static_cast<int>(*win), QStringLiteral("队伍未关联")));
  b.out.steps.append(end);
  return b.out;
}
} // namespace BattleReplay
