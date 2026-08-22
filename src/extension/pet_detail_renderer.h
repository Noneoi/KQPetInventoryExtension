#pragma once

#include "pet_detail_view_model.h"

#include <QString>

struct PetDetailRenderOptions {
  bool visualMismatchRefreshPending = false;
};

class PetDetailRenderer final {
public:
  static QString render(const PetDetailViewModel& model,
                        const PetDetailRenderOptions& options = {});
  static QString text(const QString& value);
  static QString valueOrDash(const QString& value);
  static QString row(const QString& label, const QString& richValue);
  static QString section(const QString& title, const QString& rows);
  static QString identitySection(const QString& rows, const QString& imageHtml);
  static QString document(const QString& header, const QString& body);
};
