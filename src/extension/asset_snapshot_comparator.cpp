#include "asset_snapshot_comparator.h"

#include <QHash>

AssetSnapshotDelta AssetSnapshotComparator::compare(
    const AccountAssetSnapshot& current,
    const AccountAssetSnapshot& previous) {
  AssetSnapshotDelta result;
  QHash<qint64, AssetSnapshotPet> oldPets;
  for (const AssetSnapshotPet& pet : previous.pets)
    oldPets.insert(pet.instanceId, pet);
  for (const AssetSnapshotPet& pet : current.pets) {
    if (!oldPets.contains(pet.instanceId)) {
      ++result.newPets;
      continue;
    }
    const AssetSnapshotPet old = oldPets.value(pet.instanceId);
    if (!old.fullyCultivated && pet.fullyCultivated)
      ++result.newlyFullyCultivated;
    if (!old.redStarComplete && pet.redStarComplete)
      ++result.newlyRedStarComplete;
    if (!old.astrolabeBreakthrough && pet.astrolabeBreakthrough)
      ++result.newlyAstrolabeBreakthrough;
  }
  result.totalPowerChange = current.totalCurrentPower - previous.totalCurrentPower;
  return result;
}
