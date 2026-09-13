#include "domain/pet_cultivation_requirements.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <iostream>

namespace {
bool check(bool condition,const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}
QJsonObject material(int type,int id,int count) { return {{"type",type},{"id",id},{"count",count}}; }
QJsonObject metadata() {
  return {{"pets",QJsonObject{{"7001",QJsonObject{{"name",QStringLiteral("[灵初]测试精灵")}}}}},
    {"badges",QJsonObject{
      {"101",QJsonObject{{"name",QStringLiteral("神攻·夯实基础")},{"maxLevel",4},{"levels",QJsonObject{
          {"1",QJsonObject{{"cost",QJsonArray{material(4,500,100)}}}},
          {"2",QJsonObject{{"cost",QJsonArray{material(4,500,100)}}}},
          {"3",QJsonObject{{"cost",QJsonArray{material(4,500,2),material(8,1,999),material(4,900,77)}}}},
          {"4",QJsonObject{{"cost",QJsonArray{material(4,500,3)}}}}}}}},
      {"201",QJsonObject{{"name",QStringLiteral("专属·勇毅")},{"maxLevel",1},{"activationCost",QJsonArray{material(4,501,1)}}}}}},
    {"items",QJsonObject{{"500",QJsonObject{{"name",QStringLiteral("神攻元魂")}}},
        {"501",QJsonObject{{"name",QStringLiteral("勇毅元魂")}}},{"900",QJsonObject{{"name",QStringLiteral("金币礼包")}}},
        {"2076",QJsonObject{{"name",QStringLiteral("星迹精华")}}},{"2078",QJsonObject{{"name",QStringLiteral("星灵精华")}}}}},
    {"sacredEquipment",QJsonObject{{"10",QJsonObject{{"name",QStringLiteral("天穹神源兽")},{"sourceId",88},{"sourceName",QStringLiteral("天穹源兽")}}}}},
    {"sacredStarPlans",QJsonObject{{"3",QJsonObject{{"maxLevel",9}}}}},
    {"sacredStagePlans",QJsonObject{{"24",QJsonObject{{"maxLevel",7},{"levels",QJsonObject{
        {"3",QJsonObject{{"equipmentCount",2}}},{"4",QJsonObject{{"equipmentCount",0}}},
        {"5",QJsonObject{{"equipmentCount",2}}},{"6",QJsonObject{{"equipmentCount",1}}}}}}}}},
    {"astrolabe",QJsonObject{
        {"0",QJsonObject{{"name",QStringLiteral("核心")},{"locatedType",0},{"lightUpCost",""}}},
        {"1",QJsonObject{{"name",QStringLiteral("连结")},{"locatedType",1},{"lightUpCost","8:33:6000#4:2076:100"}}},
        {"350",QJsonObject{{"name",QStringLiteral("星灵·暴击")},{"locatedType",2},{"lightUpCost","8:33:4000#4:2078:100"}}},
        {"351",QJsonObject{{"name",QStringLiteral("星灵·生命")},{"locatedType",2},{"lightUpMaterials",QJsonArray{material(4,2078,100)}}}},
        {"352",QJsonObject{{"name",QStringLiteral("星灵·速度")},{"locatedType",2},{"lightUpCost",""}}}}}};
}
QJsonObject pet() {
  return {{"r",7001},{"n",QStringLiteral("测试皮肤")},{"badge","101:2#201:0"},{"shenjue","10#3#24|7:3"},
      {"astrolabe","0:1|1:0#350:0:0|351:0:0"},{"astrolabebr",false},{"lv",120}};
}
PetBattlePowerState power() {
  PetBattlePowerState value;
  value.completionKnown = true; value.stargodSlotsKnown = true; value.stargodQualitiesKnown = true;
  value.stargodBackpackKnown = true; value.stargodSlots = 7; value.missingRedStars = 2;
  value.hasChangeableSlot = true; value.changeableOwnedKnown = true; value.changeableOwnedRed = false;
  value.stargodMaxLevel = 8; value.stargodLevelMissingSlots = 3;
  return value;
}
const PetCultivationRequirement* find(const PetCultivationRequirements& rows,const QString& key) {
  for (const auto& row : rows.items) if (row.key == key) return &row;
  return nullptr;
}
const PetCultivationMaterial* findMaterial(const PetCultivationRequirement* row,int type,int id) {
  if (row) for (const auto& item : row->materials) if (item.type == type && item.id == id) return &item;
  return nullptr;
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;
  auto m = metadata(); auto p = pet(); auto state = power();
  const auto value = calculatePetCultivationRequirements(p,m,state);
  const auto* ordinary = find(value,QStringLiteral("badge_level_1"));
  const auto* soul = findMaterial(ordinary,4,500);
  ok &= check(ordinary && ordinary->known && ordinary->name == QStringLiteral("神攻·夯实基础") &&
      ordinary->status.contains(QStringLiteral("还差 2 级")) && soul && soul->known && soul->count == 5 && ordinary->materials.size() == 1,
      "badge consumption must use the next target levels and omit unrelated coins/items");
  const auto* awake = find(value,QStringLiteral("badge_awaken_1"));
  const auto* awakeCost = findMaterial(awake,4,501);
  ok &= check(awake && awake->status == QStringLiteral("未觉醒") && awakeCost && awakeCost->count == 1 && awakeCost->name == QStringLiteral("勇毅元魂"),
      "exclusive awakening must use its actual item cost, never the badge ID as a fabricated material ID");
  const auto* beast = find(value,QStringLiteral("sacred"));
  const auto* beastCost = findMaterial(beast,24,88);
  ok &= check(beast && beast->known && beast->status.contains(QStringLiteral("还差 2 星")) && beast->status.contains(QStringLiteral("还差 4 阶")) &&
      beastCost && beastCost->known && beastCost->extra == 1 && beastCost->count == 5 && beastCost->name == QStringLiteral("天穹源兽"),
      "source beasts use the raw plans, current-to-next stage costs and source ID, including zero and multiple-beast stages");
  const auto* light = find(value,QStringLiteral("astrolabe_light"));
  const auto* essence = findMaterial(light,4,2078);
  ok &= check(light && light->known && light->materials.size() == 2 && essence && essence->known && essence->count == 200 &&
      findMaterial(light,4,2076)->count == 100 && find(value,QStringLiteral("astrolabe_breakthrough")),
      "unlit node essence must aggregate once per node, omit currencies and retain a separate breakthrough task");
  for (const auto& era : {QStringLiteral("神运"),QStringLiteral("星迹"),QStringLiteral("启元")}) {
    auto eraMetadata = m;
    eraMetadata.insert(QStringLiteral("pets"),QJsonObject{{QStringLiteral("7001"),
        QJsonObject{{QStringLiteral("name"),QStringLiteral("[%1]测试精灵").arg(era)}}}});
    auto earlierPet = p;
    earlierPet.insert(QStringLiteral("n"),QStringLiteral("[灵初]误导的皮肤名字"));
    auto earlier = calculatePetCultivationRequirements(earlierPet,eraMetadata,state);
    ok &= check(find(earlier,QStringLiteral("astrolabe_light")) && !find(earlier,QStringLiteral("astrolabe_breakthrough")),
        "non-Lingchu pets must retain unlit-node needs without inventing an astrolabe breakthrough");
    earlierPet.insert(QStringLiteral("astrolabebr"),true);
    earlierPet.insert(QStringLiteral("astrolabe"),QStringLiteral("0:1|350:1:1|351:1:0|352:1:0"));
    earlier = calculatePetCultivationRequirements(earlierPet,eraMetadata,state);
    ok &= check(!find(earlier,QStringLiteral("astrolabe_breakthrough")) && !find(earlier,QStringLiteral("astrolabe_select")),
        "a non-Lingchu raw breakthrough flag must not produce a post-breakthrough equipment task");
  }
  auto unknownEraMetadata = m; unknownEraMetadata.remove(QStringLiteral("pets"));
  auto unknownEra = calculatePetCultivationRequirements(p,unknownEraMetadata,state);
  ok &= check(!find(unknownEra,QStringLiteral("astrolabe_breakthrough")) && find(unknownEra,QStringLiteral("astrolabe_light")),
      "unknown eras must not show an invented unbroken state");
  auto lingchuSelection = p;
  lingchuSelection.insert(QStringLiteral("astrolabebr"),true);
  lingchuSelection.insert(QStringLiteral("astrolabe"),QStringLiteral("0:1|350:1:1|351:1:0|352:1:0"));
  ok &= check(find(calculatePetCultivationRequirements(lingchuSelection,m,state),QStringLiteral("astrolabe_select")),
      "Lingchu must retain the actual post-breakthrough equipment requirement");
  const auto* red = find(value,QStringLiteral("stargod_red"));
  const auto* movable = find(value,QStringLiteral("stargod_changeable"));
  ok &= check(red && red->status.contains(QStringLiteral("2 个不同种类")) && red->materials.isEmpty() && movable && movable->materials.isEmpty() &&
      find(value,QStringLiteral("stargod_level")) && !value.complete,
      "ordinary reds, changeable red and slot levels must be separate aggregated tasks, never global inventory costs");

  state.missingRedStars = 0; state.changeableOwnedRed = true; state.stargodLevelMissingSlots = 0; state.stargodLevelsFull = true;
  state.stargodPowerKnown = true; state.adjustmentGain = 150;
  auto owned = calculatePetCultivationRequirements(p,m,state);
  ok &= check(!find(owned,QStringLiteral("stargod_red")) && !find(owned,QStringLiteral("stargod_changeable")) && find(owned,QStringLiteral("stargod_equip")),
      "owned backpack red stars must remove acquisition requirements while retaining the equipment task");

  auto badgeDefinitions = m.value("badges").toObject(); auto badge = badgeDefinitions.value("101").toObject();
  auto levels = badge.value("levels").toObject(); levels.remove("4"); badge.insert("levels",levels); badgeDefinitions.insert("101",badge); m.insert("badges",badgeDefinitions);
  auto unknownCost = calculatePetCultivationRequirements(p,m,state);
  const auto* partial = findMaterial(find(unknownCost,QStringLiteral("badge_level_1")),4,500);
  ok &= check(partial && !partial->known && find(unknownCost,QStringLiteral("badge_level_1"))->known,
      "known state with a missing official cost must not report a partial subtotal as an exact requirement");

  m = metadata(); auto plans = m.value("sacredStagePlans").toObject(); auto plan = plans.value("24").toObject();
  auto stages = plan.value("levels").toObject(); stages.remove("5"); plan.insert("levels",stages); plans.insert("24",plan); m.insert("sacredStagePlans",plans);
  unknownCost = calculatePetCultivationRequirements(p,m,state);
  const auto* unknownBeast = findMaterial(find(unknownCost,QStringLiteral("sacred")),24,88);
  ok &= check(unknownBeast && !unknownBeast->known,
      "missing source-beast cost must remain unknown rather than assuming one beast per stage");

  m = metadata(); p.insert("badge","101:4#201:1"); p.insert("shenjue","10#3#24|9:7");
  p.insert("astrolabe","0:1|350:1:1|351:1:1|352:1:1"); p.insert("astrolabebr",true);
  state.adjustmentGain = 0; state.isHighest = true;
  const auto complete = calculatePetCultivationRequirements(p,m,state);
  ok &= check(complete.completeKnown && complete.complete && complete.items.isEmpty(),
      "completed cultivation must not generate empty or repetitive rows");
  p.insert("badge",QJsonValue(QJsonValue::Null)); p.insert("shenjue","10#3#24|99:7");
  const auto malformed = calculatePetCultivationRequirements(p,m,state);
  ok &= check(!malformed.completeKnown && !malformed.complete && find(malformed,QStringLiteral("badge_state")) &&
      find(malformed,QStringLiteral("sacred")) && !find(malformed,QStringLiteral("sacred"))->known,
      "missing and out-of-range cultivation data cannot produce a fully cultivated assertion");
  if (ok) std::cout << "pet cultivation requirement checks passed\n";
  return ok ? 0 : 1;
}
