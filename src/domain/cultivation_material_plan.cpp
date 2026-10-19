#include "cultivation_material_plan.h"
#include "checked_json_numbers.h"
#include <QMap>

namespace {
struct Demand {
  int type = 0, id = 0, extra = 0;
  QString category, name, scope;
  qint64 count = 0;
  bool complete = true;
  QMap<qint64,QPair<QString,qint64>> pets;
};
}

QString cultivationMaterialCategoryName(const QString& key) {
  if (key == "sacred") return QStringLiteral("神源兽");
  if (key == "astrolabe") return QStringLiteral("星轮精华");
  if (key == "badges") return QStringLiteral("元魂");
  if (key == "stargods") return QStringLiteral("红星");
  return QStringLiteral("共享材料");
}

CultivationMaterialPlan buildCultivationMaterialPlan(const QList<const PetAssetRecord*>& pets, const QString& category,
                                                     const MaterialInventorySnapshot& inventory) {
  QMap<QString,Demand> demands;
  QMap<QString,int> names;
  QSet<qint64> unknown, seen;
  for (const auto* pet : pets) if (pet) ++names[pet->name];
  for (const auto* pet : pets) {
    if (!pet || seen.contains(pet->instanceId)) continue;
    seen.insert(pet->instanceId);
    if (!pet->detailAvailable) { unknown.insert(pet->instanceId); continue; }
    const auto add = [&](const QString& group, const PetCultivationMaterial& item) {
      if (item.known && item.count == 0) return;
      const QString key = item.scope + ':' + cultivationMaterialKey(item.type,item.id,item.extra) +
          (item.id > 0 ? QString{} : ':' + item.name);
      auto& row = demands[key];
      if (row.category.isEmpty()) row.category = group;
      else if (row.category != group) row.category = "shared";
      row.type = item.type; row.id = item.id; row.extra = item.extra; row.name = item.name; row.scope = item.scope;
      row.complete &= item.known && item.count >= 0;
      if (!item.known || item.count < 0) { unknown.insert(pet->instanceId); return; }
      qint64 total = 0;
      if (!DomainNumeric::checkedAdd(row.count,item.count,&total)) { row.complete = false; unknown.insert(pet->instanceId); return; }
      row.count = total;
      auto& contribution = row.pets[pet->instanceId]; contribution.first = pet->name;
      contribution.second += item.count;
    };
    if (category.isEmpty() || category == "stargods") {
      if (!pet->redStarKnown) unknown.insert(pet->instanceId);
      if (pet->redStarKnown && pet->missingRedStars > 0)
        add("stargods",{0,0,0,QStringLiteral("普通红星"),pet->missingRedStars,true,QStringLiteral("pet")});
      if (pet->gapKeys.contains("sg_changeable"))
        add("stargods",{0,0,0,QStringLiteral("万变红星"),1,true,QStringLiteral("pet")});
    }
    for (const auto& requirement : pet->cultivationRequirements.items) {
      if (!category.isEmpty() && requirement.category != category) continue;
      if (requirement.category == "stargods" && requirement.key != "stargod_red" &&
          requirement.key != "stargod_changeable" && requirement.key != "stargod_state") continue;
      if (!requirement.known) unknown.insert(pet->instanceId);
      for (const auto& material : requirement.materials) add(requirement.category,material);
    }
  }
  CultivationMaterialPlan plan;
  plan.category = category;
  plan.petCount = seen.size();
  plan.unknownPetCount = unknown.size();
  plan.observedAt = inventory.observedAt;
  for (const auto& group : {QStringLiteral("sacred"),QStringLiteral("astrolabe"),QStringLiteral("badges"),QStringLiteral("stargods"),QStringLiteral("shared")}) {
    for (const auto& row : demands) {
      if (row.category != group) continue;
      CultivationMaterialPlanRow material;
      material.type = row.type; material.id = row.id; material.extra = row.extra;
      material.category = row.category; material.name = row.name; material.scope = row.scope;
      material.required = row.count; material.complete = row.complete;
      for (auto it = row.pets.cbegin(); it != row.pets.cend(); ++it)
        material.pets.append({it.key(),it->first,it->second,it->first.isEmpty() || names.value(it->first) > 1});
      if (row.scope == "pet") material.missing = row.count;
      else {
        material.owned = row.scope == "account" ? cultivationMaterialOwned(inventory,row.type,row.id,row.extra) : std::nullopt;
        if (material.owned && row.complete) material.missing = qMax<qint64>(0,row.count - *material.owned);
      }
      plan.materials.append(material);
    }
  }
  return plan;
}

QString cultivationMaterialPlanSummary(const CultivationMaterialPlan& plan) {
  QStringList sections;
  for (const auto& group : {QStringLiteral("sacred"),QStringLiteral("astrolabe"),QStringLiteral("badges"),QStringLiteral("stargods"),QStringLiteral("shared")}) {
    QStringList lines;
    for (const auto& row : plan.materials) {
      if (row.category != group) continue;
      QStringList contributors;
      for (const auto& pet : row.pets) {
        QString label = pet.name.isEmpty() ? QStringLiteral("未命名精灵") : pet.name;
        if (pet.distinguishInstance) label += QStringLiteral("（实例 %1）").arg(pet.instanceId);
        if (pet.count > 1) label += QStringLiteral("（需 %1）").arg(pet.count);
        contributors.append(label);
      }
      QString line = QStringLiteral("%1 × %2：%3").arg(row.name).arg(row.required).arg(contributors.join(QStringLiteral("、")));
      if (row.scope == "pet") line += QStringLiteral("\n  已计入各精灵自身持有，还缺 %1；不扣共享库存").arg(row.required);
      else {
        line += QStringLiteral("\n  已持有 %1；%2 %3").arg(row.owned ? QString::number(*row.owned) : QStringLiteral("未读取"),
            plan.unknownPetCount == 0 ? QStringLiteral("还缺") : QStringLiteral("已知缺口"),
            row.missing ? QString::number(*row.missing) : QStringLiteral("待确认"));
        if (!row.complete) line += QStringLiteral("；需求尚不完整");
      }
      lines.append(line);
    }
    if (!lines.isEmpty()) sections.append(cultivationMaterialCategoryName(group) + QStringLiteral("\n") + lines.join(QLatin1Char('\n')));
    else if (group != "shared" && (plan.category.isEmpty() || plan.category == group))
      sections.append(cultivationMaterialCategoryName(group) + QStringLiteral("：已知缺口 0"));
  }
  if (sections.isEmpty()) sections.append(QStringLiteral("已知缺口 0"));
  if (plan.unknownPetCount > 0) sections.append(QStringLiteral("另有 %1 只信息不足，已知缺口不代表全部需求").arg(plan.unknownPetCount));
  if (plan.observedAt.isValid()) sections.append(QStringLiteral("材料最近读取：%1（消耗或获得材料后请刷新）")
      .arg(plan.observedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
  return sections.join(QLatin1Char('\n'));
}

QString cultivationMaterialPlanSummary(const QList<const PetAssetRecord*>& pets, const QString& category,
                                       const MaterialInventorySnapshot& inventory) {
  return cultivationMaterialPlanSummary(buildCultivationMaterialPlan(pets,category,inventory));
}
