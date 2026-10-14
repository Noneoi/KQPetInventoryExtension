#pragma once

#include "ui/battle/battle_replay_buff_presentation.h"
#include "ui/battle/battle_replay_cell.h"
#include "ui/battle/battle_replay_page.h"
#include "ui/battle/battle_replay_timeline_events.h"
#include "battle_replay_preview_catalog.h"
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTest>
#include <QToolButton>
#include <QUrl>

inline bool verifyReplayBuffDisplay(const QJsonObject& official) {
  using namespace BattleReplay;
  const auto fail = [](const char* why) { qCritical() << "Buff display categories:" << why; return false; };
  auto catalog = official;
  auto definitions = catalog["buffs"].toObject();
  const QStringList unexplainedIds{"30098", "42383", "42384", "42385", "42386", "42387", "42388", "72899"};
  for (const auto& id : unexplainedIds) {
    const auto config = definitions[id].toObject();
    if (config.isEmpty() || replayBuffHasExplanation(config) || !replayBuffStateCues(config).isEmpty())
      return fail("an unexplained registration inherited a state from its ID or animation wording");
    for (const auto& name : {QStringLiteral("任意动画"), QStringLiteral("无法行动·增益状态"), QString()}) {
      auto changed = config; changed["nm"] = name; changed["did"] = "8";
      changed["hd"] = "1"; changed["once"] = "1"; changed["bhs"] = QStringLiteral("烧伤");
      if (replayBuffHasExplanation(changed) || !replayBuffStateCues(changed).isEmpty())
        return fail("name, resource, hidden/once flags or numeric-source wording changed explanation classification");
    }
    auto explained = config; explained["mess"] = QStringLiteral("<font color='#123456'>合成明确说明</font>");
    if (!replayBuffHasExplanation(explained)) return fail("an actual explanation was suppressed by the old ID category");
    explained["mess"] = ""; explained["beffect"] = QStringLiteral("战魂");
    if (!replayBuffHasExplanation(explained) || replayBuffStateCues(explained) != QStringList{QStringLiteral("战魂")})
      return fail("verified official presentation vocabulary did not provide an explanation");
  }
  for (const auto& id : {QStringLiteral("8"), QStringLiteral("2879"), QStringLiteral("2014")}) {
    auto config = definitions[id].toObject();
    if (config.isEmpty() || !replayBuffHasExplanation(config)) return fail("an official explanation was lost");
    config["nm"] = QStringLiteral("任意动画名字"); config["did"] = "42383";
    config["hd"] = "1"; config["once"] = "1"; config["bhs"] = "";
    if (!replayBuffHasExplanation(config)) return fail("presentation metadata moved an explained member to another category");
  }
  const QString unknownId = QStringLiteral("2147483600");
  if (replayBuffHasExplanation({}) || replayBuffHasExplanation(QJsonObject{{"nm", "晕眩"}, {"did", "8"}, {"bhs", "烧伤"}}) ||
      replayBuffHasExplanation(QJsonObject{{"mess", "<b> </b>"}, {"beffect", "血量不显示 气势不显示"}}))
    return fail("missing configuration or non-state presentation metadata became an explanation");
  auto ordinary = battleReplayTestCatalog()["buffs"].toObject()["99"].toObject();
  ordinary["once"] = "1";
  ordinary["mess"] = QStringLiteral("合成明确说明，独立于播放设置");
  ordinary["beffect"] = QStringLiteral("血量不显示 气势不显示");
  if (!replayBuffHasExplanation(ordinary) || !replayBuffStateCues(ordinary).isEmpty())
    return fail("independent explanation and playback suppression flags were conflated");
  definitions["99"] = ordinary; catalog["buffs"] = definitions;

  Fighter holder;
  holder.team = 1; holder.player = QStringLiteral("tester"); holder.id = QStringLiteral("7");
  holder.key = fighterKey(holder.team, holder.player, holder.id); holder.name = QStringLiteral("合成同名精灵");
  holder.x = holder.y = 0; holder.hp = 100; holder.maxHp = 200; holder.shield = 25;
  holder.power = 60; holder.maxPower = 100;
  const QString ally = fighterKey(0, "tester", "7"), enemy = fighterKey(1, "tester", "8");
  const auto add = [&](const QString& id, const QString& source = {}, int team = -1) {
    holder.buffs.append(id);
    holder.buffMembers.append({id, source, source == ally ? QStringLiteral("合成我方来源") :
      source == enemy ? QStringLiteral("合成敌方来源") : QString(),
      QStringLiteral("/synthetic/member/%1").arg(holder.buffs.size() - 1), team});
  };
  add("42383", ally, 0); add("42383", ally, 0); add("42383", enemy, 1); add("42383");
  add("42384", ally, 0); add("30098"); add("8", ally, 0); add("8", ally, 0);
  add("99", ally, 0); add("99", ally, 0); add(unknownId); add("72899"); add("2879");
  const auto originalHolder = holder;
  auto other = holder; other.player = QStringLiteral("different-player");
  other.key = fighterKey(other.team, other.player, other.id);

  const auto buffEvent = [&](const QString& id, int index, int step, const QString& source, int team, bool removed = false) {
    ReplayUnitEvent event;
    event.index = index; event.recordStep = step; event.buffId = id; event.removing = removed;
    event.buffSource = source; event.buffSourceTeam = team;
    event.buffSourceName = source == ally ? QStringLiteral("合成我方来源") : source == enemy ? QStringLiteral("合成敌方来源") : QString();
    event.buffSourcePath = QStringLiteral("/synthetic/source/%1").arg(index);
    event.label = (removed ? QStringLiteral("− ") : QStringLiteral("＋ ")) +
      (definitions[id].toObject()["nm"].toString().isEmpty() ? QStringLiteral("Buff [%1]").arg(id) : definitions[id].toObject()["nm"].toString());
    event.detail = QStringLiteral("合成原始变更记录 %1，行动 %2").arg(index).arg(step);
    event.color = removed ? QStringLiteral("#b4475c") : QStringLiteral("#197b50");
    return event;
  };
  const auto addOne = buffEvent("42383", 41, 3, ally, 0), addTwo = buffEvent("42383", 42, 4, ally, 0);
  const auto removeOne = buffEvent("42383", 43, 5, ally, 0, true), blankAnimation = buffEvent("30098", 44, 5, {}, -1);
  const auto stateEvent = buffEvent("8", 45, 5, ally, 0), unknownEvent = buffEvent(unknownId, 46, 5, {}, -1);
  auto trigger = buffEvent("42383", 48, 5, {}, -1); trigger.buffTriggered = true;
  trigger.label = QStringLiteral("触发 · Buff [42383]");
  trigger.detail = QStringLiteral("合成已记录数值触发：生命 130 → 100；变化 -30");
  auto restore = buffEvent("30098", 49, 5, {}, -1); restore.restoresDisplayLayers = true;
  const QList<ReplayUnitEvent> history{addOne, addTwo, removeOne, blankAnimation, stateEvent, unknownEvent, trigger, restore};
  ReplayUnitEvent numeric;
  numeric.index = 47; numeric.buffId = "42383"; numeric.metric = QStringLiteral("生命");
  numeric.valueText = QStringLiteral("-30"); numeric.label = QStringLiteral("生命 -30");
  numeric.detail = QStringLiteral("合成已记录生命 130 → 100"); numeric.observedBefore = 130; numeric.observedAfter = 100;
  numeric.color = QStringLiteral("#bd3858");
  ReplayUnitOverview cue; cue.hpBefore = 130; cue.maxHpBefore = 200;
  cue.events = {addOne, addTwo, removeOne, blankAnimation, numeric, trigger, restore};
  cue.roundBuffEvents = history; cue.buffRound = 1;
  ReplayCell cell(1); cell.resize(300, 330); cell.show();
  const auto render = [&](int ownTeam = 0) {
    cell.display(1, {holder, other}, catalog, {}, {}, {}, {}, {}, {}, {{holder.key, cue}}, ownTeam);
    cell.focusUnit(holder.key); QCoreApplication::processEvents();
  };
  const auto card = [&]() -> QWidget* {
    auto* stack = cell.findChild<QStackedWidget*>(QStringLiteral("cellUnits"));
    return stack ? stack->currentWidget() : nullptr;
  };
  const auto browser = [&]() -> QTextBrowser* { return card() ? card()->findChild<QTextBrowser*>(QStringLiteral("unitBuffs")) : nullptr; };
  const auto toggle = [&](bool playback) -> QToolButton* {
    return card() ? card()->findChild<QToolButton*>(playback ? QStringLiteral("unitBuffPlaybackToggle") : QStringLiteral("unitBuffToggle")) : nullptr;
  };
  const auto unknownToggle = [&]() -> QToolButton* {
    return card() ? card()->findChild<QToolButton*>(QStringLiteral("unitBuffUnknownToggle")) : nullptr;
  };
  const auto anchors = [](QTextBrowser* view) {
    QMap<QString, QString> result;
    for (auto block = view->document()->begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment(); const auto href = fragment.charFormat().anchorHref();
        if (!href.isEmpty()) result[href] += fragment.text();
      }
    return result;
  };
  render();
  if (!browser() || !toggle(false) || !unknownToggle() || !toggle(true) ||
      !unknownToggle()->isCheckable() || !toggle(true)->isCheckable())
    return fail("the three independent Buff expansion controls are unavailable");
  auto* view = browser();
  if (view->property("memberCount").toInt() != 13 || view->property("groupCount").toInt() != 10 ||
      view->property("primaryGroupCount").toInt() != 3 || view->property("unknownGroupCount").toInt() != 7 ||
      view->property("unknownMemberCount").toInt() != 8 || view->property("playbackGroupCount").toInt() != 0 ||
      view->property("primaryEventCount").toInt() != 3 || view->property("unknownEventCount").toInt() != 5 ||
      view->property("playbackEventCount").toInt() != 1 ||
      view->property("sourceOwnCount").toInt() != 7 || view->property("sourceEnemyCount").toInt() != 1 ||
      view->property("sourceUnknownCount").toInt() != 5 || view->property("expanded").toBool() ||
      view->property("unknownExpanded").toBool() || view->property("playbackExpanded").toBool() ||
      view->property("shownGroupCount").toInt() != 0 || view->property("shownUnknownGroupCount").toInt() != 0)
    return fail("category partition lost original records, provenance, or collapsed defaults");
  auto links = anchors(view);
  if (!view->toPlainText().contains(QStringLiteral("晕眩")) || view->toPlainText().contains(unknownId) ||
      !links.value(QStringLiteral("round-effect:5:48")).contains(QStringLiteral("触发")) ||
      !links.value(QStringLiteral("round-effect:5:49")).contains(QStringLiteral("恢复显示")) ||
      links.contains(QStringLiteral("round-effect:3:41")) || links.contains(QStringLiteral("round-effect:5:43")) ||
      links.contains(QStringLiteral("round-effect:5:44")) || links.contains(QStringLiteral("round-effect:5:46")))
    return fail("collapsed main area lost actual trigger/display commands or exposed ordinary unexplained changes");
  const auto metersIntact = [&]() {
    auto* hp = card()->findChild<QProgressBar*>(QStringLiteral("hpBar"));
    auto* shield = card()->findChild<QProgressBar*>(QStringLiteral("shieldValue"));
    auto* power = card()->findChild<QProgressBar*>(QStringLiteral("powerBar"));
    return hp && shield && power && hp->text() == QStringLiteral("100/200") &&
      hp->accessibleName().contains(QStringLiteral("变化：-30")) && shield->text() == QStringLiteral("护盾 25") &&
      power->text() == QStringLiteral("60/100");
  };
  if (!metersIntact()) return fail("classifying the Buff also suppressed its independently recorded meter change");
  auto* outcomes = card()->findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (!outcomes || outcomes->property("effectKinds").toStringList().contains(QStringLiteral("buff-add")) ||
      outcomes->property("effectKinds").toStringList().contains(QStringLiteral("buff-remove")) ||
      !outcomes->property("effectKinds").toStringList().contains(QStringLiteral("buff-trigger")) ||
      !outcomes->property("effectKinds").toStringList().contains(QStringLiteral("reveal")))
    return fail("unknown membership animated as a state, or a recorded trigger/display command was swallowed");

  // Exercise the rendered partition, not just its helper: renaming every
  // official row and swapping all presentation metadata cannot move members.
  auto renamedDefinitions = definitions;
  for (const auto& id : holder.buffs) {
    if (!renamedDefinitions.contains(id)) continue;
    auto changed = renamedDefinitions[id].toObject();
    changed["nm"] = id == "8" ? QStringLiteral("合成任意动画") : QStringLiteral("合成无法行动状态");
    changed["did"] = "42383"; changed["hd"] = "1"; changed["once"] = "1";
    changed["bhs"] = QStringLiteral("烧伤"); renamedDefinitions[id] = changed;
  }
  catalog["buffs"] = renamedDefinitions; render(); view = browser();
  if (view->property("primaryGroupCount").toInt() != 3 || view->property("unknownGroupCount").toInt() != 7 ||
      view->property("unknownMemberCount").toInt() != 8 || view->property("groupCount").toInt() != 10 ||
      view->property("memberCount").toInt() != 13 || view->property("playbackGroupCount").toInt() != 0 ||
      view->property("primaryEventCount").toInt() != 3 || view->property("unknownEventCount").toInt() != 5 || !metersIntact())
    return fail("the UI repartitioned records by name, hidden/once flags, resource ID or harm-source text");
  catalog["buffs"] = definitions; render(); view = browser();

  toggle(false)->click();
  if (!view->property("expanded").toBool() || view->property("shownGroupCount").toInt() != 3 ||
      view->property("unknownExpanded").toBool() || view->property("playbackExpanded").toBool())
    return fail("explained expansion did not expose only its own collection");
  links = anchors(view);
  for (const auto& href : links.keys())
    if (href.startsWith(QStringLiteral("buff:42383#")) || href.startsWith(QStringLiteral("buff:42384#")) ||
        href.startsWith(QStringLiteral("buff:30098#")) || href.startsWith(QStringLiteral("buff:72899#")) ||
        href.startsWith(QStringLiteral("buff:") + unknownId + QLatin1Char('#')))
      return fail("explained collection exposed unexplained current members");
  if (!view->toPlainText().contains(QStringLiteral("记录 2 条")) || view->toPlainText().contains(QStringLiteral("2层")) ||
      view->toPlainText().contains(QStringLiteral("2 层")) || view->toPlainText().contains(QStringLiteral("隐藏附着动画")) ||
      view->toPlainText().contains(QStringLiteral("动画仅播一次"))) return fail("raw member multiplicity became layers or presentation notes stayed in the primary collection");
  unknownToggle()->click();
  if (!view->property("unknownExpanded").toBool() || !view->property("expanded").toBool() ||
      view->property("playbackExpanded").toBool() || view->property("shownGroupCount").toInt() != 3 ||
      view->property("shownUnknownGroupCount").toInt() != 7 || !metersIntact())
    return fail("unexplained expansion changed another drawer, current membership or exact meter values");
  links = anchors(view);
  int animatedOne = 0, animatedTwo = 0, blank = 0;
  bool repeatedSource = false, enemySource = false;
  for (auto it = links.cbegin(); it != links.cend(); ++it) {
    if (it.key().startsWith(QStringLiteral("buff:42383#"))) {
      ++animatedOne;
      repeatedSource |= it.value().contains(QStringLiteral("记录 2 条")) && it.value().contains(QStringLiteral("合成我方来源"));
      enemySource |= it.value().contains(QStringLiteral("合成敌方来源")) &&
        view->property("sourceEnemyCount").toInt() == 1 && view->accessibleName().contains(QStringLiteral("敌方施加"));
    }
    animatedTwo += it.key().startsWith(QStringLiteral("buff:42384#"));
    blank += it.key().startsWith(QStringLiteral("buff:30098#"));
  }
  if (animatedOne != 3 || animatedTwo != 1 || blank != 1 || !repeatedSource || !enemySource ||
      view->toPlainText().contains(unknownId) || !view->toPlainText().contains(QStringLiteral("效果名称未收录")) ||
      !links.keys().join(QLatin1Char('\n')).contains(QStringLiteral("buff:72899#")) ||
      view->toPlainText().contains(QStringLiteral("动画仅播一次")))
    return fail("unexplained drawer merged IDs/sources, lost registrations, or became a playback collection");
  toggle(true)->click();
  if (!view->property("playbackExpanded").toBool() || !view->property("expanded").toBool() ||
      !view->property("unknownExpanded").toBool() || view->property("shownGroupCount").toInt() != 3 ||
      view->property("shownUnknownGroupCount").toInt() != 7 || view->property("playbackGroupCount").toInt() != 0 ||
      view->property("playbackEventCount").toInt() != 1 || !metersIntact() ||
      !view->toPlainText().contains(QStringLiteral("隐藏附着动画")) || !view->toPlainText().contains(QStringLiteral("动画仅播一次")) ||
      !view->toPlainText().contains(QStringLiteral("原版隐藏生命飘字")) || !view->toPlainText().contains(QStringLiteral("原版隐藏气势飘字")))
    return fail("playback settings acquired members, changed another drawer, or omitted actual presentation evidence");
  links = anchors(view);
  QString publicText = view->toPlainText() + QLatin1Char('\n') + view->property("publicTipTexts").toStringList().join(QLatin1Char('\n'));
  for (auto* widget : cell.findChildren<QWidget*>()) publicText += QLatin1Char('\n') + widget->toolTip() + QLatin1Char('\n') + widget->accessibleName();
  for (const auto& privateText : {QStringLiteral("42383"), QStringLiteral("42384"), QStringLiteral("30098"), QStringLiteral("72899"),
      unknownId, ally, enemy, QStringLiteral("/synthetic/"), QStringLiteral("buffWaitType"), QStringLiteral("buffWaitEndFrame"), QStringLiteral("Buff ID")})
    if (publicText.contains(privateText)) return fail("normal cell text, tooltip or accessibility leaked a code, path or fighter key");
  const auto rawEvidence = QJsonDocument::fromVariant(view->property("rawEvidence")).toJson();
  if (!rawEvidence.contains("42383") || !rawEvidence.contains("/synthetic/source/41") ||
      !rawEvidence.contains("/synthetic/member/0") || !rawEvidence.contains(unknownId.toUtf8()))
    return fail("plain labels discarded their exact internal identities or source evidence");
  if (!view->toPlainText().contains(QStringLiteral("等待播放完成")) ||
      view->toPlainText().contains(QStringLiteral("播放等待类型")) || view->toPlainText().contains(QStringLiteral("回调帧")))
    return fail("playback details exposed technical parameters instead of their reading meaning");

  QList<QPair<QString, QString>> inspectedMembers;
  QObject::connect(&cell, &ReplayCell::buffActivated, &cell, [&](const QString& key, const QString& id) {
    inspectedMembers.append(qMakePair(key, id));
  });
  for (auto it = links.cbegin(); it != links.cend(); ++it) {
    if (!it.key().startsWith(QStringLiteral("buff:"))) continue;
    const auto id = it.key().mid(5).section(QLatin1Char('#'), 0, 0);
    if (!QMetaObject::invokeMethod(view, "anchorClicked", Q_ARG(QUrl, QUrl(it.key()))) || inspectedMembers.isEmpty() ||
        inspectedMembers.last() != qMakePair(holder.key, id))
      return fail("a plain member name no longer opens the exact effect identity");
  }
  if (inspectedMembers.size() < 10) return fail("source-group member detail links were dropped during visual cleanup");

  QList<QPair<int, int>> selectedRecords;
  QObject::connect(&cell, &ReplayCell::recordedEffectActivated, &cell,
    [&](int step, int index) { selectedRecords.append(qMakePair(step, index)); });
  QSet<QString> inspected;
  for (auto it = links.cbegin(); it != links.cend(); ++it) {
    if (it.key().startsWith(QStringLiteral("round-effect:"))) {
      const auto fields = it.key().split(QLatin1Char(':'));
      if (fields.size() != 3 || !QMetaObject::invokeMethod(view, "anchorClicked", Q_ARG(QUrl, QUrl(it.key()))) ||
          selectedRecords.isEmpty() || selectedRecords.last() != qMakePair(fields[1].toInt(), fields[2].toInt()))
        return fail("direct historical Buff record no longer selects its exact step/effect");
      inspected.insert(fields[1] + QLatin1Char(':') + fields[2]);
    } else if (it.key().startsWith(QStringLiteral("events:"))) {
      if (!QMetaObject::invokeMethod(view, "anchorClicked", Q_ARG(QUrl, QUrl(it.key()))))
        return fail("historical grouped records are not actionable");
      QCoreApplication::processEvents();
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      if (!menu || menu->objectName() != QStringLiteral("buffEventMenu") || menu->actions().isEmpty())
        return fail("historical grouped records lost their individual menu");
      for (auto* choice : menu->actions()) {
        const int index = choice->data().toInt(), step = choice->property("recordStep").toInt();
        choice->trigger();
        if (selectedRecords.isEmpty() || selectedRecords.last() != qMakePair(step, index))
          return fail("historical menu selected another record after category separation");
        if (choice->text().contains(QStringLiteral("42383")) || choice->toolTip().contains(QStringLiteral("/synthetic/")) ||
            choice->property("rawEvidence").toString().isEmpty())
          return fail("historical menu leaked raw identifiers or discarded its original evidence");
        inspected.insert(QStringLiteral("%1:%2").arg(step).arg(index));
      }
      menu->close();
    }
  }
  for (const auto& event : history)
    if (!inspected.contains(QStringLiteral("%1:%2").arg(event.recordStep).arg(event.index)))
      return fail("an explained/unexplained add, removal, trigger or display command cannot be individually selected");

  render(1); view = browser();
  if (!view->property("expanded").toBool() || !view->property("unknownExpanded").toBool() ||
      !view->property("playbackExpanded").toBool() ||
      view->property("sourceOwnCount").toInt() != 1 || view->property("sourceEnemyCount").toInt() != 7 ||
      view->property("sourceUnknownCount").toInt() != 5 || !metersIntact())
    return fail("perspective refresh changed category expansion or inferred unknown sources");
  cell.focusUnit(other.key); view = browser();
  if (!view || view->property("expanded").toBool() || view->property("unknownExpanded").toBool() ||
      view->property("playbackExpanded").toBool())
    return fail("same-name same-cell fighter inherited another full identity's expansion state");
  toggle(true)->click();
  if (view->property("expanded").toBool() || view->property("unknownExpanded").toBool() ||
      !view->property("playbackExpanded").toBool() || view->property("shownGroupCount").toInt() ||
      view->property("shownUnknownGroupCount").toInt() ||
      view->toPlainText().contains(QStringLiteral("记录 2 条")) || view->toPlainText().contains(QStringLiteral("合成我方来源")))
    return fail("playback settings also exposed or took ownership of current member registrations");
  unknownToggle()->click();
  if (view->property("expanded").toBool() || !view->property("unknownExpanded").toBool() ||
      !view->property("playbackExpanded").toBool() || view->property("shownUnknownGroupCount").toInt() != 7)
    return fail("the unexplained drawer is not independent of playback settings");
  cell.focusUnit(holder.key); view = browser(); toggle(false)->click();
  if (view->property("expanded").toBool() || !view->property("unknownExpanded").toBool() ||
      !view->property("playbackExpanded").toBool() || view->property("shownGroupCount").toInt() != 0 ||
      view->property("shownUnknownGroupCount").toInt() != 7) return fail("explained collapse also closed another drawer");
  cell.focusUnit(other.key); view = browser();
  if (!view->property("playbackExpanded").toBool() || !view->property("unknownExpanded").toBool() || view->property("expanded").toBool())
    return fail("changing one occupant changed the other occupant's independent controls");
  cell.resetBuffExpansion();
  for (auto* document : cell.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs")))
    if (document->property("expanded").toBool() || document->property("unknownExpanded").toBool() ||
        document->property("playbackExpanded").toBool() || document->property("shownGroupCount").toInt() != 0 ||
        document->property("shownUnknownGroupCount").toInt() != 0)
      return fail("session reset did not collapse all three controls for every identity");
  cue.events = {stateEvent, unknownEvent, numeric}; render();
  outcomes = card()->findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (!outcomes || !outcomes->property("effectKinds").toStringList().contains(QStringLiteral("status-stun")) ||
      outcomes->property("effectKinds").toStringList().contains(QStringLiteral("buff-add")) ||
      outcomes->property("effectKinds").toStringList().contains(QStringLiteral("buff-trigger")) || !metersIntact())
    return fail("an unexplained registration became a large state animation, or a historical trigger replayed in another phase");
  if (holder.buffs != originalHolder.buffs || recordedBuffMembers(holder).size() != originalHolder.buffMembers.size() ||
      holder.hp != originalHolder.hp || holder.shield != originalHolder.shield || holder.power != originalHolder.power)
    return fail("presentation filtering mutated original fighter observations");

  // Both auxiliary drawers can become empty in a later observation. Their
  // hidden controls must not keep empty content or a compact outcome open.
  toggle(true)->click(); unknownToggle()->click();
  if (!browser()->property("playbackExpanded").toBool() || !browser()->property("unknownExpanded").toBool())
    return fail("lifecycle fixture did not open the two auxiliary drawers");
  holder.buffs = {QStringLiteral("8"), QStringLiteral("8")};
  holder.buffMembers = {{"8", ally, QStringLiteral("合成我方来源"), QStringLiteral("/synthetic/later/0"), 0},
    {"8", ally, QStringLiteral("合成我方来源"), QStringLiteral("/synthetic/later/1"), 0}};
  cue.events = {stateEvent, numeric}; cue.roundBuffEvents = QList<ReplayUnitEvent>{stateEvent};
  render(); view = browser(); outcomes = card()->findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (view->property("playbackExpanded").toBool() || view->property("unknownExpanded").toBool() ||
      toggle(true)->isChecked() || !toggle(true)->isHidden() || unknownToggle()->isChecked() || !unknownToggle()->isHidden() ||
      view->toPlainText().contains(QStringLiteral("播放详情")) || !outcomes || outcomes->property("compact").toBool())
    return fail("disappearing auxiliary details retained an inaccessible drawer or compact outcome");
  holder = originalHolder; render();
  if (browser()->property("playbackExpanded").toBool() || browser()->property("unknownExpanded").toBool() ||
      toggle(true)->isChecked() || toggle(true)->isHidden() || unknownToggle()->isChecked() || unknownToggle()->isHidden())
    return fail("returning registrations/settings reopened preferences cleared when their drawers disappeared");

  // Integration uses parsed protocol records, not a preclassified UI model.
  auto document = QJsonDocument::fromJson(realtimeMultiUnitFixture()).object();
  auto battle = document["br"].toObject(); auto segment = battle["fr"].toArray()[0].toObject();
  auto first = segment["rl"].toArray()[1].toObject(); first["csn"] = 1;
  first["cn"] = QJsonObject{{"bd_bfs", "1_42383_1,1_42383_1,1_30098_1,1_8_1"}, {"bb_hps", "42383_-10_2"}, {"ahp", 90}};
  first["trl"] = QJsonArray{};
  auto second = first; second["cn"] = QJsonObject{{"ad_bfs", "0_42383_1"}, {"ahp", 90}};
  segment["rl"] = QJsonArray{first, second}; battle["fr"] = QJsonArray{segment}; document["br"] = battle;
  const auto bytes = QJsonDocument(document).toJson();
  QTemporaryFile file;
  if (!file.open() || file.write(bytes) != bytes.size() || !file.flush()) return fail("integration fixture file unavailable");
  BattleReplayPage page(nullptr, previewBattleReplayCatalog());
  if (!page.loadFile(file.fileName())) return fail("integration fixture parse failed");
  QList<int> actions;
  for (int i = 0; i < page.replay().steps.size(); ++i)
    if (ReplayTimeline::isAction(page.replay().steps[i])) actions.append(i);
  if (actions.size() != 2) return fail("integration actions are missing");
  const auto key = fighterKey(1, "tester", "7");
  const auto pageView = [&]() -> QTextBrowser* {
    for (auto* candidate : page.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs")))
      if (candidate->property("fighterKey").toString() == key) return candidate;
    return nullptr;
  };
  page.seek(actions[0]);
  const auto expected = page.displayedState();
  if (expected.value(key).hp != 90 || expected.value(key).buffs.count(QStringLiteral("42383")) != 2 ||
      expected.value(key).buffs.count(QStringLiteral("8")) != 1 || expected.value(key).buffs.count(QStringLiteral("30098")) != 1 || !pageView())
    return fail("officially parsed members or independent numeric record were not retained");
  if (pageView()->property("primaryEventCount").toInt() != 2 || pageView()->property("unknownEventCount").toInt() != 3 ||
      pageView()->property("playbackGroupCount").toInt() != 0 || pageView()->property("playbackEventCount").toInt() != 0 ||
      !pageView()->toPlainText().contains(QStringLiteral("触发")) || !pageView()->toPlainText().contains(QStringLiteral("晕眩")))
    return fail("parsed unknown numeric trigger was folded away or wait configuration became an executed display command");
  const auto immutableCheck = [&]() {
    return page.replay().original == bytes && replayStateMatches(page, expected);
  };
  for (const auto& name : {QStringLiteral("unitBuffToggle"), QStringLiteral("unitBuffUnknownToggle"), QStringLiteral("unitBuffPlaybackToggle")}) {
    auto* button = pageView()->parentWidget()->findChild<QToolButton*>(name);
    if (!button || button->isHidden()) return fail("integrated expansion control is missing");
    button->click();
    if (!immutableCheck()) return fail("expanding an integrated category changed the replay state or source bytes");
  }
  QString actualMemberLink;
  const auto currentMemberLinks = anchors(pageView());
  for (const auto& href : currentMemberLinks.keys())
    if (href.startsWith(QStringLiteral("buff:42383#"))) { actualMemberLink = href; break; }
  if (actualMemberLink.isEmpty() || !QMetaObject::invokeMethod(pageView(), "anchorClicked", Q_ARG(QUrl, QUrl(actualMemberLink))))
    return fail("the public member name cannot open its original Buff detail");
  auto* boardDetails = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  auto* unitDetails = page.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
  if (!boardDetails || !unitDetails || boardDetails->toPlainText().contains(QStringLiteral("42383")) ||
      !anchors(boardDetails).contains(QStringLiteral("buff-raw:42383")) ||
      unitDetails->property("buffHistoryId").toString() != QStringLiteral("42383") || !immutableCheck())
    return fail("public Buff detail leaked the ID or hid the precise raw-detail entry");
  if (!QMetaObject::invokeMethod(boardDetails, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("buff-raw:42383")))) ||
      !boardDetails->toPlainText().contains(QStringLiteral("42383")) || !boardDetails->toPlainText().contains(QStringLiteral("/br/")) ||
      !immutableCheck()) return fail("explicit raw detail lost the original Buff identity/path or changed the replay");
  if (!QMetaObject::invokeMethod(boardDetails, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("buff:42383")))) ||
      boardDetails->toPlainText().contains(QStringLiteral("42383"))) return fail("returning to the readable detail retained raw IDs");
  const auto& firstStep = page.replay().steps[actions[0]];
  int addedIndex = -1;
  for (int i = 0; i < firstStep.effects.size(); ++i)
    if (firstStep.effects[i].kind == QStringLiteral("buff") && firstStep.effects[i].causeId == QStringLiteral("42383")) {
      addedIndex = i; break;
    }
  if (addedIndex < 0) return fail("parsed unexplained registration was erased from the domain effects");
  page.seek(actions[1]);
  if (page.displayedState().value(key).buffs.count(QStringLiteral("42383")) != 1 ||
      !pageView()->property("unknownExpanded").toBool() || !pageView()->property("playbackExpanded").toBool())
    return fail("later removal or independent unknown/playback expansion state was lost");
  const auto historyHref = QStringLiteral("round-effect:%1:%2").arg(actions[0]).arg(addedIndex);
  const auto historyLinks = anchors(pageView());
  bool activatedHistory = false;
  if (historyLinks.contains(historyHref)) {
    activatedHistory = QMetaObject::invokeMethod(pageView(), "anchorClicked", Q_ARG(QUrl, QUrl(historyHref)));
  } else {
    // Round notices are newest first. A grouped row can link the newer add
    // directly while the earliest original record remains in its real menu.
    // Follow only existing anchors; never manufacture an unreachable URL.
    for (auto it = historyLinks.cbegin(); it != historyLinks.cend(); ++it) {
      if (!it.key().startsWith(QStringLiteral("events:"))) continue;
      if (!QMetaObject::invokeMethod(pageView(), "anchorClicked", Q_ARG(QUrl, QUrl(it.key()))))
        return fail("folded historical group cannot be opened");
      auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
      if (!menu || menu->objectName() != QStringLiteral("buffEventMenu"))
        return fail("folded historical group did not expose its actual menu");
      for (auto* choice : menu->actions()) {
        if (choice->data().toInt() != addedIndex || choice->property("recordStep").toInt() != actions[0]) continue;
        choice->trigger(); activatedHistory = true; break;
      }
      // The triggered seek is deferred. Close the popup before processing
      // that callback, which rebuilds and destroys the old fighter card.
      menu->close();
      if (activatedHistory) break;
    }
  }
  if (!activatedHistory) return fail("folded earliest original add is absent from both real links and grouped menus");
  // Page defers the seek so rendering cannot delete the clicked browser while
  // its signal is still being dispatched. Inspect after that callback runs.
  QCoreApplication::processEvents();
  if (page.currentStep() != actions[0] || page.currentMoment() != firstStep.effects[addedIndex].moment ||
      page.replay().original != bytes)
    return fail("folded original add record no longer jumps to its real action and command");

  // The official waiting-Buff callback restores child display layers only.
  // Use real protocol fields so UI hints cannot manufacture entry or recovery.
  struct RestoreCase {
    const char* label; bool summoned, clone, called; std::optional<qint64> hp;
    int operation; bool configured, expectedRestore;
  };
  const QList<RestoreCase> restoreCases{
    {"waiting summon at zero HP", true, false, false, 0, 1, true, true},
    {"waiting summon with unknown HP", true, false, false, std::nullopt, 1, true, true},
    {"called clone at zero HP", false, true, true, 0, 1, true, true},
    {"uncalled clone", false, true, false, 0, 1, true, false},
    {"ordinary unit", false, false, false, 0, 1, true, false},
    {"removal", true, false, false, 0, 0, true, false},
    {"configuration unavailable", true, false, false, 0, 1, false, false},
    {"initial membership only", true, false, false, 0, -1, true, false}};
  for (int caseIndex = 0; caseIndex < restoreCases.size(); ++caseIndex) {
    const auto& sample = restoreCases[caseIndex];
    auto restoreDocument = QJsonDocument::fromJson(battleReplayStateFixture()).object();
    auto restoreBattle = restoreDocument["br"].toObject(); auto original = restoreBattle["or"].toObject();
    auto teams = original["team"].toArray(); auto targetTeam = teams[1].toObject();
    auto players = targetTeam["players"].toArray(); auto targetPlayer = players[0].toObject();
    auto fighters = targetPlayer["cts"].toArray(); auto target = fighters[0].toObject();
    target["ics"] = sample.summoned; target["ias"] = sample.clone;
    target["ibs"] = sample.called; target["is"] = false;
    target["bfs"] = sample.operation <= 0 ? QStringLiteral("30098") : QString();
    if (sample.hp) target["hp"] = static_cast<double>(*sample.hp); else target.remove("hp");
    fighters[0] = target; targetPlayer["cts"] = fighters; players[0] = targetPlayer;
    targetTeam["players"] = players; teams[1] = targetTeam; original["team"] = teams; restoreBattle["or"] = original;
    QJsonObject numbers;
    if (sample.operation >= 0) numbers["ad_bfs"] = QStringLiteral("%1_30098_1").arg(sample.operation);
    const QJsonObject action{{"cti", 0}, {"cpn", "tester"}, {"orrt", 2}, {"epbi", 200195}, {"csn", 1}, {"pr", 1},
      {"cn", QJsonObject{}}, {"prl", QJsonArray{}},
      {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", false}, {"tn", numbers}}}}};
    restoreBattle["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{action}}}};
    restoreDocument["br"] = restoreBattle;
    auto caseCatalog = official; caseCatalog["bufferExecution"] = QJsonObject{};
    if (!sample.configured) {
      auto caseBuffs = caseCatalog["buffs"].toObject(); caseBuffs.remove("30098"); caseCatalog["buffs"] = caseBuffs;
    }
    const auto restoreBytes = QJsonDocument(restoreDocument).toJson(); const auto replay = BattleReplay::parse(restoreBytes, caseCatalog);
    if (!replay.error.isEmpty() || replay.steps.isEmpty() || !replay.steps.first().state.contains(key))
      return fail("restore-display boundary fixture did not parse");
    const auto before = replay.steps.first().state.value(key);
    int actionIndex = -1, restorationCount = 0, membershipChanges = 0;
    const auto stablePresence = [&](const QMap<QString, Fighter>& state) {
      const auto observed = state.value(key);
      return observed.hp == before.hp && observed.entered == before.entered && observed.entryKnown == before.entryKnown &&
        observed.cloneCalled == before.cloneCalled;
    };
    for (int i = 0; i < replay.steps.size(); ++i) {
      const auto& step = replay.steps[i]; if (!ReplayTimeline::isAction(step)) continue;
      actionIndex = i;
      if (step.playbackKind != QStringLiteral("team-simple") || !stablePresence(step.state)) {
        qCritical() << sample.label; return fail("display restoration changed entry/HP or an unmapped execution Buff lost its action");
      }
      for (const auto& moment : step.moments)
        if (!stablePresence(moment.state)) { qCritical() << sample.label; return fail("display callback invented a transient entry or recovery"); }
      for (const auto& effect : step.effects) {
        if (effect.target != key) continue;
        restorationCount += effect.restoresDisplayLayers;
        membershipChanges += effect.kind == QStringLiteral("buff");
        if (effect.kind == QStringLiteral("appearance") || effect.metric == QStringLiteral("生命"))
          return fail("display-only callback emitted a new-entry or life-change event");
      }
    }
    if (actionIndex < 0 || restorationCount != int(sample.expectedRestore) ||
        membershipChanges != int(sample.operation >= 0) || replay.original != restoreBytes) {
      qCritical() << sample.label; return fail("display-command guard, membership record or original bytes were lost");
    }
    const int expectedMembers = sample.operation == 0 ? 0 : 1;
    if (replay.steps[actionIndex].state.value(key).buffs.count(QStringLiteral("30098")) != expectedMembers)
      return fail("display restoration replaced the actual Buff registration/removal");
    if (caseIndex == 0) {
      if (before.hp != 0 || before.entered) return fail("waiting zero-HP restore fixture was not initially pending");
      QTemporaryFile restoreFile;
      if (!restoreFile.open() || restoreFile.write(restoreBytes) != restoreBytes.size() || !restoreFile.flush()) return false;
      auto snapshot = std::make_shared<BattleReplayCatalogSnapshot>(); snapshot->root = caseCatalog; snapshot->revision = 1;
      BattleReplayPage restorePage(nullptr, snapshot);
      if (!restorePage.loadFile(restoreFile.fileName())) return fail("restore display UI fixture did not load");
      restorePage.seek(actionIndex);
      QTextBrowser* restoreView = nullptr; QWidget* restoreOutcome = nullptr;
      for (auto* candidate : restorePage.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs")))
        if (candidate->property("fighterKey").toString() == key) restoreView = candidate;
      for (auto* candidate : restorePage.findChildren<QWidget*>(QStringLiteral("unitOutcomeOverlay")))
        if (candidate->property("fighterKey").toString() == key) restoreOutcome = candidate;
      const auto kinds = restoreOutcome ? restoreOutcome->property("effectKinds").toStringList() : QStringList{};
      auto* title = restorePage.findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
      if (!restoreView || !restoreView->toPlainText().contains(QStringLiteral("恢复显示")) ||
          restoreView->property("primaryEventCount").toInt() != 1 || restoreView->property("unknownEventCount").toInt() != 0 ||
          restoreView->property("unknownMemberCount").toInt() != 1 || restoreView->property("playbackEventCount").toInt() != 1 ||
          !kinds.contains(QStringLiteral("reveal")) || kinds.contains(QStringLiteral("entry")) ||
          kinds.contains(QStringLiteral("summon")) || kinds.contains(QStringLiteral("recovery")) || kinds.contains(QStringLiteral("buff-add")) ||
          !title || title->property("buffId").toString() != QStringLiteral("200195") ||
          !stablePresence(restorePage.displayedState()) || restorePage.replay().original != restoreBytes)
        return fail("real display command was folded away, renamed by a movie ID or shown as entry/recovery");
    }
  }
  return true;
}
