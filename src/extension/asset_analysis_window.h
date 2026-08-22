#pragma once

#include "asset_analysis_controller.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;

class AssetAnalysisWindow final : public QDialog {
  Q_OBJECT

public:
  explicit AssetAnalysisWindow(AssetAnalysisController* controller,
                               QWidget* parent = nullptr);

signals:
  void petRequested(qint64 instanceId);
  void shopRequested();
  void routineRequested();

private slots:
  void refreshAnalysis();
  void refreshInventory();
  void invalidateAnalysis();
  void recordSnapshotNow();
  void rebuildDiagnostics();
  void rebuildHistory();
  void activateOverviewRow(int row, int column);
  void activatePetRow(int row, int column);
  void showInstanceHistory();

private:
  void rebuildOverview();
  void addOverviewRow(const QString& label, const QString& value,
                      const QString& note, int action);
  void setDiagnosticFilter(PetAssetFilter filter);

  AssetAnalysisController* controller_ = nullptr;
  AccountInventorySummary inventory_;
  AccountAssetOverview overview_;
  QList<AccountAssetSnapshot> snapshots_;
  bool analysisReady_ = false;
  QTabWidget* tabs_ = nullptr;
  QLabel* accountSummary_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* refreshAnalysis_ = nullptr;
  QTableWidget* overviewTable_ = nullptr;
  QComboBox* filter_ = nullptr;
  QLineEdit* search_ = nullptr;
  QLabel* diagnosticSummary_ = nullptr;
  QTableWidget* diagnosticTable_ = nullptr;
  QCheckBox* autoSnapshot_ = nullptr;
  QPushButton* recordSnapshot_ = nullptr;
  QLabel* changeSummary_ = nullptr;
  QTableWidget* snapshotTable_ = nullptr;
  QLineEdit* instanceId_ = nullptr;
  QTableWidget* instanceHistoryTable_ = nullptr;
};
