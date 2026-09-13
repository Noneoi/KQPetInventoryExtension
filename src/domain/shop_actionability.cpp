#include "shop_actionability.h"

#include "compiled_shop_catalog.h"
#include "prepared_shop_conditions.h"


bool ShopActionability::excluded() const {
  return eligibilityCondition.effectiveState() == ShopConditionState::Blocked ||
         limitCondition.effectiveState() == ShopConditionState::Blocked ||
         unlockCondition.effectiveState() == ShopConditionState::Blocked;
}

int ShopActionability::unknownConditionCount() const {
  int count = 0;
  for (const ShopCondition* value :
       {&eligibilityCondition, &costCondition, &limitCondition, &unlockCondition})
    count += value->effectiveState() == ShopConditionState::Unknown;
  if (costCondition.effectiveState() == ShopConditionState::Satisfied) {
    for (const ResourceRequirement& requirement : requirements)
      count += requirement.condition.effectiveState() == ShopConditionState::Unknown;
  }
  return count;
}
