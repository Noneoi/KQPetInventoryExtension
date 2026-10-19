#include "ui/analysis/cultivation_material_panel.h"
#include "ui/analysis/cultivation_material_delegate.h"
#include "ui/analysis/asset_analysis_model.h"
#include "domain/pet_cultivation_requirements.h"
#include <QApplication>
#include <QFontDatabase>
#include <QScrollBar>
#include <QTextTable>
#include <QTableView>
#include <QHeaderView>
#include <QFont>
#include <cstdio>

namespace {
QString cellText(QTextTable* table, int row, int column) {
  const auto cell = table->cellAt(row,column);
  auto cursor = cell.firstCursorPosition();
  cursor.setPosition(cell.lastCursorPosition().position(),QTextCursor::KeepAnchor);
  return cursor.selectedText();
}
QTextTable* materialTable(QTextDocument* document) {
  for (auto* frame : document->rootFrame()->childFrames())
    if (auto* table = qobject_cast<QTextTable*>(frame); table && table->columns() == 5) return table;
  return nullptr;
}
}

int main(int argc, char** argv) {
  QApplication app(argc,argv);
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  bool ok = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { ok = false; std::fprintf(stderr,"FAIL: %s\n",message); }
  };
  PetAssetRecord first;
  first.instanceId = 101; first.name = QStringLiteral("[神运]虚构之神·奈非利塔"); first.detailAvailable = true;
  first.cultivationRequirements.items = {{"sacred","sacred",QStringLiteral("神源兽"),"",
      {{24,9001,1,QStringLiteral("神·虚构之神源兽"),5,true}},true}};
  auto second = first;
  second.instanceId = 102; second.name = QStringLiteral("[神运]画境成神·缪斯");
  second.cultivationRequirements.items[0].materials[0].id = 9002;
  second.cultivationRequirements.items[0].materials[0].name = QStringLiteral("神·勇者小队源兽");
  MaterialInventorySnapshot inventory;
  inventory.counts.insert(cultivationMaterialKey(24,9001,1),4);
  inventory.counts.insert(cultivationMaterialKey(24,9002,1),2);
  inventory.observedAt = QDateTime::fromString(QStringLiteral("2026-10-19T09:00:00Z"),Qt::ISODate);
  CultivationMaterialPanel panel;
  panel.resize(1080,250); panel.show();
  const auto render = [&](const CultivationMaterialPlan& plan) { panel.setPlan(plan); app.processEvents(); };
  auto plan = buildCultivationMaterialPlan({&first,&second},"sacred",inventory);
  render(plan);
  auto* table = materialTable(panel.document());
  check(table && table->rows() == 3,"materials did not form distinct rows");
  if (table && table->rows() == 3) {
    check(cellText(table,1,0) == QStringLiteral("神·虚构之神源兽") &&
          cellText(table,1,1) == first.name && cellText(table,1,2) == "5" &&
          cellText(table,1,3) == "4" && cellText(table,1,4) == "1",
          "first material's pet, required, owned and shortage columns diverged");
    check(cellText(table,2,1) == second.name && cellText(table,2,3) == "2" &&
          cellText(table,2,4) == "3","second material borrowed another row's quantity");
  }
  check(panel.horizontalScrollBar()->maximum() == 0,"example requires horizontal scrolling");
  auto selection = panel.textCursor();
  selection.select(QTextCursor::Document); panel.setTextCursor(selection);
  render(plan);
  check(panel.textCursor().hasSelection(),"unchanged material refresh rebuilt the document and lost text selection");
  selection.clearSelection(); panel.setTextCursor(selection);
  panel.grab().save(QStringLiteral("cultivation-materials-example.png"));
  panel.resize(760,250); app.processEvents();
  check(panel.horizontalScrollBar()->maximum() == 0,"narrow example clips columns");
  panel.grab().save(QStringLiteral("cultivation-materials-narrow.png"));

  auto duplicate = first; duplicate.instanceId = 103;
  duplicate.name = first.name = QStringLiteral("[神运]<同名精灵 & 长名称·用于测试自动换行>");
  plan = buildCultivationMaterialPlan({&first,&duplicate},"sacred",inventory);
  render(plan); table = materialTable(panel.document());
  check(table && cellText(table,1,2) == "10" && cellText(table,1,3) == "4" && cellText(table,1,4) == "6",
      "shared stock was deducted more than once in the visible row");
  check(panel.toPlainText().contains(first.name) && panel.toPlainText().contains(QStringLiteral("实例 101")) &&
      panel.toPlainText().contains(QStringLiteral("实例 103")) && panel.toPlainText().count(QStringLiteral("需 5")) == 2,
      "contributors lost their literal names, identity or individual need");
  check(panel.horizontalScrollBar()->maximum() == 0,"long contributors cause horizontal clipping");
  panel.grab().save(QStringLiteral("cultivation-materials-shared.png"));

  render(buildCultivationMaterialPlan({&second},"sacred",{})); table = materialTable(panel.document());
  check(table && cellText(table,1,3) == QStringLiteral("未读取") && cellText(table,1,4) == QStringLiteral("待确认"),
      "unread stock was presented as zero or ready");
  auto partial = second; partial.cultivationRequirements.items[0].materials[0].known = false;
  render(buildCultivationMaterialPlan({&partial},"sacred",inventory));
  check(panel.toPlainText().contains(QStringLiteral("资料不全")) && panel.toPlainText().contains(QStringLiteral("待确认")) &&
        !panel.toPlainText().contains(QStringLiteral("已备齐")),"incomplete demand was labelled ready");
  inventory.counts[cultivationMaterialKey(24,9002,1)] = 20;
  render(buildCultivationMaterialPlan({&second},"sacred",inventory)); table = materialTable(panel.document());
  check(table && cellText(table,1,4) == QStringLiteral("已备齐"),"surplus inventory did not get a readable ready state");

  first.redStarKnown = true; first.missingRedStars = 2;
  render(buildCultivationMaterialPlan({&first},"stargods",inventory)); table = materialTable(panel.document());
  check(table && cellText(table,1,2) == QStringLiteral("—") && cellText(table,1,3) == QStringLiteral("已计入") &&
        cellText(table,1,4) == "2","pet-bound deficits were mislabelled as total demand or shared stock");
  panel.setPlan(buildCultivationMaterialPlan({},"sacred",{}),true);
  check(panel.toPlainText().contains(QStringLiteral("请在下方列表选择")) && !materialTable(panel.document()),
      "empty selection retained old material rows");
  auto many = buildCultivationMaterialPlan({&second},"sacred",inventory);
  const auto repeated = many.materials.first();
  for (int i = 0; i < 30; ++i) many.materials.append(repeated);
  render(many);
  panel.verticalScrollBar()->setValue(panel.verticalScrollBar()->maximum());
  const int scroll = panel.verticalScrollBar()->value();
  check(scroll > 0,"long material list cannot be scrolled");
  many.observedAt = many.observedAt.addSecs(60); render(many);
  check(panel.verticalScrollBar()->value() == scroll,"inventory refresh lost the current scroll position");
  panel.resetPlan();
  check(!panel.toPlainText().contains(first.name) && !materialTable(panel.document()),"session reset retained material details");

  // A display-only exclusion must never remove official requirements or alter
  // demand/stock arithmetic. Both the aggregate panel and pet cells use it.
  auto displayPet = second;
  displayPet.redStarKnown = true; displayPet.missingRedStars = 2;
  displayPet.gapKeys = {QStringLiteral("sg_changeable")};
  displayPet.cultivationRequirements.items.append({"light","astrolabe",QStringLiteral("星轮"),"",
      {{4,2076,0,QStringLiteral("星迹精华"),120,true}, {4,2078,0,QStringLiteral("聚星精华"),200,true}},true});
  displayPet.cultivationRequirements.items.append({"badge","badges",QStringLiteral("元魂"),"",
      {{4,501,0,QStringLiteral("勇毅元魂"),1,true},{4,502,0,QStringLiteral("神攻元魂"),25,true}},true});
  const auto originalRequirements = cultivationRequirementsToJson(displayPet.cultivationRequirements);
  inventory.counts[cultivationMaterialKey(4,2078,0)] = 30;
  const auto underlying = buildCultivationMaterialPlan({&displayPet},"astrolabe",inventory);
  check(underlying.materials.size() == 2 && underlying.materials[1].id == 2078 &&
      underlying.materials[1].required == 200 && underlying.materials[1].missing == 170,
      "hidden item was removed from calculations");
  render(underlying);
  check(!panel.toPlainText().contains(QStringLiteral("聚星精华")) &&
      panel.toPlainText().contains(QStringLiteral("星迹精华")) && !panel.toPlainText().contains(QStringLiteral("2 种")),
      "hidden item leaked into aggregate rows or material-kind counts");
  auto hiddenOnly = underlying; hiddenOnly.materials.removeFirst(); render(hiddenOnly);
  check(!materialTable(panel.document()) && !panel.toPlainText().contains(QStringLiteral("已备齐")) &&
      panel.toPlainText().contains(QStringLiteral("没有需要展示")),"hidden-only demand was presented as fully supplied");

  AssetAnalysisModel model;
  AccountAssetOverview overview; overview.pets = {displayPet}; model.setOverview(overview);
  const auto materialIndex = model.index(0,AssetAnalysisModel::Materials);
  const auto text = model.data(materialIndex).toString();
  check(!text.contains(QStringLiteral("聚星精华")) && !text.contains(QStringLiteral("已知共")) &&
      text.contains(QStringLiteral("星迹精华 × 120")) && text.contains(QStringLiteral("普通红星 × 2")) &&
      text.contains(QStringLiteral("勇毅元魂 × 1\n神攻元魂 × 25")),"pet material cell did not keep distinct visible material lines and quantities");
  check(!model.data(materialIndex,Qt::ToolTipRole).toString().contains(QStringLiteral("聚星精华")),"hidden item leaked through the tooltip");
  check(cultivationRequirementsToJson(model.petAt(0)->cultivationRequirements) == originalRequirements,
      "rendering mutated the pet's cultivation requirements");

  QTableView view;
  view.setFont(QFont(QStringLiteral("Microsoft YaHei UI"),10));
  view.setModel(&model);
  view.setItemDelegateForColumn(AssetAnalysisModel::Materials,new CultivationMaterialDelegate(&view));
  for (int column = 0; column < AssetAnalysisModel::ColumnCount; ++column)
    if (column != AssetAnalysisModel::Pet && column != AssetAnalysisModel::Materials) view.hideColumn(column);
  view.setColumnWidth(AssetAnalysisModel::Pet,235);
  view.setColumnWidth(AssetAnalysisModel::Materials,390);
  view.verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  view.verticalHeader()->hide();
  view.setShowGrid(false); view.setAlternatingRowColors(true);
  view.setSelectionBehavior(QAbstractItemView::SelectRows);
  view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  view.resize(660,330); view.show(); app.processEvents();
  QStyleOptionViewItem option; option.font = view.font();
  const int expectedHeight = view.itemDelegateForColumn(AssetAnalysisModel::Materials)->sizeHint(option,materialIndex).height();
  check(view.rowHeight(0) >= expectedHeight,"multi-category materials were clipped by the pet row height");
  view.grab().save(QStringLiteral("cultivation-pet-materials.png"));
  view.selectRow(0); app.processEvents();
  view.grab().save(QStringLiteral("cultivation-pet-materials-selected.png"));
  view.clearSelection();
  const int wideHeight = view.rowHeight(0);
  view.setColumnWidth(AssetAnalysisModel::Materials,235); app.processEvents();
  check(view.rowHeight(0) > wideHeight && view.rowHeight(0) >=
      view.itemDelegateForColumn(AssetAnalysisModel::Materials)->sizeHint(option,materialIndex).height(),
      "narrowing the material column did not expand the row for wrapped names");
  view.grab().save(QStringLiteral("cultivation-pet-materials-narrow.png"));
  model.setCultivationCategory(QStringLiteral("astrolabe"));
  check(model.data(materialIndex).toString() == QStringLiteral("星轮\n星迹精华 × 120"),"category switch retained stale material groups");
  overview.pets[0].cultivationRequirements.items[1].materials[0].count = 7;
  model.setOverview(overview);
  check(model.data(materialIndex).toString().contains(QStringLiteral("× 7")) && !model.data(materialIndex).toString().contains("120"),
      "new analysis reused stale material quantities");
  auto legacy = underlying.materials[1]; legacy.name = QStringLiteral("星灵精华");
  check(!showCultivationMaterial(legacy),"an old label bypassed the official material identity exclusion");

  auto breakthrough = displayPet;
  breakthrough.name = QStringLiteral("仅缺星轮突破的精灵");
  breakthrough.pet = {{"_metaEra",QStringLiteral("灵初")},{"_metaRating","S"}};
  breakthrough.cultivationRequirements.items = {{"astrolabe_breakthrough","astrolabe",QStringLiteral("星轮突破"),QStringLiteral("未突破"),{},true}};
  breakthrough.gapKeys = {"sg_changeable","badge_awaken","asv","astrolabe_breakthrough"};
  auto starNodes = displayPet; starNodes.instanceId = 105;
  starNodes.name = QStringLiteral("需要星轮材料的精灵"); starNodes.pet = breakthrough.pet;
  starNodes.gapKeys = {"sg_changeable","badge_awaken","asv","astrolabe_light"};
  overview.pets = {breakthrough,starNodes}; model.setOverview(overview);
  view.showColumn(AssetAnalysisModel::Gaps);
  view.showColumn(AssetAnalysisModel::Era); view.showColumn(AssetAnalysisModel::Rating);
  view.horizontalHeader()->moveSection(view.horizontalHeader()->visualIndex(AssetAnalysisModel::Era),1);
  view.horizontalHeader()->moveSection(view.horizontalHeader()->visualIndex(AssetAnalysisModel::Rating),2);
  view.horizontalHeader()->moveSection(view.horizontalHeader()->visualIndex(AssetAnalysisModel::Materials),3);
  view.setColumnWidth(AssetAnalysisModel::Materials,330); view.setColumnWidth(AssetAnalysisModel::Gaps,180);
  view.setColumnWidth(AssetAnalysisModel::Era,60); view.setColumnWidth(AssetAnalysisModel::Rating,60);
  view.resize(900,200); app.processEvents();
  check(model.rowCount() == 2 && model.data(model.index(0,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("星轮未突破") &&
      model.data(model.index(1,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("星轮未点满"),
      "category preview dropped state-only pets or displayed unrelated gaps");
  view.grab().save(QStringLiteral("cultivation-category-gap-preview.png"));
  return ok ? 0 : 1;
}
