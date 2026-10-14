#pragma once
#include "ui/battle/battle_replay_action_meters.h"
#include "ui/battle/battle_replay_meter_overlay.h"
#include "ui/battle/battle_replay_numbers.h"

// Every report and fighter below is deliberately synthetic. Existing official
// skill 100001 only supplies a playable configuration; the values and outcomes
// are test inputs, never evidence about game mechanics or a server formula.
namespace ReplayActionFloatChecks {
inline bool exactNumberGroups() {
  const QList<QPair<QString, QString>> cases{
    {QStringLiteral("0"), QStringLiteral("0")},
    {QStringLiteral("9999"), QStringLiteral("9999")},
    {QStringLiteral("10000"), QStringLiteral("1万")},
    {QStringLiteral("10001"), QStringLiteral("1万1")},
    {QStringLiteral("99999999"), QStringLiteral("9999万9999")},
    {QStringLiteral("100000000"), QStringLiteral("1亿")},
    {QStringLiteral("100000001"), QStringLiteral("1亿1")},
    {QStringLiteral("+123456789"), QStringLiteral("+1亿2345万6789")},
    {QStringLiteral("-123456789"), QStringLiteral("-1亿2345万6789")},
    {QStringLiteral("9223372036854775807"), QStringLiteral("92233720368亿5477万5807")},
    {QStringLiteral("-9223372036854775808"), QStringLiteral("-92233720368亿5477万5808")},
    {QStringLiteral("?/10001"), QStringLiteral("?/1万1")},
    {QStringLiteral("校准 -12345 → ?"), QStringLiteral("校准 -1万2345 → ?")},
    {QStringLiteral("上限 +123456789"), QStringLiteral("上限 +1亿2345万6789")}
  };
  for (const auto& pair : cases) {
    if (ReplayNumbers::display(pair.first, true) != pair.second || ReplayNumbers::display(pair.first, false) != pair.first) {
      qCritical() << "exact number grouping changed signed integer precision:" << pair.first; return false;
    }
  }
  return true;
}

inline QProgressBar* barFor(QWidget& host, const QString& key, const QString& name) {
  for (auto* bar : host.findChildren<QProgressBar*>(name))
    if (bar->property("fighterKey").toString() == key) return bar;
  return nullptr;
}

inline bool criticalAttribution() {
  using namespace BattleReplay;
  const auto fail = [](const char* reason) { qCritical() << "action float critical attribution:" << reason; return false; };
  Step step; step.actor = "own/source"; step.playbackKind = "normal";
  Effect target; target.kind = "target"; target.target = "enemy/one"; target.path = "/action/trl/0";
  target.raw = QJsonObject{{"isse", true}, {"iscr", true}, {"ishit", true}}; step.effects = {target};
  Effect damage; damage.kind = "value"; damage.cause = "skill"; damage.source = step.actor;
  damage.target = target.target; damage.metric = QStringLiteral("生命"); damage.delta = -2000000;
  damage.path = target.path + "/tn/att_hps";
  if (!ReplayActionMeters::criticalDamage(step, damage)) return fail("matching direct HP damage lost explicit critical judgement");
  auto shield = damage; shield.metric = QStringLiteral("护盾"); shield.path = target.path + "/tn/att_shps";
  if (!ReplayActionMeters::criticalDamage(step, shield)) return fail("matching direct shield damage lost explicit critical judgement");
  QList<Effect> excluded;
  auto healing = damage; healing.delta = 2000000; excluded.append(healing);
  auto zero = damage; zero.delta = 0; excluded.append(zero);
  auto power = damage; power.metric = QStringLiteral("气势"); excluded.append(power);
  auto buff = damage; buff.cause = "buff"; buff.path = target.path + "/tn/ab_hps"; excluded.append(buff);
  auto cost = damage; cost.target = step.actor; cost.path = "/action/cn/att_hps"; excluded.append(cost);
  auto counter = damage; counter.cause = "counter"; counter.path = target.path + "/tn/sb_hps"; excluded.append(counter);
  auto other = damage; other.target = "enemy/two"; excluded.append(other);
  auto otherPath = damage; otherPath.path = "/action/trl/1/tn/att_hps"; excluded.append(otherPath);
  auto unknownSource = damage; unknownSource.source.clear(); excluded.append(unknownSource);
  auto calibration = damage; calibration.cause = "calibration"; excluded.append(calibration);
  for (const auto& effect : excluded)
    if (ReplayActionMeters::criticalDamage(step, effect)) return fail("critical judgement leaked to another target, cause, path or non-damage value");
  const QList<QJsonObject> denied{
    {{"isse", true}, {"iscr", true}, {"ishit", false}},
    {{"isse", true}, {"iscr", true}, {"isIm", true}},
    {{"isse", false}, {"iscr", true}, {"ishit", true}},
    {{"isse", true}, {"ishit", true}},
    {{"isse", true}, {"iscr", false}, {"ishit", true}}};
  for (const auto& flags : denied) {
    step.effects[0].raw = flags;
    if (ReplayActionMeters::criticalDamage(step, damage)) return fail("immune, evaded, ineffective or unrecorded critical judgement rendered as critical");
  }
  step.effects[0] = target; step.playbackKind = "disabled";
  if (ReplayActionMeters::criticalDamage(step, damage)) return fail("disabled action was labelled critical");
  step.playbackKind = "team-skill"; damage.cause = "team";
  if (!ReplayActionMeters::criticalDamage(step, damage)) return fail("recorded team-skill target lost its critical judgement");
  return true;
}

inline bool prefixProjection() {
  using namespace BattleReplay;
  const auto fail = [](const char* reason) { qCritical() << "action float projection:" << reason; return false; };
  Fighter fighter; fighter.key = "synthetic/target"; fighter.hp = 140; fighter.maxHp = 300;
  fighter.shield = 25; fighter.power = 150; fighter.maxPower = 100;
  auto beginningFighter = fighter; beginningFighter.hp = 200; beginningFighter.shield = 10; beginningFighter.power = 100;
  const QMap<QString, Fighter> beginning{{fighter.key, beginningFighter}}, visible{{fighter.key, fighter}};
  Step step;
  const auto add = [&](int moment, QString metric, qint64 before, qint64 after) {
    Effect effect; effect.kind = "value"; effect.target = fighter.key; effect.metric = metric;
    effect.moment = moment; effect.before = before; effect.after = after; effect.delta = after - before;
    effect.path = QStringLiteral("/synthetic/%1").arg(step.effects.size()); step.effects.append(effect);
  };
  add(1, QStringLiteral("生命"), 200, 150);
  add(2, QStringLiteral("护盾"), 10, 30);
  add(3, QStringLiteral("生命"), 150, 180);
  add(3, QStringLiteral("气势"), 100, 150);
  add(4, QStringLiteral("生命"), 180, 160);
  add(5, QStringLiteral("生命"), 160, 150);
  add(6, QStringLiteral("护盾"), 30, 25);
  // Two observations at the SAME recorded boundary have no demonstrated
  // order between them. Keep both exact values, without summing them.
  add(7, QStringLiteral("生命"), 150, 143);
  add(7, QStringLiteral("生命"), 143, 140);
  auto hiddenTarget = step.effects.first(); hiddenTarget.target = "not-visible"; step.effects.append(hiddenTarget);
  auto noMoment = step.effects.first(); noMoment.moment = -1; step.effects.append(noMoment);
  auto unchanged = step.effects.first(); unchanged.delta.reset(); unchanged.before = 140; unchanged.after = 140;
  unchanged.changed = false; step.effects.append(unchanged);
  struct Expected { int through, historyCount; QList<int> active; };
  const QList<Expected> expected{
    {0, 0, {}}, {1, 1, {0}}, {2, 2, {0, 1}}, {3, 4, {1, 2, 3}},
    {4, 5, {1, 3, 4}}, {5, 6, {1, 3, 5}}, {6, 7, {3, 5, 6}}, {7, 9, {3, 6, 7, 8}},
    {3, 4, {1, 2, 3}}, {1, 1, {0}}, {7, 9, {3, 6, 7, 8}}, {0, 0, {}}
  };
  const QStringList exactValues{"-50", "+20", "+30", "+50", "-20", "-10", "-5", "-7", "-3"};
  for (const auto& wanted : expected) {
    QMap<QString, ReplayUnitOverview> overview;
    ReplayActionMeters::populate(step, beginning, visible, {}, wanted.through, true, overview);
    const auto& item = overview[fighter.key];
    if (!item.hasActionMeterContext || !item.actionMetersPending || item.actionMeterEvents.size() != wanted.historyCount ||
        item.actionHpBefore != 200 || item.actionShieldBefore != 10 || item.actionPowerBefore != 100)
      return fail("prefix includes future/stale values or omitted its original balance");
    for (int i = 0; i < wanted.historyCount; ++i)
      if (item.actionMeterEvents[i].index != i || item.actionMeterEvents[i].valueText != exactValues[i])
        return fail("historical amounts were netted, summed or reordered");
    if (!item.activeMeterEvents) return fail("explicit current numeric segment was not supplied");
    QSet<int> actual, wantedIndices(wanted.active.begin(), wanted.active.end());
    for (const auto& event : *item.activeMeterEvents) {
      if (actual.contains(event.index) || event.valueText != exactValues.value(event.index))
        return fail("active segment duplicated or modified an exact observation");
      actual.insert(event.index);
    }
    if (actual != wantedIndices)
      return fail("next same-direction change failed to replace its predecessor, another meter was cleared, or rewind leaked future values");
  }
  if (fighter.hp != 140 || fighter.shield != 25 || fighter.power != 150 || beginningFighter.hp != 200)
    return fail("projection mutated an observed balance");
  return true;
}

inline bool cardAndOverlay(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "action float card/overlay:" << reason; return false; };
  QWidget board; board.resize(700, 420); board.setStyleSheet(QStringLiteral("background:#f4f7fa"));
  ReplayCell cell(1, &board); cell.setGeometry(170, 135, 218, 230); cell.setPlaybackDriven(true);
  BattleReplay::Fighter fighter; fighter.key = "synthetic/meter-recipient"; fighter.name = QStringLiteral("合成样例 · 数值承受者");
  fighter.team = 1; fighter.entered = true; fighter.hp = 2600000; fighter.maxHp = 5000000;
  fighter.shield = 450000; fighter.power = 150; fighter.maxPower = 100;
  ReplayUnitOverview cue; cue.hasActionMeterContext = true; cue.actionMetersPending = true;
  cue.actionHpBefore = 5000000; cue.actionMaxHpBefore = 5000000; cue.actionShieldBefore = 300000;
  cue.actionPowerBefore = 100; cue.actionMaxPowerBefore = 100;
  const auto add = [&](QString metric, qint64 before, qint64 after, bool critical = false) {
    ReplayUnitEvent event; event.index = cue.actionMeterEvents.size(); event.metric = metric;
    event.valueText = (after > before ? QStringLiteral("+") : QString()) + QString::number(after - before);
    event.label = metric + QLatin1Char(' ') + event.valueText; event.detail = QStringLiteral("合成行为测试记录");
    event.observedBefore = before; event.observedAfter = after; event.critical = critical; cue.actionMeterEvents.append(event);
  };
  add(QStringLiteral("生命"), 5000000, 2800000, true);
  add(QStringLiteral("生命"), 2800000, 2500000, true);
  add(QStringLiteral("生命"), 2500000, 2300000);
  add(QStringLiteral("生命"), 2300000, 2600000);
  add(QStringLiteral("护盾"), 300000, 500000);
  add(QStringLiteral("护盾"), 500000, 450000);
  add(QStringLiteral("气势"), 100, 150);
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  ReplayMeterOverlay overlay(&board); board.show(); QCoreApplication::processEvents();
  overlay.setChanges(cell.floatingMeterChanges()); board.grab(); QCoreApplication::processEvents();
  auto* hp = barFor(cell, fighter.key, "hpBar"); auto* shield = barFor(cell, fighter.key, "shieldValue");
  auto* power = barFor(cell, fighter.key, "powerBar");
  if (!hp || !shield || !power || hp->format() != "2600000/5000000" || shield->format() != QStringLiteral("护盾 450000") ||
      power->format() != "150/100" || hp->value() != 5200 || shield->value() != 9000 || power->value() != 10000)
    return fail("ghost segments modified exact balances or clamped the printed vigor");
  const auto hpBands = hp->property("actionGhostSegments").toList();
  const auto shieldBands = shield->property("actionGhostSegments").toList();
  const auto powerBands = power->property("actionGhostSegments").toList();
  if (hpBands.size() != 4 || shieldBands.size() != 2 || powerBands.size() != 1 ||
      hpBands.first().toMap()["kind"].toString() != "remove" || hpBands.last().toMap()["kind"].toString() != "add" ||
      shieldBands.first().toMap()["kind"].toString() != "add" || shieldBands.last().toMap()["kind"].toString() != "remove" ||
      powerBands.first().toMap()["after"].toLongLong() != 150 || hpBands.first().toMap()["before"].toLongLong() != 5000000)
    return fail("gain and loss ghost segments did not preserve their individual observations");
  if (!overlay.testAttribute(Qt::WA_TransparentForMouseEvents) || overlay.focusPolicy() != Qt::NoFocus ||
      overlay.property("textAngle").toReal() != -15 || overlay.property("lifetime").toString() != "meter-segment" ||
      overlay.property("recordCount").toInt() != 7)
    return fail("overlay captures input, fades independently, or loses a numeric record");
  const auto painted = overlay.property("paintedLines").toList();
  if (painted.size() != 3) return fail("one of the three meter callouts was not painted");
  const QMap<QString, QString> colors{{QStringLiteral("生命"), "#c7465b"},
    {QStringLiteral("护盾"), "#246fc2"}, {QStringLiteral("气势"), "#ba751d"}};
  bool outsideCard = false;
  for (const auto& item : painted) {
    const auto line = item.toMap(); const auto metric = line["metric"].toString();
    if (line["fontPixels"].toInt() < 21 || line["angle"].toInt() != -15 || line["color"].toString() != colors.value(metric) ||
        line["fighterKey"].toString() != fighter.key || line["anchor"].toRectF().contains(line["bounds"].toRectF()))
      return fail("rotated large red/blue/orange text is clipped to the bar or attached to another fighter");
    outsideCard |= !QRectF(cell.geometry()).contains(line["bounds"].toRectF());
    const auto tokens = line["tokens"].toList();
    if (metric == QStringLiteral("生命")) {
      if (tokens.size() != 4 || tokens[0].toMap()["text"].toString() != "-2200000" ||
          !tokens[0].toMap()["critical"].toBool() || tokens[0].toMap()["count"].toInt() != 1 ||
          tokens[1].toMap()["text"].toString() != "-300000" || !tokens[1].toMap()["critical"].toBool() ||
          tokens[1].toMap()["count"].toInt() != 1 || tokens[2].toMap()["text"].toString() != "-200000" ||
          tokens[2].toMap()["critical"].toBool() || tokens[3].toMap()["text"].toString() != "+300000" ||
          tokens[3].toMap()["critical"].toBool())
        return fail("same-group exact values were summed, or critical damage and recovery were combined");
    } else if (metric == QStringLiteral("护盾")) {
      if (tokens.size() != 2 || tokens[0].toMap()["text"].toString() != "+200000" ||
          tokens[1].toMap()["text"].toString() != "-50000") return fail("shield loss cancelled its gain");
    } else if (tokens.size() != 1 || tokens[0].toMap()["text"].toString() != "+50") return fail("vigor callout differs from the recorded delta");
  }
  if (!outsideCard) return fail("large meter callouts were kept inside the fighter card");
  const auto hitPoint = hp->mapTo(&board, hp->rect().center());
  auto* hit = board.childAt(hitPoint);
  if (!hit || hit == &overlay || (hit != hp && !hp->isAncestorOf(hit))) return fail("floating text intercepts the underlying meter");
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, hp->rect().center()); QCoreApplication::processEvents();
  auto* menu = hp->findChild<QMenu*>(QStringLiteral("replayMeterEventMenu"));
  if (!menu || menu->actions().size() != 4) return fail("meter no longer opens all exact constituent records");
  menu->close(); QCoreApplication::processEvents();
  // The page supplies an explicit active subset. History stays inspectable,
  // while only each meter's latest confirmed presentation segment remains.
  cue.activeMeterEvents = QList<ReplayUnitEvent>{cue.actionMeterEvents[3], cue.actionMeterEvents[5], cue.actionMeterEvents[6]};
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  overlay.setChanges(cell.floatingMeterChanges()); QCoreApplication::processEvents(); board.grab();
  const auto latestRows = overlay.property("recordedChanges").toList();
  if (latestRows.size() != 3 || overlay.property("recordCount").toInt() != 3)
    return fail("current segment still shows replaced damage or shield gain");
  const QMap<QString, QString> latestValues{{QStringLiteral("生命"), "+300000"},
    {QStringLiteral("护盾"), "-50000"}, {QStringLiteral("气势"), "+50"}};
  for (const auto& value : latestRows) {
    const auto row = value.toMap();
    if (row["text"].toString() != latestValues.value(row["metric"].toString()) || row["critical"].toBool())
      return fail("active recovery inherited a replaced critical hit or a wrong meter delta");
  }
  for (const auto& name : {QStringLiteral("hpBar"), QStringLiteral("shieldValue"), QStringLiteral("powerBar")}) {
    auto* bar = barFor(cell, fighter.key, name);
    if (!bar || bar->property("actionGhostSegments").toList().size() != 1)
      return fail("replaced observations remain as ghost segments");
  }
  hp = barFor(cell, fighter.key, "hpBar");
  if (!hp || hp->format() != "2600000/5000000") return fail("segment replacement changed the observed balance");
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, hp->rect().center()); QCoreApplication::processEvents();
  menu = hp->findChild<QMenu*>(QStringLiteral("replayMeterEventMenu"));
  if (!menu || menu->actions().size() != 4) return fail("replacing the visible number discarded previous damage records");
  menu->close(); QCoreApplication::processEvents();
  saveElevenCapture(board, QStringLiteral("action-meter-single-card-synthetic"));
  cue.actionMetersPending = false;
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  overlay.setChanges(cell.floatingMeterChanges()); QCoreApplication::processEvents();
  for (const auto& name : {QStringLiteral("hpBar"), QStringLiteral("shieldValue"), QStringLiteral("powerBar")}) {
    auto* bar = barFor(cell, fighter.key, name);
    if (!bar || bar->property("actionMetersPending").toBool() || !bar->property("actionGhostSegments").toList().isEmpty())
      return fail("finished action retained pending ghost intervals");
  }
  if (overlay.isVisible() || overlay.property("recordCount").toInt() != 0 ||
      barFor(cell, fighter.key, "hpBar")->format() != "2600000/5000000") return fail("settlement failed to clear callouts or changed balance");
  return true;
}

inline QByteArray boardFixture() {
  const auto unit = [](int team, int id, int x, int y) {
    return QJsonObject{{"id", id}, {"name", QStringLiteral("合成%1方精灵%2").arg(team).arg(id)}, {"x", x}, {"y", y},
      {"hp", 5000000}, {"maxHp", 5000000}, {"shp", 300000}, {"vigour", 100}, {"bfs", ""}};
  };
  QJsonArray teams;
  for (int team = 0; team < 2; ++team) {
    QJsonArray fighters;
    for (int row = 0; row < 3; ++row) fighters.append(unit(team, 7 + row, 0, row));
    teams.append(QJsonObject{{"id", team}, {"players", QJsonArray{QJsonObject{{"name", "synthetic"}, {"cts", fighters}}}}});
  }
  const QJsonObject first{
    {"cti", 0}, {"cpn", "synthetic"}, {"cci", 7}, {"si", 100001}, {"csn", 1}, {"ses", 1},
    {"cn", QJsonObject{{"bb_shps", "300113_200000_1"}, {"att_hps", "-10000"}}},
    {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "synthetic"}, {"tci", 7},
      {"isse", true}, {"iscr", true}, {"ishit", true},
      {"tn", QJsonObject{{"bb_hps", "300113_-100000_1"}, {"bb_shps", "300113_100000_1"},
        {"att_hps", "-2500000"}, {"att_shps", "-200000"}, {"att_vgs", "50"},
        {"ab_hps", "300113_300000_2"}, {"ab_shps", "300113_-50000_2"}}}}}},
    {"prl", QJsonArray{}}};
  const QJsonObject second{{"cti", 1}, {"cpn", "synthetic"}, {"cci", 7}, {"si", 100001}, {"csn", 1}, {"ses", 1},
    {"cn", QJsonObject{}}, {"trl", QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "synthetic"}, {"tci", 7},
      {"isse", true}, {"ishit", true}, {"tn", QJsonObject{{"att_hps", "-120000"}}}}}}, {"prl", QJsonArray{}}};
  const QJsonObject original{{"team", teams}};
  const QJsonArray flow{QJsonObject{{"pan", "synthetic"}, {"pbn", "synthetic"}, {"rl", QJsonArray{first, second}}}};
  return QJsonDocument(QJsonObject{{"br", QJsonObject{{"win", 0}, {"or", original}, {"fr", flow}}}}).toJson();
}

inline bool boardLifetime(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "action float board lifetime:" << reason; return false; };
  if (catalog["skills"].toObject()["100001"].toObject().isEmpty()) return fail("required ordinary playback definition absent");
  const auto bytes = boardFixture(); QTemporaryFile file;
  if (!file.open()) return fail("fixture file unavailable"); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1440, 940); if (!page.loadFile(file.fileName())) return fail("synthetic board report load");
  page.show(); QTest::qWait(20);
  QList<int> actions;
  for (int i = 0; i < page.replay().steps.size(); ++i)
    if (page.replay().steps[i].raw.toObject().contains("si")) actions.append(i);
  if (actions.size() != 2) return fail("synthetic actions were not parsed");
  auto* overlay = page.findChild<QWidget*>(QStringLiteral("replayMeterOverlay"));
  if (!overlay) return fail("board-level overlay missing");
  const auto source = BattleReplay::fighterKey(0, "synthetic", "7");
  const auto target = BattleReplay::fighterKey(1, "synthetic", "7");
  page.seek(actions[0]); const auto& step = page.replay().steps[actions[0]];
  if (step.playbackKind != "normal") return fail("synthetic report was not eligible for ordinary official playback");
  bool sawBefore = false, sawAttack = false, sawAfter = false, captured = false;
  int attackPhase = -1, afterPhase = -1; QVariantList afterRecords;
  const auto indexFor = [&](const QString& key, const QString& suffix) {
    for (int i = 0; i < step.effects.size(); ++i)
      if (step.effects[i].target == key && step.effects[i].path.endsWith(suffix)) return i;
    return -1;
  };
  const int sourceHp = indexFor(source, "/cn/att_hps"), sourceShield = indexFor(source, "/cn/bb_shps");
  const int beforeHp = indexFor(target, "/tn/bb_hps"), beforeShield = indexFor(target, "/tn/bb_shps");
  const int attackHp = indexFor(target, "/tn/att_hps"), attackShield = indexFor(target, "/tn/att_shps");
  const int attackPower = indexFor(target, "/tn/att_vgs");
  const int afterHp = indexFor(target, "/tn/ab_hps"), afterShield = indexFor(target, "/tn/ab_shps");
  const QMap<int, QString> exactValues{{sourceHp, "-10000"}, {sourceShield, "+200000"},
    {beforeHp, "-100000"}, {beforeShield, "+100000"}, {attackHp, "-2500000"},
    {attackShield, "-200000"}, {attackPower, "+50"}, {afterHp, "+300000"}, {afterShield, "-50000"}};
  if (exactValues.size() != 9 || exactValues.contains(-1)) return fail("fixture's nine exact numeric records were not parsed");
  const auto checkExactState = [&] {
    const int moment = page.property("playbackLastMoment").toInt();
    if (moment < 0 || moment >= step.moments.size() || !replayStateMatches(page, step.moments[moment].state)) return false;
    for (const auto& key : {source, target}) {
      const auto f = page.displayedState()[key];
      const auto text = [](std::optional<qint64> value) { return value ? QString::number(*value) : QStringLiteral("?"); };
      auto* hp = barFor(page, key, "hpBar"); auto* shield = barFor(page, key, "shieldValue"); auto* power = barFor(page, key, "powerBar");
      if (!hp || !shield || !power || hp->format() != text(f.hp) + QLatin1Char('/') + text(f.maxHp) ||
          shield->format() != QStringLiteral("护盾 ") + text(f.shield) ||
          power->format() != text(f.power) + QLatin1Char('/') + text(f.maxPower)) return false;
    }
    return true;
  };
  for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
    page.seekPlaybackPhase(phase); QCoreApplication::processEvents();
    if (!checkExactState()) return fail("a pending band or callout changed an exact observed snapshot");
    const auto kind = page.playbackPhaseKind(); const int through = page.property("playbackLastMoment").toInt();
    const auto rows = overlay->property("recordedChanges").toList();
    if (overlay->isVisible()) {
      auto* arrows = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
      if (!arrows) return fail("relation layer missing");
      arrows->raise(); // Includes the raise performed by a delayed layout.
      const auto layers = overlay->parentWidget()->children();
      if (layers.indexOf(overlay) <= layers.indexOf(arrows)) return fail("arrows cover the damage callouts after a layout change");
    }
    if (kind == "finish") {
      if (overlay->isVisible() || !rows.isEmpty()) return fail("finish did not remove action callouts");
      for (auto* bar : page.findChildren<QProgressBar*>())
        if (bar->property("hasActionMeterContext").toBool() &&
            (bar->property("actionMetersPending").toBool() || !bar->property("actionGhostSegments").toList().isEmpty()))
          return fail("finish retained pending bands");
      continue;
    }
    // These are the fixture's five meter timelines, written from its input
    // records. No projection implementation is used to produce the oracle.
    QSet<int> expected;
    const QList<QList<int>> timelines{{sourceHp}, {sourceShield}, {beforeHp, attackHp, afterHp},
      {beforeShield, attackShield, afterShield}, {attackPower}};
    for (const auto& timeline : timelines) {
      int active = -1;
      for (int index : timeline) if (step.effects[index].moment <= through) active = index;
      if (active >= 0) expected.insert(active);
    }
    QSet<int> actual;
    for (const auto& value : rows) {
      const auto row = value.toMap(); const int index = row["effectIndex"].toInt();
      if (index < 0 || index >= step.effects.size() || actual.contains(index)) return fail("a numeric observation was duplicated or misidentified");
      actual.insert(index); const auto& effect = step.effects[index];
      if (row["fighterKey"].toString() != effect.target || row["text"].toString() != exactValues.value(index))
        return fail("a numeric callout moved to another recipient, was summed or changed sign");
      const bool critical = effect.target == target && effect.cause == "skill" &&
        (effect.path.endsWith("/tn/att_hps") || effect.path.endsWith("/tn/att_shps"));
      if (row["critical"].toBool() != critical) return fail("target critical leaked to self-cost, Buff tick or vigor");
    }
    if (actual != expected) return fail("phase transition kept replaced numbers, cleared another meter, or showed a future record");
    if (kind == "before" && actual.contains(beforeHp)) sawBefore = true;
    if (kind == "attack" && actual.contains(attackHp)) { sawAttack = true; attackPhase = phase; }
    if (kind == "after" && actual.contains(afterHp) && actual.contains(afterShield)) {
      sawAfter = true; afterPhase = phase; afterRecords = rows;
      if (!captured) {
        if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"))) {
          saveElevenCapture(*board, QStringLiteral("action-meter-full-board-synthetic")); captured = true;
        }
      }
    }
  }
  if (!sawBefore || !sawAttack || !sawAfter || afterPhase < 0 || attackPhase < 0 || afterRecords.size() != 5)
    return fail("fixture did not exercise all before/attack/after observations");
  page.seekPlaybackPhase(afterPhase); QCoreApplication::processEvents();
  if (overlay->property("recordedChanges").toList() != afterRecords) return fail("re-seeking produced different action evidence");
  QTest::qWait(460);
  if (overlay->property("recordedChanges").toList() != afterRecords || !overlay->isVisible())
    return fail("numbers disappeared before the next meter change or action boundary");
  if (!captured) {
    auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"));
    if (!board) return fail("actual battle board unavailable for capture");
    saveElevenCapture(*board, QStringLiteral("action-meter-full-board-synthetic"));
  }
  auto* numberFormat = page.findChild<QComboBox*>(QStringLiteral("replayNumberFormat"));
  auto* targetHp = barFor(page, target, "hpBar");
  if (!numberFormat || !targetHp || numberFormat->findData("yi-wan") < 0) return fail("number format selector missing");
  const auto ghostSegments = targetHp->property("actionGhostSegments");
  const auto progress = page.property("playbackActionProgress");
  const auto moment = page.currentMoment();
  const auto segment = page.property("numericSegment"), segmentCount = page.property("numericSegmentCount");
  numberFormat->setCurrentIndex(numberFormat->findData("yi-wan"));
  QCoreApplication::processEvents(); page.grab();
  if (page.currentPlaybackPhase() != afterPhase || page.currentMoment() != moment || page.property("playbackActionProgress") != progress ||
      targetHp != barFor(page, target, "hpBar") || targetHp->property("actionGhostSegments") != ghostSegments ||
      overlay->property("recordedChanges").toList() != afterRecords || page.replay().original != bytes ||
      page.property("numericSegment") != segment || page.property("numericSegmentCount") != segmentCount)
    return fail("number format switch restarted the action, rebuilt cards or changed recorded evidence");
  if (targetHp->format() != QStringLiteral("270万/500万") ||
      barFor(page, target, "shieldValue")->format() != QStringLiteral("护盾 15万") ||
      barFor(page, target, "powerBar")->format() != QStringLiteral("150/?"))
    return fail("grouped balances lost precision, shield semantics or an unknown vigor threshold");
  bool criticalGrouped = false, gainGrouped = false;
  for (const auto& entry : overlay->property("paintedLines").toList()) {
    const auto line = entry.toMap(); if (line["fighterKey"].toString() != target || line["metric"].toString() != QStringLiteral("生命")) continue;
    for (const auto& value : line["tokens"].toList()) {
      const auto token = value.toMap();
      criticalGrouped |= token["text"].toString() == QStringLiteral("-250万") && token["critical"].toBool();
      gainGrouped |= token["text"].toString() == QStringLiteral("+30万") && !token["critical"].toBool();
    }
  }
  if (criticalGrouped || !gainGrouped) return fail("recovery lost its sign or still displayed the previous grouped critical damage");
  QTest::mouseClick(targetHp, Qt::LeftButton, Qt::NoModifier, targetHp->rect().center()); QCoreApplication::processEvents();
  auto* history = targetHp->findChild<QMenu*>(QStringLiteral("replayMeterEventMenu"));
  if (!history || history->actions().size() != 3) return fail("current recovery no longer exposes both prior damage records");
  history->close(); QCoreApplication::processEvents();
  page.seekPlaybackPhase(attackPhase); QCoreApplication::processEvents(); page.grab();
  bool currentCritical = false, futureRecovery = false;
  for (const auto& entry : overlay->property("paintedLines").toList()) {
    const auto line = entry.toMap(); if (line["fighterKey"].toString() != target || line["metric"].toString() != QStringLiteral("生命")) continue;
    for (const auto& value : line["tokens"].toList()) {
      const auto token = value.toMap();
      currentCritical |= token["text"].toString() == QStringLiteral("-250万") && token["critical"].toBool();
      futureRecovery |= token["text"].toString() == QStringLiteral("+30万");
    }
  }
  if (!currentCritical || futureRecovery) return fail("rewind lost a current critical hit or retained future recovery");
  page.seekPlaybackPhase(afterPhase); QCoreApplication::processEvents();
  if (barFor(page, target, "hpBar")->format() != QStringLiteral("270万/500万")) return fail("number format was lost on replay refresh");
  if (auto* arena = page.findChild<QWidget*>(QStringLiteral("replayArena")))
    saveElevenCapture(*arena, QStringLiteral("action-meter-grouped-board-synthetic"));
  numberFormat->setCurrentIndex(numberFormat->findData("full")); QCoreApplication::processEvents();
  if (!checkExactState() || overlay->property("recordedChanges").toList() != afterRecords)
    return fail("switching back did not restore the complete exact numbers");
  page.seekPlaybackPhase(0); QCoreApplication::processEvents();
  if (overlay->property("recordCount").toInt() || overlay->isVisible()) return fail("rewind retained future changes");
  page.seekPlaybackPhase(afterPhase);
  if (overlay->property("recordedChanges").toList() != afterRecords) return fail("forward seek accumulated duplicate changes");
  page.seek(actions[1]); page.seekPlaybackPhase(0); QCoreApplication::processEvents();
  if (overlay->property("recordCount").toInt() || overlay->isVisible()) return fail("next actor inherited previous action callouts");
  for (auto* bar : page.findChildren<QProgressBar*>())
    if (bar->property("actionMeterEventCount").toInt() || !bar->property("actionGhostSegments").toList().isEmpty())
      return fail("next actor inherited previous action segments or click history");
  page.seek(actions[0]); page.seekMoment(0);
  if (overlay->property("recordCount").toInt() || overlay->isVisible() || page.replay().original != bytes)
    return fail("microstep rewind leaked future numbers or presentation altered original report");
  return true;
}

inline bool successiveMeterSegments(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "successive meter segments:" << reason; return false; };
  auto root = QJsonDocument::fromJson(boardFixture()).object(); auto br = root["br"].toObject();
  auto flow = br["fr"].toArray(); auto battle = flow[0].toObject(); auto action = battle["rl"].toArray()[0].toObject();
  action["cn"] = QJsonObject{};
  auto targets = action["trl"].toArray(); auto targetRecord = targets[0].toObject();
  targetRecord["tn"] = QJsonObject{
    {"bb_hps", "300113_-100000_1,300113_-200000_2,300113_50000_3"},
    {"bb_shps", "300113_100000_1,300113_-50000_4"}, {"att_hps", "-2500000"}};
  targets[0] = targetRecord; action["trl"] = targets; battle["rl"] = QJsonArray{action};
  flow[0] = battle; br["fr"] = flow; root["br"] = br; const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return fail("fixture file unavailable"); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1440, 940); if (!page.loadFile(file.fileName())) return fail("report load");
  page.show(); QTest::qWait(20);
  int actionIndex = -1;
  for (int i = 0; i < page.replay().steps.size(); ++i)
    if (page.replay().steps[i].raw.toObject().contains("si")) { actionIndex = i; break; }
  if (actionIndex < 0) return fail("action missing"); page.seek(actionIndex);
  const auto& step = page.replay().steps[actionIndex]; const auto target = BattleReplay::fighterKey(1, "synthetic", "7");
  auto* overlay = page.findChild<QWidget*>(QStringLiteral("replayMeterOverlay"));
  if (!overlay) return fail("numeric overlay missing");
  struct Wanted { qint64 hp, shield; QString hpText, shieldText; qint64 hpBefore, shieldBefore; };
  const QList<Wanted> wanted{
    {4900000, 400000, "-100000", "+100000", 5000000, 300000},
    {4700000, 400000, "-200000", "+100000", 4900000, 300000},
    {4750000, 400000, "+50000", "+100000", 4700000, 300000},
    {4750000, 350000, "+50000", "-50000", 4700000, 400000}
  };
  QMap<int, int> phases;
  const auto check = [&](int group) {
    const auto& expected = wanted[group]; const auto& fighter = page.displayedState()[target];
    if (fighter.hp != expected.hp || fighter.shield != expected.shield) return false;
    const auto rows = overlay->property("recordedChanges").toList();
    if (rows.size() != 2) return false;
    QMap<QString, QString> texts;
    for (const auto& value : rows) {
      const auto row = value.toMap();
      if (row["fighterKey"].toString() != target || row["critical"].toBool()) return false;
      texts[row["metric"].toString()] = row["text"].toString();
    }
    if (texts.value(QStringLiteral("生命")) != expected.hpText || texts.value(QStringLiteral("护盾")) != expected.shieldText) return false;
    auto* hp = barFor(page, target, "hpBar"); auto* shield = barFor(page, target, "shieldValue");
    if (!hp || !shield || hp->format() != QString::number(expected.hp) + "/5000000" ||
        shield->format() != QStringLiteral("护盾 ") + QString::number(expected.shield)) return false;
    const auto hpBands = hp->property("actionGhostSegments").toList(), shieldBands = shield->property("actionGhostSegments").toList();
    if (hpBands.size() != 1 || shieldBands.size() != 1) return false;
    const auto hpBand = hpBands[0].toMap(), shieldBand = shieldBands[0].toMap();
    return hpBand["before"].toLongLong() == expected.hpBefore && hpBand["after"].toLongLong() == expected.hp &&
      shieldBand["before"].toLongLong() == expected.shieldBefore && shieldBand["after"].toLongLong() == expected.shield &&
      hpBand["kind"].toString() == (group < 2 ? "remove" : "add") &&
      shieldBand["kind"].toString() == (group < 3 ? "add" : "remove");
  };
  for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
    page.seekPlaybackPhase(phase); QCoreApplication::processEvents();
    const int m = page.property("playbackLastMoment").toInt();
    if (m < 0 || m >= step.moments.size()) continue;
    const auto& moment = step.moments[m];
    if (page.playbackPhaseKind() != "before" || moment.part != "values" || moment.groupIndex < 0 || moment.groupIndex >= wanted.size()) continue;
    phases[moment.groupIndex] = phase;
    if (!check(moment.groupIndex) || !replayStateMatches(page, moment.state))
      return fail("same-direction damage or recovery retained its predecessor, changed the balance, or cleared the independent shield segment");
  }
  if (phases.size() != 4) return fail("four distinct recorded changeset groups were not exercised");
  for (int group : {1, 3, 0, 2}) {
    page.seekPlaybackPhase(phases[group]); QCoreApplication::processEvents();
    if (!check(group)) return fail("backward or forward seek accumulated stale numeric segments");
  }
  auto* hp = barFor(page, target, "hpBar");
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, hp->rect().center()); QCoreApplication::processEvents();
  auto* history = hp->findChild<QMenu*>(QStringLiteral("replayMeterEventMenu"));
  if (!history || history->actions().size() != 3) return fail("replacement removed an earlier same-direction damage record from history");
  history->close();
  page.seekPlaybackPhase(page.playbackPhaseCount() - 1); QCoreApplication::processEvents();
  if (overlay->isVisible() || overlay->property("recordCount").toInt() || page.replay().original != bytes)
    return fail("settlement retained a segment or changed the original report");
  return true;
}

inline bool counterSourceBoundary(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "meter counter source boundary:" << reason; return false; };
  if (catalog["skills"].toObject()["106986"].toObject().isEmpty()) return fail("required official close-combat definition absent");
  auto root = QJsonDocument::fromJson(boardFixture()).object(); auto br = root["br"].toObject();
  auto flow = br["fr"].toArray(); auto battle = flow[0].toObject(); auto action = battle["rl"].toArray()[0].toObject();
  action["si"] = 106986; action["fbs"] = false;
  action["cn"] = QJsonObject{{"att_hps", "-10000"}, {"sb_hps", "-80000"}};
  auto targets = action["trl"].toArray(); auto targetRecord = targets[0].toObject(); targetRecord["issb"] = true;
  targetRecord["tn"] = QJsonObject{{"att_hps", "-2500000"}, {"sb_hps", "-20000"}};
  targets[0] = targetRecord; action["trl"] = targets; battle["rl"] = QJsonArray{action};
  flow[0] = battle; br["fr"] = flow; root["br"] = br; const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return fail("fixture file unavailable"); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1440, 940); if (!page.loadFile(file.fileName())) return fail("report load");
  page.show(); QTest::qWait(20);
  int actionIndex = -1;
  for (int i = 0; i < page.replay().steps.size(); ++i)
    if (page.replay().steps[i].raw.toObject().contains("si")) { actionIndex = i; break; }
  if (actionIndex < 0) return fail("action missing"); page.seek(actionIndex);
  const auto& step = page.replay().steps[actionIndex];
  const auto source = BattleReplay::fighterKey(0, "synthetic", "7"), target = BattleReplay::fighterKey(1, "synthetic", "7");
  auto* overlay = page.findChild<QWidget*>(QStringLiteral("replayMeterOverlay")); if (!overlay) return fail("numeric overlay missing");
  int attackPhase = -1, castPhase = -1, counterPhase = -1;
  for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
    page.seekPlaybackPhase(phase); QCoreApplication::processEvents();
    const auto kind = page.playbackPhaseKind(); const auto rows = overlay->property("recordedChanges").toList();
    if (kind == "attack") {
      attackPhase = phase;
      bool damage = false;
      for (const auto& value : rows) {
        const auto row = value.toMap();
        damage |= row["fighterKey"].toString() == target && row["text"].toString() == "-2500000" && row["critical"].toBool();
      }
      if (!damage) return fail("first attack lacked its explicitly critical damage");
    } else if (kind == "counter-cast") {
      castPhase = phase;
      if (!rows.isEmpty() || overlay->isVisible()) return fail("counter caster inherited the original actor's floating damage");
      for (auto* bar : page.findChildren<QProgressBar*>())
        if (!bar->property("actionGhostSegments").toList().isEmpty()) return fail("counter cast retained the original actor's pending bands");
    } else if (kind == "counter") {
      counterPhase = phase; QMap<QString, QString> amounts;
      for (const auto& value : rows) {
        const auto row = value.toMap(); const int index = row["effectIndex"].toInt();
        if (index < 0 || index >= step.effects.size() || step.effects[index].cause != "counter" || row["critical"].toBool())
          return fail("counter inherited a different cause or original critical flag");
        amounts[row["fighterKey"].toString()] = row["text"].toString();
      }
      if (rows.size() != 2 || amounts.value(source) != "-80000" || amounts.value(target) != "-20000")
        return fail("counter changed either exact self-cost or damage amount");
      if (page.displayedState()[source].hp != 4910000 || page.displayedState()[target].hp != 2480000)
        return fail("counter segment changed the recorded balances");
    }
  }
  if (attackPhase < 0 || castPhase <= attackPhase || counterPhase <= castPhase) return fail("explicit attack, counter cast and counter result were not exercised");
  page.seekPlaybackPhase(counterPhase); QCoreApplication::processEvents();
  auto* hp = barFor(page, source, "hpBar"); if (!hp) return fail("original actor's life bar absent");
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, hp->rect().center()); QCoreApplication::processEvents();
  auto* history = hp->findChild<QMenu*>(QStringLiteral("replayMeterEventMenu"));
  if (!history || history->actions().size() != 2) return fail("counter cast discarded the earlier source cost from history");
  history->close();
  page.seekPlaybackPhase(attackPhase); QCoreApplication::processEvents();
  for (const auto& value : overlay->property("recordedChanges").toList()) {
    const int index = value.toMap()["effectIndex"].toInt();
    if (index < 0 || index >= step.effects.size() || step.effects[index].cause == "counter") return fail("rewinding the counter retained future damage");
  }
  page.seekPlaybackPhase(castPhase); QCoreApplication::processEvents();
  if (overlay->isVisible() || overlay->property("recordCount").toInt() || page.replay().original != bytes)
    return fail("returning to the counter-cast boundary retained prior or future callouts");
  return true;
}
} // namespace ReplayActionFloatChecks

inline bool verifyReplayActionFloatPresentation(const QJsonObject& catalog) {
  if (!ReplayActionFloatChecks::exactNumberGroups() || !ReplayActionFloatChecks::criticalAttribution() || !ReplayActionFloatChecks::prefixProjection() ||
      !ReplayActionFloatChecks::cardAndOverlay(catalog) || !ReplayActionFloatChecks::boardLifetime(catalog) ||
      !ReplayActionFloatChecks::successiveMeterSegments(catalog) || !ReplayActionFloatChecks::counterSourceBoundary(catalog)) return false;
  qInfo() << "meter-segment ghosts, exact unmerged callouts, independent meter lifetimes, critical provenance and actor/seek boundaries PASS";
  return true;
}
