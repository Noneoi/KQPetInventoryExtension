#include "pet_analysis_facts.h"
#include "asset_derivation.h"
#include "checked_json_numbers.h"
#include "compiled_shop_catalog.h"
#include "pet_identity.h"
#include "pet_power_calculator.h"
#include <QJsonArray>
#include <QSet>
#include <iterator>
#include <limits>

namespace {
template<class T> struct IntegerField { const char* name; int T::* member; };
template<class T> struct BooleanField { const char* name; bool T::* member; };
#define INTEGER_FIELD(T, name) {#name, &T::name}
#define BOOLEAN_FIELD(T, name) {#name, &T::name}
const IntegerField<PetAssetRecord> assetIntegers[]{
    INTEGER_FIELD(PetAssetRecord,raceId), INTEGER_FIELD(PetAssetRecord,metadataSlotMaxLevel),
    INTEGER_FIELD(PetAssetRecord,currentPower), INTEGER_FIELD(PetAssetRecord,extremePower),
    INTEGER_FIELD(PetAssetRecord,highestPower), INTEGER_FIELD(PetAssetRecord,completionPercent),
    INTEGER_FIELD(PetAssetRecord,highestPowerGap), INTEGER_FIELD(PetAssetRecord,missingRedStars),
    INTEGER_FIELD(PetAssetRecord,stargodLevelMissingSlots)};
const BooleanField<PetAssetRecord> assetBooleans[]{
    BOOLEAN_FIELD(PetAssetRecord,observationVerified), BOOLEAN_FIELD(PetAssetRecord,detailAvailable),
    BOOLEAN_FIELD(PetAssetRecord,currentPowerKnown), BOOLEAN_FIELD(PetAssetRecord,extremePowerKnown),
    BOOLEAN_FIELD(PetAssetRecord,highestPowerKnown),
    BOOLEAN_FIELD(PetAssetRecord,completionKnown), BOOLEAN_FIELD(PetAssetRecord,powerGapKnown),
    BOOLEAN_FIELD(PetAssetRecord,cultivationKnown), BOOLEAN_FIELD(PetAssetRecord,redStarKnown),
    BOOLEAN_FIELD(PetAssetRecord,astrolabeKnown), BOOLEAN_FIELD(PetAssetRecord,fullyCultivated),
    BOOLEAN_FIELD(PetAssetRecord,improvable), BOOLEAN_FIELD(PetAssetRecord,redStarMissing),
    BOOLEAN_FIELD(PetAssetRecord,astrolabeMissing), BOOLEAN_FIELD(PetAssetRecord,sacredMissing),
    BOOLEAN_FIELD(PetAssetRecord,soulMissing), BOOLEAN_FIELD(PetAssetRecord,shopImprovable),
    BOOLEAN_FIELD(PetAssetRecord,stargodSlotsKnown), BOOLEAN_FIELD(PetAssetRecord,hasChangeableSlot),
    BOOLEAN_FIELD(PetAssetRecord,changeableRed)};
const IntegerField<PetBattlePowerState> powerIntegers[]{
    INTEGER_FIELD(PetBattlePowerState,serverCurrent), INTEGER_FIELD(PetBattlePowerState,current),
    INTEGER_FIELD(PetBattlePowerState,extreme), INTEGER_FIELD(PetBattlePowerState,highest),
    INTEGER_FIELD(PetBattlePowerState,stargodSlots), INTEGER_FIELD(PetBattlePowerState,equippedStars),
    INTEGER_FIELD(PetBattlePowerState,backpackStars), INTEGER_FIELD(PetBattlePowerState,availableStars),
    INTEGER_FIELD(PetBattlePowerState,missingStars), INTEGER_FIELD(PetBattlePowerState,redStars),
    INTEGER_FIELD(PetBattlePowerState,goldStars), INTEGER_FIELD(PetBattlePowerState,missingRedStars),
    INTEGER_FIELD(PetBattlePowerState,knownExtremeGap), INTEGER_FIELD(PetBattlePowerState,equippedStargodPower),
    INTEGER_FIELD(PetBattlePowerState,currentStargodPower), INTEGER_FIELD(PetBattlePowerState,bestOrdinaryStargodPower),
    INTEGER_FIELD(PetBattlePowerState,changeableStargodPower), INTEGER_FIELD(PetBattlePowerState,targetStargodPower),
    INTEGER_FIELD(PetBattlePowerState,highestStargodPower), INTEGER_FIELD(PetBattlePowerState,changeableQuality),
    INTEGER_FIELD(PetBattlePowerState,stargodMaxLevel), INTEGER_FIELD(PetBattlePowerState,stargodLevelMissingSlots),
    INTEGER_FIELD(PetBattlePowerState,astrolabeBonus), INTEGER_FIELD(PetBattlePowerState,highestGap),
    INTEGER_FIELD(PetBattlePowerState,equippedCurrent), INTEGER_FIELD(PetBattlePowerState,ownedMaxStargodPower),
    INTEGER_FIELD(PetBattlePowerState,adjustmentGain), INTEGER_FIELD(PetBattlePowerState,stargodUpgradeGap),
    INTEGER_FIELD(PetBattlePowerState,stargodAcquisitionGap), INTEGER_FIELD(PetBattlePowerState,changeableOwnedQuality),
    INTEGER_FIELD(PetBattlePowerState,astrolabeTargetBonus), INTEGER_FIELD(PetBattlePowerState,componentCurrentTotal),
    INTEGER_FIELD(PetBattlePowerState,componentExtremeTotal), INTEGER_FIELD(PetBattlePowerState,astrolabeCurrent),
    INTEGER_FIELD(PetBattlePowerState,astrolabeMaximum), INTEGER_FIELD(PetBattlePowerState,astrolabeUnlitNodes),
    INTEGER_FIELD(PetBattlePowerState,astrolabeSelectedNodes)};
const BooleanField<PetBattlePowerState> powerBooleans[]{
    BOOLEAN_FIELD(PetBattlePowerState,breakthrough), BOOLEAN_FIELD(PetBattlePowerState,stargodSlotsKnown),
    BOOLEAN_FIELD(PetBattlePowerState,stargodBackpackKnown), BOOLEAN_FIELD(PetBattlePowerState,stargodFull),
    BOOLEAN_FIELD(PetBattlePowerState,redStargodFull), BOOLEAN_FIELD(PetBattlePowerState,hasChangeableSlot),
    BOOLEAN_FIELD(PetBattlePowerState,changeableRed), BOOLEAN_FIELD(PetBattlePowerState,stargodLevelsFull),
    BOOLEAN_FIELD(PetBattlePowerState,stargodPowerKnown), BOOLEAN_FIELD(PetBattlePowerState,currentLocallyCalculated),
    BOOLEAN_FIELD(PetBattlePowerState,hasCurrent), BOOLEAN_FIELD(PetBattlePowerState,hasExtreme),
    BOOLEAN_FIELD(PetBattlePowerState,hasHighest), BOOLEAN_FIELD(PetBattlePowerState,isHighest),
    BOOLEAN_FIELD(PetBattlePowerState,hasServerCurrent), BOOLEAN_FIELD(PetBattlePowerState,equippedCurrentKnown),
    BOOLEAN_FIELD(PetBattlePowerState,highestGapKnown), BOOLEAN_FIELD(PetBattlePowerState,stargodQualitiesKnown),
    BOOLEAN_FIELD(PetBattlePowerState,stargodAcquisitionKnown), BOOLEAN_FIELD(PetBattlePowerState,ownedMaxStargodPowerKnown),
    BOOLEAN_FIELD(PetBattlePowerState,changeableOwnedKnown), BOOLEAN_FIELD(PetBattlePowerState,changeableOwnedRed),
    BOOLEAN_FIELD(PetBattlePowerState,breakthroughKnown), BOOLEAN_FIELD(PetBattlePowerState,astrolabeApplicable),
    BOOLEAN_FIELD(PetBattlePowerState,breakthroughApplicable), BOOLEAN_FIELD(PetBattlePowerState,breakthroughApplicabilityKnown),
    BOOLEAN_FIELD(PetBattlePowerState,astrolabeApplicabilityKnown), BOOLEAN_FIELD(PetBattlePowerState,astrolabePowerKnown),
    BOOLEAN_FIELD(PetBattlePowerState,componentTotalsKnown), BOOLEAN_FIELD(PetBattlePowerState,completionKnown)};
const IntegerField<PetBattlePowerComponent> componentIntegers[]{
    INTEGER_FIELD(PetBattlePowerComponent,current), INTEGER_FIELD(PetBattlePowerComponent,extreme),
    INTEGER_FIELD(PetBattlePowerComponent,highest), INTEGER_FIELD(PetBattlePowerComponent,gap)};
const BooleanField<PetBattlePowerComponent> componentBooleans[]{
    BOOLEAN_FIELD(PetBattlePowerComponent,applicable), BOOLEAN_FIELD(PetBattlePowerComponent,currentKnown),
    BOOLEAN_FIELD(PetBattlePowerComponent,extremeKnown), BOOLEAN_FIELD(PetBattlePowerComponent,highestKnown),
    BOOLEAN_FIELD(PetBattlePowerComponent,gapKnown)};
const IntegerField<PetStargodSlotPower> slotIntegers[]{
    INTEGER_FIELD(PetStargodSlotPower,slot), INTEGER_FIELD(PetStargodSlotPower,level),
    INTEGER_FIELD(PetStargodSlotPower,equippedId), INTEGER_FIELD(PetStargodSlotPower,ownedBestId),
    INTEGER_FIELD(PetStargodSlotPower,equippedPower), INTEGER_FIELD(PetStargodSlotPower,ownedPower),
    INTEGER_FIELD(PetStargodSlotPower,ownedMaxPower), INTEGER_FIELD(PetStargodSlotPower,highestPower)};
const BooleanField<PetStargodSlotPower> slotBooleans[]{
    BOOLEAN_FIELD(PetStargodSlotPower,changeable), BOOLEAN_FIELD(PetStargodSlotPower,equippedKnown),
    BOOLEAN_FIELD(PetStargodSlotPower,ownedKnown), BOOLEAN_FIELD(PetStargodSlotPower,ownedMaxKnown),
    BOOLEAN_FIELD(PetStargodSlotPower,highestKnown), BOOLEAN_FIELD(PetStargodSlotPower,fromBackpack)};
#undef INTEGER_FIELD
#undef BOOLEAN_FIELD
bool fail(QString* error, const QString& message) {
  if (error) *error = message;
  return false;
}
quint64 textBytes(const QString& value) {
  return value.isEmpty() ? 0 : 64 + quint64(qMax(value.size(),value.capacity())) * sizeof(QChar);
}
quint64 listBytes(const QStringList& values) {
  quint64 bytes = quint64(values.capacity()) * sizeof(QString);
  for (const auto& value : values) bytes += textBytes(value);
  return bytes;
}
bool sourceIdentityValid(const PetAssetRecord& seed, QString* error) {
  if (seed.instanceId <= 0 || seed.raceId < 0 || seed.metadataSlotMaxLevel < 0)
    return fail(error,QStringLiteral("invalid fact identity or metadata level"));
  qint64 id = 0;
  if (seed.pet.contains(QStringLiteral("id"))) {
    if (!DomainNumeric::checkedInteger(seed.pet.value(QStringLiteral("id")),&id,1) || id != seed.instanceId)
      return fail(error,QStringLiteral("raw/summary instance IDs differ"));
  } else if (seed.detailAvailable) return fail(error,QStringLiteral("complete detail is missing its instance ID"));
  if ((seed.pet.contains(QStringLiteral("r")) || seed.pet.contains(QStringLiteral("ri"))) && petRaceId(seed.pet) != seed.raceId)
    return fail(error,QStringLiteral("raw/summary race IDs differ"));
  return true;
}
}

std::optional<PetAnalysisFacts> derivePetAnalysisFacts(const PetAssetRecord& seed,
    const ShopPetMetadataSnapshot& metadata, AlgorithmPipelineStats* stats, QString* error) {
  if (!sourceIdentityValid(seed,error)) return std::nullopt;
  PetAnalysisFacts facts;
  if (seed.detailAvailable) {
    if (stats) ++stats->rawPowerCalculations;
    facts.battlePower = calculatePetBattlePower(seed.pet, petPowerMetadataFromCatalog(seed.pet,
        seed.metadataSlotMaxLevel, metadata.stargods, metadata.astrolabe, metadata.pets));
    facts.eligibility = deriveShopPetWithPower(seed.pet,true,metadata,facts.battlePower,stats);
  } else {
    facts.eligibility.raceId = seed.raceId;
    facts.eligibility.metadataRaceId = seed.pet.value(QStringLiteral("_metaRaceId")).toInt();
    facts.eligibility.hasData = !seed.pet.isEmpty();
  }
  facts.asset = AssetDerivation::derivePetWithPower(seed,facts.battlePower);
  AssetDerivation::applyCultivation(&facts.asset,facts.eligibility);
  facts.asset.pet = AssetDerivation::identityFields(seed.pet);
  if (!facts.asset.pet.contains(QStringLiteral("id"))) facts.asset.pet.insert(QStringLiteral("id"),seed.instanceId);
  if (!facts.asset.pet.contains(QStringLiteral("r")) && !facts.asset.pet.contains(QStringLiteral("ri")) && seed.raceId > 0)
    facts.asset.pet.insert(QStringLiteral("r"),seed.raceId);
  facts.asset.memoryRetention.reset();
  facts.asset.shopImprovable = false; // An account/catalog decision belongs to the later job.
  if (!validatePetAnalysisFacts(facts,error)) return std::nullopt;
  return facts;
}

bool validatePetAnalysisFacts(const PetAnalysisFacts& facts, QString* error) {
  const auto& asset = facts.asset;
  const auto& eligibility = facts.eligibility;
  const auto groupKnown = [&eligibility](bool required,int first,int second) {
    return !required || (eligibility.components[first] != ShopCultivationState::Unknown && eligibility.components[second] != ShopCultivationState::Unknown);
  };
  const auto groupMissing = [&eligibility](bool required,int first,int second) {
    return required && (eligibility.components[first] == ShopCultivationState::Useful || eligibility.components[second] == ShopCultivationState::Useful);
  };
  const bool detailedCultivationKnown = groupKnown(eligibility.sacredStateRequired,7,8) &&
      groupKnown(eligibility.badgeStateRequired,3,4) && groupKnown(eligibility.astrolabeStateRequired,6,13);
  const bool detailedCultivationMissing = groupMissing(eligibility.sacredStateRequired,7,8) ||
      groupMissing(eligibility.badgeStateRequired,3,4) || groupMissing(eligibility.astrolabeStateRequired,6,13);
  if (facts.analysisVersion != AssetAnalysisVersion::kCurrentAnalysis)
    return fail(error,QStringLiteral("fact analysis version differs"));
  if (!sourceIdentityValid(asset,error)) return false;
  qint64 id = 0;
  if (!DomainNumeric::checkedInteger(asset.pet.value(QStringLiteral("id")),&id,1) || id != asset.instanceId)
    return fail(error,QStringLiteral("compact identity is missing or inconsistent"));
  if (asset.pet != AssetDerivation::identityFields(asset.pet))
    return fail(error,QStringLiteral("compact facts retain non-identity JSON"));
  if (eligibility.raceId < 0 || eligibility.metadataRaceId < 0 || asset.raceId != eligibility.raceId ||
      eligibility.metadataRaceId != asset.pet.value(QStringLiteral("_metaRaceId")).toInt() ||
      asset.detailAvailable != eligibility.hasFullDetail)
    return fail(error,QStringLiteral("compact identity and eligibility differ"));
  for (const auto state : eligibility.components)
    if (state != ShopCultivationState::Unknown && state != ShopCultivationState::Useful && state != ShopCultivationState::Full)
      return fail(error,QStringLiteral("invalid cultivation component state"));
  if (eligibility.changeableLevel < 0 ||
      (eligibility.changeableLevelKnown && (!eligibility.changeableSlotKnown || !eligibility.hasChangeableSlot || eligibility.changeableLevel <= 0)) ||
      (eligibility.hasChangeableSlot && !eligibility.changeableSlotKnown))
    return fail(error,QStringLiteral("invalid changeable slot cultivation facts"));
  if (asset.gapKeys.size() > 32 || asset.gaps.size() > 40 || facts.battlePower.componentGaps.size() > 64 ||
      facts.battlePower.components.size() > 64 || facts.battlePower.stargodDetails.size() > 64 ||
      facts.battlePower.unknownReasons.size() > 128 ||
      asset.currentPower < 0 || asset.extremePower < 0 || asset.highestPower < 0 || asset.highestPowerGap < 0 ||
      asset.completionPercent < 0 || asset.completionPercent > 100 || asset.missingRedStars < 0 ||
      asset.stargodLevelMissingSlots < 0)
    return fail(error,QStringLiteral("invalid derived fact ranges"));
  for (const auto& field : powerIntegers) if (facts.battlePower.*(field.member) < 0)
    return fail(error,QStringLiteral("negative battle-power fact"));
  if ((facts.battlePower.breakthroughApplicable && !facts.battlePower.breakthroughApplicabilityKnown) ||
      (!facts.battlePower.breakthroughApplicable && (facts.battlePower.breakthrough || facts.battlePower.breakthroughKnown ||
          facts.battlePower.astrolabeBonus || facts.battlePower.astrolabeTargetBonus)))
    return fail(error,QStringLiteral("breakthrough power disagrees with era applicability"));
  for (const auto& gap : facts.battlePower.componentGaps)
    if (gap.current < 0 || gap.extreme < gap.current || gap.gap != gap.extreme - gap.current)
      return fail(error,QStringLiteral("invalid battle-power gap"));
  QSet<QString> componentKeys;
  for (const auto& component : facts.battlePower.components) {
    if (component.key.isEmpty() || componentKeys.contains(component.key))
      return fail(error,QStringLiteral("duplicate or empty battle-power component"));
    componentKeys.insert(component.key);
    for (const auto& field : componentIntegers) if (component.*(field.member) < 0)
      return fail(error,QStringLiteral("negative battle-power component"));
    if (component.gapKnown && (!component.currentKnown || !component.highestKnown ||
        component.gap != qMax(0,component.highest-component.current)))
      return fail(error,QStringLiteral("inconsistent battle-power component gap"));
  }
  for (const auto& slot : facts.battlePower.stargodDetails)
    for (const auto& field : slotIntegers) if (slot.*(field.member) < 0)
      return fail(error,QStringLiteral("negative star-slot fact"));
  if (!asset.detailAvailable) {
    if (eligibility.sacredStateRequired) return fail(error,QStringLiteral("unknown detail contains sacred state"));
    if (eligibility.badgeStateRequired || eligibility.astrolabeStateRequired)
      return fail(error,QStringLiteral("unknown detail contains badge or astrolabe state"));
    if (eligibility.changeableSlotKnown || eligibility.hasChangeableSlot || eligibility.changeableLevelKnown || eligibility.changeableLevel)
      return fail(error,QStringLiteral("unknown detail contains changeable slot facts"));
    if (asset.currentPowerKnown || asset.extremePowerKnown || asset.highestPowerKnown || asset.completionKnown || asset.cultivationKnown ||
        asset.redStarKnown || asset.astrolabeKnown || asset.fullyCultivated || asset.improvable ||
        facts.battlePower.hasCurrent || facts.battlePower.hasExtreme || facts.battlePower.hasHighest)
      return fail(error,QStringLiteral("unknown detail contains fabricated derived state"));
    for (const auto state : eligibility.components) if (state != ShopCultivationState::Unknown)
      return fail(error,QStringLiteral("unknown detail contains known cultivation components"));
    for (const auto& field : powerIntegers) if (facts.battlePower.*(field.member) != 0)
      return fail(error,QStringLiteral("unknown detail contains battle-power values"));
    for (const auto& field : powerBooleans) if (facts.battlePower.*(field.member))
      return fail(error,QStringLiteral("unknown detail contains battle-power flags"));
    if (!facts.battlePower.componentGaps.isEmpty() || !facts.battlePower.components.isEmpty() ||
        !facts.battlePower.stargodDetails.isEmpty() || !facts.battlePower.unknownReasons.isEmpty())
      return fail(error,QStringLiteral("unknown detail contains battle-power analysis"));
  } else if (!eligibility.hasData || asset.currentPower != facts.battlePower.current ||
      asset.extremePower != facts.battlePower.extreme || asset.highestPower != facts.battlePower.highest ||
      asset.currentPowerKnown != facts.battlePower.hasCurrent || asset.extremePowerKnown != facts.battlePower.hasExtreme ||
      asset.highestPowerKnown != facts.battlePower.hasHighest ||
      asset.completionKnown != facts.battlePower.completionKnown ||
      asset.powerGapKnown != facts.battlePower.highestGapKnown ||
      asset.cultivationKnown != (facts.battlePower.completionKnown && detailedCultivationKnown) ||
      (asset.fullyCultivated && (!facts.battlePower.isHighest || !asset.cultivationKnown || detailedCultivationMissing)))
    return fail(error,QStringLiteral("asset and battle-power facts differ"));
  return true;
}

quint64 petAnalysisFactsRetainedBytes(const PetAnalysisFacts& facts) {
  const auto& asset = facts.asset;
  quint64 bytes = sizeof(PetAnalysisFacts) + textBytes(asset.name) + textBytes(asset.location) +
      listBytes(asset.gapKeys) + listBytes(asset.gaps) +
      quint64(facts.battlePower.componentGaps.capacity()) * sizeof(PetBattlePowerGap);
  for (auto field = asset.pet.constBegin(); field != asset.pet.constEnd(); ++field)
    bytes += 128 + textBytes(field.key()) + (field.value().isString() ? textBytes(field.value().toString()) : 0);
  for (const auto& gap : facts.battlePower.componentGaps) bytes += textBytes(gap.key) + textBytes(gap.label);
  bytes += quint64(facts.battlePower.components.capacity()) * sizeof(PetBattlePowerComponent) +
      quint64(facts.battlePower.stargodDetails.capacity()) * sizeof(PetStargodSlotPower) + listBytes(facts.battlePower.unknownReasons);
  for (const auto& component : facts.battlePower.components)
    bytes += textBytes(component.key) + textBytes(component.label) + textBytes(component.explanation);
  for (const auto& slot : facts.battlePower.stargodDetails)
    bytes += textBytes(slot.equippedName) + textBytes(slot.ownedBestName) + textBytes(slot.action);
  return bytes;
}

namespace {
template<class T, size_t N, size_t M>
QJsonObject scalarObject(const T& value, const IntegerField<T>(&integers)[N], const BooleanField<T>(&booleans)[M]) {
  QJsonObject object;
  for (const auto& field : integers) object.insert(QString::fromLatin1(field.name),value.*(field.member));
  for (const auto& field : booleans) object.insert(QString::fromLatin1(field.name),value.*(field.member));
  return object;
}
template<class T, size_t N, size_t M>
bool readScalars(const QJsonObject& object, T* value, const IntegerField<T>(&integers)[N], const BooleanField<T>(&booleans)[M]) {
  for (const auto& field : integers) {
    qint64 parsed = 0;
    const auto number = object.value(QString::fromLatin1(field.name));
    if (!number.isDouble() || !DomainNumeric::checkedInteger(number,&parsed,0,std::numeric_limits<int>::max())) return false;
    value->*(field.member) = int(parsed);
  }
  for (const auto& field : booleans) {
    const auto flag = object.value(QString::fromLatin1(field.name));
    if (!flag.isBool()) return false;
    value->*(field.member) = flag.toBool();
  }
  return true;
}
bool strings(const QJsonValue& value, QStringList* result, int maximum) {
  if (!value.isArray() || value.toArray().size() > maximum) return false;
  for (const auto& text : value.toArray()) {
    if (!text.isString()) return false;
    result->append(text.toString());
  }
  return true;
}
}

QJsonObject petAnalysisFactsToJson(const PetAnalysisFacts& facts) {
  if (!validatePetAnalysisFacts(facts)) return {};
  auto asset = scalarObject(facts.asset,assetIntegers,assetBooleans);
  asset.insert(QStringLiteral("instanceId"),QString::number(facts.asset.instanceId));
  asset.insert(QStringLiteral("name"),facts.asset.name); asset.insert(QStringLiteral("location"),facts.asset.location);
  asset.insert(QStringLiteral("gapKeys"),QJsonArray::fromStringList(facts.asset.gapKeys));
  asset.insert(QStringLiteral("gaps"),QJsonArray::fromStringList(facts.asset.gaps));
  auto identity = facts.asset.pet;
  identity.insert(QStringLiteral("id"),QString::number(facts.asset.instanceId));
  asset.insert(QStringLiteral("identity"),identity);
  QJsonArray components;
  for (const auto value : facts.eligibility.components) components.append(int(value));
  const QJsonObject eligibility{{QStringLiteral("raceId"),facts.eligibility.raceId},
      {QStringLiteral("metadataRaceId"),facts.eligibility.metadataRaceId},
      {QStringLiteral("hasFullDetail"),facts.eligibility.hasFullDetail},
      {QStringLiteral("hasData"),facts.eligibility.hasData},
      {QStringLiteral("sacredStateRequired"),facts.eligibility.sacredStateRequired},
      {QStringLiteral("badgeStateRequired"),facts.eligibility.badgeStateRequired},
      {QStringLiteral("astrolabeStateRequired"),facts.eligibility.astrolabeStateRequired},
      {QStringLiteral("changeableSlotKnown"),facts.eligibility.changeableSlotKnown},
      {QStringLiteral("hasChangeableSlot"),facts.eligibility.hasChangeableSlot},
      {QStringLiteral("changeableLevelKnown"),facts.eligibility.changeableLevelKnown},
      {QStringLiteral("changeableLevel"),facts.eligibility.changeableLevel},{QStringLiteral("components"),components}};
  auto power = scalarObject(facts.battlePower,powerIntegers,powerBooleans);
  QJsonArray gaps;
  for (const auto& gap : facts.battlePower.componentGaps)
    gaps.append(QJsonObject{{QStringLiteral("key"),gap.key},{QStringLiteral("label"),gap.label},
        {QStringLiteral("current"),gap.current},{QStringLiteral("extreme"),gap.extreme},{QStringLiteral("gap"),gap.gap}});
  power.insert(QStringLiteral("componentGaps"),gaps);
  QJsonArray powerComponents, slotEntries;
  for (const auto& component : facts.battlePower.components) {
    auto value = scalarObject(component,componentIntegers,componentBooleans);
    value.insert(QStringLiteral("key"),component.key); value.insert(QStringLiteral("label"),component.label);
    value.insert(QStringLiteral("explanation"),component.explanation); powerComponents.append(value);
  }
  for (const auto& slot : facts.battlePower.stargodDetails) {
    auto value = scalarObject(slot,slotIntegers,slotBooleans);
    value.insert(QStringLiteral("equippedName"),slot.equippedName); value.insert(QStringLiteral("ownedBestName"),slot.ownedBestName);
    value.insert(QStringLiteral("action"),slot.action); slotEntries.append(value);
  }
  power.insert(QStringLiteral("components"),powerComponents); power.insert(QStringLiteral("stargodDetails"),slotEntries);
  power.insert(QStringLiteral("unknownReasons"),QJsonArray::fromStringList(facts.battlePower.unknownReasons));
  return {{QStringLiteral("schema"),1},{QStringLiteral("analysisVersion"),facts.analysisVersion},
      {QStringLiteral("asset"),asset},{QStringLiteral("eligibility"),eligibility},{QStringLiteral("battlePower"),power}};
}

std::optional<PetAnalysisFacts> petAnalysisFactsFromJson(const QJsonObject& object, QString* error) {
  const auto invalid = [error]() -> std::optional<PetAnalysisFacts> {
    fail(error,QStringLiteral("invalid compact-facts index schema or fields")); return std::nullopt;
  };
  qint64 version = 0, schema = 0;
  if (object.size() != 5 || !object.value(QStringLiteral("schema")).isDouble() ||
      !object.value(QStringLiteral("analysisVersion")).isDouble() ||
      !DomainNumeric::checkedInteger(object.value(QStringLiteral("schema")),&schema,1,1) ||
      !DomainNumeric::checkedInteger(object.value(QStringLiteral("analysisVersion")),&version,
          AssetAnalysisVersion::kCurrentAnalysis,AssetAnalysisVersion::kCurrentAnalysis) ||
      !object.value(QStringLiteral("asset")).isObject() || !object.value(QStringLiteral("eligibility")).isObject() ||
      !object.value(QStringLiteral("battlePower")).isObject()) return invalid();
  PetAnalysisFacts facts;
  facts.analysisVersion = int(version);
  const auto asset = object.value(QStringLiteral("asset")).toObject();
  if (asset.size() != qsizetype(std::size(assetIntegers)+std::size(assetBooleans)+6) ||
      !readScalars(asset,&facts.asset,assetIntegers,assetBooleans) ||
      !asset.value(QStringLiteral("instanceId")).isString() ||
      !DomainNumeric::checkedInteger(asset.value(QStringLiteral("instanceId")),&facts.asset.instanceId,1) ||
      !asset.value(QStringLiteral("name")).isString() || !asset.value(QStringLiteral("location")).isString() ||
      !asset.value(QStringLiteral("identity")).isObject() ||
      !strings(asset.value(QStringLiteral("gapKeys")),&facts.asset.gapKeys,32) ||
      !strings(asset.value(QStringLiteral("gaps")),&facts.asset.gaps,40)) return invalid();
  facts.asset.name = asset.value(QStringLiteral("name")).toString();
  facts.asset.location = asset.value(QStringLiteral("location")).toString();
  facts.asset.pet = asset.value(QStringLiteral("identity")).toObject();
  const auto eligibility = object.value(QStringLiteral("eligibility")).toObject();
  qint64 race = 0, metadataRace = 0, changeableLevel = 0;
  if (eligibility.size() != 12 || !DomainNumeric::checkedInteger(eligibility.value(QStringLiteral("raceId")),&race,0,std::numeric_limits<int>::max()) ||
      !DomainNumeric::checkedInteger(eligibility.value(QStringLiteral("metadataRaceId")),&metadataRace,0,std::numeric_limits<int>::max()) ||
      !eligibility.value(QStringLiteral("hasFullDetail")).isBool() || !eligibility.value(QStringLiteral("hasData")).isBool() ||
      !eligibility.value(QStringLiteral("sacredStateRequired")).isBool() ||
      !eligibility.value(QStringLiteral("badgeStateRequired")).isBool() || !eligibility.value(QStringLiteral("astrolabeStateRequired")).isBool() ||
      !eligibility.value(QStringLiteral("changeableSlotKnown")).isBool() || !eligibility.value(QStringLiteral("hasChangeableSlot")).isBool() ||
      !eligibility.value(QStringLiteral("changeableLevelKnown")).isBool() ||
      !DomainNumeric::checkedInteger(eligibility.value(QStringLiteral("changeableLevel")),&changeableLevel,0,std::numeric_limits<int>::max()) ||
      !eligibility.value(QStringLiteral("components")).isArray() || eligibility.value(QStringLiteral("components")).toArray().size() != kShopCultivationComponentCount) return invalid();
  facts.eligibility.raceId = int(race); facts.eligibility.metadataRaceId = int(metadataRace);
  facts.eligibility.hasFullDetail = eligibility.value(QStringLiteral("hasFullDetail")).toBool();
  facts.eligibility.hasData = eligibility.value(QStringLiteral("hasData")).toBool();
  facts.eligibility.sacredStateRequired = eligibility.value(QStringLiteral("sacredStateRequired")).toBool();
  facts.eligibility.badgeStateRequired = eligibility.value(QStringLiteral("badgeStateRequired")).toBool();
  facts.eligibility.astrolabeStateRequired = eligibility.value(QStringLiteral("astrolabeStateRequired")).toBool();
  facts.eligibility.changeableSlotKnown = eligibility.value(QStringLiteral("changeableSlotKnown")).toBool();
  facts.eligibility.hasChangeableSlot = eligibility.value(QStringLiteral("hasChangeableSlot")).toBool();
  facts.eligibility.changeableLevelKnown = eligibility.value(QStringLiteral("changeableLevelKnown")).toBool();
  facts.eligibility.changeableLevel = int(changeableLevel);
  const auto components = eligibility.value(QStringLiteral("components")).toArray();
  for (std::size_t index = 0; index < kShopCultivationComponentCount; ++index) {
    qint64 state = 0;
    if (!DomainNumeric::checkedInteger(components[qsizetype(index)],&state,0,2)) return invalid();
    facts.eligibility.components[index] = static_cast<ShopCultivationState>(state);
  }
  const auto power = object.value(QStringLiteral("battlePower")).toObject();
  if (power.size() != qsizetype(std::size(powerIntegers)+std::size(powerBooleans)+4) ||
      !readScalars(power,&facts.battlePower,powerIntegers,powerBooleans) ||
      !power.value(QStringLiteral("componentGaps")).isArray() || power.value(QStringLiteral("componentGaps")).toArray().size() > 64 ||
      !power.value(QStringLiteral("components")).isArray() || power.value(QStringLiteral("components")).toArray().size() > 64 ||
      !power.value(QStringLiteral("stargodDetails")).isArray() || power.value(QStringLiteral("stargodDetails")).toArray().size() > 64 ||
      !strings(power.value(QStringLiteral("unknownReasons")),&facts.battlePower.unknownReasons,128)) return invalid();
  for (const auto& value : power.value(QStringLiteral("componentGaps")).toArray()) {
    if (!value.isObject()) return invalid();
    const auto entry = value.toObject();
    qint64 current = 0, extreme = 0, gap = 0;
    if (entry.size() != 5 || !entry.value(QStringLiteral("key")).isString() || !entry.value(QStringLiteral("label")).isString() ||
        !DomainNumeric::checkedInteger(entry.value(QStringLiteral("current")),&current,0,std::numeric_limits<int>::max()) ||
        !DomainNumeric::checkedInteger(entry.value(QStringLiteral("extreme")),&extreme,0,std::numeric_limits<int>::max()) ||
        !DomainNumeric::checkedInteger(entry.value(QStringLiteral("gap")),&gap,0,std::numeric_limits<int>::max())) return invalid();
    facts.battlePower.componentGaps.append({entry.value(QStringLiteral("key")).toString(),entry.value(QStringLiteral("label")).toString(),int(current),int(extreme),int(gap)});
  }
  for (const auto& item : power.value(QStringLiteral("components")).toArray()) {
    if (!item.isObject()) return invalid();
    const auto value = item.toObject(); PetBattlePowerComponent component;
    if (value.size() != qsizetype(std::size(componentIntegers)+std::size(componentBooleans)+3) ||
        !readScalars(value,&component,componentIntegers,componentBooleans) ||
        !value.value(QStringLiteral("key")).isString() || !value.value(QStringLiteral("label")).isString() ||
        !value.value(QStringLiteral("explanation")).isString()) return invalid();
    component.key = value.value(QStringLiteral("key")).toString(); component.label = value.value(QStringLiteral("label")).toString();
    component.explanation = value.value(QStringLiteral("explanation")).toString(); facts.battlePower.components.append(component);
  }
  for (const auto& item : power.value(QStringLiteral("stargodDetails")).toArray()) {
    if (!item.isObject()) return invalid();
    const auto value = item.toObject(); PetStargodSlotPower slot;
    if (value.size() != qsizetype(std::size(slotIntegers)+std::size(slotBooleans)+3) ||
        !readScalars(value,&slot,slotIntegers,slotBooleans) ||
        !value.value(QStringLiteral("equippedName")).isString() || !value.value(QStringLiteral("ownedBestName")).isString() ||
        !value.value(QStringLiteral("action")).isString()) return invalid();
    slot.equippedName = value.value(QStringLiteral("equippedName")).toString(); slot.ownedBestName = value.value(QStringLiteral("ownedBestName")).toString();
    slot.action = value.value(QStringLiteral("action")).toString(); facts.battlePower.stargodDetails.append(slot);
  }
  if (!validatePetAnalysisFacts(facts,error)) return std::nullopt;
  return facts;
}
