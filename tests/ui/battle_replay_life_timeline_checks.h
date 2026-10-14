#pragma once

#include "ui/battle/battle_replay_life_timeline.h"
#include "ui/battle/battle_replay_page.h"
#include "ui/battle/battle_replay_timeline_events.h"
#include "battle_replay_preview_catalog.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QTemporaryFile>
#include <QTest>
#include <QToolButton>
#include <QVariantAnimation>
#include <algorithm>

QByteArray realtimeMultiUnitFixture();

inline bool verifyReplayLifeTimeline() {
  using namespace BattleReplay;
  const auto fail = [](const char* why) { qCritical() << "life timeline:" << why; return false; };
  // Values here are explicit test observations. They exercise life-event
  // navigation without calculating damage or inventing a resurrection cause.
  auto root = QJsonDocument::fromJson(realtimeMultiUnitFixture()).object();
  auto battle = root["br"].toObject();
  auto block = battle["fr"].toArray()[0].toObject();
  const auto base = block["rl"].toArray()[1].toObject();
  const auto action = [&](int team, int round, const QString& numbers, int finalHp) {
    auto result = base;
    result["cti"] = team;
    if (round >= 0) result["csn"] = round; else result.remove("csn");
    result["cn"] = QJsonObject{{"att_hps", numbers}, {"ahp", finalHp}};
    result["trl"] = QJsonArray{QJsonObject{{"tti", 1 - team}, {"tpn", "tester"}, {"tci", 7},
      {"isse", true}, {"ishit", true}, {"tn", QJsonObject{{"att_hps", "0"}}}}};
    return result;
  };
  const auto pulse = QStringLiteral("-100,100,-100,100");
  auto firstAction = action(1, 4, pulse, 100);
  auto firstNumbers = firstAction["cn"].toObject();
  firstNumbers["att_vgs"] = "5";
  firstNumbers["ad_bfs"] = "1_500073_1";
  firstAction["cn"] = firstNumbers;
  block["rl"] = QJsonArray{firstAction, action(1, 5, pulse, 100)};
  auto continuation = block;
  continuation["rl"] = QJsonArray{action(0, 5, QStringLiteral("-100"), 0),
    action(1, -1, pulse, 100), action(1, 4, pulse, 100), action(1, -1, pulse, 100)};
  // An empty segment and the next segment header are real non-action Steps.
  // They neither consume small-turn numbers nor split the ongoing csn 5.
  auto emptySegment = block; emptySegment["rl"] = QJsonArray{};
  battle["fr"] = QJsonArray{block, emptySegment, continuation};

  // Include event-free fighters and identical names/cells with different
  // full identities. Enough rows force real scrolling at the preview size.
  auto opening = battle["or"].toObject();
  auto teams = opening["team"].toArray();
  for (int teamIndex = 0; teamIndex < teams.size(); ++teamIndex) {
    auto team = teams[teamIndex].toObject(); auto players = team["players"].toArray();
    auto player = players[0].toObject(); auto fighters = player["cts"].toArray();
    for (int i = 0; i < fighters.size(); ++i) {
      auto fighter = fighters[i].toObject(); fighter["name"] = "synthetic duplicate";
      fighters[i] = fighter;
    }
    for (int i = 0; i < 18; ++i)
      fighters.append(QJsonObject{{"id", 100 + i}, {"name", "synthetic duplicate"},
        {"x", i % 3}, {"y", i / 3 % 3}, {"hp", 100}, {"maxHp", 100},
        {"shp", 0}, {"vigour", 60}});
    player["cts"] = fighters; players[0] = player; team["players"] = players; teams[teamIndex] = team;
  }
  opening["team"] = teams; battle["or"] = opening; root["br"] = battle;
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file;
  if (!file.open() || file.write(bytes) != bytes.size() || !file.flush()) return fail("fixture file unavailable");

  BattleReplayPage page(nullptr, previewBattleReplayCatalog());
  page.resize(1440, 980);
  if (!page.loadFile(file.fileName())) return fail("fixture could not be parsed");
  page.show(); QCoreApplication::processEvents();
  auto* timeline = page.findChild<ReplayLifeTimeline*>("replaySlider");
  auto* side = page.findChild<QComboBox*>("replayOwnTeam");
  auto* rounds = page.findChild<QComboBox*>("replayRounds");
  auto* mode = page.findChild<QComboBox*>("replayTimelineView");
  auto* counter = page.findChild<QLabel*>("replaySmallTurn");
  auto* play = page.findChild<QPushButton*>("replayPlay");
  auto* clock = page.findChild<QVariantAnimation*>("replayPhaseAnimation");
  auto* scroll = page.findChild<QScrollBar*>("replayUnitTimelineScroll");
  if (!timeline || !side || !rounds || !mode || !counter || !play || !clock || !scroll)
    return fail("integrated timeline controls are missing");
  const int roundMode = mode->findData(QStringLiteral("round")), unitMode = mode->findData(QStringLiteral("units"));
  if (roundMode < 0 || unitMode < 0 || mode->currentIndex() != roundMode || timeline->unitView())
    return fail("round overview is not the default or the unit view is unavailable");

  QList<int> actions;
  int counted = 0;
  for (int i = 0; i < page.replay().steps.size(); ++i) {
    if (ReplayTimeline::isAction(page.replay().steps[i])) { actions.append(i); ++counted; }
    if (timeline->turnAt(i) != counted) return fail("a segment, opening or ending consumed a small turn");
  }
  if (actions.size() != 6 || timeline->turnCount() != 6 || page.replay().steps.size() != 11)
    return fail("small-turn count disagrees with six actions and three segment headers");
  const QList<int> expectedRounds{4, 5, 5, -1, 4, -1};
  const QList<int> expectedLocalTurns{1, 1, 2, 1, 1, 1};
  const QList<int> expectedRoundGroups{0, 1, 1, 2, 3, 4};
  for (int i = 0; i < actions.size(); ++i)
    if (timeline->roundTurnAt(actions[i]) != expectedLocalTurns[i] ||
        timeline->roundNumberAt(actions[i]) != expectedRounds[i])
      return fail("round-local numbering leaked across a round or counted a segment");

  const auto source = fighterKey(1, "tester", "7"), other = fighterKey(0, "tester", "7");
  const auto sameNameCell = fighterKey(0, "tester", "8");
  const auto& markers = timeline->markers();
  if (markers.size() != 21) {
    qCritical() << "actual life marker count" << markers.size();
    return fail("life crossings were lost or attack, power or Buff events became life markers");
  }
  bool hasBuff = false, hasPower = false, hasTarget = false;
  for (const auto& effect : page.replay().steps[actions[0]].effects) {
    hasBuff |= effect.kind == QStringLiteral("buff");
    hasPower |= effect.metric == QStringLiteral("气势");
    hasTarget |= effect.kind == QStringLiteral("target");
  }
  if (!hasBuff || !hasPower || !hasTarget) return fail("fixture did not retain independent non-life effects");
  QMap<int, QList<int>> markersAtAction;
  int deathMarker = -1;
  for (int i = 0; i < markers.size(); ++i) {
    const auto& marker = markers[i];
    const int actionIndex = actions.indexOf(marker.step);
    if (actionIndex < 0 || marker.turn != actionIndex + 1 || marker.round != expectedRounds[actionIndex] ||
        marker.roundTurn != expectedLocalTurns[actionIndex] || marker.roundIndex != expectedRoundGroups[actionIndex] ||
        marker.name.isEmpty() || (marker.label != QStringLiteral("死亡") && marker.label != QStringLiteral("复活")))
      return fail("marker identity, scope, local turn or life-only label is incorrect");
    markersAtAction[actionIndex].append(i);
    if (actionIndex == 2) {
      if (deathMarker >= 0 || marker.key != other || marker.team != 0 || marker.recovery)
        return fail("the other-side death was duplicated or misidentified");
      deathMarker = i;
    }
  }
  for (int actionIndex : {0, 1, 3, 4, 5}) {
    const auto indices = markersAtAction.value(actionIndex);
    if (indices.size() != 4) return fail("same-command repeated death/recovery was collapsed");
    for (int i = 0; i < indices.size(); ++i) {
      const auto& marker = markers[indices[i]];
      if (marker.key != source || marker.team != 1 || marker.recovery != (i % 2 == 1))
        return fail("same-command life crossing order or full fighter key changed");
    }
  }
  if (deathMarker < 0) return fail("ordinary unsplit death marker is missing");

  const auto atActionStart = [&](int step, const QString& key, qint64 hp) {
    return page.currentStep() == step && page.currentMoment() == 0 && page.currentPlaybackPhase() == 0 &&
      page.playbackPhaseKind() == QStringLiteral("prepare") && page.displayedState().value(key).hp == hp &&
      !page.property("playbackRunning").toBool();
  };
  const auto onlyRoundVisible = [&](int group) {
    if (timeline->activeRoundIndex() != group) return false;
    for (int i = 0; i < markers.size(); ++i)
      if ((!timeline->markerRect(i).isEmpty()) != (markers[i].roundIndex == group)) return false;
    return true;
  };
  const QList<int> groupFirstActions{0, 1, 3, 4, 5};
  for (int group = 0; group < groupFirstActions.size(); ++group) {
    auto* button = timeline->roundButton(group);
    if (!button) return fail("a repeated or unknown round was merged out of the header");
    button->click();
    const int actionIndex = groupFirstActions[group];
    if (!atActionStart(actions[actionIndex], source, 100) || !onlyRoundVisible(group) ||
        counter->property("smallTurn").toInt() != 1 ||
        counter->property("globalTurn").toInt() != actionIndex + 1 ||
        counter->property("round").toInt() != expectedRounds[actionIndex])
      return fail("round button failed to seek its first action, pause, or refresh round-local labels");
  }
  if (timeline->roundButton(groupFirstActions.size())) return fail("non-action segments created extra rounds");

  // Completing the current action through the real playback callback must
  // automatically expose the next round, with no click or mode change.
  page.seek(actions[0]); page.seekPlaybackPhase(page.playbackPhaseCount() - 1); play->click();
  clock->setCurrentTime(clock->duration());
  if (page.currentStep() != actions[1] || !page.property("playbackRunning").toBool() ||
      !onlyRoundVisible(1) || counter->property("smallTurn").toInt() != 1)
    return fail("continuous playback did not follow the next round and reset the local turn");
  page.seekPlaybackPhase(0);

  const int zeroSide = side->findData(0), oneSide = side->findData(1);
  if (zeroSide < 0 || oneSide < 0) return fail("fixture sides are unavailable");
  side->setCurrentIndex(zeroSide);
  const int enemyMarker = markersAtAction.value(1).first();
  const auto enemyBefore = timeline->markerRect(enemyMarker), ownBefore = timeline->markerRect(deathMarker);
  if (enemyBefore.isEmpty() || ownBefore.isEmpty() || enemyBefore.center().y() <= ownBefore.center().y())
    return fail("overview does not show the current round in distinct my/enemy lanes");
  side->setCurrentIndex(oneSide);
  if (timeline->markerRect(enemyMarker).center().y() != ownBefore.center().y() ||
      timeline->markerRect(deathMarker).center().y() != enemyBefore.center().y())
    return fail("perspective failed to exchange overview lanes");

  // Switching presentation preserves a selected intermediate life balance
  // both while paused and while the playback clock is running.
  int pulsePhase = -1;
  for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
    page.seekPlaybackPhase(phase);
    if (page.property("numericSegmentCount").toInt() >= 4 && page.displayedState().value(source).hp == 0) {
      pulsePhase = phase; break;
    }
  }
  if (pulsePhase < 0) return fail("intermediate life crossing phase is unavailable");
  const int savedStep = page.currentStep(), savedMoment = page.currentMoment();
  const auto locationPreserved = [&](bool running) {
    return page.currentStep() == savedStep && page.currentMoment() == savedMoment &&
      page.currentPlaybackPhase() == pulsePhase && page.displayedState().value(source).hp == 0 &&
      page.property("playbackRunning").toBool() == running;
  };
  mode->setCurrentIndex(unitMode);
  if (!timeline->unitView() || !locationPreserved(false)) return fail("unit view changed the paused playback position");
  mode->setCurrentIndex(roundMode);
  if (timeline->unitView() || !locationPreserved(false)) return fail("round view changed the paused playback position");
  play->click();
  if (!locationPreserved(true)) return fail("fixture failed to resume its selected numeric phase");
  mode->setCurrentIndex(unitMode);
  if (!timeline->unitView() || !locationPreserved(true)) return fail("unit view paused or reset running playback");
  mode->setCurrentIndex(roundMode);
  if (timeline->unitView() || !locationPreserved(true)) return fail("round view paused or reset running playback");
  page.seekPlaybackPhase(pulsePhase); mode->setCurrentIndex(unitMode);
  QCoreApplication::processEvents();

  QSet<QString> expectedUnits;
  const auto& initial = page.replay().steps.first().state;
  for (const auto& fighter : initial) if (!fighter.representer) expectedUnits.insert(fighter.key);
  const auto allUnitsPresent = [&] {
    QSet<QString> actual;
    for (const auto& key : timeline->unitKeys()) actual.insert(key);
    return actual == expectedUnits && actual.size() == timeline->unitKeys().size();
  };
  if (!allUnitsPresent() || !timeline->unitKeys().contains(other) || !timeline->unitKeys().contains(sameNameCell) ||
      timeline->unitKeys().indexOf(other) == timeline->unitKeys().indexOf(sameNameCell))
    return fail("unit rows omitted event-free fighters or merged identical names/cells");
  const auto ownTeamFirst = [&](int team) {
    bool enemySeen = false;
    for (const auto& key : timeline->unitKeys()) {
      if (initial.value(key).team != team) enemySeen = true;
      else if (enemySeen) return false;
    }
    return enemySeen;
  };
  side->setCurrentIndex(zeroSide); scroll->setValue(0);
  if (!allUnitsPresent() || !ownTeamFirst(0)) return fail("my units are not first in unit mode");
  side->setCurrentIndex(oneSide); scroll->setValue(0);
  if (!allUnitsPresent() || !ownTeamFirst(1)) return fail("perspective failed to reorder unit rows by full identity");
  if (scroll->maximum() <= 0) return fail("long unit list is clipped without a usable scroll range");
  QSet<QString> reached;
  bool hidAnOffscreenMarker = false;
  for (int position = 0;; position = std::min(scroll->maximum(), position + std::max(1, scroll->pageStep() / 2))) {
    scroll->setValue(position);
    for (const auto& key : timeline->unitKeys()) if (!timeline->unitRowRect(key).isEmpty()) reached.insert(key);
    for (int i = 0; i < markers.size(); ++i) {
      if (markers[i].roundIndex != 1) {
        if (!timeline->markerRect(i).isEmpty()) return fail("unit mode displays a different round's event");
      } else if (timeline->unitRowRect(markers[i].key).isEmpty()) {
        hidAnOffscreenMarker = true;
        if (!timeline->markerRect(i).isEmpty()) return fail("an offscreen unit has a stray clickable event marker");
      }
    }
    if (position == scroll->maximum()) break;
  }
  if (reached != expectedUnits || !hidAnOffscreenMarker) return fail("scrolling cannot reach all unit identities or hide offscreen markers");
  const auto locateMarker = [&](int index) {
    for (int position = 0;; position = std::min(scroll->maximum(), position + std::max(1, scroll->pageStep() / 2))) {
      scroll->setValue(position);
      const auto rect = timeline->markerRect(index);
      if (!rect.isEmpty()) return rect;
      if (position == scroll->maximum()) return QRect{};
    }
  };
  const auto clickDeath = [&] {
    const auto rect = locateMarker(deathMarker);
    if (rect.isEmpty()) return false;
    QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, rect.center());
    return true;
  };
  page.seek(actions[2]); page.seekPlaybackPhase(page.playbackPhaseCount() - 1);
  if (page.displayedState().value(other).hp != 0) return fail("death fixture did not end at zero HP");
  play->click();
  if (!clickDeath() || !atActionStart(actions[2], other, 100))
    return fail("current unit event did not return to its action's initial HP and pause");
  if (counter->property("smallTurn").toInt() != 2 || counter->property("globalTurn").toInt() != 3 ||
      counter->property("round").toInt() != 5)
    return fail("unit-event seeking did not synchronize local and global turn labels");
  page.seek(actions[1]); play->click();
  if (!clickDeath() || !atActionStart(actions[2], other, 100))
    return fail("another unit event sought the wrong action or kept playback running");

  timeline->setValue(actions[3]);
  if (!atActionStart(actions[3], source, 100) || timeline->activeRoundIndex() != 2 ||
      counter->property("smallTurn").toInt() != 1 || counter->property("globalTurn").toInt() != 4 ||
      counter->property("round").toInt() != -1 || rounds->currentIndex() != -1 ||
      !rounds->placeholderText().contains(QStringLiteral("未记录")))
    return fail("unknown csn inherited the preceding round or lost its local turn");

  // A new log rebuilds round/unit indices and clears the scroll offset even
  // when the current display preference remains the per-unit view.
  scroll->setValue(scroll->maximum());
  auto replacementRoot = QJsonDocument::fromJson(realtimeMultiUnitFixture()).object();
  auto replacementBattle = replacementRoot["br"].toObject();
  auto replacementBlock = replacementBattle["fr"].toArray()[0].toObject();
  replacementBlock["rl"] = QJsonArray{action(1, 9, QStringLiteral("0"), 100)};
  replacementBattle["fr"] = QJsonArray{replacementBlock}; replacementRoot["br"] = replacementBattle;
  const auto replacement = QJsonDocument(replacementRoot).toJson();
  if (!file.resize(0) || !file.seek(0) || file.write(replacement) != replacement.size() || !file.flush() ||
      !page.loadFile(file.fileName())) return fail("replacement fixture could not be loaded");
  QCoreApplication::processEvents();
  if (!timeline->markers().isEmpty() || timeline->turnCount() != 1 ||
      timeline->property("lifeEventCount").toInt() != 0 || counter->property("smallTurn").toInt() != 0 ||
      counter->property("globalTurn").toInt() != 0 || scroll->value() != 0 || timeline->roundButton(1))
    return fail("new log retained old events, rounds, turn labels or scroll offset");
  if (timeline->unitKeys().size() != 3 || page.replay().original != replacement)
    return fail("new log retained old unit rows or changed the original bytes");

  const auto withActions = [](const QJsonArray& records) {
    auto document = QJsonDocument::fromJson(realtimeMultiUnitFixture()).object();
    auto payload = document["br"].toObject(); auto segment = payload["fr"].toArray()[0].toObject();
    segment["rl"] = records; payload["fr"] = QJsonArray{segment}; document["br"] = payload;
    return QJsonDocument(document).toJson();
  };
  // The legacy round dropdown shares the contiguous-round interpretation.
  // Ignoring the unknown action here would merge the two distinct csn 4s.
  const auto repeatedRoundBytes = withActions(QJsonArray{action(1, 4, pulse, 100),
    action(1, -1, pulse, 100), action(1, 4, pulse, 100)});
  if (!file.resize(0) || !file.seek(0) || file.write(repeatedRoundBytes) != repeatedRoundBytes.size() ||
      !file.flush() || !page.loadFile(file.fileName())) return fail("repeated-round fixture could not be loaded");
  QList<int> repeatedActions, roundFourChoices;
  for (int i = 0; i < page.replay().steps.size(); ++i)
    if (ReplayTimeline::isAction(page.replay().steps[i])) repeatedActions.append(i);
  for (int i = 0; i < rounds->count(); ++i)
    if (rounds->itemText(i) == QStringLiteral("第 4 回合")) roundFourChoices.append(i);
  if (repeatedActions.size() != 3 || roundFourChoices.size() != 2 || !timeline->roundButton(2) ||
      timeline->roundButton(3) || rounds->itemData(roundFourChoices[0]).toInt() != repeatedActions[0] ||
      rounds->itemData(roundFourChoices[1]).toInt() != repeatedActions[2])
    return fail("known/unknown/same-known rounds merged in the header or legacy dropdown");
  for (int occurrence = 0; occurrence < roundFourChoices.size(); ++occurrence) {
    page.seek(repeatedActions[1]); play->click();
    const int choice = roundFourChoices[occurrence]; rounds->setCurrentIndex(choice);
    if (!QMetaObject::invokeMethod(rounds, "activated", Qt::DirectConnection, Q_ARG(int, choice)) ||
        !atActionStart(repeatedActions[occurrence == 0 ? 0 : 2], source, 100) ||
        timeline->activeRoundIndex() != (occurrence == 0 ? 0 : 2))
      return fail("legacy dropdown failed to reach the selected repeated round's own action start");
  }

  const auto verifyDenseMenu = [&](const QJsonArray& records, int expectedCount, int expectedDestinations) {
    const auto parsed = BattleReplay::parse(withActions(records), previewBattleReplayCatalog()->root);
    if (!parsed.error.isEmpty()) return fail("dense-menu fixture could not be parsed");
    ReplayLifeTimeline dense;
    dense.resize(260, dense.sizeHint().height()); dense.setRange(0, parsed.steps.size() - 1);
    dense.setReplay(parsed, previewBattleReplayCatalog()->root); dense.setPerspective(1);
    if (dense.markers().size() != expectedCount) return fail("dense-menu fixture lost observed life transitions");
    dense.setValue(dense.markers().first().step);
    dense.show(); QCoreApplication::processEvents();
    const auto rect = dense.markerRect(0);
    QSet<int> destinations;
    for (int i = 0; i < dense.markers().size(); ++i) {
      destinations.insert(dense.markers()[i].step);
      if (rect.isEmpty() || dense.markerRect(i) != rect) return fail("dense-menu fixture did not share one visible bucket");
    }
    if (destinations.size() != expectedDestinations) return fail("dense-menu fixture has unexpected action destinations");
    QList<int> requested;
    QObject::connect(&dense, &ReplayLifeTimeline::actionStartRequested, &dense,
      [&](int step) { requested.append(step); });
    QTest::mouseClick(&dense, Qt::LeftButton, Qt::NoModifier, rect.center());
    auto* menu = dense.findChild<QMenu*>(QStringLiteral("replayLifeEventMenu"));
    if (!menu || !menu->isVisible() || menu->actions().size() != expectedCount || !requested.isEmpty())
      return fail("dense group did not offer every record for explicit selection");
    const auto choices = menu->actions();
    for (int i = 0; i < choices.size(); ++i) {
      const auto& marker = dense.markers()[i];
      if (choices[i]->data().toInt() != marker.step ||
          !choices[i]->text().contains(QStringLiteral("第 %1 小回合").arg(marker.roundTurn)))
        return fail("dense menu associates a record with the wrong round-local action");
      choices[i]->trigger();
      if (requested.size() != i + 1 || requested.last() != marker.step)
        return fail("dense-menu selection did not emit its record's exact action start");
    }
    menu->close(); dense.hide();
    return true;
  };
  // Two adjacent actions collide at a narrow width once the round contains
  // enough other actions. Both destinations and every event remain selectable.
  QJsonArray denseActions{action(1, 6, pulse, 100), action(1, 6, pulse, 100)};
  for (int i = 0; i < 18; ++i) denseActions.append(action(1, 6, QStringLiteral("0"), 100));
  if (!verifyDenseMenu(denseActions, 8, 2)) return false;
  // A single action can exceed the tooltip's 12-record preview, so the
  // promised expanded menu must exist even though all destinations coincide.
  QStringList repeatedValues;
  for (int i = 0; i < 7; ++i) repeatedValues.append({QStringLiteral("-100"), QStringLiteral("100")});
  if (!verifyDenseMenu(QJsonArray{action(1, 7, repeatedValues.join(QLatin1Char(',')), 100)}, 14, 1)) return false;
  page.hide();
  return page.replay().original == repeatedRoundBytes;
}
