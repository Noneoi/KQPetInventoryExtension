#pragma once

#include "domain/cultivation_material_plan.h"
#include <QTextBrowser>

class CultivationMaterialPanel final : public QTextBrowser {
public:
  explicit CultivationMaterialPanel(QWidget* parent = nullptr);
  void setPlan(const CultivationMaterialPlan& plan, bool onlySelected = false);
  void resetPlan();

private:
  void showDocument(const QString& body, const QString& scope);
  QString renderedHtml_, scope_;
};
