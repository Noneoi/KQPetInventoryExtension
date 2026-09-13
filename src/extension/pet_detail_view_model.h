#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "../domain/pet_power_types.h"

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
