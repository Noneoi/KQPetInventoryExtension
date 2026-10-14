#include "support/preview_inventory_support.h"
#include "ui/pet/pet_window.h"
#include "ui/common/pet_image_cache.h"
#include "ui/detail/pet_power_analysis_renderer.h"
#include "ui/detail/pet_skill_renderer.h"
#include "ui/detail/prepared_pet_detail_renderer.h"
#include "ui/detail/stargod_ring_object.h"
#include "ui/workbench/pet_settings_dialog.h"
#include "application/catalog/pet_skill_catalog.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "application/catalog/routine_overview_catalog.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QPainter>
#include <QImage>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QMouseEvent>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTabWidget>
#include <QRegularExpression>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollBar>
#include <cmath>

namespace {
std::shared_ptr<PreparedPetDetail> relationPreviewFixture() {
  auto detail=std::make_shared<PreparedPetDetail>();
  detail->identity.originalName=QStringLiteral("关系布局合成测试");
  detail->detailKnown=detail->sourceVerified=true;
  DetailPage summon; summon.section=DetailSection::SummonRelations;
  summon.summonCandidatesKnown=summon.hasSummonCandidates=true; summon.summonCandidateCount=13;
  const QStringList names{QStringLiteral("[灵初]弑灭魔神·修尔"),QStringLiteral("[灵初]时朔天神·诺亚"),
      QStringLiteral("[灵初]至元御寰·极"),QStringLiteral("[灵初]星冕圣王·帝释天"),
      QStringLiteral("[灵初]辉金龙皇·阿瑞斯"),QStringLiteral("[灵初]炽狱狂神·龙炎"),
      QStringLiteral("[神运]焰屠龙皇·龙炎"),QStringLiteral("[神运]星极龙皇·帝释天"),
      QStringLiteral("[神运]裁律龙皇·阿瑞斯"),QStringLiteral("[灵初]撼空龙皇·修尔"),
      QStringLiteral("[灵初]苍芜灵王·阿瑞斯"),QStringLiteral("[神运]龙枢王者·极"),
      QStringLiteral("[神运]圣元龙皇·诺亚")};
  for (int i=0; i<names.size(); ++i) {
    DetailEntry entry; entry.group=DetailRelationGroup::summonList();
    entry.originalName=names[i]; entry.name=QStringLiteral("不应显示的示例皮肤%1").arg(i);
    entry.definitionId=9000+i; entry.relatedInstanceId=i<11 ? 100+i : 0;
    entry.ownership=i<4 ? DetailOwnership::Backpack : i<11 ? DetailOwnership::WarehouseElite : DetailOwnership::Missing;
    entry.selected=i==5; summon.entries.append(entry);
    if (entry.selected) { entry.group=DetailRelationGroup::summoned(); summon.entries.prepend(entry); }
  }
  detail->pages.append(summon);
  DetailPage carry; carry.section=DetailSection::CarryRelations; carry.contractCarrierRole=true;
  carry.carryKind=DetailCarryKind::Swallower;
  carry.carryCandidatesKnown=carry.hasCarryCandidates=carry.carryCandidatesExpanded=true; carry.carryCandidateCount=3;
  const QStringList carryNames{QStringLiteral("[灵初]青穹掠风·阿特拉斯"),QStringLiteral("[神运]青穹之骑·阿特拉斯"),
      QStringLiteral("[灵初]万界寻观·塔梨")};
  for (int i=0; i<carryNames.size(); ++i) {
    DetailEntry entry; entry.group=DetailRelationGroup::carryCandidate();
    entry.originalName=carryNames[i]; entry.name=QStringLiteral("不应显示的吞噬皮肤%1").arg(i);
    entry.relatedInstanceId=200+i; entry.ownership=DetailOwnership::WarehouseElite; entry.selected=i==2;
    carry.entries.append(entry);
    if (entry.selected) { entry.group=DetailRelationGroup::carriedEnvoy(); carry.entries.prepend(entry); }
  }
  detail->pages.append(carry); return detail;
}
QString relationsOnly(const PreparedPetDetailHandle& detail) {
  auto html=PreparedPetDetailRenderer::render(detail,{});
  const auto first=html.indexOf(QStringLiteral("<h3>召唤物关系</h3>"));
  const auto body=html.indexOf(QStringLiteral("<body>"));
  if (first<0 || body<0) return {};
  return html.left(body+6)+QStringLiteral("<h2>关系布局预览 · 合成测试资料</h2>")+html.mid(first);
}
int saveRelationPreview(const QString& path) {
  const auto html=relationsOnly(relationPreviewFixture());
  if (html.isEmpty()) return 1;
  QTextDocument document; document.setHtml(html); document.setTextWidth(1000);
  const int height=int(std::ceil(document.size().height()));
  if (height<=0 || height>10000) return 1;
  QImage image(1000,height,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
  QPainter painter(&image); document.drawContents(&painter); painter.end();
  if (!image.save(path,"PNG")) { std::fprintf(stderr,"FAIL: cannot save relation preview\n"); return 1; }
  std::puts("PASS: synthetic relation preview saved"); return 0;
}
}

int main(int argc, char** argv) {
  QApplication app(argc,argv);
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  const auto arguments=QCoreApplication::arguments();
  const int relationPreviewArgument=arguments.indexOf(QStringLiteral("--relation-preview"));
  if (relationPreviewArgument>=0) return relationPreviewArgument+1<arguments.size() ? saveRelationPreview(arguments[relationPreviewArgument+1]) : 1;
  QTemporaryDir temporary;
  PreviewInventory::Fixture fixture(temporary.path(),QStringLiteral("selected-refresh-fixture"));
  auto require = [](bool ok,const char* reason) { if(!ok) std::fprintf(stderr,"FAIL: %s\n",reason); return ok; };
  const QString astrolabeRaw = QStringLiteral("0:1|350:1:2#351:0:1#352:2:0#353:0#invalid-node");
  QJsonObject pet{{"id",42},{"r",7396},{"lv",100},{"n",QStringLiteral("详情刷新样例")},
      {"astrolabe",astrolabeRaw},
      {"czdlv",QJsonObject{{"iv",111}}},{"mzdlv",QJsonObject{{"iv",222}}},{"opaqueDetailRevision",1}};
  bool ok = require(fixture.initialized && fixture.publishLists({pet},{},{}) && fixture.factsReady(),"initial fixture did not become ready");
  PetWindow window(&fixture.inventory); window.resize(1400,900); window.show(); window.focusPet(42);
  auto* browser = window.findChild<PetImageBrowser*>(QStringLiteral("KQPetPreparedDetail"));
  auto* analysis = window.findChild<QTextBrowser*>(QStringLiteral("KQPetPowerAnalysis"));
  auto* skills = window.findChild<QTextBrowser*>(QStringLiteral("KQPetSkillDetail"));
  auto* analysisPage = window.findChild<QWidget*>(QStringLiteral("KQPetAnalysisPage"));
  auto* refreshMaterials = window.findChild<QPushButton*>(QStringLiteral("KQRefreshMaterialInventory"));
  int materialRequests = 0;
  QObject::connect(&window,&PetWindow::cultivationMaterialsRefreshRequested,&window,[&] { ++materialRequests; });
  int gameRequests = 0;
  QObject::connect(&window, &PetWindow::detailRequested, &window, [&](qint64) { ++gameRequests; });
  QObject::connect(&window, &PetWindow::listRefreshRequested, &window, [&] { ++gameRequests; });
  ok &= require(PreviewInventory::until([&] { return fixture.inventory.preparedDetail(0,42) && browser &&
      !browser->toPlainText().contains(QStringLiteral("正在准备本地详情")); }),"initial selected detail was not prepared");
  {
    const auto detailText = browser ? browser->toPlainText() : QString();
    const auto pendingPosition = detailText.indexOf(QStringLiteral("状态待确认"));
    const auto normalText = pendingPosition >= 0 ? detailText.left(pendingPosition) : detailText;
    const auto pendingText = pendingPosition >= 0 ? detailText.mid(pendingPosition) : QString();
    ok &= require(pendingPosition >= 0 && normalText.contains(QStringLiteral("已点亮：—")) &&
                      !detailText.contains(QStringLiteral("星迹·核心")) &&
                      normalText.contains(QStringLiteral("未点亮：星灵·闪避与强化")) &&
                      !normalText.contains(QStringLiteral("星灵·暴击")) &&
                      !normalText.contains(QStringLiteral("星灵·防暴")) &&
                      !normalText.contains(QStringLiteral("星灵·命中与抵抗")),
                  "invalid astrolabe nodes entered the ordinary lighted/unlighted lists or legal two-field nodes disappeared");
    for (const auto& pattern : {
             QStringLiteral("星灵·暴击\\s+点亮\\s+已点亮\\s+装备\\s+无法解释：待确认"),
             QStringLiteral("星灵·防暴\\s+点亮\\s+未点亮\\s+装备\\s+已装备"),
             QStringLiteral("星灵·命中与抵抗\\s+点亮\\s+无法解释：待确认\\s+装备\\s+未装备")})
      ok &= require(QRegularExpression(pattern).match(pendingText).hasMatch(),
                    "an invalid astrolabe node hid independently known flags or invented an unknown flag");
    ok &= require(pendingText.count(QStringLiteral("星轮记录无效，保留已返回状态")) == 3 &&
                      pendingText.contains(QStringLiteral("星轮字段无效")) &&
                      fixture.inventory.detailFor(42).value(QStringLiteral("astrolabe")).toString() == astrolabeRaw,
                  "astrolabe diagnostics or the original invalid record were discarded");
  }
  {
    // Pure prepared values exercise rendering; Domain tests separately verify
    // official sign tags and the exact instance roster classifications.
    auto detail = std::make_shared<PreparedPetDetail>();
    detail->identity.originalName = QStringLiteral("关系测试精灵");
    DetailPage page; page.section=DetailSection::CarryRelations;
    page.carryKind=DetailCarryKind::Swallower; page.contractCarrierRole=true;
    page.carryCandidatesExpanded=page.hasCarryCandidates=page.carryCandidatesKnown=true;
    DetailEntry current; current.group=DetailRelationGroup::carriedEnvoy();
    current.name=QStringLiteral("不应显示的当前皮肤"); current.originalName=QStringLiteral("当前对象原名"); current.relatedInstanceId=100;
    current.ownership=DetailOwnership::WarehouseNormal; page.entries.append(current);
    const QVector<QPair<QString,DetailOwnership>> candidates{
        {QStringLiteral("背包对象"),DetailOwnership::Backpack},
        {QStringLiteral("仓库对象"),DetailOwnership::WarehouseElite},
        {QStringLiteral("待定位对象"),DetailOwnership::OwnedLocationUnknown},
        {QStringLiteral("未查位置对象"),DetailOwnership::Unknown},
        {QStringLiteral("未记录对象"),DetailOwnership::Missing}};
    int id=101;
    for (const auto& value : candidates) {
      DetailEntry entry; entry.group=DetailRelationGroup::carryCandidate();
      entry.name=QStringLiteral("不应显示的候选皮肤"); entry.originalName=value.first;
      entry.ownership=value.second; entry.relatedInstanceId=id++; entry.selected=entry.ownership==DetailOwnership::Backpack;
      entry.fields.append({{},QStringLiteral("当前战力"),QStringLiteral("99999999"),DetailKnowledge::Known});
      page.entries.append(entry);
    }
    detail->pages.append(page);
    const auto html = PreparedPetDetailRenderer::render(detail,{});
    QTextDocument document; document.setHtml(html); const auto text=document.toPlainText();
    const auto choices = text.mid(text.indexOf(QStringLiteral("可选吞噬对象")));
    ok &= require(text.contains(QStringLiteral("当前吞噬对象")) &&
        choices.contains(QStringLiteral("当前关联")) && !choices.contains(QStringLiteral("当前对象原名")) &&
        text.contains(QStringLiteral("当前对象原名")) && !text.contains(QStringLiteral("不应显示")) &&
        !choices.contains(QStringLiteral("99999999")) && !choices.contains(QStringLiteral("展开")) &&
        !text.contains(QStringLiteral("可契约候选")) && !text.contains(QStringLiteral("神使契约")) &&
        !text.contains(QStringLiteral("以下未拥有")),"current carry object and possible choices were conflated");
    ok &= require(QRegularExpression(QStringLiteral("背包内\\s+1 只\\s+背包对象")).match(choices).hasMatch() &&
        QRegularExpression(QStringLiteral("仓库内\\s+1 只\\s+仓库对象")).match(choices).hasMatch() &&
        choices.contains(QStringLiteral("精英仓库")) && choices.contains(QStringLiteral("已拥有 · 位置待确认")) &&
        choices.contains(QStringLiteral("当前列表未记录")) && choices.contains(QStringLiteral("不等同于未拥有")) &&
        !html.contains(QStringLiteral("kqdetail://pet/104")) && !html.contains(QStringLiteral("kqdetail://pet/105")) &&
        html.contains(QStringLiteral("kqdetail://pet/101")),
        "backpack, warehouse and unrecorded candidates were not distinctly grouped or unknown instances linked");
    detail->pages[0].entries={current}; detail->pages[0].carryCandidatesExpanded=false;
    detail->pages[0].carryCandidateCount=5;
    document.setHtml(PreparedPetDetailRenderer::render(detail,{}));
    ok &= require(document.toPlainText().contains(QStringLiteral("查看全部")) &&
        !document.toPlainText().contains(QStringLiteral("无可选对象")),"legacy folded candidates became an empty result");
    detail->pages[0].carryCandidatesExpanded=true; detail->pages[0].hasCarryCandidates=false;
    detail->pages[0].carryCandidatesKnown=false; detail->pages[0].relationCatalogLoading=true;
    document.setHtml(PreparedPetDetailRenderer::render(detail,{}));
    ok &= require(document.toPlainText().contains(QStringLiteral("正在读取官方关联资料")) &&
        !document.toPlainText().contains(QStringLiteral("无可选对象")),"loading relation rules were reported as no candidates");
    detail->pages[0].relationCatalogLoading=false;
    detail->pages[0].relationCatalogProblem=QStringLiteral("C:/private/source.json: invalid 123456");
    document.setHtml(PreparedPetDetailRenderer::render(detail,{}));
    ok &= require(document.toPlainText().contains(QStringLiteral("官方关联资料暂不可用")) &&
        !document.toPlainText().contains(QStringLiteral("source.json")) &&
        !document.toPlainText().contains(QStringLiteral("123456")),"technical relation error leaked into ordinary detail");
    detail->pages[0].relationCatalogProblem.clear();
    document.setHtml(PreparedPetDetailRenderer::render(detail,{}));
    ok &= require(document.toPlainText().contains(QStringLiteral("暂无可用的官方配对规则")) &&
        !document.toPlainText().contains(QStringLiteral("无可选对象")),"unavailable relation rules became an empty confirmed result");
    detail->pages[0].carryCandidatesKnown=true;
    document.setHtml(PreparedPetDetailRenderer::render(detail,{}));
    ok &= require(document.toPlainText().contains(QStringLiteral("无可选对象")),"confirmed empty candidate list lost its clear result");
    detail->pages[0].contractCarrierRole=false; detail->pages[0].contractEnvoyRole=true;
    detail->pages[0].entries.clear(); current.group=DetailRelationGroup::carrierOwner();
    current.carryKind=DetailCarryKind::Almighty; detail->pages[0].entries.append(current);
    document.setHtml(PreparedPetDetailRenderer::render(detail,{}));
    ok &= require(document.toPlainText().contains(QStringLiteral("天觉携带方")) &&
        !document.toPlainText().contains(QStringLiteral("可选吞噬对象")) &&
        !document.toPlainText().contains(QStringLiteral("神使")),"reverse mechanism became a generic envoy or acquired choices");
    const auto fixtureHtml=relationsOnly(relationPreviewFixture());
    document.setHtml(fixtureHtml); const auto fixtureText=document.toPlainText();
    ok &= require(fixtureText.contains(QStringLiteral("可选召唤对象")) && fixtureText.contains(QStringLiteral("共 13 项")) &&
        fixtureText.contains(QStringLiteral("[神运]圣元龙皇·诺亚")) &&
        fixtureText.contains(QStringLiteral("当前吞噬对象")) && fixtureText.contains(QStringLiteral("共 3 项")) &&
        !fixtureText.contains(QStringLiteral("不应显示")) && !fixtureText.contains(QStringLiteral("展开")) &&
        !fixtureText.contains(QStringLiteral("极限战力")) && !fixtureText.contains(QStringLiteral("等级")) &&
        !fixtureText.contains(QStringLiteral("时代")),"complete summon/carry candidates were hidden, paged or rendered as verbose stat rows");
  }
  QTabWidget* detailTabs = nullptr;
  for (auto* tabs : window.findChildren<QTabWidget*>())
    if (analysisPage && tabs->indexOf(analysisPage) >= 0) detailTabs = tabs;
  ok &= require(analysis && detailTabs && detailTabs->tabText(detailTabs->indexOf(analysisPage)) == QStringLiteral("精灵分析") &&
      detailTabs->indexOf(analysisPage) == 3, "analysis tab was not placed after detail, skill and raw data");
  ok &= require(skills && detailTabs && detailTabs->indexOf(skills) == 1 &&
      detailTabs->tabText(1) == QStringLiteral("技能资料"), "skill tab was not placed beside pet detail");
  if (detailTabs && skills) detailTabs->setCurrentWidget(skills);
  QCoreApplication::processEvents();
  ok &= require(skills && skills->toPlainText().contains(QStringLiteral("技能槽总览")) &&
      skills->toPlainText().contains(QStringLiteral("普通技")) &&
      skills->toPlainText().contains(QStringLiteral("万道御宇")) && gameRequests == 0,
      "opening local skill data queried the game or failed to map the selected race");
  const QString skillText = skills ? skills->toPlainText() : QString();
  ok &= require(!skillText.contains(QStringLiteral("灵初溯灵规则")) &&
      !skillText.contains(QStringLiteral("技能实现 buff")) &&
      !skillText.contains(QStringLiteral("机制词条展开")) &&
      !skillText.contains(QStringLiteral("技能 ID")) && !skillText.contains(QStringLiteral("种族 ")) &&
      !skillText.contains(QStringLiteral("107396")),
      "skill detail kept a removed rule, internal identifier, raw buff or standalone mechanism section");
  ok &= require(!skillText.contains(QStringLiteral("超必杀技")) &&
      !skillText.contains(QStringLiteral("龙职技")) &&
      !skillText.contains(QStringLiteral("通灵技")),
      "skill slot overview still listed slots the selected pet does not have");
  {
    QJsonObject officialOnly = fixture.inventory.skillSnapshot()->root;
    QString error;
    ok &= require(PetSkillCatalog::prepare(officialOnly, QStringLiteral("仅官方技能缓存"), {}, &error) &&
                      error.isEmpty(),
                  "the official game-data cache incorrectly depended on the static supplement");
  }
  const QTextCursor term = skills ? skills->document()->find(QStringLiteral("[灵力枷锁]")) : QTextCursor();
  ok &= require(!term.isNull() && term.charFormat().fontWeight() >= QFont::Bold &&
      term.charFormat().toolTip().contains(QStringLiteral("无法行动")) &&
      term.charFormat().toolTip().contains(QChar('\n')) &&
      !term.charFormat().toolTip().contains(QStringLiteral("&#")),
      "mechanism term was not bold or its hover explanation retained encoded line breaks");
  const QString renderedSkillHtml = PetSkillRenderer::render(fixture.inventory.skillSnapshot(), 7396,
                                                              QStringLiteral("技能卡样例"));
  ok &= require(renderedSkillHtml.count(QStringLiteral("class='skill-type' width='33%'")) >= 4 &&
                    !renderedSkillHtml.contains(QStringLiteral("本地资料没有单独释义")),
                "all owned skill cards did not use the one-third label layout or kept the empty-definition message");
  {
    QTextDocument lingchuDocument;
    lingchuDocument.setHtml(renderedSkillHtml);
    const QString lingchuText = lingchuDocument.toPlainText();
    QTextDocument ordinaryDocument;
    ordinaryDocument.setHtml(PetSkillRenderer::render(fixture.inventory.skillSnapshot(), 1,
                                                       QStringLiteral("普通精灵样例")));
    ok &= require(!lingchuText.contains(QStringLiteral("技能简评")) &&
                      !lingchuText.contains(QStringLiteral("重点偏向")) &&
                      !ordinaryDocument.toPlainText().contains(QStringLiteral("技能简评")),
                  "historical keyword evaluations leaked into the current official skill UI");
    QTextDocument missingDocument;
    missingDocument.setHtml(PetSkillRenderer::render(fixture.inventory.skillSnapshot(), 987654321,
                                                      QStringLiteral("缺少资料的精灵")));
    ok &= require(missingDocument.toPlainText().contains(QStringLiteral("缺少资料的精灵")) &&
                      missingDocument.toPlainText().contains(QStringLiteral("暂无对应技能资料")) &&
                      !missingDocument.toPlainText().contains(QStringLiteral("987654321")) &&
                      fixture.inventory.skillSnapshot()->root.value(QStringLiteral("pets")).toObject()
                          .value(QStringLiteral("7396")).toObject().value(QStringLiteral("slots")).toObject()
                          .value(QStringLiteral("normal")).toInt() == 107396,
                  "missing skill data exposed an internal ID or display filtering changed the retained raw slots");
  }
  if (skills && !term.isNull()) {
    QTextCursor hover = term;
    hover.clearSelection();
    hover.setPosition(term.selectionStart() + 1);
    skills->setTextCursor(hover);
    skills->ensureCursorVisible();
    QCoreApplication::processEvents();
    const QPoint position = skills->cursorRect(hover).center();
    QMouseEvent move(QEvent::MouseMove, QPointF(position),
                     QPointF(skills->viewport()->mapToGlobal(position)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(skills->viewport(), &move);
    QCoreApplication::processEvents();
    auto* popup = window.findChild<QWidget*>(QStringLiteral("KQSkillTermPopup"));
    auto* popupText = window.findChild<QLabel*>(QStringLiteral("KQSkillTermPopupText"));
    ok &= require(popup && popup->isVisible() && popupText &&
                      popupText->text().contains(QStringLiteral("无法行动")) &&
                      popupText->textFormat() == Qt::PlainText && popupText->wordWrap() &&
                      popupText->width() == 400 && !popupText->text().contains(QStringLiteral("&#")),
                  "mechanism explanation was not an immediately wrapped, decoded plain-text popup");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(skills->viewport(), &leave);
    QCoreApplication::processEvents();
    ok &= require(popup && !popup->isVisible(),
                  "mechanism explanation remained after the pointer left the skill text");
  }
  {
    QTextDocument dragonDocument;
    dragonDocument.setHtml(PetSkillRenderer::render(fixture.inventory.skillSnapshot(), 5307,
                                                     QStringLiteral("龙尊机制样例")));
    const QTextCursor dragon = dragonDocument.find(QStringLiteral("[龙尊权能]"));
    ok &= require(!dragon.isNull() && dragon.charFormat().fontWeight() >= QFont::Bold &&
        dragon.charFormat().toolTip().contains(QStringLiteral("暂无官方说明")) &&
        dragon.charFormat().toolTip().contains(QStringLiteral("当前原文")),
        "missing exact entry borrowed an archived/reverse-reference definition instead of preserving current text");
  }
  {
    QJsonObject futureRoot = fixture.inventory.skillSnapshot()->root;
    QJsonObject futureEntries = futureRoot.value(QStringLiteral("entries")).toObject();
    futureEntries.insert(QStringLiteral("龙尊权能"), QStringLiteral("官方新增的龙尊权能独立释义"));
    futureEntries.insert(QStringLiteral("受伤降低·30%"), QStringLiteral("官方新增的精确参数词条释义"));
    futureRoot.insert(QStringLiteral("entries"), futureEntries);
    QString error;
    const auto future = PetSkillCatalog::prepare(futureRoot, QStringLiteral("未来官方技能缓存"), {}, &error);
    QTextDocument futureDocument;
    if (future) futureDocument.setHtml(PetSkillRenderer::render(future, 5307, QStringLiteral("官方优先样例")));
    const QTextCursor term = futureDocument.find(QStringLiteral("[龙尊权能]"));
    QTextDocument parameterDocument;
    if (future) parameterDocument.setHtml(PetSkillRenderer::render(future, 7396, QStringLiteral("参数词条样例")));
    const QTextCursor parameterTerm = parameterDocument.find(QStringLiteral("[受伤降低·30%]"));
    ok &= require(future && !term.isNull() && !parameterTerm.isNull() &&
                      term.charFormat().toolTip().contains(QStringLiteral("官方新增的龙尊权能独立释义")) &&
                      !term.charFormat().toolTip().contains(QStringLiteral("龙尊觉醒")) &&
                      parameterTerm.charFormat().toolTip().contains(QStringLiteral("官方新增的精确参数词条释义")),
                  "a future official definition did not override the bundled static supplement");
  }
  {
    QJsonObject exactRoot = fixture.inventory.skillSnapshot()->root;
    auto entries = exactRoot.value(QStringLiteral("entries")).toObject();
    entries.insert(QStringLiteral("护盾"), QStringLiteral("不可借用的护盾基础说明"));
    entries.insert(QStringLiteral("官方空"), QString());
    for (const QString term : {QStringLiteral("护盾Ⅱ"),QStringLiteral("末护盾"),QStringLiteral("护盾-Ⅱ")})
      entries.remove(term);
    exactRoot.insert(QStringLiteral("entries"), entries);
    auto definitions = exactRoot.value(QStringLiteral("skills")).toObject();
    auto normal = definitions.value(QStringLiteral("107396")).toObject();
    normal.insert(QStringLiteral("description"), QStringLiteral("当前技能保留[护盾Ⅱ]、[末护盾]、[护盾-Ⅱ]和[官方空]这四个原文。"));
    definitions.insert(QStringLiteral("107396"), normal);
    exactRoot.insert(QStringLiteral("skills"), definitions);
    auto transforms = exactRoot.value(QStringLiteral("transformSkills")).toObject();
    auto transform = transforms.value(QStringLiteral("507396")).toObject();
    transform.insert(QStringLiteral("name"), QStringLiteral("名称含幻元但不是类别证据"));
    transform.insert(QStringLiteral("type"), 1);
    transforms.insert(QStringLiteral("507396"), transform);
    exactRoot.insert(QStringLiteral("transformSkills"), transforms);
    QString error;
    const auto exact = PetSkillCatalog::prepare(exactRoot, QStringLiteral("精确关联样例"), {}, &error);
    const QString html = exact ? PetSkillRenderer::render(exact,7396,QStringLiteral("精确关联样例")) : QString();
    QTextDocument exactDocument; exactDocument.setHtml(html);
    for (const QString term : {QStringLiteral("护盾Ⅱ"),QStringLiteral("末护盾"),QStringLiteral("护盾-Ⅱ"),QStringLiteral("官方空")}) {
      const QTextCursor cursor = exactDocument.find(QStringLiteral("[%1]").arg(term));
      ok &= require(exact && !cursor.isNull() && cursor.charFormat().toolTip().contains(QStringLiteral("暂无官方说明")) &&
                        cursor.charFormat().toolTip().contains(QStringLiteral("当前原文")) &&
                        !cursor.charFormat().toolTip().contains(QStringLiteral("不可借用的护盾基础说明")),
                    "a suffix, substring, or official empty term borrowed another definition in the actual renderer");
    }
    ok &= require(html.contains(QStringLiteral("class='skill-type' width='33%'><b>赋能技</b>")) &&
                      !html.contains(QStringLiteral("class='skill-type' width='33%'><b>幻元技</b>")),
                  "rendered transform category followed its name instead of the same-ID official type");
  }
  {
    PetSettingsDialog settings(RefreshTimings{});
    auto* checkUpdate = settings.findChild<QPushButton*>(QStringLiteral("KQSoftwareUpdateCheck"));
    auto* installUpdate = settings.findChild<QPushButton*>(QStringLiteral("KQSoftwareUpdateInstall"));
    auto* updateNotes = settings.findChild<QTextBrowser*>(QStringLiteral("KQSoftwareUpdateNotes"));
    ok &= require(checkUpdate && installUpdate && updateNotes && checkUpdate->isEnabled() &&
        !installUpdate->isEnabled(),
        "settings did not expose a manual GitHub update flow with confirmation disabled before checking");
    auto* updatePage = settings.findChild<QWidget*>(QStringLiteral("KQSoftwareUpdatePage"));
    auto* settingsTabs = settings.findChild<QTabWidget*>(QStringLiteral("KQSettingsTabs"));
    if (settingsTabs && updatePage) settingsTabs->setCurrentWidget(updatePage);
    settings.resize(760, 680);
    settings.show();
    QCoreApplication::processEvents();
    ok &= require(settings.grab().save(QDir::current().filePath(QStringLiteral("settings-update-preview.png"))),
                  "software update settings preview could not be captured");
    if (qEnvironmentVariableIsSet("KQPET_TEST_LIVE_GITHUB")) {
      auto* updateProgress = settings.findChild<QProgressBar*>(QStringLiteral("KQSoftwareUpdateProgress"));
      auto* updateStatus = settings.findChild<QLabel*>(QStringLiteral("KQSoftwareUpdateStatus"));
      checkUpdate->click();
      ok &= require(PreviewInventory::until([&] {
          return updateProgress && updateStatus && !updateProgress->isVisible() &&
                 !updateStatus->text().contains(QStringLiteral("正在"));
        }, 30000) && !updateNotes->toPlainText().trimmed().isEmpty() &&
            (updateStatus->text().contains(QStringLiteral("最新正式版")) ||
             updateStatus->text().contains(QStringLiteral("发现新版本"))),
          "live GitHub check did not finish through the API or official-page fallback");
    }
    settings.hide();
  }
  if (skills) ok &= require(skills->grab().save(QDir::current().filePath(QStringLiteral("skill-ui-preview.png"))),
                            "redesigned skill preview could not be captured");
  if (detailTabs) detailTabs->setCurrentWidget(analysisPage);
  QCoreApplication::processEvents();
  ok &= require(analysis && analysis->toPlainText().contains(QStringLiteral("官方极限战斗力")) && gameRequests == 0,
      "opening the local analysis tab queried the game or did not render prepared power");
  ok &= require(refreshMaterials && refreshMaterials->isVisible() && refreshMaterials->isEnabled() &&
      refreshMaterials->text() == QStringLiteral("刷新材料背包") && materialRequests == 0,
      "manual inventory button was hidden without material rows or page opening queried inventory");
  if (refreshMaterials) {
    analysis->verticalScrollBar()->setValue(analysis->verticalScrollBar()->maximum());
    ok &= require(refreshMaterials->isVisible(),"material refresh disappeared when analysis scrolled");
    refreshMaterials->click();
    MaterialInventorySnapshot busy; busy.running = true; window.setCultivationMaterials(busy);
    refreshMaterials->click();
    ok &= require(materialRequests == 1 && !refreshMaterials->isEnabled() && gameRequests == 0,
        "manual click did not emit once or running inventory query accepted a duplicate click");
    window.setCultivationMaterials({});
  }
  const auto before = fixture.repository.recordVersion(42).key;
  const auto beforeBrief = fixture.repository.briefFor(42);
  const QString shown = browser ? browser->toPlainText() : QString();
  QSemaphore entered, release;
  const bool queued = fixture.controller.postPriorityCompute([&] { entered.release(); release.acquire(); });
  const bool held = queued && PreviewInventory::until([&] { return entered.tryAcquire(); });
  ok &= require(held,"could not hold the existing Compute executor");
  if(held) {
    pet.insert(QStringLiteral("opaqueDetailRevision"),2);
    ok &= require(fixture.publishLists({pet},{},{}) && fixture.repository.briefFor(42) == beforeBrief &&
        !(fixture.repository.recordVersion(42).key == before),"fixture did not change raw revision while retaining the same brief");
    QCoreApplication::processEvents();
    ok &= require(browser && browser->toPlainText() == shown,"list refresh erased the visible prepared detail while Compute was busy");
    ok &= require(analysis && analysis->toPlainText().contains(QStringLiteral("保留上次分析")) &&
        analysis->toPlainText().contains(QStringLiteral("数据时间")),
        "local recalculation erased the previous analysis or its observation timestamp");
  }
  release.release();
  ok &= require(PreviewInventory::until([&] { const auto detail=fixture.inventory.preparedDetail(0,42);
      return detail && detail->version.facts.record == fixture.repository.recordVersion(42).key; }),
      "raw-only list refresh left selected detail waiting forever");
  ok &= require(PreviewInventory::until([&] { return analysis &&
      !analysis->toPlainText().contains(QStringLiteral("保留上次分析")); }) && gameRequests == 0,
      "fresh prepared facts did not replace old analysis or background analysis queried the game");

  {
    auto incomplete = std::make_shared<PreparedPetDetail>();
    incomplete->identity.name = QStringLiteral("<未知养成>");
    incomplete->identity.instanceId = 9001;
    incomplete->battlePower.hasServerCurrent = true;
    incomplete->battlePower.serverCurrent = 12345;
    incomplete->battlePower.highest = 987654; // A value with no knowledge flag must stay hidden.
    incomplete->battlePower.current = 987655;
    QTextDocument unknownAnalysis;
    unknownAnalysis.setHtml(PetPowerAnalysisRenderer::render(incomplete, {}));
    const auto text = unknownAnalysis.toPlainText();
    ok &= require(text.contains(QStringLiteral("<未知养成>")) && text.contains(QStringLiteral("12345")) &&
        !text.contains(QStringLiteral("987654")) && !text.contains(QStringLiteral("987655")) &&
        text.contains(QStringLiteral("数据尚未齐全")),
        "analysis hid a server observation, exposed an unknown total, or interpreted a pet name as markup");
  }

  {
    auto concise = std::make_shared<PreparedPetDetail>();
    concise->identity.name = QStringLiteral("缺口展示样例");
    concise->detailKnown = true;
    PetBattlePowerComponent component;
    component.label = QStringLiteral("元魂战力");
    component.currentKnown = true;
    component.current = 1234;
    component.explanation = QStringLiteral("不应向用户显示的词条解释");
    concise->battlePower.components.append(component);
    concise->battlePower.unknownReasons.append(QStringLiteral("不应逐项列出的分析原因"));
    PetStargodSlotPower slot;
    slot.equippedName = QStringLiteral("不应逐槽列出的星神名字");
    concise->battlePower.stargodDetails.append(slot);
    PetCultivationRequirement soul;
    soul.category = QStringLiteral("元魂");
    soul.name = QStringLiteral("<神攻·夯实基础>");
    soul.status = QStringLiteral("未觉醒，差 1 次觉醒");
    soul.known = true;
    PetCultivationMaterial soulMaterial;
    soulMaterial.type = 4;
    soulMaterial.id = 41;
    soulMaterial.name = QStringLiteral("<对应元魂>");
    soulMaterial.count = 2;
    soulMaterial.known = true;
    soul.materials.append(soulMaterial);
    concise->cultivationRequirements.items.append(soul);
    auto secondSoul = soul;
    secondSoul.name = QStringLiteral("神攻·锤炼技巧");
    secondSoul.materials[0].count = 3;
    concise->cultivationRequirements.items.append(secondSoul);
    PetCultivationRequirement source;
    source.category = QStringLiteral("源兽");
    source.name = QStringLiteral("源兽");
    source.status = QStringLiteral("差 2 星、3 阶");
    source.known = true;
    PetCultivationMaterial sourceMaterial;
    sourceMaterial.type = 24;
    sourceMaterial.id = 44;
    sourceMaterial.extra = 1;
    sourceMaterial.name = QStringLiteral("对应源兽");
    sourceMaterial.count = 4;
    sourceMaterial.known = true;
    source.materials.append(sourceMaterial);
    concise->cultivationRequirements.items.append(source);
    PetCultivationRequirement stars;
    stars.category = QStringLiteral("星神");
    stars.name = QStringLiteral("星神");
    stars.status = QStringLiteral("差 2 个红星、1 个万变红星");
    stars.known = true;
    concise->cultivationRequirements.items.append(stars);
    PetCultivationRequirement wheel;
    wheel.name = QStringLiteral("星轮");
    wheel.status = QStringLiteral("未突破");
    PetCultivationMaterial essence;
    essence.type = 35;
    essence.id = 100;
    essence.name = QStringLiteral("星轮精华");
    essence.count = 7;
    essence.known = true;
    wheel.materials.append(essence);
    concise->cultivationRequirements.items.append(wheel);
    MaterialInventorySnapshot materials;
    materials.knownTypes.insert(4);
    materials.counts.insert(cultivationMaterialKey(4, 41), 1);
    materials.counts.insert(cultivationMaterialKey(24, 44), 99);
    materials.counts.insert(cultivationMaterialKey(24, 44, 1), 2);
    QTextDocument conciseAnalysis;
    const auto conciseHtml = PetPowerAnalysisRenderer::render(concise, {}, false, {}, materials);
    conciseAnalysis.setHtml(conciseHtml);
    const auto text = conciseAnalysis.toPlainText();
    ok &= require(text.contains(QStringLiteral("距离至高还缺")) && text.contains(soul.name) &&
        text.contains(soul.status) && text.contains(source.status) && text.contains(stars.status) &&
        text.contains(component.label) && text.contains(QStringLiteral("1234")) &&
        text.indexOf(QStringLiteral("距离至高还缺")) < text.indexOf(QStringLiteral("战斗力对照")) &&
        !text.contains(component.explanation) && !text.contains(concise->battlePower.unknownReasons.first()) &&
        !text.contains(slot.equippedName) && !text.contains(QStringLiteral("星神逐槽分析")) &&
        !text.contains(QStringLiteral("已有资源与真正缺口")),
        "analysis omitted concrete deficits, interpreted names as markup, or retained unwanted explanations and per-slot analysis");
    const auto materialRow = [](const QString& text, const QString& name, const QString& cells) {
      return QRegularExpression(QRegularExpression::escape(name) + QStringLiteral("\\s+") + cells).match(text).hasMatch();
    };
    ok &= require(text.count(soulMaterial.name) == 1 &&
        materialRow(text, soulMaterial.name, QStringLiteral("5\\s+1\\s+4")) &&
        materialRow(text, sourceMaterial.name, QStringLiteral("4\\s+2\\s+2")) &&
        materialRow(text, essence.name, QStringLiteral("7\\s+未读取\\s+—")) &&
        !conciseHtml.contains(QStringLiteral("kqanalysis://materials")),
        "material requirements were not aggregated, inventory variants were mixed, or unread inventory appeared as zero");
    materials.counts.remove(cultivationMaterialKey(24, 44, 1));
    materials.knownTypes.insert(24); // Knowing the base group does not establish arbitrary selectors.
    materials.knownTypes.insert(35);
    materials.running = true;
    const auto updatingHtml = PetPowerAnalysisRenderer::render(concise, {}, false, {}, materials);
    conciseAnalysis.setHtml(updatingHtml);
    ok &= require(materialRow(conciseAnalysis.toPlainText(), sourceMaterial.name, QStringLiteral("4\\s+未读取\\s+—")) &&
        materialRow(conciseAnalysis.toPlainText(), essence.name, QStringLiteral("7\\s+0\\s+7")) &&
        !updatingHtml.contains(QStringLiteral("kqanalysis://materials")),
        "material inventory reused a different variant, lost a confirmed zero, or kept the refresh link active while reading");
    materials.knownExtraGroups.insert(QStringLiteral("24:1"));
    conciseAnalysis.setHtml(PetPowerAnalysisRenderer::render(concise, {}, false, {}, materials));
    ok &= require(materialRow(conciseAnalysis.toPlainText(), sourceMaterial.name, QStringLiteral("4\\s+0\\s+4")),
                  "a specifically confirmed source-beast selector did not establish an empty stock");
    concise->battlePower.completionKnown = true;
    concise->battlePower.isHighest = true;
    concise->cultivationRequirements.completeKnown = true;
    concise->cultivationRequirements.complete = false;
    conciseAnalysis.setHtml(PetPowerAnalysisRenderer::render(concise, {}));
    ok &= require(conciseAnalysis.toPlainText().contains(QStringLiteral("仍有可提升项目")) &&
        !conciseAnalysis.toPlainText().contains(QStringLiteral("已具备至高培养条件")),
        "old maximum-power observation overrode the newer official cultivation requirements");
    concise->cultivationRequirements.completeKnown = false;
    conciseAnalysis.setHtml(PetPowerAnalysisRenderer::render(concise, {}));
    ok &= require(conciseAnalysis.toPlainText().contains(QStringLiteral("数据尚未齐全")) &&
        !conciseAnalysis.toPlainText().contains(QStringLiteral("已具备至高培养条件")),
        "unknown future cultivation plan was presented as fully cultivated");
  }

  QFile metadata(QStringLiteral(":/kqpet/pet-detail-data.json")); metadata.open(QIODevice::ReadOnly);
  const auto stars=QJsonDocument::fromJson(metadata.readAll()).object().value("stargods").toObject();
  QVector<StargodRingSlot> cells;
  StargodRingSlot center;
  for(auto it=stars.begin();it!=stars.end();++it) {
    const auto value=it.value().toObject(); const int id=it.key().toInt();
    if(value.value("quality").toInt()<6 || !QFile::exists(QStringLiteral(":/kqpet/stargod-icons/%1.png").arg(id))) continue;
    StargodRingSlot cell{id,6,8,value.value("name").toString(),false,value.value("changeable").toBool()};
    if(cell.center) center=cell; else if(cells.size()<7) cells.append(cell);
  }
  if(center.imageId) cells.append(center);
  const QString ring=stargodRingUrl(cells);
  ok &= require(cells.size()==8 && stargodRingSlots(QUrl(ring)).size()==8,"circular diagram lost the seven outer and center cells");
  PetImageBrowser inlineRing;
  inlineRing.setImagesHtml(QStringLiteral("<p><img src='%1'></p>").arg(ring),{});
  bool inlineObject = false;
  for (auto block=inlineRing.document()->begin();block.isValid();block=block.next())
    for(auto it=block.begin();!it.atEnd();++it)
      inlineObject = inlineObject || it.fragment().charFormat().objectType()==StargodRingObject::ObjectType;
  ok &= require(inlineObject,"HTML diagram did not become a native inline circle renderer");
  StargodRingObject object([](const QString& url) { const int id=QUrl(url).path().mid(1).toInt();
    return QImage(QStringLiteral(":/kqpet/stargod-icons/%1.png").arg(id)); });
  QImage preview(480,480,QImage::Format_ARGB32_Premultiplied); preview.fill(Qt::transparent);
  QPainter painter(&preview); QTextCharFormat format; format.setProperty(StargodRingObject::DataProperty,ring);
  object.drawObject(&painter,QRectF(0,0,480,480),nullptr,0,format); painter.end();
  ok &= require(preview.save(QDir::current().filePath(QStringLiteral("stargod-ring-preview.png"))),"ring preview could not be saved");
  {
    // One bundle switch delivers matching metadata and skills to every signal
    // observer and renews the selected prepared detail against that same input.
    window.focusPet(42);
    auto dataset = std::make_shared<PublicDatasetSnapshot>(); dataset->revision = QStringLiteral("synthetic-bundle-1");
    auto detail = std::make_shared<PetDetailCatalogSnapshot>(*fixture.inventory.metadataSnapshot()); ++detail->revision;
    auto skill = std::make_shared<PetSkillCatalogSnapshot>(*fixture.inventory.skillSnapshot()); ++skill->revision;
    dataset->detail = detail; dataset->skill = skill;
    dataset->shop = ShopExchangeCatalog::instance().snapshot(); dataset->routine = RoutineOverviewCatalog::instance().snapshot();
    bool coherent = true; int notifications = 0;
    const auto inspect = [&](quint64) {
      ++notifications;
      coherent &= fixture.inventory.metadataSnapshot() == dataset->detail && fixture.inventory.skillSnapshot() == dataset->skill &&
          fixture.inventory.publicDatasetSnapshot() == dataset;
    };
    const auto a = QObject::connect(&fixture.inventory, &InventoryReadView::metadataChanged, &window, inspect);
    const auto b = QObject::connect(&fixture.inventory, &InventoryReadView::skillMetadataChanged, &window, inspect);
    fixture.publisher.setPublicDataset(dataset); fixture.controller.setPublicDataset(dataset);
    ok &= require(PreviewInventory::until([&] {
      const auto prepared = fixture.inventory.preparedDetail(0,42);
      return prepared && prepared->version.publicDatasetRevision == dataset->revision;
    }) && coherent && notifications == 2, "one public bundle exposed mixed detail/skill versions or reused obsolete prepared detail");
    QObject::disconnect(a); QObject::disconnect(b);
    const auto prepared = fixture.inventory.preparedDetail(0,42);
    const auto computations = fixture.details.stats().completedTasks;
    auto imageOnly = std::make_shared<PublicDatasetSnapshot>(*dataset); imageOnly->revision = QStringLiteral("synthetic-bundle-images-only");
    fixture.publisher.setPublicDataset(imageOnly); fixture.controller.setPublicDataset(imageOnly);
    ok &= require(PreviewInventory::until([&] { return fixture.inventory.publicDatasetSnapshot() == imageOnly; }) &&
        fixture.inventory.preparedDetail(0,42) == prepared && fixture.details.stats().completedTasks == computations,
        "unrelated public image update discarded a valid prepared-detail result");
  }
  window.resetSessionContext();
  ok &= require(refreshMaterials && refreshMaterials->isVisible() && refreshMaterials->isEnabled(),
      "material refresh entry disappeared when no pet was selected");
  window.hide(); ok &= require(fixture.close(),"fixture did not close cleanly");
  if(ok) std::puts("PASS: selected detail survives list refresh, raw revisions trigger renewal, circular stargod rendering");
  return ok?0:1;
}
