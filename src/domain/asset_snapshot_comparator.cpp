#include "asset_snapshot_comparator.h"

#include <QHash>
#include <QSet>

AssetSnapshotDelta AssetSnapshotComparator::compare(
    const AccountAssetSnapshot& current,
    const AccountAssetSnapshot& previous) {
  AssetSnapshotDelta result;
  if (current.account.isEmpty() || current.account != previous.account ||
      !current.petsComplete || !previous.petsComplete) return result;
  const auto validInstances = [](const QList<AssetSnapshotPet>& pets) {
    QSet<qint64> seen;
    for (const AssetSnapshotPet& pet : pets) {
      if (pet.instanceId <= 0 || seen.contains(pet.instanceId) ||
          (pet.currentPowerKnown && pet.currentPower < 0)) return false;
      seen.insert(pet.instanceId);
    }
    return true;
  };
  if (!validInstances(current.pets) || !validInstances(previous.pets)) return result;
  result.accountComparable = true;
  result.cultivationComparable = current.analysisVersion > 0 &&
      current.analysisVersion == previous.analysisVersion;
  result.powerChangeKnown = true;
  QHash<qint64, AssetSnapshotPet> oldPets;
  for (const AssetSnapshotPet& pet : previous.pets)
    oldPets.insert(pet.instanceId, pet);
  QSet<qint64> seen;
  for (const AssetSnapshotPet& pet : current.pets) {
    if (pet.instanceId <= 0 || seen.contains(pet.instanceId)) continue;
    seen.insert(pet.instanceId);
    if (!oldPets.contains(pet.instanceId)) {
      ++result.newPets;
      continue;
    }
    const AssetSnapshotPet old = oldPets.value(pet.instanceId);
    if (result.cultivationComparable && old.cultivationKnown && pet.cultivationKnown &&
        !old.fullyCultivated && pet.fullyCultivated)
      ++result.newlyFullyCultivated;
    if (result.cultivationComparable && old.redStarKnown && pet.redStarKnown &&
        !old.redStarComplete && pet.redStarComplete)
      ++result.newlyRedStarComplete;
    if (result.cultivationComparable && old.astrolabeKnown && pet.astrolabeKnown &&
        !old.astrolabeBreakthrough && pet.astrolabeBreakthrough)
      ++result.newlyAstrolabeBreakthrough;
    if (old.currentPowerKnown && pet.currentPowerKnown)
      result.totalPowerChange += qint64(pet.currentPower) - old.currentPower;
    else
      result.powerChangeKnown = false;
  }
  for (auto it = oldPets.cbegin(); it != oldPets.cend(); ++it)
    if (!seen.contains(it.key())) ++result.removedPets;
  return result;
}
