#include "domain/asset_derivation.h"
#include "domain/pet_power_types.h"
#include "domain/pet_analysis_facts.h"
#include "domain/shop_pet_eligibility.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>

namespace {
bool require(bool value, const char* text) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", text);
  return value;
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  bool ok = true;
  const QJsonObject pet{{QStringLiteral("id"), 321}, {QStringLiteral("r"), 7001}};
  const auto redRule = compileShopPetRule(QStringLiteral("34"));
  const auto goldRule = compileShopPetRule(QStringLiteral("31"));
  PetBattlePowerState power;
  power.stargodSlotsKnown = power.stargodBackpackKnown = power.stargodQualitiesKnown = true;
  power.stargodAcquisitionKnown = true;
  power.stargodSlots = 3;
  power.redStars = 3;
  power.missingRedStars = 0;
  power.hasChangeableSlot = true;
  power.changeableOwnedKnown = true;
  power.changeableOwnedRed = true;
  power.changeableOwnedQuality = 6;
  power.adjustmentGain = 250;
  power.stargodPowerKnown = true;
  power.ownedMaxStargodPowerKnown = true;
  power.stargodLevelsFull = true;
  power.stargodMaxLevel = 8;
  auto derived = deriveShopPetWithPower(pet, true, {}, power);
  ok &= require(evaluateShopPetRule(redRule, derived).state == ShopPetEligibilityState::NotUsable,
      "owned unequipped red stars were recommended for exchange again");
  ok &= require(evaluateShopPetRule(goldRule, derived).state == ShopPetEligibilityState::NotUsable,
      "owned superior stars were treated as needing another gold-star package");
  power.changeableOwnedQuality = 5;
  power.changeableOwnedRed = false;
  derived = deriveShopPetWithPower(pet, true, {}, power);
  ok &= require(evaluateShopPetRule(goldRule, derived).state == ShopPetEligibilityState::NotUsable &&
      evaluateShopPetRule(redRule, derived).state == ShopPetEligibilityState::NotUsable,
      "gold changeable star was confused with ordinary red-star acquisition");
  power.changeableOwnedQuality = 4;
  derived = deriveShopPetWithPower(pet, true, {}, power);
  ok &= require(evaluateShopPetRule(goldRule, derived).state == ShopPetEligibilityState::Usable,
      "real changeable gold-star shortage was hidden");
  power.stargodAcquisitionKnown = false;
  power.stargodBackpackKnown = false;
  derived = deriveShopPetWithPower(pet, true, {}, power);
  ok &= require(evaluateShopPetRule(redRule, derived).state == ShopPetEligibilityState::Unknown &&
      evaluateShopPetRule(goldRule, derived).state == ShopPetEligibilityState::Unknown,
      "missing backpack information was treated as empty or full");

  PetAssetRecord seed;
  power.stargodLevelsFull = false;
  power.stargodUpgradeGap = 400;
  seed.instanceId = 321; seed.raceId = 7001; seed.detailAvailable = true;
  seed.pet = pet;
  seed.observationVerified = false;
  power.stargodAcquisitionKnown = power.stargodBackpackKnown = true;
  power.hasCurrent = power.hasExtreme = power.hasHighest = true;
  power.currentLocallyCalculated = true;
  power.current = power.extreme = 1000;
  power.highest = 1400;
  power.highestGap = 400;
  power.highestGapKnown = power.completionKnown = true;
  auto asset = AssetDerivation::derivePetWithPower(seed, power);
  ok &= require(!asset.fullyCultivated && asset.cultivationKnown && asset.highestPowerGap == 400,
      "official extreme incorrectly meant fully cultivated or readonly data lost analysis");
  ok &= require(!asset.redStarMissing &&
      AssetDerivation::matchesFilter(asset, PetAssetFilter::StargodEquipNeeded) &&
      AssetDerivation::matchesFilter(asset, PetAssetFilter::StargodUpgradeNeeded) &&
      AssetDerivation::matchesFilter(asset, PetAssetFilter::ChangeableMissing),
      "owned-star actions and changeable acquisition were not separated");
  power.stargodAcquisitionKnown = false;
  power.missingRedStars = 3;
  power.completionKnown = power.highestGapKnown = false;
  asset = AssetDerivation::derivePetWithPower(seed, power);
  ok &= require(!asset.redStarMissing && !asset.completionKnown &&
      AssetDerivation::matchesFilter(asset, PetAssetFilter::AnalysisIncomplete),
      "unknown acquisition/completion was counted as a proven shortage");
  power.components.append({QStringLiteral("iv"), QStringLiteral("天赋"), {}, 10, 20, 20, 10,
      true, true, true, true, true});
  derived = deriveShopPetWithPower(pet, true, {}, power);
  ok &= require(evaluateShopPetRule(compileShopPetRule(QStringLiteral("62")), derived).state ==
      ShopPetEligibilityState::Usable, "non-star components did not reuse the shared power result");
  PetStargodSlotPower slot;
  slot.slot = 1; slot.level = 8; slot.equippedId = 29; slot.ownedBestId = 66;
  slot.equippedPower = 500; slot.ownedPower = slot.ownedMaxPower = slot.highestPower = 650;
  slot.equippedKnown = slot.ownedKnown = slot.ownedMaxKnown = slot.highestKnown = slot.fromBackpack = true;
  slot.equippedName = QStringLiteral("原金星"); slot.ownedBestName = QStringLiteral("已有红星");
  slot.action = QStringLiteral("已有星神可调整装备");
  power.stargodDetails.append(slot);
  power.unknownReasons.append(QStringLiteral("另一培养分项待补"));
  PetAnalysisFacts facts;
  facts.battlePower = power;
  facts.asset = AssetDerivation::derivePetWithPower(seed, power);
  facts.asset.pet = AssetDerivation::identityFields(pet);
  facts.eligibility = deriveShopPetWithPower(pet, true, {}, power);
  QString factError;
  ok &= require(validatePetAnalysisFacts(facts, &factError), "new consumer fact fixture is invalid");
  const auto encoded = petAnalysisFactsToJson(facts);
  const auto decoded = petAnalysisFactsFromJson(encoded, &factError);
  ok &= require(decoded && petAnalysisFactsToJson(*decoded) == encoded &&
      decoded->battlePower.stargodDetails.first().fromBackpack &&
      decoded->battlePower.stargodDetails.first().action == slot.action &&
      decoded->battlePower.stargodUpgradeGap == power.stargodUpgradeGap &&
      decoded->battlePower.components.first().gapKnown &&
      decoded->battlePower.unknownReasons == power.unknownReasons,
      "new detailed power fields did not survive facts cache round trip");
  auto invalidFacts = facts;
  invalidFacts.battlePower.components[0].currentKnown = false;
  ok &= require(!validatePetAnalysisFacts(invalidFacts) && petAnalysisFactsToJson(invalidFacts).isEmpty(),
      "unknown component current value retained a known gap");
  auto invalidEncoded = encoded;
  auto invalidPower = invalidEncoded.value(QStringLiteral("battlePower")).toObject();
  invalidPower.insert(QStringLiteral("stargodUpgradeGap"), -1);
  invalidEncoded.insert(QStringLiteral("battlePower"), invalidPower);
  ok &= require(!petAnalysisFactsFromJson(invalidEncoded), "negative power action gap was accepted by cache decoder");
  const auto fields = AssetDerivation::analysisInputFields(pet,
      {{QStringLiteral("astrolabe"), QStringLiteral("1#2#3")}});
  ok &= require(fields.contains(QStringLiteral("astrolabe")), "astrolabe nodes were dropped before analysis");
  const auto latest = AssetDerivation::analysisInputFields(
      {{QStringLiteral("zdl"), 10}, {QStringLiteral("_position"), 2}},
      {{QStringLiteral("zdl"), 20}, {QStringLiteral("_position"), 1}});
  ok &= require(latest.value(QStringLiteral("zdl")).toInt() == 20 &&
      latest.value(QStringLiteral("_position")).toInt() == 2,
      "list summary overwrote the latest full detail or detail overwrote current position");
  const auto missingLatest = AssetDerivation::analysisInputFields(
      {{QStringLiteral("sgsp"), QJsonArray{1, 2}}}, {{QStringLiteral("id"), 321}});
  ok &= require(!missingLatest.contains(QStringLiteral("sgsp")),
      "missing detail field borrowed stale summary stars as current ownership");
  return ok ? 0 : 1;
}
