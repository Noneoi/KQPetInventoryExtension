#include "shop_actionability.h"

#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "shop_pet_eligibility.h"

namespace {

bool raceAllowed(const ShopExchangeGood& good, const QJsonObject& pet) {
  const int raceId = petRaceId(pet);
  const int metadataRaceId = pet.value(QStringLiteral("_metaRaceId")).toInt();
  return good.raceIds.contains(raceId) ||
         (metadataRaceId > 0 && good.raceIds.contains(metadataRaceId));
}

QList<ResourceRequirement> parseRequirements(
    const QString& cost, const AccountResourceView& resources) {
  QList<ResourceRequirement> result;
  QString normalized = cost;
  normalized.replace(QLatin1Char('|'), QLatin1Char('#'));
  for (const QString& part :
       normalized.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
    const QStringList fields = part.split(QLatin1Char(':'));
    bool typeOk = false;
    bool idOk = false;
    bool countOk = false;
    const int type = fields.value(0).toInt(&typeOk);
    const int id = fields.value(1).toInt(&idOk);
    const qint64 required = fields.value(2).toLongLong(&countOk);
    if (fields.size() < 3 || !typeOk || !idOk || !countOk ||
        type <= 0 || id <= 0 || required <= 0)
      continue;
    ResourceRequirement requirement;
    requirement.resourceKey =
        QStringLiteral("%1:%2").arg(type).arg(id);
    requirement.resourceName =
        PetDetailCatalog::instance().materialName(type, id);
    requirement.required = required;
    requirement.ownedKnown =
        resources.hasReliableCount(requirement.resourceKey);
    if (requirement.ownedKnown)
      requirement.owned = resources.count(requirement.resourceKey);
    result.append(requirement);
  }
  return result;
}

}  // namespace

ShopActionability evaluateShopActionability(
    const ShopExchangeGood& good, const QJsonObject& pet,
    const QJsonObject& shopPacket, const AccountResourceView& resources) {
  ShopActionability result;
  result.remainingCount =
      ShopExchangeCatalog::remainingCount(shopPacket, good);
  if (!raceAllowed(good, pet)) {
    result.eligibilityReason = QStringLiteral("该真实项目不适用于此精灵种族");
    return result;
  }
  const ShopPetEligibility eligibility =
      analyzeShopPetEligibility(good, pet, true);
  result.eligibilityReason = eligibility.reason;
  result.applicableCodes = eligibility.usefulCodes;
  result.petEligible =
      eligibility.state == ShopPetEligibilityState::Usable;
  if (!result.petEligible) return result;

  result.requirements = parseRequirements(good.cost, resources);
  result.resourcesKnown = true;
  result.resourcesEnough = true;
  // A non-empty but unparseable cost is unknown. Never treat it as free.
  if (!good.cost.trimmed().isEmpty() && result.requirements.isEmpty()) {
    result.resourcesKnown = false;
    result.resourcesEnough = false;
    return result;
  }
  for (const ResourceRequirement& requirement : result.requirements) {
    if (!requirement.ownedKnown) {
      result.resourcesKnown = false;
      result.resourcesEnough = false;
    } else if (requirement.owned < requirement.required) {
      result.resourcesEnough = false;
    }
  }
  return result;
}
