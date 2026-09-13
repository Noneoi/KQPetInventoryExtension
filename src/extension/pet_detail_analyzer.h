#pragma once

#include "pet_detail_view_model.h"

#include <QJsonObject>

class InventoryReadView;

class PetDetailAnalyzer final {
public:
  static PetDetailViewModel analyze(const QJsonObject& pet,
                                    const InventoryReadView* repository,
                                    const QString& imagePath = {},
                                    bool fetchingLatest = false);
  static PetBattlePowerState analyzeBattlePower(const QJsonObject& pet);
};
