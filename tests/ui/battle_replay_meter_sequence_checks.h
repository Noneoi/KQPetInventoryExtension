#pragma once

#include "ui/battle/battle_replay_meter_sequence.h"
#include "ui/battle/battle_replay_page.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryFile>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>

// These deliberately synthetic values test reading order and observation
// projection. They neither calculate combat results nor extend the catalog.
inline bool verifyReplayMeterSequenceProjection() {
  using namespace BattleReplay;
  const auto fail = [](const char* reason) { qCritical() << "meter sequence projection:" << reason; return false; };
  const auto fighter = [](const QString& key) {
    Fighter value; value.key = key; value.name = key; value.hp = value.maxHp = 100;
    value.shield = 50; value.power = value.maxPower = 100; value.entered = value.entryKnown = true;
    value.hpPath = value.shieldPath = value.powerPath = QStringLiteral("/initial/") + key;
    value.raceId = QStringLiteral("synthetic-origin"); updatePresence(value); return value;
  };
  const auto effect = [](const QString& target, const QString& metric, qint64 before, qint64 after,
                         const QString& path, const QString& source = QString(), const QString& cause = QStringLiteral("buff")) {
    Effect value; value.kind = QStringLiteral("value"); value.target = target; value.metric = metric;
    value.before = before; value.after = after; value.delta = after - before; value.changed = before != after;
    value.path = path; value.source = source; value.cause = cause; value.moment = 1;
    value.raw = QString::number(*value.delta); return value;
  };
  const auto make = [&](const QString& phase, const QString& part, const QList<Effect>& effects,
                        const QString& actor = QStringLiteral("source")) {
    Step step; step.path = QStringLiteral("/synthetic/action"); step.actor = QStringLiteral("source");
    step.playbackKind = QStringLiteral("normal");
    Moment initial; initial.phase = QStringLiteral("setup"); initial.part = QStringLiteral("initial");
    initial.state = {{"source", fighter("source")}, {"one", fighter("one")}, {"two", fighter("two")}};
    Effect setup; setup.kind = QStringLiteral("target"); setup.target = QStringLiteral("one"); setup.moment = 0;
    initial.effects = {setup}; step.effects = {setup};
    Moment moment; moment.phase = phase; moment.part = part; moment.actor = actor;
    moment.effects = effects; moment.state = initial.state;
    for (const auto& current : effects) ReplayMeterSequence::Detail::apply(moment.state, current);
    step.moments = {initial, moment}; step.effects.append(effects); step.state = moment.state; return step;
  };
  const auto hp = QStringLiteral("生命"), shield = QStringLiteral("护盾"), power = QStringLiteral("气势");
  const auto sourceHp = QStringLiteral("/synthetic/action/cn/att_hps");
  const auto own = QStringLiteral("source"), one = QStringLiteral("one"), two = QStringLiteral("two");

  const auto sameDirection = make("attack", "result", {
    effect(own, hp, 100, 80, sourceHp, own, "skill"),
    effect(own, hp, 80, 50, sourceHp, own, "skill")});
  const auto loss = ReplayMeterSequence::split(sameDirection, 1);
  if (loss.size() != 2 || loss[0].state[own].hp != 80 || loss[1].beforeState[own].hp != 80 || loss[1].state[own].hp != 50 ||
      loss[0].effects != QList<int>{1} || loss[1].effects != QList<int>{2} ||
      loss[0].completed != QSet<int>{1} || loss[1].completed != QSet<int>{1, 2})
    return fail("same-direction source changes were merged or lost global effect identities");
  if (loss[0].state[own].hpPath != sourceHp || loss[0].state[one].hpPath != sameDirection.moments[0].state[one].hpPath)
    return fail("consumed provenance or an untouched fighter's starting observation was lost");

  const auto pulse = make("attack", "result", {
    effect(own, hp, 100, 0, sourceHp, own, "skill"),
    effect(own, hp, 0, 100, sourceHp, own, "skill"),
    effect(own, hp, 100, 0, sourceHp, own, "skill"),
    effect(own, hp, 0, 100, sourceHp, own, "skill")});
  const auto crossings = ReplayMeterSequence::split(pulse, 1);
  if (crossings.size() != 4) return fail("repeated zero-net life crossings were collapsed");
  for (int i = 0; i < crossings.size(); ++i) {
    const bool zero = i % 2 == 0;
    if (crossings[i].state[own].hp != (zero ? 0 : 100) ||
        crossings[i].state[own].presence.contains(QStringLiteral("生命为 0")) != zero)
      return fail("the projected death/recovery state disagrees with its exact balance");
  }

  const auto attack = make("attack", "result", {
    effect(own, hp, 100, 70, sourceHp, own, "skill"),
    effect(own, hp, 70, 90, sourceHp, own, "skill"),
    effect(own, shield, 50, 30, "/synthetic/action/cn/att_shps", own, "skill"),
    effect(own, power, 100, 20, "/synthetic/action/cn/att_vgs", own, "skill"),
    effect(one, hp, 100, 40, "/synthetic/action/trl/0/tn/att_hps", own, "skill"),
    effect(two, hp, 100, 60, "/synthetic/action/trl/1/tn/att_hps", own, "skill")});
  const auto hits = ReplayMeterSequence::split(attack, 1);
  if (hits.size() != 4 || hits[1].state[own].shield != 50 || hits[2].state[own].shield != 30 ||
      hits[2].state[own].power != 100 || hits[3].state[own].power != 20 ||
      hits[2].state[one].hp != 100 || hits[2].state[two].hp != 100 ||
      hits[3].state[one].hp != 40 || hits[3].state[two].hp != 60 || hits[3].effects != QList<int>({4, 5, 6}))
    return fail("source HP/shield/power order or atomic parallel target totals changed");

  const auto before = make("before", "values", {
    effect(one, hp, 100, 80, "/synthetic/action/trl/0/tn/bb_hps"),
    effect(one, hp, 80, 90, "/synthetic/action/trl/0/tn/bb_hps"),
    effect(one, shield, 50, 20, "/synthetic/action/trl/0/tn/bb_shps"),
    effect(two, hp, 100, 60, "/synthetic/action/trl/1/tn/bb_hps"),
    effect(two, shield, 50, 40, "/synthetic/action/trl/1/tn/bb_shps")});
  const auto parallel = ReplayMeterSequence::split(before, 1);
  if (parallel.size() != 3 || parallel[0].state[one].hp != 80 || parallel[0].state[two].hp != 60 ||
      parallel[0].state[one].shield != 50 || parallel[0].state[two].shield != 50 ||
      parallel[1].state[one].hp != 90 || parallel[1].state[one].shield != 50 || parallel[1].state[two].shield != 40 ||
      parallel[2].state[one].shield != 20 || parallel[2].completed != QSet<int>({1, 2, 3, 4, 5}))
    return fail("different Buff recipients were serialized or local HP/shield order was lost");
  const auto afterPower = make("after", "power-buff", {
    effect(one, power, 100, 0, "/synthetic/action/trl/0/tn/ab_vgs"),
    effect(one, power, 0, 50, "/synthetic/action/trl/0/tn/ab_vgs")});
  const auto powers = ReplayMeterSequence::split(afterPower, 1);
  if (powers.size() != 2 || powers[0].state[one].power != 0 || powers[1].state[one].power != 50)
    return fail("ordered power changes were not kept inside their power-buff boundary");

  const auto counter = make("counter", "result", {
    effect(one, hp, 100, 80, "/synthetic/action/trl/0/tn/sb_hps", one, "counter"),
    effect(one, hp, 80, 90, "/synthetic/action/trl/0/tn/sb_hps", one, "counter"),
    effect(own, hp, 100, 30, "/synthetic/action/cn/sb_hps", one, "counter")}, one);
  const auto reversed = ReplayMeterSequence::split(counter, 1);
  if (reversed.size() != 2 || reversed[0].state[own].hp != 100 || reversed[1].state[own].hp != 30 ||
      reversed[0].state[one].hp != 80 || reversed[1].effects != QList<int>({2, 3}))
    return fail("counter source and target were not reversed according to the recorded actor");

  QList<Step> unsafe;
  auto unknown = sameDirection; unknown.effects[1].before.reset(); unknown.moments[1].effects[0].before.reset(); unsafe.append(unknown);
  auto discontinuous = sameDirection; discontinuous.effects[2].before = 79; discontinuous.moments[1].effects[1].before = 79; unsafe.append(discontinuous);
  auto incorrectFinal = sameDirection; incorrectFinal.moments[1].state[own].hp = 49; unsafe.append(incorrectFinal);
  auto unknownWait = before; unknownWait.moments[1].part = "group-end"; unsafe.append(unknownWait);
  auto unresolved = sameDirection; unresolved.moments[1].phase = "unresolved"; unsafe.append(unresolved);
  auto reset = sameDirection; reset.moments[1].part = "objects";
  reset.moments[1].effects[0].raw = QJsonObject{{"t", 2}, {"new_curv", 80}, {"new_maxv", 100}};
  reset.effects[1] = reset.moments[1].effects[0]; unsafe.append(reset);
  auto misplacedReset = reset; misplacedReset.moments[1].part = "result"; unsafe.append(misplacedReset);
  auto futureBuff = sameDirection; futureBuff.moments[1].state[one].buffs.append("future-buff"); unsafe.append(futureBuff);
  auto futureForm = sameDirection; futureForm.moments[1].state[own].raceId = "future-form"; unsafe.append(futureForm);
  auto overlapping = before;
  overlapping.effects[2].path = "/synthetic/action/trl/9/tn/bb_hps";
  overlapping.moments[1].effects[1].path = overlapping.effects[2].path; unsafe.append(overlapping);
  auto mixed = afterPower; Effect membership; membership.kind = "buff"; membership.target = one; membership.moment = 1;
  mixed.effects.append(membership); mixed.moments[1].effects.append(membership); unsafe.append(mixed);
  const auto noRepeat = make("before", "values", {
    effect(one, hp, 100, 90, "/synthetic/action/trl/0/tn/bb_hps"),
    effect(one, shield, 50, 40, "/synthetic/action/trl/0/tn/bb_shps")});
  unsafe.append(noRepeat);
  const auto targetOnly = make("attack", "result", {
    effect(one, hp, 100, 60, "/synthetic/action/trl/0/tn/att_hps", own, "skill"),
    effect(one, hp, 60, 80, "/synthetic/action/trl/0/tn/att_hps", own, "skill")});
  unsafe.append(targetOnly);
  for (int i = 0; i < unsafe.size(); ++i) if (!ReplayMeterSequence::split(unsafe[i], 1).isEmpty()) {
    qCritical() << "unsafe meter sequence case" << i; return fail("an unconfirmed or already atomic command was subdivided");
  }
  if (!ReplayMeterSequence::split(sameDirection, 0).isEmpty() || !ReplayMeterSequence::split(sameDirection, -1).isEmpty() ||
      !ReplayMeterSequence::split(sameDirection, 2).isEmpty()) return fail("an absent command or initial snapshot was subdivided");
  return true;
}

QByteArray realtimeMultiUnitFixture();

inline bool verifyReplayMeterSequenceNavigation() {
  using namespace BattleReplay;
  const auto fail = [](const char* reason) { qCritical() << "meter sequence navigation:" << reason; return false; };
  // Reuse the explicitly synthetic two-side fixture and known playable
  // references. Repeated source values are logged input, never simulated hits.
  auto root = QJsonDocument::fromJson(realtimeMultiUnitFixture()).object();
  auto br = root["br"].toObject(); auto flow = br["fr"].toArray(); auto battle = flow[0].toObject();
  const auto original = battle["rl"].toArray(); auto first = original[1].toObject();
  first["csn"] = 1;
  first["cn"] = QJsonObject{{"att_hps", "-100,100,-100,100"}, {"ahp", 100}, {"avg", 60},
    {"bb_shps", "300113_-5_1,300113_2_1,300113_3_1"}, {"bb_vgs", "300113_1_1"}};
  auto firstTargets = first["trl"].toArray(); auto firstTarget = firstTargets[0].toObject();
  auto firstNumbers = firstTarget["tn"].toObject();
  firstNumbers["bb_shps"] = "300113_-7_1,300113_7_1";
  firstTarget["tn"] = firstNumbers; firstTargets[0] = firstTarget; first["trl"] = firstTargets;
  auto second = original[1].toObject(); second["csn"] = 2;
  second["cn"] = QJsonObject{{"att_hps", "-100,100,-100,100"}, {"ahp", 100}, {"avg", 60}};
  auto secondTargets = second["trl"].toArray(); auto secondTarget = secondTargets[0].toObject();
  auto secondNumbers = secondTarget["tn"].toObject(); secondNumbers["ahp"] = 90;
  secondTarget["tn"] = secondNumbers; secondTargets[0] = secondTarget; second["trl"] = secondTargets;
  // A following ordinary command supplies the other unit's single death;
  // unit filtering and unsplit/cross-action event navigation must still work.
  auto third = original[1].toObject(); third["cti"] = 0; third["csn"] = 3;
  third["cn"] = QJsonObject{{"att_hps", "-90"}, {"ahp", 0}, {"avg", 100}};
  third["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true},
    {"tn", QJsonObject{{"att_hps", "0"}, {"ahp", 100}, {"avg", 60}}}}};
  battle["rl"] = QJsonArray{first, second, third}; flow[0] = battle; br["fr"] = flow; root["br"] = br;
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return fail("fixture file unavailable"); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); if (!page.loadFile(file.fileName())) return fail("fixture load failed");
  const auto source = fighterKey(1, "tester", "7"), other = fighterKey(0, "tester", "7");
  QMap<int, int> actions;
  for (int i = 0; i < page.replay().steps.size(); ++i) {
    const auto action = page.replay().steps[i].raw.toObject();
    if (action.contains("si")) actions[action["csn"].toInt()] = i;
  }
  if (actions.size() != 3) return fail("three synthetic actions were not retained");
  auto* nextEffect = page.findChild<QPushButton*>("replayNextEffect");
  auto* nextEvent = page.findChild<QPushButton*>("replayNextEvent");
  auto* eventType = page.findChild<QComboBox*>("replayEventType");
  auto* unitFilter = page.findChild<QComboBox*>("replayUnitFilter");
  auto* effects = page.findChild<QListWidget*>("replayEffects");
  QPushButton* previousEffect = nullptr; QPushButton* previousEvent = nullptr;
  for (auto* button : page.findChildren<QPushButton*>()) {
    if (button->text() == QStringLiteral("上一作用")) previousEffect = button;
    if (button->text() == QStringLiteral("上个事件")) previousEvent = button;
  }
  if (!nextEffect || !previousEffect || !nextEvent || !previousEvent || !eventType || !unitFilter || !effects)
    return fail("navigation controls unavailable");
  const auto click = [](QPushButton* button) { button->click(); QCoreApplication::processEvents(); };
  const auto selectedEffect = [&] { return effects->currentItem() ? effects->currentItem()->data(Qt::UserRole).toInt() : -1; };
  const auto atPulse = [&](int action, int segment) {
    return page.currentStep() == actions[action] && page.property("numericSegmentCount").toInt() == 4 &&
      page.property("numericSegment").toInt() == segment && page.playbackPhaseKind() == "attack" &&
      page.displayedState().value(source).hp == (segment % 2 ? 100 : 0);
  };
  page.seek(actions[1]);
  QMap<int, int> pulsePhases, shieldPhases; int powerMoment = -1;
  for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
    page.seekPlaybackPhase(phase);
    if (page.playbackPhaseKind() == "attack" && page.property("numericSegmentCount").toInt() == 4)
      pulsePhases[page.property("numericSegment").toInt()] = phase;
    if (page.playbackPhaseKind() == "before" && page.property("numericSegmentCount").toInt() == 3)
      shieldPhases[page.property("numericSegment").toInt()] = phase;
  }
  const auto& firstStep = page.replay().steps[actions[1]];
  QList<int> sourceShields, targetShields;
  for (int i = 0; i < firstStep.effects.size(); ++i) {
    const auto& effect = firstStep.effects[i];
    if (effect.metric == QStringLiteral("护盾") && effect.path.endsWith("/bb_shps"))
      (effect.target == source ? sourceShields : targetShields).append(i);
    if (effect.metric == QStringLiteral("气势") && effect.path.endsWith("/bb_vgs")) powerMoment = effect.moment;
  }
  if (pulsePhases.size() != 4 || shieldPhases.size() != 3 || sourceShields.size() != 3 || targetShields.size() != 2 || powerMoment < 0)
    return fail("numeric navigation fixture did not refine its proven lists");
  page.selectEffect(sourceShields[0]);
  const QList<QPair<int, int>> forward{{targetShields[0], 0}, {sourceShields[1], 1}, {targetShields[1], 1}, {sourceShields[2], 2}};
  for (const auto& point : forward) {
    click(nextEffect);
    if (selectedEffect() != point.first || page.currentPlaybackPhase() != shieldPhases[point.second])
      return fail("parallel recipients followed raw array order or skipped a same-slice record");
  }
  click(previousEffect);
  if (selectedEffect() != targetShields[1] || page.currentPlaybackPhase() != shieldPhases[1])
    return fail("reverse navigation did not use stable phase/effect order");
  page.seekMoment(powerMoment); click(previousEffect);
  if (selectedEffect() != sourceShields[2] || page.currentPlaybackPhase() != shieldPhases[2])
    return fail("entering a refined command backwards selected the last raw index instead of the latest slice");
  page.seekPlaybackPhase(pulsePhases[0]); click(nextEffect);
  if (!atPulse(1, 1)) return fail("next effect skipped unplayed changes within the current Moment");
  click(previousEffect);
  if (!atPulse(1, 0)) return fail("previous effect failed to restore the prior numeric balance");

  eventType->setCurrentIndex(0); unitFilter->setCurrentIndex(0);
  page.seek(actions[1]); page.seekMoment(0);
  for (int segment = 0; segment < 4; ++segment) {
    click(nextEvent);
    if (!atPulse(1, segment)) return fail("repeated same-Moment zero/recovery event was skipped or showed the final balance");
  }
  click(nextEvent);
  if (!atPulse(2, 0)) return fail("forward key-event navigation did not cross into the next action");
  click(previousEvent);
  if (!atPulse(1, 3)) return fail("reverse cross-action navigation lost the final recorded recovery");
  for (int segment = 2; segment >= 0; --segment) {
    click(previousEvent);
    if (!atPulse(1, segment)) return fail("reverse key-event navigation collapsed repeated crossings");
  }
  eventType->setCurrentIndex(eventType->findText(QStringLiteral("生命恢复")));
  page.seek(actions[1]); page.seekMoment(0); click(nextEvent);
  if (!atPulse(1, 1)) return fail("event-kind filter did not locate the first recovery slice");
  click(nextEvent);
  if (!atPulse(1, 3)) return fail("domain event deduplication hid the second recovery");
  click(previousEvent);
  if (!atPulse(1, 1)) return fail("reverse event-kind filter did not return to the first recovery");
  const int otherFilter = unitFilter->findData(other);
  if (otherFilter < 0) return fail("recorded unit filter missing");
  unitFilter->setCurrentIndex(otherFilter);
  eventType->setCurrentIndex(eventType->findText(QStringLiteral("生命归零")));
  page.seek(actions[1]); page.seekMoment(0); click(nextEvent);
  if (page.currentStep() != actions[3] || page.currentPlaybackPhase() >= 0 || page.displayedState().value(other).hp != 0)
    return fail("unit filtering or the ordinary unsplit event path changed");
  unitFilter->setCurrentIndex(unitFilter->findData(source)); eventType->setCurrentIndex(0); click(previousEvent);
  if (!atPulse(2, 3)) return fail("unit-filtered reverse navigation did not cross back to a numeric recovery");
  return page.replay().original == bytes;
}
