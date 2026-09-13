#pragma once
#include <QList>
#include <QString>
#include <QStringList>

struct PetBattlePowerGap {
  QString key;
  QString label;
  int current = 0;
  int extreme = 0;
  int gap = 0;
};

// Values whose corresponding known flag is false are unavailable, never zero.
struct PetBattlePowerComponent {
  QString key, label, explanation;
  int current = 0, extreme = 0, highest = 0, gap = 0;
  bool applicable = true, currentKnown = false, extremeKnown = false;
  bool highestKnown = false, gapKnown = false;
};

struct PetStargodSlotPower {
  int slot = 0, level = 0, equippedId = 0, ownedBestId = 0;
  int equippedPower = 0, ownedPower = 0, ownedMaxPower = 0, highestPower = 0;
  bool changeable = false, equippedKnown = false, ownedKnown = false;
  bool ownedMaxKnown = false, highestKnown = false, fromBackpack = false;
  QString equippedName, ownedBestName, action;
};

struct PetBattlePowerState {
  int serverCurrent = 0;
  int current = 0;
  int extreme = 0;
  int highest = 0;
  int stargodSlots = 0;
  int equippedStars = 0;
  int backpackStars = 0;
  int availableStars = 0;
  int missingStars = 0;
  int redStars = 0;
  int goldStars = 0;
  int missingRedStars = 0;
  int knownExtremeGap = 0;
  int equippedStargodPower = 0;
  int currentStargodPower = 0;
  int bestOrdinaryStargodPower = 0;
  int changeableStargodPower = 0;
  int targetStargodPower = 0;
  int highestStargodPower = 0;
  int changeableQuality = 0;
  int stargodMaxLevel = 0;
  int stargodLevelMissingSlots = 0;
  int astrolabeBonus = 0;
  int highestGap = 0;
  bool breakthrough = false;
  bool stargodSlotsKnown = false;
  bool stargodBackpackKnown = false;
  bool stargodFull = false;
  bool redStargodFull = false;
  bool hasChangeableSlot = false;
  bool changeableRed = false;
  bool stargodLevelsFull = false;
  bool stargodPowerKnown = false;
  bool currentLocallyCalculated = false;
  bool hasCurrent = false;
  bool hasExtreme = false;
  bool hasHighest = false;
  bool isHighest = false;
  QList<PetBattlePowerGap> componentGaps;
  // current is the best legal use of stars already owned by this pet, at its
  // actual slot levels. It is not an assertion that those stars are equipped.
  int equippedCurrent = 0, ownedMaxStargodPower = 0, adjustmentGain = 0;
  int stargodUpgradeGap = 0, stargodAcquisitionGap = 0;
  int astrolabeTargetBonus = 0, componentCurrentTotal = 0, componentExtremeTotal = 0;
  int astrolabeCurrent = 0, astrolabeMaximum = 0;
  int astrolabeUnlitNodes = 0, astrolabeSelectedNodes = 0;
  int changeableOwnedQuality = 0;
  bool hasServerCurrent = false, equippedCurrentKnown = false, highestGapKnown = false;
  bool stargodQualitiesKnown = false, stargodAcquisitionKnown = false;
  bool ownedMaxStargodPowerKnown = false, changeableOwnedKnown = false, changeableOwnedRed = false;
  bool breakthroughKnown = false, astrolabeApplicable = false, astrolabeApplicabilityKnown = false;
  bool breakthroughApplicable = false, breakthroughApplicabilityKnown = false;
  bool astrolabePowerKnown = false, componentTotalsKnown = false, completionKnown = false;
  QList<PetBattlePowerComponent> components;
  QList<PetStargodSlotPower> stargodDetails;
  QStringList unknownReasons;
};
