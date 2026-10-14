#include "domain/battle_replay_flow.h"

#include <QtCore/QJsonObject>
#include <array>
#include <cmath>
#include <optional>

namespace BattleReplay {
namespace {

constexpr int laneCount = 3;  // Official FormationConsts.MAX_ROW_COUNT.

struct FlowUnit {
  int line = -1;
  int actionCount = 0;
  int shiftCount = 0;
  QString playerA, playerB;
  QStringList deletedPlayers;
};

struct Lane {
  int unit = -1;
  int nextAction = 0;
};

std::optional<int> readLine(const QJsonValue& value) {
  // BattleUnit assigns bfn to an AS int field: absent/undefined and null
  // become zero. An explicitly malformed/out-of-range lane is not guessed.
  if (value.isUndefined() || value.isNull()) return 0;
  bool ok = value.isDouble();
  const double number = value.isString() ? value.toString().toDouble(&ok) : value.toDouble();
  if (!ok || !std::isfinite(number) || number != std::floor(number) || number < 0 || number >= laneCount)
    return {};
  return static_cast<int>(number);
}

QString unitPath(const QString& base, int index) {
  return base + QLatin1Char('/') + QString::number(index);
}

FlowPlanItem barrier(const QString& base, int index, int line, int batch) {
  return {FlowPlanItemKind::SegmentBarrier, index, -1, line, batch, unitPath(base, index)};
}

FlowPlanItem action(const QString& base, int index, int round, int line, int batch) {
  return {FlowPlanItemKind::Action, index, round, line, batch,
    unitPath(base, index) + QStringLiteral("/rl/") + QString::number(round)};
}

void retainUnscheduled(BattleFlowPlan& plan, const QList<FlowUnit>& units,
                       const QList<bool>& loaded, const QList<int>& consumed,
                       const QString& base) {
  for (int i = 0; i < units.size(); ++i) {
    const auto& unit = units[i];
    if (!loaded[i]) plan.unscheduled.append(barrier(base, i, unit.line, -1));
    for (int r = consumed[i]; r < unit.actionCount; ++r)
      plan.unscheduled.append(action(base, i, r, unit.line, -1));
  }
}

}  // namespace

BattleFlowPlan planBattleFlow(const QJsonArray& flow, const QString& flowPath) {
  BattleFlowPlan plan;
  QString base = flowPath;
  while (base.endsWith(QLatin1Char('/'))) base.chop(1);
  QList<FlowUnit> units;
  QList<bool> loaded;
  QList<int> consumed;
  units.reserve(flow.size()); loaded.reserve(flow.size()); consumed.reserve(flow.size());
  const auto issue = [&](const QString& path, const QString& reason) {
    plan.issues.append(path + QStringLiteral("：") + reason);
  };

  for (int i = 0; i < flow.size(); ++i) {
    const auto path = unitPath(base, i);
    const auto raw = flow[i].toObject();
    FlowUnit unit;
    const auto line = readLine(raw[QStringLiteral("bfn")]);
    if (line) unit.line = *line;
    unit.actionCount = static_cast<int>(raw[QStringLiteral("rl")].toArray().size());
    unit.playerA = raw[QStringLiteral("pan")].toString();
    unit.playerB = raw[QStringLiteral("pbn")].toString();
    if (!flow[i].isObject()) issue(path, QStringLiteral("战斗段不是对象，无法确认装入时机"));
    else {
      if (!line) issue(path + QStringLiteral("/bfn"), QStringLiteral("战线索引须为 0、1、2；显式非法值不能作为战线 0"));
      if (!raw[QStringLiteral("rl")].isArray())
        issue(path + QStringLiteral("/rl"), QStringLiteral("行动列表不是数组，流程推进待核实"));
      const auto isFight = raw[QStringLiteral("isf")];
      if (!isFight.isUndefined() && !isFight.isNull() && !isFight.isBool())
        issue(path + QStringLiteral("/isf"), QStringLiteral("交战标记不是布尔值，流程推进待核实"));
      else if (!isFight.toBool() && unit.actionCount > 0)
        // run() consumes these actions, but isf does not set _lineState. When
        // the last action leaves the mask unchanged, the official callbacks
        // need not admit another unit or finish. Retain, never silently fix it.
        issue(path + QStringLiteral("/isf"), QStringLiteral("交战标记为否或缺省，但 rl 非空；官方结束回填条件不完整，保留全部行动，流程推进待核实"));

      const auto shiftsValue = raw[QStringLiteral("pr")];
      if (!shiftsValue.isUndefined() && !shiftsValue.isNull() && !shiftsValue.isObject())
        issue(path + QStringLiteral("/pr"), QStringLiteral("玩家变动不是对象，无法确认装入屏障"));
      const auto shifts = shiftsValue.toObject();
      for (const auto& field : {QStringLiteral("dl"), QStringLiteral("ml")}) {
        const auto values = shifts[field];
        if (values.isUndefined() || values.isNull()) continue;
        if (!values.isArray()) {
          issue(path + QStringLiteral("/pr/") + field, QStringLiteral("玩家变动列表不是数组，无法确认装入屏障"));
          continue;
        }
        const auto entries = values.toArray();
        if (field == QStringLiteral("ml")) unit.shiftCount = static_cast<int>(entries.size());
        for (int j = 0; j < entries.size(); ++j) {
          const auto entryPath = path + QStringLiteral("/pr/") + field + QLatin1Char('/') + QString::number(j);
          const auto entry = entries[j].toObject();
          const auto player = entry[QStringLiteral("player")];
          if (!entries[j].isObject() || !player.isString() || player.toString().isEmpty()) {
            issue(entryPath, QStringLiteral("玩家变动缺少明确玩家名，无法确认装入屏障"));
            continue;
          }
          if (field == QStringLiteral("dl")) unit.deletedPlayers.append(player.toString());
        }
      }
    }
    units.append(unit); loaded.append(false); consumed.append(0);
  }

  // Do not publish a plausible-looking partial timeline for a structurally
  // unconfirmed input. Its original segment/action paths remain enumerable.
  if (!plan.issues.isEmpty()) {
    retainUnscheduled(plan, units, loaded, consumed, base);
    return plan;
  }

  std::array<Lane, laneCount> lanes{};
  const auto hasActions = [&](const Lane& lane) {
    return lane.unit >= 0 && lane.nextAction < units[lane.unit].actionCount;
  };
  const auto activePlayer = [&](const QString& player) {
    for (const auto& lane : lanes) if (hasActions(lane)) {
      const auto& unit = units[lane.unit];
      if (unit.playerA == player || unit.playerB == player) return true;
    }
    return false;
  };
  const auto firstMatchingPlayerActive = [&](const QString& player) {
    // The original returns at the first matching slot, including an exhausted
    // slot. Repeated player names can therefore make its answer ambiguous.
    for (const auto& lane : lanes) if (lane.unit >= 0) {
      const auto& unit = units[lane.unit];
      if (unit.playerA == player || unit.playerB == player) return hasActions(lane);
    }
    return false;
  };

  int pending = 0;
  bool refill = true;
  while (true) {
    QString blockedReason;
    if (refill) {
      int shiftingUnit = -1;
      // BattleFlowManager never skips a blocked queue head, even if a later
      // segment could fit another empty lane.
      while (pending < units.size()) {
        const auto& unit = units[pending];
        auto& lane = lanes[unit.line];
        if (hasActions(lane)) {
          blockedReason = QStringLiteral("该战线仍有未完成行动");
          break;
        }
        for (int deletion = 0; deletion < unit.deletedPlayers.size(); ++deletion) {
          const auto& player = unit.deletedPlayers[deletion];
          const bool active = activePlayer(player);
          const auto deletionPath = unitPath(base, pending) + QStringLiteral("/pr/dl/") + QString::number(deletion);
          if (active != firstMatchingPlayerActive(player)) {
            issue(deletionPath, QStringLiteral("同名玩家同时出现在已结束和活跃战线，官方首次匹配的删除检查存在歧义，流程推进待核实"));
            retainUnscheduled(plan, units, loaded, consumed, base);
            return plan;
          }
          if (!active) continue;
          if (deletion > 0) {
            // handleDeleteAction applies earlier deletions before returning
            // false for this one. A single admission barrier cannot express
            // that partial mutation, so do not silently move it to a later batch.
            issue(deletionPath, QStringLiteral("官方会先处理前面的删除，再因当前活跃玩家阻塞；部分删除无法归入单一装入屏障，流程推进待核实"));
            retainUnscheduled(plan, units, loaded, consumed, base);
            return plan;
          }
          blockedReason = QStringLiteral("待删除玩家仍在活跃战线：") + player;
          break;
        }
        if (!blockedReason.isEmpty()) break;
        if (shiftingUnit >= 0) {
          // handleShiftActions assigns, rather than adds to, _shiftSemaphore.
          // Another admitted segment overwrites an outstanding movement count
          // (even when its own ml is empty), so callback timing can start extra
          // run() calls. Do not invent a complete inter-action order for it.
          issue(unitPath(base, pending) + QStringLiteral("/pr/ml"),
            QStringLiteral("本次装入会覆盖前段 ") + unitPath(base, shiftingUnit) +
            QStringLiteral("/pr/ml 的移动回调计数，官方动作推进依赖实际回调时机，流程推进待核实"));
          retainUnscheduled(plan, units, loaded, consumed, base);
          return plan;
        }
        plan.items.append(barrier(base, pending, unit.line, plan.batchCount));
        loaded[pending] = true;
        lane = {pending, 0};
        if (unit.shiftCount > 0) shiftingUnit = pending;
        ++pending;
      }
    }

    // Shift callbacks are a barrier before this batch. Within a batch, each
    // active lane starts one action; the next batch waits for ALL to complete.
    bool anyAction = false, ended = false;
    for (int line = 0; line < laneCount; ++line) {
      auto& lane = lanes[line];
      if (!hasActions(lane)) continue;
      anyAction = true;
      plan.items.append(action(base, lane.unit, lane.nextAction, line, plan.batchCount));
      ++lane.nextAction; ++consumed[lane.unit];
      ended = ended || !hasActions(lane);
    }
    if (!anyAction) {
      if (pending < units.size()) {
        issue(unitPath(base, pending), QStringLiteral("战斗段无法装入且没有可推进的活跃战线") +
          (blockedReason.isEmpty() ? QString() : QStringLiteral("；") + blockedReason));
        retainUnscheduled(plan, units, loaded, consumed, base);
      } else plan.complete = true;
      return plan;
    }
    ++plan.batchCount;
    refill = ended;
    // Every nonterminal iteration consumes at least one action, so malformed
    // dependencies cannot produce an unbounded retry loop.
  }
}

}  // namespace BattleReplay
