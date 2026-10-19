#include "ui/analysis/asset_analysis_model.h"
#include "ui/analysis/asset_analysis_filter_proxy_model.h"
#include "domain/pet_cultivation_requirements.h"
#include "domain/asset_derivation.h"
#include "domain/cultivation_material_plan.h"
#include <QCoreApplication>
#include <cstdio>

int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;
  const auto check = [&](bool value,const char* message) { if (!value) { ok = false; std::fprintf(stderr,"FAIL: %s\n",message); } };
  PetAssetRecord first; first.instanceId = 1; first.name = QStringLiteral("灵初测试甲"); first.detailAvailable = true;
  first.pet = {{"_metaEra",QStringLiteral("灵初")},{"_metaRating","SP"}};
  first.redStarKnown = true; first.missingRedStars = 2; first.gapKeys = {"sg_changeable"};
  first.cultivationRequirements.items = {
    {"sacred","sacred",QStringLiteral("神源兽"),"",{{24,88,1,QStringLiteral("天穹源兽"),5,true}},true},
    {"light","astrolabe",QStringLiteral("星轮"),"",{{4,2076,0,QStringLiteral("星迹精华"),100,true},
       {4,2078,0,QStringLiteral("星灵精华"),200,true}},true},
    {"badge","badges",QStringLiteral("元魂"),"",{{4,500,0,QStringLiteral("神攻元魂"),5,true}},true},
    {"stargod_red","stargods",QStringLiteral("普通红星"),"",{},true}};
  auto second = first; second.instanceId = 2; second.name = QStringLiteral("神运测试乙");
  second.pet = {{"_metaEra",QStringLiteral("神运")},{"_metaRating","SSS"}};
  auto third = first; third.instanceId = 3; third.name = QStringLiteral("灵初测试丙"); third.pet.insert("_metaRating","SS");
  third.cultivationRequirements.items[1].materials[0].known = false;
  PetAssetRecord missing; missing.instanceId = 4; missing.name = QStringLiteral("详情缺失"); missing.pet = first.pet;
  AccountAssetOverview overview; overview.pets = {first,second,third,missing}; overview.totalPets = 4;
  AssetAnalysisModel model; model.setOverview(overview);
  AssetAnalysisFilterProxyModel proxy; proxy.setSourceModel(&model);
  proxy.setCultivationFilters("sacred",QStringLiteral("灵初"),"SP");
  check(proxy.rowCount() == 2,"era, rating and category intersect while missing details remain visible");
  auto summary = proxy.materialSummary();
  check(summary.contains(QStringLiteral("天穹源兽 × 5")) && summary.contains(QStringLiteral("1 只信息不足")),"unknown instances do not become known zero or inflate the known total");
  check(summary.contains(QStringLiteral("天穹源兽 × 5：灵初测试甲（需 5）")) &&
      !summary.contains(second.name) && !summary.contains(third.name) && !summary.contains(missing.name),
      "material contributors must be the known quantities in the same filtered result");
  proxy.setQuery(QStringLiteral("测试甲"));
  check(proxy.rowCount() == 1 && !proxy.materialSummary().contains(QStringLiteral("信息不足")),"search changes totals on the same filtered rows");
  proxy.setQuery({}); proxy.setCultivationFilters("astrolabe",{},{});
  summary = proxy.materialSummary();
  check(summary.contains(QStringLiteral("星迹精华 × 200")) && summary.contains(QStringLiteral("星灵精华 × 600")) &&
      summary.contains(QStringLiteral("2 只信息不足")),"essence types aggregate independently and partial counts stay explicit");
  const auto lines = summary.split(QLatin1Char('\n'));
  const auto starLine = lines.filter(QStringLiteral("星迹精华 ×")).value(0);
  check(starLine.contains(first.name) && starLine.contains(second.name) && !starLine.contains(third.name),
      "partially unknown pet must not be named for an unknown material quantity");
  proxy.sort(0,Qt::DescendingOrder);
  check(proxy.materialSummary() == summary,"sorting must not change contributor identities or totals");
  proxy.setCultivationFilters("stargods",QStringLiteral("神运"),"SSS");
  check(proxy.rowCount() == 1 && proxy.materialSummary().contains(QStringLiteral("普通红星 × 2")) &&
      proxy.materialSummary().contains(QStringLiteral("万变红星 × 1")),"ordinary and changeable red stars remain distinct");
  proxy.setCultivationFilters("badges",QStringLiteral("灵初"),"A");
  check(proxy.rowCount() == 0 && proxy.materialSummary().contains(QStringLiteral("已知缺口 0")),"empty selection clears old totals");
  auto json = cultivationRequirementsToJson(first.cultivationRequirements);
  const auto restored = cultivationRequirementsFromJson(json);
  check(restored && cultivationRequirementsToJson(*restored) == json,"material facts survive cache serialization");
  json.insert("completeKnown",QStringLiteral("false"));
  check(!cultivationRequirementsFromJson(json),"malformed material knownness is rejected");
  auto compact = third;
  compact.cultivationRequirements = compactCultivationRequirements(third.cultivationRequirements);
  check(AssetDerivation::cultivationMaterialSummary({&compact}) == AssetDerivation::cultivationMaterialSummary({&third}),
      "compact category facts changed known quantities or missing-information totals");
  auto sameName = first; sameName.instanceId = 5;
  sameName.cultivationRequirements.items[0].materials[0].count = 1;
  auto repeat = sameName.cultivationRequirements.items[0]; repeat.materials[0].count = 2;
  sameName.cultivationRequirements.items.append(repeat);
  summary = AssetDerivation::cultivationMaterialSummary({&first,&sameName},"sacred",true);
  check(summary.contains(QStringLiteral("天穹源兽 × 8：")) &&
      summary.contains(QStringLiteral("灵初测试甲（实例 1）（需 5）")) &&
      summary.contains(QStringLiteral("灵初测试甲（实例 5）（需 3）")) &&
      summary.count(QStringLiteral("实例 5")) == 1,
      "same-name pets need distinct identities and repeated requirements need one summed contribution");
  sameName.detailAvailable = false;
  summary = AssetDerivation::cultivationMaterialSummary({&sameName},"sacred",true);
  check(summary.contains(QStringLiteral("已知缺口 0")) && !summary.contains(QStringLiteral("天穹源兽")),
      "stale material facts on a pet without detail must not become known totals");
  auto concise = first; concise.cultivationKnown = true;
  concise.gapKeys = {"sgv","sg_acquire","sg_changeable"};
  concise.gaps = {QStringLiteral("星神 +500"),QStringLiteral("缺少可用普通红星 2 个种类（已含本宠背包）")};
  overview.pets = {concise}; model.setOverview(overview);
  const auto gapIndex = model.index(0,AssetAnalysisModel::Gaps);
  check(model.data(gapIndex).toString() == QStringLiteral("缺少万变红星；缺少2颗普通红星"),
      "primary gap display should say what is missing, without power arithmetic or duplicate component labels");
  check(model.data(gapIndex,Qt::ToolTipRole).toString() == concise.gaps.join(QLatin1Char('\n')),
      "concise display must preserve detailed evidence in the tooltip");
  concise.redStarKnown = false; concise.cultivationKnown = false;
  concise.gapKeys = {"sg_acquire"}; overview.pets = {concise}; model.setOverview(overview);
  check(!model.data(gapIndex).toString().contains(QStringLiteral("2颗")) &&
      model.data(gapIndex).toString().contains(QStringLiteral("部分信息待补")),
      "unknown red-star quantities must not be presented as known missing items");
  concise.redStarKnown = true; concise.missingRedStars = 0; concise.cultivationKnown = true;
  concise.gapKeys = {"sjv","sacred_star","sacred_stage","bsv","badge_awaken","asv","astrolabe_breakthrough"};
  overview.pets = {concise}; model.setOverview(overview);
  check(model.data(gapIndex).toString() == QStringLiteral("神源兽星级未满；神源兽阶级未满；专属元魂未觉醒；星轮未突破"),
      "specific cultivation gaps must replace duplicate broad component labels");
  MaterialInventorySnapshot stock;
  stock.counts.insert(cultivationMaterialKey(24,88,1),6);
  stock.knownExtraGroups.insert("24:1");
  auto plan = cultivationMaterialPlanSummary({&first,&second},"sacred",stock);
  check(plan.contains(QStringLiteral("天穹源兽 × 10")) && plan.contains(QStringLiteral("已持有 6；还缺 4")) &&
      plan.contains(first.name) && plan.contains(second.name),"shared stock was not subtracted exactly once from selected demand");
  plan = cultivationMaterialPlanSummary({&first},"sacred",stock);
  check(plan.contains(QStringLiteral("已持有 6；还缺 0")),"surplus stock produced a negative shortage");
  plan = cultivationMaterialPlanSummary({&first},"sacred",{});
  check(plan.contains(QStringLiteral("已持有 未读取；还缺 待确认")),"unread inventory was silently treated as zero");
  stock.counts.clear();
  plan = cultivationMaterialPlanSummary({&first},"sacred",stock);
  check(plan.contains(QStringLiteral("已持有 0；还缺 5")),"absence from a completely decoded extra group should be a known zero");
  stock.knownExtraGroups.clear(); stock.knownTypes.insert(24);
  check(cultivationMaterialPlanSummary({&first},"sacred",stock).contains(QStringLiteral("还缺 待确认")),
      "unproven material variants borrowed the generic inventory quantity");
  auto otherCategory = first; otherCategory.instanceId = 10; otherCategory.name = QStringLiteral("跨类材料测试");
  otherCategory.cultivationRequirements.items = {first.cultivationRequirements.items[0]};
  otherCategory.cultivationRequirements.items[0].category = "badges";
  stock.counts.insert(cultivationMaterialKey(24,88,1),6);
  plan = cultivationMaterialPlanSummary({&first,&otherCategory},{},stock);
  check(plan.count(QStringLiteral("天穹源兽 × 10")) == 1 && plan.contains(QStringLiteral("已持有 6；还缺 4")),
      "same account material appearing in two categories spent the stock twice");
  stock.counts.insert("ordinary",100);
  plan = cultivationMaterialPlanSummary({&first},"stargods",stock);
  check(plan.contains(QStringLiteral("普通红星 × 2")) && plan.contains(QStringLiteral("还缺 2；不扣共享库存")),
      "pet-bound star deficits were reduced by account-wide inventory");
  overview.pets = {first,second}; model.setOverview(overview); proxy.setQuery({}); proxy.setCultivationFilters("sacred",{},{});
  plan = proxy.materialPlan(stock,{first.instanceId},true);
  check(plan.contains(QStringLiteral("天穹源兽 × 5")) && !plan.contains(second.name),"selected-only planning included unselected pets");
  check(proxy.materialPlan(stock,{},true).contains(QStringLiteral("请在下方列表选择")),"empty selection silently planned the whole account");

  // A category scopes the explanation, not the underlying cultivation result
  // or membership: breakthrough-only pets still belong to the star-wheel view.
  auto breakthrough = first;
  breakthrough.gapKeys = {"sg_changeable","sg_acquire","badge_awaken","asv","astrolabe_breakthrough"};
  breakthrough.gaps = {QStringLiteral("原始红星依据"),QStringLiteral("原始元魂依据"),QStringLiteral("原始星轮依据")};
  breakthrough.cultivationKnown = true;
  breakthrough.cultivationRequirements.items = {{"astrolabe_breakthrough","astrolabe",QStringLiteral("星轮突破"),QStringLiteral("未突破"),{},true}};
  auto nodes = second; nodes.pet = first.pet;
  nodes.gapKeys = {"sg_changeable","asv","astrolabe_light"}; nodes.cultivationKnown = true;
  auto otherEra = nodes; otherEra.instanceId = 20; otherEra.pet.insert("_metaEra",QStringLiteral("神运"));
  auto otherRating = nodes; otherRating.instanceId = 21; otherRating.pet.insert("_metaRating","S");
  overview.pets = {breakthrough,nodes,otherEra,otherRating}; model.setOverview(overview);
  proxy.setCultivationFilters("astrolabe",QStringLiteral("灵初"),"SP");
  model.setCultivationCategory("astrolabe");
  check(proxy.rowCount() == 2 && proxy.materialPlanDetails({}).petCount == 2,
      "category scoping removed breakthrough-only pets or changed era/rating membership");
  check(model.data(model.index(0,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("星轮未突破") &&
        model.data(model.index(1,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("星轮未点满"),
      "star-wheel gap cells leaked red-star or badge gaps");
  check(model.data(model.index(0,AssetAnalysisModel::Gaps),Qt::ToolTipRole).toString() == QStringLiteral("星轮未突破") &&
        model.headerData(AssetAnalysisModel::Gaps,Qt::Horizontal,Qt::DisplayRole).toString() == QStringLiteral("星轮缺口"),
      "gap tooltip/header did not follow the selected category");
  check(model.petAt(0)->gapKeys == breakthrough.gapKeys && model.petAt(0)->gaps == breakthrough.gaps,
      "category display modified the original cultivation evidence");
  model.setCultivationCategory("badges");
  check(model.data(model.index(0,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("专属元魂未觉醒"),
      "badge category retained unrelated gap text");
  model.setCultivationCategory("stargods");
  check(model.data(model.index(0,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("缺少万变红星；缺少2颗普通红星"),
      "red-star category changed the existing quantities or kept unrelated gaps");
  auto sacred = breakthrough; sacred.gapKeys.append("sacred_stage");
  overview.pets[0] = sacred; model.setOverview(overview); model.setCultivationCategory("sacred");
  check(model.data(model.index(0,AssetAnalysisModel::Gaps)).toString() == QStringLiteral("神源兽阶级未满"),
      "sacred category did not scope its explanation");
  model.setCultivationCategory({});
  const auto allGaps = model.data(model.index(0,AssetAnalysisModel::Gaps)).toString();
  check(allGaps.contains(QStringLiteral("缺少2颗普通红星")) && allGaps.contains(QStringLiteral("专属元魂未觉醒")) &&
        allGaps.contains(QStringLiteral("星轮未突破")) && allGaps.contains(QStringLiteral("神源兽阶级未满")) &&
        model.data(model.index(0,AssetAnalysisModel::Gaps),Qt::ToolTipRole).toString() == sacred.gaps.join(QLatin1Char('\n')),
      "switching back to all cultivation did not restore complete gap evidence");
  return ok ? 0 : 1;
}
