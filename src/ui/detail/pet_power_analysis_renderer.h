#pragma once

#include "contracts/pet_detail_types.h"
#include "contracts/cultivation_material_inventory.h"
#include <QDateTime>

namespace PetPowerAnalysisRenderer {
// Presentation only: all power values and requirements are prepared by Core.
QString render(const PreparedPetDetailHandle& detail, const QDateTime& observedAt,
               bool refreshing = false, const QString& error = {},
               const MaterialInventorySnapshot& materials = {});
QString waiting(const QString& name, const QString& error = {});
}
