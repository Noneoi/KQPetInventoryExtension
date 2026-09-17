#include "asset_derivation.h"
#include "shop_pet_eligibility.h"
#include "pet_power_calculator.h"
#include "checked_json_numbers.h"
#include <climits>

namespace {
bool hasGap(const PetBattlePowerState& state, const QString& key) {
  for (const PetBattlePowerGap& gap : state.componentGaps)
    if (gap.key == key && gap.gap > 0) return true;
  return false;
}

}

QJsonObject AssetDerivation::identityFields(const QJsonObject& pet) {
  static const QStringList fields{
      QStringLiteral("id"), QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("fr"),
      QStringLiteral("n"), QStringLiteral("customName"), QStringLiteral("lv"), QStringLiteral("g"),
      QStringLiteral("gd"), QStringLiteral("_location"), QStringLiteral("_warehouseGroup"),
      QStringLiteral("_position"), QStringLiteral("_visualMismatch"), QStringLiteral("_metaOriginalName"),
      QStringLiteral("_metaAttributes"), QStringLiteral("_metaJobs"), QStringLiteral("_metaEra"), QStringLiteral("_metaRaceId")};
  QJsonObject identity;
  for (const QString& field : fields) {
    const QJsonValue value = pet.value(field);
    if (value.isString() || value.isDouble() || value.isBool()) identity.insert(field, value);
  }
  return identity;
}

QJsonObject AssetDerivation::analysisInputFields(const QJsonObject& brief, const QJsonObject& detail) {
  static const QStringList detailFields{
      QStringLiteral("zdl"), QStringLiteral("xzdl"), QStringLiteral("czdlv"), QStringLiteral("mzdlv"),
      QStringLiteral("sgs"), QStringLiteral("sgsp"), QStringLiteral("badge"), QStringLiteral("shenjue"),
      QStringLiteral("astrolabe"), QStringLiteral("astrolabebr"), QStringLiteral("stargodSlotMaxLevel"),
      QStringLiteral("ip"), QStringLiteral("gps"), QStringLiteral("cps"),
      QStringLiteral("eps"), QStringLiteral("lss")};
  static const QStringList fields{
      QStringLiteral("id"), QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("fr"),
      QStringLiteral("n"), QStringLiteral("customName"), QStringLiteral("lv"), QStringLiteral("g"),
      QStringLiteral("gd"), QStringLiteral("_location"), QStringLiteral("_warehouseGroup"),
      QStringLiteral("_position"), QStringLiteral("_visualMismatch"), QStringLiteral("_metaOriginalName"),
      QStringLiteral("_metaAttributes"), QStringLiteral("_metaJobs"), QStringLiteral("_metaEra"), QStringLiteral("_metaRaceId"),
      QStringLiteral("zdl"), QStringLiteral("xzdl"), QStringLiteral("czdlv"), QStringLiteral("mzdlv"),
      QStringLiteral("sgs"), QStringLiteral("sgsp"), QStringLiteral("badge"), QStringLiteral("shenjue"),
      QStringLiteral("astrolabe"), QStringLiteral("astrolabebr"), QStringLiteral("stargodSlotMaxLevel"),
      QStringLiteral("ip"), QStringLiteral("gps"), QStringLiteral("cps"),
      QStringLiteral("eps"), QStringLiteral("lss")};
  QJsonObject result;
  for (const auto& key : fields) {
    if (detailFields.contains(key) && detail.contains(QStringLiteral("id"))) {
      if (detail.contains(key)) result.insert(key, detail.value(key));
    }
    else if (detailFields.contains(key) && detail.contains(key)) result.insert(key, detail.value(key));
    else if (brief.contains(key)) result.insert(key, brief.value(key));
    else if (detail.contains(key)) result.insert(key, detail.value(key));
  }
  return result;
}

PetAssetRecord AssetDerivation::derivePet(const PetAssetRecord& seed, const QJsonObject& stargods,
                                         const QJsonObject& astrolabe, const QJsonObject& pets) {
  return derivePetWithPower(seed, seed.detailAvailable
      ? calculatePetBattlePower(seed.pet, petPowerMetadataFromCatalog(seed.pet, seed.metadataSlotMaxLevel,
            stargods, astrolabe, pets)) : PetBattlePowerState{});
}

PetAssetRecord AssetDerivation::derivePetWithPower(const PetAssetRecord& seed, const PetBattlePowerState& power) {
  PetAssetRecord record;
  record.instanceId = seed.instanceId; record.raceId = seed.raceId;
  record.metadataSlotMaxLevel = seed.metadataSlotMaxLevel;
  record.observationVerified = seed.observationVerified;
  record.name = seed.name; record.location = seed.location;
  record.detailAvailable = seed.detailAvailable; record.pet = seed.pet;
  if (!record.detailAvailable) return record;
  record.currentPower = power.current; record.extremePower = power.extreme; record.highestPower = power.highest;
  record.currentPowerKnown = power.hasCurrent;
  record.extremePowerKnown = power.hasExtreme;
  record.highestPowerKnown = power.hasHighest;
  record.completionKnown = power.completionKnown;
  record.powerGapKnown = power.highestGapKnown;
  record.cultivationKnown = power.completionKnown;
  record.redStarKnown = power.stargodAcquisitionKnown;
  record.astrolabeKnown = power.astrolabeApplicabilityKnown &&
      (!power.astrolabeApplicable || (power.astrolabePowerKnown && power.breakthroughApplicabilityKnown &&
          (!power.breakthroughApplicable || power.breakthroughKnown)));
  record.highestPowerGap = power.highestGap;
  record.missingRedStars = power.missingRedStars;
  record.stargodLevelMissingSlots = power.stargodLevelMissingSlots;
  record.stargodSlotsKnown = power.stargodSlotsKnown;
  record.hasChangeableSlot = power.hasChangeableSlot; record.changeableRed = power.changeableOwnedRed;
  if (record.completionKnown && power.highest > 0)
    record.completionPercent = power.current >= power.highest ? 100
        : static_cast<int>((qint64(power.current) * 100 + power.highest / 2) / power.highest);
  record.fullyCultivated = power.isHighest;
  record.redStarMissing = record.redStarKnown && power.missingRedStars > 0;
  record.astrolabeMissing = power.astrolabeApplicabilityKnown && power.astrolabeApplicable &&
      ((power.breakthroughApplicable && power.breakthroughKnown && !power.breakthrough) || hasGap(power, QStringLiteral("asv")));
  record.sacredMissing = hasGap(power, QStringLiteral("sjv"));
  record.soulMissing = hasGap(power, QStringLiteral("bsv"));
  for (const auto& gap : power.componentGaps) {
    record.gapKeys.append(gap.key);
    record.gaps.append(QStringLiteral("%1 +%2").arg(gap.label).arg(gap.gap));
  }
  const auto addAction = [&record](const QString& key, const QString& text) {
    if (!record.gapKeys.contains(key)) record.gapKeys.append(key);
    record.gaps.append(text);
  };
  if (record.redStarMissing)
    addAction(QStringLiteral("sg_acquire"), QStringLiteral("缺少可用普通红星 %1 个种类（已含本宠背包）").arg(power.missingRedStars));
  if (power.stargodPowerKnown && power.adjustmentGain > 0)
    addAction(QStringLiteral("sg_equip"), QStringLiteral("已有星神待装备/调整，实际已装战力可提升 +%1").arg(power.adjustmentGain));
  if (power.stargodPowerKnown && power.ownedMaxStargodPowerKnown && power.stargodUpgradeGap > 0)
    addAction(QStringLiteral("sg_level"), QStringLiteral("星神等级待提升，已有星神升级后 +%1").arg(power.stargodUpgradeGap));
  if (power.hasChangeableSlot && power.changeableOwnedKnown && !power.changeableOwnedRed)
    addAction(QStringLiteral("sg_changeable"), QStringLiteral("万变栏仍缺红色万变（与普通红星分开）"));
  if (power.astrolabeApplicabilityKnown && power.astrolabeApplicable && power.breakthroughApplicable && power.breakthroughKnown && !power.breakthrough)
    addAction(QStringLiteral("astrolabe_breakthrough"), QStringLiteral("星轮未突破"));
  record.improvable = !record.fullyCultivated &&
      ((record.powerGapKnown && record.highestPowerGap > 0) || !record.gaps.isEmpty());
  return record;
}

void AssetDerivation::applySacredCultivation(PetAssetRecord* record, const ShopPetDerived& cultivation) {
  if (!record || !record->detailAvailable) return;
  if (!cultivation.sacredStateRequired) return;
  const auto star = cultivation.components[7], stage = cultivation.components[8];
  const bool unknown = star == ShopCultivationState::Unknown || stage == ShopCultivationState::Unknown;
  const bool starMissing = star == ShopCultivationState::Useful;
  const bool stageMissing = stage == ShopCultivationState::Useful;
  record->sacredMissing = starMissing || stageMissing;
  if (unknown) record->cultivationKnown = false;
  if (unknown || record->sacredMissing) record->fullyCultivated = false;
  const auto add = [record](const QString& key, const QString& text) {
    if (!record->gapKeys.contains(key)) record->gapKeys.append(key);
    if (!record->gaps.contains(text)) record->gaps.append(text);
  };
  if (starMissing) add(QStringLiteral("sacred_star"), QStringLiteral("源兽星级未满"));
  if (stageMissing) add(QStringLiteral("sacred_stage"), QStringLiteral("源兽阶级未满"));
  if (record->sacredMissing) record->improvable = true;
}

void AssetDerivation::applyCultivation(PetAssetRecord* record, const ShopPetDerived& cultivation) {
  applySacredCultivation(record,cultivation);
  if (!record || !record->detailAvailable) return;
  const auto add = [record](const QString& key,const QString& text) {
    if (!record->gapKeys.contains(key)) record->gapKeys.append(key);
    if (!record->gaps.contains(text)) record->gaps.append(text);
  };
  const auto apply = [record](bool required,ShopCultivationState first,ShopCultivationState second) {
    if (!required) return;
    const bool known = first != ShopCultivationState::Unknown && second != ShopCultivationState::Unknown;
    const bool missing = first == ShopCultivationState::Useful || second == ShopCultivationState::Useful;
    if (!known) record->cultivationKnown = false;
    if (!known || missing) record->fullyCultivated = false;
    if (missing) record->improvable = true;
  };
  const auto badgeLevel = cultivation.components[3], badgeAwaken = cultivation.components[4];
  apply(cultivation.badgeStateRequired,badgeLevel,badgeAwaken);
  if (cultivation.badgeStateRequired) {
    record->soulMissing = badgeLevel == ShopCultivationState::Useful || badgeAwaken == ShopCultivationState::Useful;
    if (badgeLevel == ShopCultivationState::Useful) add(QStringLiteral("badge_level"),QStringLiteral("元魂等级未满"));
    if (badgeAwaken == ShopCultivationState::Useful) add(QStringLiteral("badge_awaken"),QStringLiteral("专属元魂未觉醒"));
  }
  const auto nodes = cultivation.components[6], breakthrough = cultivation.components[13];
  apply(cultivation.astrolabeStateRequired,nodes,breakthrough);
  if (cultivation.astrolabeStateRequired) {
    record->astrolabeKnown = nodes != ShopCultivationState::Unknown && breakthrough != ShopCultivationState::Unknown;
    record->astrolabeMissing = nodes == ShopCultivationState::Useful || breakthrough == ShopCultivationState::Useful;
    if (nodes == ShopCultivationState::Useful) add(QStringLiteral("astrolabe_light"),QStringLiteral("星轮仍有可点亮节点"));
    if (breakthrough == ShopCultivationState::Useful) add(QStringLiteral("astrolabe_breakthrough"),QStringLiteral("星轮未突破"));
  }
}

void AssetDerivation::accumulate(AccountAssetOverview* overview, const PetAssetRecord& record) {
  ++overview->totalPets;
  if (overview->totalPets == 1) overview->totalCurrentPowerKnown = true;
  overview->totalCurrentPowerKnown &= record.currentPowerKnown;
  if (record.location == QStringLiteral("背包")) ++overview->backpackPets;
  else if (record.location == QStringLiteral("精英仓库")) ++overview->eliteWarehousePets;
  else ++overview->normalWarehousePets;
  if (!record.detailAvailable) ++overview->missingDetailPets;
  if (record.fullyCultivated) ++overview->fullyCultivatedPets;
  if (record.improvable) ++overview->improvablePets;
  if (record.redStarMissing) ++overview->redStarMissingPets;
  if (record.astrolabeMissing) ++overview->astrolabeMissingPets;
  if (record.sacredMissing) ++overview->sacredMissingPets;
  if (record.soulMissing) ++overview->soulMissingPets;
  if (record.shopImprovable) ++overview->shopImprovablePets;
  if (record.currentPowerKnown) {
    qint64 total = 0;
    if (DomainNumeric::checkedAdd(overview->totalCurrentPower, record.currentPower, &total)) overview->totalCurrentPower = total;
    else overview->totalCurrentPowerKnown = false;
  }
}

bool AssetDerivation::matchesFilter(const PetAssetRecord& pet,
                                  PetAssetFilter filter) {
  switch (filter) {
    case PetAssetFilter::All: return true;
    case PetAssetFilter::Backpack: return pet.location == QStringLiteral("背包");
    case PetAssetFilter::NormalWarehouse:
      return pet.location == QStringLiteral("普通仓库");
    case PetAssetFilter::EliteWarehouse:
      return pet.location == QStringLiteral("精英仓库");
    case PetAssetFilter::FullyCultivated: return pet.fullyCultivated;
    case PetAssetFilter::Improvable: return pet.improvable;
    case PetAssetFilter::MissingDetail: return !pet.detailAvailable;
    case PetAssetFilter::RedStarMissing: return pet.redStarMissing;
    case PetAssetFilter::AstrolabeMissing: return pet.astrolabeMissing;
    case PetAssetFilter::SacredMissing: return pet.sacredMissing;
    case PetAssetFilter::SoulMissing: return pet.soulMissing;
    case PetAssetFilter::ShopImprovable: return pet.shopImprovable;
    case PetAssetFilter::StargodEquipNeeded: return pet.gapKeys.contains(QStringLiteral("sg_equip"));
    case PetAssetFilter::StargodUpgradeNeeded: return pet.gapKeys.contains(QStringLiteral("sg_level"));
    case PetAssetFilter::ChangeableMissing: return pet.gapKeys.contains(QStringLiteral("sg_changeable"));
    case PetAssetFilter::AnalysisIncomplete: return !pet.detailAvailable || !pet.cultivationKnown;
  }
  return true;
}
