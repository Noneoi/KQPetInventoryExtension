#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QList>

class QLabel;
class QLineEdit;
class QComboBox;
class QHBoxLayout;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QJsonValue;
class PetImageCache;
class PetRepository;

class PetWindow final : public QDialog {
  Q_OBJECT

public:
  explicit PetWindow(PetRepository* repository, QWidget* parent = nullptr);
  void setStatus(const QString& status);
  static QString renderCachedDetailHtml(const QJsonObject& pet,
                                        const PetRepository* repository,
                                        const QString& imagePath = {},
                                        bool fetchingLatest = false);

public slots:
  void setListRefreshRunning(bool running);
  void setDetailProgress(bool running, bool paused, int completed, int total,
                         int succeeded, int failed, qint64 currentInstanceId,
                         int estimatedSeconds);
  void setMoveRunning(bool running);
  void requestReplacement(qint64 incomingInstanceId,
                          const QList<qint64>& eligibleBackpackIds);

signals:
  void listRefreshRequested();
  void warehouseDetailRefreshRequested();
  void warehouseDetailPauseRequested();
  void warehouseDetailResumeRequested();
  void warehouseDetailCancelRequested();
  void detailRequested(qint64 instanceId);
  void settingsRequested();
  void moveToWarehouseRequested(qint64 instanceId);
  void moveToBackpackRequested(qint64 instanceId);
  void moveReplacementChosen(qint64 outgoingInstanceId);
  void moveCancelRequested();

private slots:
  void rebuild();
  void applyFilter();
  void selectBackpack(int row, int column);
  void selectWarehouse(int row, int column);
  void updateCurrentDetail(qint64 instanceId);
  void updateCurrentImage(const QString& visualKey, const QString& localPath);
  void setBackpackPage(int page);
  void moveCurrentToWarehouse();
  void moveCurrentToBackpack();

private:
  void fillTable(QTableWidget* table, const QList<QJsonObject>& pets,
                 const QString& location);
  void fillRow(QTableWidget* table, int row, const QJsonObject& pet,
               const QString& location);
  void updateWarehouseRow(qint64 instanceId);
  void refreshViews(bool rebuildFilterChoices = false);
  void rebuildFilterChoices();
  QList<QJsonObject> filteredAndSorted(const QList<QJsonObject>& pets,
                                       bool backpack) const;
  bool matchesCurrentQueryAndFilters(const QJsonObject& pet) const;
  void rebuildPageButtons(int pageCount);
  void updateSortDirectionState();
  void showDetail(const QJsonObject& pet);
  void addJsonValue(const QString& key, const QJsonValue& value, QTreeWidgetItem* parent);
  QString displayName(const QJsonObject& pet) const;
  static qint64 rowId(QTableWidget* table, int row);
  void updateMoveButtons();

  PetRepository* repository_ = nullptr;
  PetImageCache* imageCache_ = nullptr;
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
  QPushButton* moveToWarehouse_ = nullptr;
  QPushButton* moveToBackpack_ = nullptr;
  QLabel* progress_ = nullptr;
  QLabel* backpackTitle_ = nullptr;
  QHBoxLayout* backpackPages_ = nullptr;
  QTabWidget* warehouseTabs_ = nullptr;
  QTableWidget* backpackTable_ = nullptr;
  QTableWidget* warehouseTable_ = nullptr;
  QTableWidget* eliteWarehouseTable_ = nullptr;
  QTabWidget* detailTabs_ = nullptr;
  QTextBrowser* detailView_ = nullptr;
  QTreeWidget* rawTree_ = nullptr;
  QLabel* status_ = nullptr;
  qint64 currentId_ = 0;
  int currentRaceId_ = 0;
  QString currentVisualKey_;
  int backpackPage_ = 0;
  bool detailBatchPaused_ = false;
  bool detailBatchRunning_ = false;
  bool listRefreshRunning_ = false;
  bool moveRunning_ = false;
  QString currentLocation_;
};
