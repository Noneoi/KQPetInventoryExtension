#pragma once

// Explicit synthetic observations check presentation only. They do not model
// game calculations or add evidence to the official game dictionary.
inline bool verifyActionMeterCellPresentation(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "action meter cell:" << reason; return false; };
  BattleReplay::Fighter fighter; fighter.key = "action-meter-observed-recipient";
  fighter.name = QStringLiteral("本次行动数值展示测试"); fighter.team = 1; fighter.entered = true;
  fighter.hp = 500; fighter.maxHp = 1000; fighter.shield = 350; fighter.power = 160; fighter.maxPower = 100;
  const auto event = [](int index, const QString& metric, const QString& amount,
                        std::optional<qint64> before, std::optional<qint64> after, bool critical = false) {
    ReplayUnitEvent result; result.index = index; result.recordStep = 4; result.metric = metric;
    result.valueText = amount; result.observedBefore = before; result.observedAfter = after; result.critical = critical;
    result.label = metric + amount; result.detail = result.label + QStringLiteral(" · 已记录变化");
    result.color = metric == QStringLiteral("生命") ? "#b52d46" : metric == QStringLiteral("护盾") ? "#256bab" : "#a9660a";
    return result;
  };
  ReplayUnitOverview cue; cue.hasActionMeterContext = true; cue.actionMetersPending = true; cue.animate = true;
  cue.actionHpBefore = 800; cue.actionMaxHpBefore = 1000; cue.actionShieldBefore = 200;
  cue.actionPowerBefore = 130; cue.actionMaxPowerBefore = 100;
  cue.badge = QStringLiteral("被暴击 · 格挡");
  cue.actionMeterEvents = {event(11, QStringLiteral("生命"), "-600", 800, 200, true),
    event(12, QStringLiteral("生命"), "+300", 200, 500), event(13, QStringLiteral("生命"), "+7", {}, {}),
    event(14, QStringLiteral("护盾"), "+150", 200, 350), event(15, QStringLiteral("气势"), "-80", 130, 50),
    event(16, QStringLiteral("气势"), "+110", 50, 160)};
  // Phase records may now be empty: the action prefix must remain visible.
  cue.events.clear();
  ReplayCell cell(1); cell.resize(300, 244); cell.setPlaybackDriven(true); cell.show();
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  QCoreApplication::processEvents();
  auto* hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  auto* shield = cell.findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  auto* power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  if (!hp || !shield || !power) return fail("a recorded meter is missing");
  if (hp->format() != "500/1000" || shield->format() != QStringLiteral("护盾 350") || power->format() != "160/100")
    return fail("pending presentation changed a real balance or invented a shield maximum");
  if (hp->height() > 17 || shield->height() > 17 || power->height() > 17)
    return fail("action mode retained the old in-bar delta row");
  const auto hpSegments = hp->property("actionGhostSegments").toList();
  if (hpSegments.size() != 2 || hpSegments[0].toMap()["kind"] != "remove" || hpSegments[1].toMap()["kind"] != "add" ||
      hpSegments[0].toMap()["before"].toLongLong() != 800 || hpSegments[0].toMap()["after"].toLongLong() != 200 ||
      hpSegments[1].toMap()["from"].toDouble() != .2 || hpSegments[1].toMap()["to"].toDouble() != .5)
    return fail("ordered loss and gain were netted or an unobserved delta acquired a width");
  if (shield->property("actionMeterReference").toLongLong() != 350 || !shield->toolTip().contains(QStringLiteral("不表示护盾上限")) ||
      power->property("actionMeterReference").toLongLong() != 160 || power->property("actionGhostSegments").toList().size() != 2)
    return fail("shield/vigor display scale lost the recorded over-threshold range");
  const auto floating = cell.floatingMeterChanges();
  if (floating.size() != 3 || floating[0].anchor != hp || floating[0].fighterKey != fighter.key ||
      floating[0].metric != QStringLiteral("生命") || floating[0].events.size() != 3 || !floating[0].events.first().critical)
    return fail("floating callouts lost exact events or their concrete bar anchors");
  for (qreal position : {qreal(0), qreal(.45), qreal(1)}) {
    cell.setPlaybackProgress(position); QCoreApplication::processEvents();
    if (hp->property("actionGhostSegments").toList() != hpSegments || hp->value() != 5000)
      return fail("a phase clock prematurely settled or interpolated the action ghost");
  }
  if (hp->findChild<QVariantAnimation*>(QStringLiteral("replayMeterAnimation")))
    return fail("action mode still started the old per-phase interpolation");
  auto* badge = cell.findChild<QLabel*>(QStringLiteral("unitResultBadges"));
  auto* outcome = cell.findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (!badge || badge->text() != QStringLiteral("格挡") || !badge->accessibleName().contains(QStringLiteral("被暴击")) ||
      (outcome && outcome->property("effectKinds").toStringList().contains("critical")) ||
      !cell.toPlainText().contains(QStringLiteral("被暴击")))
    return fail("critical damage was duplicated in a large effect or its original evidence was discarded");
  int activatedStep = -1, activatedIndex = -1;
  QObject::connect(&cell, &ReplayCell::recordedEffectActivated, &cell,
    [&](int step, int index) { activatedStep = step; activatedIndex = index; });
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, hp->rect().center()); QCoreApplication::processEvents();
  auto* menu = hp->findChild<QMenu*>(QStringLiteral("replayMeterEventMenu"));
  if (!menu || menu->actions().size() != 3) return fail("bar did not retain its complete ordered action records");
  menu->actions()[1]->trigger(); menu->close(); QCoreApplication::processEvents();
  if (activatedStep != 4 || activatedIndex != 12) return fail("action-history click selected a different phase record");
  const auto captureDirectory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
  if (!captureDirectory.isEmpty()) {
    QDir().mkpath(captureDirectory); cell.grab().save(QDir(captureDirectory).filePath(QStringLiteral("action-meter-pending-bands.png")));
  }

  // Settlement changes only the visual layer. Exact records stay inspectable.
  cue.actionMetersPending = false;
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  if (!cell.floatingMeterChanges().isEmpty() || !hp->property("actionGhostSegments").toList().isEmpty() ||
      hp->format() != "500/1000" || hp->property("actionMeterEventCount").toInt() != 3)
    return fail("settling erased recorded values/events or retained a pending band");

  // A complete zero-net loss/recovery keeps both observations, never no-ops.
  fighter.hp = 100; fighter.maxHp = 100; cue.actionHpBefore = 100; cue.actionMaxHpBefore = 100;
  cue.actionMetersPending = true; cue.actionMeterEvents = {
    event(21, QStringLiteral("生命"), "-100", 100, 0), event(22, QStringLiteral("生命"), "+100", 0, 100)};
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  const auto zeroNet = hp->property("actionGhostSegments").toList();
  if (zeroNet.size() != 2 || zeroNet.last().toMap()["kind"] != "add" || hp->value() != 10000)
    return fail("zero-net loss/recovery disappeared or the final balance changed");
  outcome = cell.findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (!outcome || !outcome->property("effectKinds").toStringList().contains("critical"))
    return fail("unattached critical judgement was hidden without a damage callout");

  // Every occupant keeps its own anchor; a hidden occupant is filtered by the
  // board's isVisibleTo check, not substituted with a same-slot neighbor.
  auto other = fighter; other.key = "action-meter-other-occupant"; other.name = QStringLiteral("同格另一个精灵");
  cell.display(1, {fighter, other}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}, {other.key, cue}}, 0);
  cell.focusUnit(fighter.key); QCoreApplication::processEvents();
  const auto all = cell.floatingMeterChanges(); int visible = 0;
  for (const auto& item : all) if (item.anchor && item.anchor->isVisibleTo(&cell)) {
    ++visible; if (item.fighterKey != fighter.key) return fail("hidden occupant reused a visible neighbor's meter");
  }
  if (all.size() != 6 || visible != 3) return fail("same-slot anchors lost or gained an occupant");
  const QPointer<QWidget> oldAnchor = all.first().anchor;
  cell.display(1, {fighter}, catalog, {}, {});
  if (!oldAnchor.isNull() || !cell.floatingMeterChanges().isEmpty()) return fail("rebuilding a card retained stale pending anchors");
  return true;
}
