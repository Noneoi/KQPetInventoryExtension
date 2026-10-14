#pragma once

#include "ui/battle/battle_replay_cell.h"
#include "ui/battle/battle_replay_life_timeline_data.h"
#include "ui/battle/battle_replay_page.h"
#include "fixtures/battle_replay_fixture.h"
#include <QJsonArray>
#include <QLabel>

inline bool verifySummonDisplayCell() {
  using namespace BattleReplay;
  const auto fail = [](const char* why) { qCritical() << "summon display:" << why; return false; };
  Fighter summon; summon.key = "summon"; summon.name = "synthetic summon"; summon.team = 0;
  summon.summoned = true; summon.entered = false; summon.hp = summon.maxHp = 50;
  updatePresence(summon);
  Fighter other = summon; other.key = "other"; other.name = "synthetic other";
  other.summoned = false; other.entered = true; other.hp = 0; updatePresence(other);
  const auto pending = summon;
  ReplayCell cell(0);
  cell.display(1, {other, summon}, {}, {}, {});
  cell.focusUnit(summon.key);
  if (!cell.property("summonPending").toBool() || !cell.toPlainText().contains(QStringLiteral("待召唤")) ||
      cell.relationAnchor(summon.key)->property("presenceText").toString() != QStringLiteral("待召唤"))
    return fail("positive initial HP was treated as summon entry");
  summon.displayLayersRestored = true; summon.displayLayersPath = "/synthetic/tn/ad_bfs";
  updatePresence(summon);
  cell.display(1, {other, summon}, {}, {}, {});
  if (cell.property("summonPending").toBool() || cell.toPlainText().contains(QStringLiteral("待召唤")) ||
      cell.property("visibleFighterKey").toString() != summon.key ||
      cell.relationAnchor(summon.key)->property("presenceText").toString() != QStringLiteral("存活") ||
      cell.relationAnchor(summon.key)->property("isDead").toBool())
    return fail("restored summon still pending in the selected card or same-cell row");
  cell.focusUnit(other.key); cell.focusUnit(summon.key);
  if (cell.property("selectedUnitDead").toBool() || cell.property("deadUnit").toBool())
    return fail("same-cell dead occupant obscured the restored summon");
  ReplayCell sorted(0); sorted.display(1, {other, summon}, {}, {}, {});
  if (sorted.property("visibleFighterKey").toString() != summon.key)
    return fail("restored summon lost active-occupant sorting");
  auto dead = summon; dead.hp = 0; updatePresence(dead);
  cell.display(1, {dead}, {}, {}, {});
  if (!cell.property("deadUnit").toBool() || !cell.toPlainText().contains(QStringLiteral("已阵亡")))
    return fail("restored presentation concealed the recorded death");
  cell.display(1, {pending}, {}, {}, {});
  if (!cell.property("summonPending").toBool() || !cell.toPlainText().contains(QStringLiteral("待召唤")))
    return fail("backward navigation retained a future display restoration");

  // Restoration itself supplies no death or revival event. Only the two
  // explicitly recorded HP crossings below may become timeline markers.
  Replay replay; Step step;
  Moment start; start.phase = "setup"; start.part = "initial"; start.state.insert(summon.key, summon);
  Moment died = start; died.phase = "attack"; died.part = "values"; died.state[summon.key] = dead;
  Moment revived = died; revived.state[summon.key] = summon;
  step.moments = {start, died, revived}; step.state = revived.state;
  step.events = {{QStringLiteral("生命归零"), summon.key, 1}, {QStringLiteral("生命恢复"), summon.key, 2}};
  replay.steps = {step};
  const auto model = ReplayTimeline::buildLifeModel(replay, {});
  if (model.markers.size() != 2 || model.markers[0].label != QStringLiteral("死亡") ||
      model.markers[1].label != QStringLiteral("复活")) return fail("life timeline still treated restored summon as pending");
  return true;
}

inline QJsonObject verifyRecordedSummonDisplay(BattleReplayPage& page) {
  QJsonArray checked;
  const auto fail = [&](const QString& error) {
    return QJsonObject{{"passed", false}, {"error", error}, {"sha256", page.replay().sha256}, {"checked", checked}};
  };
  const auto rolesFor = [&](const QString& key) -> QLabel* {
    for (auto* card : page.findChildren<QWidget*>(QStringLiteral("replayUnitCard")))
      if (card->property("fighterKey").toString() == key)
        return card->findChild<QLabel*>(QStringLiteral("unitRoles"));
    return nullptr;
  };
  for (int index = 0; index < page.replay().steps.size(); ++index) {
    const auto& step = page.replay().steps[index];
    for (const auto& effect : step.effects) {
      if (!effect.restoresDisplayLayers || effect.moment <= 0) continue;
      const auto before = step.moments[effect.moment - 1].state.value(effect.target);
      const auto after = step.moments[effect.moment].state.value(effect.target);
      if (!before.summoned || before.entered || before.displayLayersRestored) continue;
      if (!after.displayLayersRestored || after.entered || after.displayLayersPath != effect.path)
        return fail(QStringLiteral("官方显现命令没有独立保存，或被错误写为入场指令"));
      page.seek(index); page.seekMoment(effect.moment - 1);
      auto* roles = rolesFor(effect.target);
      if (!roles || !roles->accessibleName().contains(QStringLiteral("待召唤")))
        return fail(QStringLiteral("显现前格子没有保留待召唤状态"));
      page.seekMoment(effect.moment); roles = rolesFor(effect.target);
      if (!roles || roles->accessibleName().contains(QStringLiteral("待召唤")) ||
          !roles->accessibleName().contains(QStringLiteral("召唤")) ||
          page.displayedState()[effect.target].presence.contains(QStringLiteral("待召唤")))
        return fail(QStringLiteral("官方已经显现，格子或详情仍显示待召唤"));
      page.seekMoment(step.moments.size() - 1); roles = rolesFor(effect.target);
      if (!roles || roles->accessibleName().contains(QStringLiteral("待召唤")))
        return fail(QStringLiteral("小回合结束又丢失显现状态"));
      page.seekMoment(effect.moment - 1); roles = rolesFor(effect.target);
      if (!roles || !roles->accessibleName().contains(QStringLiteral("待召唤")))
        return fail(QStringLiteral("回退后保留了未来显现状态"));
      checked.append(QJsonObject{{"step", index}, {"moment", effect.moment}, {"path", effect.path},
        {"target", effect.target}, {"buffId", effect.causeId}, {"raw", effect.raw},
        {"entryConfirmed", after.entered}, {"displayRestored", after.displayLayersRestored}});
    }
  }
  if (checked.isEmpty()) return fail(QStringLiteral("样本没有经过待召唤到官方显现的记录"));
  return QJsonObject{{"passed", true}, {"sha256", page.replay().sha256}, {"checked", checked}};
}
