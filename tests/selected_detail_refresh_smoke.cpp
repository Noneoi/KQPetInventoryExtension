#include "preview_inventory_support.h"
#include "pet_window.h"
#include "pet_image_cache.h"
#include "pet_power_analysis_renderer.h"
#include "stargod_ring_object.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QPainter>
#include <QFile>
#include <QFontDatabase>
#include <QTextBlock>
#include <QTextDocument>
#include <QTabWidget>
#include <QRegularExpression>
#include <QPushButton>
#include <QScrollBar>

int main(int argc, char** argv) {
  QApplication app(argc,argv);
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  QTemporaryDir temporary;
  PreviewInventory::Fixture fixture(temporary.path(),QStringLiteral("selected-refresh-fixture"));
  auto require = [](bool ok,const char* reason) { if(!ok) std::fprintf(stderr,"FAIL: %s\n",reason); return ok; };
  QJsonObject pet{{"id",42},{"r",7152},{"lv",100},{"n",QStringLiteral("详情刷新样例")},
      {"czdlv",QJsonObject{{"iv",111}}},{"mzdlv",QJsonObject{{"iv",222}}},{"opaqueDetailRevision",1}};
  bool ok = require(fixture.initialized && fixture.publishLists({pet},{},{}) && fixture.factsReady(),"initial fixture did not become ready");
  PetWindow window(&fixture.inventory); window.resize(1400,900); window.show(); window.focusPet(42);
  auto* browser = window.findChild<PetImageBrowser*>(QStringLiteral("KQPetPreparedDetail"));
  auto* analysis = window.findChild<QTextBrowser*>(QStringLiteral("KQPetPowerAnalysis"));
  auto* analysisPage = window.findChild<QWidget*>(QStringLiteral("KQPetAnalysisPage"));
  auto* refreshMaterials = window.findChild<QPushButton*>(QStringLiteral("KQRefreshMaterialInventory"));
  int materialRequests = 0;
  QObject::connect(&window,&PetWindow::cultivationMaterialsRefreshRequested,&window,[&] { ++materialRequests; });
  int gameRequests = 0;
  QObject::connect(&window, &PetWindow::detailRequested, &window, [&](qint64) { ++gameRequests; });
  QObject::connect(&window, &PetWindow::listRefreshRequested, &window, [&] { ++gameRequests; });
  ok &= require(PreviewInventory::until([&] { return fixture.inventory.preparedDetail(0,42) && browser &&
      !browser->toPlainText().contains(QStringLiteral("正在准备本地详情")); }),"initial selected detail was not prepared");
  QTabWidget* detailTabs = nullptr;
  for (auto* tabs : window.findChildren<QTabWidget*>())
    if (analysisPage && tabs->indexOf(analysisPage) >= 0) detailTabs = tabs;
  ok &= require(analysis && detailTabs && detailTabs->tabText(detailTabs->indexOf(analysisPage)) == QStringLiteral("精灵分析") &&
      detailTabs->indexOf(analysisPage) == 2, "analysis tab was not placed after detail and raw data");
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
  window.resetSessionContext();
  ok &= require(refreshMaterials && refreshMaterials->isVisible() && refreshMaterials->isEnabled(),
      "material refresh entry disappeared when no pet was selected");
  window.hide(); ok &= require(fixture.close(),"fixture did not close cleanly");
  if(ok) std::puts("PASS: selected detail survives list refresh, raw revisions trigger renewal, circular stargod rendering");
  return ok?0:1;
}
