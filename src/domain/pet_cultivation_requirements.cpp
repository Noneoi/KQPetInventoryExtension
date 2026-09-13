#include "pet_cultivation_requirements.h"
#include "pet_era.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <cmath>
#include <limits>

namespace {
bool integer(const QJsonValue& value, int* result, int minimum = 0, int maximum = 1000000000) {
  if (value.isDouble()) {
    const double number = value.toDouble();
    if (!std::isfinite(number) || number != std::floor(number) || number < minimum || number > maximum) return false;
    *result = int(number); return true;
  }
  if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 10) return false;
  const auto text = value.toString();
  for (const auto character : text) if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
  bool ok = false; const qlonglong number = text.toLongLong(&ok);
  if (!ok || number < minimum || number > maximum) return false;
  *result = int(number); return true;
}
QString entryName(const QJsonObject& section, int id, const QString& prefix) {
  const auto name = section.value(QString::number(id)).toObject().value(QStringLiteral("name")).toString();
  return name.isEmpty() ? QStringLiteral("%1 %2").arg(prefix).arg(id) : name;
}
struct Builder {
  const QJsonObject& pet;
  const QJsonObject& metadata;
  const PetBattlePowerState& power;
  PetCultivationRequirements result;
  bool statesKnown = true;

  const PetBattlePowerComponent* component(const QString& key) const {
    for (const auto& row : power.components) if (row.key == key) return &row;
    return nullptr;
  }
  bool positiveGap(const QString& key) const {
    const auto* row = component(key);
    return row && row->gapKnown && row->gap > 0;
  }
  void unknown(const QString& key, const QString& category, const QString& name, const QString& status) {
    result.items.append({key,category,name,status,{},false}); statesKnown = false;
  }
  static void appendMaterial(QList<PetCultivationMaterial>& target, PetCultivationMaterial material) {
    for (auto& existing : target) if (existing.type == material.type && existing.id == material.id &&
        existing.extra == material.extra && (existing.id > 0 || existing.name == material.name)) {
      if (qint64(existing.count) + material.count > std::numeric_limits<int>::max()) existing.known = false;
      else existing.count += material.count;
      existing.known = existing.known && material.known; return;
    }
    target.append(std::move(material));
  }
  static void unknownMaterial(QList<PetCultivationMaterial>& target, const QString& fallback) {
    if (target.isEmpty()) { PetCultivationMaterial item; item.name = fallback; target.append(item); }
    for (auto& item : target) item.known = false;
  }
  // Only the user-requested named soul or essence materials are shown. Other
  // official consumption (coins, generic experience, etc.) is deliberately absent.
  bool filteredCost(const QJsonValue& cost, const QString& nameNeedle,
                    QList<PetCultivationMaterial>& materials, bool allItems = false) const {
    if (!cost.isArray() || cost.toArray().size() > 128) return false;
    bool valid = true;
    for (const auto& value : cost.toArray()) {
      const auto row = value.toObject(); int type = 0,id = 0,count = 0,extra = 0;
      if (!value.isObject() || !integer(row.value(QStringLiteral("type")),&type,1) ||
          !integer(row.value(QStringLiteral("id")),&id,1) || !integer(row.value(QStringLiteral("count")),&count) ||
          (row.contains(QStringLiteral("extra")) && !integer(row.value(QStringLiteral("extra")),&extra))) { valid = false; continue; }
      if (!count || type != 4) continue;
      const auto definition = metadata.value(QStringLiteral("items")).toObject().value(QString::number(id)).toObject();
      QString name = definition.value(QStringLiteral("name")).toString();
      if (name.isEmpty()) {
        // A missing material dictionary must not silently hide a possible soul/essence cost.
        valid = false; if (!allItems) continue;
        name = QStringLiteral("元魂道具 %1").arg(id);
      }
      if (!allItems && !name.contains(nameNeedle)) continue;
      PetCultivationMaterial material; material.type = type; material.id = id; material.extra = extra;
      material.name = name; material.count = count; material.known = true;
      appendMaterial(materials,std::move(material));
    }
    return valid;
  }
  void badges() {
    const auto raw = pet.value(QStringLiteral("badge"));
    if (!raw.isString() || raw.toString().size() > 16384) {
      unknown(QStringLiteral("badge_state"),QStringLiteral("badges"),QStringLiteral("元魂"),QStringLiteral("元魂状态待刷新")); return;
    }
    const auto text = raw.toString();
    if (text.isEmpty()) {
      if (positiveGap(QStringLiteral("bsv"))) unknown(QStringLiteral("badge_state"),QStringLiteral("badges"),QStringLiteral("元魂"),QStringLiteral("缺少元魂培养信息"));
      return;
    }
    const auto definitions = metadata.value(QStringLiteral("badges")).toObject();
    const auto badgeSlots = text.split(QLatin1Char('|'));
    if (badgeSlots.size() > 32) { unknown(QStringLiteral("badge_state"),QStringLiteral("badges"),QStringLiteral("元魂"),QStringLiteral("元魂状态待刷新")); return; }
    int slotIndex = 0;
    for (const auto& slot : badgeSlots) {
      const auto parts = slot.split(QLatin1Char('#')); const auto ordinary = parts.value(0).split(QLatin1Char(':'));
      int id = 0,level = 0; const QString suffix = QString::number(++slotIndex);
      if (parts.size() > 2 || ordinary.size() != 2 || !integer(ordinary.value(0),&id,1) || !integer(ordinary.value(1),&level,0,1000)) {
        unknown(QStringLiteral("badge_state_")+suffix,QStringLiteral("badges"),QStringLiteral("元魂"),QStringLiteral("元魂状态待刷新")); continue;
      }
      const auto definition = definitions.value(QString::number(id)).toObject(); int maximum = 0;
      const auto name = entryName(definitions,id,QStringLiteral("元魂"));
      if (!integer(definition.value(QStringLiteral("maxLevel")),&maximum,1,1000) || level > maximum) {
        unknown(QStringLiteral("badge_level_")+suffix,QStringLiteral("badges"),name,QStringLiteral("当前 %1 级，目标等级待确认").arg(level));
      } else if (level < maximum) {
        PetCultivationRequirement row{QStringLiteral("badge_level_")+suffix,QStringLiteral("badges"),name,
          QStringLiteral("等级 %1/%2，还差 %3 级").arg(level).arg(maximum).arg(maximum-level),{},true};
        bool costsKnown = true; const auto levels = definition.value(QStringLiteral("levels")).toObject();
        for (int next = level+1; next <= maximum; ++next)
          costsKnown = filteredCost(levels.value(QString::number(next)).toObject().value(QStringLiteral("cost")),QStringLiteral("元魂"),row.materials) && costsKnown;
        if (!costsKnown) unknownMaterial(row.materials,QStringLiteral("对应元魂"));
        result.items.append(std::move(row));
      }
      if (parts.size() != 2) continue;
      const auto exclusive = parts[1].split(QLatin1Char(':')); int exclusiveId = 0,awake = 0;
      if (exclusive.size() != 2 || !integer(exclusive.value(0),&exclusiveId,1) || !integer(exclusive.value(1),&awake,0,1)) {
        unknown(QStringLiteral("badge_awaken_")+suffix,QStringLiteral("badges"),QStringLiteral("专属元魂"),QStringLiteral("觉醒信息待确认")); continue;
      }
      if (awake) continue;
      const auto exclusiveDefinition = definitions.value(QString::number(exclusiveId)).toObject();
      PetCultivationRequirement row{QStringLiteral("badge_awaken_")+suffix,QStringLiteral("badges"),
          entryName(definitions,exclusiveId,QStringLiteral("专属元魂")),QStringLiteral("未觉醒"),{},true};
      if (!filteredCost(exclusiveDefinition.value(QStringLiteral("activationCost")),QStringLiteral("元魂"),row.materials,true))
        unknownMaterial(row.materials,row.name+QStringLiteral("元魂"));
      result.items.append(std::move(row));
    }
  }
  void sacred() {
    const auto raw = pet.value(QStringLiteral("shenjue"));
    if (!raw.isString() || raw.toString().size() > 1024) {
      unknown(QStringLiteral("sacred_state"),QStringLiteral("sacred"),QStringLiteral("源兽"),QStringLiteral("源兽状态待刷新")); return;
    }
    if (raw.toString().isEmpty()) {
      if (positiveGap(QStringLiteral("sjv"))) unknown(QStringLiteral("sacred_state"),QStringLiteral("sacred"),QStringLiteral("源兽"),QStringLiteral("未装备，对应源兽待确认"));
      return;
    }
    const auto parts = raw.toString().split(QLatin1Char('|')); const auto definition = parts.value(0).split(QLatin1Char('#'));
    const auto levels = parts.value(1).split(QLatin1Char(':'));
    int id = 0,starPlan = 0,stagePlan = 0,star = 0,stage = 0;
    if (parts.size() != 2 || definition.size() != 3 || levels.size() != 2 || !integer(definition.value(0),&id,1) ||
        !integer(definition.value(1),&starPlan,1) || !integer(definition.value(2),&stagePlan,1) ||
        !integer(levels.value(0),&star,0,1000) || !integer(levels.value(1),&stage,0,1000)) {
      unknown(QStringLiteral("sacred_state"),QStringLiteral("sacred"),QStringLiteral("源兽"),QStringLiteral("星级、阶级信息待刷新")); return;
    }
    const auto beasts = metadata.value(QStringLiteral("sacredEquipment")).toObject();
    const auto starDefinition = metadata.value(QStringLiteral("sacredStarPlans")).toObject().value(QString::number(starPlan)).toObject();
    const auto stageDefinition = metadata.value(QStringLiteral("sacredStagePlans")).toObject().value(QString::number(stagePlan)).toObject();
    int maxStar = 0,maxStage = 0; const auto name = entryName(beasts,id,QStringLiteral("源兽"));
    const bool starKnown = integer(starDefinition.value(QStringLiteral("maxLevel")),&maxStar,1,1000) && star <= maxStar;
    const bool stageKnown = integer(stageDefinition.value(QStringLiteral("maxLevel")),&maxStage,1,1000) && stage <= maxStage;
    if (starKnown && stageKnown && star == maxStar && stage == maxStage) return;
    PetCultivationRequirement row{QStringLiteral("sacred"),QStringLiteral("sacred"),name,{}, {},starKnown && stageKnown};
    QStringList states;
    if (starKnown && star < maxStar) states.append(QStringLiteral("星级 %1/%2，还差 %3 星").arg(star).arg(maxStar).arg(maxStar-star));
    else if (!starKnown) states.append(QStringLiteral("当前 %1 星，目标待确认").arg(star));
    if (stageKnown && stage < maxStage) states.append(QStringLiteral("阶级 %1/%2，还差 %3 阶").arg(stage).arg(maxStage).arg(maxStage-stage));
    else if (!stageKnown) states.append(QStringLiteral("当前 %1 阶，目标待确认").arg(stage));
    row.status = states.join(QStringLiteral("；"));
    if (!stageKnown || stage < maxStage) {
      int source = 0,count = 0; bool costsKnown = stageKnown && integer(beasts.value(QString::number(id)).toObject().value(QStringLiteral("sourceId")),&source,1);
      const auto table = stageDefinition.value(QStringLiteral("levels")).toObject();
      if (stageKnown) for (int current = stage; current < maxStage; ++current) {
        int amount = 0;
        if (!integer(table.value(QString::number(current)).toObject().value(QStringLiteral("equipmentCount")),&amount) ||
            qint64(count)+amount > std::numeric_limits<int>::max()) costsKnown = false;
        else count += amount;
      }
      if (!costsKnown || count > 0) {
        PetCultivationMaterial material; material.type = 24; material.id = source; material.extra = 1;
        material.name = beasts.value(QString::number(id)).toObject().value(QStringLiteral("sourceName")).toString();
        if (material.name.isEmpty()) material.name = source > 0 ? QStringLiteral("源兽 %1").arg(source) : QStringLiteral("对应源兽");
        material.count = count; material.known = costsKnown; row.materials.append(material);
      }
    }
    statesKnown = statesKnown && row.known; result.items.append(std::move(row));
  }
  QJsonValue lightCost(const QJsonObject& node) const {
    if (node.contains(QStringLiteral("lightUpMaterials"))) return node.value(QStringLiteral("lightUpMaterials"));
    const auto encoded = node.value(QStringLiteral("lightUpCost"));
    if (!encoded.isString() || encoded.toString().size() > 8192) return {};
    QJsonArray costs; if (encoded.toString().isEmpty()) return costs;
    for (const auto& token : encoded.toString().split(QLatin1Char('#'))) {
      const auto fields = token.split(QLatin1Char(':')); int type = 0,id = 0,count = 0,extra = 0;
      if ((fields.size() != 3 && fields.size() != 4) || !integer(fields[0],&type,1) || !integer(fields[1],&id,1) ||
          !integer(fields.last(),&count) || (fields.size() == 4 && !integer(fields[2],&extra))) return {};
      costs.append(QJsonObject{{QStringLiteral("type"),type},{QStringLiteral("id"),id},{QStringLiteral("count"),count},{QStringLiteral("extra"),extra}});
    }
    return costs;
  }
  void astrolabe() {
    const auto raw = pet.value(QStringLiteral("astrolabe"));
    if (!raw.isString() || raw.toString().size() > 16384) {
      unknown(QStringLiteral("astrolabe_state"),QStringLiteral("astrolabe"),QStringLiteral("星轮"),QStringLiteral("星轮状态待刷新")); return;
    }
    if (raw.toString().isEmpty()) return;
    const auto nodes = metadata.value(QStringLiteral("astrolabe")).toObject();
    PetCultivationRequirement lighting{QStringLiteral("astrolabe_light"),QStringLiteral("astrolabe"),QStringLiteral("星轮点亮"),{}, {},true};
    int unlit = 0,selected = 0,senior = 0,nodeCount = 0; bool costsKnown = true;
    for (const auto& chain : raw.toString().split(QLatin1Char('|'))) for (const auto& token : chain.split(QLatin1Char('#'))) {
      const auto fields = token.split(QLatin1Char(':')); int id = 0,lit = 0,equipped = 0;
      if (++nodeCount > 128 || fields.size() < 2 || fields.size() > 3 || !integer(fields[0],&id) ||
          !integer(fields[1],&lit,0,1) || (fields.size() == 3 && !integer(fields[2],&equipped,0,1)) || (equipped && !lit)) {
        lighting.known = false; continue;
      }
      const auto node = nodes.value(QString::number(id)).toObject();
      if (node.value(QStringLiteral("isTBD")).isBool() && node.value(QStringLiteral("isTBD")).toBool()) continue;
      if (node.value(QStringLiteral("locatedType")).toInt(-1) == 2) ++senior;
      if (lit && equipped) ++selected;
      if (lit) continue;
      ++unlit;
      costsKnown = filteredCost(lightCost(node),QStringLiteral("精华"),lighting.materials) && costsKnown;
    }
    if (unlit || !lighting.known) {
      lighting.status = lighting.known ? QStringLiteral("还差 %1 个星轮节点未点亮").arg(unlit) : QStringLiteral("点亮状态待刷新");
      if (!costsKnown || !lighting.known) unknownMaterial(lighting.materials,QStringLiteral("星轮精华"));
      statesKnown = statesKnown && lighting.known; result.items.append(std::move(lighting));
    }
    if (!eraHasAstrolabeBreakthrough(resolvePetEra(pet, metadata.value(QStringLiteral("pets")).toObject()))) return;
    const auto breakthrough = pet.value(QStringLiteral("astrolabebr"));
    if (!breakthrough.isBool()) unknown(QStringLiteral("astrolabe_breakthrough"),QStringLiteral("astrolabe"),QStringLiteral("星轮突破"),QStringLiteral("突破状态待刷新"));
    else if (!breakthrough.toBool()) result.items.append({QStringLiteral("astrolabe_breakthrough"),QStringLiteral("astrolabe"),QStringLiteral("星轮突破"),QStringLiteral("未突破"),{},true});
    else if (senior >= 3 && selected < 3) result.items.append({QStringLiteral("astrolabe_select"),QStringLiteral("astrolabe"),QStringLiteral("星轮装备"),QStringLiteral("已突破，还需装备 %1 个星灵").arg(3-selected),{},true});
  }
  void stargods() {
    if (!power.stargodSlotsKnown || !power.stargodQualitiesKnown || !power.stargodBackpackKnown) {
      unknown(QStringLiteral("stargod_state"),QStringLiteral("stargods"),QStringLiteral("星神"),QStringLiteral("星神持有信息待刷新")); return;
    }
    if (power.missingRedStars > 0) result.items.append({QStringLiteral("stargod_red"),QStringLiteral("stargods"),QStringLiteral("普通红星"),
        QStringLiteral("还缺 %1 个不同种类的可用红星").arg(power.missingRedStars),{},true});
    if (power.hasChangeableSlot) {
      if (!power.changeableOwnedKnown) unknown(QStringLiteral("stargod_changeable"),QStringLiteral("stargods"),QStringLiteral("万变红星"),QStringLiteral("持有状态待刷新"));
      else if (!power.changeableOwnedRed) result.items.append({QStringLiteral("stargod_changeable"),QStringLiteral("stargods"),QStringLiteral("万变红星"),QStringLiteral("还缺 1 个万变红星"),{},true});
    }
    if (power.stargodLevelMissingSlots > 0 && power.stargodMaxLevel > 0) result.items.append({QStringLiteral("stargod_level"),QStringLiteral("stargods"),QStringLiteral("星神等级"),
        QStringLiteral("%1 个槽位未满级，需升至 %2 级").arg(power.stargodLevelMissingSlots).arg(power.stargodMaxLevel),{},true});
    else if (!power.stargodLevelsFull && (power.stargodSlots > 0 || power.hasChangeableSlot))
      unknown(QStringLiteral("stargod_level"),QStringLiteral("stargods"),QStringLiteral("星神等级"),QStringLiteral("目标等级待确认"));
    if (power.stargodPowerKnown && power.adjustmentGain > 0) result.items.append({QStringLiteral("stargod_equip"),QStringLiteral("stargods"),QStringLiteral("已有星神"),QStringLiteral("已有更合适的星神，待装备或调整"),{},true});
  }
  void remainingComponents() {
    for (const auto& row : power.components) {
      if (!row.applicable || row.key == QStringLiteral("bsv") || row.key == QStringLiteral("sjv") ||
          row.key == QStringLiteral("asv") || row.key == QStringLiteral("sgv")) continue;
      if (!row.gapKnown) { unknown(QStringLiteral("component_")+row.key,QStringLiteral("other"),row.label,QStringLiteral("培养状态待刷新")); continue; }
      if (row.gap <= 0) continue;
      QString status = QStringLiteral("尚未培养满");
      if (row.key == QStringLiteral("lv")) { int level = 0; if (integer(pet.value(QStringLiteral("lv")),&level)) status = QStringLiteral("当前 %1 级，尚未满级").arg(level); }
      else if (row.key == QStringLiteral("iv")) status = QStringLiteral("天赋尚未培养满");
      result.items.append({QStringLiteral("component_")+row.key,QStringLiteral("other"),row.label,status,{},true});
    }
  }
  PetCultivationRequirements run() {
    badges(); sacred(); astrolabe(); stargods(); remainingComponents();
    result.completeKnown = statesKnown && power.completionKnown;
    // Adjustment of already-owned stars is useful guidance but does not mean
    // the pet lacks cultivation resources or falls short of owned supreme power.
    bool cultivationPending = false;
    for (const auto& row : result.items) if (row.key != QStringLiteral("stargod_equip")) cultivationPending = true;
    result.complete = result.completeKnown && power.isHighest && !cultivationPending;
    return std::move(result);
  }
};
}

PetCultivationRequirements calculatePetCultivationRequirements(const QJsonObject& pet,
    const QJsonObject& metadataRoot, const PetBattlePowerState& power) {
  return Builder{pet,metadataRoot,power}.run();
}
