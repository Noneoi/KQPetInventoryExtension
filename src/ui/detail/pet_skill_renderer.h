#pragma once

#include "domain/catalog_types.h"

#include <QString>
#include <memory>

class PetSkillRenderer final {
public:
  static QString render(const std::shared_ptr<const PetSkillCatalogSnapshot>& catalog,
                        int raceId, const QString& fallbackName = {});
};
