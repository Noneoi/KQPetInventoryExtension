#pragma once
#include "contracts/public_data_detail.h"
#include <QJsonArray>

// Synthetic namespace/retention fixture. It is not game evidence.
inline PublicDatasetSnapshotHandle publicDetailFixture(const QString& revision=QStringLiteral("first")) {
  auto data=std::make_shared<PublicDatasetSnapshot>(); data->revision=revision;
  auto detail=std::make_shared<PetDetailCatalogSnapshot>(); detail->loaded=true;
  detail->root=QJsonObject{{"pets",QJsonObject{
    {"1",QJsonObject{{"name",QStringLiteral("原始精灵")},{"groupRaceId",0}}},
    {"2",QJsonObject{{"name",QStringLiteral("外观名")},{"groupRaceId",1}}}}},
    {"stargods",QJsonObject{{"17",QJsonObject{{"name",QStringLiteral("同号星神")},{"description",QStringLiteral("星神定义")}}}}},
    {"astrolabe",QJsonObject{{"0",QJsonObject{{"name",QStringLiteral("零号星轮节点")}}}}},
    {"badges",QJsonObject{{"17",QJsonObject{{"name",QStringLiteral("同号元魂")}}}}},
    {"items",QJsonObject{{"1",QJsonObject{{"name",QStringLiteral("测试材料")}}}}}};
  data->detail=detail;
  auto skill=std::make_shared<PetSkillCatalogSnapshot>(); skill->loaded=true;
  skill->root=QJsonObject{{"source",QJsonObject{{"url","https://example.invalid/official-fixture"}}},
    {"pets",QJsonObject{{"2",QJsonObject{{"name",QStringLiteral("外观名")},{"slots",QJsonObject{{"normal",17},{"ultimate",18}}}}}}},
    {"skills",QJsonObject{{"17",QJsonObject{{"name",revision+QStringLiteral(" · H5技能")},{"description",QStringLiteral("[提升·25] 官方配置说明")},{"opaqueCode",987654321}}},
                          {"18",QJsonObject{{"name",QStringLiteral("超杀技能")},{"description",QStringLiteral("另一技能说明")}}}}},
    {"buffs",QJsonObject{{"17",QJsonObject{{"name",QStringLiteral("同号H5效果")},{"description",QStringLiteral("独立效果说明")}}},
                         {"88",QJsonObject{{"name",QStringLiteral("组合状态")}}}}},
    {"entries",QJsonObject{{QStringLiteral("提升"),QStringLiteral("提升ARG点")},{QStringLiteral("空说明"),QString()}}},
    {"combos",QJsonObject{{"17",QJsonArray{QJsonObject{{"buff",88},{"skill",18}}}}}}};
  data->skill=skill;
  auto skillPets=skill->root.value("pets").toObject(); auto owner=skillPets.value("2").toObject();
  owner.insert("starGodSkill",QJsonObject{{"name",QStringLiteral("精灵内嵌星神技")},{"description",QStringLiteral("按精灵字典内嵌记录读取")}});
  skillPets.insert("2",owner); skill->root.insert("pets",skillPets);
  auto replay=std::make_shared<BattleReplayCatalogSnapshot>(); replay->sha256=revision+"-flash-sha";
  replay->root=QJsonObject{{"skills",QJsonObject{{"17",QJsonObject{{"name",QStringLiteral("Flash技能")},{"description",QStringLiteral("Flash独立说明")}}}}},
    {"buffs",QJsonObject{{"17",QJsonObject{{"name",QStringLiteral("非官方标题字段")},{"nm",QStringLiteral("同号Flash效果")},{"mess",QStringLiteral("Flash状态原文")}}}}},
    {"pets",QJsonObject{{"2",QJsonObject{{"name",QStringLiteral("Flash外观")},
      {"identity",QJsonObject{{"status","skin"},{"canonicalName",QStringLiteral("Flash原始精灵")}}}}}}},
    {"petMechanics",QJsonObject{{"2",QJsonObject{{"skillLinks",QJsonArray{QJsonObject{{"skillId","17"},{"label",QStringLiteral("普通技")}}}},
      {"legendReferenceCandidates",QJsonArray{QJsonObject{{"id","999"}}}}}}}},
    {"replayCultivation",QJsonObject{{"stargods",QJsonObject{{"definitions",QJsonObject{
      {"17",QJsonObject{{"name",QStringLiteral("Flash星神")},{"description",QStringLiteral("星神独立说明")}}}}}}}}}};
  data->replay=replay;
  replay->root.insert("provenance",QJsonObject{{"packages",QJsonArray{QJsonObject{
      {"resource","battleconfig/battleconfig"},{"url","https://example.invalid/official-fixture.swf"}}}}});
  replay->root.insert("officialEntryMentions",QJsonObject{{"unresolved",QJsonObject{{"status","missing"},{"description",QStringLiteral("不能展示为已确认说明")}}}});
  replay->root.insert("relationsBySkill",QJsonObject{{"17",QJsonArray{"known","unclassified"}}});
  replay->root.insert("tableRelations",QJsonArray{
    QJsonObject{{"id","known"},{"kind","combo_buff_playback"},{"skillId","17"},{"buffId","17"},{"targetSkillId","18"}},
    QJsonObject{{"id","unclassified"},{"kind","unclassified"},{"skillId","17"},{"buffId","999"}}});
  auto shop=std::make_shared<ShopCatalogSnapshot>(); shop->loaded=true;
  for (const auto& source : {QStringLiteral("module-a"),QStringLiteral("module-b")}) {
    ShopExchangeGood good; good.shopId=1; good.itemServerId=1; good.tab=1; good.sourceKey=source;
    good.description=source+QStringLiteral(" 项目"); good.shopName=source; good.cost="4:1:5"; good.raceIds={1};
    ShopExchangeShop store; store.shopId=1; store.name=source; store.sourceKey=source; store.goods={good}; shop->allShops.append(store);
  }
  data->shop=shop;
  return data;
}
