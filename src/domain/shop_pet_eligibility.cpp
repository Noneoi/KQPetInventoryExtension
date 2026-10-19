#include "shop_pet_eligibility.h"

#include "compiled_shop_catalog.h"

#include "pet_identity.h"
#include "pet_metadata_view.h"
#include "pet_era.h"
#include "checked_json_numbers.h"
#include "pet_power_calculator.h"

#include <algorithm>
#include <limits>

namespace {

using ComponentState = ShopCultivationState;

bool nonnegativeInteger(const QString& text, int* value) {
  if (text.isEmpty()) return false;
  int parsed = 0;
  for (const QChar character : text) {
    if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
    const int digit = character.unicode() - '0';
    if (parsed > (std::numeric_limits<int>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

bool numericParameters(const QString& text, QList<int>* values) {
  const auto tokens = text.split(QLatin1Char(':'));
  if (tokens.isEmpty() || tokens.size() > 32) return false;
  for (const auto& token : tokens) {
    int value = 0;
    if (!nonnegativeInteger(token,&value)) return false;
    values->append(value);
  }
  return true;
}

ComponentState powerGap(const PetBattlePowerState& power, const QString& key) {
  for (const auto& component : power.components) {
    if (component.key != key) continue;
    if (!component.applicable) return ComponentState::Full;
    if (!component.currentKnown || !component.extremeKnown) return ComponentState::Unknown;
    return component.current < component.extreme ? ComponentState::Useful : ComponentState::Full;
  }
  return ComponentState::Unknown;
}

ComponentState oneRedStargod(const PetBattlePowerState& power) {
  if (!power.stargodAcquisitionKnown) return ComponentState::Unknown;
  return power.missingRedStars > 0 ? ComponentState::Useful : ComponentState::Full;
}

ComponentState oneChangeableRedStargod(const PetBattlePowerState& power) {
  if (!power.stargodSlotsKnown) return ComponentState::Unknown;
  if (!power.hasChangeableSlot) return ComponentState::Full;
  if (!power.changeableOwnedKnown) return ComponentState::Unknown;
  // Ownership is intentionally broader than the game's equipped-only reach
  // checker. The offer's independent minimum slot level is applied per rule.
  return power.changeableOwnedQuality >= 6 ? ComponentState::Full : ComponentState::Useful;
}

ComponentState componentState(const ShopPetComponentRule& rule, const ShopPetDerived& pet, bool suppliesOrdinaryAstrolabe) {
  if (rule.componentIndex < 0 || rule.componentIndex >= int(pet.components.size())) return ComponentState::Unknown;
  if (rule.componentIndex == 18) {
    if (pet.usefulSpecifiedStars.contains(rule.targetId)) return ComponentState::Useful;
    if (pet.satisfiedSpecifiedStars.contains(rule.targetId) || pet.unavailableSpecifiedStars.contains(rule.targetId)) return ComponentState::Full;
    return ComponentState::Unknown;
  }
  const auto state = pet.components[rule.componentIndex];
  if (rule.componentIndex == 19 || rule.componentIndex == 21) {
    const bool equipment = rule.componentIndex == 19;
    const bool known = equipment ? pet.equipmentLevelsKnown : pet.legendStoneLevelsKnown;
    if (!known || rule.targetLevel <= 0) return state;
    const int minimum = equipment ? pet.minimumEquipmentLevel : pet.minimumLegendStoneLevel;
    return minimum < rule.targetLevel ? ComponentState::Useful : ComponentState::Full;
  }
  if (rule.componentIndex == 13 && state == ComponentState::Useful) {
    if (suppliesOrdinaryAstrolabe || pet.components[12] == ComponentState::Full) return ComponentState::Useful;
    return pet.components[12] == ComponentState::Unknown ? ComponentState::Unknown : ComponentState::Full;
  }
  if (rule.componentIndex != 9) return state;
  if (rule.targetLevel <= 0 || !pet.changeableSlotKnown) return ComponentState::Unknown;
  if (!pet.hasChangeableSlot) return ComponentState::Full;
  if (!pet.changeableLevelKnown) return ComponentState::Unknown;
  if (pet.changeableLevel < rule.targetLevel) return ComponentState::Useful;
  return state;
}

ComponentState goldStargods(const PetBattlePowerState& power) {
  if (!power.stargodSlotsKnown || !power.stargodBackpackKnown ||
      !power.stargodQualitiesKnown) return ComponentState::Unknown;
  const bool ordinaryMissing = power.goldStars + power.redStars < power.stargodSlots;
  if (ordinaryMissing) return ComponentState::Useful;
  if (power.hasChangeableSlot && !power.changeableOwnedKnown) return ComponentState::Unknown;
  if (power.hasChangeableSlot && power.changeableOwnedQuality < 5) return ComponentState::Useful;
  if (power.stargodLevelsFull) return ComponentState::Full;
  return power.stargodMaxLevel > 0 ? ComponentState::Useful : ComponentState::Unknown;
}

ComponentState allRedStargods(const PetBattlePowerState& power) {
  if (!power.stargodSlotsKnown || !power.stargodBackpackKnown || !power.stargodQualitiesKnown) return ComponentState::Unknown;
  if (power.missingRedStars > 0) return ComponentState::Useful;
  if (power.hasChangeableSlot && !power.changeableOwnedKnown) return ComponentState::Unknown;
  if (power.hasChangeableSlot && !power.changeableOwnedRed) return ComponentState::Useful;
  if (power.stargodLevelsFull) return ComponentState::Full;
  return power.stargodMaxLevel > 0 ? ComponentState::Useful : ComponentState::Unknown;
}

ComponentState oneGoldStargod(const PetBattlePowerState& power, const QJsonObject& stars) {
  if (!power.stargodSlotsKnown) return ComponentState::Unknown;
  if (power.stargodSlots == 0) return ComponentState::Full;
  if (!power.stargodBackpackKnown || !power.stargodQualitiesKnown) return ComponentState::Unknown;
  if (power.goldStars + power.redStars < power.stargodSlots) return ComponentState::Useful;
  // OneGold supplies a level-six ordinary gold star. Existing red stars are
  // already better; a gold offer must not recommend downgrading them.
  bool unknown = false;
  for (const auto& slot : power.stargodDetails) if (!slot.changeable) {
    qint64 quality = 0;
    if (!DomainNumeric::checkedInteger(stars.value(QString::number(slot.ownedBestId)).toObject().value(QStringLiteral("quality")),&quality,0,6)) {
      unknown = true; continue;
    }
    if (quality >= 6) continue;
    if (slot.level <= 0) unknown = true;
    else if (slot.level < 6) return ComponentState::Useful;
  }
  return unknown ? ComponentState::Unknown : ComponentState::Full;
}

bool minimumEquipmentLevel(const QJsonValue& raw, int* minimum) {
  if (!raw.isString() || raw.toString().size() > 16384) return false;
  const auto slotTexts = raw.toString().split(QLatin1Char(','));
  if (slotTexts.size() != 3) return false;
  int result = std::numeric_limits<int>::max();
  for (const auto& slot : slotTexts) {
    if (slot.isEmpty() || slot == QStringLiteral("0") || slot == QStringLiteral("-1")) { result = 0; continue; }
    const auto parts = slot.split(QLatin1Char('|'));
    int id = 0, experience = 0, level = 0;
    if (parts.size() < 3 || parts.size() > 5 || !nonnegativeInteger(parts[0],&id) || id <= 0 ||
        !nonnegativeInteger(parts[1],&experience) || !nonnegativeInteger(parts[2],&level) || level <= 0) return false;
    result = std::min(result,level);
  }
  *minimum = result; return true;
}

bool minimumLegendStoneLevel(const QJsonValue& raw, int* minimum) {
  if (!raw.isString() || raw.toString().size() > 16384) return false;
  const auto slotTexts = raw.toString().split(QLatin1Char(','));
  if (slotTexts.size() != 5) return false;
  int result = std::numeric_limits<int>::max();
  for (const auto& slot : slotTexts) {
    const auto parts = slot.split(QLatin1Char(':'));
    int id = 0, state = 0, level = 5, experience = 0;
    if (parts.size() < 2 || parts.size() > 4 ||
        (parts[0] != QStringLiteral("-1") && !nonnegativeInteger(parts[0],&id)) ||
        !nonnegativeInteger(parts[1],&state) || state > 2 ||
        (parts.size() >= 3 && !nonnegativeInteger(parts[2],&level)) ||
        (parts.size() == 4 && !nonnegativeInteger(parts[3],&experience))) return false;
    if (parts[0] == QStringLiteral("-1") || id == 0 || state == 0) level = 0;
    result = std::min(result,level);
  }
  *minimum = result; return true;
}

ComponentState cultivationState(bool missing, bool unknown) {
  return missing ? ComponentState::Useful : unknown ? ComponentState::Unknown : ComponentState::Full;
}
struct BadgeStates { ComponentState level = ComponentState::Unknown, awaken = ComponentState::Unknown; };
BadgeStates badgeStates(const QJsonObject& pet, const QJsonObject& metadata) {
  const auto raw = pet.value(QStringLiteral("badge"));
  if (!raw.isString() || raw.toString().size() > 16384) return {};
  if (raw.toString().isEmpty()) return {ComponentState::Full,ComponentState::Full};
  bool levelMissing = false, levelUnknown = false, awakenMissing = false, awakenUnknown = false;
  for (const auto& slot : raw.toString().split(QLatin1Char('|'))) {
    const auto halves = slot.split(QLatin1Char('#')), job = halves.value(0).split(QLatin1Char(':'));
    int id = 0,level = 0;
    if (halves.size() > 2 || job.size() != 2 || !nonnegativeInteger(job.value(0),&id) || id <= 0 || !nonnegativeInteger(job.value(1),&level)) {
      levelUnknown = awakenUnknown = true; continue;
    }
    qint64 maximum = 0;
    if (!DomainNumeric::checkedInteger(metadata.value(QString::number(id)).toObject().value(QStringLiteral("maxLevel")),&maximum,1,std::numeric_limits<int>::max()) || level > maximum)
      levelUnknown = true;
    else levelMissing |= level < maximum;
    if (halves.size() == 1) continue; // Official parseServerInfo defaults absent exclusive to ID_NULL.
    const auto exclusive = halves[1].split(QLatin1Char(':')); int exclusiveId = 0,activated = 0;
    if (exclusive.size() != 2 || !nonnegativeInteger(exclusive.value(0),&exclusiveId) ||
        !nonnegativeInteger(exclusive.value(1),&activated) || activated > 1) awakenUnknown = true;
    else awakenMissing |= exclusiveId > 0 && activated == 0;
  }
  return {cultivationState(levelMissing,levelUnknown),cultivationState(awakenMissing,awakenUnknown)};
}

struct AstrolabeStates {
  ComponentState all = ComponentState::Unknown, ordinary = ComponentState::Unknown;
  bool rawKnown = false, hasNodes = false;
};
AstrolabeStates astrolabeStates(const QJsonObject& pet, const QJsonObject& metadata) {
  const auto raw = pet.value(QStringLiteral("astrolabe"));
  if (!raw.isString() || raw.toString().size() > 16384) return {};
  AstrolabeStates result; result.rawKnown = true; result.hasNodes = !raw.toString().isEmpty();
  bool allMissing = false, ordinaryMissing = false, allUnknown = false, ordinaryUnknown = false;
  if (result.hasNodes) for (const auto& chain : raw.toString().split(QLatin1Char('|'))) for (const auto& slot : chain.split(QLatin1Char('#'))) {
    const auto parts = slot.split(QLatin1Char(':')); int id = 0,lit = 0,selected = 0;
    if (parts.size() < 2 || parts.size() > 3 || !nonnegativeInteger(parts.value(0),&id) ||
        !nonnegativeInteger(parts.value(1),&lit) || lit > 1 ||
        (parts.size() == 3 && (!nonnegativeInteger(parts[2],&selected) || selected > 1)) || (selected && !lit)) {
      allUnknown = ordinaryUnknown = true; result.rawKnown = false; continue;
    }
    const auto definition = metadata.value(QString::number(id)).toObject();
    if (!definition.value(QStringLiteral("isTBD")).isBool()) { allUnknown = ordinaryUnknown = true; continue; }
    if (definition.value(QStringLiteral("isTBD")).toBool()) continue;
    if (!lit) allMissing = true;
    if (!definition.value(QStringLiteral("exclusive")).isBool()) ordinaryUnknown = true;
    else if (!definition.value(QStringLiteral("exclusive")).toBool() && !lit) ordinaryMissing = true;
  }
  result.all = cultivationState(allMissing,allUnknown);
  result.ordinary = cultivationState(ordinaryMissing,ordinaryUnknown);
  return result;
}

void deriveSingleAstrolabe(const QJsonObject& pet, const QJsonObject& metadata, ShopPetDerived* out) {
  const auto raw=pet.value(QStringLiteral("astrolabe"));
  if (!raw.isString() || raw.toString().size() > 16384) return;
  std::array<bool,3> useful{}, unknown{}, blocked{};
  if (!raw.toString().isEmpty()) for (const auto& chain : raw.toString().split(QLatin1Char('|'))) {
    bool reachable=true, reachKnown=true;
    for (const auto& slot : chain.split(QLatin1Char('#'))) {
      const auto parts=slot.split(QLatin1Char(':')); int id=0,lit=0,selected=0;
      if (parts.size()<2 || parts.size()>3 || !nonnegativeInteger(parts.value(0),&id) ||
          !nonnegativeInteger(parts.value(1),&lit) || lit>1 ||
          (parts.size()==3 && (!nonnegativeInteger(parts[2],&selected) || selected>1)) || (selected && !lit)) {
        unknown.fill(true); reachKnown=false; continue;
      }
      if (!lit) {
        const auto definition=metadata.value(QString::number(id)).toObject();
        qint64 layer=0;
        if (!definition.value("isTBD").isBool()) unknown.fill(true);
        else if (!definition.value("isTBD").toBool()) {
          if (!definition.value("exclusive").isBool() ||
              !DomainNumeric::checkedInteger(definition.value("locatedType"),&layer,0,2)) unknown.fill(true);
          else {
            const bool exclusive=definition.value("exclusive").toBool();
            const int category=exclusive ? (layer==2 ? 2 : -1) : layer==1 ? 0 : layer==2 ? 1 : -1;
            if (category>=0) {
              if (!reachKnown) unknown[category]=true;
              else if (reachable) useful[category]=true;
              else blocked[category]=true;
            }
          }
        }
      }
      reachable &= lit!=0;
    }
  }
  for (int i=0;i<3;++i) {
    out->components[14+i]=cultivationState(useful[i],unknown[i]);
    if (blocked[i] && !useful[i] && !unknown[i]) out->astrolabeBlockedMask |= 1<<i;
  }
}

ComponentState astrolabeBreakthrough(const QJsonObject& pet, const ShopPetMetadataSnapshot& metadata, const AstrolabeStates& astrolabe) {
  const auto era = resolvePetEra(pet,metadata.pets);
  if (era != PetEra::Unknown && !eraHasAstrolabeBreakthrough(era)) return ComponentState::Full;
  if (era == PetEra::Unknown) return ComponentState::Unknown;
  const auto breakthrough = pet.value(QStringLiteral("astrolabebr"));
  if (breakthrough.isBool() && breakthrough.toBool()) return ComponentState::Full;
  if (astrolabe.rawKnown && !astrolabe.hasNodes) return ComponentState::Full;
  const auto definition = metadata.pets.value(QString::number(petRaceId(pet))).toObject();
  const auto costs = definition.value(QStringLiteral("astrolabeBreakCosts"));
  if (!costs.isString()) return ComponentState::Unknown;
  if (costs.toString().isEmpty()) return ComponentState::Full;
  if (!breakthrough.isBool() || !astrolabe.rawKnown) return ComponentState::Unknown;
  return ComponentState::Useful;
}

struct SacredEquipmentLevels {
  int starPlan = 0;
  int stagePlan = 0;
  int star = 0;
  int stage = 0;
  bool valid = false;
};

SacredEquipmentLevels sacredEquipmentLevels(const QJsonObject& pet) {
  const QString sequence = pet.value(QStringLiteral("shenjue")).toString();
  const QStringList parts = sequence.split(QLatin1Char('|'));
  const QStringList define = parts.value(0).split(QLatin1Char('#'));
  const QStringList current = parts.value(1).split(QLatin1Char(':'));
  SacredEquipmentLevels levels;
  int id = 0;
  levels.valid = parts.size() == 2 && define.size() == 3 && current.size() == 2 &&
      nonnegativeInteger(define.value(0), &id) && id > 0 &&
      nonnegativeInteger(define.value(1), &levels.starPlan) && levels.starPlan > 0 &&
      nonnegativeInteger(define.value(2), &levels.stagePlan) && levels.stagePlan > 0 &&
      nonnegativeInteger(current.value(0), &levels.star) &&
      nonnegativeInteger(current.value(1), &levels.stage);
  return levels;
}

QString componentName(const QString& code) {
  int specifiedStar = 0;
  if (code.startsWith(QStringLiteral("35$")) && nonnegativeInteger(code.mid(3), &specifiedStar) && specifiedStar > 0)
    return QStringLiteral("指定星神（适用条件以活动内为准）");
  static const QHash<QString, QString> names = {
      {QStringLiteral("11"), QStringLiteral("等级")},
      {QStringLiteral("31"), QStringLiteral("满金星+万变金星")},
      {QStringLiteral("32"), QStringLiteral("满红星+万变红星（槽位满级）")},
      {QStringLiteral("34"), QStringLiteral("红色星神")},
      {QStringLiteral("39$1"), QStringLiteral("1颗红色星神（含限定）")},
      {QStringLiteral("39$3"), QStringLiteral("任选3颗红色星神（含限定）")},
      {QStringLiteral("39"), QStringLiteral("1颗红色星神（含限定）")},
      {QStringLiteral("41"), QStringLiteral("元魂等级")},
      {QStringLiteral("42"), QStringLiteral("全部专属元魂觉醒")},
      {QStringLiteral("43"), QStringLiteral("1个元魂升满级")},
      {QStringLiteral("44"), QStringLiteral("专属元魂觉醒")},
      {QStringLiteral("62"), QStringLiteral("天赋")},
      {QStringLiteral("84"), QStringLiteral("天迹星轮")},
      {QStringLiteral("85"), QStringLiteral("全部普通星迹")},
      {QStringLiteral("86"), QStringLiteral("全部星迹（含专属）")},
      {QStringLiteral("89"), QStringLiteral("星轮突破")},
      {QStringLiteral("89$1"), QStringLiteral("星迹突破")},
      {QStringLiteral("91"), QStringLiteral("源兽星级")},
      {QStringLiteral("92"), QStringLiteral("源兽/神源兽阶级")},
      {QStringLiteral("94"), QStringLiteral("源兽升至满阶")},
      {QStringLiteral("95"), QStringLiteral("源兽升1阶（无需源兽材料阶段）")}};
  return names.value(code, QStringLiteral("未知培养类型 %1").arg(code));
}

}  // namespace



CompiledShopPetRule compileShopPetRule(const QString& enhanceType,
                                      AlgorithmPipelineStats* stats) {
  static const QStringList knownCodes{QStringLiteral("11"), QStringLiteral("31"),
      QStringLiteral("34"), QStringLiteral("41"), QStringLiteral("44"),
      QStringLiteral("62"), QStringLiteral("84"), QStringLiteral("91"), QStringLiteral("92")};
  CompiledShopPetRule result;
  QStringList codes = enhanceType.split(QLatin1Char('-'));
  for (QString& code : codes) code = code.trimmed();
  codes.removeDuplicates();
  if (stats) ++stats->enhancementExpressionsParsed;
  for (const QString& code : codes) {
    int index = static_cast<int>(knownCodes.indexOf(code));
    int targetLevel = 0, targetId = 0;
    QString name = componentName(code);
    // Type 33 uses parameter zero as its minimum slot level (default one).
    // Type 39's currently documented flows are one red star and the official
    // 2026-09-18 three-choice bundle. Unseen parameter variants stay unknown.
    if (code == QStringLiteral("39") || code == QStringLiteral("39$1") ||
        code == QStringLiteral("39$3")) index = 2;
    else if (code == QStringLiteral("32")) index = 11;
    else if (code == QStringLiteral("42")) index = 4;
    else if (code == QStringLiteral("43")) index = 3;
    else if (code == QStringLiteral("85")) { index = 12; result.suppliesOrdinaryAstrolabe = true; }
    else if (code == QStringLiteral("86")) { index = 6; result.suppliesOrdinaryAstrolabe = true; }
    else if (code == QStringLiteral("89") || code == QStringLiteral("89$1")) index = 13;
    else if (code == QStringLiteral("94")) index = 8;
    else if (code == QStringLiteral("95")) index = 10;
    else if (code == QStringLiteral("61")) { index=17; name=QStringLiteral("超凡入圣天赋"); }
    else if (code == QStringLiteral("81")) { index=14; name=QStringLiteral("内圈星迹"); }
    else if (code == QStringLiteral("82")) { index=15; name=QStringLiteral("外圈星迹"); }
    else if (code == QStringLiteral("83")) { index=16; name=QStringLiteral("专属星迹"); }
    else if (code == QStringLiteral("25") || code.startsWith(QStringLiteral("25$")) ||
             code == QStringLiteral("53") || code.startsWith(QStringLiteral("53$"))) {
      const bool equipment = code.startsWith(QStringLiteral("25"));
      QList<int> parameters;
      if (code.size() == 2) parameters.append(equipment ? 6 : 5);
      else if (!numericParameters(code.mid(3),&parameters)) parameters.clear();
      if (!parameters.isEmpty() && parameters[0] > 0) {
        targetLevel = parameters[0]; index = equipment ? 19 : 21;
        name = equipment ? QStringLiteral("一个%1星源兽").arg(targetLevel)
                         : QStringLiteral("一个%1级传说石").arg(targetLevel);
      }
    }
    else if (code == QStringLiteral("37") || code.startsWith(QStringLiteral("37$"))) {
      QList<int> parameters;
      if (code == QStringLiteral("37") || (numericParameters(code.mid(3),&parameters) && parameters[0] > 0)) {
        index = 20; name = QStringLiteral("一个6级金色星神");
      }
    }
    else if (code.startsWith(QStringLiteral("34$")) || code.startsWith(QStringLiteral("92$"))) {
      int parameter=0;
      // The official single-red flow uses the same OneRed strategy; the
      // stage-more flow is useful until the dynamic plan maximum is reached.
      if (nonnegativeInteger(code.mid(3),&parameter) && parameter>0) {
        const bool red=code.startsWith(QStringLiteral("34$")); index=red ? 2 : 8;
        name=componentName(red ? QStringLiteral("34") : QStringLiteral("92"));
      }
    }
    else if (code.startsWith(QStringLiteral("35$"))) {
      QList<int> parameters;
      // All three official consumers use only parameter zero. Keep the raw
      // expression; trailing numeric fields are not a quantity or level.
      if (numericParameters(code.mid(3),&parameters) && parameters[0]>0) {
        targetId = parameters[0];
        index=18; name=QStringLiteral("指定星神 #%1").arg(targetId);
      }
    }
    else if (code == QStringLiteral("33") || code.startsWith(QStringLiteral("33$"))) {
      const auto parameter = code == QStringLiteral("33") ? QStringLiteral("1") : code.mid(3);
      if (nonnegativeInteger(parameter,&targetLevel) && targetLevel > 0) {
        index = 9; name = QStringLiteral("万变红星（至少%1级）").arg(targetLevel);
      }
    }
    result.components.append({code, name, index, targetLevel, targetId});
    if (stats) ++stats->enhancementComponentsCompiled;
  }
  return result;
}

ShopPetDerived deriveShopPet(const QJsonObject& pet, bool hasFullDetail,
                             const ShopPetMetadataSnapshot& metadata,
                             AlgorithmPipelineStats* stats) {
  const auto power = hasFullDetail ? calculatePetBattlePower(pet,
      petPowerMetadataFromCatalog(pet, 0, metadata.stargods, metadata.astrolabe, metadata.pets))
      : PetBattlePowerState{};
  return deriveShopPetWithPower(pet, hasFullDetail, metadata, power, stats);
}

ShopPetDerived deriveShopPetWithPower(const QJsonObject& pet, bool hasFullDetail,
                                      const ShopPetMetadataSnapshot& metadata,
                                      const PetBattlePowerState& power,
                                      AlgorithmPipelineStats* stats) {
  ShopPetDerived result;
  if (stats) ++stats->petsDerived;
  result.raceId = petRaceId(pet);
  result.metadataRaceId = pet.value(QStringLiteral("_metaRaceId")).toInt();
  result.hasFullDetail = hasFullDetail;
  result.hasData = !pet.isEmpty();
  if (!hasFullDetail || !result.hasData) return result;
  const auto era = resolvePetEra(pet, metadata.pets);
  const auto systems = systemsForEra(era);
  const auto sacred = pet.value(QStringLiteral("shenjue"));
  result.sacredStateRequired = systems.sacred && !sacred.isUndefined() && (!sacred.isString() || !sacred.toString().isEmpty());
  const auto requiresState = [&pet,&power](const QString& field,const QString& component) {
    const auto raw = pet.value(field);
    if (!raw.isUndefined() && (!raw.isString() || !raw.toString().isEmpty())) return true;
    for (const auto& value : power.components) if (value.key == component && value.applicable &&
        ((value.currentKnown && value.current > 0) || (value.highestKnown && value.highest > 0))) return true;
    return false;
  };
  result.badgeStateRequired = systems.badge && requiresState(QStringLiteral("badge"),QStringLiteral("bsv"));
  result.astrolabeStateRequired = systems.astrolabe && requiresState(QStringLiteral("astrolabe"),QStringLiteral("asv"));
  result.components[0] = powerGap(power, QStringLiteral("lv"));
  result.components[1] = goldStargods(power);
  result.components[2] = oneRedStargod(power);
  if (systems.badge) {
    const auto badges = badgeStates(pet,metadata.badges);
    result.components[3] = badges.level;
    result.components[4] = badges.awaken;
  } else {
    result.components[3] = result.components[4] = ComponentState::Full;
  }
  result.components[5] = powerGap(power, QStringLiteral("iv"));
  if (systems.astrolabe) {
    const auto astrolabe = astrolabeStates(pet,metadata.astrolabe);
    result.components[6] = astrolabe.all;
    result.components[12] = astrolabe.ordinary;
    result.components[13] = astrolabeBreakthrough(pet,metadata,astrolabe);
    deriveSingleAstrolabe(pet,metadata.astrolabe,&result);
  } else {
    result.components[6] = result.components[12] = result.components[13] = ComponentState::Full;
    result.components[14] = result.components[15] = result.components[16] = ComponentState::Full;
  }
  qint64 giftLevel=0;
  if (DomainNumeric::checkedInteger(pet.value(QStringLiteral("gt")),&giftLevel,0,std::numeric_limits<int>::max()))
    result.components[17]=giftLevel>=6 ? ComponentState::Full : ComponentState::Useful;
  const auto specified=specifiedStargodAcquisition(pet,
      petPowerMetadataFromCatalog(pet,0,metadata.stargods,metadata.astrolabe,metadata.pets),power);
  result.usefulSpecifiedStars=specified.useful; result.satisfiedSpecifiedStars=specified.satisfied;
  result.unavailableSpecifiedStars=specified.unavailable;
  result.components[9] = oneChangeableRedStargod(power);
  result.components[11] = allRedStargods(power);
  result.components[20] = oneGoldStargod(power,metadata.stargods);
  if (!systems.equipment) result.components[19] = ComponentState::Full;
  else if (era != PetEra::Unknown && minimumEquipmentLevel(pet.value(QStringLiteral("eps")),&result.minimumEquipmentLevel)) {
    result.equipmentLevelsKnown = true;
    result.components[19] = result.minimumEquipmentLevel < 6 ? ComponentState::Useful : ComponentState::Full;
  }
  if (!systems.legendStone) result.components[21] = ComponentState::Full;
  else if (era != PetEra::Unknown && minimumLegendStoneLevel(pet.value(QStringLiteral("lss")),&result.minimumLegendStoneLevel)) {
    result.legendStoneLevelsKnown = true;
    result.components[21] = result.minimumLegendStoneLevel < 5 ? ComponentState::Useful : ComponentState::Full;
  }
  result.changeableSlotKnown = power.stargodSlotsKnown;
  result.hasChangeableSlot = power.hasChangeableSlot;
  bool foundChangeable = false;
  result.changeableLevelKnown = power.stargodSlotsKnown;
  for (const auto& slot : power.stargodDetails) if (slot.changeable) {
    if (slot.level <= 0) result.changeableLevelKnown = false;
    if (!foundChangeable || slot.level < result.changeableLevel) result.changeableLevel = slot.level;
    foundChangeable = true;
  }
  result.changeableLevelKnown = result.changeableLevelKnown && foundChangeable && result.changeableLevel > 0;
  if (!systems.sacred) {
    result.components[7] = result.components[8] = result.components[10] = ComponentState::Full;
  } else {
    // The same sacred equipment sequence supplies both component states.
    const SacredEquipmentLevels levels = sacredEquipmentLevels(pet);
    if (levels.valid) {
      const int maximumStar = PetMetadataView::sacredPlanMaximum(metadata.sacredStarPlans, levels.starPlan);
      const int maximumStage = PetMetadataView::sacredPlanMaximum(metadata.sacredStagePlans, levels.stagePlan);
      if (maximumStar > 0 && levels.star <= maximumStar)
        result.components[7] = levels.star < maximumStar
            ? ShopCultivationState::Useful : ShopCultivationState::Full;
      if (maximumStage > 0 && levels.stage <= maximumStage)
        result.components[8] = levels.stage < maximumStage
            ? ShopCultivationState::Useful : ShopCultivationState::Full;
      if (maximumStage > 0 && levels.stage == maximumStage) result.components[10] = ShopCultivationState::Full;
      else if (maximumStage > 0 && levels.stage < maximumStage) {
        const auto definition = metadata.sacredStagePlans.value(QString::number(levels.stagePlan)).toObject();
        const auto cost = definition.value(QStringLiteral("levels")).toObject().value(QString::number(levels.stage))
            .toObject().value(QStringLiteral("equipmentCount"));
        qint64 equipmentCount = 0;
        if (DomainNumeric::checkedInteger(cost,&equipmentCount,0,std::numeric_limits<int>::max()))
          result.components[10] = equipmentCount == 0 ? ShopCultivationState::Useful : ShopCultivationState::Full;
      }
    }
  }
  if (stats) stats->cultivationComponentsDerived += result.components.size();
  return result;
}

ShopPetEligibilitySummary evaluateShopPetRule(
    const CompiledShopPetRule& rule, const ShopPetDerived& pet,
    AlgorithmPipelineStats* stats) {
  ShopPetEligibilitySummary result;
  if (!pet.hasFullDetail || !pet.hasData || rule.components.isEmpty()) return result;
  bool unknown = false;
  for (const ShopPetComponentRule& component : rule.components) {
    if (stats) ++stats->candidateComponentsChecked;
    const ShopCultivationState state = componentState(component,pet,rule.suppliesOrdinaryAstrolabe);
    if (state == ShopCultivationState::Useful) {
      ++result.usefulCount;
      result.redStarUseful |= component.componentIndex == 2 || component.componentIndex == 18;
    } else if (state == ShopCultivationState::Unknown) unknown = true;
  }
  result.state = result.usefulCount > 0 ? ShopPetEligibilityState::Usable
      : unknown ? ShopPetEligibilityState::Unknown : ShopPetEligibilityState::NotUsable;
  return result;
}

ShopPetEligibility describeShopPetRule(const CompiledShopPetRule& rule,
                                       const ShopPetDerived& pet) {
  if (!pet.hasFullDetail)
    return {ShopPetEligibilityState::Unknown,
            QStringLiteral("缺少该实例的完整本地详情，无法安全判断")};
  if (!pet.hasData)
    return {ShopPetEligibilityState::Unknown, QStringLiteral("本地详情为空")};
  if (rule.components.isEmpty())
    return {ShopPetEligibilityState::Unknown,
            QStringLiteral("官方项目没有可识别的培养类型")};
  QStringList useful, usefulCodes, full, unknown;
  bool needsSourceBeastMaterial = false;
  bool needsOrdinaryAstrolabe = false;
  bool needsPreviousTrace = false;
  bool specifiedStarUnavailable = false;
  for (const ShopPetComponentRule& component : rule.components) {
    const ShopCultivationState state = componentState(component,pet,rule.suppliesOrdinaryAstrolabe);
    if (state == ShopCultivationState::Useful) {
      useful.append(component.name);
      usefulCodes.append(component.code);
    } else if (state == ShopCultivationState::Full) {
      full.append(component.name);
      needsSourceBeastMaterial |= component.componentIndex == 10 && pet.components[8] == ShopCultivationState::Useful;
      needsOrdinaryAstrolabe |= component.componentIndex == 13 && pet.components[13] == ShopCultivationState::Useful;
      needsPreviousTrace |= component.componentIndex>=14 && component.componentIndex<=16 &&
          (pet.astrolabeBlockedMask & (1<<(component.componentIndex-14)));
      specifiedStarUnavailable |= component.componentIndex==18 && pet.unavailableSpecifiedStars.contains(component.targetId);
    }
    else unknown.append(component.name);
  }
  if (!useful.isEmpty())
    return {ShopPetEligibilityState::Usable,
            QStringLiteral("可提升：%1").arg(useful.join(QStringLiteral("、"))), usefulCodes};
  if (unknown.isEmpty()) {
    if (specifiedStarUnavailable) return {ShopPetEligibilityState::NotUsable,QStringLiteral("指定星神不适用此精灵的职业或普通星神槽位")};
    if (needsPreviousTrace) return {ShopPetEligibilityState::NotUsable,QStringLiteral("需先激活前置星迹，当前不能使用此单点激活奖励")};
    if (needsSourceBeastMaterial && full.size() == 1)
      return {ShopPetEligibilityState::NotUsable,QStringLiteral("当前升阶需要源兽材料，不适用此兑换分支")};
    if (needsOrdinaryAstrolabe && full.size() == 1)
      return {ShopPetEligibilityState::NotUsable,QStringLiteral("需先激活全部普通星迹后再突破")};
    const bool includesStars = std::any_of(rule.components.cbegin(), rule.components.cend(),
        [](const ShopPetComponentRule& component) { return component.componentIndex == 1 || component.componentIndex == 2 || component.componentIndex == 9 || component.componentIndex == 11 || component.componentIndex == 18; });
    return {ShopPetEligibilityState::NotUsable,
            QStringLiteral("对应培养项已满足或资源已持有：%1").arg(full.join(QStringLiteral("、"))) +
                (includesStars ? QStringLiteral("；已有星神先装备或升级，无需重复兑换") : QString{})};
  }
  return {ShopPetEligibilityState::Unknown,
          QStringLiteral("待补详情或官方数据：%1").arg(unknown.join(QStringLiteral("、")))};
}
