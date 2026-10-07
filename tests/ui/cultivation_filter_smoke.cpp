#include "ui/analysis/asset_analysis_model.h"
#include "ui/analysis/asset_analysis_filter_proxy_model.h"
#include "domain/pet_cultivation_requirements.h"
#include "domain/asset_derivation.h"
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
  proxy.setQuery(QStringLiteral("测试甲"));
  check(proxy.rowCount() == 1 && !proxy.materialSummary().contains(QStringLiteral("信息不足")),"search changes totals on the same filtered rows");
  proxy.setQuery({}); proxy.setCultivationFilters("astrolabe",{},{});
  summary = proxy.materialSummary();
  check(summary.contains(QStringLiteral("星迹精华 × 200")) && summary.contains(QStringLiteral("星灵精华 × 600")) &&
      summary.contains(QStringLiteral("2 只信息不足")),"essence types aggregate independently and partial counts stay explicit");
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
  return ok ? 0 : 1;
}
