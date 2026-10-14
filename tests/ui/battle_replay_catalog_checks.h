#pragma once
#include <QSignalSpy>
#include "ui/battle/battle_replay_timeline_events.h"
#include "ui/detail/public_data_detail_dialog.h"
#include "ui/battle/battle_replay_identity_presentation.h"

inline BattleReplayCatalogHandle syntheticReplayCatalog(quint64 revision, const QString& name, int maximumPower) {
  auto root = battleReplayBattlefieldCatalog();
  auto pets = root[QStringLiteral("pets")].toObject();
  auto pet = pets[QStringLiteral("9001")].toObject();
  pet[QStringLiteral("name")] = name;
  pet[QStringLiteral("maxPower")] = maximumPower;
  pet[QStringLiteral("identity")] = QJsonObject{{QStringLiteral("status"), QStringLiteral("native")},
    {QStringLiteral("canonicalName"), name}};
  pets[QStringLiteral("9001")] = pet; root[QStringLiteral("pets")] = pets;
  auto value = std::make_shared<BattleReplayCatalogSnapshot>();
  value->root = root; value->revision = revision;
  value->sha256 = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(root).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
  value->sourceLabel = QStringLiteral("合成测试资料 %1").arg(revision);
  value->sourcePath = QStringLiteral("synthetic-catalog-%1.json").arg(revision);
  value->loadedAt = QDateTime::fromString(QStringLiteral("2026-10-10T00:00:00Z"), Qt::ISODate);
  return value;
}

inline bool verifyReplayCatalogSnapshots() {
  const auto fail = [](const char* reason) { qCritical() << "catalog snapshot:" << reason; return false; };
  QTemporaryFile battle, malformed;
  if (!battle.open() || !malformed.open()) return fail("cannot create synthetic files");
  const auto bytes = battleReplayBattlefieldFixture();
  battle.write(bytes); battle.flush(); malformed.write("{"); malformed.flush();
  const auto first = syntheticReplayCatalog(1, QStringLiteral("合成资料甲"), 100);
  const auto next = syntheticReplayCatalog(2, QStringLiteral("合成资料乙"), 180);
  {
    auto catalog=first->root; auto pets=catalog.value("pets").toObject();
    pets.insert("9100",QJsonObject{{"identity",QJsonObject{{"status","unresolved"}}},{"transFromRaceId",9001}});
    catalog.insert("pets",pets);
    BattleReplay::Fighter form; form.raceId="9100";
    if (replayFighterPublicDefinitionRace(form,catalog)!="9100") return fail("resource identity was guessed as an original pet link");
    form.originRaceId="9001"; form.originRelation="transform"; form.originFighterKey="recorded-body";
    if (replayFighterPublicDefinitionRace(form,catalog)!="9100") return fail("origin without recorded evidence became a definition link");
    form.originPath="/synthetic/action/transform";
    if (replayFighterPublicDefinitionRace(form,catalog)!="9001") return fail("verified transformation origin was not retained by detail navigation");
    form.raw=QJsonObject{{"oRaceId",9200}};
    if (replayFighterPublicDefinitionRace(form,catalog)!="9200") return fail("explicit original-race record was replaced by a model identity");
  }

  // Before a battle is opened, the reference browser follows the latest
  // catalog. An old open detail must not remain visible after it changes.
  {
    BattleReplayPage libraryOnly(nullptr, first); libraryOnly.resize(1440, 1000);
    auto* library = libraryOnly.findChild<QToolButton*>(QStringLiteral("replayReferences"));
    auto* categories = libraryOnly.findChild<QComboBox*>(QStringLiteral("replayReferenceCategory"));
    auto* query = libraryOnly.findChild<QLineEdit*>(QStringLiteral("replayReferenceSearch"));
    auto* shown = libraryOnly.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
    auto* records = libraryOnly.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
    if (!library || !categories || !query || !shown || !records) return fail("missing unloaded reference controls");
    library->click(); categories->setCurrentIndex(categories->findData(QStringLiteral("pets")));
    query->setText(QStringLiteral("合成资料甲"));
    const auto oldDetail = shown->toPlainText();
    if (!oldDetail.contains(QStringLiteral("合成资料甲"))) return fail("unloaded reference query did not use initial snapshot");
    libraryOnly.setCatalogState(first, QStringLiteral("正在读取本地资料"), true);
    if (shown->toPlainText() != oldDetail) return fail("progress notification cleared current reference detail");
    libraryOnly.setCatalogState(next, {}, false);
    if (!shown->toPlainText().isEmpty() || !records->toPlainText().isEmpty()) return fail("unloaded catalog change retained old reference detail");
    query->setText(QStringLiteral("合成资料乙"));
    if (records->property("referenceMatchCount").toInt() != 1 || !shown->toPlainText().contains(QStringLiteral("合成资料乙")))
      return fail("unloaded reference query did not switch catalogs");
  }

  // No fallback resource read is allowed inside the page. An unavailable
  // snapshot must block parsing instead of silently producing unknown names.
  BattleReplayPage page;
  page.resize(1440, 1000); page.show(); QTest::qWait(10);
  auto* status = page.findChild<QLabel*>(QStringLiteral("replayCatalogStatus"));
  auto* importStatus = page.findChild<QLabel*>(QStringLiteral("replayStatus"));
  auto* reload = page.findChild<QToolButton*>(QStringLiteral("replayCatalogReload"));
  auto* configuration = page.findChild<QToolButton*>(QStringLiteral("replayConfiguration"));
  auto* references = page.findChild<QToolButton*>(QStringLiteral("replayReferences"));
  auto* category = page.findChild<QComboBox*>(QStringLiteral("replayReferenceCategory"));
  auto* search = page.findChild<QLineEdit*>(QStringLiteral("replayReferenceSearch"));
  auto* details = page.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
  auto* unit = page.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
  if (!status || !importStatus || !reload || !configuration || !references || !category || !search || !details || !unit)
    return fail("missing catalog controls");
  if (page.loadFile(battle.fileName()) || !page.replay().steps.isEmpty() || !status->text().contains(QStringLiteral("未就绪")) ||
      !importStatus->text().contains(QStringLiteral("资料未就绪")) || references->isEnabled() || !configuration->isEnabled())
    return fail("unready page silently parsed a replay");
  page.setCatalogState({}, QStringLiteral("合成读取失败"), false);
  if (page.loadFile(battle.fileName()) || !page.replay().steps.isEmpty()) return fail("failed first load enabled parsing");

  page.setCatalogState(first, QStringLiteral("已读取合成资料"), false);
  if (!page.loadFile(battle.fileName())) return fail("ready catalog did not unblock import");
  const auto key = BattleReplay::fighterKey(0, QStringLiteral("tester"), QStringLiteral("7"));
  const auto matches = [&](const QString& name, int maximum) {
    const auto fighter = page.displayedState().value(key);
    return BattleReplay::fighterName(fighter) == name && fighter.maxPower == maximum;
  };
  if (!matches(QStringLiteral("合成资料甲"), 100)) return fail("first snapshot was not used by parser");
  const auto hash = page.replay().sha256;
  references->click(); category->setCurrentIndex(category->findData(QStringLiteral("pets")));
  search->setText(QStringLiteral("合成资料甲"));
  if (unit->property("referenceMatchCount").toInt() != 1 || !details->toPlainText().contains(QStringLiteral("合成资料甲")))
    return fail("cannot prime reference cache with original catalog");

  QSignalSpy requested(&page, &BattleReplayPage::catalogReloadRequested);
  if (!requested.isValid()) return fail("reload signal unavailable");
  reload->click();
  if (requested.size() != 1 || page.replay().sha256 != hash) return fail("local reload did not emit exactly one request");
  auto* play = page.findChild<QPushButton*>(QStringLiteral("replayPlay"));
  if (!play) return fail("missing playback button");
  play->click();
  const int runningStep = page.currentStep(), runningMoment = page.currentMoment(), runningPhase = page.currentPlaybackPhase();
  if (!page.property("playbackRunning").toBool()) return fail("catalog update test did not start playback");
  page.setCatalogState({}, QStringLiteral("正在读取本地资料"), true);
  if (reload->isEnabled() || !status->text().contains(QStringLiteral("正在读取")) || !matches(QStringLiteral("合成资料甲"), 100))
    return fail("loading cleared or changed active replay");
  reload->click(); if (requested.size() != 1) return fail("busy reload accepted duplicate request");
  page.setCatalogState(next, QStringLiteral("新合成资料已读取"), false);
  if (!page.property("playbackRunning").toBool() || page.currentStep() != runningStep ||
      page.currentMoment() != runningMoment || page.currentPlaybackPhase() != runningPhase)
    return fail("catalog progress/publication interrupted or repositioned continuous playback");
  page.seek(2); page.seek(0);
  if (!reload->isEnabled() || !matches(QStringLiteral("合成资料甲"), 100) || page.replay().sha256 != hash ||
      page.replay().original != bytes || !status->text().contains(QStringLiteral("下次打开战报")))
    return fail("published catalog changed existing replay or backwards navigation");
  configuration->click();
  if (!details->toPlainText().contains(first->sourceLabel) || !details->toPlainText().contains(next->sourceLabel) ||
      !details->toPlainText().contains(first->sha256) || !details->toPlainText().contains(next->sha256))
    return fail("configuration does not distinguish pinned and next catalogs");

  // A malformed new log must not publish its candidate catalog to the old
  // replay. A subsequent catalog failure must retain the latest usable one.
  if (page.loadFile(malformed.fileName()) || !matches(QStringLiteral("合成资料甲"), 100) || page.replay().sha256 != hash)
    return fail("malformed log replaced pinned catalog");
  page.setCatalogState({}, QStringLiteral("合成读取失败；已有资料继续使用"), false);
  if (!matches(QStringLiteral("合成资料甲"), 100) || page.replay().original != bytes || !status->text().contains(QStringLiteral("读取失败")))
    return fail("failed catalog reload cleared original replay");
  if (!page.loadFile(battle.fileName()) || !matches(QStringLiteral("合成资料乙"), 180))
    return fail("new import did not retain latest usable snapshot after reload failure");
  if (status->text().contains(QStringLiteral("下次打开战报"))) return fail("new import still reports an unapplied snapshot");

  // Keep the same category selected and change only the user's search. This
  // catches both stale row caches and stale searchable-text caches.
  search->setText(QStringLiteral("合成资料乙"));
  if (unit->property("referenceMatchCount").toInt() != 1 || !details->toPlainText().contains(QStringLiteral("合成资料乙")) ||
      details->toPlainText().contains(QStringLiteral("合成资料甲")))
    return fail("new import retained reference rows or search cache from old catalog");
  page.seek(2); const int oldStep = page.currentStep();
  page.setCatalogState(std::make_shared<BattleReplayCatalogSnapshot>(), QStringLiteral("合成空资料被拒绝"), false);
  if (page.currentStep() != oldStep || page.replay().sha256 != hash) return fail("empty snapshot reset replay position");
  if (!page.loadFile(battle.fileName()) || !matches(QStringLiteral("合成资料乙"), 180)) return fail("empty snapshot replaced latest usable catalog");
  {
    BattleReplayPage opened(nullptr, first); opened.resize(1440,1000); opened.show(); QTest::qWait(10);
    if (!opened.loadFile(battle.fileName())) return fail("reinterpret fixture did not open");
    int action = -1;
    for (int i = 0; i < opened.replay().steps.size(); ++i)
      if (ReplayTimeline::isAction(opened.replay().steps[i])) { action = i; break; }
    if (action < 0) return fail("reinterpret fixture has no stable action");
    opened.seek(action); opened.seekMoment(opened.replay().steps[action].moments.size()-1);
    const auto path = opened.replay().steps[action].path;
    auto* format = opened.findChild<QComboBox*>(QStringLiteral("replayNumberFormat"));
    const int numberFormat = format ? format->currentIndex() : -1;
    auto* apply = opened.findChild<QToolButton*>(QStringLiteral("replayCatalogApply"));
    opened.setCatalogState(next,{},true);
    if (!apply || apply->isEnabled() || opened.useUpdatedCatalog()) return fail("busy catalog allowed reinterpretation");
    opened.setCatalogState(next,{},false);
    // Opening a definition uses this battle's own Flash snapshot and has no
    // effect on action position, expanded Buffs or the existing inspector.
    auto* inspector=opened.findChild<QTextBrowser*>(QStringLiteral("replayUnit"));
    auto* buffToggle=opened.findChild<QToolButton*>(QStringLiteral("unitBuffToggle"));
    if (buffToggle && !buffToggle->isChecked()) buffToggle->click();
    const int pinnedStep=opened.currentStep(), pinnedMoment=opened.currentMoment(), pinnedPhase=opened.currentPlaybackPhase();
    const QString inspectorBefore=inspector ? inspector->toHtml() : QString();
    const bool expanded=buffToggle && buffToggle->isChecked();
    if (!inspector) return fail("missing public detail source inspector");
    QMetaObject::invokeMethod(inspector,"anchorClicked",Q_ARG(QUrl,PublicDataDetailLinks::url(
      {PublicDataViewKind::BattleReplay,QStringLiteral("pets"),QStringLiteral("9001")})));
    auto* publicDetail=opened.findChild<PublicDataDetailDialog*>(QStringLiteral("KQPublicDataDetail"));
    if (!publicDetail || publicDetail->currentDetail().title!=QStringLiteral("合成资料甲") ||
        publicDetail->currentDetail().revision!=first->sha256 || opened.currentStep()!=pinnedStep ||
        opened.currentMoment()!=pinnedMoment || opened.currentPlaybackPhase()!=pinnedPhase ||
        inspector->toHtml()!=inspectorBefore || (buffToggle && buffToggle->isChecked()!=expanded))
      return fail("common detail borrowed latest catalog or reset replay progress/folds/inspector");
    auto* overlay=opened.findChild<QTextBrowser*>(QStringLiteral("replayBoardDetails"));
    if (!overlay) return fail("missing board detail overlay");
    overlay->setHtml(QStringLiteral("<p>保留阅读位置</p>").repeated(80));
    overlay->verticalScrollBar()->setValue(140);
    const auto overlayHtml=overlay->toHtml(); const int overlayScroll=overlay->verticalScrollBar()->value();
    QMetaObject::invokeMethod(overlay,"anchorClicked",Q_ARG(QUrl,PublicDataDetailLinks::url(
      {PublicDataViewKind::BattleReplay,QStringLiteral("pets"),QStringLiteral("9001")})));
    if (overlay->toHtml()!=overlayHtml || overlay->verticalScrollBar()->value()!=overlayScroll ||
        publicDetail->currentDetail().revision!=first->sha256 || opened.currentMoment()!=pinnedMoment)
      return fail("common detail navigation replaced the board overlay or reset its reading position");
    publicDetail->close();
    QTest::qWait(10);
    auto* technical = opened.findChild<QToolButton*>(QStringLiteral("replayTechnicalMenu"));
    if (!apply->isVisible() || !technical || apply->geometry().intersects(technical->geometry()))
      return fail("reinterpret control is hidden or covers an existing toolbar control");
    opened.grab().save(QDir::current().filePath(QStringLiteral("replay-updated-data-preview.png")));
    if (!apply->isEnabled() || !opened.useUpdatedCatalog()) return fail("explicit reinterpretation did not apply ready catalog");
    if (opened.replay().original != bytes || opened.replay().sha256 != hash || opened.currentStep() != action ||
        opened.replay().steps[opened.currentStep()].path != path || opened.currentPlaybackPhase() != 0 ||
        opened.property("playbackRunning").toBool() || (format && format->currentIndex() != numberFormat) ||
        BattleReplay::fighterName(opened.displayedState().value(key)) != QStringLiteral("合成资料乙"))
      return fail("reinterpretation reused internal frame state, lost original bytes or changed playback settings");
    if (opened.useUpdatedCatalog()) return fail("same catalog unnecessarily reinterpreted a replay");
  }
  return true;
}
