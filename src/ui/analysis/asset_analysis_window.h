#pragma once

#include "application/views/analysis_read_view.h"
#include "contracts/local_stargod_statistics.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableView;
class QTabWidget;
class QModelIndex;
class AssetAnalysisModel;
class AssetAnalysisFilterProxyModel;
class SnapshotHistoryModel;
class RecommendationModel;

class AssetAnalysisWindow final : public QDialog {
  Q_OBJECT

public:
  explicit AssetAnalysisWindow(AnalysisReadView* controller,
                               QWidget* parent = nullptr);
  // Number of full overview-pet passes the summary rows performed. Used by the
  // preview self-test: a routine-only change must not scan the pet list again.
  quint64 overviewPetScanCount() const { return overviewPetScans_; }

public slots:
  void resetSessionContext();
  void setLocalStargodStatistics(const LocalStargodStatistics& result);

signals:
  void petRequested(qint64 instanceId);
  void shopRequested();
  void shopGoodRequested(const QString& stableKey);
  void routineRequested();
  void localStargodStatisticsRequested();
  void localStargodStatisticsCancelled();

private slots:
  void refreshAnalysis();
  void applyAnalysis();
  void refreshInventory();
  void markInventoryMembershipChanged();
  void markPetDetailChanged(qint64 instanceId);
  void markShopAnalysisInvalidated();
  void refreshRoutineSummary();
  void refreshAccountAnalysis();
  void recordSnapshotNow();
  void rebuildDiagnostics();
  void applyDiagnosticFilter();
  void rebuildHistory();
  void activateRecommendationIndex(const QModelIndex& index);
  void activateOverviewRow(int row, int column);
  void activatePetIndex(const QModelIndex& index);
  void selectPetIndex(const QModelIndex& index);
  void showInstanceHistory();
  void rebuildInstanceHistory();
  void refreshHistoryState();
  void loadRecentComparison();

private:
  void rebuildOverview();
  // Updates only the two routine cells; the pet-derived counts stay cached.
  void updateRoutineOverviewRows();
  void addOverviewRow(const QString& label, const QString& value,
                      const QString& note, int action);
  const QHash<int, int>& overviewFilterCounts();
  void setDiagnosticFilter(PetAssetFilter filter);
  void scheduleAnalysisStatusUpdate();
  void updateAnalysisStatus();
  void updateDiagnosticSummary();
  void rebuildRecommendations();

  AnalysisReadView* controller_ = nullptr;
  AccountInventorySummary inventory_;
  AccountAssetOverview overview_;
  AccountAssetOverview routineSummary_;
  QList<AccountAssetSnapshot> snapshots_;
  bool analysisReady_ = false;
  bool sessionResetPending_ = false;
  bool analysisStatusUpdatePending_ = false;
  // Cached classification counts for the current analysis result, computed in a
  // single pass. Invalidated explicitly whenever the overview result changes.
  QHash<int, int> overviewFilterCounts_;
  bool overviewFilterCountsValid_ = false;
  quint64 overviewPetScans_ = 0;
  QHash<QString, int> overviewRowIndex_;
  QTabWidget* tabs_ = nullptr;
  QTabWidget* recommendationTabs_ = nullptr;
  QCheckBox* showAllRecommendations_ = nullptr;
  QLabel* accountSummary_ = nullptr;
  QLabel* analysisStatus_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* refreshAnalysis_ = nullptr;
  QPushButton* cancelAnalysis_ = nullptr;
  QPushButton* localStargodStatistics_ = nullptr;
  QPushButton* cancelLocalStargodStatistics_ = nullptr;
  QLabel* localStargodSummary_ = nullptr;
  QTableWidget* overviewTable_ = nullptr;
  QComboBox* filter_ = nullptr;
  QLineEdit* search_ = nullptr;
  QLabel* diagnosticSummary_ = nullptr;
  QTableView* diagnosticTable_ = nullptr;
  AssetAnalysisModel* analysisModel_ = nullptr;
  AssetAnalysisFilterProxyModel* analysisFilterModel_ = nullptr;
  QTableView* readyRecommendations_ = nullptr;
  QTableView* missingRecommendations_ = nullptr;
  QTableView* nearFullRecommendations_ = nullptr;
  RecommendationModel* readyRecommendationModel_ = nullptr;
  RecommendationModel* missingRecommendationModel_ = nullptr;
  RecommendationModel* nearFullRecommendationModel_ = nullptr;
  QCheckBox* autoSnapshot_ = nullptr;
  QPushButton* recordSnapshot_ = nullptr;
  QPushButton* olderHistory_ = nullptr;
  QPushButton* compareHistory_ = nullptr;
  QLabel* historyState_ = nullptr;
  QLabel* instanceHistoryState_ = nullptr;
  QLabel* changeSummary_ = nullptr;
  QTableView* snapshotTable_ = nullptr;
  SnapshotHistoryModel* snapshotModel_ = nullptr;
  QLineEdit* instanceId_ = nullptr;
  QTableWidget* instanceHistoryTable_ = nullptr;
};
