#pragma once

#include <QDialog>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <memory>
#include "../contracts/cultivation_material_inventory.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QHBoxLayout;
class QPushButton;
class QModelIndex;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTimer;
class QResizeEvent;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QJsonValue;
class QGridLayout;
class QSplitter;
class PetImageCache;
class PetImageBrowser;
class PetRawDataTree;
class PetFilterProxyModel;
class InventoryReadView;
class PetTableModel;
struct PreparedPetDetail;

class PetWindow final : public QDialog {
  Q_OBJECT

public:
  explicit PetWindow(InventoryReadView* repository, QWidget* parent = nullptr,
                     PetImageCache* sharedImages = nullptr);
  void setStatus(const QString& status);
  void setCultivationMaterials(const MaterialInventorySnapshot& materials);

public slots:
  void setWorkbenchMode(bool embedded, bool compact);
  void resetSessionContext();
  void focusPet(qint64 instanceId);
  void setListRefreshRunning(bool running);
  void setDetailProgress(bool running, bool paused, int completed, int total,
                         int succeeded, int failed, qint64 currentInstanceId,
                         int estimatedSeconds);
  void setMoveRunning(bool running);

signals:
  void sidebarStatusChanged(const QString& text);
  void sidebarDetailProgressChanged(const QString& text, bool running, int completed, int total);
  void listRefreshRequested();
  void warehouseDetailRefreshRequested(const QList<qint64>& instanceIds);
  void warehouseDetailPauseRequested();
  void warehouseDetailResumeRequested();
  void warehouseDetailCancelRequested();
  void detailRequested(qint64 instanceId);
  void cultivationMaterialsRefreshRequested();
  void settingsRequested();
  void copyDiagnosticsRequested();
  void moveToWarehouseRequested(qint64 instanceId);
  void moveToBackpackRequested(qint64 instanceId);

private slots:
  void rebuild();
  void applyFilter();
  void selectBackpack(int row, int column);
  void selectWarehouse(const QModelIndex& index);
  void updateCurrentDetail(qint64 instanceId);
  void updateCurrentImage(const QString& visualKey, const QString& localPath);
  void setBackpackPage(int page);
  void moveCurrentToWarehouse();
  void moveCurrentToBackpack();

protected:
  bool event(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void fillTable(QTableWidget* table, const QList<QJsonObject>& pets,
                 const QString& location);
  void fillRow(QTableWidget* table, int row, const QJsonObject& pet,
               const QString& location);
  void updateWarehouseRow(qint64 instanceId);
  void refreshViews(bool rebuildFilterChoices = false);
  void rebuildFilterChoices();
  void rebuildPageButtons(int pageCount);
  void updateSortDirectionState();
  void showDetail(const QJsonObject& pet);
  void showRelatedPetDetail(qint64 instanceId);
  void refreshRelatedPetDetail();
  void closeRelatedPetDetail();
  void showAnalysis(const std::shared_ptr<const PreparedPetDetail>& detail,
                    qint64 instanceId, const QString& name);
  static qint64 rowId(QTableWidget* table, int row);
  void updateMoveButtons();
  void updateWorkbenchPanels();
  void requestSelectedWarehouseDetails();
  void fitInventoryGeometry();

  InventoryReadView* repository_ = nullptr;
  QPointer<PetImageCache> imageCache_;
  QGridLayout* toolbarLayout_ = nullptr;
  QGridLayout* filterLayout_ = nullptr;
  QWidget* filterPanel_ = nullptr;
  QPushButton* filterToggle_ = nullptr;
  QWidget* detailEraFilters_ = nullptr;
  QList<QCheckBox*> detailEraChecks_;
  QList<QWidget*> toolbarWidgets_;
  QList<QWidget*> filterWidgets_;
  QSplitter* contentSplitter_ = nullptr;
  QSplitter* inventorySplitter_ = nullptr;
  QPushButton* detailToggle_ = nullptr;
  bool workbenchEmbedded_ = false;
  bool workbenchCompact_ = false;
  bool compactDetails_ = false;
  QList<int> workbenchWideSizes_;
  QLineEdit* search_ = nullptr;
  QTimer* searchDebounce_ = nullptr;
  QComboBox* attributeFilter_ = nullptr;
  QComboBox* jobFilter_ = nullptr;
  QComboBox* eraFilter_ = nullptr;
  QComboBox* backpackSort_ = nullptr;
  QComboBox* backpackSortDirection_ = nullptr;
  QComboBox* warehouseSort_ = nullptr;
  QComboBox* warehouseSortDirection_ = nullptr;
  QPushButton* refresh_ = nullptr;
  QPushButton* refreshDetails_ = nullptr;
  QPushButton* pauseDetails_ = nullptr;
  QPushButton* cancelDetails_ = nullptr;
  QPushButton* settings_ = nullptr;
  QPushButton* diagnostics_ = nullptr;
  QPushButton* moveToWarehouse_ = nullptr;
  QPushButton* moveToBackpack_ = nullptr;
  QLabel* progress_ = nullptr;
  QLabel* backpackTitle_ = nullptr;
  QHBoxLayout* backpackPages_ = nullptr;
  QTabWidget* warehouseTabs_ = nullptr;
  QTableWidget* backpackTable_ = nullptr;
  QTableView* warehouseTable_ = nullptr;
  QTableView* eliteWarehouseTable_ = nullptr;
  PetTableModel* warehouseModel_ = nullptr;
  PetTableModel* backpackModel_ = nullptr;
  PetTableModel* eliteWarehouseModel_ = nullptr;
  PetFilterProxyModel* warehouseProxy_ = nullptr;
  PetFilterProxyModel* backpackProxy_ = nullptr;
  PetFilterProxyModel* eliteWarehouseProxy_ = nullptr;
  QTabWidget* detailTabs_ = nullptr;
  PetImageBrowser* detailView_ = nullptr;
  PetRawDataTree* rawTree_ = nullptr;
  QWidget* analysisPage_ = nullptr;
  QTextBrowser* analysisView_ = nullptr;
  QPushButton* refreshMaterials_ = nullptr;
  QLabel* materialInventoryStatus_ = nullptr;
  QLabel* status_ = nullptr;
  // Related-pet popup. It holds its own detail subscription so opening it never
  // replaces the selected pet's detail in the main panel.
  QPointer<QDialog> relatedDialog_;
  PetImageBrowser* relatedView_ = nullptr;
  QLabel* relatedTitle_ = nullptr;
  qint64 relatedId_ = 0;
  std::shared_ptr<const PreparedPetDetail> renderedRelatedDetail_;
  qint64 currentId_ = 0;
  qint64 pendingFocusId_ = 0;
  qint64 renderedDetailId_ = 0;
  std::shared_ptr<const PreparedPetDetail> renderedPreparedDetail_;
  QString renderedDetailImagePath_;
  bool renderedDetailNarrow_ = false;
  std::shared_ptr<const PreparedPetDetail> renderedAnalysisDetail_;
  QDateTime analysisObservedAt_;
  MaterialInventorySnapshot cultivationMaterials_;
  quint64 analysisRenderGeneration_ = 0;
  qint64 pendingDetailScrollId_ = 0;
  int pendingDetailScroll_ = 0;
  quint64 detailRenderGeneration_ = 0;
  int currentRaceId_ = 0;
  QString currentVisualKey_;
  int backpackPage_ = 0;
  int backpackPageCount_ = -1;
  bool detailBatchPaused_ = false;
  bool detailBatchRunning_ = false;
  int detailCompleted_ = 0;
  int detailTotal_ = 0;
  bool listRefreshRunning_ = false;
  bool moveRunning_ = false;
  bool backpackDisplayRefreshScheduled_ = false;
  QString currentLocation_;
};
