#include "ui/battle/battle_replay_page.h"
#include "battle_replay_preview_catalog.h"
#include "ui/battle/battle_replay_cell.h"
#include "ui/battle/battle_replay_spirit.h"
#include "ui/battle/battle_replay_links.h"
#include "ui/battle/battle_replay_meter_sequence.h"
#include "domain/battle_replay_empowerment.h"
#include "battle_replay_effect_audit.h"
#include "ui/workbench/workbench_window.h"
#include "ui/common/ui_preferences.h"
#include "fixtures/battle_replay_fixture.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QGraphicsEffect>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTableView>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QUrl>
#include <algorithm>
#include <functional>

QMap<QString, BattleReplay::Fighter> expectedReadingState(const BattleReplayPage& page);

// The embedded workbench hides on Close to keep the game alive. A standalone
// preview must instead end its own process when the user closes its window.
class PreviewCloseFilter final : public QObject {
  bool eventFilter(QObject* object, QEvent* event) override {
    if (event->type() == QEvent::Close) QCoreApplication::quit();
    return QObject::eventFilter(object, event);
  }
};

bool clickReplayTextAnchor(QTextBrowser* view, const QString& href) {
  for (auto block = view->document()->begin(); block.isValid(); block = block.next())
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const auto fragment = it.fragment();
      if (fragment.charFormat().anchorHref() != href || fragment.length() < 1) continue;
      QTextCursor cursor(view->document()); cursor.setPosition(fragment.position());
      view->setTextCursor(cursor); view->ensureCursorVisible(); QCoreApplication::processEvents();
      const auto caret = view->cursorRect(cursor); const auto point = QPoint(caret.left() + 2, caret.center().y());
      if (!view->viewport()->rect().contains(point) || view->anchorAt(point) != href) return false;
      QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point); return true;
    }
  return false;
}

bool hasReplayTextAnchor(const QTextBrowser* view, const QString& href) {
  if (!view) return false;
  for (auto block = view->document()->begin(); block.isValid(); block = block.next())
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const auto actual = it.fragment().charFormat().anchorHref();
      if (!actual.isEmpty() && QUrl(actual) == QUrl(href)) return true;
    }
  return false;
}

bool activateReplayTextAnchor(QTextBrowser* view, const QString& href) {
  return hasReplayTextAnchor(view, href) &&
    QMetaObject::invokeMethod(view, "anchorClicked", Q_ARG(QUrl, QUrl(href)));
}

QByteArray stateLedger(const QMap<QString, BattleReplay::Fighter>& state) {
  QByteArray bytes;
  for (const auto& f : state) {
    auto buffs = f.buffs; std::sort(buffs.begin(), buffs.end());
    bytes += (f.key + QLatin1Char('\t') + (f.shield ? QString::number(*f.shield) : QStringLiteral("?")) +
      QLatin1Char('\t') + buffs.join(QLatin1Char(',')) + QLatin1Char('\t') + (f.buffsKnown ? QStringLiteral("1") : QStringLiteral("0")) + QLatin1Char('\n')).toUtf8();
  }
  return bytes;
}

QByteArray battlefieldLedger(const QMap<QString, BattleReplay::Fighter>& state) {
  QByteArray bytes;
  for (const auto& f : state) {
    const QStringList fields{f.key, f.raceId, f.faceId, f.normalSkill, f.uniqueSkill, f.superUniqueSkill,
      f.attributes.join(QLatin1Char(',')), f.attributesKnown ? QStringLiteral("1") : QStringLiteral("0"),
      f.maxPower ? QString::number(*f.maxPower) : QStringLiteral("?"), f.entered ? QStringLiteral("1") : QStringLiteral("0"),
      f.displayLayersRestored ? QStringLiteral("1") : QStringLiteral("0"), f.displayLayersPath,
      f.cloneCalled ? QStringLiteral("1") : QStringLiteral("0"), QString::number(f.playerX), QString::number(f.playerY),
      f.playerRemoved ? QStringLiteral("1") : QStringLiteral("0")};
    bytes += (fields.join(QLatin1Char('\t')) + QLatin1Char('\n')).toUtf8();
  }
  return bytes;
}

bool verifyFormationLayout() {
  // Independent screen orders from the official SingalLocator_default
  // placement matrices; see tools/battle_replay/formation-evidence.json.
  const std::array<std::array<int, 9>, 2> expected{{
    {{7, 4, 1, 8, 5, 2, 9, 6, 3}}, {{1, 4, 7, 2, 5, 8, 3, 6, 9}}
  }};
  QJsonArray teams;
  for (int team = 0; team < 2; ++team) {
    QJsonArray fighters;
    for (int slot = 1; slot <= 9; ++slot) {
      fighters.append(QJsonObject{{"id", slot}, {"name", QStringLiteral("合成阵位 t%1-f%2").arg(team).arg(slot)},
        {"x", (slot - 1) % 3}, {"y", (slot - 1) / 3}, {"hp", 100}, {"maxHp", 100}, {"vigour", 50}, {"shp", 0}});
    }
    fighters.append(QJsonObject{{"id", 91}, {"name", QStringLiteral("合成同格 t%1").arg(team)},
      {"x", 0}, {"y", 0}, {"icr", true}, {"hp", 100}, {"maxHp", 100}});
    const QJsonObject player{{"name", QStringLiteral("synthetic-team-%1").arg(team)}, {"cts", fighters}};
    teams.append(QJsonObject{{"id", team}, {"players", QJsonArray{player}}});
  }
  const QJsonObject root{{"br", QJsonObject{{"or", QJsonObject{{"team", teams}}}, {"fr", QJsonArray{}}, {"win", -1}}}};
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile fixture; if (!fixture.open()) return false;
  fixture.write(bytes); fixture.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog());
  if (!page.loadFile(fixture.fileName())) return false;
  auto* selector = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  if (!selector) return false;
  for (int ownTeam : {0, 1, 0}) {
    selector->setCurrentIndex(selector->findData(ownTeam));
    // End and beginning use the same projection; switching must not mutate logs.
    for (int step : {1, 0}) {
      page.seek(step);
      if (page.replay().original != bytes) return false;
      for (int side = 0; side < 2; ++side) {
        const int team = side == 0 ? ownTeam : 1 - ownTeam;
        for (int cell = 0; cell < 9; ++cell) {
          auto* view = page.findChild<ReplayCell*>(QStringLiteral("replayCell%1").arg(side * 9 + cell));
          if (!view) return false;
          auto* grid = view->parentWidget()->findChild<QGridLayout*>();
          if (!grid || !grid->itemAtPosition(cell / 3, cell % 3) || grid->itemAtPosition(cell / 3, cell % 3)->widget() != view) return false;
          const int slot = expected[side][cell];
          const auto text = view->toPlainText();
          if (view->property("formationSlot").toInt() != slot ||
              !text.contains(QStringLiteral("合成阵位 t%1-f%2").arg(team).arg(slot))) return false;
          // Position remains exact, but the visible card no longer spends a
          // heading row on its slot number.
          for (const auto* label : view->findChildren<QLabel*>())
            if (label->text().contains(QStringLiteral("号位"))) return false;
          if (text.contains(QStringLiteral("前排")) || text.contains(QStringLiteral("中排")) || text.contains(QStringLiteral("后排"))) return false;
          if (slot == 1 && (!text.contains(QStringLiteral("同格 2 名")) || !text.contains(QStringLiteral("合成同格 t%1").arg(team)))) return false;
        }
      }
    }
  }
  return true;
}

bool verifyStateNavigation() {
  // Synthetic numbers with an existing official close-skill command profile.
  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106986");
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false; page.seek(2);
  const auto key = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  auto* next = page.findChild<QPushButton*>(QStringLiteral("replayNextMoment"));
  auto* prev = page.findChild<QPushButton*>(QStringLiteral("replayPreviousMoment"));
  auto* choices = page.findChild<QComboBox*>(QStringLiteral("replayMoments"));
  auto* cell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  if (!next || !prev || !choices || !cell || choices->count() < 6) return false;
  if (page.displayedState()[key].shield != -33 || !cell->toPlainText().contains(QStringLiteral("护盾 -33"))) return false;
  QTest::mouseClick(next, Qt::LeftButton);
  if (page.currentMoment() != 0 || page.displayedState()[key].shield != 50 || !cell->toPlainText().contains(QStringLiteral("护盾 50"))) return false;
  auto* name = cell->findChild<QPushButton*>(QStringLiteral("unitName"));
  if (!name) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  auto* unit = page.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
  if (!unit || !unit->toPlainText().contains(QStringLiteral("当前效果"))) return false;
  const auto initialUnitText = unit->toPlainText();
  while (next->isEnabled()) QTest::mouseClick(next, Qt::LeftButton);
  if (page.displayedState()[key].hp != 77 || page.displayedState()[key].shield != -33 || unit->toPlainText() == initialUnitText) return false;
  while (prev->isEnabled()) QTest::mouseClick(prev, Qt::LeftButton);
  if (page.currentMoment() != 0 || page.displayedState()[key].buffs.count("99") != 2 || unit->toPlainText() != initialUnitText) return false;
  page.seekMoment(-1);
  return page.replay().original == bytes && page.displayedState()[key].shield == -33;
}

bool verifyMeters() {
  // Deliberately exceed both visual references: text must not be clamped,
  // coerced to int32, or replaced by QProgressBar's percentage formatting.
  BattleReplay::Fighter f;
  f.key = QStringLiteral("meter-fixture"); f.name = QStringLiteral("合成大数值");
  f.hp = 12345678901LL; f.maxHp = 22345678901LL;
  f.shield = 32345678901LL; f.power = 330; f.maxPower = 150;
  ReplayCell cell(0); cell.display(1, {f}, {}, {}, {});
  auto* hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  auto* shield = cell.findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  auto* power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  if (!hp || !shield || !power || hp->text() != QStringLiteral("12345678901/22345678901") ||
      shield->text() != QStringLiteral("护盾 32345678901") || power->text() != QStringLiteral("330/150")) return false;
  if (hp->value() <= 0 || hp->value() >= hp->maximum() || cell.findChild<QProgressBar*>(QStringLiteral("shieldBar")) || power->value() != power->maximum()) return false;
  f.shield = -33; f.power.reset(); f.maxPower.reset();
  cell.display(1, {f}, {}, {}, {});
  power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  shield = cell.findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  return power->text() == QStringLiteral("?/?") && power->maximum() > 0 &&
    shield->text() == QStringLiteral("护盾 -33");
}

bool verifyBattlefieldNavigation() {
  // UI uses embedded official IDs; the domain fixture separately exercises
  // deliberately synthetic form IDs and the cu=0 fallback.
  auto bytes = battleReplayBattlefieldFixture(); bytes.replace("999999999", "106986");
  bytes.replace("9001", "6980"); bytes.replace("9002", "6981");
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  auto* cell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* player = page.findChild<QComboBox*>(QStringLiteral("replayPlayer0"));
  auto* unitFilter = page.findChild<QComboBox*>(QStringLiteral("replayUnitFilter"));
  auto* important = page.findChild<QCheckBox*>(QStringLiteral("replayImportant"));
  auto* timeline = page.findChild<QTableView*>(QStringLiteral("replayTimeline"));
  auto* kind = page.findChild<QComboBox*>(QStringLiteral("replayEventType"));
  auto* nextEvent = page.findChild<QPushButton*>(QStringLiteral("replayNextEvent"));
  if (!cell || !player || !unitFilter || !important || !timeline || !kind || !nextEvent) return false;
  page.seek(1);
  const auto sourceKey = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  const auto sourceName = BattleReplay::fighterName(page.displayedState()[sourceKey]);
  if (!cell->toPlainText().contains(sourceName) || cell->toPlainText().contains(QStringLiteral("synthetic source")) || cell->toPlainText().contains(QStringLiteral("other formation"))) return false;
  player->setCurrentIndex(player->findData(QStringLiteral("reserve-player")));
  QMetaObject::invokeMethod(player, "activated", Q_ARG(int, player->currentIndex()));
  if (!cell->toPlainText().contains(QStringLiteral("other formation")) || cell->toPlainText().contains(sourceName)) return false;
  page.seek(2);
  if (player->currentData().toString() != QStringLiteral("tester")) return false;
  const auto enemy = BattleReplay::fighterKey(1, QStringLiteral("foe"), QStringLiteral("7"));
  unitFilter->setCurrentIndex(unitFilter->findData(enemy)); important->setChecked(true);
  if (timeline->model()->rowCount() != 1) return false;
  page.seek(0); kind->setCurrentIndex(kind->findText(QStringLiteral("生命归零")));
  QTest::mouseClick(nextEvent, Qt::LeftButton);
  if (page.currentStep() != 2 || page.currentMoment() < 0 || page.displayedState()[enemy].hp != 0) return false;
  auto* enemyCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
  if (!enemyCell || !enemyCell->property("deadUnit").toBool() || !enemyCell->graphicsEffect() || !enemyCell->graphicsEffect()->isEnabled()) return false;
  const auto atDeath = page.currentMoment();
  kind->setCurrentIndex(kind->findText(QStringLiteral("生命恢复"))); QTest::mouseClick(nextEvent, Qt::LeftButton);
  if (page.currentStep() != 2 || page.currentMoment() <= atDeath || page.displayedState()[enemy].hp != 200) return false;
  if (enemyCell->property("deadUnit").toBool()) return false;
  unitFilter->setCurrentIndex(0); important->setChecked(false);
  page.seek(0);
  return !page.displayedState()[BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("9"))].entered && page.replay().original == bytes;
}

bool verifyEffectsAndSpirit() {
  const auto fail = [](const QString& reason, const QString& actual = {}) {
    qCritical().noquote() << "effect/Buff/spirit:" << reason << actual; return false;
  };
  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106986");
  QTemporaryFile file; if (!file.open()) return fail(QStringLiteral("cannot open fixture")); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return fail(QStringLiteral("cannot import fixture"));
  auto* spirit = page.findChild<ReplaySpiritBar*>(QStringLiteral("replaySpirit0"));
  if (!spirit || !spirit->toPlainText().contains(QStringLiteral("2/10")))
    return fail(QStringLiteral("initial main spirit balance"), spirit ? spirit->toPlainText() : QStringLiteral("missing control"));
  page.seek(2);
  auto* effects = page.findChild<QListWidget*>(QStringLiteral("replayEffects"));
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  auto* relation = page.findChild<QLabel*>(QStringLiteral("replayRelation"));
  if (!effects || !links || !relation || effects->count() < 2) return fail(QStringLiteral("missing effect controls or records"));
  bool known = false, unknown = false, counter = false;
  const auto& step = page.replay().steps[2];
  for (int row = 0; row < effects->count(); ++row) {
    const auto index = effects->item(row)->data(Qt::UserRole).toInt();
    const auto& e = step.effects[index];
    if ((!known && e.cause == QStringLiteral("skill")) || (!unknown && e.kind == QStringLiteral("buff")) || (!counter && e.cause == QStringLiteral("counter"))) {
      effects->setCurrentRow(row);
      if (page.currentMoment() != e.moment || links->property("hasKnownSource").toBool() != !e.source.isEmpty() || !links->property("hasVisibleTarget").toBool())
        return fail(QStringLiteral("selected effect relation does not match recorded identity"),
          QStringLiteral("effect=%1 kind=%2 cause=%3 moment=%4 expected=%5 source=%6 known=%7 target=%8 visible=%9")
            .arg(index).arg(e.kind, e.cause).arg(page.currentMoment()).arg(e.moment).arg(e.source)
            .arg(links->property("hasKnownSource").toBool()).arg(e.target).arg(links->property("hasVisibleTarget").toBool()));
      if (e.kind == QStringLiteral("buff")) {
        unknown = true;
        if (!relation->text().contains(QStringLiteral("施加者未记录"))) return fail(QStringLiteral("unrecorded Buff applier inferred"), relation->text());
      }
      if (e.cause == QStringLiteral("skill")) known = true;
      if (e.cause == QStringLiteral("counter")) {
        counter = true;
        const auto presentations = links->property("relationPresentations").toList();
        const bool exactDirection = std::any_of(presentations.cbegin(), presentations.cend(), [&](const QVariant& value) {
          const auto row = value.toMap();
          return row[QStringLiteral("sourceKey")].toString() == e.source && row[QStringLiteral("targetKey")].toString() == e.target;
        });
        if (!relation->text().startsWith(QStringLiteral("敌方 · ")) || relation->text().contains(QStringLiteral("号位")) || !exactDirection)
          return fail(QStringLiteral("counter relation direction"), relation->text());
        int highlighted = 0;
        for (const auto* view : page.findChildren<ReplayCell*>()) if (view->property("actorEmphasis").toBool()) {
          ++highlighted;
          if (view->property("visibleFighterKey").toString() != e.source) return fail(QStringLiteral("wrong counter actor emphasized"), view->property("visibleFighterKey").toString());
        }
        if (highlighted != 1) return fail(QStringLiteral("counter actor count"), QString::number(highlighted));
        page.seekMoment(e.moment);
        auto* stage = page.findChild<QLabel*>(QStringLiteral("replayOutcome"));
        // Selecting a microstep focuses its first changed record; the banner
        // identifies that record as a counter rather than an overview title.
        if (!stage || !stage->text().contains(QStringLiteral("反击")) || links->property("connectionCount").toInt() < 1) {
          return fail(QStringLiteral("counter microstep lost result or relation"), stage ? stage->text() : QStringLiteral("missing outcome"));
        }
        for (const auto* view : page.findChildren<ReplayCell*>()) if (view->property("actorEmphasis").toBool())
          if (view->property("visibleFighterKey").toString() != e.source) return fail(QStringLiteral("microstep counter actor changed"), view->property("visibleFighterKey").toString());
      }
    }
  }
  if (!known || !unknown || !counter) return fail(QStringLiteral("fixture effect coverage"), QStringLiteral("known=%1 unknown=%2 counter=%3").arg(known).arg(unknown).arg(counter));
  page.seekMoment(0);
  auto* outcome = page.findChild<QLabel*>(QStringLiteral("replayOutcome"));
  if (!outcome || !outcome->text().contains(QStringLiteral("该小步没有"))) return fail(QStringLiteral("initial microstep fabricated result"), outcome ? outcome->text() : QStringLiteral("missing outcome"));
  auto* filter = page.findChild<QComboBox*>(QStringLiteral("replayUnitFilter"));
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* timer = page.findChild<QTimer*>(QStringLiteral("replayPlaybackTimer"));
  auto* playbackMode = page.findChild<QComboBox*>(QStringLiteral("replayPlaybackMode"));
  if (!filter || !play || !timer || !playbackMode) return fail(QStringLiteral("missing detailed playback control"));
  // Detailed playback remains available, but it is no longer the default.
  playbackMode->setCurrentIndex(playbackMode->findData(QStringLiteral("effect")));
  filter->setCurrentIndex(filter->findData(BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"))));
  const int firstEffect = effects->item(0)->data(Qt::UserRole).toInt();
  page.selectEffect(firstEffect); const int playingMoment = page.currentMoment();
  QTest::mouseClick(play, Qt::LeftButton);
  if (page.currentStep() != 2 || page.currentMoment() != playingMoment || !timer->isActive()) return fail(QStringLiteral("detailed playback start moved selection or timer inactive"));
  QMetaObject::invokeMethod(timer, "timeout");
  if (page.currentStep() != 2 || page.currentMoment() < playingMoment) return fail(QStringLiteral("detailed playback timeout moved backward or left action"));
  QTest::mouseClick(play, Qt::LeftButton);
  const auto audit = auditBattleReplayEffects(page.replay());
  if (audit.errors()) { qCritical() << audit.report(); return fail(QStringLiteral("effect audit rejected fixture")); }
  ReplaySpiritBar meter;
  for (const auto& values : {QPair<qint64,qint64>{0,12}, {7,14}, {17,14}}) {
    meter.setProgress(values.first, values.second, {});
    if (!meter.toPlainText().contains(QStringLiteral("%1/%2").arg(values.first).arg(values.second))) return fail(QStringLiteral("spirit altered exact balance"), meter.toPlainText());
  }
  meter.setProgress({}, {}, {});
  if (!meter.toPlainText().contains(QStringLiteral("未记录"))) return fail(QStringLiteral("missing spirit balance not explicit"), meter.toPlainText());
  meter.setPlaybackDriven(true);
  meter.setProgress(9, 10, QStringLiteral("合成进度记录"));
  meter.setTransition(2, 4, {}, true);
  for (const qreal position : {qreal(0), qreal(0.5), qreal(1)}) {
    meter.setPlaybackProgress(position);
    const auto text = meter.toPlainText();
    if (!text.contains(QStringLiteral("9/10")) || !text.contains(QStringLiteral("+4")) ||
        text.contains(QStringLiteral("+7")) || meter.property("full").toBool()) return fail(QStringLiteral("spirit transition replaced raw vc or invented fullness"), text);
  }
  meter.setProgress(0, 10, {}); meter.setTransition(9, -9);
  if (!meter.toPlainText().contains(QStringLiteral("-9")) || meter.toPlainText().contains(QStringLiteral("施放"))) return fail(QStringLiteral("zero spirit balance inferred skill cast"), meter.toPlainText());
  meter.setProgress(17, 14, {}); meter.setTransition(9, 3);
  if (!meter.property("full").toBool() || !meter.toPlainText().contains(QStringLiteral("17/14")) ||
      !meter.toPlainText().contains(QStringLiteral("+3"))) return fail(QStringLiteral("overflow spirit balance or raw vc lost"), meter.toPlainText());
  meter.setProgress(9, {}, {}); meter.setTransition(2, 4, {}, true);
  // With no maximum, no fill ratio can be computed. The balance and recorded
  // change remain readable and must not become interpolated numerical text.
  for (const qreal position : {qreal(0), qreal(0.5), qreal(1)}) {
    meter.setPlaybackProgress(position);
    if (meter.property("full").toBool() || !meter.toPlainText().contains(QStringLiteral("9/?")) ||
        !meter.toPlainText().contains(QStringLiteral("+4"))) return fail(QStringLiteral("unknown maximum altered recorded spirit text"), meter.toPlainText());
    for (const auto* motion : meter.findChildren<QVariantAnimation*>())
      if (motion->state() != QAbstractAnimation::Stopped) return fail(QStringLiteral("unknown maximum started fill animation"));
  }
  ReplayCell cell(0); BattleReplay::Fighter f;
  f.key = "buff-fixture"; f.name = "synthetic Buff owner"; f.summoned = true; f.entered = true; f.buffs = {"99", "99", "101"};
  cell.display(4, {f}, battleReplayTestCatalog(), {}, {}, {}, {}, {}, {f.key});
  auto* buffs = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  if (!buffs || !buffs->accessibleName().contains(QStringLiteral("synthetic normal · 记录 2 条")) || !buffs->accessibleName().contains(QStringLiteral("synthetic marker")) || !cell.toPlainText().contains(QStringLiteral("召唤")) || !cell.toPlainText().contains(QStringLiteral("元能"))) return fail(QStringLiteral("cell lost Buff counts or summon/empowerment identity"), cell.toPlainText());
  bool clicked = false;
  QObject::connect(&cell, &ReplayCell::buffActivated, [&clicked, &f](const QString& key, const QString& id) { clicked = key == f.key && id == "99"; });
  QMetaObject::invokeMethod(buffs, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("buff:99"))));
  if (!clicked) return fail(QStringLiteral("Buff link emitted wrong identity"));
  bytes.replace("1_99_30,0_99_40", "1_99_30,0_99_40,0_999_50");
  QTemporaryFile missing; if (!missing.open()) return fail(QStringLiteral("cannot open missing-Buff fixture")); missing.write(bytes); missing.flush();
  if (!page.loadFile(missing.fileName())) return fail(QStringLiteral("cannot import missing-Buff fixture")); page.seek(2);
  const auto& missingEffects = page.replay().steps[2].effects;
  for (int i = 0; i < missingEffects.size(); ++i) if (missingEffects[i].kind == QStringLiteral("buff") && missingEffects[i].causeId == QStringLiteral("999")) {
    page.selectEffect(i);
    if (!outcome->text().contains(QStringLiteral("无匹配项"))) return fail(QStringLiteral("unmatched Buff removal hidden"), outcome->text());
    break;
  }
  auto progressBytes = battleReplayFixture(); progressBytes.replace("999999999", "106986");
  QTemporaryFile progressFile; if (!progressFile.open()) return fail(QStringLiteral("cannot open progress fixture")); progressFile.write(progressBytes); progressFile.flush();
  if (!page.loadFile(progressFile.fileName())) return fail(QStringLiteral("cannot import progress fixture"));
  auto* enemySpirit = page.findChild<ReplaySpiritBar*>(QStringLiteral("replaySpirit1"));
  if (!enemySpirit || !enemySpirit->toPlainText().contains(QStringLiteral("未记录"))) return fail(QStringLiteral("future enemy spirit leaked into initial state"), enemySpirit ? enemySpirit->toPlainText() : QStringLiteral("missing control"));
  page.seek(2);
  if (!enemySpirit->toPlainText().contains(QStringLiteral("2/10"))) return fail(QStringLiteral("recorded enemy spirit update missing"), enemySpirit->toPlainText());
  page.seek(0);
  return enemySpirit->toPlainText().contains(QStringLiteral("未记录")) || fail(QStringLiteral("rewind retained future enemy spirit"), enemySpirit->toPlainText());
}

bool verifyActionOverview() {
  const auto fail = [](const QString& reason, const QString& actual = {}) {
    qCritical().noquote() << "action overview:" << reason << actual; return false;
  };
  // The official near-range command rejects multiple effective targets.
  // 106853 is a recorded action-3 remote skill, appropriate for this fixture.
  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106853");
  // Two explicit targets, and one separate passive Buff recipient. Transient
  // and unmatched removals must remain visible without inventing a source.
  bytes.replace("1_99_30,0_99_40", "1_99_30,0_99_40,1_777777_50,0_777777_60,0_888888_70");
  bytes.replace("\"bfs\":\"99,99\"}", "\"bfs\":\"99,99\"},{\"id\":9,\"name\":\"passive recipient\",\"x\":1,\"y\":0,\"hp\":20,\"maxHp\":20}");
  bytes.replace("\"vigour\":60}", "\"vigour\":60},{\"id\":8,\"name\":\"second target\",\"x\":1,\"y\":0,\"hp\":150,\"maxHp\":150}");
  bytes.replace("}}],\"prl\":[]", "}},{\"tti\":1,\"tpn\":\"tester\",\"tci\":8,\"ishit\":true,\"isse\":true,\"tn\":{}},{\"tti\":0,\"tpn\":\"tester\",\"tci\":9,\"ishit\":false,\"isse\":false,\"tn\":{}}],\"prl\":[{\"tti\":0,\"tpn\":\"tester\",\"tci\":9,\"tn\":{\"bd_bfs\":\"1_777777_1\"}}]");
  QTemporaryFile file; if (!file.open()) return fail(QStringLiteral("fixture file")); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return fail(QStringLiteral("fixture import")); page.seek(2);
  auto* relation = page.findChild<QLabel*>(QStringLiteral("replayRelation"));
  auto* source = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* passive = page.findChild<ReplayCell*>(QStringLiteral("replayCell5"));
  auto* target = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
  auto* second = page.findChild<ReplayCell*>(QStringLiteral("replayCell12"));
  if (!relation || !source || !passive || !target || !second) return fail(QStringLiteral("missing relation or cell"));
  if (!relation->text().contains(QStringLiteral("2 个橙框目标"))) return fail(QStringLiteral("active target count"), relation->text());
  if (!target->toPlainText().contains(QStringLiteral("目标")) || !second->toPlainText().contains(QStringLiteral("目标")) ||
      passive->toPlainText().contains(QStringLiteral("目标"))) return fail(QStringLiteral("target/passive role"), target->toPlainText() + second->toPlainText() + passive->toPlainText());
  if (passive->findChild<QLabel*>(QStringLiteral("unitResultBadges"))) return fail(QStringLiteral("passive recipient fabricated judgement"));
  auto* hp = source->findChild<QProgressBar*>(QStringLiteral("hpBar"));
  auto* shield = source->findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  auto* power = source->findChild<QProgressBar*>(QStringLiteral("powerBar"));
  auto* sourceBuffs = source->findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  if (!hp || !shield || !power || !sourceBuffs || !hp->accessibleName().contains(QStringLiteral("变化：")) ||
      !hp->accessibleName().contains(QStringLiteral("-10")) || !shield->accessibleName().contains(QStringLiteral("-7")) ||
      !power->accessibleName().contains(QStringLiteral("+5")) ||
      !source->toPlainText().contains(QStringLiteral("Buff +3 / −4")) || !source->toPlainText().contains(QStringLiteral("移除未匹配 1 条")))
    return fail(QStringLiteral("recorded meter/Buff totals"), source->toPlainText());
  const auto details = sourceBuffs->toolTip();
  if (details.contains(QStringLiteral("777777")) || details.contains(QStringLiteral("888888")) ||
      !details.contains(QStringLiteral("来源未记"))) return fail(QStringLiteral("public Buff detail/source"), details);
  if (hp->toolTip().contains(QStringLiteral("护盾")) || shield->toolTip().contains(QStringLiteral("生命"))) return fail(QStringLiteral("meter detail leaked another metric"), hp->toolTip() + QLatin1Char('\n') + shield->toolTip());
  if (auto* events = source->findChild<QTextBrowser*>(QStringLiteral("unitEvents")))
    if (events->toPlainText().contains(QStringLiteral("生命 -10")) || events->toPlainText().contains(QStringLiteral("护盾 -7"))) return fail(QStringLiteral("meter record duplicated in separate event area"));
  const auto sourceKey = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  if (page.displayedState()[sourceKey].buffs.contains(QStringLiteral("777777"))) return fail(QStringLiteral("transient Buff remained in final membership"));
  auto* summary = page.findChild<QTextBrowser*>(QStringLiteral("replayOverview"));
  if (!summary || !summary->toPlainText().contains(QStringLiteral("施加者未记录")) ||
      !summary->toPlainText().contains(QStringLiteral("无匹配")) || summary->toPlainText().contains(QStringLiteral("888888")))
    return fail(QStringLiteral("public overview lost unmatched removal/source"), summary ? summary->toPlainText() : QStringLiteral("missing overview"));
  auto* sourceName = source->findChild<QPushButton*>(QStringLiteral("unitName"));
  auto* detailView = page.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
  if (!sourceName || !detailView) return fail(QStringLiteral("source detail unavailable"));
  sourceName->click();
  if (!activateReplayTextAnchor(detailView, QStringLiteral("buff:777777")) || detailView->toPlainText().contains(QStringLiteral("777777")) ||
      !activateReplayTextAnchor(detailView, QStringLiteral("buff-raw:777777")) ||
      !detailView->toPlainText().contains(QStringLiteral("1_777777_50")) || !detailView->toPlainText().contains(QStringLiteral("0_777777_60")))
    return fail(QStringLiteral("transient Buff raw add/remove history lost"));
  const auto sourceHref = QStringLiteral("unit:") + QString::fromLatin1(QUrl::toPercentEncoding(sourceKey));
  if (!activateReplayTextAnchor(detailView, sourceHref) || !activateReplayTextAnchor(detailView, QStringLiteral("buff:888888")) ||
      detailView->toPlainText().contains(QStringLiteral("888888")) || !activateReplayTextAnchor(detailView, QStringLiteral("buff-raw:888888")) ||
      !detailView->toPlainText().contains(QStringLiteral("0_888888_70")))
    return fail(QStringLiteral("unmatched removal raw record lost"));
  const auto passiveKey = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("9"));
  const auto& effects = page.replay().steps[2].effects;
  auto* allLinks = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  if (!allLinks || allLinks->property("connectionCount").toInt() != 2) return fail(QStringLiteral("action relation count"), allLinks ? allLinks->property("connectionCount").toString() : QStringLiteral("missing links"));
  bool foundPassive = false, retainedInactive = false;
  for (const auto& e : effects) if (e.kind == QStringLiteral("target") && e.target == passiveKey) retainedInactive = true;
  if (!retainedInactive) return fail(QStringLiteral("inactive target raw record missing"));
  for (int i = 0; i < effects.size(); ++i) if (effects[i].target == passiveKey && effects[i].kind == QStringLiteral("buff")) {
    // Act directly on the recipient's Buff card, without the side panel.
    auto* cardBuffs = passive->findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
    if (!cardBuffs) return fail(QStringLiteral("passive Buff card missing"));
    if (auto* unknown = cardBuffs->parentWidget()->findChild<QToolButton*>(QStringLiteral("unitBuffUnknownToggle")); unknown && !unknown->isHidden())
      unknown->setChecked(true);
    const auto href = QStringLiteral("round-effect:%1:%2").arg(page.currentStep()).arg(i); bool hasLink = false;
    for (auto block = cardBuffs->document()->begin(); block.isValid(); block = block.next())
      for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment)
        if (fragment.fragment().charFormat().anchorHref() == href) hasLink = true;
    if (!hasLink || !QMetaObject::invokeMethod(cardBuffs, "anchorClicked", Q_ARG(QUrl, QUrl(href)))) return fail(QStringLiteral("passive Buff record not actionable"), cardBuffs->toPlainText());
    QCoreApplication::processEvents();
    foundPassive = true;
    auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
    if (page.currentMoment() != effects[i].moment || !relation->text().startsWith(QStringLiteral("施加者未记录")) || !links || links->property("hasKnownSource").toBool())
      return fail(QStringLiteral("passive record navigation attributed a source or wrong moment"), relation->text());
    break;
  }
  if (!foundPassive) return fail(QStringLiteral("passive Buff effect missing"));
  page.seek(0);
  for (const auto* bar : source->findChildren<QProgressBar*>())
    if (bar->property("actionMeterEventCount").toInt() || bar->property("activeMeterEventCount").toInt() ||
        !bar->property("actionGhostSegments").toList().isEmpty()) return fail(QStringLiteral("opening retained future meter change"));
  if (source->findChild<QTextBrowser*>(QStringLiteral("unitEvents")) || target->toPlainText().contains(QStringLiteral("目标")) || page.replay().original != bytes)
    return fail(QStringLiteral("opening/raw preservation"));
  return true;
}

bool verifyBoardLayoutAndInspector() {
  WorkbenchWindow host([](WorkbenchPage page, QWidget* parent) -> QWidget* {
    return page == WorkbenchPage::BattleReplay ? static_cast<QWidget*>(createPreviewBattleReplayPage(parent)) : new QWidget(parent);
  }, nullptr, WorkbenchOptions{QSize(1920, 1080), QSize(1440, 940)});
  if (!host.showPage(WorkbenchPage::BattleReplay)) return false;
  auto* replayPage = qobject_cast<BattleReplayPage*>(host.page(WorkbenchPage::BattleReplay));
  if (!replayPage) return false;
  auto& page = *replayPage;
  host.show(); QCoreApplication::processEvents();
  BattleReplay::Fighter f; f.key = "dense-unit"; f.name = QStringLiteral("合成的很长名字用于验证血条不会被 Buff 挤走");
  f.hp = 100; f.maxHp = 200; f.shield = 50; f.power = 80; f.maxPower = 100;
  for (int i = 0; i < 25; ++i) f.buffs.append(QString::number(900000 + i));
  auto second = f; second.key = "second-unit"; second.name = "second occupant"; second.hp = 70;
  ReplayUnitOverview cue; cue.badge = QStringLiteral("格挡 · 免疫 · 无效 · 暴击 · 闪避");
  cue.clientBadge = QStringLiteral("强效·神草 · 神职受击");
  cue.clientDetail = QStringLiteral("官方客户端提示：合成属性/神职显示测试，不是日志直接判定。");
  cue.events.append({42, QStringLiteral("生命 -100"), QStringLiteral("合成作用记录"), "#bd3858", {}, false, true, QStringLiteral("生命"), QStringLiteral("-100")});
  cue.events.append({43, QStringLiteral("护盾 +50"), QStringLiteral("合成护盾记录"), "#246ca5", {}, false, false, QStringLiteral("护盾"), QStringLiteral("+50")});
  cue.events.append({44, QStringLiteral("气势 -20"), QStringLiteral("合成气势记录"), "#a76b19", {}, false, false, QStringLiteral("气势"), QStringLiteral("-20")});
  // This dense card is a child of the real workbench, so inherited progress
  // bar sizing and generic widget styles participate in the layout check.
  ReplayCell cell(0, &page); cell.resize(190, 204); cell.display(1, {f, second}, {}, {}, {}, {}, {}, {}, {}, {{f.key, cue}});
  cell.focusUnit(f.key); cell.show(); QTest::qWait(30);
  auto* pages = cell.findChild<QStackedWidget*>(QStringLiteral("cellUnits"));
  if (!pages || !pages->currentWidget()) return false;
  auto* card = pages->currentWidget();
  auto* hp = card->findChild<QProgressBar*>(QStringLiteral("hpBar"));
  auto* shield = card->findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  auto* power = card->findChild<QProgressBar*>(QStringLiteral("powerBar"));
  auto* buffs = card->findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  auto* toggle = card->findChild<QToolButton*>(QStringLiteral("unitBuffUnknownToggle"));
  auto* badges = card->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
  auto* hints = card->findChild<QLabel*>(QStringLiteral("unitClientHints"));
  if (!hp || !shield || !power || !buffs || !toggle || !badges || !hints || hints == badges ||
      !hints->property("clientPresentation").toBool() || hints->property("rawEvidence").toString() != cue.clientDetail ||
      hints->toolTip().contains(cue.clientDetail) ||
      !hints->accessibleName().contains(cue.clientBadge) || badges->accessibleName().contains(QStringLiteral("强效")) ||
      card->findChild<QTextBrowser*>(QStringLiteral("unitEvents")) ||
      buffs->accessibleName().contains(QStringLiteral("900024")) || cell.toPlainText().contains(QStringLiteral("900024")) ||
      buffs->property("memberCount").toInt() != 25 || buffs->property("groupCount").toInt() != 25 ||
      buffs->property("unknownExpanded").toBool() || buffs->property("shownUnknownGroupCount").toInt() != 0 ||
      buffs->toPlainText().contains(QStringLiteral("900000")) || buffs->toPlainText().contains(QStringLiteral("900024")) ||
      !hp->accessibleName().contains(QStringLiteral("变化：-100")) ||
      hp->toolTip().contains(QStringLiteral("合成作用记录")) ||
      hp->property("rawEvidence").toList().value(0).toMap().value(QStringLiteral("detail")).toString() != QStringLiteral("合成作用记录") ||
      !shield->accessibleName().contains(QStringLiteral("变化：+50")) ||
      !power->accessibleName().contains(QStringLiteral("变化：-20"))) return false;
  for (const auto& badge : cue.badge.split(QStringLiteral(" · ")))
    if (!badges->accessibleName().contains(badge)) return false;
  int unitDetails = 0, activated = -1;
  QObject::connect(&cell, &ReplayCell::unitActivated, [&] { ++unitDetails; });
  QObject::connect(&cell, &ReplayCell::effectActivated, [&](int index) { activated = index; });
  for (const int height : {174, 204}) {
    cell.resize(190, height); QTest::qWait(20);
    QList<QRect> rectangles;
    for (auto* widget : {static_cast<QWidget*>(hp), static_cast<QWidget*>(shield), static_cast<QWidget*>(power),
        static_cast<QWidget*>(badges), static_cast<QWidget*>(hints), static_cast<QWidget*>(toggle), static_cast<QWidget*>(buffs)}) {
      const QRect rect(widget->mapTo(&cell, QPoint()), widget->size());
      if (!widget->isVisible() || !cell.rect().contains(rect)) {
        qCritical() << "dense card control outside cell" << widget->objectName() << rect << cell.size(); return false;
      }
      for (const auto& other : rectangles) if (rect.intersects(other)) { qCritical() << "dense card controls overlap" << rect << other; return false; }
      rectangles.append(rect);
    }
    if (buffs->viewport()->height() < 14 || hp->text() != QStringLiteral("100/200") ||
        shield->text() != QStringLiteral("护盾 50") || power->text() != QStringLiteral("80/100")) return false;
    for (const auto& pair : {qMakePair(hp, 42), qMakePair(shield, 43), qMakePair(power, 44)}) {
      activated = -1; QTest::mouseClick(pair.first, Qt::LeftButton, Qt::NoModifier, QPoint(14, pair.first->height() - 5));
      if (activated != pair.second) return false;
    }
    const auto fixedHp = hp->mapTo(&cell, QPoint());
    QTest::mouseClick(toggle, Qt::LeftButton);
    if (!buffs->property("unknownExpanded").toBool() || buffs->property("shownUnknownGroupCount").toInt() != 25 ||
        buffs->toPlainText().contains(QStringLiteral("900024")) || !hasReplayTextAnchor(buffs, QStringLiteral("buff:900024#member24")) ||
        hp->mapTo(&cell, QPoint()) != fixedHp || unitDetails) return false;
    QTest::mouseClick(toggle, Qt::LeftButton);
    if (buffs->property("unknownExpanded").toBool() || buffs->property("shownUnknownGroupCount").toInt() != 0 ||
        buffs->toPlainText().contains(QStringLiteral("900024")) || hp->mapTo(&cell, QPoint()) != fixedHp || unitDetails) return false;
  }
  QTest::mouseClick(toggle, Qt::LeftButton);
  QString selectedBuff;
  QObject::connect(&cell, &ReplayCell::buffActivated, [&](const QString&, const QString& id) { selectedBuff = id; });
  if (!clickReplayTextAnchor(buffs, QStringLiteral("buff:900024#member24")) || selectedBuff != QStringLiteral("900024") || unitDetails)
    return false;
  const auto hpPosition = hp->mapTo(&cell, QPoint());
  buffs->verticalScrollBar()->setValue(buffs->verticalScrollBar()->maximum());
  if (hp->mapTo(&cell, QPoint()) != hpPosition || hp->text() != QStringLiteral("100/200")) return false;
  bool clicked = false; QObject::connect(&cell, &ReplayCell::effectActivated, [&clicked](int index) { clicked = index == 42; });
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, QPoint(14, hp->height() - 7));
  if (!clicked) return false;
  cell.focusUnit(second.key);
  if (cell.property("visibleFighterKey").toString() != second.key || pages->currentWidget()->findChild<QProgressBar*>(QStringLiteral("hpBar"))->text() != QStringLiteral("70/200")) return false;

  // A long same-metric sequence stays ordered and fully reachable, even when
  // only a few change chips fit within the meter itself.
  cue.events.clear();
  for (int i = 0; i < 18; ++i) cue.events.append({100 + i, QStringLiteral("生命记录 %1").arg(i),
    QStringLiteral("完整合成记录 %1").arg(i), "#bd3858", {}, false, false, QStringLiteral("生命"), QStringLiteral("-%1").arg(10 + i)});
  cell.display(1, {f}, {}, {}, {}, {}, {}, {}, {}, {{f.key, cue}}); QTest::qWait(20);
  hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  if (!hp || !hp->accessibleName().contains(QStringLiteral("-27")) || hp->toolTip().contains(QStringLiteral("完整合成记录 17")) ||
      hp->property("rawEvidence").toList().value(17).toMap().value(QStringLiteral("detail")).toString() != QStringLiteral("完整合成记录 17")) return false;
  int selectedRecord = -1;
  QObject::connect(&cell, &ReplayCell::effectActivated, [&selectedRecord](int index) { selectedRecord = index; });
  QTest::mouseClick(hp, Qt::LeftButton, Qt::NoModifier, QPoint(hp->width() / 2, 4));
  QCoreApplication::processEvents();
  auto* records = qobject_cast<QMenu*>(QApplication::activePopupWidget());
  if (!records || records->actions().size() != 18) return false;
  for (int i = 0; i < records->actions().size(); ++i)
    if (!records->actions()[i]->text().contains(QStringLiteral("-%1").arg(10 + i)) ||
        records->actions()[i]->data().toInt() != 100 + i ||
        records->actions()[i]->toolTip().contains(QStringLiteral("完整合成记录")) ||
        records->actions()[i]->property("rawEvidence").toString() != QStringLiteral("完整合成记录 %1").arg(i)) return false;
  QTest::mouseClick(records, Qt::LeftButton, Qt::NoModifier, records->actionGeometry(records->actions()[7]).center());
  if (selectedRecord != 107) return false;
  cell.close();

  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106986");
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  if (!page.loadFile(file.fileName())) return false; QTest::qWait(30);
  auto* pane = page.findChild<QWidget*>(QStringLiteral("replayDetailPane"));
  auto* arena = page.findChild<QWidget*>(QStringLiteral("replayArena"));
  auto* unitCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* name = unitCell ? unitCell->findChild<QPushButton*>(QStringLiteral("unitName")) : nullptr;
  if (!pane || pane->isVisible() || !arena || !name) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  auto* inspector = page.findChild<QWidget*>(QStringLiteral("replayBoardInspector"));
  auto* detail = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  if (!inspector || !inspector->isVisible() || !detail || !detail->toPlainText().contains(QStringLiteral("当前效果")) || !arena->rect().contains(inspector->geometry())) return false;
  page.seek(2); if (inspector->isVisible()) return false;
  auto* motion = page.findChild<QVariantAnimation*>(QStringLiteral("replayFlowAnimation"));
  auto* enabled = page.findChild<QCheckBox*>(QStringLiteral("replayAnimations"));
  if (!motion || !enabled) return false;
  enabled->setChecked(false);
  if (motion->state() != QAbstractAnimation::Stopped || page.replay().original != bytes) return false;
  host.hide(); return true;
}

bool verifyUnitEmphasisAndResults() {
  const auto fail = [](int line) { qCritical() << "unit emphasis/results at line" << line; return false; };
  BattleReplay::Fighter active; active.key = "active-unit"; active.name = "active fixture";
  active.hp = 100; active.maxHp = 100; active.shield = 40; active.power = 80; active.maxPower = 100;
  auto sameSlot = active; sameSlot.key = "same-slot-unit"; sameSlot.name = "living occupant";
  auto other = active; other.key = "other-slot-unit"; other.name = "other cell";
  QWidget board; auto* grid = new QGridLayout(&board); grid->setContentsMargins(20, 20, 20, 20);
  auto* cell = new ReplayCell(0, &board); auto* neighbor = new ReplayCell(0, &board);
  grid->addWidget(cell, 0, 0); grid->addWidget(neighbor, 0, 1);
  cell->display(1, {active, sameSlot}, {}, active.key, {});
  neighbor->display(4, {other}, {}, active.key, {});
  board.resize(450, 250); board.show(); QTest::qWait(20);
  const auto cellGeometry = cell->geometry(), neighborGeometry = neighbor->geometry();
  if (!cell->property("actorEmphasis").toBool() || cell->property("visibleFighterKey").toString() != active.key ||
      !qFuzzyCompare(cell->property("visualScale").toDouble(), 1.16) || !cell->graphicsEffect() || !cell->graphicsEffect()->isEnabled() ||
      neighbor->property("actorEmphasis").toBool() || cell->property("deadUnit").toBool()) return fail(__LINE__);
  // The acting occupant remains independently visible even while another
  // detail card is selected. Keep its cell emphasized and its own marker only.
  cell->focusUnit(sameSlot.key); QCoreApplication::processEvents();
  if (!cell->property("actorEmphasis").toBool() || cell->property("deadUnit").toBool() ||
      cell->property("visibleFighterKey").toString() != sameSlot.key ||
      !cell->relationAnchor(active.key)->property("isActor").toBool() || cell->relationAnchor(sameSlot.key)->property("isActor").toBool() ||
      !qFuzzyCompare(cell->property("visualScale").toDouble(), 1.16) || !cell->graphicsEffect()->isEnabled()) return fail(__LINE__);
  cell->focusUnit(active.key);
  if (!cell->property("actorEmphasis").toBool() || cell->geometry() != cellGeometry || neighbor->geometry() != neighborGeometry) return fail(__LINE__);
  active.hp = 0;
  cell->display(1, {active, sameSlot}, {}, {}, {}); cell->focusUnit(active.key);
  if (cell->property("deadUnit").toBool() || !cell->property("selectedUnitDead").toBool() || cell->property("actorEmphasis").toBool() ||
      cell->graphicsEffect()->isEnabled() || !cell->relationAnchor(active.key)->property("isDead").toBool() ||
      cell->relationAnchor(sameSlot.key)->property("isDead").toBool() || !cell->toPlainText().contains(QStringLiteral("已阵亡"))) return fail(__LINE__);
  cell->focusUnit(sameSlot.key);
  if (cell->property("deadUnit").toBool() || cell->graphicsEffect()->isEnabled()) return fail(__LINE__);
  cell->focusUnit(active.key);
  sameSlot.hp = 0; cell->display(1, {active, sameSlot}, {}, {}, {});
  if (!cell->property("deadUnit").toBool() || !cell->graphicsEffect()->isEnabled()) return fail(__LINE__);
  sameSlot.hp = 100;
  active.hp = 30; cell->display(1, {active, sameSlot}, {}, {}, {});
  if (cell->property("visibleFighterKey").toString() != active.key || cell->property("deadUnit").toBool() ||
      cell->graphicsEffect()->isEnabled() || cell->geometry() != cellGeometry || neighbor->geometry() != neighborGeometry) return fail(__LINE__);
  active.hp.reset(); cell->display(1, {active}, {}, {}, {});
  if (cell->property("deadUnit").toBool()) return fail(__LINE__); // Unrecorded HP is not a death record.
  board.close();

  // Read judgement badges from the actual target fields. A zero change alone
  // must not be labelled immune, blocked, missed, or ineffective.
  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106986");
  bytes.replace("\"isse\":true,\"issb\":true,\"ishit\":false", "\"isse\":false,\"issb\":true,\"ishit\":false,\"isbl\":true,\"iscr\":true,\"isIm\":true");
  QTemporaryFile file; if (!file.open()) return fail(__LINE__); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return fail(__LINE__); page.seek(2);
  auto* target = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
  auto* source = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* badges = target ? target->findChild<QLabel*>(QStringLiteral("unitResultBadges")) : nullptr;
  if (!source || !badges) return fail(__LINE__);
  const QStringList labels{QStringLiteral("免疫"), QStringLiteral("格挡"), QStringLiteral("被暴击"), QStringLiteral("未命中"), QStringLiteral("技能未生效"), QStringLiteral("反击标记")};
  for (const auto& label : labels) if (!badges->accessibleName().contains(label)) return fail(__LINE__);
  // The source may have its own recorded life transitions in this action;
  // target judgements must still never be copied to it.
  if (auto* sourceBadges = source->findChild<QLabel*>(QStringLiteral("unitResultBadges")))
    for (const auto& label : labels) if (sourceBadges->accessibleName().contains(label)) return fail(__LINE__);
  if (!source->property("actorEmphasis").toBool() || target->property("actorEmphasis").toBool()) return fail(__LINE__);
  int actors = 0;
  for (const auto* view : page.findChildren<ReplayCell*>()) if (view->property("actorEmphasis").toBool()) {
    ++actors;
    if (view->property("visibleFighterKey").toString() != page.replay().steps[2].actor) return fail(__LINE__);
  }
  if (actors != 1) return fail(__LINE__);
  if (page.replay().original != bytes) return fail(__LINE__);

  auto zeroRoot = QJsonDocument::fromJson(bytes).object(); auto br = zeroRoot["br"].toObject();
  auto rounds = br["fr"].toArray(); auto round = rounds[0].toObject(); auto actions = round["rl"].toArray(); auto action = actions[0].toObject();
  action["cn"] = QJsonObject{};
  action["trl"] = QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"ishit", true}, {"isse", true},
    {"tn", QJsonObject{{"att_hps", "0"}, {"ahp", 200}}}}};
  actions[0] = action; round["rl"] = actions; rounds[0] = round; br["fr"] = rounds; zeroRoot["br"] = br;
  const auto zeroBytes = QJsonDocument(zeroRoot).toJson();
  QTemporaryFile zeroFile; if (!zeroFile.open()) return fail(__LINE__); zeroFile.write(zeroBytes); zeroFile.flush();
  if (!page.loadFile(zeroFile.fileName())) return fail(__LINE__); page.seek(2);
  target = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
  const auto targetKey = BattleReplay::fighterKey(1, QStringLiteral("tester"), QStringLiteral("7"));
  if (!target || target->findChild<QLabel*>(QStringLiteral("unitResultBadges")) || page.displayedState()[targetKey].hp != 200) return fail(__LINE__);
  return page.replay().original == zeroBytes;
}

bool verifyBuffSourcePresentation() {
  auto sourceCatalog = battleReplayTestCatalog();
  auto described = sourceCatalog["buffs"].toObject();
  for (const auto* id : {"99", "100", "101"}) {
    auto row = described[id].toObject(); row["mess"] = QStringLiteral("合成来源测试的明确说明"); described[id] = row;
  }
  sourceCatalog["buffs"] = described;
  // These provenance records are deliberately synthetic. Production logs
  // without applier metadata must never inherit the holder's or actor's team.
  BattleReplay::Fighter holder;
  holder.key = QStringLiteral("synthetic-enemy-holder"); holder.team = 1;
  holder.name = QStringLiteral("合成敌方持有者"); holder.hp = 100; holder.maxHp = 100;
  holder.buffs = {QStringLiteral("99"), QStringLiteral("99"), QStringLiteral("99"),
    QStringLiteral("99"), QStringLiteral("101"), QStringLiteral("100")};
  holder.buffMembers = {
    {QStringLiteral("99"), QStringLiteral("synthetic-ally-applier"), QStringLiteral("合成我方施加者"), QStringLiteral("/synthetic/0"), 0},
    {QStringLiteral("99"), QStringLiteral("synthetic-ally-applier"), QStringLiteral("合成我方施加者"), QStringLiteral("/synthetic/1"), 0},
    {QStringLiteral("99"), QStringLiteral("synthetic-enemy-applier"), QStringLiteral("合成敌方施加者"), QStringLiteral("/synthetic/2"), 1},
    {QStringLiteral("99"), {}, {}, QStringLiteral("/synthetic/3"), -1},
    {QStringLiteral("101"), QStringLiteral("synthetic-team-unrecorded"), {}, QStringLiteral("/synthetic/4"), -1},
    {QStringLiteral("100"), {}, {}, QStringLiteral("/synthetic/5"), 0}
  };
  ReplayUnitEvent added; added.index = 41; added.label = QStringLiteral("＋ synthetic normal");
  added.buffId = QStringLiteral("99"); added.color = QStringLiteral("#197b50");
  added.buffSource = QStringLiteral("synthetic-ally-applier"); added.buffSourceName = QStringLiteral("合成我方施加者");
  added.buffSourceTeam = 0; added.buffSourcePath = QStringLiteral("/synthetic/add");
  auto removed = added; removed.index = 42; removed.label = QStringLiteral("− synthetic normal"); removed.removing = true;
  removed.buffSource = QStringLiteral("synthetic-enemy-applier"); removed.buffSourceName = QStringLiteral("合成敌方施加者");
  removed.buffSourceTeam = 1; removed.buffSourcePath = QStringLiteral("/synthetic/original-membership");
  auto unknown = added; unknown.index = 43; unknown.buffSource.clear(); unknown.buffSourceName.clear();
  auto repeated = added; repeated.index = 45; repeated.detail = QStringLiteral("合成第二条新增记录");
  auto triggered = unknown; triggered.index = 44; triggered.buffTriggered = true; triggered.label = QStringLiteral("触发 · synthetic normal");
  ReplayUnitOverview cue; cue.events = {added, repeated, removed, unknown, triggered};
  ReplayCell cell(1); cell.resize(240, 280); cell.show();
  int unitActivations = 0, expansionSignals = 0, activatedEffect = -1;
  QObject::connect(&cell, &ReplayCell::unitActivated, [&] { ++unitActivations; });
  QObject::connect(&cell, &ReplayCell::buffExpansionChanged, [&](bool) { ++expansionSignals; });
  QObject::connect(&cell, &ReplayCell::effectActivated, [&](int index) { activatedEffect = index; });
  const auto fail = [](const char* reason) { qCritical() << "Buff source presentation:" << reason; return false; };
  const auto render = [&](int ownTeam, bool changes = true) {
    const QMap<QString, ReplayUnitOverview> overview = changes ? QMap<QString, ReplayUnitOverview>{{holder.key, cue}} : QMap<QString, ReplayUnitOverview>{};
    cell.display(1, {holder}, sourceCatalog, {}, {}, {}, {}, {}, {}, overview, ownTeam);
    QCoreApplication::processEvents();
    return cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  };
  const auto anchorText = [](const QTextBrowser* view, const QString& href) {
    QString result;
    for (auto block = view->document()->begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.charFormat().anchorHref() == href) result += fragment.text();
      }
    return result;
  };
  const auto originColor = [](const QTextBrowser* view, const QString& label) {
    for (auto block = view->document()->begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.text().contains(label)) return fragment.charFormat().foreground().color();
      }
    return QColor();
  };
  const auto anchorColor = [](const QTextBrowser* view, const QString& href) {
    for (auto block = view->document()->begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.charFormat().anchorHref() == href) return fragment.charFormat().foreground().color();
      }
    return QColor();
  };
  auto* buffs = render(0);
  if (!buffs) return fail("missing Buff document");
  auto membership = buffs->accessibleName();
  if (!membership.contains(QStringLiteral("我方施加 · synthetic normal · 记录 2 条")) ||
      !membership.contains(QStringLiteral("敌方施加 · synthetic normal · 记录 1 条")) ||
      !membership.contains(QStringLiteral("来源未记 · synthetic normal · 记录 1 条")) ||
      membership.count(QStringLiteral("来源未记")) != 3 || membership.contains(QStringLiteral("synthetic normal · 记录 4 条")) ||
      buffs->property("memberCount").toInt() != 6 || buffs->property("groupCount").toInt() != 5 ||
      buffs->property("sourceOwnCount").toInt() != 2 || buffs->property("sourceEnemyCount").toInt() != 1 ||
      buffs->property("sourceUnknownCount").toInt() != 3) return fail("provenance groups or raw member counts lost");
  const auto compact = buffs->toPlainText();
  if (buffs->property("expanded").toBool() || buffs->property("shownGroupCount").toInt() != 0 ||
      compact.contains(QStringLiteral("当前集合")) || !compact.contains(QStringLiteral("获得 · synthetic normal")) || !compact.contains(QStringLiteral("移除 · synthetic normal")) ||
      !compact.contains(triggered.label) || !anchorText(buffs, QStringLiteral("buff:99#member0")).isEmpty() ||
      compact.contains(QStringLiteral("synthetic marker")) || !membership.contains(QStringLiteral("synthetic marker")))
    return fail("collapsed view exposed current members or lost received effects/accessible membership");
  const auto allyColor = originColor(buffs, QStringLiteral("我方施加"));
  const auto enemyColor = originColor(buffs, QStringLiteral("敌方施加"));
  const auto unknownColor = originColor(buffs, QStringLiteral("来源未记"));
  if (!allyColor.isValid() || !enemyColor.isValid() || !unknownColor.isValid() ||
      allyColor == enemyColor || allyColor == unknownColor || enemyColor == unknownColor ||
      anchorColor(buffs, QStringLiteral("effect:41")) != allyColor || anchorColor(buffs, QStringLiteral("effect:42")) != enemyColor ||
      anchorColor(buffs, QStringLiteral("effect:43")) != unknownColor || anchorColor(buffs, QStringLiteral("effect:44")) != unknownColor)
    return fail("applier colors inferred from holder or missing source");
  auto* toggle = cell.findChild<QToolButton*>(QStringLiteral("unitBuffToggle"));
  if (!toggle || toggle->property("fighterKey").toString() != holder.key) return fail("fixed in-cell expand control missing");
  const auto cellSize = cell.size();
  QTest::mouseClick(toggle, Qt::LeftButton);
  if (!buffs->property("expanded").toBool() || buffs->property("shownGroupCount").toInt() != 5 ||
      !buffs->toPlainText().contains(QStringLiteral("synthetic marker")) || buffs->accessibleName() != membership ||
      cell.size() != cellSize || unitActivations || expansionSignals != 1) return fail("expand left cell, dropped records or opened unit detail");
  if (!clickReplayTextAnchor(buffs, QStringLiteral("events:0"))) return fail("grouped change menu inaccessible");
  QCoreApplication::processEvents();
  auto* eventMenu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
  if (!eventMenu || eventMenu->objectName() != QStringLiteral("buffEventMenu") || eventMenu->actions().size() != 2 ||
      eventMenu->actions()[0]->data().toInt() != 41 || eventMenu->actions()[1]->data().toInt() != 45 ||
      eventMenu->actions()[1]->toolTip().contains(repeated.detail) ||
      eventMenu->actions()[1]->property("rawEvidence").toString() != repeated.detail) return fail("grouped changes lost individual records");
  QTest::mouseClick(eventMenu, Qt::LeftButton, Qt::NoModifier, eventMenu->actionGeometry(eventMenu->actions()[1]).center());
  if (activatedEffect != 45) return fail("grouped change selected wrong record");
  // Same Buff ID has distinct member links, so opening one source group
  // neither combines its count nor changes the referenced official Buff ID.
  bool clicked = false;
  QObject::connect(&cell, &ReplayCell::buffActivated, [&clicked, &holder](const QString& key, const QString& id) {
    clicked = key == holder.key && id == QStringLiteral("99");
  });
  const auto memberLink = QStringLiteral("buff:99#member1");
  if (!anchorText(buffs, memberLink).contains(QStringLiteral("合成敌方施加者")) ||
      anchorColor(buffs, memberLink) != enemyColor || !clickReplayTextAnchor(buffs, memberLink) || !clicked || unitActivations)
    return fail("source-specific membership link combined groups or opened unit detail");
  buffs = render(1); if (!buffs) return false;
  membership = buffs->accessibleName();
  if (!membership.contains(QStringLiteral("敌方施加 · synthetic normal · 记录 2 条")) ||
      !membership.contains(QStringLiteral("我方施加 · synthetic normal · 记录 1 条")) ||
      membership.count(QStringLiteral("来源未记")) != 3 ||
      buffs->property("sourceOwnCount").toInt() != 1 || buffs->property("sourceEnemyCount").toInt() != 2)
    { qCritical() << membership; return fail("perspective switch changed membership or unknown source"); }
  if (!buffs->property("expanded").toBool() || expansionSignals != 1)
    return fail("perspective refresh lost expanded state or emitted a user expansion");
  if (anchorColor(buffs, QStringLiteral("effect:41")) != enemyColor || anchorColor(buffs, QStringLiteral("effect:42")) != allyColor)
    return fail("perspective refresh used incorrect applier colors");
  if (buffs->verticalScrollBar()->value() != 0) {
    qCritical() << "expanded refreshed scroll" << buffs->verticalScrollBar()->value() << buffs->verticalScrollBar()->maximum();
    return fail("expanded refresh hid the new phase's first changes");
  }
  const auto completeHolder = holder;
  holder.buffs.removeLast(); holder.buffMembers.removeLast();
  buffs = render(0, false);
  if (!buffs->property("expanded").toBool() || buffs->property("memberCount").toInt() != 5 ||
      buffs->property("groupCount").toInt() != 4 || buffs->toPlainText().contains(QStringLiteral("本次")) ||
      !anchorText(buffs, QStringLiteral("effect:41")).isEmpty() || !anchorText(buffs, QStringLiteral("buff:100#member4")).isEmpty())
    return fail("refresh retained stale events or membership");
  holder = completeHolder; buffs = render(0, false);
  if (!buffs->property("expanded").toBool() || buffs->property("shownGroupCount").toInt() != 5 ||
      anchorText(buffs, QStringLiteral("buff:100#member4")).isEmpty() || expansionSignals != 1)
    return fail("returning to prior state lost membership or expanded state");
  auto other = holder; other.key = QStringLiteral("synthetic-other-holder");
  cell.display(1, {holder, other}, sourceCatalog, {}, {});
  cell.focusUnit(other.key); QCoreApplication::processEvents();
  auto* pages = cell.findChild<QStackedWidget*>(QStringLiteral("cellUnits"));
  if (!pages || !pages->currentWidget()) return fail("same-cell pages missing");
  auto* otherBuffs = pages->currentWidget()->findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  auto* otherToggle = pages->currentWidget()->findChild<QToolButton*>(QStringLiteral("unitBuffToggle"));
  if (!otherBuffs || !otherToggle || otherBuffs->property("expanded").toBool() ||
      otherBuffs->property("shownGroupCount").toInt() != 0 || otherBuffs->toPlainText().contains(QStringLiteral("synthetic marker")))
    return fail("another occupant inherited expansion or exposed current membership");
  QTest::mouseClick(otherToggle, Qt::LeftButton); cell.focusUnit(holder.key);
  buffs = pages->currentWidget()->findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  toggle = pages->currentWidget()->findChild<QToolButton*>(QStringLiteral("unitBuffToggle"));
  if (!buffs || !toggle || !buffs->property("expanded").toBool()) return fail("same-cell switch forgot expanded holder");
  QTest::mouseClick(toggle, Qt::LeftButton); cell.focusUnit(other.key);
  if (!otherBuffs->property("expanded").toBool() || unitActivations) return fail("collapsing one occupant changed another");
  const int beforeResetSignals = expansionSignals;
  cell.resetBuffExpansion();
  if (otherBuffs->property("expanded").toBool() || buffs->property("expanded").toBool() ||
      otherBuffs->property("shownGroupCount").toInt() != 0 || buffs->property("shownGroupCount").toInt() != 0 ||
      otherBuffs->toPlainText().contains(QStringLiteral("synthetic marker")) || buffs->toPlainText().contains(QStringLiteral("synthetic marker")) ||
      expansionSignals != beforeResetSignals)
    return fail("session reset failed to silently collapse all occupants");
  holder.buffMembers[0].id = QStringLiteral("stale-member-id");
  buffs = render(0, false); if (!buffs) return false;
  membership = buffs->accessibleName();
  if (membership.contains(QStringLiteral("我方施加")) || membership.contains(QStringLiteral("敌方施加")) ||
      !membership.contains(QStringLiteral("来源未记 · synthetic normal · 记录 4 条")) ||
      buffs->property("sourceUnknownCount").toInt() != 6) return fail("misaligned provenance did not safely fall back to unknown");
  const auto safeMembers = BattleReplay::recordedBuffMembers(holder);
  if (safeMembers.size() != holder.buffs.size()) return false;
  for (int i = 0; i < safeMembers.size(); ++i)
    if (safeMembers[i].id != holder.buffs[i] || !safeMembers[i].source.isEmpty() || safeMembers[i].sourceTeam != -1) return false;
  return holder.buffs.count(QStringLiteral("99")) == 4;
}

bool verifyRelationColorsAndHitCues() {
  // Synthetic multi-target observations with an official remote-skill profile:
  // god grass -> god water is strong, -> god fire weak, -> self neutral.
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object(); auto br = root["br"].toObject();
  auto initial = br["or"].toObject(); auto teams = initial["team"].toArray();
  for (int side = 0; side < 2; ++side) {
    auto team = teams[side].toObject(); auto players = team["players"].toArray(); auto player = players[0].toObject();
    auto fighters = player["cts"].toArray(); auto fighter = fighters[0].toObject(); fighter["attr"] = side == 0 ? "21" : "22";
    fighters[0] = fighter;
    if (side == 0) fighters.append(QJsonObject{{"id", 8}, {"name", "synthetic ally"}, {"x", 1}, {"y", 0},
      {"hp", 100}, {"maxHp", 100}, {"vigour", 50}, {"shp", 20}, {"attr", "23"}});
    player["cts"] = fighters; players[0] = player; team["players"] = players; teams[side] = team;
  }
  initial["team"] = teams; br["or"] = initial;
  const QJsonObject attack{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106853}, {"csn", 1}, {"ses", 1}, {"cn", QJsonObject{}},
    {"trl", QJsonArray{
      QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true}, {"isbl", true}, {"iscr", true},
        {"tn", QJsonObject{{"att_hps", "-10"}, {"ahp", 190}}}},
      QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 8}, {"isse", true}, {"ishit", true}, {"isIm", true}, {"iscr", true},
        {"tn", QJsonObject{{"att_hps", "5"}, {"ahp", 105}}}},
      QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", false}, {"isIm", true}, {"isbl", true},
        {"tn", QJsonObject{}}}}}};
  const QJsonObject reply{{"cti", 1}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"csn", 2}, {"ses", 1}, {"cn", QJsonObject{}},
    {"trl", QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true},
      {"tn", QJsonObject{{"att_hps", "-5"}, {"ahp", 95}}}}}}};
  br["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{attack, reply}}}};
  root["br"] = br; const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1400, 1000); if (!page.loadFile(file.fileName())) return false;
  page.show(); QCoreApplication::processEvents();
  const auto fail = [](const char* reason) { qCritical() << "relation color/hit cues:" << reason; return false; };
  const auto source = BattleReplay::fighterKey(0, "tester", "7"), ally = BattleReplay::fighterKey(0, "tester", "8");
  const auto enemy = BattleReplay::fighterKey(1, "tester", "7");
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  auto* perspective = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  if (!links || !perspective) return fail("presentation controls missing");
  const auto relation = [&](const QString& from, const QString& to) {
    for (const auto& value : links->property("relationPresentations").toList()) {
      const auto item = value.toMap();
      if (item["sourceKey"].toString() == from && item["targetKey"].toString() == to) return item;
    }
    return QVariantMap{};
  };
  const auto overlay = [&](const QString& key) -> QWidget* {
    for (auto* cell : page.findChildren<ReplayCell*>()) {
      if (cell->property("visibleFighterKey").toString() == key) return cell->findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
    }
    return nullptr;
  };
  page.seek(2); int hit = -1;
  for (int i = 0; i < page.playbackPhaseCount(); ++i) {
    page.seekPlaybackPhase(i);
    if (page.playbackPhaseKind() == QStringLiteral("attack")) { hit = i; break; }
    for (const auto& value : links->property("relationPresentations").toList())
      if (!value.toMap()["labels"].toStringList().isEmpty()) return fail("future attribute cue appeared before hit");
    if (overlay(enemy) && !overlay(enemy)->property("effectLabels").toStringList().isEmpty()) {
      qCritical() << "before-hit stage" << i << page.playbackPhaseKind() << overlay(enemy)->property("effectLabels");
      return fail("future outcome appeared before hit");
    }
  }
  if (hit < 0) return fail("hit stage missing");
  const auto enemyArrow = relation(source, enemy), allyArrow = relation(source, ally), selfArrow = relation(source, source);
  if (enemyArrow["color"].toString() != "red" || allyArrow["color"].toString() != "green" || selfArrow["color"].toString() != "green" ||
      enemyArrow["labels"].toStringList() != QStringList{QStringLiteral("神草强效")} ||
      allyArrow["labels"].toStringList() != QStringList{QStringLiteral("弱效")} || !selfArrow["labels"].toStringList().isEmpty())
    return fail("relative teams or per-target attribute annotations mixed");
  auto* hitOverlay = overlay(enemy); auto* allyOverlay = overlay(ally); auto* selfOverlay = overlay(source);
  if (!hitOverlay || !allyOverlay || !selfOverlay) return fail("prominent outcome overlay missing");
  const auto enemyLabels = hitOverlay->property("effectLabels").toStringList();
  const auto allyLabels = allyOverlay->property("effectLabels").toStringList();
  const auto selfLabels = selfOverlay->property("effectLabels").toStringList();
  const auto* numbers = page.findChild<QWidget*>(QStringLiteral("replayMeterOverlay"));
  bool criticalNumber = false;
  if (numbers) for (const auto& row : numbers->property("recordedChanges").toList()) {
    const auto value = row.toMap();
    if (value["fighterKey"].toString() == enemy && value["text"].toString() == QStringLiteral("-10") && value["critical"].toBool()) criticalNumber = true;
  }
  if (!enemyLabels.contains(QStringLiteral("格挡")) || enemyLabels.contains(QStringLiteral("被暴击")) || !criticalNumber ||
      !allyLabels.contains(QStringLiteral("免疫")) || allyLabels.contains(QStringLiteral("被暴击")) ||
      !selfLabels.contains(QStringLiteral("闪避")) || selfLabels.contains(QStringLiteral("免疫")) || selfLabels.contains(QStringLiteral("格挡")))
    return fail("official miss/immunity/block/critical animation precedence lost");
  if (page.displayedState()[enemy].hp != 190 || page.displayedState()[ally].hp != 105) return fail("visual outcomes changed recorded balances");
  const auto qaDirectory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
  if (!qaDirectory.isEmpty()) {
    if (!QDir().mkpath(qaDirectory)) return fail("cannot create QA directory");
    QTest::qWait(25);
    if (!page.grab().save(QDir(qaDirectory).filePath(QStringLiteral("relation-outcomes.png")))) return fail("cannot capture multi-target presentation");
  }
  perspective->setCurrentIndex(perspective->findData(1));
  if (relation(source, enemy)["color"].toString() != "red" || relation(source, ally)["color"].toString() != "green" ||
      relation(source, enemy)["labels"].toStringList() != enemyArrow["labels"].toStringList())
    return fail("perspective switch inverted allegiance or strong-effect target");
  page.seek(3);
  for (int i = 0; i < page.playbackPhaseCount(); ++i) {
    page.seekPlaybackPhase(i); if (page.playbackPhaseKind() == QStringLiteral("attack")) break;
  }
  if (relation(enemy, source)["color"].toString() != "red" || relation(enemy, source)["labels"].toStringList() != QStringList{QStringLiteral("弱效")})
    return fail("enemy-origin attack used viewing-side color or previous strong effect");
  page.seek(2); page.seekPlaybackPhase(0);
  if (overlay(enemy) && !overlay(enemy)->property("effectLabels").toStringList().isEmpty()) return fail("rewind retained outcome overlay");
  if (page.replay().original != bytes) return fail("visualization changed original log");
  QWidget board; QWidget from(&board), to(&board); ReplayLinks unknown(&board);
  ReplayRelation missing; missing.source = &from; missing.target = &to; missing.targetTeam = 1;
  unknown.setRelations({missing}, {}, false);
  if (unknown.property("relationPresentations").toList().first().toMap()["color"].toString() != "gray") return fail("unknown team guessed as enemy or ally");
  page.hide(); return true;
}

bool verifyOutcomeVisualMotion() {
  const auto fail = [](const char* reason) { qCritical() << "outcome motion:" << reason; return false; };
  BattleReplay::Fighter fighter; fighter.key = "synthetic visual recipient"; fighter.name = QStringLiteral("合成动效测试精灵");
  fighter.hp = 80; fighter.maxHp = 100; fighter.shield = 20; fighter.power = 70; fighter.maxPower = 100; fighter.entered = true;
  ReplayUnitOverview cue; cue.badge = QStringLiteral("格挡 · 免疫"); cue.visualBadge = QStringLiteral("免疫"); cue.animate = true;
  ReplayCell cell(0); cell.resize(220, 204); cell.setPlaybackDriven(true);
  cell.display(1, {fighter}, {}, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}); cell.show(); QTest::qWait(20);
  auto* overlay = cell.findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  auto* hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  auto* shield = cell.findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  auto* power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  if (!overlay || !hp || !shield || !power || !overlay->property("playbackDriven").toBool()) return fail("shared overlay missing");
  for (const auto* bar : {hp, shield, power})
    if (QRect(overlay->mapTo(&cell, QPoint()), overlay->size()).intersects(QRect(bar->mapTo(&cell, QPoint()), bar->size())))
      return fail("effect graphic covers a numerical bar");
  cell.setPlaybackProgress(.15); const auto early = overlay->grab().toImage();
  cell.setPlaybackProgress(.7); const auto late = overlay->grab().toImage();
  if (early == late || overlay->property("currentEffectKind").toString() != "immune" || hp->text() != "80/100" || power->text() != "70/100")
    return fail("outcome did not animate or changed exact meter text");
  QTest::qWait(35);
  if (overlay->grab().toImage() != late || std::abs(overlay->property("playbackProgress").toReal() - .7) > .001)
    return fail("paused shared clock left an independent moving effect");
  auto* toggle = cell.findChild<QToolButton*>(QStringLiteral("unitBuffToggle")); toggle->click();
  QCoreApplication::processEvents();
  if (!overlay->isVisible() || !overlay->property("compact").toBool())
    return fail("expanded Buffs concealed the current outcome instead of retaining a compact cue");
  auto* expandedBuffs = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  if (!expandedBuffs || !expandedBuffs->property("expanded").toBool())
    return fail("compact outcome prevented inspecting expanded Buffs");
  toggle->click();
  cue.animate = false; cell.display(1, {fighter}, {}, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  overlay = cell.findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (!overlay || overlay->property("animated").toBool() || !overlay->isVisible()) return fail("animation switch removed static effect");
  cell.setPlaybackProgress(.15); const auto still = overlay->grab().toImage(); cell.setPlaybackProgress(.7);
  if (overlay->grab().toImage() != still) return fail("disabled animation still moves");
  // Buff stat icons and harm annotations are not judgement flags. They must
  // use only the current event, never initial membership or round history.
  const QJsonObject catalog{{"buffs", QJsonObject{
    {"991", QJsonObject{{"nm", "synthetic rate buff"}, {"beffect", QStringLiteral("暴击↑，闪避↓，格挡↑")}}},
    {"992", QJsonObject{{"nm", "synthetic sleep buff"}, {"beffect", QStringLiteral("睡眠")}}}}}};
  fighter.buffs = {"991", "992"}; cue = {};
  cue.visualBadge = QString();
  ReplayUnitEvent rate; rate.index = 1; rate.buffId = "991"; cue.events.append(rate);
  ReplayUnitEvent sleep; sleep.index = 2; sleep.buffId = "992"; cue.events.append(sleep);
  ReplayUnitEvent harm; harm.index = 3; harm.metric = QStringLiteral("生命"); harm.harmLabel = QStringLiteral("烧伤"); cue.events.append(harm);
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  overlay = cell.findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (!overlay) return fail("verified Buff effects not rendered");
  const auto kinds = overlay->property("effectKinds").toStringList(), labels = overlay->property("effectLabels").toStringList();
  if (!labels.contains(QStringLiteral("暴击↑")) || !labels.contains(QStringLiteral("睡眠")) ||
      kinds.contains("critical") || kinds.contains("miss") || kinds.contains("block")) return fail("Buff attribute change misread as a hit judgement");
  cue.events.clear(); cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  overlay = cell.findChild<QWidget*>(QStringLiteral("unitOutcomeOverlay"));
  if (overlay && !overlay->property("effectLabels").toStringList().isEmpty()) return fail("persistent Buff membership replayed an effect");
  // Two occupants share geometry, never judgement identity. Even the hidden
  // detail card must leave its own cue on that occupant's selectable row.
  auto other = fighter; other.key = "synthetic second recipient"; other.name = QStringLiteral("同格第二只精灵");
  cue = {}; cue.animate = true; cue.visualBadge = QStringLiteral("免疫");
  auto otherCue = cue; otherCue.visualBadge = QStringLiteral("闪避");
  cell.display(1, {fighter, other}, {}, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}, {other.key, otherCue}});
  QCoreApplication::processEvents();
  const auto miniFor = [&](const QString& key) -> QPushButton* {
    for (auto* row : cell.findChildren<QPushButton*>(QStringLiteral("unitMiniRow")))
      if (row->property("fighterKey").toString() == key) return row;
    return nullptr;
  };
  auto* firstMini = miniFor(fighter.key); auto* secondMini = miniFor(other.key);
  if (!firstMini || !secondMini || !firstMini->isVisible() || !secondMini->isVisible() ||
      firstMini->property("effectKinds").toStringList() != QStringList{"immune"} ||
      secondMini->property("effectKinds").toStringList() != QStringList{"miss"}) {
    qCritical() << "mini outcomes" << firstMini << secondMini;
    if (firstMini) qCritical() << firstMini->isVisible() << firstMini->property("effectKinds");
    if (secondMini) qCritical() << secondMini->isVisible() << secondMini->property("effectKinds");
    return fail("same-cell occupants mixed or concealed outcomes");
  }
  cell.setPlaybackProgress(.2); const auto firstMiniEarly = firstMini->grab().toImage();
  cell.setPlaybackProgress(.65); const auto firstMiniLate = firstMini->grab().toImage();
  if (firstMiniEarly == firstMiniLate || std::abs(secondMini->property("playbackProgress").toReal() - .65) > .001)
    return fail("same-cell outcome icons ignored shared motion");
  QTest::qWait(35);
  if (firstMini->grab().toImage() != firstMiniLate) return fail("paused same-cell outcome icon kept moving");
  secondMini->click();
  if (cell.property("visibleFighterKey").toString() != other.key ||
      firstMini->property("currentEffectKind").toString() != "immune" || secondMini->property("currentEffectKind").toString() != "miss")
    return fail("same-cell selection reassigned an outcome");
  cell.hide(); return true;
}

bool verifyRoundBuffNotices() {
  // Synthetic observations only: one Buff is added/triggered, stays through
  // the next action, survives in membership next round, then is removed.
  auto root = QJsonDocument::fromJson(battleReplayFixture()).object();
  auto br = root["br"].toObject();
  const auto action = [](int round, int playerRound, const QJsonObject& numbers) {
    return QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"ses", 1},
      {"csn", round}, {"pr", playerRound}, {"cn", QJsonObject{}},
      {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"tn", numbers}}}}};
  };
  br["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{
    action(1, 1, {{"bd_bfs", "1_99_1"}, {"bb_hps", "99_-10_2"}, {"ahp", 190}}),
    action(1, 2, {{"ahp", 190}}), action(2, 3, {{"ahp", 190}}),
    action(2, 4, {{"ad_bfs", "0_99_1"}, {"ahp", 190}})}}}};
  root["br"] = br; const auto original = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(original); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  const auto fail = [](const char* reason) { qCritical() << "round Buff notices:" << reason; return false; };
  QList<int> actions;
  for (int i = 0; i < page.replay().steps.size(); ++i) if (!page.replay().steps[i].actor.isEmpty()) actions.append(i);
  if (actions.size() != 4) return fail("fixture actions missing");
  const auto targetKey = BattleReplay::fighterKey(1, "tester", "7");
  const auto targetView = [&]() -> QTextBrowser* {
    for (auto* view : page.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs")))
      if (view->property("fighterKey").toString() == targetKey) return view;
    return nullptr;
  };
  const auto links = [](const QTextBrowser* view) {
    QSet<QString> result;
    for (auto block = view->document()->begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it)
        if (it.fragment().charFormat().isAnchor()) result.insert(it.fragment().charFormat().anchorHref());
    return result;
  };
  page.seek(0);
  for (auto* view : page.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs")))
    if (view->property("shownGroupCount").toInt() || view->property("roundEffectCount").toInt() || !view->toPlainText().isEmpty())
      return fail("initial membership shown as incoming effect");
  page.seek(actions[0]); page.seekPlaybackPhase(0);
  if (!targetView() || targetView()->property("roundEffectCount").toInt()) return fail("future effect appeared before receipt");
  int addIndex = -1;
  const auto& first = page.replay().steps[actions[0]];
  for (int i = 0; i < first.effects.size(); ++i)
    if (first.effects[i].kind == QStringLiteral("buff") && first.effects[i].target == targetKey) { addIndex = i; break; }
  if (addIndex < 0) return fail("add record missing");
  page.seekMoment(first.effects[addIndex].moment);
  if (targetView()->property("roundEffectCount").toInt() != 1) return fail("later trigger leaked into first receipt");
  page.seek(actions[0]);
  const auto firstLink = QStringLiteral("round-effect:%1:%2").arg(actions[0]).arg(addIndex);
  if (targetView()->property("roundEffectCount").toInt() != 2 || !links(targetView()).contains(firstLink) ||
      targetView()->property("shownGroupCount").toInt() || !targetView()->toPlainText().contains(QStringLiteral("本回合")))
    return fail("received add/trigger not visible while membership collapsed");
  page.seek(actions[1]); page.seekPlaybackPhase(0);
  if (targetView()->property("roundEffectCount").toInt() != 2 || !links(targetView()).contains(firstLink))
    return fail("next action/player round prematurely cleared notices");
  QMetaObject::invokeMethod(targetView(), "anchorClicked", Q_ARG(QUrl, QUrl(firstLink)));
  QCoreApplication::processEvents();
  if (page.currentStep() != actions[0] || page.currentMoment() != first.effects[addIndex].moment)
    return fail("history link selected a different action/effect");
  page.seek(actions[2]); page.seekPlaybackPhase(0);
  const auto* rounds = page.findChild<QComboBox*>(QStringLiteral("replayRounds"));
  if (targetView()->property("roundEffectCount").toInt() || !targetView()->toPlainText().isEmpty() ||
      targetView()->property("effectRound").toInt() != 2 || !page.displayedState()[targetKey].buffs.contains("99") ||
      !rounds || rounds->currentText() != QStringLiteral("第 2 回合"))
    return fail("new round retained notices or erased actual membership");
  auto* toggle = targetView()->parentWidget()->findChild<QToolButton*>(QStringLiteral("unitBuffToggle"));
  if (!toggle) return fail("expand control missing");
  toggle->click();
  if (!targetView()->property("expanded").toBool() || !links(targetView()).contains(QStringLiteral("buff:99#member0")))
    return fail("current Buff unavailable after notice expired");
  toggle->click(); page.seek(actions[3]);
  if (targetView()->property("roundEffectCount").toInt() != 1 || page.displayedState()[targetKey].buffs.contains("99") ||
      targetView()->toPlainText().contains(QStringLiteral("触发"))) return fail("removal lost or previous round leaked");
  page.seek(actions[0]); page.seekPlaybackPhase(0);
  if (targetView()->property("roundEffectCount").toInt() || !targetView()->toPlainText().isEmpty() || page.replay().original != original)
    return fail("rewind retained later notices or modified original");
  return true;
}

bool replayStateMatches(const BattleReplayPage& page, const QMap<QString, BattleReplay::Fighter>& expected) {
  const auto& actual = page.displayedState();
  if (actual.keys() != expected.keys()) return false;
  for (auto it = expected.begin(); it != expected.end(); ++it) {
    const auto& a = actual[it.key()]; const auto& e = it.value();
    if (a.hp != e.hp || a.maxHp != e.maxHp || a.power != e.power || a.maxPower != e.maxPower ||
        a.shield != e.shield || a.buffs != e.buffs || a.playerRemoved != e.playerRemoved ||
        a.entered != e.entered || a.displayLayersRestored != e.displayLayersRestored || a.displayLayersPath != e.displayLayersPath ||
        a.raceId != e.raceId || a.originRaceId != e.originRaceId ||
        a.originFighterKey != e.originFighterKey || a.originRelation != e.originRelation || a.originPath != e.originPath) return false;
    if (a.teamProgress.keys() != e.teamProgress.keys()) return false;
    for (auto progress = e.teamProgress.cbegin(); progress != e.teamProgress.cend(); ++progress) {
      const auto& actualProgress = a.teamProgress[progress.key()]; const auto& expectedProgress = progress.value();
      if (actualProgress.buffId != expectedProgress.buffId || actualProgress.path != expectedProgress.path ||
          actualProgress.current != expectedProgress.current || actualProgress.maximum != expectedProgress.maximum ||
          actualProgress.previous != expectedProgress.previous || actualProgress.delta != expectedProgress.delta ||
          actualProgress.skillEffected != expectedProgress.skillEffected || actualProgress.raw != expectedProgress.raw) return false;
    }
  }
  return true;
}

bool verifyDragonQueuePresentation() {
  // Synthetic instance IDs and actions, using extracted official definition /
  // execution IDs. This checks client display rules, not a real battle sample.
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object(); auto br = root["br"].toObject();
  auto original = br["or"].toObject(); auto teams = original["team"].toArray();
  for (int side = 0; side < 2; ++side) {
    auto team = teams[side].toObject(); auto players = team["players"].toArray(); auto player = players[0].toObject();
    player["drs"] = QJsonArray{
      QJsonObject{{"di", 1101}, {"dfi", 1}, {"q", 4}, {"nv", 5000}, {"ge", "aaAABB"}, {"dbfi", 210001}},
      QJsonObject{{"di", 2202}, {"dfi", 2}, {"q", 2}, {"nv", 2000}, {"ge", "AaAABB"}, {"dbfi", 210002}}};
    players[0] = player; team["players"] = players; teams[side] = team;
  }
  original["team"] = teams; br["or"] = original;
  const auto command = [](int buffer, const QJsonArray& ids, int round) {
    return QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"orrt", 2}, {"epbi", buffer}, {"eps", ids}, {"csn", round}, {"pr", 1},
      {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true}, {"tn", QJsonObject{}}}}},
      {"prl", QJsonArray{}}};
  };
  br["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{
    command(210001, {QStringLiteral("01101"), 2202}, 1), command(210002, {2202}, 1), command(200047, {1101}, 2)}}}};
  root["br"] = br; const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); if (!page.loadFile(file.fileName())) return false; page.show(); QTest::qWait(20);
  const auto fail = [](const char* reason) { qCritical() << "dragon queue UI:" << reason; return false; };
  auto* own = page.findChild<QLabel*>(QStringLiteral("replayDragons0"));
  auto* enemy = page.findChild<QLabel*>(QStringLiteral("replayDragons1"));
  auto* perspective = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  const QStringList ready{"1101:ready", "2202:ready"}, firstDone{"1101:finished", "2202:ready"},
    secondStarted{"1101:finished", "2202:fighting"}, finished{"1101:finished", "2202:finished"};
  if (!own || !enemy || !perspective || !own->isVisible() || !enemy->isVisible() ||
      own->property("dragonStates").toStringList() != ready || enemy->property("dragonStates").toStringList() != ready ||
      !own->accessibleName().contains(QStringLiteral("轮回界龙·傲")) || !own->accessibleName().contains(QStringLiteral("品质：S")) ||
      !own->accessibleName().contains(QStringLiteral("各100%")) || !own->property("activeDragonId").toString().isEmpty())
    return fail("initial roster/official definition/parameter or future state");
  if (!own->text().contains(QStringLiteral("dragon:0")) ||
      !QMetaObject::invokeMethod(own, "linkActivated", Q_ARG(QString, QStringLiteral("dragon:0")))) return fail("dragon detail link unavailable");
  auto* inspector = page.findChild<QWidget*>(QStringLiteral("replayBoardInspector"));
  auto* details = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  if (!inspector || !inspector->isVisible() || !details || !details->toPlainText().contains(QStringLiteral("轮回界龙·傲")) ||
      details->toPlainText().contains(QStringLiteral("实例 ID")) || details->toPlainText().contains(QStringLiteral("aaAABB")) ||
      details->toPlainText().contains(QStringLiteral("PetDragonDefines.as")) || details->toPlainText().contains(QStringLiteral("packageSha256")) ||
      !activateReplayTextAnchor(details, QStringLiteral("team-raw:0")) || !details->toPlainText().contains(QStringLiteral("实例 ID：1101")) ||
      !details->toPlainText().contains(QStringLiteral("能力值：5000")) || !details->toPlainText().contains(QStringLiteral("aaAABB")) ||
      !details->toPlainText().contains(QStringLiteral("PetDragonDefines.as")) || !details->toPlainText().contains(QStringLiteral("packageSha256")))
    return fail("in-board dragon detail lost raw parameters or official provenance");
  if (!activateReplayTextAnchor(details, QStringLiteral("team:0")) || details->toPlainText().contains(QStringLiteral("实例 ID")))
    return fail("raw dragon details did not return to public team overview");
  QList<int> actions;
  for (int i = 0; i < page.replay().steps.size(); ++i) if (page.replay().steps[i].raw.toObject().contains("eps")) actions.append(i);
  if (actions.size() != 3 || page.replay().steps[actions[2]].playbackKind != QStringLiteral("team-simple")) return fail("synthetic action branch setup");
  const QList<QStringList> before{ready, firstDone, secondStarted};
  const QList<QStringList> active{{"1101:fighting", "2202:ready"}, secondStarted, secondStarted};
  const QList<QStringList> complete{firstDone, secondStarted, finished};
  auto* sourceCard = page.findChild<QWidget*>(QStringLiteral("replayTeamSource0"));
  auto* relationLayer = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  auto* skillTitle = page.findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
  bool preciseAnchor = false;
  if (!sourceCard || !relationLayer || !skillTitle || sourceCard->isVisible()) return fail("initial source card invented an active dragon");
  for (int action = 0; action < actions.size(); ++action) {
    page.seek(actions[action]); bool sawStart = false, sawFinish = false;
    for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
      page.seekPlaybackPhase(phase); const auto kind = page.playbackPhaseKind();
      const bool preparing = kind == QStringLiteral("prepare") || kind == QStringLiteral("setup");
      const bool ending = kind == QStringLiteral("finish");
      const auto expected = preparing ? before[action] : ending ? complete[action] : active[action];
      if (own->property("dragonStates").toStringList() != expected || enemy->property("dragonStates").toStringList() != ready) {
        qCritical() << "dragon phase" << action << kind << own->property("dragonStates") << expected; return false;
      }
      if (action == 2 && (kind == QStringLiteral("cast") || kind == QStringLiteral("attack"))) return fail("simple completion invented attack stage");
      if (action < 2 && (kind == QStringLiteral("cast") || kind == QStringLiteral("attack"))) {
        const auto instance = action == 0 ? QStringLiteral("1101") : QStringLiteral("2202");
        const auto name = sourceCard->property("sourceName").toString();
        if (!sourceCard->isVisible() || sourceCard->property("sourceKind").toString() != "dragon" ||
            sourceCard->property("instanceId").toString() != instance || name.isEmpty() || !skillTitle->text().contains(name))
          return fail("specific dragon not presented as the current skill source");
        for (const auto& item : relationLayer->property("relationPresentations").toList()) {
          const auto relation = item.toMap();
          if (relation["sourceKey"].toString() != BattleReplay::fighterKey(0, "tester", "@team")) continue;
          if (relation["sourceObjectName"].toString() != "replayTeamSource0") return fail("team arrow still originates at the spirit meter");
          preciseAnchor = true;
        }
        if (action == 0 && kind == QStringLiteral("attack")) {
          const auto directory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
          if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard")); board && !directory.isEmpty()) {
            QDir().mkpath(directory); board->grab().save(QDir(directory).filePath(QStringLiteral("eleven-dragon-source.png")));
          }
        }
      }
      if (!preparing && !ending && action < 2) sawStart = true;
      sawFinish |= ending;
      const int moment = page.property("playbackLastMoment").toInt();
      if (moment < 0 || moment >= page.replay().steps[actions[action]].moments.size() ||
          !replayStateMatches(page, page.replay().steps[actions[action]].moments[moment].state)) return fail("dragon display changed combat snapshot");
    }
    if (!sawFinish || (action < 2 && !sawStart)) return fail("missing command boundary");
    page.seek(actions[action]);
    if (own->property("dragonStates").toStringList() != complete[action]) return fail("action overview disagrees with queue completion gate");
  }
  if (!preciseAnchor) return fail("no exact dragon source relation reached");
  perspective->setCurrentIndex(perspective->findData(1));
  if (own->property("dragonStates").toStringList() != ready || enemy->property("dragonStates").toStringList() != finished ||
      own->property("ownerKey").toString() != BattleReplay::fighterKey(1, "tester", "@team")) return fail("perspective mixed same-instance owners");
  perspective->setCurrentIndex(perspective->findData(0)); page.seek(actions[1]); page.seekPlaybackPhase(0);
  if (own->property("dragonStates").toStringList() != firstDone || !own->property("activeDragonId").toString().isEmpty())
    return fail("backward seek retained later START/completion");
  page.seek(0);
  if (own->property("dragonStates").toStringList() != ready || page.replay().original != bytes) return fail("rewind mutated source or retained queue state");
  // A later matching eps entry must not rescue an unknown first instance.
  auto unmatchedBr = br; auto lines = unmatchedBr["fr"].toArray(); auto line = lines[0].toObject();
  auto commands = line["rl"].toArray(); auto first = commands[0].toObject(); first["eps"] = QJsonArray{999999, 1101};
  commands[0] = first; line["rl"] = commands; lines[0] = line; unmatchedBr["fr"] = lines;
  auto unmatchedRoot = root; unmatchedRoot["br"] = unmatchedBr;
  QTemporaryFile unmatched; if (!unmatched.open()) return false; unmatched.write(QJsonDocument(unmatchedRoot).toJson()); unmatched.flush();
  if (!page.loadFile(unmatched.fileName())) return false;
  page.seek(actions[0]);
  if (sourceCard->property("sourceKind").toString() != "team-effect" || !sourceCard->property("instanceId").toString().isEmpty())
    return fail("unknown eps first item guessed a specific dragon from a later entry");
  QTemporaryFile empty; if (!empty.open()) return false; empty.write(battleReplayStateFixture()); empty.flush();
  if (!page.loadFile(empty.fileName()) || own->isVisible() || enemy->isVisible()) return fail("new report retained absent dragon roster");
  return true;
}

bool verifyClientHintsAndBuffPresentation() {
  // Deliberately synthetic grass -> water attack. The embedded official table
  // gives strong effect for this pair; HP and block remain raw report values.
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object(); auto br = root["br"].toObject();
  auto original = br["or"].toObject(); auto teams = original["team"].toArray();
  for (int side = 0; side < 2; ++side) {
    auto team = teams[side].toObject(); auto players = team["players"].toArray(); auto player = players[0].toObject();
    auto fighters = player["cts"].toArray(); auto fighter = fighters[0].toObject(); fighter["attr"] = side == 0 ? "2" : "3";
    fighters[0] = fighter; player["cts"] = fighters; players[0] = player; team["players"] = players; teams[side] = team;
  }
  original["team"] = teams; br["or"] = original;
  const QJsonObject attack{
    {"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"csn", 1}, {"ses", 1}, {"cn", QJsonObject{}},
    {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true}, {"isbl", true},
      {"tn", QJsonObject{{"att_hps", "-10"}, {"ahp", 190}}}}}}, {"prl", QJsonArray{}}};
  br["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{attack}}}};
  root["br"] = br; const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  const auto fail = [](const char* reason) { qCritical() << "client presentation UI:" << reason; return false; };
  int action = -1; for (int i = 0; i < page.replay().steps.size(); ++i) if (!page.replay().steps[i].actor.isEmpty()) { action = i; break; }
  if (action < 0) return fail("synthetic attack missing"); page.seek(action);
  bool sawHit = false; int hitPhase = -1;
  const auto targetKey = BattleReplay::fighterKey(1, "tester", "7");
  auto* hintLinks = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  const auto arrowHint = [&]() {
    if (hintLinks) for (const auto& item : hintLinks->property("relationPresentations").toList()) {
      const auto value = item.toMap();
      if (value["sourceKey"].toString() == BattleReplay::fighterKey(0, "tester", "7") && value["targetKey"].toString() == targetKey) return value;
    }
    return QVariantMap{};
  };
  for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
    page.seekPlaybackPhase(phase);
    auto* target = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
    auto* hint = target ? target->findChild<QLabel*>(QStringLiteral("unitClientHints")) : nullptr;
    if (!target) return fail("target cell missing");
    if (page.playbackPhaseKind() == QStringLiteral("attack")) {
      auto* judgement = target->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
      auto* hp = target->findChild<QProgressBar*>(QStringLiteral("hpBar"));
      const auto arrow = arrowHint();
      if (!judgement || !hp || (hint && hint->accessibleName().contains(QStringLiteral("强效"))) ||
          !arrow["labels"].toStringList().join(QString()).contains(QStringLiteral("强效")) ||
          !arrow["detail"].toString().contains(QStringLiteral("官方客户端提示")) ||
          !arrow["detail"].toString().contains(QStringLiteral("不是战报直接判定")) || !judgement->accessibleName().contains(QStringLiteral("格挡")) ||
          judgement->accessibleName().contains(QStringLiteral("强效")) || hp->text() != QStringLiteral("190/200") ||
          page.displayedState()[targetKey].hp != 190) return fail("hit hint mixed with judgement or changed raw HP");
      sawHit = true; hitPhase = phase;
    } else if (hint || !arrowHint()["labels"].toStringList().isEmpty()) return fail("hint appeared before hit or persisted into unrelated phase");
    const int moment = page.property("playbackLastMoment").toInt();
    if (moment >= 0 && !replayStateMatches(page, page.replay().steps[action].moments[moment].state)) return fail("hint changed snapshot");
  }
  if (!sawHit) return fail("attack phase did not show client hint");
  page.seekPlaybackPhase(hitPhase); page.seekPlaybackPhase(0);
  if (page.findChild<QLabel*>(QStringLiteral("unitClientHints")) || !arrowHint()["labels"].toStringList().isEmpty() ||
      page.displayedState()[targetKey].hp != 200 || page.replay().original != bytes)
    return fail("rewind retained future hint/result");

  auto catalog = battleReplayBattlefieldCatalog(); auto buffs = catalog["buffs"].toObject(); auto buff = buffs["99"].toObject();
  buff["mess"] = QStringLiteral("合成明确状态说明"); buff["once"] = "1"; buff["beffect"] = QStringLiteral("血量不显示 气势不显示"); buffs["99"] = buff; catalog["buffs"] = buffs;
  BattleReplay::Fighter chaser; chaser.key = "synthetic-chaser"; chaser.name = "synthetic chaser"; chaser.raceId = "9002";
  chaser.chaser = true; chaser.originRaceId = "9001"; chaser.originFighterKey = "synthetic-body";
  chaser.originRelation = "chaser"; chaser.originPath = "/synthetic/shared-cell";
  chaser.hp = 123; chaser.maxHp = 200; chaser.power = 67; chaser.maxPower = 100; chaser.shield = 8; chaser.buffs = {"99", "99"};
  ReplayCell cell(0); cell.resize(220, 204); cell.display(1, {chaser}, catalog, {}, {}); cell.show(); QTest::qWait(20);
  auto* card = cell.findChild<QFrame*>(QStringLiteral("replayUnitCard"));
  auto* view = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs")); auto* toggle = cell.findChild<QToolButton*>(QStringLiteral("unitBuffToggle"));
  auto* playbackToggle = cell.findChild<QToolButton*>(QStringLiteral("unitBuffPlaybackToggle"));
  auto* roles = cell.findChild<QLabel*>(QStringLiteral("unitRoles"));
  if (!card || !view || !toggle || !playbackToggle || !roles || card->property("originRaceId").toString() != "9001" ||
      card->property("originFighterKey").toString() != "synthetic-body" || !roles->accessibleName().contains(QStringLiteral("追击")) ||
      view->property("expanded").toBool() || view->property("shownGroupCount").toInt() != 0 ||
      view->toPlainText().contains(QStringLiteral("关联本体：synthetic form A")) || view->property("memberCount").toInt() != 2 ||
      view->property("primaryGroupCount").toInt() != 1 || view->property("playbackGroupCount").toInt() != 0 ||
      view->property("playbackExpanded").toBool()) return fail("origin or nontechnical Buff membership evidence lost");
  for (const auto& label : {QStringLiteral("隐藏附着动画"), QStringLiteral("动画仅播一次"), QStringLiteral("原版隐藏生命飘字"), QStringLiteral("原版隐藏气势飘字")})
    if (view->toPlainText().contains(label)) return fail("collapsed Buff exposed persistent presentation notes");
  QTest::mouseClick(toggle, Qt::LeftButton);
  if (!view->property("expanded").toBool() || view->property("shownGroupCount").toInt() != view->property("groupCount").toInt() ||
      !view->toPlainText().contains(QStringLiteral("关联本体：synthetic form A"))) return fail("expanded Buff lost verified origin");
  for (const auto& label : {QStringLiteral("隐藏附着动画"), QStringLiteral("动画仅播一次"), QStringLiteral("原版隐藏生命飘字"), QStringLiteral("原版隐藏气势飘字")})
    if (view->toPlainText().contains(label)) return fail("ordinary Buff collection exposed playback-only notes");
  QTest::mouseClick(playbackToggle, Qt::LeftButton);
  if (!view->property("playbackExpanded").toBool() || !view->property("expanded").toBool())
    return fail("playback details did not expand independently of the ordinary collection");
  for (const auto& label : {QStringLiteral("隐藏附着动画"), QStringLiteral("动画仅播一次"), QStringLiteral("原版隐藏生命飘字"), QStringLiteral("原版隐藏气势飘字")})
    if (!view->toPlainText().contains(label)) return fail("playback details lost verified presentation flag");
  if (cell.findChild<QProgressBar*>(QStringLiteral("hpBar"))->text() != QStringLiteral("123/200") ||
      cell.findChild<QProgressBar*>(QStringLiteral("powerBar"))->text() != QStringLiteral("67/100") ||
      view->property("memberCount").toInt() != 2) return fail("hidden/one-shot animation changed Buff membership or meter values");
  chaser.chaser = false; chaser.originRelation = "transform"; cell.display(1, {chaser}, catalog, {}, {});
  view = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs")); roles = cell.findChild<QLabel*>(QStringLiteral("unitRoles"));
  if (!view || !roles || !view->property("expanded").toBool() || !view->property("playbackExpanded").toBool() || !view->toPlainText().contains(QStringLiteral("变身来源：synthetic form A")) ||
      roles->accessibleName().contains(QStringLiteral("追击"))) return fail("refresh retained stale origin role or lost in-cell expansion");
  // Missing global skin mapping must not hide a separately verified local
  // body/origin, or turn a mere dictionary group into a precise mapping.
  auto pets = catalog["pets"].toObject(); auto form = pets["9002"].toObject();
  form["identity"] = QJsonObject{{"status", "unresolved"}, {"groupRaceId", "9001"}};
  pets["9002"] = form; catalog["pets"] = pets;
  chaser.formName = QStringLiteral("原名待核实 [9002]");
  cell.display(1, {chaser}, catalog, {}, {});
  auto* displayedName = cell.findChild<QPushButton*>(QStringLiteral("unitName"));
  if (!displayedName || displayedName->text() != QStringLiteral("synthetic form A（变身后）") ||
      displayedName->toolTip().contains(QStringLiteral("[9002]")) || displayedName->toolTip().contains(chaser.originPath))
    return fail("verified transform name or public tooltip invalid");
  chaser.chaser = true; chaser.originRelation = "chaser"; cell.display(1, {chaser}, catalog, {}, {});
  displayedName = cell.findChild<QPushButton*>(QStringLiteral("unitName"));
  if (!displayedName || displayedName->text() != QStringLiteral("synthetic form A（追击体）")) return fail("verified chaser body missing from name");
  chaser.originPath.clear(); cell.display(1, {chaser}, catalog, {}, {});
  displayedName = cell.findChild<QPushButton*>(QStringLiteral("unitName"));
  if (!displayedName || displayedName->text() != QStringLiteral("原名待确认")) return fail("dictionary group guessed as exact name without local evidence");
  return true;
}

bool verifyReadablePhaseTransitions() {
  // Reuse the synthetic death/recovery/form/entry fixture. The skill IDs come
  // from the embedded official catalog; no server-side calculation is tested.
  auto bytes = battleReplayBattlefieldFixture();
  bytes.replace("999999999", "106986"); bytes.replace("999999998", "401393");
  bytes.replace("9001", "6980"); bytes.replace("9002", "6981");
  bytes.replace("\"cn\":{\"rl\"", "\"cn\":{\"bd_bfs\":\"1_99_1\",\"rl\"");
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  const auto fail = [](const char* reason) { qCritical() << "readable playback:" << reason; return false; };
  const auto source = BattleReplay::fighterKey(0, "tester", "7");
  const auto summon = BattleReplay::fighterKey(0, "tester", "8");
  const auto clone = BattleReplay::fighterKey(0, "tester", "9");
  const auto target = BattleReplay::fighterKey(1, "foe", "7");
  const auto badge = [&](const QString& key) {
    for (const auto* card : page.findChildren<QWidget*>(QStringLiteral("replayUnitCard")))
      if (card->property("fighterKey").toString() == key) {
        const auto* label = card->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
        return label ? label->accessibleName() : QString();
      }
    return QString();
  };
  QSet<QString> observed;
  QList<int> majorDurations; int ordinaryDuration = -1, prepareDuration = -1;
  for (int action = 0; action < page.replay().steps.size(); ++action) {
    const auto& step = page.replay().steps[action];
    if (!step.raw.toObject().contains(QStringLiteral("si"))) continue;
    page.seek(action);
    auto previous = step.moments.first().state;
    for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
      page.seekPlaybackPhase(phase);
      const int moment = page.property("playbackLastMoment").toInt();
      if (moment < 0 || moment >= step.moments.size() || !replayStateMatches(page, step.moments[moment].state))
        return fail("readability stage altered recorded state");
      const auto& current = page.displayedState();
      const QMap<QString, QPair<QString, bool>> expected{
        {QStringLiteral("生命归零"), {target, previous[target].hp == 200 && current[target].hp == 0}},
        {QStringLiteral("生命恢复"), {target, previous[target].hp == 0 && current[target].hp == 200}},
        {QStringLiteral("变身"), {source, previous[source].raceId == QStringLiteral("6980") && current[source].raceId == QStringLiteral("6981")}},
        {QStringLiteral("分身登场"), {clone, !previous[clone].entered && current[clone].entered}},
        {QStringLiteral("召唤登场"), {summon, !previous[summon].entered && current[summon].entered}}
      };
      bool major = false;
      for (auto event = expected.begin(); event != expected.end(); ++event) {
        if (badge(event->first).contains(event.key()) != event->second) {
          qCritical() << "major badge" << action << phase << moment << event.key() << "expected" << event->second << "actual" << badge(event->first);
          return fail("major-change badge appeared early, lingered or was omitted");
        }
        if (event->second) { observed.insert(event.key()); major = true; }
      }
      const int duration = page.property("playbackBaseDurationMs").toInt();
      if (duration <= 0) return fail("stage duration missing");
      if (major) majorDurations.append(duration);
      if (page.playbackPhaseKind() == QStringLiteral("prepare")) prepareDuration = duration;
      if (!major && (page.playbackPhaseKind() == QStringLiteral("before") || page.playbackPhaseKind() == QStringLiteral("after")) &&
          step.moments[moment].part == QStringLiteral("power-buff")) ordinaryDuration = duration;
      previous = current;
    }
  }
  if (observed.size() != 5 || ordinaryDuration <= 0 || prepareDuration <= 0 || prepareDuration >= ordinaryDuration ||
      std::any_of(majorDurations.begin(), majorDurations.end(), [&](int duration) { return duration <= ordinaryDuration; }))
    return fail("major transitions were not retained longer than ordinary updates");

  // Selection does not turn a zero-change record into a Buff trigger. Likewise,
  // 0/-1 numeric display markers retain their changes without naming a Buff.
  for (const auto& sample : {qMakePair(QStringLiteral("99"), 0), qMakePair(QStringLiteral("-1"), -5), qMakePair(QStringLiteral("0"), -6)}) {
    const int expectedHp = 100 + sample.second;
    auto numericRoot = QJsonDocument::fromJson(battleReplayStateFixture()).object();
    auto numericBr = numericRoot[QStringLiteral("br")].toObject();
    numericBr[QStringLiteral("fr")] = QJsonArray{QJsonObject{{"rl", QJsonArray{QJsonObject{
      {"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"ses", 1}, {"csn", 1},
      {"cn", QJsonObject{{"bb_hps", QStringLiteral("%1_%2_1").arg(sample.first).arg(sample.second)}, {"ahp", expectedHp}, {"avg", 100}}},
      {"trl", QJsonArray{}}, {"prl", QJsonArray{}}
    }}}}}; numericRoot[QStringLiteral("br")] = numericBr;
    const auto numericBytes = QJsonDocument(numericRoot).toJson();
    QTemporaryFile numericFile; if (!numericFile.open()) return false;
    numericFile.write(numericBytes); numericFile.flush();
    if (!page.loadFile(numericFile.fileName())) return fail("non-trigger record fixture import");
    page.seek(2); int effectIndex = -1;
    const auto& effects = page.replay().steps[2].effects;
    for (int i = 0; i < effects.size(); ++i)
      if (effects[i].cause == QStringLiteral("buff") && effects[i].causeId == sample.first && effects[i].metric == QStringLiteral("生命") &&
          effects[i].delta == sample.second && effects[i].changed == (sample.second != 0)) { effectIndex = i; break; }
    if (effectIndex < 0) return fail("zero or numeric-marker record lost");
    page.selectEffect(effectIndex);
    const auto* sourceCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
    const auto* buffs = sourceCell ? sourceCell->findChild<QTextBrowser*>(QStringLiteral("unitBuffs")) : nullptr;
    const auto* hp = sourceCell ? sourceCell->findChild<QProgressBar*>(QStringLiteral("hpBar")) : nullptr;
    if (!buffs || !hp || page.currentMoment() != effects[effectIndex].moment || page.displayedState()[source].hp != expectedHp ||
        hp->text() != QStringLiteral("%1/100").arg(expectedHp) || !hp->accessibleName().contains(QStringLiteral("变化：%1").arg(sample.second)) ||
        buffs->toPlainText().contains(QStringLiteral("触发")) || buffs->accessibleName().contains(QStringLiteral("触发")) ||
        page.replay().original != numericBytes) return fail("numeric marker or zero change fabricated trigger or lost exact values");
  }
  return true;
}

QJsonObject verifyImportedPlayback(BattleReplayPage& page) {
  QElapsedTimer elapsed; elapsed.start();
  int phases = 0, limit = 64, teamSimplePhases = 0; QSet<int> actions, steps, teamSimpleActions; QJsonObject kinds;
  qint64 totalBaseDurationMs = 0;
  const auto original = page.replay().original;
  const auto report = [&](const QString& error) {
    return QJsonObject{{"passed", error.isEmpty()}, {"error", error}, {"sha256", page.replay().sha256},
      {"actions", actions.size()}, {"expectedActions", page.replay().actions}, {"phases", phases},
      {"teamSimpleActions", teamSimpleActions.size()}, {"teamSimplePhases", teamSimplePhases},
      {"totalBaseDurationMs", totalBaseDurationMs},
      {"stepsVisited", steps.size()}, {"phaseKinds", kinds}, {"step", page.currentStep()},
      {"phase", page.currentPlaybackPhase()}, {"phaseLimit", limit}, {"elapsedMs", elapsed.elapsed()}};
  };
  auto* mode = page.findChild<QComboBox*>(QStringLiteral("replayPlaybackMode"));
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* animation = page.findChild<QVariantAnimation*>(QStringLiteral("replayPhaseAnimation"));
  auto* timer = page.findChild<QTimer*>(QStringLiteral("replayPlaybackTimer"));
  auto* visual = page.findChild<QCheckBox*>(QStringLiteral("replayAnimations"));
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  if (page.replay().steps.isEmpty() || !mode || !play || !animation || !timer || !visual || !links)
    return report(QStringLiteral("missing replay or playback control"));
  // A generous data-size bound catches stalled/repeating phase clocks without
  // recomputing the official grouping or assuming any particular phase list.
  for (const auto& step : page.replay().steps) limit += 4 * (int(step.moments.size()) + 1);
  mode->setCurrentIndex(mode->findData(QStringLiteral("action"))); visual->setChecked(false);
  page.seek(0); play->click();
  if (!page.property("playbackRunning").toBool()) return report(QStringLiteral("playback did not start"));
  while (page.property("playbackRunning").toBool() && phases < limit) {
    const int index = page.currentStep(), phase = page.currentPlaybackPhase();
    if (index < 0 || index >= page.replay().steps.size() || phase < 0 || phase >= page.playbackPhaseCount())
      return report(QStringLiteral("step or phase index outside range"));
    const auto& step = page.replay().steps[index];
    const int first = page.property("playbackFirstMoment").toInt(), last = page.property("playbackLastMoment").toInt();
    if (first < -1 || last < -1 || first > last || first >= step.moments.size() || last >= step.moments.size() ||
        page.currentMoment() != last || (last < 0 && !step.moments.isEmpty()))
      return report(QStringLiteral("phase references invalid recorded moments"));
    if (first >= 0 && last > first && step.moments[first].part == QStringLiteral("values") &&
        step.moments[last].part == QStringLiteral("power-buff"))
      return report(QStringLiteral("value callback merged with later power/Buff callback"));
    const auto expected = expectedReadingState(page);
    if (!replayStateMatches(page, expected) || stateLedger(page.displayedState()) != stateLedger(expected) ||
        battlefieldLedger(page.displayedState()) != battlefieldLedger(expected))
      return report(QStringLiteral("displayed state differs from recorded snapshot"));
    if (animation->state() != QAbstractAnimation::Running || animation->duration() <= 0 || timer->isActive())
      return report(QStringLiteral("continuous playback has no active single phase clock"));
    const int baseDuration = page.property("playbackBaseDurationMs").toInt();
    const qreal actionProgress = page.property("playbackActionProgress").toReal();
    if (baseDuration <= 0 || !(actionProgress >= 0 && actionProgress <= 1))
      return report(QStringLiteral("invalid playback duration or action progress"));
    totalBaseDurationMs += baseDuration;
    ++phases; steps.insert(index); if (!step.actor.isEmpty()) actions.insert(index);
    const auto kind = page.playbackPhaseKind(); kinds[kind] = kinds[kind].toInt() + 1;
    if (step.playbackKind == QStringLiteral("team-simple")) {
      teamSimpleActions.insert(index); ++teamSimplePhases;
      if (kind == QStringLiteral("cast") || kind == QStringLiteral("attack") ||
          kind == QStringLiteral("counter-cast") || kind == QStringLiteral("counter") ||
          links->property("connectionCount").toInt() != 0)
        return report(QStringLiteral("team-simple action fabricated a cast, attack or directed attack relation"));
    }
    // No event loop or real wait: the one animation's finished signal advances
    // synchronously, so every presented phase is inspected exactly once.
    animation->setCurrentTime(animation->duration());
    if (page.currentStep() == index && page.property("playbackActionProgress").toReal() + 1e-9 < actionProgress)
      return report(QStringLiteral("action progress moved backward across phases"));
    if (page.property("playbackRunning").toBool() &&
        !((page.currentStep() == index && page.currentPlaybackPhase() == phase + 1) ||
          (page.currentStep() > index && page.currentPlaybackPhase() == 0)))
      return report(QStringLiteral("phase repeated, skipped or moved backward"));
  }
  if (page.property("playbackRunning").toBool()) return report(QStringLiteral("phase count exceeded bound"));
  if (page.currentStep() != page.replay().steps.size() - 1 || timer->isActive() ||
      animation->state() == QAbstractAnimation::Running || !replayStateMatches(page, page.replay().steps.last().state) ||
      qAbs(page.property("playbackActionProgress").toReal() - 1) > 1e-9)
    return report(QStringLiteral("playback did not stop at final recorded state"));
  if (actions.size() != page.replay().actions || page.replay().original != original)
    return report(QStringLiteral("action coverage or original log changed"));
  return report({});
}

QMap<QString, BattleReplay::Fighter> expectedReadingState(const BattleReplayPage& page) {
  const auto& step = page.replay().steps[page.currentStep()];
  const int moment = page.currentMoment();
  const int count = page.property("numericSegmentCount").toInt();
  if (count > 0) {
    const auto slices = ReplayMeterSequence::split(step, moment);
    const int part = page.property("numericSegment").toInt();
    if (slices.size() != count || part < 0 || part >= count) return {};
    return slices[part].state;
  }
  return moment >= 0 ? step.moments[moment].state : step.state;
}

QJsonObject verifyRealtimePlayback(BattleReplayPage& page, bool requireTransientChain = false) {
  QElapsedTimer wall; wall.start();
  QString error; int samples = 0, intermediate = 0, relationSamples = 0, pausedSamples = 0;
  QSet<int> actions; QSet<QString> phases; QJsonArray sampledSteps;
  bool paused = false, resumed = false, pauseAttempted = false;
  int transientSamples = 0, minimumGhostBoundary = 10000;
  QSet<QString> lossSegments, recoverySegments;
  const auto original = page.replay().original;
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* mode = page.findChild<QComboBox*>(QStringLiteral("replayPlaybackMode"));
  auto* speed = page.findChild<QComboBox*>(QStringLiteral("replaySpeed"));
  auto* visual = page.findChild<QCheckBox*>(QStringLiteral("replayAnimations"));
  auto* clock = page.findChild<QVariantAnimation*>(QStringLiteral("replayPhaseAnimation"));
  auto* detailedTimer = page.findChild<QTimer*>(QStringLiteral("replayPlaybackTimer"));
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  const auto report = [&] {
    return QJsonObject{{"passed", error.isEmpty()}, {"error", error}, {"sha256", page.replay().sha256},
      {"clockMode", "real event loop; no setCurrentTime; animations enabled"},
      {"coverage", "last two ordinary actions and following records through final state"},
      {"selectedActionSteps", sampledSteps}, {"actionsVisited", actions.size()}, {"phasesObserved", phases.size()},
      {"samples", samples}, {"intermediateSamples", intermediate}, {"relationSamples", relationSamples},
      {"transientSamples", transientSamples}, {"minimumGhostBoundary", minimumGhostBoundary},
      {"pausedSamples", pausedSamples}, {"pauseResumeVerified", resumed}, {"elapsedMs", wall.elapsed()},
      {"finalStep", page.currentStep()}};
  };
  if (!play || !mode || !speed || !visual || !clock || !detailedTimer || !links || page.replay().steps.isEmpty()) {
    error = QStringLiteral("missing replay or playback controls"); return report();
  }
  QList<int> candidates;
  for (int index = 0; index < page.replay().steps.size(); ++index) {
    const auto& step = page.replay().steps[index];
    if (!step.actor.isEmpty() && (step.playbackKind == QStringLiteral("normal") || step.playbackKind == QStringLiteral("summon")))
      candidates.append(index);
  }
  if (candidates.isEmpty()) {
    error = QStringLiteral("no ordinary action available for realtime sample"); return report();
  }
  const int start = candidates[std::max(0, int(candidates.size()) - 2)];
  for (int index : candidates) if (index >= start) sampledSteps.append(index);
  mode->setCurrentIndex(mode->findData(QStringLiteral("action")));
  visual->setChecked(true); speed->setCurrentIndex(speed->findData(1800));
  page.seek(start);
  QEventLoop loop; QTimer sampleTimer, deadline;
  sampleTimer.setInterval(10); deadline.setSingleShot(true); deadline.setInterval(45000);
  int pausedStep = -1, pausedPhase = -1, pausedTime = -1;
  qint64 resumeAt = -1; qreal pausedProgress = -1, pausedActionProgress = -1;
  QMap<QString, BattleReplay::Fighter> pausedState;
  QMap<QObject*, QVariant> pausedVisuals;
  int previousStep = -1; qreal previousActionProgress = -1;
  const auto fail = [&](const QString& reason) { error = reason; loop.quit(); };
  const auto inspect = [&] {
    ++samples;
    if (!page.property("playbackRunning").toBool() && !paused) { loop.quit(); return; }
    const int index = page.currentStep(), phase = page.currentPlaybackPhase();
    if (index < start || index >= page.replay().steps.size() || phase < 0 || phase >= page.playbackPhaseCount()) {
      fail(QStringLiteral("invalid realtime step/phase")); return;
    }
    const auto& step = page.replay().steps[index];
    const int moment = page.currentMoment();
    if (moment < -1 || moment >= step.moments.size()) { fail(QStringLiteral("invalid realtime snapshot moment")); return; }
    const auto expected = expectedReadingState(page);
    if (!replayStateMatches(page, expected) || stateLedger(page.displayedState()) != stateLedger(expected) ||
        battlefieldLedger(page.displayedState()) != battlefieldLedger(expected)) {
      fail(QStringLiteral("wall-clock animation changed a recorded state")); return;
    }
    if (detailedTimer->isActive() || !visual->isChecked()) { fail(QStringLiteral("realtime run used detailed timer or disabled animation")); return; }
    const qreal progress = clock->currentValue().toReal();
    const qreal actionProgress = page.property("playbackActionProgress").toReal();
    if (index == previousStep && actionProgress + .0001 < previousActionProgress) {
      fail(QStringLiteral("realtime action progress moved backwards")); return;
    }
    previousStep = index; previousActionProgress = actionProgress;
    phases.insert(QStringLiteral("%1:%2").arg(index).arg(phase));
    if (!step.actor.isEmpty()) actions.insert(index);
    if (progress > 0 && progress < 1) ++intermediate;
    for (auto* cell : page.findChildren<ReplayCell*>()) {
      if (!cell->property("playbackDriven").toBool() || qAbs(cell->property("playbackProgress").toReal() - progress) > .02) {
        fail(QStringLiteral("cell decoration left the shared playback clock")); return;
      }
      if (cell->property("actorEmphasis").toBool() &&
          (cell->property("visualScale").toReal() <= 1 || !cell->graphicsEffect() || !cell->graphicsEffect()->isEnabled())) {
        fail(QStringLiteral("current actor lost its visual emphasis during real playback")); return;
      }
      for (auto* bar : cell->findChildren<QProgressBar*>()) {
        if (!bar->isVisibleTo(cell) || !bar->property("actionMetersPending").toBool()) continue;
        const auto segments = bar->property("actionGhostSegments").toList();
        if (page.property("numericSegmentCount").toInt() < 2 || segments.size() != 1 || bar->property("metric").toString() != QStringLiteral("生命")) continue;
        const auto entry = segments.first().toMap();
        const bool lostToZero = entry["before"].toLongLong() == 100 && entry["after"].toLongLong() == 0 && entry["kind"].toString() == "remove";
        const bool recovered = entry["before"].toLongLong() == 0 && entry["after"].toLongLong() == 100 && entry["kind"].toString() == "add";
        if (!lostToZero && !recovered) continue;
        const auto segmentKey = QStringLiteral("%1/%2/%3").arg(index).arg(phase).arg(bar->property("fighterKey").toString());
        (lostToZero ? lossSegments : recoverySegments).insert(segmentKey);
        ++transientSamples;
        minimumGhostBoundary = std::min(minimumGhostBoundary, qRound(segments.first().toMap()["from"].toDouble() * 10000));
        if (bar->text() != (lostToZero ? QStringLiteral("0/100") : QStringLiteral("100/100")) || bar->value() != (lostToZero ? 0 : 10000)) {
          fail(QStringLiteral("reading slice did not display its own exact recorded balance")); return;
        }
        auto* numbers = page.findChild<QWidget*>(QStringLiteral("replayMeterOverlay"));
        bool lossNumber = false, gainNumber = false;
        if (numbers) for (const auto& raw : numbers->property("recordedChanges").toList()) {
          const auto item = raw.toMap();
          if (item["fighterKey"] != bar->property("fighterKey") || item["metric"] != bar->property("metric")) continue;
          lossNumber |= item["text"].toString() == QStringLiteral("-100");
          gainNumber |= item["text"].toString() == QStringLiteral("+100");
        }
        if (lossNumber != lostToZero || gainNumber != recovered) {
          fail(QStringLiteral("damage and recovery callouts were accumulated instead of replaced in order")); return;
        }
      }
    }
    if (links->property("connectionCount").toInt() > 0 && links->property("playbackProgress").toReal() < 1) {
      ++relationSamples;
      if (!links->property("playbackDriven").toBool() || qAbs(links->property("playbackProgress").toReal() - progress) > .02) {
        fail(QStringLiteral("directed relations left the shared playback clock")); return;
      }
    }
    for (auto* local : page.findChildren<QVariantAnimation*>()) {
      if (local != clock && local->state() == QAbstractAnimation::Running) {
        fail(QStringLiteral("a second animation clock kept running")); return;
      }
    }
    if (paused) {
      ++pausedSamples;
      if (page.property("playbackRunning").toBool() || clock->state() != QAbstractAnimation::Paused ||
          clock->currentTime() != pausedTime || page.currentStep() != pausedStep || page.currentPlaybackPhase() != pausedPhase ||
          !replayStateMatches(page, pausedState) || qAbs(actionProgress - pausedActionProgress) > 1e-9) {
        fail(QStringLiteral("pause did not freeze state and presentation clock")); return;
      }
      for (auto it = pausedVisuals.cbegin(); it != pausedVisuals.cend(); ++it)
        if (it.key()->property("playbackProgress") != it.value()) { fail(QStringLiteral("paused visual continued moving")); return; }
      if (wall.elapsed() >= resumeAt) {
        speed->setCurrentIndex(speed->findData(450));
        if (!replayStateMatches(page, pausedState) || qAbs(page.property("playbackActionProgress").toReal() - pausedActionProgress) > .002 ||
            qAbs(links->property("playbackProgress").toReal() - pausedProgress) > .02) {
          fail(QStringLiteral("paused speed change altered state or advanced the relation")); return;
        }
        paused = false; resumed = true; play->click();
        if (!page.property("playbackRunning").toBool() || clock->state() != QAbstractAnimation::Running) {
          fail(QStringLiteral("paused playback did not resume")); return;
        }
      }
      return;
    }
    if (!pauseAttempted && links->property("connectionCount").toInt() > 0 && progress > .15 && progress < .8) {
      pauseAttempted = true; play->click(); paused = true;
      pausedStep = index; pausedPhase = phase; pausedTime = clock->currentTime();
      pausedProgress = links->property("playbackProgress").toReal(); pausedActionProgress = actionProgress;
      pausedState = page.displayedState(); resumeAt = wall.elapsed() + 120;
      pausedVisuals.insert(links, links->property("playbackProgress"));
      for (auto* cell : page.findChildren<ReplayCell*>()) pausedVisuals.insert(cell, cell->property("playbackProgress"));
    }
  };
  QObject::connect(&sampleTimer, &QTimer::timeout, &loop, inspect);
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { fail(QStringLiteral("realtime sample exceeded 45 seconds")); });
  play->click(); sampleTimer.start(); deadline.start(); loop.exec();
  sampleTimer.stop(); deadline.stop();
  if (!error.isEmpty()) { if (page.property("playbackRunning").toBool()) play->click(); return report(); }
  if (!resumed || intermediate < 3 || relationSamples < 2 || pausedSamples < 2)
    error = QStringLiteral("sample did not exercise real intermediate frames, relations and pause/resume");
  else if (requireTransientChain && (transientSamples < 2 || minimumGhostBoundary != 0 || lossSegments.isEmpty() || recoverySegments.isEmpty()))
    error = QStringLiteral("recorded 100-to-zero-to-100 chain lost its ordered loss/recovery ghosts during real playback");
  else if (page.property("playbackRunning").toBool() || clock->state() == QAbstractAnimation::Running ||
      page.currentStep() != page.replay().steps.size() - 1 || !replayStateMatches(page, page.replay().steps.last().state) ||
      qAbs(page.property("playbackActionProgress").toReal() - 1) > 1e-9 || page.replay().original != original)
    error = QStringLiteral("realtime playback did not preserve and stop at the final recorded state");
  return report();
}

bool verifyIncomingReplayAndIdentity() {
  const auto fail = [](const char* why) { qCritical() << "incoming replay UI:" << why; return false; };
  QTemporaryFile originalFile; if (!originalFile.open()) return false;
  const auto original = battleReplayFixture(); originalFile.write(original); originalFile.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); page.show();
  if (!page.loadFile(originalFile.fileName())) return fail("initial import failed");
  QCoreApplication::processEvents();
  auto* incoming = page.findChild<QToolButton*>(QStringLiteral("replayIncoming"));
  auto* source = page.findChild<QToolButton*>(QStringLiteral("replaySourceStatus"));
  auto* details = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  auto* ownTeam = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* clock = page.findChild<QVariantAnimation*>(QStringLiteral("replayPhaseAnimation"));
  if (!incoming || !source || !details || !ownTeam || !play || !clock) return fail("entry controls missing");
  auto nextRoot = QJsonDocument::fromJson(original).object(); auto br = nextRoot[QStringLiteral("br")].toObject();
  auto initial = br[QStringLiteral("or")].toObject(); auto teams = initial[QStringLiteral("team")].toArray();
  auto enemy = teams[1].toObject(); auto players = enemy[QStringLiteral("players")].toArray();
  auto player = players[0].toObject(); player[QStringLiteral("name")] = QStringLiteral("synthetic-local-nickname");
  players[0] = player; enemy[QStringLiteral("players")] = players; teams[1] = enemy; initial[QStringLiteral("team")] = teams;
  br[QStringLiteral("or")] = initial; br[QStringLiteral("fr")] = QJsonArray{}; br[QStringLiteral("bi")] = 901;
  nextRoot[QStringLiteral("br")] = br; const auto nextBytes = QJsonDocument(nextRoot).toJson();
  const auto sha = QString::fromLatin1(QCryptographicHash::hash(nextBytes, QCryptographicHash::Sha256).toHex());
  page.setAccountIdentity(QStringLiteral("synthetic-local-nickname"), true, QStringLiteral("synthetic accepted-session login evidence"));
  page.setReplaySourceStatus(QStringLiteral("synthetic existing-channel + read-only directory status"));
  ownTeam->setCurrentIndex(ownTeam->findData(0)); page.seek(2); play->click();
  const int currentStep = page.currentStep(), currentPhase = page.currentPlaybackPhase(), time = clock->currentTime();
  const auto snapshot = page.displayedState();
  page.enqueueReplay(QStringLiteral("synthetic-fail.txt"), nextBytes, sha);
  page.enqueueReplay(QStringLiteral("same-content-win.txt"), nextBytes, sha);
  page.enqueueReplay(QStringLiteral("wrong-sha.txt"), nextBytes, QStringLiteral("invalid"));
  if (!page.property("playbackRunning").toBool() || page.replay().original != original || !replayStateMatches(page, snapshot) ||
      page.currentStep() != currentStep || page.currentPlaybackPhase() != currentPhase || clock->currentTime() != time ||
      incoming->property("pendingCount").toInt() != 1 || incoming->isHidden() || ownTeam->currentData().toInt() != 0)
    return fail("arrival replaced/interrupted current playback or bypassed dedup");
  page.setAccountIdentity(QStringLiteral("tester"), true, QStringLiteral("synthetic asynchronous nickname refresh"));
  if (ownTeam->currentData().toInt() != 0 || !page.property("playbackRunning").toBool()) return fail("identity refresh stole manual perspective");
  page.setAccountIdentity(QStringLiteral("synthetic-local-nickname"), true, QStringLiteral("synthetic accepted-session login evidence"));
  QTest::mouseClick(incoming, Qt::LeftButton);
  if (page.property("playbackRunning").toBool() || !details->isVisible() || details->toPlainText().contains(sha) ||
      !hasReplayTextAnchor(details, QStringLiteral("incoming:") + sha) ||
      !clickReplayTextAnchor(details, QStringLiteral("incoming:") + sha)) return fail("pending snapshot cannot be inspected/opened by click");
  if (page.replay().original != nextBytes || incoming->property("pendingCount").toInt() != 0 ||
      ownTeam->currentData().toInt() != 1 || !ownTeam->property("identityMatched").toBool())
    return fail("unique accepted nickname did not select actual team at explicit open");
  ownTeam->setCurrentIndex(ownTeam->findData(0));
  page.setAccountIdentity(QStringLiteral("synthetic-local-nickname"), true, QStringLiteral("same identity update"));
  if (ownTeam->currentData().toInt() != 0) return fail("late identity update overrode manual side");
  page.setAccountIdentity(QStringLiteral("synthetic-local-nickname"), false, QStringLiteral("synthetic unavailable evidence"));
  QTemporaryFile nextFile; if (!nextFile.open()) return false; nextFile.write(nextBytes); nextFile.flush();
  if (!page.loadFile(nextFile.fileName()) || ownTeam->property("identityMatched").toBool()) return fail("unreliable identity auto-selected a side");
  page.setAccountIdentity(QStringLiteral("tester"), true, QStringLiteral("synthetic nickname present on both teams"));
  if (!page.loadFile(originalFile.fileName()) || ownTeam->property("identityMatched").toBool()) return fail("ambiguous nickname auto-selected a side");
  QTest::mouseClick(source, Qt::LeftButton);
  if (details->toPlainText().contains(QStringLiteral("synthetic existing-channel")) ||
      details->toPlainText().contains(QStringLiteral("synthetic nickname present on both teams")) || details->toPlainText().contains(sha))
    return fail("public source summary exposed raw receiving or account evidence");
  if (!clickReplayTextAnchor(details, QStringLiteral("source-raw:all")) ||
      !details->toPlainText().contains(QStringLiteral("synthetic existing-channel + read-only directory status")) ||
      !details->toPlainText().contains(QStringLiteral("synthetic nickname present on both teams")))
    return fail("source state is inaccessible from explicit raw entry");
  return true;
}

QByteArray realtimeMultiUnitFixture() {
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object(); auto br = root[QStringLiteral("br")].toObject();
  auto initial = br[QStringLiteral("or")].toObject(); auto teams = initial[QStringLiteral("team")].toArray();
  auto own = teams[0].toObject(); auto players = own[QStringLiteral("players")].toArray();
  auto player = players[0].toObject(); auto fighters = player[QStringLiteral("cts")].toArray();
  fighters.append(QJsonObject{{"id", 8}, {"name", "synthetic same-cell target"}, {"x", 0}, {"y", 0},
    {"hp", 100}, {"maxHp", 100}, {"shp", 20}, {"vigour", 45}});
  player[QStringLiteral("cts")] = fighters; players[0] = player; own[QStringLiteral("players")] = players;
  teams[0] = own;
  auto enemy = teams[1].toObject(); auto enemyPlayers = enemy[QStringLiteral("players")].toArray();
  auto enemyPlayer = enemyPlayers[0].toObject(); auto enemyFighters = enemyPlayer[QStringLiteral("cts")].toArray();
  auto enemyFighter = enemyFighters[0].toObject(); enemyFighter[QStringLiteral("hp")] = 100;
  enemyFighter[QStringLiteral("maxHp")] = 100; enemyFighters[0] = enemyFighter;
  enemyPlayer[QStringLiteral("cts")] = enemyFighters; enemyPlayers[0] = enemyPlayer;
  enemy[QStringLiteral("players")] = enemyPlayers; teams[1] = enemy;
  initial[QStringLiteral("team")] = teams; br[QStringLiteral("or")] = initial;
  const QJsonObject first{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106853}, {"ses", 1}, {"csn", 1},
    {"cn", QJsonObject{{"ahp", 100}, {"avg", 100}}}, {"prl", QJsonArray{}},
    {"trl", QJsonArray{
      QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 8}, {"isse", true}, {"ishit", true}, {"isbl", true},
        {"tn", QJsonObject{{"att_hps", "-10"}, {"att_shps", "-5"}, {"att_vgs", "5"}, {"ahp", 90}, {"avg", 50}}}},
      QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true}, {"isIm", true},
        {"tn", QJsonObject{{"att_hps", "0"}, {"ahp", 100}, {"avg", 60}}}}
    }}};
  // The official consumer applies source attack numbers separately; remote
  // target numbers are combined and cannot establish a transient HP chain.
  const QJsonObject second{{"cti", 1}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"ses", 1}, {"csn", 2},
    {"cn", QJsonObject{{"att_hps", "-100,100"}, {"ahp", 100}, {"avg", 60}}}, {"prl", QJsonArray{}},
    {"trl", QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true},
      {"tn", QJsonObject{{"att_hps", "-5"}, {"ahp", 95}, {"avg", 100}}}}}}};
  br[QStringLiteral("fr")] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{first, second}}}};
  root[QStringLiteral("br")] = br; return QJsonDocument(root).toJson();
}

bool verifySameCellUnitAnchors() {
  const auto fail = [](const char* why) { qCritical() << "same-cell occupants:" << why; return false; };
  BattleReplay::Fighter actor; actor.key = QStringLiteral("synthetic-actor"); actor.name = QStringLiteral("原名 A <带符号>");
  actor.hp = 100; actor.maxHp = 100; actor.shield = 25; actor.power = 75; actor.maxPower = 100;
  actor.entered = true; actor.presence = QStringLiteral("在场"); actor.team = 0;
  auto target = actor; target.key = QStringLiteral("synthetic-target"); target.name = QStringLiteral("原名 B");
  target.hp = 0; target.shield.reset(); target.power = 90; target.presence = QStringLiteral("已退场（生命为 0）");
  target.buffs = {QStringLiteral("99"), QStringLiteral("99"), QStringLiteral("99")};
  target.buffMembers = {{"99", "actor", "A", "synthetic/one", 0}, {"99", "enemy", "B", "synthetic/two", 1},
    {"99", {}, {}, "synthetic/unrecorded", 1}};
  ReplayUnitOverview change; change.target = true; change.animate = true;
  change.hpBefore = 100; change.maxHpBefore = 100; change.powerBefore = 85; change.maxPowerBefore = 100;
  change.badge = QStringLiteral("格挡 · 免疫 · 无效 · 反击");
  for (const auto& metric : {QStringLiteral("生命"), QStringLiteral("护盾"), QStringLiteral("气势")}) {
    ReplayUnitEvent event; event.index = static_cast<int>(change.events.size()); event.metric = metric;
    event.label = metric + QStringLiteral("有记录变化"); event.valueText = metric == QStringLiteral("生命") ? QStringLiteral("-100") : QStringLiteral("+5");
    event.detail = QStringLiteral("synthetic recorded delta"); change.events.append(event);
  }
  auto catalog = battleReplayTestCatalog();
  auto buffs = catalog[QStringLiteral("buffs")].toObject(); auto buff = buffs[QStringLiteral("99")].toObject();
  buff[QStringLiteral("mess")] = QStringLiteral("<b>#pet#</b> 的已记录说明"); buffs[QStringLiteral("99")] = buff;
  catalog[QStringLiteral("buffs")] = buffs;
  ReplayCell cell(0); cell.setFixedSize(148, 174); cell.show(); cell.setPlaybackDriven(true);
  cell.display(1, {actor, target}, catalog, actor.key, {target.key}, actor.key, target.key, {}, {}, {{target.key, change}}, 0);
  QCoreApplication::processEvents();
  auto* from = cell.relationAnchor(actor.key); auto* to = cell.relationAnchor(target.key);
  if (!from || !to || from == to || !from->isVisibleTo(&cell) || !to->isVisibleTo(&cell) ||
      !from->property("isActor").toBool() || !to->property("isTarget").toBool() ||
      !to->property("phaseChanges").toString().contains(QStringLiteral("生命")) ||
      !cell.property("actorEmphasis").toBool() || cell.property("deadUnit").toBool())
    return fail("actor and recipient did not retain distinct visible anchors/statuses");
  QString geometryFailure;
  const auto dumpDenseFailure = [&] {
    qCritical() << "dense target geometry failure:" << geometryFailure << "cell" << cell.size()
      << "visibleFighterKey" << cell.property("visibleFighterKey") << "selectedUnitDead" << cell.property("selectedUnitDead")
      << "deadUnit" << cell.property("deadUnit") << "actorEmphasis" << cell.property("actorEmphasis");
    for (auto* child : cell.findChildren<QWidget*>()) {
      if (child->objectName().isEmpty()) continue;
      qCritical() << child->objectName() << child->metaObject()->className() << "fighter" << child->property("fighterKey")
        << "visible" << child->isVisibleTo(&cell) << "rect" << QRect(child->mapTo(&cell, QPoint()), child->size())
        << "minimum" << child->minimumSize() << "hint" << child->minimumSizeHint();
      if (auto* browser = qobject_cast<QTextBrowser*>(child)) qCritical() << "viewport" << browser->viewport()->geometry();
    }
    const auto qaDirectory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
    if (!qaDirectory.isEmpty() && QDir().mkpath(qaDirectory)) {
      const auto path = QDir(qaDirectory).filePath(QStringLiteral("same-cell-failure.png"));
      if (cell.grab().save(path)) qCritical() << "dense failure image:" << path;
    }
  };
  const auto verifyVisibleContent = [&] {
    geometryFailure.clear();
    int bars = 0, toggles = 0, documents = 0; QList<QRect> bounds;
    for (auto* bar : cell.findChildren<QProgressBar*>()) {
      if (!bar->isVisibleTo(&cell)) continue;
      const QRect rect(bar->mapTo(&cell, QPoint()), bar->size());
      if (rect.height() < 16 || !cell.rect().contains(rect)) { geometryFailure = bar->objectName() + QStringLiteral(" clipped/too short"); return false; }
      for (const auto& previous : bounds) if (previous.intersects(rect)) { geometryFailure = bar->objectName() + QStringLiteral(" overlaps another meter"); return false; }
      bounds.append(rect); ++bars;
    }
    for (auto* toggle : cell.findChildren<QToolButton*>(QStringLiteral("unitBuffToggle"))) if (toggle->isVisibleTo(&cell)) {
      ++toggles;
      const QRect rect(toggle->mapTo(&cell, QPoint()), toggle->size());
      if (!cell.rect().contains(rect) || rect.height() < 10) { geometryFailure = QStringLiteral("Buff toggle clipped/too short"); return false; }
    }
    for (auto* text : cell.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs"))) if (text->isVisibleTo(&cell)) {
      ++documents;
      if (text->viewport()->height() < 14 || !cell.rect().contains(QRect(text->mapTo(&cell, QPoint()), text->size()))) {
        geometryFailure = QStringLiteral("Buff browser clipped or viewport below 14px (%1)").arg(text->viewport()->height()); return false;
      }
    }
    if (bars != 3 || toggles != 1 || documents != 1) {
      geometryFailure = QStringLiteral("visible counts: meters %1, toggle %2, Buff document %3").arg(bars).arg(toggles).arg(documents); return false;
    }
    return true;
  };
  cell.focusUnit(target.key); QCoreApplication::processEvents();
  for (int height : {174, 204}) {
    cell.setFixedSize(148, height); QCoreApplication::processEvents();
    if (!verifyVisibleContent() || !cell.property("selectedUnitDead").toBool()) {
      if (geometryFailure.isEmpty()) geometryFailure = QStringLiteral("selectedUnitDead did not describe the selected fallen target");
      dumpDenseFailure(); return fail("148px target card clipped meters/Buff entry or lost death state");
    }
  }
  cell.setFixedSize(148, 174); QCoreApplication::processEvents();
  QTextBrowser* currentBuffs = nullptr; QProgressBar* unknownShield = nullptr;
  for (auto* text : cell.findChildren<QTextBrowser*>(QStringLiteral("unitBuffs"))) if (text->isVisibleTo(&cell)) currentBuffs = text;
  for (auto* bar : cell.findChildren<QProgressBar*>(QStringLiteral("shieldValue"))) if (bar->isVisibleTo(&cell)) unknownShield = bar;
  if (!currentBuffs || currentBuffs->property("sourceOwnCount").toInt() != 1 || currentBuffs->property("sourceEnemyCount").toInt() != 1 ||
      currentBuffs->property("sourceUnknownCount").toInt() != 1 || !unknownShield || !unknownShield->property("diagnosticAvailable").toBool())
    return fail("unknown source/unknown shield was inferred or source groups collapsed");
  const auto qaDirectory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
  if (!qaDirectory.isEmpty()) {
    if (!QDir().mkpath(qaDirectory)) return fail("cannot create requested QA image directory");
    for (int height : {174, 204}) {
      cell.setFixedSize(148, height); QCoreApplication::processEvents();
      if (!cell.grab().save(QDir(qaDirectory).filePath(QStringLiteral("same-cell-%1.png").arg(height))))
        return fail("cannot save requested dense-cell QA image");
    }
    cell.setFixedSize(148, 174); QCoreApplication::processEvents();
  }
  QString diagnosed; int activated = 0;
  QObject::connect(&cell, &ReplayCell::diagnosticActivated, &cell, [&](const QString& key) { diagnosed = key; });
  QObject::connect(&cell, &ReplayCell::unitActivated, &cell, [&](const QString&) { ++activated; });
  QTest::mouseClick(unknownShield, Qt::LeftButton, Qt::NoModifier, QPoint(unknownShield->width() / 2, 3));
  if (diagnosed != target.key || activated != 0) return fail("unknown metric did not open its unit diagnostic route");
  QTest::mouseClick(from, Qt::LeftButton); QCoreApplication::processEvents();
  if (cell.property("visibleFighterKey").toString() != actor.key || cell.relationAnchor(actor.key) != from || cell.relationAnchor(target.key) != to ||
      !verifyVisibleContent() || activated != 0) { dumpDenseFailure(); return fail("unit selection moved anchors, hid meters or opened unrelated detail"); }
  return true;
}

bool verifyRealtimeSyntheticPlayback() {
  QTemporaryFile file; if (!file.open()) return false; file.write(realtimeMultiUnitFixture()); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); page.show(); if (!page.loadFile(file.fileName())) return false;
  QCoreApplication::processEvents(); const auto report = verifyRealtimePlayback(page, true);
  if (!report[QStringLiteral("passed")].toBool()) qCritical().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
  return report[QStringLiteral("passed")].toBool();
}

bool verifyContinuousPlaybackAndSkills() {
  // Synthetic values, two distinct actors and official skill references.
  // Complete actions remain accessible for inspection; continuous playback
  // must advance through their recorded stages before the next action.
  auto root = QJsonDocument::fromJson(battleReplayFixture()).object();
  auto br = root[QStringLiteral("br")].toObject(); auto rounds = br[QStringLiteral("fr")].toArray();
  auto round = rounds[0].toObject(); auto actions = round[QStringLiteral("rl")].toArray();
  auto first = actions[0].toObject(); first[QStringLiteral("si")] = 106986; first[QStringLiteral("ses")] = 1;
  auto firstTargets = first[QStringLiteral("trl")].toArray(); auto firstTarget = firstTargets[0].toObject();
  firstTarget[QStringLiteral("isse")] = true;
  auto firstNumbers = firstTarget[QStringLiteral("tn")].toObject(); firstNumbers[QStringLiteral("att_hps")] = "-20";
  firstTarget[QStringLiteral("tn")] = firstNumbers; firstTargets[0] = firstTarget;
  first[QStringLiteral("trl")] = firstTargets; actions[0] = first;
  auto second = actions[1].toObject(); second[QStringLiteral("cti")] = 1; second[QStringLiteral("si")] = 206853;
  second[QStringLiteral("cn")] = QJsonObject{{"ahp", 180}, {"avg", 60}, {"att_hps", "-10"}, {"att_shps", "-10"},
    {"rl", QJsonArray{QJsonObject{{"t", 3}, {"ti", 0}, {"pn", "tester"}, {"ci", 8}, {"o", 2}, {"changeset", 2}}}}};
  second[QStringLiteral("trl")] = QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7},
    {"isse", true}, {"ishit", true}, {"tn", QJsonObject{{"att_hps", "-6"}, {"ahp", 94}}}}};
  actions[1] = second; round[QStringLiteral("rl")] = actions; rounds[0] = round;
  br[QStringLiteral("fr")] = rounds; root[QStringLiteral("br")] = br;
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); if (!page.loadFile(file.fileName())) return false;
  page.show(); QCoreApplication::processEvents();
  const auto fail = [](const char* reason) { qCritical() << "continuous replay:" << reason; return false; };
  auto* mode = page.findChild<QComboBox*>(QStringLiteral("replayPlaybackMode"));
  auto* micro = page.findChild<QWidget*>(QStringLiteral("replayMicroControls"));
  auto* microToggle = page.findChild<QToolButton*>(QStringLiteral("replayMicroToggle"));
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* timer = page.findChild<QTimer*>(QStringLiteral("replayPlaybackTimer"));
  auto* phaseAnimation = page.findChild<QVariantAnimation*>(QStringLiteral("replayPhaseAnimation"));
  auto* progress = page.findChild<QProgressBar*>(QStringLiteral("replayPhaseProgress"));
  auto* speed = page.findChild<QComboBox*>(QStringLiteral("replaySpeed"));
  auto* animations = page.findChild<QCheckBox*>(QStringLiteral("replayAnimations"));
  auto* title = page.findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
  auto* side = page.findChild<QLabel*>(QStringLiteral("replaySkillSide"));
  auto* perspective = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  auto* arena = page.findChild<QWidget*>(QStringLiteral("replayArena"));
  auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"));
  auto* inspector = page.findChild<QWidget*>(QStringLiteral("replayBoardInspector"));
  auto* details = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  auto* filter = page.findChild<QComboBox*>(QStringLiteral("replayUnitFilter"));
  auto* timeline = page.findChild<QTableView*>(QStringLiteral("replayTimeline"));
  if (!mode || !micro || !microToggle || !play || !timer || !phaseAnimation || !progress || !speed || !animations ||
      !title || !side || !perspective || !arena || !board || !inspector || !details || !filter || !timeline) return fail("missing control");
  if (mode->currentData() != QStringLiteral("action") || mode->findData(QStringLiteral("effect")) < 0 ||
      !micro->isHidden() || microToggle->isChecked() || timer->isActive() || page.property("playbackRunning").toBool() ||
      phaseAnimation->state() != QAbstractAnimation::Stopped) return fail("default playback state");
  if (!board->isAncestorOf(title)) return fail("skill caption must stay on the formation board");
  QList<int> actionSteps; int removal = -1;
  for (int i = 0; i < page.replay().steps.size(); ++i) {
    const auto& step = page.replay().steps[i];
    if (step.raw.toObject().contains(QStringLiteral("si"))) actionSteps.append(i);
    if (step.keyChanges.contains(QStringLiteral("玩家移除"))) removal = i;
  }
  if (actionSteps.size() != 2 || actionSteps.first() <= 1 || removal <= actionSteps.last()) return fail("synthetic fixture layout");
  const auto running = [&] {
    return page.property("playbackRunning").toBool() && phaseAnimation->state() == QAbstractAnimation::Running && !timer->isActive();
  };
  const auto completeAt = [&](int index) {
    return page.currentStep() == index && page.currentMoment() == -1 && replayStateMatches(page, page.replay().steps[index].state);
  };
  QSet<int> completedProgressActions;
  bool validProgress = true; int advancingProgressBoundaries = 0;
  QObject progressObserver;
  QObject::connect(phaseAnimation, &QVariantAnimation::valueChanged, &progressObserver, [&](const QVariant&) {
    if (page.currentPlaybackPhase() < 0) return;
    const auto value = page.property("playbackActionProgress");
    const qreal actionProgress = value.toReal();
    validProgress = validProgress && value.isValid() && actionProgress >= 0 && actionProgress <= 1 &&
      qAbs(progress->value() - qRound(actionProgress * 1000)) <= 1;
    if (phaseAnimation->currentTime() == phaseAnimation->duration() &&
        page.currentPlaybackPhase() + 1 == page.playbackPhaseCount()) {
      validProgress = validProgress && qAbs(actionProgress - 1) < 1e-9;
      completedProgressActions.insert(page.currentStep());
    }
  });
  const auto tick = [&] {
    if (!running() || phaseAnimation->duration() <= 0) return false;
    const int step = page.currentStep(), phase = page.currentPlaybackPhase();
    const auto snapshot = page.displayedState();
    const qreal startingProgress = page.property("playbackActionProgress").toReal();
    phaseAnimation->setCurrentTime(phaseAnimation->duration() - 1);
    const qreal endingProgress = page.property("playbackActionProgress").toReal();
    if (!validProgress || endingProgress + 1e-9 < startingProgress || !replayStateMatches(page, snapshot))
      return fail("animation changed recorded values or action progress moved backward");
    phaseAnimation->setCurrentTime(phaseAnimation->duration());
    QCoreApplication::processEvents();
    if (!validProgress) return fail("action progress was invalid or did not reach one");
    if (page.currentStep() == step && page.currentPlaybackPhase() > phase) {
      if (page.property("playbackActionProgress").toReal() + 1e-9 < endingProgress)
        return fail("action progress reset at a phase boundary");
      ++advancingProgressBoundaries;
    }
    return true;
  };
  const auto until = [&](const std::function<bool()>& condition) {
    for (int count = 0; count < 200; ++count) {
      if (condition()) return true;
      if (!tick()) return false;
    }
    return false;
  };
  const auto button = [&](const QString& text) -> QPushButton* {
    for (auto* candidate : page.findChildren<QPushButton*>()) if (candidate->text() == text) return candidate;
    return nullptr;
  };
  const auto snapshot = previewBattleReplayCatalog();
  if (!snapshot) return fail("official catalog unavailable");
  const auto skills = snapshot->root[QStringLiteral("skills")].toObject();
  const auto firstName = skills[QStringLiteral("106986")].toObject()[QStringLiteral("name")].toString();
  const auto secondName = skills[QStringLiteral("206853")].toObject()[QStringLiteral("name")].toString();
  if (firstName.isEmpty() || secondName.isEmpty()) return fail("official fixture skills unnamed");
  const auto& firstStep = page.replay().steps[actionSteps[0]];
  const auto& secondStep = page.replay().steps[actionSteps[1]];
  const auto firstHeading = BattleReplay::fighterName(firstStep.state.value(firstStep.actor)) + QStringLiteral(" · 普攻技 · ") + firstName;
  const auto secondHeading = BattleReplay::fighterName(secondStep.state.value(secondStep.actor)) + QStringLiteral(" · 超杀技 · ") + secondName;

  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentStep() != actionSteps[0] || page.playbackPhaseKind() != QStringLiteral("prepare") ||
      page.currentPlaybackPhase() != 0 || !replayStateMatches(page, page.replay().steps[actionSteps[0]].moments.first().state))
    return fail("play must begin before action results");
  if (title->property("skillId").toString() != QStringLiteral("106986") || title->accessibleName() != firstHeading ||
      !title->text().contains(QStringLiteral("skill:106986")) || side->text() != QStringLiteral("我方施放")) return fail("first skill heading");
  phaseAnimation->setCurrentTime(phaseAnimation->duration() / 3);
  const int pausedPhase = page.currentPlaybackPhase(), pausedTime = phaseAnimation->currentTime(), pausedProgress = progress->value();
  if (pausedProgress <= 0 || pausedProgress >= 1000) return fail("phase progress not continuous");
  if (!QMetaObject::invokeMethod(title, "linkActivated", Q_ARG(QString, QStringLiteral("skill:106986"))) ||
      page.property("playbackRunning").toBool() || phaseAnimation->state() != QAbstractAnimation::Paused ||
      !inspector->isVisible() || !details->toPlainText().contains(firstName) || details->toPlainText().contains(QStringLiteral("106986")))
    return fail("skill detail must pause current phase");
  if (!activateReplayTextAnchor(details, QStringLiteral("skill-raw:106986")) || !details->toPlainText().contains(QStringLiteral("106986")) ||
      !activateReplayTextAnchor(details, QStringLiteral("skill:106986")) || details->toPlainText().contains(QStringLiteral("106986")))
    return fail("skill raw record or return link lost");
  if (page.currentPlaybackPhase() != pausedPhase || phaseAnimation->currentTime() != pausedTime || progress->value() != pausedProgress)
    return fail("inspection reset phase position");
  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentPlaybackPhase() != pausedPhase || phaseAnimation->currentTime() != pausedTime)
    return fail("resume reset phase progress");
  if (!until([&] { return page.currentStep() == actionSteps[1]; }) || page.playbackPhaseKind() != QStringLiteral("prepare"))
    return fail("stages did not advance to next action");
  if (title->property("skillId").toString() != QStringLiteral("206853") || title->accessibleName() != secondHeading ||
      side->text() != QStringLiteral("敌方施放")) return fail("second actor skill heading");
  const auto secondPhase = page.currentPlaybackPhase();
  perspective->setCurrentIndex(perspective->findData(1));
  if (side->text() != QStringLiteral("我方施放") || title->accessibleName() != secondHeading ||
      page.currentPlaybackPhase() != secondPhase || !running()) return fail("skill perspective changed stage");
  perspective->setCurrentIndex(perspective->findData(0));
  if (!until([&] { return page.currentStep() == removal; }) || !replayStateMatches(page, page.replay().steps[removal].state))
    return fail("formation event skipped");
  const int last = int(page.replay().steps.size()) - 1;
  if (!until([&] { return !page.property("playbackRunning").toBool(); }) || page.currentStep() != last ||
      !replayStateMatches(page, page.replay().steps[last].state) || timer->isActive()) return fail("end stop");
  if (!validProgress || advancingProgressBoundaries < 1 || !completedProgressActions.contains(actionSteps[0]) ||
      !completedProgressActions.contains(actionSteps[1]) || !completedProgressActions.contains(last))
    return fail("whole-action progress did not cover each completed action");

  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentStep() != actionSteps[0] || page.playbackPhaseKind() != QStringLiteral("prepare")) return fail("replay from end");
  auto* sourceCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* sourcePages = sourceCell ? sourceCell->findChild<QStackedWidget*>(QStringLiteral("cellUnits")) : nullptr;
  auto* buffToggle = sourcePages && sourcePages->currentWidget()
    ? sourcePages->currentWidget()->findChild<QToolButton*>(QStringLiteral("unitBuffToggle")) : nullptr;
  if (!buffToggle || buffToggle->isChecked()) return fail("fresh playback missing collapsed Buff entry");
  const auto beforeExpand = page.displayedState(); const int expandPhase = page.currentPlaybackPhase(), expandTime = phaseAnimation->currentTime();
  QTest::mouseClick(buffToggle, Qt::LeftButton);
  if (page.property("playbackRunning").toBool() || phaseAnimation->state() != QAbstractAnimation::Paused ||
      page.currentPlaybackPhase() != expandPhase || phaseAnimation->currentTime() != expandTime ||
      !replayStateMatches(page, beforeExpand) || !buffToggle->isChecked() || inspector->isVisible())
    return fail("in-cell Buff expansion did not pause in place or opened a separate inspector");
  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentPlaybackPhase() != expandPhase || !replayStateMatches(page, beforeExpand))
    return fail("resume after Buff expansion skipped record");
  phaseAnimation->setCurrentTime(phaseAnimation->duration() / 4);
  const auto pauseTime = phaseAnimation->currentTime(), pauseStage = page.currentPlaybackPhase();
  const auto pausedSnapshot = page.displayedState();
  const qreal pausedActionProgress = page.property("playbackActionProgress").toReal();
  const int pausedDuration = phaseAnimation->duration();
  QTest::mouseClick(play, Qt::LeftButton);
  if (running() || page.property("playbackRunning").toBool() || phaseAnimation->state() != QAbstractAnimation::Paused ||
      phaseAnimation->currentTime() != pauseTime || page.currentPlaybackPhase() != pauseStage) return fail("pause changed stage");
  speed->setCurrentIndex(speed->findData(900));
  if (phaseAnimation->state() != QAbstractAnimation::Paused || page.property("playbackRunning").toBool() || timer->isActive() ||
      phaseAnimation->duration() >= pausedDuration || page.currentPlaybackPhase() != pauseStage ||
      qAbs(page.property("playbackActionProgress").toReal() - pausedActionProgress) > 1e-9 ||
      qAbs(qreal(phaseAnimation->currentTime()) / phaseAnimation->duration() - qreal(pauseTime) / pausedDuration) > 1.0 / phaseAnimation->duration() ||
      !replayStateMatches(page, pausedSnapshot)) return fail("speed change while paused moved data, progress or playback state");
  const int resumedTime = phaseAnimation->currentTime();
  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentPlaybackPhase() != pauseStage || phaseAnimation->currentTime() != resumedTime ||
      !replayStateMatches(page, pausedSnapshot)) return fail("resume after paused speed change restarted stage");
  QTest::mouseClick(play, Qt::LeftButton); speed->setCurrentIndex(speed->findData(1800));
  if (phaseAnimation->state() != QAbstractAnimation::Paused || !replayStateMatches(page, pausedSnapshot))
    return fail("restoring speed resumed or changed paused state");
  microToggle->setChecked(true); if (micro->isHidden()) return fail("micro details cannot expand");
  const auto& firstEffects = page.replay().steps[actionSteps[0]].effects;
  int detailIndex = -1;
  for (int i = 0; i < firstEffects.size(); ++i) if (firstEffects[i].changed && firstEffects[i].moment > 0) { detailIndex = i; break; }
  if (detailIndex < 0) return fail("no synthetic detail record");
  page.selectEffect(detailIndex);
  const int detailMoment = firstEffects[detailIndex].moment;
  if (page.currentMoment() != detailMoment || page.currentPlaybackPhase() != -1 ||
      phaseAnimation->state() != QAbstractAnimation::Stopped || timer->isActive()) return fail("detail selection left old playback active");
  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentStep() != actionSteps[0] ||
      page.property("playbackFirstMoment").toInt() > detailMoment || page.property("playbackLastMoment").toInt() < detailMoment)
    return fail("resume did not use selected moment's stage");
  phaseAnimation->setCurrentTime(phaseAnimation->duration() / 3);
  const auto oldDuration = phaseAnimation->duration(), oldProgress = progress->value(), oldStage = page.currentPlaybackPhase();
  const auto speedSnapshot = page.displayedState();
  speed->setCurrentIndex(speed->findData(900));
  if (phaseAnimation->duration() >= oldDuration || qAbs(progress->value() - oldProgress) > 1 ||
      page.currentPlaybackPhase() != oldStage || !running() || !replayStateMatches(page, speedSnapshot))
    return fail("speed restarted stage or changed data");
  animations->setChecked(false);
  for (const auto* animation : page.findChildren<QVariantAnimation*>())
    if (animation != phaseAnimation && animation->state() != QAbstractAnimation::Stopped) return fail("disabled visual animation still running");
  if (!running() || page.currentPlaybackPhase() != oldStage || !replayStateMatches(page, speedSnapshot))
    return fail("animation toggle stopped presentation clock or changed data");
  if (!until([&] { return page.currentStep() == actionSteps[1]; })) return fail("resume after detail failed");
  auto* previous = button(QStringLiteral("上一行动"));
  if (!previous) return fail("previous action missing");
  QTest::mouseClick(previous, Qt::LeftButton);
  if (page.property("playbackRunning").toBool() || phaseAnimation->state() != QAbstractAnimation::Stopped ||
      page.currentPlaybackPhase() != -1 || !completeAt(actionSteps[0])) return fail("backward seek left phase active");
  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.playbackPhaseKind() != QStringLiteral("prepare") ||
      !until([&] { return page.currentStep() == actionSteps[1]; })) return fail("resume after backward seek");
  auto* restart = button(QStringLiteral("回到开局"));
  if (!restart) return fail("restart missing");
  QTest::mouseClick(restart, Qt::LeftButton);
  if (page.property("playbackRunning").toBool() || phaseAnimation->state() != QAbstractAnimation::Stopped || !completeAt(0))
    return fail("restart changed opening or kept phase alive");

  const auto reserve = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("8"));
  filter->setCurrentIndex(filter->findData(reserve));
  if (timeline->model()->rowCount() != 1) return fail("reserve filter fixture");
  QTest::mouseClick(play, Qt::LeftButton);
  if (!running() || page.currentStep() != actionSteps[1] ||
      !until([&] { return !page.property("playbackRunning").toBool(); }) || page.currentStep() != actionSteps[1])
    return fail("filtered playback left matching action");
  filter->setCurrentIndex(0);
  bool exactRaw = false;
  for (const auto* raw : page.findChildren<QPlainTextEdit*>()) if (raw->toPlainText() == QString::fromUtf8(bytes)) exactRaw = true;
  if (!exactRaw || page.replay().original != bytes) return fail("raw log changed");
  page.hide(); return true;
}

bool verifyRecordedPlaybackStages() {
  // Three objects have distinct raw changeset IDs but one ordinal before/after
  // group. Values and flags below are adversarial synthetic records; displaying
  // their numbers does not infer server damage formulas from judgement flags.
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object();
  auto br = root[QStringLiteral("br")].toObject(); auto initial = br[QStringLiteral("or")].toObject();
  auto teams = initial[QStringLiteral("team")].toArray(); auto enemyTeam = teams[1].toObject();
  auto players = enemyTeam[QStringLiteral("players")].toArray(); auto player = players[0].toObject();
  auto fighters = player[QStringLiteral("cts")].toArray();
  fighters.append(QJsonObject{{"id", 8}, {"name", "synthetic second target"}, {"x", 1}, {"y", 0},
    {"hp", 300}, {"maxHp", 300}, {"shp", 90}, {"vigour", 70}});
  player[QStringLiteral("cts")] = fighters; players[0] = player; enemyTeam[QStringLiteral("players")] = players;
  teams[1] = enemyTeam; initial[QStringLiteral("team")] = teams; br[QStringLiteral("or")] = initial;
  const QJsonObject action{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"csn", 1}, {"ses", 1}, {"si", 106853},
    {"cn", QJsonObject{{"bb_hps", "99_-5_9"}, {"bb_shps", "99_10_9"}, {"bd_bfs", "1_101_9"},
      {"att_hps", "-10"}, {"att_shps", "-7"}, {"att_vgs", "5"}, {"ab_hps", "99_4_30"}, {"ab_shps", "99_-20_30"},
      {"ad_bfs", "0_101_30"}, {"ahp", 89}, {"avg", 105}}},
    {"trl", QJsonArray{
      QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true}, {"isbl", true},
        {"tn", QJsonObject{{"bb_hps", "99_-10_1"}, {"bb_shps", "99_-3_1"}, {"bd_bfs", "1_100_1"},
          {"att_hps", "-20,-30"}, {"att_shps", "-11"}, {"att_vgs", "10"}, {"ab_hps", "99_5_8"}, {"ab_shps", "99_1_8"},
          {"ad_bfs", "0_100_8"}, {"ahp", 145}, {"avg", 70}}}},
      QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 8}, {"isse", true}, {"ishit", true}, {"isIm", true},
        {"tn", QJsonObject{{"bb_hps", "99_-15_12"}, {"bb_shps", "99_-5_12"}, {"bd_bfs", "1_101_12"},
          {"att_hps", "-40,-10"}, {"att_shps", "-9"}, {"att_vgs", "-20"}, {"ab_hps", "99_7_72"}, {"ab_shps", "99_2_72"},
          {"ad_bfs", "0_101_72"}, {"ahp", 242}, {"avg", 50}}}}
    }}, {"prl", QJsonArray{}}};
  br[QStringLiteral("fr")] = QJsonArray{QJsonObject{{"rl", QJsonArray{action}}}}; root[QStringLiteral("br")] = br;
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false; page.seek(2);
  const auto fail = [](const char* reason) { qCritical() << "recorded playback stages:" << reason; return false; };
  auto* animation = page.findChild<QVariantAnimation*>(QStringLiteral("replayPhaseAnimation"));
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  auto* title = page.findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
  auto* side = page.findChild<QLabel*>(QStringLiteral("replaySkillSide"));
  auto* perspective = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  auto* sourceCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* targetCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
  auto* secondCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell12"));
  if (!animation || !play || !links || !title || !side || !perspective || !sourceCell || !targetCell || !secondCell)
    return fail("missing control");
  const auto source = BattleReplay::fighterKey(0, "tester", "7");
  const auto target = BattleReplay::fighterKey(1, "tester", "7");
  const auto target2 = BattleReplay::fighterKey(1, "tester", "8");
  const auto values = [&](qint64 ahp, qint64 ashp, qint64 bhp, qint64 bshp, qint64 chp, qint64 cshp) {
    const auto& state = page.displayedState();
    return state[source].hp == ahp && state[source].shield == ashp && state[target].hp == bhp &&
      state[target].shield == bshp && state[target2].hp == chp && state[target2].shield == cshp;
  };
  const auto noJudgements = [&] {
    return !targetCell->findChild<QLabel*>(QStringLiteral("unitResultBadges")) &&
      !secondCell->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
  };
  bool prepare = false, beforeValues = false, beforeBuffs = false, cast = false, hit = false;
  bool afterValues = false, afterBuffs = false;
  const int count = page.playbackPhaseCount();
  if (count < 5) return fail("stages missing");
  for (int i = 0; i < count; ++i) {
    page.seekPlaybackPhase(i);
    const auto kind = page.playbackPhaseKind();
    if (page.currentPlaybackPhase() != i || page.property("playbackRunning").toBool()) return fail("manual stage selection");
    const int lastMoment = page.property("playbackLastMoment").toInt();
    if (lastMoment >= page.replay().steps[2].moments.size() ||
        (lastMoment >= 0 && !replayStateMatches(page, page.replay().steps[2].moments[lastMoment].state))) return fail("stage state not recorded snapshot");
    const auto part = lastMoment >= 0 ? page.replay().steps[2].moments[lastMoment].part : QString();
    if (kind == QStringLiteral("prepare")) {
      prepare = true;
      if (!values(100, 50, 200, 70, 300, 90) || !noJudgements() || page.displayedState()[source].buffs.contains("101"))
        return fail("prepare exposed future values, Buffs or immunity");
    } else if (kind == QStringLiteral("before")) {
      if (!noJudgements()) return fail("before exposed future hit judgement");
      if (part == QStringLiteral("values")) {
        beforeValues = true;
        if (!values(95, 60, 190, 67, 285, 85) || page.displayedState()[source].buffs.contains("101") ||
            page.displayedState()[target].buffs.contains("100") || page.displayedState()[target2].buffs.contains("101"))
          return fail("before values split across targets or prematurely applied ordinary Buffs");
      } else if (part == QStringLiteral("power-buff")) {
        beforeBuffs = true;
        if (!beforeValues || !values(95, 60, 190, 67, 285, 85) ||
            !page.displayedState()[source].buffs.contains("101") || !page.displayedState()[target].buffs.contains("100") ||
            !page.displayedState()[target2].buffs.contains("101")) return fail("ordinary Buff callback not separate after values");
      }
    } else if (kind == QStringLiteral("cast")) {
      cast = true;
      if (!values(95, 60, 190, 67, 285, 85) || !noJudgements() || !sourceCell->property("actorEmphasis").toBool() ||
          links->property("connectionCount").toInt() != 2) return fail("cast advanced hit state or lost directed targets");
    } else if (kind == QStringLiteral("attack")) {
      hit = true;
      const auto* sourceRoles = sourceCell->findChild<QLabel*>(QStringLiteral("unitRoles"));
      if (sourceRoles && sourceRoles->text().contains(QStringLiteral("目标")))
        return fail("caster resource costs were labelled as a self attack");
      auto* firstBadge = targetCell->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
      auto* secondBadge = secondCell->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
      if (!values(85, 53, 140, 56, 235, 76) || !firstBadge || !secondBadge ||
          !firstBadge->accessibleName().contains(QStringLiteral("格挡")) || !secondBadge->accessibleName().contains(QStringLiteral("免疫")))
        return fail("hit flags and values did not appear together");
      for (const auto* cell : {targetCell, secondCell}) {
        const auto* hp = cell->findChild<QProgressBar*>(QStringLiteral("hpBar"));
        if (!hp || !hp->accessibleName().contains(QStringLiteral("-50"))) return fail("hit omitted recorded combined change");
      }
      if (page.displayedState()[source].power != 105 || page.displayedState()[target].power != 70 ||
          page.displayedState()[target2].power != 50) return fail("hit power values not simultaneous");
    } else if (kind == QStringLiteral("after")) {
      if (part == QStringLiteral("values")) {
        afterValues = true;
        if (!values(89, 33, 145, 57, 242, 78) || !page.displayedState()[source].buffs.contains("101") ||
            !page.displayedState()[target].buffs.contains("100") || !page.displayedState()[target2].buffs.contains("101"))
          return fail("after values split across targets or prematurely removed Buffs");
      } else if (part == QStringLiteral("power-buff")) {
        afterBuffs = true;
        if (!afterValues || !values(89, 33, 145, 57, 242, 78) || page.displayedState()[source].buffs.contains("101") ||
            page.displayedState()[target].buffs.contains("100") || page.displayedState()[target2].buffs.contains("101"))
          return fail("after Buff callback not separate after values");
      }
    }
  }
  if (!prepare || !beforeValues || !beforeBuffs || !cast || !hit || !afterValues || !afterBuffs || page.replay().original != bytes)
    return fail("verified stage coverage or raw preservation");

  // Official execution type 2 suppresses the attack command even when raw
  // target flags exist. Preserve those flags as records, without animating a
  // skill cast, hit, counter, or directed attack that was never executed.
  auto disabledAction = action; disabledAction[QStringLiteral("ses")] = -1;
  auto disabledRoot = root; auto disabledBr = br;
  disabledBr[QStringLiteral("fr")] = QJsonArray{QJsonObject{{"rl", QJsonArray{disabledAction}}}};
  disabledRoot[QStringLiteral("br")] = disabledBr;
  const auto disabledBytes = QJsonDocument(disabledRoot).toJson();
  QTemporaryFile disabledFile; if (!disabledFile.open()) return false; disabledFile.write(disabledBytes); disabledFile.flush();
  if (!page.loadFile(disabledFile.fileName())) return fail("disabled execution fixture import");
  page.seek(2); bool disabledFlags = false;
  for (int i = 0; i < page.playbackPhaseCount(); ++i) {
    page.seekPlaybackPhase(i);
    const auto kind = page.playbackPhaseKind();
    if (kind == QStringLiteral("cast") || kind == QStringLiteral("attack") ||
        kind == QStringLiteral("counter-cast") || kind == QStringLiteral("counter")) return fail("disabled execution fabricated attack stage");
    if (kind != QStringLiteral("settle")) continue;
    const auto* firstBadge = targetCell->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
    const auto* secondBadge = secondCell->findChild<QLabel*>(QStringLiteral("unitResultBadges"));
    if (firstBadge && secondBadge && firstBadge->accessibleName().contains(QStringLiteral("格挡")) &&
        secondBadge->accessibleName().contains(QStringLiteral("免疫"))) {
      disabledFlags = true;
      if (links->property("connectionCount").toInt() != 0) return fail("disabled target record drew attack arrow");
      for (const auto* cell : {targetCell, secondCell})
        for (const auto* overlay : cell->findChildren<QWidget*>(QStringLiteral("unitOutcomeOverlay"))) {
          const auto kinds = overlay->property("effectKinds").toStringList();
          for (const auto& kind : {"block", "immune", "critical", "miss", "invalid", "counter-marker"})
            if (kinds.contains(QLatin1String(kind))) return fail("disabled target record animated an unexecuted hit");
        }
    }
  }
  if (!disabledFlags || page.replay().original != disabledBytes) return fail("disabled target flags or original log lost");

  // A single close target enters the separately recorded counter branch.
  // The wind-up retains the post-attack state, and the counter stage changes
  // both units together with the countering unit as the arrow source.
  auto counterRoot = QJsonDocument::fromJson(battleReplayStateFixture()).object();
  auto counterBr = counterRoot[QStringLiteral("br")].toObject();
  auto counterInitial = counterBr[QStringLiteral("or")].toObject();
  auto counterTeams = counterInitial[QStringLiteral("team")].toArray();
  auto counterTeam = counterTeams[1].toObject(); auto counterPlayers = counterTeam[QStringLiteral("players")].toArray();
  auto counterPlayer = counterPlayers[0].toObject(); auto counterFighters = counterPlayer[QStringLiteral("cts")].toArray();
  auto counterFighter = counterFighters[0].toObject(); counterFighter[QStringLiteral("nskill")] = 106853;
  counterFighters[0] = counterFighter; counterPlayer[QStringLiteral("cts")] = counterFighters;
  counterPlayers[0] = counterPlayer; counterTeam[QStringLiteral("players")] = counterPlayers;
  counterTeams[1] = counterTeam; counterInitial[QStringLiteral("team")] = counterTeams;
  counterBr[QStringLiteral("or")] = counterInitial;
  auto counterRounds = counterBr[QStringLiteral("fr")].toArray(); auto counterRound = counterRounds[0].toObject();
  auto counterActions = counterRound[QStringLiteral("rl")].toArray(); auto counterAction = counterActions[0].toObject();
  counterAction[QStringLiteral("fbs")] = false; counterActions[0] = counterAction;
  counterRound[QStringLiteral("rl")] = counterActions; counterRound[QStringLiteral("pan")] = QStringLiteral("tester");
  counterRound[QStringLiteral("pbn")] = QStringLiteral("tester"); counterRounds[0] = counterRound;
  counterBr[QStringLiteral("fr")] = counterRounds; counterRoot[QStringLiteral("br")] = counterBr;
  auto counterBytes = QJsonDocument(counterRoot).toJson(); counterBytes.replace("999999999", "106986");
  QTemporaryFile counterFile; if (!counterFile.open()) return false; counterFile.write(counterBytes); counterFile.flush();
  if (!page.loadFile(counterFile.fileName())) return fail("counter fixture import");
  page.seek(2); bool counterCast = false, counter = false;
  const auto* originalQueue = page.findChild<QLabel*>(QStringLiteral("replayQueue0"));
  const auto* counterQueue = page.findChild<QLabel*>(QStringLiteral("replayQueue1"));
  if (!originalQueue || !counterQueue || !originalQueue->accessibleName().contains(QStringLiteral("队列外行动")))
    return fail("original action queue status missing");
  for (int i = 0; i < page.playbackPhaseCount(); ++i) {
    page.seekPlaybackPhase(i);
    const auto kind = page.playbackPhaseKind();
    if (kind != QStringLiteral("counter-cast") && kind != QStringLiteral("counter")) continue;
    if (counterQueue->accessibleName().contains(QStringLiteral("本次为队列外行动")) ||
        counterQueue->accessibleName().contains(QStringLiteral("本次从队列出手")))
      return fail("counter borrowed original action's fbs queue status");
    const auto& state = page.displayedState();
    if (kind == QStringLiteral("counter-cast")) {
      counterCast = true;
      if (state[source].hp != 85 || state[target].hp != 50) return fail("counter lead exposed future damage");
    } else {
      counter = true;
      if (state[source].hp != 77 || state[target].hp != 48 || state[source].shield != 47 || state[target].shield != 54)
        return fail("counter values not same recorded stage");
    }
    if (title->property("skillId").toString() != QStringLiteral("106853") || !title->text().contains(QStringLiteral("skill:106853")) ||
        !title->accessibleName().contains(QStringLiteral("天誓启明")) || side->text() != QStringLiteral("敌方反击") ||
        !targetCell->property("actorEmphasis").toBool() || sourceCell->property("actorEmphasis").toBool() ||
        !sourceCell->toPlainText().contains(QStringLiteral("目标")) || links->property("connectionCount").toInt() < 1 ||
        (kind == QStringLiteral("counter-cast") && links->property("connectionCount").toInt() != 1))
      return fail("counter direction, actor, target or title");
  }
  if (!counterCast || !counter) return fail("counter stage omitted");
  // Direct record inspection keeps counter identity and supports perspective
  // changes independently of the continuous phase clock.
  int counterIndex = -1;
  for (int i = 0; i < page.replay().steps[2].effects.size(); ++i)
    if (page.replay().steps[2].effects[i].cause == QStringLiteral("counter")) { counterIndex = i; break; }
  if (counterIndex < 0) return fail("counter record missing");
  page.selectEffect(counterIndex);
  perspective->setCurrentIndex(perspective->findData(1));
  if (side->text() != QStringLiteral("我方反击") || title->property("skillId").toString() != QStringLiteral("106853") ||
      page.replay().original != counterBytes) return fail("counter perspective or raw log changed");
  if (!QMetaObject::invokeMethod(title, "linkActivated", Q_ARG(QString, QStringLiteral("skill:106853"))))
    return fail("counter skill link cannot open");
  const auto* counterDetails = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  if (!counterDetails || !counterDetails->toPlainText().contains(QStringLiteral("天誓启明"))) return fail("counter details opened original attack skill");
  return true;
}

bool verifyMeterTransitionsKeepRecordedNumbers() {
  BattleReplay::Fighter fighter; fighter.key = QStringLiteral("synthetic-animated-unit"); fighter.name = QStringLiteral("合成进度过渡");
  fighter.hp = 50; fighter.maxHp = 100; fighter.shield = 40; fighter.power = 80; fighter.maxPower = 100;
  ReplayUnitOverview cue; cue.animate = true; cue.transitionMs = 350;
  cue.hpBefore = 150; cue.maxHpBefore = 200; cue.powerBefore = 20; cue.maxPowerBefore = 100;
  ReplayCell cell(0); cell.display(1, {fighter}, {}, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  auto* hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar"));
  auto* power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  auto* shield = cell.findChild<QProgressBar*>(QStringLiteral("shieldValue"));
  if (!hp || !power || !shield || hp->text() != QStringLiteral("50/100") || power->text() != QStringLiteral("80/100") ||
      shield->text() != QStringLiteral("护盾 40") || hp->value() != 7500 || power->value() != 2000) return false;
  const auto motions = cell.findChildren<QVariantAnimation*>(QStringLiteral("replayMeterAnimation"));
  if (motions.size() != 2) return false;
  for (auto* motion : motions) {
    if (motion->state() != QAbstractAnimation::Running || motion->duration() != 350) return false;
    motion->setCurrentTime(motion->duration() / 2);
    if (hp->text() != QStringLiteral("50/100") || power->text() != QStringLiteral("80/100")) return false;
    motion->setCurrentTime(motion->duration());
  }
  if (hp->value() != 5000 || power->value() != 8000 || fighter.hp != 50 || fighter.power != 80) return false;
  cue.animate = false; cell.display(1, {fighter}, {}, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  if (!cell.findChildren<QVariantAnimation*>(QStringLiteral("replayMeterAnimation")).isEmpty()) return false;
  hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar")); power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  if (!hp || !power || hp->value() != 5000 || power->value() != 8000 ||
      hp->text() != QStringLiteral("50/100") || power->text() != QStringLiteral("80/100")) return false;
  // Continuous playback owns one phase clock. Driven cards must neither run
  // their own timer nor invent numerical values between logged snapshots.
  cell.setPlaybackDriven(true); cue.animate = true; cue.hpBefore = 100; cue.maxHpBefore = 100;
  cell.display(1, {fighter}, {}, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  hp = cell.findChild<QProgressBar*>(QStringLiteral("hpBar")); power = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  if (!hp || !power || !cell.findChildren<QVariantAnimation*>(QStringLiteral("replayMeterAnimation")).isEmpty()) return false;
  cell.setPlaybackProgress(0);
  if (hp->value() != 10000 || hp->text() != QStringLiteral("50/100") || power->text() != QStringLiteral("80/100")) return false;
  cell.setPlaybackProgress(0.5);
  if (hp->value() != 5625 || power->value() != 7250 || hp->text() != QStringLiteral("50/100") || power->text() != QStringLiteral("80/100")) return false;
  cell.setPlaybackDriven(false);
  return !cell.property("playbackDriven").toBool() && hp->value() == 5000 && power->value() == 8000 &&
    hp->text() == QStringLiteral("50/100") && power->text() == QStringLiteral("80/100");
}

bool verifySpecialRecordDisplayAudit() {
  // Synthetic adversarial t=2: the official bubble uses the record's old/new
  // pair, while state replacement starts from a different preceding snapshot.
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object();
  auto br = root[QStringLiteral("br")].toObject();
  const QJsonObject special{{"t", 2}, {"o", 2}, {"changeset", 1},
    {"old_curv", 200}, {"new_curv", 90}, {"old_maxv", 200}, {"new_maxv", 150}};
  const QJsonObject action{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"csn", 1}, {"ses", 1}, {"si", 106986},
    {"cn", QJsonObject{{"rl", QJsonArray{special}}}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
  br[QStringLiteral("fr")] = QJsonArray{QJsonObject{{"rl", QJsonArray{action}}}}; root[QStringLiteral("br")] = br;
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false; page.seek(2);
  const auto fail = [](const char* reason) { qCritical() << "special record display:" << reason; return false; };
  const auto source = BattleReplay::fighterKey(0, "tester", "7");
  const auto& step = page.replay().steps[2]; int lifeIndex = -1; bool maximum = false;
  for (int i = 0; i < step.effects.size(); ++i) {
    const auto& e = step.effects[i];
    if (e.kind != QStringLiteral("value") || e.cause != QStringLiteral("record")) continue;
    if (e.target != source || e.raw != special) return fail("t=2 raw fields or recipient lost");
    if (e.metric == QStringLiteral("生命")) {
      lifeIndex = i;
      if (e.before != 100 || e.after != 90 || e.delta != -10 || e.recordedBefore != 200 ||
          e.recordedAfter != 90 || e.recordedDelta != -110) return fail("t=2 recorded display delta replaced state delta");
    } else if (e.metric == QStringLiteral("生命上限")) {
      maximum = true;
      if (e.before != 100 || e.after != 150 || e.delta != 50 || e.recordedBefore != 200 ||
          e.recordedAfter != 150 || e.recordedDelta != -50) return fail("t=2 maximum display delta replaced state delta");
    }
  }
  if (lifeIndex < 0 || !maximum || step.state[source].hp != 90 || step.state[source].maxHp != 150)
    return fail("t=2 replacement state missing");
  const auto audit = auditBattleReplayEffects(page.replay());
  if (audit.errors()) { qCritical() << audit.report(); return fail("valid distinct record/state differences rejected"); }
  page.selectEffect(lifeIndex);
  auto* cell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* hp = cell ? cell->findChild<QProgressBar*>(QStringLiteral("hpBar")) : nullptr;
  if (!hp || hp->text() != QStringLiteral("90/150") || !hp->accessibleName().contains(QStringLiteral("-110")))
    return fail("bar omitted original difference or subtracted it again");
  auto corrupted = page.replay();
  for (auto& e : corrupted.steps[2].effects)
    if (e.kind == QStringLiteral("value") && e.cause == QStringLiteral("record") && e.metric == QStringLiteral("生命")) e.recordedDelta = -10;
  for (auto& moment : corrupted.steps[2].moments) for (auto& e : moment.effects)
    if (e.kind == QStringLiteral("value") && e.cause == QStringLiteral("record") && e.metric == QStringLiteral("生命")) e.recordedDelta = -10;
  if (auditBattleReplayEffects(corrupted).beforeAfterErrors == 0) return fail("audit missed overwritten raw display difference");
  return page.replay().original == bytes;
}

bool verifyOfficialActionBranchesAndQueues() {
  // All combat values are synthetic. Skill/Buff IDs below reference the
  // bundled official combo and player-buffer execution definitions.
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object();
  auto br = root[QStringLiteral("br")].toObject(); auto initial = br[QStringLiteral("or")].toObject();
  auto teams = initial[QStringLiteral("team")].toArray(); auto own = teams[0].toObject();
  auto players = own[QStringLiteral("players")].toArray(); auto owner = players[0].toObject();
  auto fighters = owner[QStringLiteral("cts")].toArray(); auto fighter = fighters[0].toObject();
  fighter[QStringLiteral("bfs")] = QStringLiteral("99,99,2793"); fighters[0] = fighter;
  fighters.append(QJsonObject{{"id", 8}, {"name", "synthetic queue reserve"}, {"x", 1}, {"y", 0},
    {"hp", 80}, {"maxHp", 80}, {"vigour", 30}});
  owner[QStringLiteral("cts")] = fighters; players[0] = owner;
  players.append(QJsonObject{{"name", "other-player"}, {"cts", QJsonArray{
    QJsonObject{{"id", 7}, {"name", "synthetic unrelated same-ID fighter"}, {"x", 0}, {"y", 0}, {"hp", 90}, {"maxHp", 90}},
    QJsonObject{{"id", 8}, {"name", "synthetic unrelated reserve"}, {"x", 1}, {"y", 0}, {"hp", 90}, {"maxHp", 90}}}}});
  own[QStringLiteral("players")] = players;
  teams[0] = own; initial[QStringLiteral("team")] = teams; br[QStringLiteral("or")] = initial;
  const QJsonObject combo{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"csn", 1}, {"pr", 3}, {"ses", 1},
    {"si", 201597}, {"fbs", false}, {"seqa", QJsonArray{8, 7}}, {"seqb", QJsonArray{7}}, {"cn", QJsonObject{}},
    {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true},
      {"ishit", true}, {"tn", QJsonObject{{"att_hps", "-5"}, {"ahp", 195}}}}}}, {"prl", QJsonArray{}}};
  const QJsonObject simple{{"cti", 1}, {"cpn", "tester"}, {"orrt", 2}, {"epbi", 200047}, {"csn", 1}, {"pr", 5},
    {"si", 106986}, {"cn", QJsonObject{{"att_hps", "-999"}}},
    {"trl", QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true},
      {"isIm", true}, {"tn", QJsonObject{{"bb_hps", "99_-5_1"}, {"att_hps", "-500"}, {"ab_hps", "99_2_2"}, {"ahp", 97},
        {"rl", QJsonArray{QJsonObject{{"t", 4}, {"o", 2}, {"changeset", 2}, {"pbid", 77},
          {"prob", 2}, {"proa", 9}, {"vc", 4}, {"mp", 10}, {"isse", false}}}}}}}}},
    {"prl", QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"isse", false},
      {"tn", QJsonObject{{"ad_bfs", "1_99_2"}}}}}}};
  const QJsonObject teamSkill{{"cti", 0}, {"cpn", "tester"}, {"orrt", 2}, {"epbi", 210001}, {"csn", 2}, {"pr", 4},
    {"si", 206853}, {"seqa", QJsonValue(QJsonValue::Null)}, {"seqb", QJsonArray{}},
    {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true},
      {"tn", QJsonObject{{"att_hps", "-7"}, {"ahp", 188}}}}}}, {"prl", QJsonArray{}}};
  const QJsonObject disabled{{"cti", 1}, {"cpn", "tester"}, {"cci", 7}, {"csn", 2}, {"pr", 6}, {"ses", -2},
    {"si", 106986}, {"isor", true}, {"fbs", false}, {"seqa", QJsonArray{}}, {"seqb", QJsonArray{7}}, {"cn", QJsonObject{}},
    {"trl", QJsonArray{QJsonObject{{"tti", 0}, {"tpn", "tester"}, {"tci", 7}, {"isse", true}, {"ishit", true},
      {"tn", QJsonObject{{"att_hps", "-300"}, {"ahp", 97}}}}}}, {"prl", QJsonArray{}}};
  br[QStringLiteral("fr")] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"},
    {"rl", QJsonArray{combo, simple, teamSkill, disabled}}}}; root[QStringLiteral("br")] = br;
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  const auto fail = [](const char* reason) { qCritical() << "official action branches:" << reason; return false; };
  auto* title = page.findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
  auto* phase = page.findChild<QLabel*>(QStringLiteral("replayActionPhase"));
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks"));
  auto* ownQueue = page.findChild<QLabel*>(QStringLiteral("replayQueue0"));
  auto* enemyQueue = page.findChild<QLabel*>(QStringLiteral("replayQueue1"));
  auto* ownPlayer = page.findChild<QComboBox*>(QStringLiteral("replayPlayer0"));
  auto* perspective = page.findChild<QComboBox*>(QStringLiteral("replayOwnTeam"));
  auto* spirit = page.findChild<ReplaySpiritBar*>(QStringLiteral("replaySpirit0"));
  if (!title || !phase || !links || !ownQueue || !enemyQueue || !ownPlayer || !perspective || !spirit) return fail("missing semantic control");
  const auto queued = [](const QLabel* label, const QStringList& names) {
    const auto text = label->accessibleName(); int previous = -1;
    for (const auto& name : names) { const int index = text.indexOf(name); if (index <= previous) return false; previous = index; }
    return label->property("queueKnown").toBool();
  };
  const QString sourceName = QStringLiteral("synthetic source"), reserveName = QStringLiteral("synthetic queue reserve");
  page.seek(2);
  if (title->property("skillId").toString() != QStringLiteral("2015970") || !title->text().contains(QStringLiteral("skill:2015970")) ||
      page.replay().steps[2].loggedSkillId != QStringLiteral("201597") ||
      !queued(ownQueue, {reserveName, sourceName}) || !queued(enemyQueue, {QStringLiteral("synthetic target")}) ||
      ownQueue->property("playerRound").toInt() != 3 || ownQueue->property("teamId").toInt() != 0)
    return fail("combo playback identity or recorded queue order");
  const int unrelatedPlayer = ownPlayer->findData(QStringLiteral("other-player"));
  if (unrelatedPlayer < 0) return fail("same-team independent formation missing");
  ownPlayer->setCurrentIndex(unrelatedPlayer);
  if (!QMetaObject::invokeMethod(ownPlayer, "activated", Q_ARG(int, unrelatedPlayer))) return fail("cannot select other formation");
  const auto otherQueueText = ownQueue->accessibleName();
  if (otherQueueText.contains(QStringLiteral("synthetic unrelated")) ||
      (ownQueue->property("queueKnown").toBool() && !queued(ownQueue, {reserveName, sourceName}) &&
       !otherQueueText.contains(QStringLiteral("tester"))))
    return fail("same-ID units from another player misidentified as the recorded queue");
  const int recordedPlayer = ownPlayer->findData(QStringLiteral("tester"));
  ownPlayer->setCurrentIndex(recordedPlayer);
  if (recordedPlayer < 0 || !QMetaObject::invokeMethod(ownPlayer, "activated", Q_ARG(int, recordedPlayer)) ||
      !queued(ownQueue, {reserveName, sourceName})) return fail("return to recorded player lost queue order");
  page.seek(3);
  if (page.replay().steps[3].playbackKind != QStringLiteral("team-simple") || !title->property("skillId").toString().isEmpty() ||
      !queued(ownQueue, {reserveName, sourceName}) || ownQueue->property("playerRound").toInt() != 3 ||
      enemyQueue->property("playerRound").toInt() != 5 || !spirit->toPlainText().contains(QStringLiteral("9/10")) ||
      !spirit->toPlainText().contains(QStringLiteral("+4"))) return fail("simple team command, inherited queues or original progress delta");
  bool before = false, after = false;
  const auto source = BattleReplay::fighterKey(0, "tester", "7");
  for (int i = 0; i < page.playbackPhaseCount(); ++i) {
    page.seekPlaybackPhase(i); const auto kind = page.playbackPhaseKind();
    if (kind == QStringLiteral("cast") || kind == QStringLiteral("attack") || kind == QStringLiteral("counter") ||
        kind == QStringLiteral("counter-cast") || links->property("connectionCount").toInt() != 0)
      return fail("simple team command fabricated a skill attack");
    if (kind == QStringLiteral("prepare") && spirit->toPlainText().contains(QStringLiteral("+4")))
      return fail("simple team prepare exposed future progress");
    before |= kind == QStringLiteral("before"); after |= kind == QStringLiteral("after");
  }
  if (!before || !after || page.displayedState()[source].hp != 97) return fail("simple team before/after omitted or attack numbers consumed");
  // Official RoundQueue tests field presence before assigning the array:
  // absence inherits, but explicit null is invalid and must not reuse old IDs.
  page.seek(4);
  if (page.replay().steps[4].playbackKind != QStringLiteral("team-skill") || title->property("skillId").toString() != QStringLiteral("900001") ||
      !title->text().contains(QStringLiteral("skill:900001")) ||
      ownQueue->property("queueKnown").toBool() || ownQueue->accessibleName().contains(sourceName) ||
      ownQueue->accessibleName().contains(reserveName) || ownQueue->property("playerRound").toInt() != 4 ||
      !enemyQueue->property("queueKnown").toBool() || enemyQueue->accessibleName().contains(QStringLiteral("synthetic target")))
    return fail("team execution skill, invalid null queue or explicit empty queue");
  page.seek(5); page.seekPlaybackPhase(0);
  auto* enemyCell = page.findChild<ReplayCell*>(QStringLiteral("replayCell9"));
  if (!phase->text().contains(QStringLiteral("近程攻击不能")) || !phase->property("overRoundLimit").toBool() ||
      !enemyCell || !enemyCell->toPlainText().contains(QStringLiteral("近程攻击不能")) ||
      !ownQueue->property("queueKnown").toBool() || ownQueue->accessibleName().contains(sourceName) ||
      enemyQueue->property("playerRound").toInt() != 6) return fail("disabled reason, over-round marker or empty queue missing");
  perspective->setCurrentIndex(perspective->findData(1));
  if (ownQueue->property("teamId").toInt() != 1 || ownQueue->property("playerRound").toInt() != 6 ||
      !queued(ownQueue, {QStringLiteral("synthetic target")})) return fail("queue did not follow perspective");
  perspective->setCurrentIndex(perspective->findData(0)); page.seek(2);
  if (!queued(ownQueue, {reserveName, sourceName}) || ownQueue->property("playerRound").toInt() != 3 || page.replay().original != bytes)
    return fail("rewind leaked future queues or modified original data");
  const auto audit = auditBattleReplayEffects(page.replay());
  if (audit.errors()) { qCritical() << audit.report(); return fail("effect audit rejected valid branch fixture"); }
  return true;
}

bool verifyParallelLinePlayback() {
  // Synthetic independent players: no real multi-line capture is available.
  // Each line must begin at the shared batch snapshot, not its sibling's end.
  const auto player = [](const QString& name) {
    return QJsonObject{{"name", name}, {"cts", QJsonArray{QJsonObject{{"id", 7}, {"name", name + " unit"},
      {"x", 0}, {"y", 0}, {"hp", 100}, {"maxHp", 100}, {"vigour", 50}, {"shp", 0}}}}};
  };
  const auto line = [](int index, const QString& own, const QString& enemy, int loss) {
    const QJsonObject action{{"cti", 0}, {"cpn", own}, {"cci", 7}, {"si", 106986}, {"ses", 1}, {"csn", 1}, {"pr", 1},
      {"cn", QJsonObject{{"att_hps", "-1"}, {"ahp", 99}}},
      {"trl", QJsonArray{QJsonObject{{"tti", 1}, {"tpn", enemy}, {"tci", 7}, {"isse", true}, {"ishit", true},
        {"tn", QJsonObject{{"att_hps", QString::number(-loss)}, {"ahp", 100 - loss}}}}}}, {"prl", QJsonArray{}}};
    return QJsonObject{{"bfn", index}, {"isf", true}, {"pan", own}, {"pbn", enemy}, {"rl", QJsonArray{action}}};
  };
  const QJsonObject root{{"br", QJsonObject{{"win", 0}, {"or", QJsonObject{{"team", QJsonArray{
    QJsonObject{{"id", 0}, {"players", QJsonArray{player("line-a"), player("line-b")}}},
    QJsonObject{{"id", 1}, {"players", QJsonArray{player("enemy-a"), player("enemy-b")}}}}}}},
    {"fr", QJsonArray{line(0, "line-a", "enemy-a", 10), line(1, "line-b", "enemy-b", 20)}}}}};
  const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  const auto fail = [](const char* reason) { qCritical() << "parallel line playback:" << reason; return false; };
  QMap<int, int> lineSteps; int settlement = -1;
  for (int i = 0; i < page.replay().steps.size(); ++i) {
    const auto& step = page.replay().steps[i];
    if (step.parallelBatch && !step.actor.isEmpty()) lineSteps[step.lineIndex] = i;
    if (step.keyChanges.contains(QStringLiteral("同批结算"))) settlement = i;
  }
  auto* heading = page.findChild<QLabel*>(QStringLiteral("replayActionPhase"));
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  auto* clock = page.findChild<QVariantAnimation*>(QStringLiteral("replayPhaseAnimation"));
  if (!heading || !play || !clock || lineSteps.size() != 2 || !lineSteps.contains(0) || !lineSteps.contains(1) || settlement < 0)
    return fail("missing parallel views, settlement or controls");
  const int first = lineSteps[0], second = lineSteps[1], batch = page.replay().steps[first].batchId;
  const auto a = BattleReplay::fighterKey(0, "line-a", "7"), b = BattleReplay::fighterKey(0, "line-b", "7");
  const auto enemyA = BattleReplay::fighterKey(1, "enemy-a", "7"), enemyB = BattleReplay::fighterKey(1, "enemy-b", "7");
  const auto headerMatches = [&](int index) {
    const auto& step = page.replay().steps[index];
    return heading->property("parallelBatch").toBool() && heading->property("lineIndex").toInt() == step.lineIndex &&
      heading->property("batchId").toInt() == batch && heading->text().contains(QStringLiteral("同批")) &&
      heading->text().contains(QStringLiteral("分线查看"));
  };
  page.seek(first);
  if (!headerMatches(first) || page.displayedState()[a].hp != 99 || page.displayedState()[b].hp != 100)
    return fail("first line leaked sibling result");
  page.seek(second);
  if (!headerMatches(second) || page.replay().steps[second].batchId != batch || page.displayedState()[a].hp != 100 ||
      page.displayedState()[b].hp != 99) return fail("second line was applied after first line");
  page.seekMoment(0);
  if (!replayStateMatches(page, page.replay().steps[second].batchStartState)) return fail("microstep start is not shared batch state");
  int changedEffect = -1;
  const auto& effects = page.replay().steps[second].effects;
  for (int i = 0; i < effects.size(); ++i) if (effects[i].changed && effects[i].kind == QStringLiteral("value")) { changedEffect = i; break; }
  if (changedEffect < 0) return fail("missing second-line value effect");
  page.selectEffect(changedEffect);
  if (!headerMatches(second) || !replayStateMatches(page, page.replay().steps[second].moments[effects[changedEffect].moment].state) ||
      page.displayedState()[a].hp != 100 || page.displayedState()[enemyA].hp != 100) return fail("effect inspection borrowed sibling results");
  page.seek(first); page.seekMoment(0);
  if (!replayStateMatches(page, page.replay().steps[first].batchStartState)) return fail("rewind retained second-line results");
  const auto merged = [&] {
    return page.displayedState()[a].hp == 99 && page.displayedState()[b].hp == 99 &&
      page.displayedState()[enemyA].hp == 90 && page.displayedState()[enemyB].hp == 80;
  };
  page.seek(settlement);
  if (!page.replay().steps[settlement].actor.isEmpty() || !headerMatches(settlement) || !merged()) return fail("batch settlement missing merged observation");
  page.seek(0); play->click(); QSet<int> seenLines; bool sawSettlement = false;
  for (int count = 0; page.property("playbackRunning").toBool() && count < 200; ++count) {
    const int step = page.currentStep();
    if (step == first || step == second) {
      seenLines.insert(step);
      if (!headerMatches(step)) return fail("continuous playback lost batch/line context");
    }
    if (step == settlement) {
      sawSettlement = true;
      if (!merged() || !headerMatches(step)) return fail("continuous settlement displayed a single line result");
    }
    if (clock->state() != QAbstractAnimation::Running || clock->duration() <= 0) return fail("parallel phase clock stopped unexpectedly");
    clock->setCurrentTime(clock->duration());
  }
  return (seenLines.size() == 2 && sawSettlement && !page.property("playbackRunning").toBool() &&
    page.currentStep() == page.replay().steps.size() - 1 && merged() && page.replay().original == bytes) ||
    fail("continuous playback skipped merged batch or failed to stop");
}

bool verifyTableReferenceNavigation() {
  // Synthetic battle state; these reference IDs and expected names come from
  // the embedded official tables. Combo row 1 is playback selection, not a
  // declaration that skill 201597 applies Buff 2793.
  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106986");
  bytes.replace("\"bfs\":\"99,99\"", "\"nskill\":201597,\"bfs\":\"2793,400025,645,220483,40167,14424,2147483647\"");
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  auto* cell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* name = cell ? cell->findChild<QPushButton*>(QStringLiteral("unitName")) : nullptr;
  auto* detail = page.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
  if (!name || !detail) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  const auto key = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  const auto unitHref = QStringLiteral("unit:") + QString::fromLatin1(QUrl::toPercentEncoding(key));
  auto activate = [detail](const QString& href) {
    // Require a real link in the current document, then use the same signal
    // as a user click. This catches both absent links and broken handlers.
    for (auto block = detail->document()->begin(); block.isValid(); block = block.next()) {
      for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
        const auto actualHref = fragment.fragment().charFormat().anchorHref();
        if (!actualHref.isEmpty() && QUrl(actualHref) == QUrl(href))
          return QMetaObject::invokeMethod(detail, "anchorClicked", Q_ARG(QUrl, QUrl(actualHref)));
      }
    }
    qCritical() << "missing displayed reference link" << href;
    return false;
  };
  auto heading = [detail] { return detail->document()->firstBlock().text(); };
  if (!activate(QStringLiteral("skill:201597")) || heading() != QStringLiteral("异次元·龙星坠灭")) return false;
  const auto skillText = detail->toPlainText();
  if (!skillText.contains(QStringLiteral("持有 异次元神力判断BUFF 时，改用播放技能 异次元·龙星坠灭")) ||
      skillText.contains(QStringLiteral("[2793]")) || skillText.contains(QStringLiteral("[2015970]")) ||
      skillText.contains(QStringLiteral("ComboBuffsConfigXML")) || skillText.contains(QStringLiteral("/config/combo/s[1]"))) return false;
  if (!activate(QStringLiteral("skill-raw:201597")) || heading() != QStringLiteral("异次元·龙星坠灭 [201597]")) return false;
  const auto rawSkillText = detail->toPlainText();
  if (!rawSkillText.contains(QStringLiteral("ComboBuffsConfigXML")) || !rawSkillText.contains(QStringLiteral("/config/combo/s[1]")) ||
      !hasReplayTextAnchor(detail, QStringLiteral("buff:2793")) || !hasReplayTextAnchor(detail, QStringLiteral("skill:2015970")) ||
      !activate(QStringLiteral("skill:201597"))) return false;
  if (!activate(QStringLiteral("buff:2793")) || heading() != QStringLiteral("异次元神力判断BUFF")) return false;
  if (!detail->toPlainText().contains(QStringLiteral("当前成员记录数：1")) ||
      !detail->toPlainText().contains(QStringLiteral("synthetic source"))) return false;
  // Both the base skill and the alternate playback skill must lead back to
  // the same prerequisite Buff, with the unit selection preserved throughout.
  if (!activate(QStringLiteral("skill:2015970")) || heading() != QStringLiteral("异次元·龙星坠灭")) return false;
  if (!activate(QStringLiteral("buff:2793")) || !activate(QStringLiteral("skill:201597")) ||
      heading() != QStringLiteral("异次元·龙星坠灭")) return false;
  if (!activate(QStringLiteral("buff:2793")) || !activate(unitHref) || heading() != QStringLiteral("synthetic source")) return false;
  if (!activate(QStringLiteral("buff:645")) || heading() != QStringLiteral("N极暴击")) return false;
  if (!activate(unitHref) || !activate(QStringLiteral("buff:400025")) || heading() != QStringLiteral("未命名效果")) return false;
  // 400025 shares display resource 645; the UI must not borrow Buff 645's
  // unrelated name or description, or call a present unnamed row unrecorded.
  if (detail->toPlainText().contains(QStringLiteral("N极暴击")) || detail->toPlainText().contains(QStringLiteral("400025")) ||
      !detail->toPlainText().contains(QStringLiteral("暂无独立说明")) || !activate(QStringLiteral("buff-raw:400025")) ||
      heading() != QStringLiteral("无名称 Buff [400025]")) return false;
  if (detail->toPlainText().contains(QStringLiteral("N极暴击")) || !detail->toPlainText().contains(QStringLiteral("645")) ||
      !activate(QStringLiteral("buff:400025"))) return false;
  if (!activate(unitHref) || !activate(QStringLiteral("buff:220483")) ||
      !detail->toPlainText().contains(QStringLiteral("关联技能说明")) ||
      !detail->toPlainText().contains(QStringLiteral("每集齐14点通灵之魂"))) return false;
  if (!activate(unitHref) || !activate(QStringLiteral("buff:14424")) ||
      !detail->toPlainText().contains(QStringLiteral("官方效果备注：全属性↓↓，暴击↓↓，伤害↓↓"))) return false;
  if (!activate(unitHref) || !activate(QStringLiteral("buff:40167")) ||
      !detail->toPlainText().contains(QStringLiteral("关联官方词条说明")) ||
      !detail->toPlainText().contains(QStringLiteral("在己阵精灵出手后，立刻出手1次")) ||
      detail->toPlainText().contains(QStringLiteral("EntryConfigXML")) || !activate(QStringLiteral("buff-raw:40167")) ||
      !detail->toPlainText().contains(QStringLiteral("EntryConfigXML")) || !activate(QStringLiteral("buff:40167"))) return false;
  if (!activate(unitHref) || !activate(QStringLiteral("buff:2147483647")) ||
      heading() != QStringLiteral("效果名称未收录") || detail->toPlainText().contains(QStringLiteral("2147483647")) ||
      !activate(QStringLiteral("buff-raw:2147483647")) || heading() != QStringLiteral("未收录 Buff [2147483647]")) return false;
  return page.currentStep() == 0 && page.replay().original == bytes;
}

bool verifyPublicDetailsKeepRawTraceability() {
  const auto fail = [](const char* reason, const QString& text = {}) {
    qCritical().noquote() << "public replay details:" << reason << text; return false;
  };
  // These are our generated technical labels, not a blanket number/bracket
  // filter: combat balances, turn numbers and the legitimate [灵初] prefix stay.
  const QStringList technical{QStringLiteral("[106986]"), QStringLiteral("[201597]"), QStringLiteral("[2793]"),
    QStringLiteral("[400025]"), QStringLiteral("Buff ID"), QStringLiteral("单位 ID"), QStringLiteral("种族 ID"),
    QStringLiteral("外观 ID"), QStringLiteral("/br/"), QStringLiteral("/config/"), QStringLiteral("buffWaitType"),
    QStringLiteral("bd_bfs"), QStringLiteral("ad_bfs"), QStringLiteral("changeset"), QStringLiteral("ComboBuffsConfigXML"),
    QStringLiteral("\"bfs\""), QStringLiteral("\"nskill\""), QStringLiteral("\"pbid\""), QStringLiteral("\"proa\"")};
  const auto publicText = [&](const QString& text) {
    for (const auto& token : technical) if (text.contains(token)) return false;
    return true;
  };
  auto bytes = battleReplayStateFixture(); bytes.replace("999999999", "106986");
  auto root = QJsonDocument::fromJson(bytes).object(); auto battle = root["br"].toObject();
  auto initial = battle["or"].toObject(); auto teams = initial["team"].toArray();
  auto team = teams[0].toObject(); auto players = team["players"].toArray(); auto player = players[0].toObject();
  auto fighters = player["cts"].toArray(); auto fighter = fighters[0].toObject();
  const auto publicName = QStringLiteral("[灵初]可视化测试");
  fighter["name"] = publicName; fighter["hp"] = 123456; fighter["maxHp"] = 200000;
  fighter["nskill"] = 201597; fighter["bfs"] = QStringLiteral("2793,400025");
  fighters[0] = fighter; player["cts"] = fighters; players[0] = player; team["players"] = players;
  teams[0] = team; initial["team"] = teams; battle["or"] = initial; root["br"] = battle;
  bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return fail("fixture file"); file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1320, 960);
  if (!page.loadFile(file.fileName())) return fail("fixture load"); page.show(); QTest::qWait(20);
  auto* cell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* name = cell ? cell->findChild<QPushButton*>(QStringLiteral("unitName")) : nullptr;
  auto* hp = cell ? cell->findChild<QProgressBar*>(QStringLiteral("hpBar")) : nullptr;
  auto* buffs = cell ? cell->findChild<QTextBrowser*>(QStringLiteral("unitBuffs")) : nullptr;
  auto* details = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  if (!name || !hp || !buffs || !details || name->text() != publicName || hp->text() != QStringLiteral("123456/200000"))
    return fail("public name prefix or actual balance changed");
  for (const auto& objectName : {QStringLiteral("unitBuffToggle"), QStringLiteral("unitBuffUnknownToggle")})
    if (auto* toggle = cell->findChild<QToolButton*>(objectName)) toggle->setChecked(true);
  if (buffs->property("memberCount").toInt() != 2 || buffs->property("groupCount").toInt() != 2 ||
      !hasReplayTextAnchor(buffs, QStringLiteral("buff:2793#member0")) || !hasReplayTextAnchor(buffs, QStringLiteral("buff:400025#member1")))
    return fail("hidden identifiers no longer navigate to the distinct original effects");
  if (!publicText(name->text() + name->toolTip() + name->accessibleName() + buffs->toPlainText() + buffs->toolTip() +
      buffs->accessibleName() + cell->toPlainText())) return fail("public card exposed technical identity", cell->toPlainText());

  name->click(); QCoreApplication::processEvents();
  const auto key = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(key));
  const auto unitHref = QStringLiteral("unit:") + encoded;
  if (!publicText(details->toPlainText()) || !details->toPlainText().contains(publicName) ||
      !details->toPlainText().contains(QStringLiteral("123456")) || !details->toPlainText().contains(QStringLiteral("200000")))
    return fail("normal unit details dropped game facts or exposed fields", details->toPlainText());
  if (!activateReplayTextAnchor(details, QStringLiteral("unit-raw:") + encoded) ||
      !details->toPlainText().contains(QStringLiteral("单位 ID 7")) || !details->toPlainText().contains(QStringLiteral("/br/or/team/0/players/0/cts/0/hp")) ||
      !details->toPlainText().contains(QStringLiteral("\"nskill\"")) || !activateReplayTextAnchor(details, unitHref) ||
      !publicText(details->toPlainText())) return fail("unit raw details or board forwarding lost", details->toPlainText());

  if (!activateReplayTextAnchor(details, QStringLiteral("buff:400025")) || !publicText(details->toPlainText()) ||
      details->document()->firstBlock().text() != QStringLiteral("未命名效果") || details->toPlainText().contains(QStringLiteral("N极暴击")))
    return fail("unnamed effect borrowed a display-resource name or exposed its ID");
  if (!activateReplayTextAnchor(details, QStringLiteral("buff-raw:400025")) ||
      !details->toPlainText().contains(QStringLiteral("[400025]")) || !details->toPlainText().contains(QStringLiteral("/bfs")) ||
      !details->toPlainText().contains(QStringLiteral("\"did\"")) || !activateReplayTextAnchor(details, QStringLiteral("buff:400025")) ||
      !publicText(details->toPlainText()) || !activateReplayTextAnchor(details, unitHref))
    return fail("effect raw configuration/history or return link lost");

  if (!activateReplayTextAnchor(details, QStringLiteral("skill:201597")) || !publicText(details->toPlainText()) ||
      !details->toPlainText().contains(QStringLiteral("异次元·龙星坠灭")) || !hasReplayTextAnchor(details, QStringLiteral("buff:2793")))
    return fail("normal skill relationship lost");
  if (!activateReplayTextAnchor(details, QStringLiteral("skill-raw:201597")) ||
      !details->toPlainText().contains(QStringLiteral("[201597]")) || !details->toPlainText().contains(QStringLiteral("/config/combo/s[1]")) ||
      !activateReplayTextAnchor(details, QStringLiteral("skill:201597")) || !publicText(details->toPlainText()))
    return fail("skill raw table connection or return link lost");
  page.seek(2);
  auto* banner = page.findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
  if (!banner) return fail("skill banner unavailable");
  QTextDocument bannerText; bannerText.setHtml(banner->text());
  if (banner->property("skillId").toString() != QStringLiteral("106986") || !bannerText.toPlainText().contains(publicName) ||
      !publicText(bannerText.toPlainText() + banner->toolTip() + banner->accessibleName()))
    return fail("skill banner lost identity or displayed technical fields", banner->toolTip());

  ReplaySpiritBar spirit;
  const auto progressRaw = QStringLiteral("/br/or/team/0/players/0/ppbf {\"pbid\":220483,\"mp\":14}");
  const auto transitionRaw = QStringLiteral("/br/fr/0/rl/0/tn/rl/0 {\"proa\":7,\"vc\":5}");
  spirit.setProgress(7, 14, QStringLiteral("通灵之魂"), progressRaw);
  spirit.setTransition(2, 5, QStringLiteral("通灵充能"), false); spirit.setTransitionKind(QStringLiteral("charge"), transitionRaw);
  if (!publicText(spirit.toPlainText() + spirit.toolTip() + spirit.accessibleName() + spirit.accessibleDescription()) ||
      !spirit.toolTip().contains(QStringLiteral("7/14")) || !spirit.toolTip().contains(QStringLiteral("+5")) ||
      spirit.property("transitionEvidence").toString() != transitionRaw || !spirit.property("rawEvidence").toString().contains(progressRaw))
    return fail("spirit public text and raw provenance were not separated");
  return page.replay().original == bytes;
}

bool verifyPetMechanismNavigation() {
  auto bytes = battleReplayStateFixture();
  bytes.replace("\"bfs\":\"99,99\"", "\"raceId\":6461,\"bfs\":\"400025\"");
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); if (!page.loadFile(file.fileName())) return false;
  auto* cell = page.findChild<ReplayCell*>(QStringLiteral("replayCell2"));
  auto* name = cell ? cell->findChild<QPushButton*>(QStringLiteral("unitName")) : nullptr;
  auto* detail = page.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
  if (!name || !detail) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  const auto key = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  const auto encodedKey = QString::fromLatin1(QUrl::toPercentEncoding(key));
  const auto unitHref = QStringLiteral("unit:") + encodedKey;
  const auto rawUnitHref = QStringLiteral("unit-raw:") + encodedKey;
  auto activate = [detail](const QString& href) {
    for (auto block = detail->document()->begin(); block.isValid(); block = block.next())
      for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
        const auto actual = fragment.fragment().charFormat().anchorHref();
        if (!actual.isEmpty() && QUrl(actual) == QUrl(href))
          return QMetaObject::invokeMethod(detail, "anchorClicked", Q_ARG(QUrl, QUrl(actual)));
      }
    qCritical() << "missing mechanism reference" << href;
    return false;
  };
  if (detail->toPlainText().contains(QStringLiteral("单位 ID")) || !activate(rawUnitHref) || !activate(QStringLiteral("mechanics:6461"))) return false;
  auto text = detail->toPlainText();
  if (!text.contains(QStringLiteral("气运条件与效果")) || !text.contains(QStringLiteral("属性档位 ≥ 70%")) ||
      !text.contains(QStringLiteral("本场触发情况：未判定")) || !activate(QStringLiteral("skill:926228"))) return false;
  if (detail->toPlainText().contains(QStringLiteral("926228")) || !activate(QStringLiteral("skill-raw:926228")) ||
      !detail->document()->firstBlock().text().contains(QStringLiteral("[926228]")) ||
      !activate(QStringLiteral("mechanics:6461"))) return false;
  if (!activate(unitHref) || !activate(QStringLiteral("buff:400025")) ||
      detail->document()->firstBlock().text() != QStringLiteral("未命名效果")) return false;
  auto lingchu = bytes; lingchu.replace("\"raceId\":6461", "\"raceId\":7412");
  QTemporaryFile other; if (!other.open()) return false; other.write(lingchu); other.flush();
  if (!page.loadFile(other.fileName())) return false;
  name = cell->findChild<QPushButton*>(QStringLiteral("unitName"));
  if (!name) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  if (!activate(rawUnitHref) || !activate(QStringLiteral("mechanics:7412"))) return false;
  text = detail->toPlainText();
  if (!text.contains(QStringLiteral("溯灵条件与效果")) || text.contains(QStringLiteral("气运条件与效果")) ||
      !text.contains(QStringLiteral("暴击率≥150%")) || !text.contains(QStringLiteral("【数值未记录】")) ||
      !text.contains(QStringLiteral("强化之力")) || !text.contains(QStringLiteral("本场触发情况：未判定"))) return false;
  if (!activate(QStringLiteral("skill:927396"))) return false;
  // A dictionary association cannot change raw battle state or relabel an unnamed Buff.
  if (page.currentStep() != 0 || page.replay().original != lingchu || page.displayedState()[key].buffs != QStringList{QStringLiteral("400025")}) return false;
  auto legacy = bytes; legacy.replace("\"raceId\":6461", "\"raceId\":3679");
  QTemporaryFile legacyFile; if (!legacyFile.open()) return false; legacyFile.write(legacy); legacyFile.flush();
  if (!page.loadFile(legacyFile.fileName())) return false;
  name = cell->findChild<QPushButton*>(QStringLiteral("unitName")); if (!name) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  if (!activate(rawUnitHref) || !activate(QStringLiteral("mechanics:3679"))) return false;
  text = detail->toPlainText();
  if (!text.contains(QStringLiteral("传说技能资料 [903679]")) || !text.contains(QStringLiteral("本场档位：未记录")) ||
      !text.contains(QStringLiteral("等级总和范围：0-4")) || !text.contains(QStringLiteral("等级总和范围：25-25"))) return false;
  auto unresolved = bytes; unresolved.replace("\"raceId\":6461", "\"raceId\":6685");
  QTemporaryFile unresolvedFile; if (!unresolvedFile.open()) return false; unresolvedFile.write(unresolved); unresolvedFile.flush();
  if (!page.loadFile(unresolvedFile.fileName())) return false;
  name = cell->findChild<QPushButton*>(QStringLiteral("unitName")); if (!name) return false;
  QTest::mouseClick(name, Qt::LeftButton);
  if (!activate(rawUnitHref) || !activate(QStringLiteral("mechanics:6685"))) return false;
  text = detail->toPlainText();
  if (!text.contains(QStringLiteral("当前资料未收录这项方案")) || text.contains(QStringLiteral("属性档位 ≥ 70%"))) return false;
  for (const auto& race : {QByteArray("3873"), QByteArray("4567"), QByteArray("4832")}) {
    auto huan = bytes; huan.replace("\"raceId\":6461", QByteArray("\"raceId\":") + race);
    QTemporaryFile huanFile; if (!huanFile.open()) return false; huanFile.write(huan); huanFile.flush();
    if (!page.loadFile(huanFile.fileName())) return false;
    name = cell->findChild<QPushButton*>(QStringLiteral("unitName")); if (!name) return false;
    QTest::mouseClick(name, Qt::LeftButton);
    if (!activate(rawUnitHref) || !activate(QStringLiteral("mechanics:") + QString::fromLatin1(race))) return false;
    text = detail->toPlainText();
    if (!text.contains(QStringLiteral("寰力量／异族审判资料")) || !text.contains(QStringLiteral("本场是否生效：未判定"))) return false;
    if (race == "3873" && (!text.contains(QStringLiteral("战斗力 ≥ 16000")) || !text.contains(QStringLiteral("战斗力 ≥ 21500")) || !text.contains(QStringLiteral("等级上限提升为125级")))) return false;
    if (race == "4567" && (!text.contains(QStringLiteral("战斗力 ≥ 22000")) || !text.contains(QStringLiteral("真理审判")))) return false;
    if (race == "4832" && !text.contains(QStringLiteral("当前资料未收录这项配置"))) return false;
    if (detail->toHtml().contains(QStringLiteral("skill:70") + QString::fromLatin1(race))) return false;
    if (page.currentStep() != 0 || page.replay().original != huan || page.displayedState()[key].buffs != QStringList{QStringLiteral("400025")}) return false;
  }
  return true;
}

#include "battle_replay_eleven_checks.h"
#include "battle_replay_reference_checks.h"
#include "battle_replay_arrow_caption_checks.h"
#include "battle_replay_action_meter_checks.h"
#include "battle_replay_action_float_checks.h"
#include "battle_replay_meter_sequence_checks.h"
#include "battle_replay_catalog_checks.h"
#include "battle_replay_life_timeline_checks.h"
#include "battle_replay_buff_display_checks.h"
#include "battle_replay_summon_display_checks.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  }
  const auto args = app.arguments();
  // Preview must not overwrite the user's regular workbench preferences.
  QTemporaryDir preferences; UiPreferences::setFilePath(preferences.filePath(QStringLiteral("preview.ini")));
  app.setApplicationName(QStringLiteral("KQBattleReplayPreview"));
  const auto catalogSnapshot = previewBattleReplayCatalog();
  if (!catalogSnapshot) { qCritical() << "embedded replay catalog unavailable"; return 57; }
  const auto catalog = catalogSnapshot->root;
  if (args.contains(QStringLiteral("--effect-projection"))) {
    const int inputIndex = args.indexOf(QStringLiteral("--file"));
    const int outputIndex = args.indexOf(QStringLiteral("--effect-projection"));
    if (inputIndex < 0 || args.size() <= inputIndex + 1 || args.size() <= outputIndex + 1) {
      qCritical() << "--effect-projection OUTPUT.json requires --file INPUT"; return 35;
    }
    QFile input(args[inputIndex + 1]);
    if (!input.open(QIODevice::ReadOnly)) { qCritical() << "cannot read projection input" << input.errorString(); return 35; }
    const auto replay = BattleReplay::parse(input.readAll(), catalog);
    if (!replay.error.isEmpty()) { qCritical() << "cannot parse projection input" << replay.error; return 35; }
    QJsonArray steps; qint64 rowCount = 0;
    for (int index = 0; index < replay.steps.size(); ++index) {
      const auto& step = replay.steps[index]; if (step.moments.isEmpty()) continue;
      QList<QByteArray> rows;
      QJsonArray displayCommands;
      int buffAppearances = 0;
      for (const auto& effect : step.effects) {
        if (BattleReplayEffectAuditDetail::projected(effect)) rows.append(BattleReplayEffectAuditDetail::projection(effect));
        if (effect.kind == QStringLiteral("appearance") && effect.cause == QStringLiteral("buff")) ++buffAppearances;
        if (effect.restoresDisplayLayers) displayCommands.append(QJsonObject{
          {"buffId", effect.causeId}, {"target", effect.target}, {"path", effect.path}, {"raw", effect.raw},
          {"moment", effect.moment}, {"command", QStringLiteral("summonAppear")}});
      }
      // Keep the exact compact UTF-8 strings and byte ordering used by the
      // native digest; consumers can json.loads each row for field-level diffs.
      std::sort(rows.begin(), rows.end()); QJsonArray exported;
      for (const auto& row : rows) exported.append(QString::fromUtf8(row));
      rowCount += rows.size();
      steps.append(QJsonObject{{"step", index}, {"path", step.path}, {"playbackKind", step.playbackKind},
        {"loggedSkillId", step.loggedSkillId}, {"playbackSkillId", step.playbackSkillId},
        {"counterSkillId", step.counterSkillId}, {"rows", exported}, {"displayCommands", displayCommands},
        {"buffAppearanceEvents", buffAppearances}, {"executionBuffId", step.raw.toObject()[QStringLiteral("epbi")]}});
    }
    const auto audit = auditBattleReplayEffects(replay);
    const QJsonObject report{{"sha256", replay.sha256}, {"effectsSha256", audit.effectsSha256},
      {"actions", replay.actions}, {"rowCount", rowCount}, {"auditErrors", audit.errors()}, {"steps", steps}};
    QFile output(args[outputIndex + 1]);
    if (!output.open(QIODevice::WriteOnly)) { qCritical() << "cannot write projection output" << output.errorString(); return 35; }
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size()) { qCritical() << "incomplete projection output" << output.errorString(); return 35; }
    qInfo() << "effect projection actions" << steps.size() << "rows" << rowCount << "auditErrors" << audit.errors();
    return 0;
  }
  if (args.contains(QStringLiteral("--audit")) || args.contains(QStringLiteral("--audit-empowerment"))) {
    const bool empowermentOnly = args.contains(QStringLiteral("--audit-empowerment"));
    const int index = args.indexOf(empowermentOnly ? QStringLiteral("--audit-empowerment") : QStringLiteral("--audit"));
    if (args.size() <= index + 2) return 2;
    QElapsedTimer elapsed; elapsed.start(); QJsonArray files; int failed = 0, totalActions = 0;
    for (const auto& info : QDir(args[index + 1]).entryInfoList({QStringLiteral("*.txt"), QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
      QFile file(info.absoluteFilePath()); if (!file.open(QIODevice::ReadOnly)) { ++failed; continue; }
      const auto bytes = file.readAll(); auto parsingBytes = bytes;
      if (empowermentOnly) {
        auto root = QJsonDocument::fromJson(bytes).object();
        if (root.contains(QStringLiteral("br"))) { auto br = root[QStringLiteral("br")].toObject(); br[QStringLiteral("fr")] = QJsonArray{}; root[QStringLiteral("br")] = br; }
        else root[QStringLiteral("fr")] = QJsonArray{};
        parsingBytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
      }
      const auto result = BattleReplay::parse(parsingBytes, catalog);
      const auto effectAudit = auditBattleReplayEffects(result);
      if (effectAudit.errors()) ++failed;
      QJsonArray empowerment;
      if (!result.steps.isEmpty()) for (const auto& e : BattleReplay::identifyEmpowerments(result.steps.first().state, catalog))
        empowerment.append(QJsonObject{{"source",e.source}, {"target",e.target}, {"skillId",e.skillId}});
      if (!result.error.isEmpty() || result.original != parsingBytes) ++failed;
      totalActions += result.actions;
      QCryptographicHash ledger(QCryptographicHash::Sha256), battlefield(QCryptographicHash::Sha256); int moments = 0, unknownShield = 0, negativeShield = 0, unknownBuffs = 0;
      int unknownPower = 0; QJsonObject keyEvents;
      for (const auto& step : result.steps) {
        battlefield.addData(step.path.toUtf8() + '\n'); battlefield.addData(battlefieldLedger(step.state));
        for (const auto& event : step.events) keyEvents[event.kind] = keyEvents[event.kind].toInt() + 1;
        if (step.moments.isEmpty()) continue;
        ledger.addData(step.path.toUtf8() + '\n'); ledger.addData(stateLedger(step.state));
        moments += static_cast<int>(step.moments.size());
        for (const auto& f : step.state) {
          if (!f.representer && !f.shield) ++unknownShield;
          if (!f.representer && !f.maxPower) ++unknownPower;
          if (f.shield && *f.shield < 0) ++negativeShield;
          if (!f.buffsKnown) ++unknownBuffs;
        }
      }
      files.append(QJsonObject{{"file", info.fileName()}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}, {"actions", result.actions},
        {"nodes", result.fields.size()}, {"steps", result.steps.size()}, {"error", result.error}, {"issues", QJsonArray::fromStringList(result.issues)},
        {"stateLedgerSha256", QString::fromLatin1(ledger.result().toHex())}, {"moments", moments},
        {"unknownShieldObservations", unknownShield}, {"negativeShieldObservations", negativeShield}, {"unknownBuffObservations", unknownBuffs},
        {"battlefieldLedgerSha256", QString::fromLatin1(battlefield.result().toHex())}, {"unknownPowerReferenceObservations", unknownPower}, {"keyEvents", keyEvents},
        {"effectsSha256", effectAudit.effectsSha256}, {"effectAudit", effectAudit.report()}, {"empowerment", empowerment}});
    }
    const QJsonObject report{{"files", files}, {"failed", failed}, {"actions", totalActions}, {"elapsedMs", elapsed.elapsed()}};
    QFile output(args[index + 2]); if (!output.open(QIODevice::WriteOnly)) return 3;
    output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    qInfo() << "files" << files.size() << "actions" << totalActions << "failed" << failed << "ms" << elapsed.elapsed();
    return failed ? 1 : 0;
  }
  WorkbenchWindow window([](WorkbenchPage page, QWidget* parent) -> QWidget* {
    if (page == WorkbenchPage::BattleReplay) return createPreviewBattleReplayPage(parent);
    auto* label = new QLabel(QStringLiteral("独立预览只加载战斗回放。正式工作台保留全部业务页面。"), parent); label->setWordWrap(true); return label;
  }, nullptr, WorkbenchOptions{QSize(1920, 1080), QSize(1440, 940)});
  window.setWindowTitle(QStringLiteral("战斗回放 · 独立样板"));
  PreviewCloseFilter closeFilter; window.installEventFilter(&closeFilter);
  window.setSession({{}, 0, QStringLiteral("本地日志 · 独立预览"), QStringLiteral("记录核对版"), false});
  if (!window.showPage(WorkbenchPage::BattleReplay)) return 4;
  auto* page = qobject_cast<BattleReplayPage*>(window.page(WorkbenchPage::BattleReplay));
  QTemporaryFile fixture;
  bool verify = args.contains(QStringLiteral("--verify"));
  if (args.contains(QStringLiteral("--file"))) {
    const int index = args.indexOf(QStringLiteral("--file")); if (args.size() <= index + 1 || !page->loadFile(args[index + 1])) return 5;
  } else if (args.contains(QStringLiteral("--folder"))) {
    const int index = args.indexOf(QStringLiteral("--folder")); if (args.size() <= index + 1) return 5;
    page->loadDirectory(args[index + 1]);
  } else if (verify) {
    fixture.open(); fixture.write(battleReplayFixture()); fixture.flush();
    if (!page->loadFile(fixture.fileName())) return 6;
  }
  window.show(); app.processEvents();
  if (args.contains(QStringLiteral("--verify-summon-display"))) {
    const int index = args.indexOf(QStringLiteral("--verify-summon-display"));
    if (!args.contains(QStringLiteral("--file")) || args.size() <= index + 1) return 62;
    const auto report = verifyRecordedSummonDisplay(*page);
    QFile output(args[index + 1]); if (!output.open(QIODevice::WriteOnly)) return 62;
    output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    return report[QStringLiteral("passed")].toBool() ? 0 : 62;
  }
  if (args.contains(QStringLiteral("--verify-playback")) || args.contains(QStringLiteral("--verify-realtime-playback"))) {
    if (!args.contains(QStringLiteral("--file"))) { qCritical() << "playback verification requires --file"; return 32; }
    const auto report = args.contains(QStringLiteral("--verify-realtime-playback")) ? verifyRealtimePlayback(*page) : verifyImportedPlayback(*page);
    if (args.contains(QStringLiteral("--playback-report"))) {
      const int index = args.indexOf(QStringLiteral("--playback-report"));
      if (args.size() <= index + 1) return 32;
      QFile output(args[index + 1]); if (!output.open(QIODevice::WriteOnly)) return 32;
      output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    }
    qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
    return report[QStringLiteral("passed")].toBool() ? 0 : 32;
  }
  if (verify) {
    if (!verifySummonDisplayCell()) { qCritical() << "persistent summon display/same-cell/timeline regression"; return 62; }
    if (!verifyReplayCatalogSnapshots()) { qCritical() << "catalog snapshot isolation/readiness/reload regression"; return 58; }
    if (!verifyReplayLifeTimeline()) { qCritical() << "life timeline/action-start navigation regression"; return 59; }
    if (!verifyRelationRibbonCaptions()) { qCritical() << "arrow body caption regression"; return 51; }
    if (!verifyActionMeterCellPresentation(catalog)) { qCritical() << "action meter ghost segments regression"; return 52; }
    if (!verifyReplayMeterSequenceProjection()) { qCritical() << "recorded number sequence projection regression"; return 55; }
    if (!verifyReplayMeterSequenceNavigation()) { qCritical() << "recorded number sequence navigation regression"; return 56; }
    if (!verifyReplayActionFloatPresentation(catalog)) { qCritical() << "action floating values regression"; return 53; }
    if (!verifyReplayReferenceLibrary(catalog)) { qCritical() << "official reference library regression"; return 50; }
    if (!verifyIncomingReplayAndIdentity()) { qCritical() << "pending replay/identity/source entry regression"; return 40; }
    if (!verifySameCellUnitAnchors()) { qCritical() << "same-cell identity/anchor/dense layout/unknown diagnostic regression"; return 41; }
    if (!verifyFormationLayout()) { qCritical() << "formation orientation or perspective-switch regression"; return 15; }
    if (!verifyStateNavigation()) { qCritical() << "substep board/Buff navigation regression"; return 16; }
    if (!verifyMeters()) { qCritical() << "exact meter values/reference/overflow regression"; return 17; }
    if (!verifyBattlefieldNavigation()) { qCritical() << "player selection/unit filtering/key event navigation regression"; return 18; }
    if (!verifyEffectsAndSpirit()) { qCritical() << "effect attribution/Buff/spirit interaction regression"; return 19; }
    if (!verifyActionOverview()) { qCritical() << "action target/summary/transient Buff regression"; return 23; }
    if (!verifyBoardLayoutAndInspector()) { qCritical() << "board card layout/inline interaction regression"; return 24; }
    if (!verifyUnitEmphasisAndResults()) { qCritical() << "actor emphasis/death state/recorded judgement badge regression"; return 25; }
    if (!verifyBuffSourcePresentation()) { qCritical() << "Buff applier grouping/perspective/unknown-source regression"; return 26; }
    if (!verifyReplayBuffDisplay(catalog)) { qCritical() << "Buff state/playback category and raw-record preservation regression"; return 60; }
    if (!verifyRoundBuffNotices()) { qCritical() << "round Buff lifetime/collapse/history navigation regression"; return 43; }
    if (!verifyRelationColorsAndHitCues()) { qCritical() << "relative arrow/attribute/visual judgment regression"; return 44; }
    if (!verifyOutcomeVisualMotion()) { qCritical() << "outcome animation/shared clock/Buff type regression"; return 45; }
    if (!verifyElevenCellEffects(catalog)) { qCritical() << "eleven-effect cell presentation regression"; return 46; }
    if (!verifyElevenStateTransitions(catalog)) { qCritical() << "specific skill/form/attribute transition regression"; return 47; }
    if (!verifyOpeningEmpowermentPresentation(catalog)) { qCritical() << "opening empowerment/self-transform evidence regression"; return 48; }
    if (!verifySpiritPresentationBranches()) { qCritical() << "spirit presentation branch regression"; return 49; }
    if (!verifyDragonQueuePresentation()) { qCritical() << "dragon queue display/round gate/backward seek regression"; return 38; }
    if (!verifyClientHintsAndBuffPresentation()) { qCritical() << "client hit hint/origin/Buff presentation regression"; return 39; }
    if (!verifyContinuousPlaybackAndSkills()) { qCritical() << "continuous playback/skill banner regression"; return 27; }
    if (!verifyReadablePhaseTransitions()) { qCritical() << "major transition readability/zero Buff record regression"; return 37; }
    if (!verifyRecordedPlaybackStages()) { qCritical() << "recorded stage grouping/timing/counter direction regression"; return 29; }
    if (!verifyMeterTransitionsKeepRecordedNumbers()) { qCritical() << "meter animation changed logged numbers regression"; return 28; }
    if (!verifySpecialRecordDisplayAudit()) { qCritical() << "special-record original/state delta audit regression"; return 33; }
    if (!verifyOfficialActionBranchesAndQueues()) { qCritical() << "official action branches/queue inheritance regression"; return 34; }
    if (!verifyParallelLinePlayback()) { qCritical() << "parallel-line batch snapshot/navigation regression"; return 36; }
    if (!verifyTableReferenceNavigation()) { qCritical() << "official skill/Buff reference navigation or unnamed-entry regression"; return 20; }
    if (!verifyPublicDetailsKeepRawTraceability()) { qCritical() << "public UI/explicit raw-detail traceability regression"; return 61; }
    if (!verifyPetMechanismNavigation()) { qCritical() << "pet/passive/fortune reference or activation-boundary regression"; return 22; }
    if (!verifyRealtimeSyntheticPlayback()) { qCritical() << "real-clock multi-target/transient state/pause regression"; return 42; }
    const int last = static_cast<int>(page->replay().steps.size()) - 1;
    page->seek(last); if (page->currentStep() != last) return 7;
    page->seek(0); if (page->currentStep() != 0) return 8;
    auto* next = page->findChild<QPushButton*>(QStringLiteral("replayNext")); QTest::mouseClick(next, Qt::LeftButton);
    if (page->currentStep() != 1 || window.creationCount(WorkbenchPage::BattleReplay) != 1) return 9;
    const auto hash = page->replay().sha256;
    if (page->loadFile(QStringLiteral("this-file-does-not-exist.battle")) || page->replay().sha256 != hash) return 10;
    qInfo() << "replay UI navigation, backward seek and failed-load preservation PASS";
  }
  if (args.contains(QStringLiteral("--timeline-view"))) {
    const int index = args.indexOf(QStringLiteral("--timeline-view"));
    auto* selector = page->findChild<QComboBox*>(QStringLiteral("replayTimelineView"));
    const int choice = selector && args.size() > index + 1 ? selector->findData(args[index + 1]) : -1;
    if (choice < 0) return 59;
    selector->setCurrentIndex(choice);
  }
  if (args.contains(QStringLiteral("--timeline-scroll"))) {
    const int index = args.indexOf(QStringLiteral("--timeline-scroll"));
    auto* scroll = page->findChild<QScrollBar*>(QStringLiteral("replayUnitTimelineScroll"));
    bool ok = false;
    const int position = args.size() > index + 1 ? args[index + 1].toInt(&ok) : -1;
    if (!scroll || !ok || position < 0 || position > scroll->maximum()) return 59;
    scroll->setValue(position);
  }
  if (args.contains(QStringLiteral("--number-format"))) {
    const int index = args.indexOf(QStringLiteral("--number-format"));
    auto* selector = page->findChild<QComboBox*>(QStringLiteral("replayNumberFormat"));
    const int choice = selector && args.size() > index + 1 ? selector->findData(args[index + 1]) : -1;
    if (choice < 0) return 54;
    selector->setCurrentIndex(choice);
  }
  if (args.contains(QStringLiteral("--step"))) {
    const int index = args.indexOf(QStringLiteral("--step")); if (args.size() > index + 1) page->seek(args[index + 1].toInt());
  }
  if (args.contains(QStringLiteral("--moment"))) {
    const int index = args.indexOf(QStringLiteral("--moment")); if (args.size() > index + 1) page->seekMoment(args[index + 1].toInt());
  }
  if (args.contains(QStringLiteral("--effect"))) {
    const int index = args.indexOf(QStringLiteral("--effect"));
    if (args.size() > index + 1 && args[index + 1] == QStringLiteral("first-hit")) {
      const auto& effects = page->replay().steps[page->currentStep()].effects;
      for (int i = 0; i < effects.size(); ++i) if (!effects[i].source.isEmpty() && effects[i].kind == QStringLiteral("value") && effects[i].metric == QStringLiteral("生命") && effects[i].changed) { page->selectEffect(i); break; }
    } else if (args.size() > index + 1) page->selectEffect(args[index + 1].toInt());
  }
  if (args.contains(QStringLiteral("--stage"))) {
    const int index = args.indexOf(QStringLiteral("--stage")); bool ok = false;
    const int phase = args.size() > index + 1 ? args[index + 1].toInt(&ok) : -1;
    if (!ok || phase < 0 || phase >= page->playbackPhaseCount()) return 30;
    page->seekPlaybackPhase(phase);
  }
  if (args.contains(QStringLiteral("--buff-key")) && !args.contains(QStringLiteral("--buff-details"))) return 60;
  if (args.contains(QStringLiteral("--buff-details"))) {
    const int index = args.indexOf(QStringLiteral("--buff-details"));
    const auto mode = args.size() > index + 1 ? args[index + 1] : QString();
    if (mode != QStringLiteral("primary") && mode != QStringLiteral("unknown") && mode != QStringLiteral("playback") && mode != QStringLiteral("all")) return 60;
    QString key;
    if (args.contains(QStringLiteral("--buff-key"))) {
      const int keyIndex = args.indexOf(QStringLiteral("--buff-key"));
      if (args.size() <= keyIndex + 1 || args[keyIndex + 1].isEmpty()) return 60;
      key = args[keyIndex + 1];
    }
    bool matched = false;
    for (auto* card : page->findChildren<QFrame*>(QStringLiteral("replayUnitCard"))) {
      if (!key.isEmpty() && card->property("fighterKey").toString() != key) continue;
      matched = true;
      if (mode == QStringLiteral("primary") || mode == QStringLiteral("all"))
        if (auto* toggle = card->findChild<QToolButton*>(QStringLiteral("unitBuffToggle"))) toggle->setChecked(true);
      if (mode == QStringLiteral("unknown") || mode == QStringLiteral("all"))
        if (auto* toggle = card->findChild<QToolButton*>(QStringLiteral("unitBuffUnknownToggle")); toggle && !toggle->isHidden())
          toggle->setChecked(true);
      if (mode == QStringLiteral("playback") || mode == QStringLiteral("all"))
        if (auto* toggle = card->findChild<QToolButton*>(QStringLiteral("unitBuffPlaybackToggle")); toggle && !toggle->isHidden())
          toggle->setChecked(true);
    }
    if (!key.isEmpty()) {
      if (!matched) return 60;
      for (auto* cell : page->findChildren<ReplayCell*>()) if (cell->relationAnchor(key)) {
        cell->focusUnit(key); break;
      }
    }
  }
  for (const auto& option : {QStringLiteral("--reference-skill"), QStringLiteral("--reference-buff"), QStringLiteral("--reference-mechanics")}) {
    if (!args.contains(option)) continue;
    const bool buff = option == QStringLiteral("--reference-buff");
    const bool mechanics = option == QStringLiteral("--reference-mechanics");
    const int index = args.indexOf(option);
    auto* detail = page->findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
    if (args.size() <= index + 1 || !detail || page->displayedState().isEmpty()) return 21;
    auto key = page->displayedState().first().key;
    if (buff) for (const auto& fighter : page->displayedState())
      if (fighter.buffs.contains(args[index + 1])) { key = fighter.key; break; }
    if (mechanics) for (const auto& fighter : page->displayedState())
      if (fighter.raceId == args[index + 1]) { key = fighter.key; break; }
    QMetaObject::invokeMethod(detail, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("unit:") + QString::fromLatin1(QUrl::toPercentEncoding(key)))));
    QMetaObject::invokeMethod(detail, "anchorClicked", Q_ARG(QUrl, QUrl((buff ? QStringLiteral("buff:") : mechanics ? QStringLiteral("mechanics:") : QStringLiteral("skill:")) + args[index + 1])));
  }
  if (args.contains(QStringLiteral("--reference-library")) || args.contains(QStringLiteral("--reference-entry"))) {
    const bool entry = args.contains(QStringLiteral("--reference-entry"));
    const int index = args.indexOf(entry ? QStringLiteral("--reference-entry") : QStringLiteral("--reference-library"));
    if (args.size() <= index + 1) return 50;
    auto* detail = page->findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
    QMetaObject::invokeMethod(detail, "anchorClicked", Q_ARG(QUrl, QUrl((entry ? QStringLiteral("reference:") : QStringLiteral("reference-list:")) +
      QString::fromLatin1(QUrl::toPercentEncoding(args[index + 1])))));
  }
  if (args.contains(QStringLiteral("--capture-stages"))) {
    const int index = args.indexOf(QStringLiteral("--capture-stages"));
    if (args.size() <= index + 1 || page->playbackPhaseCount() <= 0) return 30;
    QDir destination(args[index + 1]);
    if (!destination.exists() && !QDir().mkpath(destination.absolutePath())) return 31;
    auto* arena = page->findChild<QWidget*>(QStringLiteral("replayArena"));
    auto* title = page->findChild<QLabel*>(QStringLiteral("replaySkillTitle"));
    if (!arena || !title) return 31;
    const int count = page->playbackPhaseCount(), step = page->currentStep();
    QJsonArray captures;
    for (int phase = 0; phase < count; ++phase) {
      page->seekPlaybackPhase(phase); QTest::qWait(80);
      const auto kind = page->playbackPhaseKind();
      const auto name = QStringLiteral("step-%1-stage-%2-%3.png").arg(step, 4, 10, QLatin1Char('0'))
        .arg(phase, 2, 10, QLatin1Char('0')).arg(kind);
      if (!arena->grab().save(destination.filePath(name))) return 31;
      captures.append(QJsonObject{{"stage", phase}, {"kind", kind}, {"image", name}, {"skill", title->accessibleName()},
        {"firstMoment", page->property("playbackFirstMoment").toInt()}, {"lastMoment", page->property("playbackLastMoment").toInt()},
        {"baseDurationMs", page->property("playbackBaseDurationMs").toInt()},
        {"actionProgress", page->property("playbackActionProgress").toReal()},
        {"displayedMoment", page->currentMoment()}});
    }
    QFile manifest(destination.filePath(QStringLiteral("stages.json")));
    if (!manifest.open(QIODevice::WriteOnly)) return 31;
    manifest.write(QJsonDocument(QJsonObject{{"sha256", page->replay().sha256}, {"step", step},
      {"action", page->replay().steps[step].title}, {"captures", captures}}).toJson(QJsonDocument::Indented));
    qInfo() << "captured playback stages" << count << destination.absolutePath();
    return 0;
  }
  if (args.contains(QStringLiteral("--capture")) || args.contains(QStringLiteral("--capture-board"))) {
    const bool boardOnly = args.contains(QStringLiteral("--capture-board"));
    const int index = args.indexOf(boardOnly ? QStringLiteral("--capture-board") : QStringLiteral("--capture")); if (args.size() <= index + 1) return 11;
    if (args.contains(QStringLiteral("--inspect-diagnostics"))) {
      auto* diagnostics = page->findChild<QToolButton*>(QStringLiteral("replayDiagnostics"));
      if (!diagnostics) return 43;
      QTimer::singleShot(0, diagnostics, &QToolButton::click);
    }
    QTest::qWait(150);
    if (args.contains(QStringLiteral("--layout-diagnostics"))) {
      qInfo() << "layout" << window.size() << page->size() << page->pos() << page->minimumSizeHint();
      for (const auto& name : {QStringLiteral("replayArena"), QStringLiteral("replayBoard"), QStringLiteral("replayFocusPanel"), QStringLiteral("replayCell2")})
        if (auto* widget = page->findChild<QWidget*>(name)) qInfo() << name << widget->geometry() << widget->minimumSizeHint() << widget->sizeHint();
    }
    auto* capture = boardOnly ? page->findChild<QWidget*>(QStringLiteral("replayArena")) : static_cast<QWidget*>(&window);
    if (!capture || !capture->grab().save(args[index + 1])) return 12;
    return 0;
  }
  if (verify) {
    QTimer::singleShot(0, &window, &QWidget::close);
    QTimer::singleShot(2000, &app, [&app] { app.exit(14); });
  }
  return app.exec();
}
