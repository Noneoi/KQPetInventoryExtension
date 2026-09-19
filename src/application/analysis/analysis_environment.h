#pragma once
#include "domain/catalog_types.h"
#include <QDate>
#include <functional>
#include <memory>

// Core-owned reference-data providers. Each returns an immutable snapshot;
// callers may freeze these for replay without bypassing Repository or Worker.
// Omitted providers use the application's current catalog and business date.
struct AnalysisEnvironment {
  std::function<QDate()> businessDate;
  std::function<std::shared_ptr<const PetDetailCatalogSnapshot>()> petMetadataSnapshot;
  std::function<std::shared_ptr<const ShopCatalogSnapshot>()> shopCatalogSnapshot;
};
