#pragma once
#include "ui/battle/battle_replay_reference_library.h"
#include "ui/battle/battle_replay_buff_history.h"
#include <QLineEdit>
#include <memory>

inline bool verifyReplayBuffHistory() {
  using namespace BattleReplay;
  Replay replay;
  const QString key = fighterKey(0, "test", "7"), other = fighterKey(1, "test", "7");
  Fighter fighter; fighter.key = key; fighter.buffs = {"123", "123"}; fighter.buffPath = "/initial/bfs";
  Step initial; initial.state.insert(key, fighter); replay.steps.append(initial);
  Step step; step.round = 1;
  Effect removal; removal.kind = "buff"; removal.cause = "buff"; removal.causeId = "123";
  removal.target = key; removal.raw = "0_123_1"; removal.before = 0; removal.after = 0; removal.changed = false; removal.moment = 1;
  step.effects.append(removal);
  auto numeric = removal; numeric.kind = "value"; numeric.metric = QStringLiteral("生命"); numeric.before = 100; numeric.after = 100; numeric.moment = 2;
  step.effects.append(numeric);
  auto added = removal; added.raw = "1_123_1"; added.moment = 3; added.buffSource = other; added.buffSourceTeam = 1;
  step.effects.append(added);
  auto crossTeam = added; crossTeam.target = other; step.effects.append(crossTeam);
  auto differentId = added; differentId.causeId = "124"; step.effects.append(differentId);
  auto skillSameId = added; skillSameId.kind = "value"; skillSameId.cause = "skill"; step.effects.append(skillSameId);
  replay.steps.append(step);
  replay.steps.append(step);
  const auto partial = ReplayBuffHistory::recorded(replay, key, "123", 1, 2);
  if (partial.size() != 3 || !partial[0].initial || partial[0].effect.after != 2 || partial[1].effect.changed ||
      partial[2].effect.kind != "value") return false;
  const auto complete = ReplayBuffHistory::recorded(replay, key, "123", 1, -1);
  if (complete.size() != 4 || complete.last().effect.buffSource != other) return false;
  if (ReplayBuffHistory::recorded(replay, key, "123", 0, -1).size() != 1 ||
      ReplayBuffHistory::recorded(replay, key, "123", 1, 0).size() != 1) return false;
  if (ReplayBuffHistory::observedIds(replay, key, 1, 2).contains("124") ||
      !ReplayBuffHistory::observedIds(replay, key, 1, -1).contains("124")) return false;
  replay.steps[1].batchId = replay.steps[2].batchId = 3; replay.steps[2].parallelBatch = true;
  if (ReplayBuffHistory::recorded(replay, key, "123", 2, 0).size() != 1) return false;
  qInfo() << "Buff history retains zero-change, removed and numeric observations; no future, cross-team or same-ID skill leakage PASS";
  return true;
}

inline bool verifyReplayReferenceLibrary(const QJsonObject& catalog) {
  const auto fail = [](const char* reason) { qCritical() << "official reference library:" << reason; return false; };
  if (!verifyReplayBuffHistory()) return fail("Buff history observation boundary");
  const auto anonymousDetail = ReplayReference::detail(catalog, "buffs", "300113");
  if (!anonymousDetail.contains(QStringLiteral("官方 Buff 配置已收录")) || !anonymousDetail.contains(QStringLiteral("官方隐藏")) ||
      !anonymousDetail.contains(QStringLiteral("名称"))) return fail("anonymous Buff configuration/readability distinction");
  if (!ReplayReference::availability("skills", QJsonObject{{"name", "sample"}, {"playbackAvailable", false}}).contains(QStringLiteral("未取得独立播放配置")))
    return fail("description-only skill advertised as playable definition");
  if (replayBuffAvailability({}).contains(QStringLiteral("已收录"))) return fail("absent Buff advertised as unnamed definition");
  const auto consumption = catalog["petMechanics"].toObject()["4055"].toObject();
  if (consumption["referenceConsumption"].toArray().isEmpty() ||
      !ReplayReference::detail(catalog, "pets", "4055").contains(QStringLiteral("官方如何读取")) ||
      !ReplayReference::detail(catalog, "pets", "4055", true).contains(QStringLiteral("consumerSources")) ||
      ReplayReference::consumptionSources(catalog, consumption).isEmpty())
    return fail("verified official consumption conditions or source chain inaccessible");
  const auto supplements = catalog["replaySupplements"].toObject();
  if (supplements["carry"].toObject()["unresolvedExpansion"].toArray().size() ||
      supplements["carry"].toObject()["rows"].toArray().isEmpty() ||
      supplements["hero"].toObject()["rows"].toArray().isEmpty()) return fail("missing official supplement rows or carry expansion");
  // The same numeric identifier in another namespace is not an equipment or
  // applied-Buff relationship. Face IDs, not battle race IDs, select the aura.
  const QJsonObject fullPowerGroup{
    {"byRaceId", QJsonObject{{"20", 0}}}, {"rows", QJsonArray{QJsonObject{{"buffId", "10005"}, {"raceId", "20"}}}},
    {"raceIdOverrides", QJsonObject{{"30", "10003"}}}, {"defaultBuffId", "10000"}};
  const QJsonObject titleGroup{{"byId", QJsonObject{{"7", 0}}}, {"rows", QJsonArray{QJsonObject{{"titleId", "7"}}}}};
  QJsonObject synthetic{{"replaySupplements", QJsonObject{{"powerFull", fullPowerGroup}, {"titles", titleGroup}}}};
  if (ReplayReference::powerFull(synthetic, "20")["buffId"].toString() != "10005" ||
      ReplayReference::powerFull(synthetic, "30")["buffId"].toString() != "10003" ||
      ReplayReference::powerFull(synthetic, "999")["buffId"].toString() != "10000") return fail("face/override/dictionary/default lookup");
  const auto titles = ReplayReference::recordedTitles(synthetic, QJsonArray{
    QJsonObject{{"titleId", 7}, {"content", "old-content"}}, QJsonObject{{"titleId", 7}, {"content", "current-content"}},
    QJsonObject{{"titleId", 8}, {"content", "<script>unsafe</script>"}}});
  if (titles.contains("old-content") || !titles.contains("current-content") || titles.contains("<script>"))
    return fail("title duplicate handling or escaped reference content");
  const auto resources = ReplayReference::rows(catalog, "resources");
  if (!resources.contains("actionMovie/config") || !resources.contains("changeAvatar/config"))
    return fail("empty and runtime-excluded resource tables are inaccessible");
  int excluded = 0;
  for (const auto& value : resources) if (value.toObject().contains("runtimeEligible") && !value.toObject()["runtimeEligible"].toBool()) ++excluded;
  if (!excluded) return fail("excluded official changeAvatar rows lost");
  if (!ReplayReference::detail(catalog, "legends", "902642").contains(QStringLiteral("20%概率眩晕")) ||
      !ReplayReference::searchable("legends", "902642", ReplayReference::rows(catalog, "legends")["902642"].toObject()).contains(QStringLiteral("眩晕")))
    return fail("legend descX or variant-description search missing");
  if (!ReplayReference::searchable("coverage", "0", QJsonObject{{"name", "placeholder"}, {"id", "123456"}}).contains("123456"))
    return fail("array-indexed reference ID is not searchable");
  if (!ReplayReference::name("pets", "123", QJsonObject{{"name", QStringLiteral("皮肤名称")}, {"identity", QJsonObject{{"status", "unresolved"}}}}).contains(QStringLiteral("待核实")))
    return fail("unresolved identity shown as a verified original name");
  QJsonObject relationGroups;
  relationGroups["carry"] = QJsonObject{{"rows", QJsonArray{QJsonObject{{"sourceRaceIds", QJsonArray{"1"}}, {"targetRaceIds", QJsonArray{"2"}}}}}};
  relationGroups["formationTeams"] = QJsonObject{{"rows", QJsonArray{QJsonObject{{"name", "combination"}, {"petAlternatives", QJsonArray{QJsonValue(QJsonArray{"2", "3"})}}}}}};
  const QJsonObject relationFixture{{"replaySupplements", relationGroups}};
  const auto targetContext = ReplayReference::context(relationFixture, "2");
  if (!targetContext.contains("carry%2F0") || !targetContext.contains("formationTeams%2F0")) {
    qCritical().noquote() << targetContext;
    return fail("carried-target or formation-member references are inaccessible");
  }
  auto file = std::make_unique<QTemporaryFile>();
  if (!file->open()) return false; file->write(battleReplayFixture()); file->flush();
  BattleReplayPage page(nullptr, previewBattleReplayCatalog()); page.resize(1440, 940); if (!page.loadFile(file->fileName())) return false;
  page.show(); QTest::qWait(20);
  auto* button = page.findChild<QToolButton*>("replayReferences");
  auto* browser = page.findChild<QTextBrowser*>("replayBoardDetails");
  auto* unit = page.findChild<QTextBrowser*>("replayUnit");
  auto* category = page.findChild<QComboBox*>("replayReferenceCategory");
  auto* search = page.findChild<QLineEdit*>("replayReferenceSearch");
  auto* inspector = page.findChild<QFrame*>("replayBoardInspector");
  if (!button || !browser || !unit || !category || !search || !inspector) return fail("missing board reference controls");
  QTest::mouseClick(button, Qt::LeftButton);
  if (!inspector->isVisible() || !browser->toPlainText().contains(QStringLiteral("召唤配对"))) return fail("board library entry");
  category->setCurrentIndex(category->findData(QStringLiteral("stargods")));
  const auto stars = ReplayReference::rows(catalog, "stargods");
  if (unit->property("referenceMatchCount").toInt() != stars.size() || stars.size() < 26) return fail("full stargod index");
  QSet<QString> reached;
  const int pageCount = (stars.size() + 24) / 25;
  for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
    if (pageIndex && (!clickReplayTextAnchor(browser, QStringLiteral("reference-page:%1").arg(pageIndex)) ||
                     unit->property("referencePage").toInt() != pageIndex)) return fail("pagination cannot reach last page");
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto href = it.fragment().charFormat().anchorHref();
        if (!href.startsWith(QStringLiteral("reference:"))) continue;
        const auto payload = QUrl::fromPercentEncoding(href.mid(10).toLatin1());
        if (payload.startsWith(QStringLiteral("stargods/"))) reached.insert(payload.mid(9));
      }
  }
  const auto expectedIds = stars.keys();
  if (reached != QSet<QString>(expectedIds.begin(), expectedIds.end())) return fail("pagination dropped or repeated definition keys");
  QString example;
  for (auto it = stars.begin(); it != stars.end(); ++it) if (it.value().toObject()["levels"].toObject().size() > 1) { example = it.key(); break; }
  if (example.isEmpty()) return fail("official level data absent");
  search->setText(ReplayReference::name("stargods", example, stars[example].toObject()));
  const auto href = QStringLiteral("reference:") + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("stargods/") + example));
  if (!clickReplayTextAnchor(browser, href)) return fail("name search or detail link");
  const auto details = browser->toPlainText();
  const auto levels = stars[example].toObject()["levels"].toObject();
  for (auto it = levels.begin(); it != levels.end(); ++it)
    if (!details.contains(QStringLiteral("等级 %1").arg(it.key()))) return fail("reference level was truncated");
  if (!details.contains(QStringLiteral("战报未记录")) && !details.contains(QStringLiteral("本场养成快照未记录")))
    return fail("equipment boundary missing");
  if (browser->toHtml().contains("href=\"buff:")) return fail("cultivation IDs misjoined as applied Buffs");
  const auto rawHref = QStringLiteral("reference-raw:") + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("stargods/") + example));
  if (!clickReplayTextAnchor(browser, rawHref) || !browser->toPlainText().contains(QStringLiteral("sha256")))
    return fail("complete original fields or source not accessible");
  QMetaObject::invokeMethod(unit, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("reference-list:coverage"))));
  if (unit->property("referenceMatchCount").toInt() != catalog["referenceCoverage"].toObject()["items"].toArray().size())
    return fail("unresolved references were dropped");
  const auto* arena = page.findChild<QWidget*>("replayArena");
  if (!arena || !arena->rect().contains(inspector->geometry())) return fail("reference inspector outside battle board");
  saveElevenCapture(page, QStringLiteral("official-reference-library"));
  auto historyFixture = battleReplayFixture();
  // This board loads the real catalog, so use an existing ordinary playback
  // definition. The surrounding report remains explicitly test-only data.
  historyFixture.replace("999999999", "100001");
  historyFixture.replace("\"bfs\":\"99,99\"", "\"bfs\":\"300113,300113\"");
  historyFixture.replace("99_-20_1", "300113_-20_1");
  historyFixture.replace("1_99_2", "1_300113_2,0_300113_3");
  auto historyFile = std::make_unique<QTemporaryFile>();
  if (!historyFile->open()) return false; historyFile->write(historyFixture); historyFile->flush();
  if (!page.loadFile(historyFile->fileName())) return fail("Buff history fixture load");
  const auto parsedHistory = BattleReplay::parse(historyFixture, catalog);
  int historyStep = -1;
  for (int n = 0; n < parsedHistory.steps.size(); ++n)
    if (std::any_of(parsedHistory.steps[n].effects.begin(), parsedHistory.steps[n].effects.end(),
        [](const BattleReplay::Effect& effect) { return effect.causeId == QStringLiteral("300113"); })) { historyStep = n; break; }
  if (historyStep < 0) return fail("history fixture has no recorded action");
  page.seek(historyStep);
  const auto cells = page.findChildren<ReplayCell*>();
  if (cells.isEmpty()) return false;
  const auto historyKey = BattleReplay::fighterKey(1, "tester", "7");
  QMetaObject::invokeMethod(cells.first(), "buffActivated", Q_ARG(QString, historyKey), Q_ARG(QString, QStringLiteral("300113")));
  if (unit->property("buffHistoryCount").toInt() != 3 || !browser->toPlainText().contains(QStringLiteral("移除一条")) ||
      !browser->toPlainText().contains(QStringLiteral("生命 200 → 180")) || browser->toHtml().contains("<pre>")) {
    qCritical().noquote() << "history entries" << unit->property("buffHistoryCount") << browser->toPlainText();
    return fail("anonymous removed Buff history not readable on board");
  }
  saveElevenCapture(page, QStringLiteral("anonymous-buff-history"));
  QMetaObject::invokeMethod(unit, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("unit:") + QString::fromLatin1(QUrl::toPercentEncoding(historyKey)))));
  if (!unit->toPlainText().contains(QStringLiteral("此前出现的效果")) ||
      !unit->toHtml().contains(QStringLiteral("buff:300113")) || unit->toPlainText().contains(QStringLiteral("300113")))
    return fail("removed-only history lost its precise link or leaked its technical ID");
  QMetaObject::invokeMethod(cells.first(), "buffActivated", Q_ARG(QString, historyKey), Q_ARG(QString, QStringLiteral("300113")));
  QString historyAnchor;
  for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const auto href = it.fragment().charFormat().anchorHref();
      if (href.startsWith(QStringLiteral("buff-record:"))) historyAnchor = href;
    }
  if (historyAnchor.isEmpty() || !clickReplayTextAnchor(browser, historyAnchor) || page.currentMoment() < 0 || inspector->isVisible())
    return fail("history does not seek and expose board");
  qInfo() << "official reference library, source navigation, full levels and namespace boundaries PASS";
  return true;
}
