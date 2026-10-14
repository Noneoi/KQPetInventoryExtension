#include "domain/battle_replay_dragon.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace BattleReplay {
namespace {

std::optional<qint64> integer(const QJsonValue& value) {
  if (value.isDouble()) {
    const double n = value.toDouble();
    if (std::isfinite(n) && std::trunc(n) == n && std::abs(n) <= 9007199254740991.0)
      return static_cast<qint64>(n);
  } else if (value.isString()) {
    static const QRegularExpression digits(QStringLiteral("^[+-]?[0-9]+$"));
    const auto text = value.toString(); bool ok = false;
    const auto n = text.toLongLong(&ok);
    if (ok && digits.match(text).hasMatch() && std::abs(static_cast<double>(n)) <= 9007199254740991.0) return n;
  }
  return {};
}

QString identifier(const QJsonValue& value) {
  const auto n = integer(value);
  return n && *n > 0 ? QString::number(*n) : QString();
}

void formatDescription(DragonInfo& dragon) {
  dragon.description = dragon.descriptionTemplate;
  if (dragon.description.isEmpty()) return;
  dragon.descriptionComplete = true;
  if (dragon.description.contains(QLatin1Char('N'))) {
    if (!dragon.skillValue) {
      dragon.descriptionComplete = false;
      dragon.issues.append(QStringLiteral("能力值未记录；保留官方技能说明模板"));
      return;
    }
    dragon.description.replace(QLatin1Char('N'), QString::number(*dragon.skillValue));
  }
  // All seven verified definitions use a numeric multiplication inside [].
  // Match that language exactly; future unverified expressions remain visible.
  // This is the official description formatter, not battle-effect calculation.
  static const QRegularExpression bracket(QStringLiteral("\\[([^\\[\\]]+)\\]"));
  static const QRegularExpression product(QStringLiteral("^\\s*([+-]?[0-9]+(?:\\.[0-9]+)?)\\s*\\*\\s*([+-]?[0-9]+(?:\\.[0-9]+)?)\\s*$"));
  QString result; qsizetype cursor = 0;
  auto matches = bracket.globalMatch(dragon.description);
  while (matches.hasNext()) {
    const auto match = matches.next();
    result += dragon.description.mid(cursor, match.capturedStart() - cursor);
    const auto operands = product.match(match.captured(1));
    bool substituted = false;
    if (operands.hasMatch()) {
      const double value = operands.captured(1).toDouble() * operands.captured(2).toDouble();
      if (std::isfinite(value) && value >= std::numeric_limits<qint32>::min() && value <= std::numeric_limits<qint32>::max()) {
        result += QString::number(static_cast<qint32>(value)); substituted = true;
      }
    }
    if (!substituted) { result += match.captured(0); dragon.descriptionComplete = false; }
    cursor = match.capturedEnd();
  }
  result += dragon.description.mid(cursor); dragon.description = result;
  if (!dragon.descriptionComplete) dragon.issues.append(QStringLiteral("说明中含未确认的运算格式；已保留原表达式"));
}

DragonRoster initialRoster(const Fighter& owner, const QJsonObject& catalog) {
  DragonRoster result; result.ownerKey = owner.key; result.path = owner.path + QStringLiteral("/drs");
  result.raw = owner.raw[QStringLiteral("drs")];
  if (!owner.representer) {
    result.issues.append(QStringLiteral("小神龙资料必须属于玩家队伍对象")); return result;
  }
  if (result.raw.isUndefined() || result.raw.isNull()) return result;
  if (!result.raw.isArray()) {
    result.issues.append(result.path + QStringLiteral("：小神龙列表格式无效")); return result;
  }
  result.rosterKnown = true;
  const auto definitions = catalog[QStringLiteral("petDragons")].toObject()[QStringLiteral("definitions")].toObject();
  const auto qualities = catalog[QStringLiteral("petDragons")].toObject()[QStringLiteral("qualities")].toObject();
  const auto roster = result.raw.toArray(); QSet<QString> ids;
  for (int slot = 0; slot < roster.size(); ++slot) {
    DragonInfo dragon; dragon.slot = slot; dragon.path = result.path + QLatin1Char('/') + QString::number(slot);
    dragon.key = owner.key + QStringLiteral("/dragon/") + QString::number(slot); dragon.raw = roster[slot];
    const auto raw = roster[slot].toObject();
    dragon.id = identifier(raw[QStringLiteral("di")]); dragon.defineId = identifier(raw[QStringLiteral("dfi")]);
    dragon.quality = integer(raw[QStringLiteral("q")]); dragon.skillValue = integer(raw[QStringLiteral("nv")]);
    dragon.executeBufferId = identifier(raw[QStringLiteral("dbfi")]);
    if (dragon.quality) dragon.qualityName = qualities[QString::number(*dragon.quality)].toString();
    if (dragon.qualityName.isEmpty()) dragon.qualityName = QStringLiteral("未知");
    const auto gene = raw[QStringLiteral("ge")];
    dragon.geneRecorded = gene.isString();
    if (dragon.geneRecorded) {
      dragon.gene = gene.toString();
      // PetDragonGene.isGlittery checks these exact two case-sensitive chars.
      dragon.glittery = dragon.gene.startsWith(QStringLiteral("aa"));
    } else if (!gene.isUndefined() && !gene.isNull()) dragon.issues.append(QStringLiteral("基因字段格式无效"));
    const auto definition = definitions[dragon.defineId].toObject();
    dragon.name = definition[QStringLiteral("name")].toString();
    dragon.definitionKnown = !dragon.name.isEmpty();
    if (!dragon.definitionKnown)
      dragon.name = dragon.defineId.isEmpty() ? QStringLiteral("小神龙（定义未记）") : QStringLiteral("小神龙 [%1]").arg(dragon.defineId);
    dragon.skillType = definition[QStringLiteral("skillType")].toString();
    dragon.descriptionTemplate = definition[QStringLiteral("descriptionTemplate")].toString();
    dragon.definitionSource = definition[QStringLiteral("source")].toObject();
    formatDescription(dragon);
    if (!roster[slot].isObject() || dragon.id.isEmpty()) dragon.issues.append(QStringLiteral("实例编号未确认，不能关联出手记录"));
    else if (slot < 2) dragon.state = DragonState::Ready;
    if (slot >= 2) dragon.issues.append(QStringLiteral("超出官方两席队列范围；保留资料，不推定播放状态"));
    if (!dragon.id.isEmpty() && ids.contains(dragon.id))
      dragon.issues.append(QStringLiteral("实例编号重复；官方队列只定位第一个匹配项"));
    ids.insert(dragon.id);
    result.dragons.append(dragon);
  }
  return result;
}

} // namespace

QString dragonStateKey(DragonState state) {
  switch (state) {
  case DragonState::Ready: return QStringLiteral("ready");
  case DragonState::Fighting: return QStringLiteral("fighting");
  case DragonState::Finished: return QStringLiteral("finished");
  default: return QStringLiteral("unknown");
  }
}

QString dragonStateText(DragonState state) {
  switch (state) {
  case DragonState::Ready: return QStringLiteral("准备");
  case DragonState::Fighting: return QStringLiteral("出手中");
  case DragonState::Finished: return QStringLiteral("已完成");
  default: return QStringLiteral("状态未确认");
  }
}

DragonRoster dragonRosterAt(const Replay& replay, const Fighter& representer, int stepIndex,
                           DragonCommandProgress progress, const QJsonObject& officialCatalog) {
  auto result = initialRoster(representer, officialCatalog);
  if (!representer.representer || stepIndex < 0) return result;
  if (stepIndex >= replay.steps.size()) {
    result.issues.append(QStringLiteral("回放位置越界；未应用出手状态")); return result;
  }
  // PetDragonSeqBar starts at slot 0 and accepts one FINISHED callback per
  // roundNum. Command completion and queue-icon completion are not synonyms.
  std::optional<int> currentSlot = 0;
  qint64 lastFinishedRound = -1;
  for (int index = 0; index <= stepIndex; ++index) {
    const auto& step = replay.steps[index];
    if (step.actor != representer.key) continue;
    const auto raw = step.raw.toObject(); const auto eps = raw[QStringLiteral("eps")];
    const auto path = step.path + QStringLiteral("/eps");
    if (index == stepIndex) {
      result.executeRaw = eps; result.executePath = path; result.executionSkillId = step.playbackSkillId;
      for (const auto& id : eps.toArray()) result.executeIds.append(valueText(id));
    }
    if (index == stepIndex && progress == DragonCommandProgress::Before) continue;
    if (eps.isUndefined() || eps.isNull()) continue;
    if (!eps.isArray()) { result.issues.append(path + QStringLiteral("：出手实例列表格式无效")); continue; }
    const auto ids = eps.toArray(); if (ids.isEmpty()) continue;
    if (step.parallelBatch) { result.issues.append(path + QStringLiteral("：多战线小神龙播放不在此模型范围内")); continue; }
    const bool hasStart = step.playbackKind == QStringLiteral("team-skill");
    if (!hasStart && step.playbackKind != QStringLiteral("team-simple")) {
      result.issues.append(path + QStringLiteral("：仅保留小神龙关联编号，未确认小神龙出手指令")); continue;
    }
    if (hasStart) {
      const auto id = identifier(ids.first());
      int match = -1;
      if (!id.isEmpty()) for (int i = 0; i < result.dragons.size(); ++i)
        if (result.dragons[i].id == id) { match = i; break; }
      if (match < 0 || match >= 2) {
        result.issues.append(path + QStringLiteral("：首个出手编号未匹配本玩家的两席小神龙记录；当前槽位未确认"));
        currentSlot.reset(); continue;
      }
      // START names an instance, never another dragon sharing its dbfi.
      currentSlot = match;
      auto& dragon = result.dragons[match];
      dragon.state = DragonState::Fighting;
      dragon.lastAction = index; dragon.lastActionPath = step.path; dragon.lastSkillId = step.playbackSkillId;
    }
    if (index == stepIndex && progress == DragonCommandProgress::Active) continue;
    // Both representer command variants emit FINISHED when eps is nonempty.
    // Missing/null csn becomes 0 in RoundResult's int member. Other malformed
    // values are retained rather than guessed or used to advance the queue.
    const auto csn = raw[QStringLiteral("csn")];
    const auto round = csn.isUndefined() || csn.isNull() ? std::optional<qint64>(0) : integer(csn);
    if (!round || *round < std::numeric_limits<qint32>::min() || *round > std::numeric_limits<qint32>::max()) {
      result.issues.append(step.path + QStringLiteral("/csn：回合值未确认，不能更新小神龙完成状态")); continue;
    }
    if (!currentSlot) { result.issues.append(path + QStringLiteral("：结束回调的当前槽位未确认，未猜测完成对象")); continue; }
    if (*currentSlot >= 2 || *round == lastFinishedRound) continue;
    if (*currentSlot >= result.dragons.size() || result.dragons[*currentSlot].id.isEmpty()) {
      result.issues.append(path + QStringLiteral("：结束回调没有可确认的当前小神龙")); currentSlot.reset(); continue;
    }
    auto& completed = result.dragons[*currentSlot];
    completed.state = DragonState::Finished;
    completed.lastCompletionAction = index; completed.lastCompletionPath = step.path;
    lastFinishedRound = *round;
    int next = 2;
    for (int slot = *currentSlot + 1; slot < std::min(2, static_cast<int>(result.dragons.size())); ++slot)
      if (!result.dragons[slot].id.isEmpty()) { next = slot; break; }
    currentSlot = next;
    if (next < 2) result.dragons[next].state = DragonState::Ready;
  }
  if (currentSlot && *currentSlot < result.dragons.size() && result.dragons[*currentSlot].state == DragonState::Fighting)
    result.activeId = result.dragons[*currentSlot].id;
  return result;
}

} // namespace BattleReplay
