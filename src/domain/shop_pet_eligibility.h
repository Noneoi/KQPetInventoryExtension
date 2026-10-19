#pragma once

#include "catalog_types.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <array>

struct AlgorithmPipelineStats;
struct PetBattlePowerState;

enum class ShopCultivationState { Unknown, Useful, Full };
inline constexpr std::size_t kShopCultivationComponentCount = 22;

struct ShopPetMetadataSnapshot {
  QJsonObject stargods;
  QJsonObject astrolabe;
  QJsonObject pets;
  QJsonObject sacredStarPlans;
  QJsonObject sacredStagePlans;
  QJsonObject badges;
  QJsonObject items;
  QJsonObject sacredEquipment;
};

struct ShopPetComponentRule {
  QString code;
  QString name;
  int componentIndex = -1;
  int targetLevel = 0;
  int targetId = 0;
};

struct CompiledShopPetRule {
  QList<ShopPetComponentRule> components;
  bool suppliesOrdinaryAstrolabe = false;
};

struct ShopPetDerived {
  int raceId = 0;
  int metadataRaceId = 0;
  bool hasFullDetail = false;
  bool hasData = false;
  bool sacredStateRequired = false;
  bool badgeStateRequired = false;
  bool astrolabeStateRequired = false;
  bool changeableSlotKnown = false;
  bool hasChangeableSlot = false;
  bool changeableLevelKnown = false;
  int changeableLevel = 0;
  int astrolabeBlockedMask = 0;
  bool equipmentLevelsKnown = false;
  int minimumEquipmentLevel = 0;
  bool legendStoneLevelsKnown = false;
  int minimumLegendStoneLevel = 0;
  QList<int> usefulSpecifiedStars;
  QList<int> satisfiedSpecifiedStars;
  QList<int> unavailableSpecifiedStars;
  std::array<ShopCultivationState, kShopCultivationComponentCount> components{};
};

enum class ShopPetEligibilityState {
  Usable,
  NotUsable,
  Unknown
};

struct ShopPetEligibilitySummary {
  ShopPetEligibilityState state = ShopPetEligibilityState::Unknown;
  int usefulCount = 0;
  bool redStarUseful = false;
};

struct ShopPetEligibility {
  ShopPetEligibilityState state = ShopPetEligibilityState::Unknown;
  QString reason;
  QStringList usefulCodes;
};

CompiledShopPetRule compileShopPetRule(const QString& enhanceType,
                                      AlgorithmPipelineStats* stats = nullptr);
ShopPetDerived deriveShopPet(const QJsonObject& pet, bool hasFullDetail,
                             const ShopPetMetadataSnapshot& metadata,
                             AlgorithmPipelineStats* stats = nullptr);
ShopPetDerived deriveShopPetWithPower(const QJsonObject& pet, bool hasFullDetail,
                                      const ShopPetMetadataSnapshot& metadata,
                                      const PetBattlePowerState& power,
                                      AlgorithmPipelineStats* stats = nullptr);
ShopPetEligibilitySummary evaluateShopPetRule(
    const CompiledShopPetRule& rule, const ShopPetDerived& pet,
    AlgorithmPipelineStats* stats = nullptr);
ShopPetEligibility describeShopPetRule(const CompiledShopPetRule& rule,
                                       const ShopPetDerived& pet);
