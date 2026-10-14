#pragma once

#include <QTextBlock>
#include "ui/battle/battle_replay_buff_presentation.h"

// Synthetic UI scenarios using the embedded official definitions. They verify
// readable presentation and provenance, never infer a server damage formula.
inline QWidget* elevenOverlayFor(QWidget& host, const QString& key = {}) {
  for (auto* overlay : host.findChildren<QWidget*>(QStringLiteral("unitOutcomeOverlay")))
    if (key.isEmpty() || overlay->property("fighterKey").toString() == key) return overlay;
  return nullptr;
}

inline void saveElevenCapture(QWidget& widget, const QString& name) {
  const auto directory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
  if (directory.isEmpty()) return;
  QDir().mkpath(directory);
  widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

inline bool verifyElevenCellEffects(const QJsonObject& catalog);
inline bool verifySpiritPresentationBranches();

inline bool verifyElevenStateTransitions(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "eleven state transitions:" << reason; return false; };
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object(); auto br = root["br"].toObject();
  auto original = br["or"].toObject(); auto teams = original["team"].toArray();
  auto own = teams[0].toObject(); auto players = own["players"].toArray(); auto player = players[0].toObject();
  auto fighters = player["cts"].toArray(); auto fighter = fighters[0].toObject();
  fighter["raceId"] = 7412; fighter["oRaceId"] = 7412; fighter["faceId"] = 7412;
  fighter["nskill"] = 106986; fighter["uskills"] = 401393; fighter["suskill"] = 401393; fighter["attr"] = "27";
  fighters[0] = fighter; player["cts"] = fighters; players[0] = player; own["players"] = players; teams[0] = own;
  original["team"] = teams; br["or"] = original;
  const auto action = [](const QJsonObject& record, int round) {
    return QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"ses", 1}, {"csn", round},
      {"cn", QJsonObject{{"rl", QJsonArray{record}}}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}};
  };
  const QJsonObject onlySkill{{"t", 1}, {"o", 1}, {"changeset", 1}, {"or", 7412}, {"of", 7412},
    {"cr", 7412}, {"cf", 7412}, {"cn", 106853}, {"cu", "401393"}, {"scu", 401393}, {"ca", "27"}};
  auto form = onlySkill; form["cr"] = 7413; form["cf"] = 7413; form["ca"] = "24";
  br["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", QJsonArray{
    action(onlySkill, 1), action(form, 2), action(QJsonObject{{"t", 6}, {"o", 1}, {"changeset", 1}, {"ca", "23"}}, 3)}}}};
  root["br"] = br; const auto bytes = QJsonDocument(root).toJson();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); if (!page.loadFile(file.fileName())) return false;
  page.show(); QTest::qWait(20);
  const auto source = BattleReplay::fighterKey(0, "tester", "7");
  const auto oldSkill = catalog["skills"].toObject()["106986"].toObject()["name"].toString();
  const auto newSkill = catalog["skills"].toObject()["106853"].toObject()["name"].toString();
  const auto oldCanonical = catalog["pets"].toObject()["7412"].toObject()["identity"].toObject()["canonicalName"].toString();
  const auto newCanonical = catalog["pets"].toObject()["7413"].toObject()["identity"].toObject()["canonicalName"].toString();
  if (oldSkill.isEmpty() || newSkill.isEmpty()) return fail("required official skill definitions missing");
  QStringList saw;
  for (int step = 0; step < page.replay().steps.size(); ++step) {
    const auto& recorded = page.replay().steps[step];
    if (!recorded.raw.toObject().contains("si")) continue;
    page.seek(step);
    for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
      page.seekPlaybackPhase(phase); QCoreApplication::processEvents();
      auto* overlay = elevenOverlayFor(page, source);
      const auto kinds = overlay ? overlay->property("effectKinds").toStringList() : QStringList{};
      const auto labels = overlay ? overlay->property("effectLabels").toStringList().join(" | ") : QString();
      if (overlay && QStringList{"skill-swap", "transform", "attributes"}.contains(overlay->property("currentEffectKind").toString())) {
        overlay->grab(); // Force the actual painted/elided strings to be checked.
        if (!overlay->property("paintedEffectLines").toStringList().join(" ").contains(QChar(0x2192)))
          return fail("a concrete before/after transition lost its arrow in the visible text");
      }
      if (page.playbackPhaseKind() == QStringLiteral("prepare") &&
          (kinds.contains("skill-swap") || kinds.contains("transform") || kinds.contains("attributes")))
        return fail("prepare revealed a future state transition");
      if (recorded.round == 1) {
        if (kinds.contains("transform")) return fail("a pure skill replacement was mislabeled as transformation");
        if (kinds.contains("skill-swap")) {
          if (!labels.contains(oldSkill) || !labels.contains(newSkill) || !labels.contains(QChar(0x2192)))
            return fail("skill replacement omitted concrete old/new official skill names");
          saw.append("skill");
          if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"))) saveElevenCapture(*board, "eleven-skill-change");
        }
      }
      if (recorded.round == 2 && kinds.contains("transform")) {
        if (oldCanonical.isEmpty() || newCanonical.isEmpty() || !labels.contains(QChar(0x2192)) ||
            !labels.contains(oldCanonical) || !labels.contains(newCanonical) || labels.contains(QStringLiteral("永夜君王")))
          return fail("transformation omitted canonical before/after names");
        saw.append("form");
        if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"))) saveElevenCapture(*board, "eleven-form-change");
      }
      if (recorded.round == 3 && kinds.contains("attributes")) {
        if (!labels.contains(QStringLiteral("神暗")) || !labels.contains(QStringLiteral("神火")) || !labels.contains(QChar(0x2192)))
          return fail("attribute change omitted the concrete before/after attributes");
        saw.append("attribute");
        if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"))) saveElevenCapture(*board, "eleven-attribute-change");
      }
      const auto moment = page.property("playbackLastMoment").toInt();
      if (moment >= 0 && !replayStateMatches(page, recorded.moments[moment].state)) return fail("cue changed the recorded state");
    }
  }
  if (!saw.contains("skill") || !saw.contains("form") || !saw.contains("attribute")) return fail("missing a typed transition");
  page.seek(2); page.seekPlaybackPhase(0);
  auto* oldOverlay = elevenOverlayFor(page, source);
  if (oldOverlay && oldOverlay->property("effectKinds").toStringList().contains("skill-swap")) return fail("rewind retained future replacement");
  if (page.replay().original != bytes) return fail("presentation changed the report bytes");

  // Synthetic input, consumed through the official source-number branch:
  // its recorded 100 -> 0 -> 100 chain must be readable in separate segments.
  const auto chainBytes = realtimeMultiUnitFixture();
  QTemporaryFile chain; if (!chain.open()) return false; chain.write(chainBytes); chain.flush();
  if (!page.loadFile(chain.fileName())) return false;
  const auto enemy = BattleReplay::fighterKey(1, "tester", "7");
  bool checkedFallen = false, checkedRecovery = false; int fallenPhase = -1;
  for (int step = 0; step < page.replay().steps.size(); ++step) {
    if (page.replay().steps[step].round != 2 || !page.replay().steps[step].raw.toObject().contains("si")) continue;
    page.seek(step);
    for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
      page.seekPlaybackPhase(phase); QCoreApplication::processEvents();
      auto* overlay = elevenOverlayFor(page, enemy); if (!overlay) continue;
      const auto kinds = overlay->property("effectKinds").toStringList();
      const bool fallen = kinds.contains("fallen"), recovery = kinds.contains("recovery");
      if (!fallen && !recovery) continue;
      if (fallen && recovery) return fail("separate life-chain segments exposed both death and future recovery");
      for (auto* cell : page.findChildren<ReplayCell*>()) cell->setPlaybackProgress(1);
      ReplayCell* card = nullptr;
      for (auto* cell : page.findChildren<ReplayCell*>())
        if (cell->property("visibleFighterKey").toString() == enemy) { card = cell; break; }
      QProgressBar* hp = nullptr;
      for (auto* bar : page.findChildren<QProgressBar*>(QStringLiteral("hpBar")))
        if (bar->property("fighterKey").toString() == enemy) { hp = bar; break; }
      auto* numbers = page.findChild<QWidget*>(QStringLiteral("replayMeterOverlay"));
      if (!card || !hp || !numbers) return fail("life-chain card or meter missing");
      const qint64 expectedHp = fallen ? 0 : 100;
      const QString expectedKind = fallen ? QStringLiteral("fallen") : QStringLiteral("recovery");
      const QString expectedDelta = fallen ? QStringLiteral("-100") : QStringLiteral("+100");
      if (page.displayedState()[enemy].hp != expectedHp || hp->format() != QString::number(expectedHp) + "/100" ||
          overlay->property("currentEffectKind").toString() != expectedKind || card->property("deadUnit").toBool() != fallen ||
          card->property("selectedUnitDead").toBool() != fallen ||
          (fallen && (!card->graphicsEffect() || !card->graphicsEffect()->isEnabled())))
        return fail("life-chain balance, card dimming or settled cue contradicted the current recorded segment");
      int ownNumbers = 0;
      for (const auto& entry : numbers->property("recordedChanges").toList()) {
        const auto row = entry.toMap();
        if (row["fighterKey"].toString() != enemy || row["metric"].toString() != QStringLiteral("生命")) continue;
        if (row["text"].toString() != expectedDelta || row["critical"].toBool())
          return fail("life-chain segment retained replaced damage, exposed future recovery or invented a critical self-cost");
        ++ownNumbers;
      }
      const auto bands = hp->property("actionGhostSegments").toList();
      if (ownNumbers != 1 || bands.size() != 1 || bands[0].toMap()["before"].toLongLong() != (fallen ? 100 : 0) ||
          bands[0].toMap()["after"].toLongLong() != expectedHp ||
          bands[0].toMap()["kind"].toString() != (fallen ? "remove" : "add"))
        return fail("life-chain segment did not retain exactly its own number and pending interval");
      if (fallen) { checkedFallen = true; fallenPhase = phase; }
      else {
        if (!checkedFallen || phase <= fallenPhase) return fail("recovery appeared before the recorded fallen segment");
        checkedRecovery = true;
      }
      if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard")))
        saveElevenCapture(*board, fallen ? "eleven-chain-fallen-synthetic" : "eleven-chain-recovery-synthetic");
    }
  }
  if (page.replay().original != chainBytes) return fail("life-chain presentation changed the synthetic report");
  return (checkedFallen && checkedRecovery) || fail("both recorded life-chain segments were not reached");
}

inline QByteArray elevenOpeningFixture() {
  const auto unit = [](int id, int race, const QString& attribute, int x, bool empowered) {
    return QJsonObject{{"id", id}, {"raceId", race}, {"oRaceId", race}, {"faceId", race},
      {"x", x}, {"y", 0}, {"hp", 100}, {"maxHp", 100}, {"shp", 0}, {"vigour", 100},
      {"attr", attribute}, {"ibat", empowered}};
  };
  const QJsonObject own{{"id", 0}, {"players", QJsonArray{QJsonObject{{"name", "opening-evidence"}, {"cts", QJsonArray{
    unit(7, 7412, "27", 0, false), unit(8, 7413, "27", 1, true), unit(9, 7390, "23", 2, false)}}}}}};
  const QJsonObject enemy{{"id", 1}, {"players", QJsonArray{QJsonObject{{"name", "other"},
    {"cts", QJsonArray{unit(7, 7413, "24", 0, false)}}}}}};
  return QJsonDocument(QJsonObject{{"br", QJsonObject{{"or", QJsonObject{{"team", QJsonArray{own, enemy}}}}, {"fr", QJsonArray{}}}}}).toJson();
}

inline bool verifyOpeningEmpowermentPresentation(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "opening presentation:" << reason; return false; };
  const auto bytes = elevenOpeningFixture();
  QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); if (!page.loadFile(file.fileName())) return false;
  page.show(); QTest::qWait(20); page.seek(0); QCoreApplication::processEvents();
  const auto source = BattleReplay::fighterKey(0, "opening-evidence", "7"), target = BattleReplay::fighterKey(0, "opening-evidence", "8"),
    self = BattleReplay::fighterKey(0, "opening-evidence", "9");
  const auto evidence = BattleReplay::identifyEmpowerments(page.displayedState(), catalog);
  const auto transforms = BattleReplay::identifySelfTransforms(page.displayedState(), catalog);
  if (evidence.size() != 1 || !evidence.contains(target) || transforms.size() != 1 || !transforms.contains(self))
    return fail("synthetic opening is not supported by strict official evidence");
  auto* targetOverlay = elevenOverlayFor(page, target); auto* selfOverlay = elevenOverlayFor(page, self);
  if (!targetOverlay || !selfOverlay || !targetOverlay->property("effectKinds").toStringList().contains("empowerment") ||
      !selfOverlay->property("effectKinds").toStringList().contains("self-transform")) return fail("confirmed opening relation not shown in recipient cells");
  const auto selfLabels = selfOverlay->property("effectLabels").toStringList().join(" | ");
  if (!selfLabels.contains(QStringLiteral("神灵")) || !selfLabels.contains(QStringLiteral("神火")))
    return fail("self transform did not show native and initial attributes");
  auto* links = page.findChild<QWidget*>(QStringLiteral("replayRelationLinks")); if (!links) return false;
  bool linked = false;
  for (const auto& row : links->property("relationPresentations").toList()) {
    const auto relation = row.toMap();
    if (relation["sourceKey"].toString() == source && relation["targetKey"].toString() == target) {
      linked = relation["color"].toString() == "green" && relation["labels"].toStringList().join(" ").contains(QStringLiteral("元能"));
    }
  }
  if (!linked) return fail("confirmed giver/recipient opening arrow missing or wrong allegiance");
  if (auto* board = page.findChild<QWidget*>(QStringLiteral("replayBoard"))) saveElevenCapture(*board, "eleven-opening-evidence");
  if (page.replay().original != bytes) return fail("opening cue mutated original report");
  return true;
}

inline bool verifyGenericBuffAdditions(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "generic Buff additions:" << reason; return false; };
  const auto definitions = catalog["buffs"].toObject();
  const auto anonymous = definitions["300111"].toObject(), named = definitions["5014"].toObject();
  if (anonymous.isEmpty() || !anonymous["nm"].toString().isEmpty() || !anonymous["beffect"].toString().isEmpty() ||
      anonymous["hd"].toVariant().toString() != "1" || named["nm"].toString().isEmpty() || !named["beffect"].toString().isEmpty() ||
      definitions.contains("987654321")) return fail("official anonymous/named/missing examples changed");
  BattleReplay::Fighter fighter; fighter.key = "generic Buff recipient"; fighter.name = QStringLiteral("新增记录测试精灵");
  fighter.team = 1; fighter.entered = true; fighter.hp = 80; fighter.maxHp = 100;
  fighter.shield = 20; fighter.power = 90; fighter.maxPower = 100;
  ReplayCell cell(1); cell.resize(220, 174); cell.setPlaybackDriven(true); cell.show();
  ReplayUnitOverview cue; cue.visualBadge = QStringLiteral("被暴击"); cue.animate = false;
  struct Case { QString id, applier; int team; QString origin, caption; };
  const QList<Case> cases{
    {"300111", {}, 0, QStringLiteral("来源未记"), QStringLiteral("未命名效果")},
    {"5014", "recorded-own-giver", 0, QStringLiteral("我方施加"), named["nm"].toString()},
    {"987654321", "recorded-enemy-giver", 1, QStringLiteral("敌方施加"), QStringLiteral("效果名称未收录")}};
  QPair<QString, QString> activatedBuff;
  QObject::connect(&cell, &ReplayCell::buffActivated, &cell, [&](const QString& key, const QString& id) { activatedBuff = qMakePair(key, id); });
  for (const auto& sample : cases) {
    ReplayUnitEvent event; event.index = 11; event.buffId = sample.id;
    event.buffSource = sample.applier; event.buffSourceTeam = sample.team;
    event.label = QStringLiteral("Buff [%1] /synthetic/private/effect").arg(sample.id);
    event.detail = QStringLiteral("原始 Buff [%1] /synthetic/private/source").arg(sample.id);
    cue.events = {event}; fighter.buffs = {sample.id};
    cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
    QCoreApplication::processEvents(); cell.setPlaybackProgress(1);
    auto* overlay = elevenOverlayFor(cell, fighter.key);
    if (!overlay) return fail("acquisition disappeared behind a simultaneous judgement");
    overlay->grab();
    const auto kinds = overlay->property("effectKinds").toStringList();
    const int acquisition = kinds.indexOf("buff-add");
    if (!replayBuffHasExplanation(definitions[sample.id].toObject())) {
      // Unknown registrations remain inspectable without becoming a fabricated
      // prominent state effect or obscuring the explicit critical judgement.
      auto* members = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
      auto* unknown = cell.findChild<QToolButton*>(QStringLiteral("unitBuffUnknownToggle"));
      if (acquisition >= 0 || kinds != QStringList{"critical"} || !members || !unknown || unknown->isHidden() ||
          members->property("memberCount").toInt() != 1 || members->property("unknownExpanded").toBool())
        return fail("unknown registration hid the judgement, disappeared, or became a state animation");
      unknown->click();
      if (!members->property("unknownExpanded").toBool() || members->toPlainText().contains(sample.id) ||
          !members->toPlainText().contains(sample.caption) || !members->toPlainText().contains(sample.origin) ||
          members->accessibleName().contains(sample.id) || members->toolTip().contains(sample.id))
        return fail("unknown record leaked an identifier or lost its plain name/provenance");
      QString memberLink;
      for (auto block = members->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
          const auto href = it.fragment().charFormat().anchorHref();
          if (href.startsWith(QStringLiteral("buff:") + sample.id + QLatin1Char('#'))) memberLink = href;
        }
      const auto rawEvidence = QJsonDocument::fromVariant(members->property("rawEvidence")).toJson();
      if (memberLink.isEmpty() || !rawEvidence.contains(sample.id.toUtf8()) ||
          !QMetaObject::invokeMethod(members, "anchorClicked", Q_ARG(QUrl, QUrl(memberLink))) ||
          activatedBuff != qMakePair(fighter.key, sample.id))
        return fail("plain unknown label lost the exact internal identity or detail action");
      unknown->click();
      if (sample.id == "300111") {
        auto* playback = cell.findChild<QToolButton*>(QStringLiteral("unitBuffPlaybackToggle"));
        if (!playback || playback->isHidden()) return fail("hidden-movie setting is unavailable");
        playback->click();
        if (!members->toPlainText().contains(QStringLiteral("隐藏附着动画")) || members->property("unknownExpanded").toBool())
          return fail("playback settings changed unknown membership or lost their exact field meaning");
        playback->click();
        saveElevenCapture(cell, "anonymous-buff-record-critical");
      }
      continue;
    }
    if (acquisition < 0 || kinds.size() != 2 || !kinds.contains("critical"))
      return fail("plain acquisition missing or unnamed effect was interpreted as a status");
    const auto labels = overlay->property("effectLabels").toStringList();
    if (labels.value(acquisition).contains(sample.id) || !labels.value(acquisition).contains(sample.caption) ||
        overlay->property("effectBuffIds").toStringList().value(acquisition) != sample.id ||
        !QJsonDocument::fromVariant(overlay->property("rawEvidence")).toJson().contains(sample.id.toUtf8()) ||
        overlay->accessibleName().contains(sample.id) || overlay->toolTip().contains(sample.id) ||
        overlay->property("paintedEffectLines").toStringList().join(" ").contains(sample.id))
      return fail("acquisition leaked an ID in normal text or lost the underlying reference");
    if ((!sample.applier.isEmpty() && (overlay->accessibleName().contains(sample.applier) || overlay->toolTip().contains(sample.applier))) ||
        overlay->toolTip().contains(QStringLiteral("/synthetic/")))
      return fail("missing public source name fell back to an internal fighter key/path");
    if (overlay->property("effectOriginLabels").toStringList().value(acquisition) != sample.origin ||
        overlay->property("effectSources").toStringList().value(acquisition) != "recorded-buff")
      return fail("an applier was inferred from the holder/team or a recorded addition became a derived effect");
    const auto visible = overlay->property("visibleEffectKinds").toStringList();
    if (!visible.contains("critical") || !visible.contains("buff-add")) return fail("small cell still hides acquisition behind critical");
    auto* members = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
    if (!members || members->property("shownGroupCount").toInt() || members->property("memberCount").toInt() != 1)
      return fail("transient cue expanded or changed current membership");
  }
  // Keeping the member in the next phase must not replay its acquisition.
  cue.events.clear();
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
  const auto* later = elevenOverlayFor(cell, fighter.key);
  if (!later || later->property("effectKinds").toStringList() != QStringList{"critical"})
    return fail("a previous or persistent member replayed the generic acquisition");
  return true;
}

inline bool verifyElevenCellEffects(const QJsonObject& catalog) {
  if (!verifyGenericBuffAdditions(catalog)) return false;
  const auto fail = [](const char* reason) { qCritical() << "eleven cell effects:" << reason; return false; };
  BattleReplay::Fighter fighter; fighter.key = "synthetic eleven-effect recipient"; fighter.name = QStringLiteral("合成效果测试精灵");
  fighter.team = 1; fighter.entered = true; fighter.hp = 80; fighter.maxHp = 100;
  fighter.shield = 20; fighter.power = 90; fighter.maxPower = 100;
  ReplayCell cell(1); cell.resize(260, 236); cell.setPlaybackDriven(true); cell.show();
  ReplayUnitOverview cue; cue.visualBadge = QString(); cue.animate = true;
  const auto draw = [&]() -> QWidget* {
    cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 0);
    QCoreApplication::processEvents(); cell.setPlaybackProgress(.6);
    return elevenOverlayFor(cell, fighter.key);
  };
  const QList<QPair<QString, QString>> states{{"1", "status-burn"}, {"2", "status-frost"}, {"3", "status-poison"},
    {"4", "status-restraint"}, {"5", "status-blind"}, {"6", "status-physical-seal"}, {"8", "status-stun"}, {"9", "status-paralysis"}};
  for (const auto& state : states) {
    fighter.buffs = {state.first}; ReplayUnitEvent event; event.index = 1; event.buffId = state.first;
    event.buffSource = "recorded-own-giver"; event.buffSourceName = QStringLiteral("已记录施加者"); event.buffSourceTeam = 0;
    cue.events = {event}; auto* overlay = draw();
    if (!overlay || !overlay->property("effectKinds").toStringList().contains(state.second))
      return fail(qPrintable(QStringLiteral("missing official abnormal-state cue %1").arg(state.first)));
    const auto kinds = overlay->property("effectKinds").toStringList();
    if (kinds.contains("miss") || kinds.contains("critical") || kinds.contains("harm-source"))
      return fail("Buff acquisition invented a hit judgement or an actual damage event");
    if (!overlay->property("effectOriginLabels").toStringList().join(" ").contains(QStringLiteral("我方施加")))
      return fail("Buff cue omitted the independently recorded applier allegiance");
    if (state.first == "8") saveElevenCapture(cell, "eleven-stun");
  }
  // Matching an ID/name without the corroborating official display definition
  // is insufficient. This intentionally altered catalog is a negative test.
  auto mismatched = catalog; auto definitions = mismatched["buffs"].toObject(); auto definition = definitions["4"].toObject();
  definition["did"] = "999999"; definitions["4"] = definition; mismatched["buffs"] = definitions;
  cue.events[0].buffId = "4";
  cell.display(1, {fighter}, mismatched, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}});
  if (auto* overlay = elevenOverlayFor(cell); overlay && overlay->property("effectKinds").toStringList().contains("status-restraint"))
    return fail("unverified abnormal-state mapping guessed from its name");

  ReplayUnitEvent own; own.index = 1; own.buffId = "8"; own.buffSource = "own-giver"; own.buffSourceName = QStringLiteral("甲"); own.buffSourceTeam = 0;
  auto enemy = own; enemy.index = 2; enemy.buffSource = "enemy-giver"; enemy.buffSourceName = QStringLiteral("乙"); enemy.buffSourceTeam = 1;
  auto unknown = own; unknown.index = 3; unknown.buffSource.clear(); unknown.buffSourceName.clear(); unknown.buffSourceTeam = -1;
  fighter.buffs = {"8", "8", "8"}; cue.events = {own, enemy, unknown}; auto* overlay = draw();
  if (!overlay || overlay->property("effectKinds").toStringList().count("status-stun") != 3)
    return fail("effect merging erased distinct recorded or unknown appliers");
  const auto origins = overlay->property("effectOriginLabels").toStringList();
  for (const auto& origin : {QStringLiteral("我方施加"), QStringLiteral("敌方施加"), QStringLiteral("来源未记")})
    if (!origins.join(" ").contains(origin)) return fail("effect origin category omitted");
  cell.display(1, {fighter}, catalog, {}, {}, {}, {}, {}, {}, {{fighter.key, cue}}, 1); QCoreApplication::processEvents();
  overlay = elevenOverlayFor(cell);
  const auto appliers = overlay->property("effectAppliers").toStringList(), reversed = overlay->property("effectOriginLabels").toStringList();
  if (appliers.size() != reversed.size() || appliers.indexOf("own-giver") < 0 || appliers.indexOf("enemy-giver") < 0 ||
      !reversed[appliers.indexOf("own-giver")].contains(QStringLiteral("敌方施加")) ||
      !reversed[appliers.indexOf("enemy-giver")].contains(QStringLiteral("我方施加"))) return fail("perspective reassigned rather than relabeled a Buff applier");
  saveElevenCapture(cell, "eleven-buff-origins");

  // Removing one member leaves the other intact; a trigger never adds one.
  own.removing = true; enemy.buffTriggered = true; fighter.buffs = {"8"}; cue.events = {own, enemy}; overlay = draw();
  if (!overlay || !overlay->property("effectKinds").toStringList().contains("buff-remove") ||
      !overlay->property("effectKinds").toStringList().contains("buff-trigger") ||
      overlay->property("effectLabels").toStringList().join(" ").contains(QStringLiteral("全部解除")))
    return fail("remove/trigger graphics missing or overstated");
  auto* members = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  if (!members || members->property("memberCount").toInt() != 1) return fail("visual Buff operations changed current membership");
  saveElevenCapture(cell, "eleven-buff-remove-trigger");

  QSet<QString> harmSymbols;
  for (const auto& harm : {QStringLiteral("烧伤"), QStringLiteral("冻伤"), QStringLiteral("中毒"), QStringLiteral("吸血"), QStringLiteral("反弹")}) {
    ReplayUnitEvent numeric; numeric.index = 4; numeric.metric = QStringLiteral("生命"); numeric.valueText = "-5"; numeric.harmLabel = harm;
    cue.events = {numeric}; overlay = draw();
    if (!overlay || overlay->property("effectKinds").toStringList() != QStringList{"harm-source"}) return fail("recorded harm source missing or invented extra events");
    const auto symbols = overlay->property("effectSymbols").toStringList();
    if (symbols.size() != 1 || symbols.first().isEmpty()) return fail("harm source has no distinct symbol");
    harmSymbols.insert(symbols.first());
  }
  if (harmSymbols.size() != 5) return fail("fire/ice/poison/leech/reflect still share one pictogram");
  cue.events.clear(); overlay = draw();
  if (overlay && !overlay->property("effectKinds").toStringList().isEmpty()) return fail("no-event phase replayed a harm source or persistent Buff");

  for (const auto& special : QList<QPair<QString, QString>>{{"21", "war-soul"}, {"2676", "indomitable-will"},
      {"3719", "mark"}, {"3735", "mark"}, {"100011", "potential"}}) {
    ReplayUnitEvent event; event.index = 5; event.buffId = special.first; cue.events = {event}; fighter.buffs = {special.first}; overlay = draw();
    if (!overlay || !overlay->property("effectKinds").toStringList().contains(special.second)) return fail("special Buff lacks a dedicated graphic");
    if (special.first == "3735" && !overlay->property("effectLabels").toStringList().join(" ").contains(QStringLiteral("*2")))
      return fail("official mark count was replaced by current member count");
  }
  cue.events.clear(); cue.clientBadge = QStringLiteral("神职攻击 · 神职受击 · 天启"); overlay = draw();
  for (const auto& kind : {"god-job-attack", "god-job-defend", "tianqi"})
    if (!overlay || !overlay->property("effectKinds").toStringList().contains(QLatin1String(kind))) return fail("client god-job/TianQi distinctions missing");
  saveElevenCapture(cell, "eleven-god-job-tianqi");

  cue.clientBadge.clear(); fighter.power = 100; cue.powerBefore = 90; cue.maxPowerBefore = 100;
  ReplayUnitEvent power; power.index = 6; power.metric = QStringLiteral("气势"); power.valueText = "+10"; power.observedBefore = 90; power.observedAfter = 100;
  cue.events = {power}; overlay = draw();
  if (!overlay || !overlay->property("effectKinds").toStringList().contains("power-full")) return fail("known full-power crossing lacks a prominent cue");
  auto* powerBar = cell.findChild<QProgressBar*>(QStringLiteral("powerBar"));
  if (!powerBar || !powerBar->text().contains("100/100") || !powerBar->accessibleName().contains("+10")) return fail("full-power graphic hid or altered exact values");
  saveElevenCapture(cell, "eleven-power-full");
  cue.events.clear(); cue.powerBefore = 100; overlay = draw();
  if (overlay && overlay->property("effectKinds").toStringList().contains("power-full")) return fail("already-full power repeated the crossing effect");
  fighter.maxPower.reset(); cue.powerBefore = 90; cue.maxPowerBefore.reset(); cue.events = {power}; overlay = draw();
  if (overlay && overlay->property("effectKinds").toStringList().contains("power-full")) return fail("unknown full threshold was guessed");

  // Retain a judgement, final life state and abnormal-state chip together in
  // the small cell that previously showed only a single large result + 1/N.
  fighter.maxPower = 100; fighter.power = 90; fighter.hp = 0; cue = {}; cue.animate = true;
  cue.visualBadge = QStringLiteral("被暴击 · 生命归零"); own.removing = false; cue.events = {own};
  cell.resize(220, 174); overlay = draw();
  if (!overlay) return fail("crowded-cell outcomes absent");
  const auto visible = overlay->property("visibleEffectKinds").toStringList();
  for (const auto& kind : {"critical", "fallen", "status-stun"})
    if (!visible.contains(QLatin1String(kind))) return fail("crowded cell still hides a critical category behind cycling");
  saveElevenCapture(cell, "eleven-crowded-critical");
  auto* toggle = cell.findChild<QToolButton*>(QStringLiteral("unitBuffToggle")); if (!toggle) return false; toggle->click();
  QCoreApplication::processEvents();
  if (!overlay->isVisible() || !overlay->property("compact").toBool()) return fail("expanded Buff list conceals ongoing outcomes");
  members = cell.findChild<QTextBrowser*>(QStringLiteral("unitBuffs"));
  if (!members || !members->property("expanded").toBool()) return fail("compact result prevents Buff expansion");
  saveElevenCapture(cell, "eleven-expanded-outcomes");
  return true;
}

inline bool verifySpiritPresentationBranches() {
  const auto fail = [](const char* reason) { qCritical() << "spirit branch presentation:" << reason; return false; };
  struct Case { int id, before, after, delta; std::optional<bool> effected; const char* kind; bool nullFlag = false; bool multiple = false; };
  const QList<Case> cases{
    {220483, 2, 9, 4, false, "charge"},  // vc is authoritative; not after-before.
    {220483, 5, 3, -2, false, "drain"},
    {220483, 14, 0, -14, true, "release"},
    {220483, 14, 0, -14, false, "neutral"},
    {220483, 5, 3, -2, std::nullopt, "drain"}, // verified official default for absent isse.
    {77, 5, 3, -2, false, "neutral"},   // no matching official execution config.
    {220483, 3, 3, 0, true, "release"},
    {220483, 5, 3, -2, std::nullopt, "neutral", true},
    {220483, 6, 3, -3, false, "drain", false, true}};
  auto root = QJsonDocument::fromJson(battleReplayStateFixture()).object(); auto br = root["br"].toObject();
  auto original = br["or"].toObject(); auto teams = original["team"].toArray(); auto own = teams[0].toObject();
  auto players = own["players"].toArray(); auto player = players[0].toObject();
  player["ppbf"] = QJsonObject{{"pbid", 220483}, {"cp", 2}, {"mp", 14}};
  players[0] = player; own["players"] = players; teams[0] = own; original["team"] = teams; br["or"] = original;
  QJsonArray commands;
  for (int i = 0; i < cases.size(); ++i) {
    const auto& sample = cases[i];
    QJsonObject progress{{"t", 4}, {"o", 2}, {"changeset", 1}, {"pbid", sample.id}, {"prob", sample.before},
      {"proa", sample.after}, {"vc", sample.delta}, {"mp", 14}};
    if (sample.effected) progress["isse"] = *sample.effected;
    if (sample.nullFlag) progress["isse"] = QJsonValue(QJsonValue::Null);
    QJsonArray changes;
    if (sample.multiple) changes.append(QJsonObject{{"t", 4}, {"o", 2}, {"changeset", 1}, {"pbid", sample.id},
      {"prob", 2}, {"proa", 6}, {"vc", 4}, {"mp", 14}, {"isse", false}});
    changes.append(progress);
    commands.append(QJsonObject{{"cti", 0}, {"cpn", "tester"}, {"cci", 7}, {"si", 106986}, {"ses", 1}, {"csn", i + 1},
      {"cn", QJsonObject{{"rl", changes}}}, {"trl", QJsonArray{}}, {"prl", QJsonArray{}}});
  }
  br["fr"] = QJsonArray{QJsonObject{{"pan", "tester"}, {"pbn", "tester"}, {"rl", commands}}}; root["br"] = br;
  const auto bytes = QJsonDocument(root).toJson(); QTemporaryFile file; if (!file.open()) return false; file.write(bytes); file.flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1280, 940); if (!page.loadFile(file.fileName())) return false;
  page.show(); QTest::qWait(20); QSet<int> checked;
  auto* spirit = page.findChild<ReplaySpiritBar*>(QStringLiteral("replaySpirit0")); if (!spirit) return false;
  for (int step = 0; step < page.replay().steps.size(); ++step) {
    const auto& recorded = page.replay().steps[step];
    if (!recorded.raw.toObject().contains("si")) continue;
    const int sampleIndex = recorded.round - 1; if (sampleIndex < 0 || sampleIndex >= cases.size()) return false;
    const auto& sample = cases[sampleIndex]; page.seek(step);
    for (int phase = 0; phase < page.playbackPhaseCount(); ++phase) {
      page.seekPlaybackPhase(phase); QCoreApplication::processEvents();
      const auto kind = spirit->property("transitionKind").toString();
      if (page.playbackPhaseKind() == QStringLiteral("prepare")) {
        if (!kind.isEmpty()) return fail("prepare replayed a future or stale progress effect");
        continue;
      }
      if (kind.isEmpty()) continue;
      if (kind != QLatin1String(sample.kind)) {
        qCritical() << sampleIndex << "expected" << sample.kind << "actual" << kind;
        return fail("progress branch violates official effect conditions");
      }
      const auto exact = QStringLiteral("%1/14").arg(sample.after), delta = (sample.delta >= 0 ? "+" : "") + QString::number(sample.delta);
      if (!spirit->toPlainText().contains(exact) || !spirit->property("transitionText").toString().contains(delta))
        return fail("branch rendering changed the raw progress balance or vc");
      if (!spirit->property("transitionEvidence").toString().contains("pbid")) return fail("branch provenance omitted raw record");
      if (sample.multiple) {
        const auto visibleKinds = spirit->property("visibleTransitionKinds").toStringList();
        const auto visibleDeltas = spirit->property("visibleTransitionDeltas").toStringList().join(" / ").replace(QChar(0x2212), QLatin1Char('-'));
        if (!visibleKinds.contains("charge") || !visibleKinds.contains("drain") || !visibleDeltas.contains("+4") || !visibleDeltas.contains("-3"))
          return fail("one progress operation concealed another in the same phase");
        saveElevenCapture(*spirit, "eleven-spirit-multiple");
      }
      checked.insert(sampleIndex);
      if (sampleIndex < 3) saveElevenCapture(*spirit, QStringLiteral("eleven-spirit-%1").arg(kind));
    }
  }
  if (checked.size() != cases.size() || page.replay().original != bytes) return fail("progress cases omitted or report mutated");
  page.seek(0);
  if (!spirit->property("transitionKind").toString().isEmpty()) return fail("rewind retained a future drain/release cue");
  // The decoration uses the one shared playback clock and keeps exact values
  // available even when the width is between two recorded balances.
  ReplaySpiritBar direct; direct.resize(480, 46); direct.setPlaybackDriven(true); direct.show();
  direct.setProgress(3, 14, QStringLiteral("合成通灵表现测试")); direct.setTransition(5, -2, QStringLiteral("通灵扣除"), true);
  direct.setTransitionKind("drain", QStringLiteral("合成已记录分支"));
  direct.setPlaybackProgress(.15); const auto early = direct.grab().toImage(); direct.setPlaybackProgress(.7); const auto later = direct.grab().toImage();
  if (early == later || !direct.toPlainText().contains("3/14") || !direct.toPlainText().contains("-2")) return fail("drain decoration stationary or numerical display invented interpolation");
  QTest::qWait(30); if (direct.grab().toImage() != later) return fail("paused shared clock did not pause progress decoration");
  return true;
}
