#pragma once

#include "domain/catalog_types.h"
#include <QDialog>
#include <memory>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;
class QPushButton;

class ShopSelectionDialog final : public QDialog {
  Q_OBJECT
public:
  explicit ShopSelectionDialog(QWidget* parent = nullptr,
      std::shared_ptr<const PetDetailCatalogSnapshot> metadata = {});
  void setCatalog(std::shared_ptr<const ShopCatalogSnapshot> catalog, bool editable, const QString& message);
  void setUpdateBusy(bool busy);
signals:
  void selectionRequested();
  void selectionSaveRequested(const QStringList& selected, const QStringList& excludedAutomatic);
  void scanRequested();
private:
  void rebuild();
  void filter();
  void updateSummary();
  void updateParent(QTreeWidgetItem* parent);
  void setChecks(Qt::CheckState state);
  void changeCheck(QTreeWidgetItem* child, Qt::CheckState state);
  void updateDirty();
  QTreeWidget* tree_ = nullptr;
  QLineEdit* search_ = nullptr;
  QLabel* summary_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* save_ = nullptr;
  QPushButton* scan_ = nullptr;
  QPushButton* selectAll_ = nullptr;
  QPushButton* clear_ = nullptr;
  QPushButton* diagnostics_ = nullptr;
  std::shared_ptr<const ShopCatalogSnapshot> catalog_;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadata_;
  QSet<QString> draft_, submitted_;
  QSet<QString> excludedDraft_, excludedSubmitted_;
  QDateTime nextAvailabilityChange_;
  bool editable_ = false, updating_ = false, dirty_ = false, saving_ = false;
};
