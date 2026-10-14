#include "ui/detail/public_data_detail_dialog.h"
#include "fixtures/public_data_detail_fixture.h"
#include <QApplication>
#include <QTextBrowser>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QScrollBar>
#include <QTest>
#include <cstdio>

int main(int argc,char** argv) {
  QApplication app(argc,argv);
#ifdef KQPET_DETAIL_UI_PREVIEW
  // Independent preview executable: every record here is a synthetic test
  // fixture, never a claim about an actual game skill or battle.
  auto* preview=PublicDataDetailDialog::open({publicDetailFixture(QStringLiteral("合成测试资料")),{}},
      {PublicDataViewKind::PetSkills,QStringLiteral("skills"),QStringLiteral("17")},nullptr);
  preview->setWindowTitle(QStringLiteral("共用资料详情预览 · 合成测试资料"));
  return app.exec();
#else
  bool ok=true;
  const auto require=[&](bool pass,const char* why) { if (!pass) { std::fprintf(stderr,"FAIL: %s\n",why); ok=false; } };
  const PublicRecordKey special{PublicDataViewKind::PetSkills,QStringLiteral("entries"),QStringLiteral("词条·A/B%?=&:17's")};
  PublicRecordKey decoded;
  require(PublicDataDetailLinks::decode(PublicDataDetailLinks::url(special),&decoded) && decoded.view==special.view && decoded.table==special.table && decoded.id==special.id,
      "detail URL did not preserve namespace/unicode/reserved characters");
  require(!PublicDataDetailLinks::decode(QUrl("kqpublic://record?view=99&table=skills&id=17"),&decoded) &&
      !PublicDataDetailLinks::decode(QUrl("https://record?view=1&table=skills&id=17"),&decoded),"invalid navigation authority accepted");
  QWidget owner; owner.resize(500,400); owner.show();
  auto first=std::make_shared<PublicDatasetSnapshot>(*publicDetailFixture());
  auto longSkill=std::make_shared<PetSkillCatalogSnapshot>(*first->skill); auto skills=longSkill->root.value("skills").toObject();
  auto skill=skills.value("17").toObject(); skill.insert("name",QStringLiteral("first · H5技能 <原文>&"));
  skill.insert("description",QStringLiteral("官方长说明\n").repeated(160)); skills.insert("17",skill); longSkill->root.insert("skills",skills); first->skill=longSkill;
  const PublicRecordKey key{PublicDataViewKind::PetSkills,"skills","17"};
  auto* dialog=PublicDataDetailDialog::open({first,{}},key,&owner); QTest::qWait(15);
  auto* browser=dialog->findChild<QTextBrowser*>("publicDetailContent"); auto* sources=dialog->findChild<QToolButton*>("publicDetailSources");
  auto* rawToggle=dialog->findChild<QToolButton*>("publicDetailRawToggle"); auto* raw=dialog->findChild<QPlainTextEdit*>("publicDetailRaw");
  require(browser && sources && raw && rawToggle && !dialog->isModal() && !sources->isChecked() && !raw->isVisible(),"public detail was modal or exposed diagnostics by default");
  if (!browser || !sources || !raw || !rawToggle) return 1;
  require(browser->toPlainText().contains(QStringLiteral("first · H5技能 <原文>&")) && !browser->toPlainText().contains("987654321"),"ordinary detail exposes raw code or interprets a literal name as HTML");
  sources->click(); rawToggle->click(); QTest::qWait(5);
  require(raw->isVisible() && raw->toPlainText().contains("987654321") && raw->toPlainText().contains("first"),"nested original/source evidence discarded");
  browser->verticalScrollBar()->setValue(170); const int saved=browser->verticalScrollBar()->value();
  const auto next=publicDetailFixture("next");
  dialog->navigate({next,{}},key); QTest::qWait(5);
  require(dialog->currentDetail().title.startsWith("next") && !raw->isVisible(),"new navigation did not own its state/context");
  dialog->goBack(); QTest::qWait(5);
  require(dialog->currentDetail().title.startsWith("first") && browser->verticalScrollBar()->value()==saved && raw->isVisible(),"back lost old publication/scroll/fold state");
  dialog->goForward(); QTest::qWait(5);
  require(dialog->currentDetail().title.startsWith("next"),"forward navigation failed");
  const auto same=PublicDataDetailDialog::open({first,{}},{PublicDataViewKind::PetDetail,"stargods","17"},&owner);
  require(same==dialog && dialog->currentDetail().title==QStringLiteral("同号星神"),"entry opened unrelated duplicate or same-number Buff");
  require(owner.findChildren<PublicDataDetailDialog*>().size()==1,"one origin created unbounded detail windows");
  dialog->navigate({{},first->replay},{PublicDataViewKind::BattleReplay,"skills","17"});
  sources->click();
  auto* sourceLinks=dialog->findChild<QTextBrowser*>("publicDetailSourceLinks");
  require(sourceLinks && sourceLinks->toPlainText().contains("official-fixture.swf") &&
      sourceLinks->toHtml().contains("https://example.invalid/official-fixture.swf") &&
      !sourceLinks->toPlainText().contains(QStringLiteral("未单独保存")),"Flash source link was lost or all link labels became only the host name");
  dialog->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  return ok ? 0 : 1;
#endif
}
