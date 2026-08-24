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
};

struct PetTalentState {
  int level = 0;
  QString levelName;
  QStringList normalLines;
  QStringList doubleEnergyLines;
  QString currentPower;
  QString fullPower;
};

struct PetBadgeSlot {
  QString jobName;
  int level = 0;
  QString exclusiveName;
  bool exclusiveAwakened = false;
};

struct PetSacredState {
  bool equipped = false;
  QString name;
  int star = 0;
  int maxStar = 0;
  int stage = 0;
  int maxStage = 0;
  bool fullStar = false;
  bool fullStage = false;
};

struct PetAstrolabeStar {
  QString name;
  QStringList lightUpCosts;
  bool exclusive = false;
  bool activated = false;
  bool selected = false;
};

struct PetAstrolabeState {
  QList<PetAstrolabeStar> stars;
  int activatedCount = 0;
  int selectedCount = 0;
  bool breakthrough = false;
};

struct PetStargodEntry {
  int defineId = 0;
  int level = 0;
  QString name;
  QString sourceName;
  QString category;
  int quality = 0;
  bool changeable = false;
};

struct PetStargodBackpackEntry {
  int defineId = 0;
  QString name;
  int quality = 0;
  bool changeable = false;
};

struct PetRelatedDisplay {
  QString name;
  QStringList facts;
};

struct PetRelationRow {
  QString label;
  QList<PetRelatedDisplay> pets;
};

struct PetRelationshipState {
  QList<PetRelationRow> summonRows;
  QList<PetRelationRow> carryRows;
};

struct PetDetailViewModel {
  bool available = false;
  bool visualMismatch = false;
  QString imagePath;
  bool fetchingLatest = false;
  int raceId = 0;
  int level = 0;
  qint64 instanceId = 0;
  QString name;
  QString originalName;
  QString customName;
  QString attributes;
  QString jobs;
  QString era;
  PetBattlePowerState battlePower;
  PetTalentState talent;
  QList<PetBadgeSlot> badges;
  PetSacredState sacred;
  PetAstrolabeState astrolabe;
  QList<PetStargodEntry> stargods;
  QList<PetStargodBackpackEntry> stargodBackpack;
  PetRelationshipState relationships;
};
