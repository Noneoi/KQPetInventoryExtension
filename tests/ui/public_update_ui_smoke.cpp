#include "ui/workbench/pet_settings_dialog.h"
#include "ui/workbench/public_update_panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTest>
#include <cstdio>

namespace {
PublicUpdateState sample() {
  PublicUpdateState state;
  state.outcome = PublicUpdateOutcome::Partial;
  state.summary = QStringLiteral("精灵、技能和回放资料已应用。公共图标未完成，继续使用原图标，可重试。");
  state.sessionApplied = state.diskCommitted = state.candidatePassed = true;
  state.canRestorePrevious = true;
  state.latestAttempt = QString(32, 'a'); state.diskRevision = state.sessionRevision = QString(64, 'b');
  state.checkedUtc = QStringLiteral("2026-10-14T08:30:00.000Z"); state.retryComponents = {QStringLiteral("icons")};
  for (const auto& key : {"pets", "skills", "battleReplay", "shop", "images", "icons", "routines"}) {
    PublicUpdateComponentState part; part.key = QString::fromLatin1(key);
    part.status = part.key == "icons" ? QStringLiteral("retained") : QStringLiteral("updated");
    part.retryable = part.hasWarnings = part.key == "icons";
    part.message = part.retryable ? QStringLiteral("这组图标获取未完成，原图标仍可用。") : QStringLiteral("已通过检查并应用。");
    state.components.append(part);
  }
  PublicUpdateChange change;
  change.component = "skills"; change.kind = "changed"; change.name = QStringLiteral("测试技能说明");
  change.beforeName = change.afterName = change.name;
  change.beforeText = QStringLiteral("测试用旧说明"); change.afterText = QStringLiteral("测试用新说明");
  change.technical = {{"id", "987654321"}, {"namespace", "fixture-only"}};
  state.changes.append(change); state.totalChanges = 1; state.changesRecorded = true;
  state.technical = {{"attempt", state.latestAttempt}, {"revision", state.diskRevision}};
  return state;
}
}
int main(int argc, char** argv) {
  QApplication application(argc, argv); bool good = true;
  const auto require = [&](bool ok, const char* text) { if (!ok) { good = false; std::fprintf(stderr, "FAIL: %s\n", text); } };
  auto state = sample();
  PetSettingsDialog settings(RefreshTimings{});
  settings.setWindowTitle(QStringLiteral("第三轮更新页 · 测试资料预览"));
  settings.setPublicUpdateState(state);
  settings.findChild<QTabWidget*>(QStringLiteral("KQSettingsTabs"))->setCurrentIndex(1);
  settings.show(); QTest::qWait(15);
#ifdef KQPET_UPDATE_UI_PREVIEW
  return application.exec();
#endif
  if (application.arguments().contains(QStringLiteral("--interactive"))) return application.exec();
  auto* panel = settings.findChild<PublicUpdatePanel*>();
  auto* title = panel->findChild<QLabel*>("KQPublicUpdateTitle");
  auto* main = panel->findChild<QPushButton*>("KQCheckDataUpdate");
  auto* retry = panel->findChild<QPushButton*>("KQRetryPublicData");
  auto* rollback = panel->findChild<QPushButton*>("KQRestorePublicData");
  auto* reload = panel->findChild<QPushButton*>("KQReloadPublicData");
  auto* advanced = panel->findChild<QPushButton*>("KQAdvancedPublicData");
  auto* part = panel->findChild<QPushButton*>("KQDataUpdate-battleReplay");
  require(title->text().contains(QStringLiteral("部分")) && retry->isVisible() && rollback->isEnabled() &&
      !reload->isVisible() && !part->isVisible(), "partial result or collapsed controls incorrect");
  for (auto& component : state.components) component.status = QStringLiteral("unchanged");
  state.totalChanges = 0; state.changesRecorded = true;
  settings.setPublicUpdateState(state);
  require(!title->text().contains(QStringLiteral("已更新")) && title->text().contains(QStringLiteral("需留意")),
      "existing coverage warning falsely claimed unchanged data was updated");
  state = sample(); settings.setPublicUpdateState(state);
  for (const auto* label : panel->findChildren<QLabel*>()) if (label->isVisible())
    require(!label->text().contains(state.diskRevision) && !label->text().contains("987654321"), "default view exposes technical identifiers");
  settings.setDataUpdateStatus(QStringLiteral("无法开始，请先完成当前缓存任务。"), false);
  auto* notice = panel->findChild<QLabel*>("KQPublicUpdateNotice");
  require(notice->isVisible() && notice->text().contains(QStringLiteral("无法开始")) &&
      title->text().contains(QStringLiteral("部分")) && main->isEnabled(), "rejected operation hid feedback or replaced durable result");
  settings.setPublicUpdateState(state); require(!notice->isVisible(), "new receipt retained stale operation feedback");
  int retries = 0; QObject::connect(&settings, &PetSettingsDialog::publicDataRetryRequested, &settings, [&] { ++retries; });
  retry->click(); require(retries == 1 && !main->isEnabled(), "retry did not disable duplicate actions");
  settings.setPublicUpdateState(state);
  advanced->click(); require(part->isVisible() && part->toolTip().contains(QStringLiteral("关联")), "advanced scope does not explain dependencies");
  panel->findChild<QPushButton*>("KQPublicDataChanges")->click(); QTest::qWait(10);
  auto* list = panel->findChild<QListWidget*>("KQPublicChangeList");
  auto* description = panel->findChild<QTextBrowser*>("KQPublicChangeText");
  require(list && list->count() == 1 && description->toPlainText().contains(QStringLiteral("测试用旧说明")) &&
      description->toPlainText().contains(QStringLiteral("测试用新说明")) && !description->toPlainText().contains("987654321"), "named old/new change explanation missing");
  state.outcome = PublicUpdateOutcome::Checking; state.busy = state.canCancel = true;
  state.progress.phase = PublicUpdatePhase::FetchingResources;
  state.progress.completed = 3; state.progress.total = 9;
  settings.setPublicUpdateState(state);
  auto* progress = panel->findChild<QProgressBar*>("KQPublicUpdateProgress");
  require(progress->minimum() == 0 && progress->maximum() == 9 && progress->value() == 3 && !main->isEnabled(), "progress invented or ignored measured counts");
  state.progress.completed = state.progress.total = -1;
  state.canCancel = false; state.progress.phase = PublicUpdatePhase::Applying;
  settings.setPublicUpdateState(state);
  require(progress->maximum() == 0 && !panel->findChild<QPushButton*>("KQCancelPublicData")->isEnabled(), "unknown total shows percent or commit is cancellable");
  state = sample(); state.outcome = PublicUpdateOutcome::PendingApply; state.sessionApplied = false;
  settings.setPublicUpdateState(state);
  require(reload->isVisible() && title->text().contains(QStringLiteral("等待载入")), "committed-only result claims applied");
  for (const auto outcome : {PublicUpdateOutcome::Unchanged, PublicUpdateOutcome::Applied, PublicUpdateOutcome::KeptPrevious,
                            PublicUpdateOutcome::Cancelled, PublicUpdateOutcome::Restored}) {
    state = sample(); state.outcome = outcome; settings.setPublicUpdateState(state);
    require(!title->text().isEmpty() && !progress->isVisible() && main->isEnabled(), "terminal outcome remains busy");
  }
  settings.setImageBatchProgress(2, 5, 1);
  require(!main->isEnabled() && !rollback->isEnabled(), "image batch allowed conflicting data mutation");
  settings.finishImageBatch(false, 1); require(main->isEnabled(), "image failure blocks valid core result");
  state = sample(); settings.setPublicUpdateState(state); settings.hide();
  PetSettingsDialog reopened(RefreshTimings{}); reopened.setPublicUpdateState(state);
  reopened.findChild<QTabWidget*>("KQSettingsTabs")->setCurrentIndex(1); reopened.show(); QTest::qWait(10);
  require(reopened.findChild<QLabel*>("KQLastPublicCheck")->text().contains("10-14") &&
      reopened.findChild<QLabel*>("KQPublicUpdateTitle")->text().contains(QStringLiteral("部分")), "reopen reset receipt-derived result");
  if (good) std::puts("PASS: persistent result presentation, partial/retry/apply states, real progress, named changes and folded diagnostics");
  return good ? 0 : 1;
}
