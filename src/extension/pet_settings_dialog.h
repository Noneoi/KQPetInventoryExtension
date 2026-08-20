#pragma once

#include "pet_refresh_controller.h"

#include <QDialog>

class QSpinBox;

class PetSettingsDialog final : public QDialog {
  Q_OBJECT

public:
  explicit PetSettingsDialog(const PetRefreshController::Timings& timings,
                             QWidget* parent = nullptr);
  PetRefreshController::Timings timings() const;

private:
  void applyTimings(const PetRefreshController::Timings& timings);

  QSpinBox* automaticIntervalSeconds_ = nullptr;
  QSpinBox* listRequestGapMs_ = nullptr;
  QSpinBox* listTimeoutSeconds_ = nullptr;
  QSpinBox* detailRequestGapMs_ = nullptr;
  QSpinBox* detailBatchSize_ = nullptr;
  QSpinBox* detailBatchRestSeconds_ = nullptr;
  QSpinBox* detailTimeoutSeconds_ = nullptr;
  QSpinBox* detailMaxRetries_ = nullptr;
};
