#include "asset_snapshot_comparator.h"
#include "asset_analysis_window.h"

#include "asset_analysis_filter_proxy_model.h"
#include "asset_analysis_model.h"
#include "build_info.h"
#include "recommendation_model.h"
#include "snapshot_history_model.h"
#include "../domain/asset_derivation.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>


namespace {

constexpr int kOpenShop = -2;
constexpr int kOpenRoutine = -3;

// A confirmed subtotal from part of the sources is reported as such; only a
// complete source set is presented as the period total.
QString opportunitySummaryText(const RoutineOpportunitySummary& value) {
  switch (value.completeness) {
    case RoutineCompleteness::Complete:
      return QStringLiteral("玩法剩余 %1 次").arg(value.total);
    case RoutineCompleteness::Partial:
      return QStringLiteral("已确认 %1 次，另有 %2 项未更新")
          .arg(value.total)
          .arg(value.pendingSources.size());
    case RoutineCompleteness::Overflow:
      return QStringLiteral("玩法次数合计无效（超出可表示范围）");
    case RoutineCompleteness::Unknown:
      return value.expectedSources == 0 ? QStringLiteral("当前无适用玩法")
                                        : QStringLiteral("玩法次数未确认");
  }
  return QStringLiteral("玩法次数未确认");
}

QString opportunitySummaryNote(const RoutineOpportunitySummary& value) {
  QString note = QStringLiteral("点击打开日常活动窗口查看逐项来源");
  if (!value.pendingSources.isEmpty())
    note += QStringLiteral("；未更新：%1").arg(value.pendingSources.join(QStringLiteral("、")));
  return note;
}

QTableWidget* makeTable(QWidget* parent, const QStringList& headers) {
  auto* table = new QTableWidget(parent);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->setShowGrid(false);
  table->verticalHeader()->setVisible(false);
  table->setWordWrap(false);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  return table;
}

QTableView* makeView(QWidget* parent) {
  auto* table = new QTableView(parent);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->setShowGrid(false);
  table->verticalHeader()->setVisible(false);
  table->setWordWrap(false);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table->horizontalHeader()->setDefaultSectionSize(150);
  table->horizontalHeader()->setStretchLastSection(true);
  return table;
}

QTableView* makeRecommendationView(QWidget* parent,
                                   RecommendationModel** model) {
  auto* view = makeView(parent);
  *model = new RecommendationModel(view);
  view->setModel(*model);
  view->setWordWrap(true);
  view->setTextElideMode(Qt::ElideNone);
  view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  view->horizontalHeader()->setStretchLastSection(false);
  for (int column : {RecommendationModel::Pet, RecommendationModel::Gap,
                     RecommendationModel::Project,
                     RecommendationModel::Resources,
                     RecommendationModel::Conclusion})
    view->horizontalHeader()->setSectionResizeMode(column,
                                                    QHeaderView::Stretch);
  for (int column : {RecommendationModel::Status,
                     RecommendationModel::Remaining,
                     RecommendationModel::ViewPet,
                     RecommendationModel::ViewShop})
    view->horizontalHeader()->setSectionResizeMode(column,
                                                    QHeaderView::Fixed);
  view->setColumnWidth(RecommendationModel::Status, 112);
  view->setColumnWidth(RecommendationModel::Remaining, 88);
  view->setColumnWidth(RecommendationModel::ViewPet, 76);
  view->setColumnWidth(RecommendationModel::ViewShop, 76);
  return view;
}

QTableWidgetItem* item(const QString& text, const QColor& color = {}) {
  auto* result = new QTableWidgetItem(text);
  result->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  if (color.isValid()) result->setForeground(QBrush(color));
  return result;
}

QString signedNumber(qint64 value) {
  return value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
}

QString yesNo(bool value) {
  return value ? QStringLiteral("已完成") : QStringLiteral("未完成");
}

}  // namespace

AssetAnalysisWindow::AssetAnalysisWindow(AnalysisReadView* controller,
                                         QWidget* parent)
    : QDialog(parent), controller_(controller) {
  setObjectName(QStringLiteral("KQAssetAnalysisWindow"));
  setWindowTitle(QStringLiteral("原版氪奇 · 账号资产与养成分析 · %1")
                     .arg(BuildInfo::displayVersion()));
  resize(1380, 850);
  setMinimumSize(1050, 680);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* root = new QVBoxLayout(this);
  auto* summaryRow = new QHBoxLayout();
  accountSummary_ = new QLabel(this);
  accountSummary_->setWordWrap(true);
  accountSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  refreshAnalysis_ = new QPushButton(
      QStringLiteral("重新计算养成分析（仅本地）"), this);
  refreshAnalysis_->setObjectName(QStringLiteral("KQAssetAnalysisRefresh"));
  refreshAnalysis_->setToolTip(
      QStringLiteral("读取当前账号本地缓存，重新计算培养完成度、缺口和商店适用性，不发送服务器请求。"));
  summaryRow->addWidget(accountSummary_, 1);
  cancelAnalysis_ = new QPushButton(QStringLiteral("取消计算"), this);
  cancelAnalysis_->setObjectName(QStringLiteral("KQAssetAnalysisCancel"));
  cancelAnalysis_->setEnabled(false);
  cancelAnalysis_->hide();
  summaryRow->addWidget(cancelAnalysis_);
  localStargodStatistics_ = new QPushButton(QStringLiteral("统计本地红星"),this);
  localStargodStatistics_->setObjectName(QStringLiteral("KQLocalStargodStatistics"));
  localStargodStatistics_->setToolTip(QStringLiteral("只读取当前账号磁盘中保存的完整精灵详情，统计实际持有的普通红星和万变红星，不联网。"));
  cancelLocalStargodStatistics_ = new QPushButton(QStringLiteral("取消红星统计"),this);
  cancelLocalStargodStatistics_->setObjectName(QStringLiteral("KQLocalStargodStatisticsCancel"));
  cancelLocalStargodStatistics_->hide();
  summaryRow->addWidget(cancelLocalStargodStatistics_);
  summaryRow->addWidget(localStargodStatistics_);
  summaryRow->addWidget(refreshAnalysis_);
  root->addLayout(summaryRow);
  analysisStatus_ = new QLabel(this);
  analysisStatus_->setObjectName(QStringLiteral("KQAssetAnalysisStatus"));
  analysisStatus_->setWordWrap(true);
  analysisStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(analysisStatus_);

  tabs_ = new QTabWidget(this);
  auto* overviewPage = new QWidget(tabs_);
  auto* overviewLayout = new QVBoxLayout(overviewPage);
  auto* overviewNote = new QLabel(
      QStringLiteral("精灵数量、位置、缺少详情数随列表同步轻量更新；培养和商店指标仅在点击“重新计算养成分析（仅本地）”后计算，日常/周常概要随对应缓存更新。"),
      overviewPage);
  overviewNote->setWordWrap(true);
  overviewTable_ = makeTable(
      overviewPage, {QStringLiteral("资产指标"), QStringLiteral("数量 / 状态"),
                     QStringLiteral("说明与入口")});
  overviewTable_->setObjectName(QStringLiteral("KQAssetOverviewTable"));
  overviewLayout->addWidget(overviewNote);
  localStargodSummary_ = new QLabel(QStringLiteral("本地红星：尚未统计"),overviewPage);
  localStargodSummary_->setObjectName(QStringLiteral("KQLocalStargodStatisticsSummary"));
  localStargodSummary_->setTextFormat(Qt::PlainText);
  localStargodSummary_->setWordWrap(true);
  localStargodSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  overviewLayout->addWidget(localStargodSummary_);
  overviewLayout->addWidget(overviewTable_, 1);
  tabs_->addTab(overviewPage, QStringLiteral("账号资产总览"));

  auto* diagnosticPage = new QWidget(tabs_);
  auto* diagnosticLayout = new QVBoxLayout(diagnosticPage);
  auto* filters = new QHBoxLayout();
  filter_ = new QComboBox(diagnosticPage);
  const QList<QPair<QString, PetAssetFilter>> choices = {
      {QStringLiteral("全部精灵"), PetAssetFilter::All},
      {QStringLiteral("背包"), PetAssetFilter::Backpack},
      {QStringLiteral("普通仓库"), PetAssetFilter::NormalWarehouse},
      {QStringLiteral("精英仓库"), PetAssetFilter::EliteWarehouse},
      {QStringLiteral("至高已满"), PetAssetFilter::FullyCultivated},
      {QStringLiteral("只看可提升"), PetAssetFilter::Improvable},
      {QStringLiteral("缺少详情缓存"), PetAssetFilter::MissingDetail},
      {QStringLiteral("确实缺少普通红星"), PetAssetFilter::RedStarMissing},
      {QStringLiteral("已有星神待装备"), PetAssetFilter::StargodEquipNeeded},
      {QStringLiteral("星神等级待提升"), PetAssetFilter::StargodUpgradeNeeded},
      {QStringLiteral("万变红星未具备"), PetAssetFilter::ChangeableMissing},
      {QStringLiteral("培养数据待补"), PetAssetFilter::AnalysisIncomplete},
      {QStringLiteral("星轮培养/突破未满"), PetAssetFilter::AstrolabeMissing},
      {QStringLiteral("神源兽未满"), PetAssetFilter::SacredMissing},
      {QStringLiteral("元魂未满"), PetAssetFilter::SoulMissing},
      {QStringLiteral("当前兑换商店可提升"), PetAssetFilter::ShopImprovable}};
  for (const auto& choice : choices)
    filter_->addItem(choice.first, static_cast<int>(choice.second));
  search_ = new QLineEdit(diagnosticPage);
  search_->setPlaceholderText(QStringLiteral("搜索名称或实例 ID"));
  diagnosticSummary_ = new QLabel(diagnosticPage);
  filters->addWidget(new QLabel(QStringLiteral("筛选"), diagnosticPage));
  filters->addWidget(filter_);
  filters->addWidget(search_, 1);
  filters->addWidget(diagnosticSummary_);
  diagnosticLayout->addLayout(filters);
  diagnosticTable_ = makeView(diagnosticPage);
  diagnosticTable_->setObjectName(QStringLiteral("KQAssetDiagnosticTable"));
  analysisModel_ = new AssetAnalysisModel(this);
  analysisFilterModel_ = new AssetAnalysisFilterProxyModel(this);
  analysisFilterModel_->setSourceModel(analysisModel_);
  diagnosticTable_->setModel(analysisFilterModel_);
  diagnosticTable_->setSortingEnabled(true);
  diagnosticLayout->addWidget(diagnosticTable_, 1);
  tabs_->addTab(diagnosticPage, QStringLiteral("养成诊断中心"));

  auto* recommendationPage = new QWidget(tabs_);
  auto* recommendationLayout = new QVBoxLayout(recommendationPage);
  auto* recommendationToolbar = new QHBoxLayout();
  auto* recommendationNote = new QLabel(
      QStringLiteral("只使用上次手动养成分析、当前账号真实资源缓存和真实兑换目录。每个子页默认显示排序后的前 10 条。"),
      recommendationPage);
  recommendationNote->setWordWrap(true);
  showAllRecommendations_ = new QCheckBox(
      QStringLiteral("显示全部建议"), recommendationPage);
  showAllRecommendations_->setObjectName(
      QStringLiteral("KQShowAllRecommendations"));
  recommendationToolbar->addWidget(recommendationNote, 1);
  recommendationToolbar->addWidget(showAllRecommendations_);
  recommendationLayout->addLayout(recommendationToolbar);
  recommendationTabs_ = new QTabWidget(recommendationPage);
  recommendationTabs_->setObjectName(QStringLiteral("KQRecommendationTabs"));
  const auto addRecommendationSection =
      [this](const QString& title, const QString& objectName,
             QTableView** view, RecommendationModel** model) {
        auto* page = new QWidget(recommendationTabs_);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        *view = makeRecommendationView(page, model);
        (*view)->setObjectName(objectName);
        connect(*view, &QTableView::clicked, this,
                &AssetAnalysisWindow::activateRecommendationIndex);
        layout->addWidget(*view, 1);
        recommendationTabs_->addTab(page, title);
      };
  addRecommendationSection(QStringLiteral("现在可以处理"),
                           QStringLiteral("KQReadyRecommendationTable"),
                           &readyRecommendations_,
                           &readyRecommendationModel_);
  addRecommendationSection(QStringLiteral("还缺资源 / 条件待确认"),
                           QStringLiteral("KQMissingRecommendationTable"),
                           &missingRecommendations_,
                           &missingRecommendationModel_);
  addRecommendationSection(QStringLiteral("本地培养建议"),
                           QStringLiteral("KQNearFullRecommendationTable"),
                           &nearFullRecommendations_,
                           &nearFullRecommendationModel_);
  recommendationLayout->addWidget(recommendationTabs_, 1);
  tabs_->addTab(recommendationPage, QStringLiteral("行动建议"));

  auto* historyPage = new QWidget(tabs_);
  auto* historyLayout = new QVBoxLayout(historyPage);
  auto* historyToolbar = new QHBoxLayout();
  autoSnapshot_ = new QCheckBox(
      QStringLiteral("每次手动刷新资产分析后同步更新当日快照"), historyPage);
  autoSnapshot_->setObjectName(QStringLiteral("KQAutoSnapshot"));
  recordSnapshot_ = new QPushButton(QStringLiteral("立即记录快照"), historyPage);
  historyToolbar->addWidget(autoSnapshot_);
  historyToolbar->addWidget(recordSnapshot_);
  auto* refreshHistory = new QPushButton(QStringLiteral("读取历史"), historyPage);
  olderHistory_ = new QPushButton(QStringLiteral("更早历史"), historyPage);
  compareHistory_ = new QPushButton(QStringLiteral("比较最近两次"), historyPage);
  historyToolbar->addWidget(refreshHistory);
  historyToolbar->addWidget(olderHistory_);
  historyToolbar->addWidget(compareHistory_);
  historyToolbar->addStretch(1);
  historyLayout->addLayout(historyToolbar);
  historyState_ = new QLabel(historyPage);
  historyState_->setWordWrap(true);
  historyLayout->addWidget(historyState_);
  changeSummary_ = new QLabel(historyPage);
  changeSummary_->setWordWrap(true);
  historyLayout->addWidget(changeSummary_);
  auto* historySplitter = new QSplitter(Qt::Vertical, historyPage);
  snapshotTable_ = makeView(historySplitter);
  snapshotTable_->setObjectName(QStringLiteral("KQAssetSnapshotTable"));
  snapshotModel_ = new SnapshotHistoryModel(this);
  snapshotTable_->setModel(snapshotModel_);
  auto* instancePane = new QWidget(historySplitter);
  auto* instanceLayout = new QVBoxLayout(instancePane);
  auto* instanceToolbar = new QHBoxLayout();
  instanceId_ = new QLineEdit(instancePane);
  instanceId_->setPlaceholderText(QStringLiteral("输入实例 ID，或在诊断列表选择一只精灵"));
  auto* showInstance = new QPushButton(QStringLiteral("查看实例历史"), instancePane);
  instanceToolbar->addWidget(new QLabel(QStringLiteral("单只精灵历史"), instancePane));
  instanceToolbar->addWidget(instanceId_, 1);
  instanceToolbar->addWidget(showInstance);
  instanceHistoryTable_ = makeTable(
      instancePane,
      {QStringLiteral("日期"), QStringLiteral("名称"), QStringLiteral("当前战斗力"),
       QStringLiteral("至高战斗力"), QStringLiteral("完成度"),
       QStringLiteral("星神满战力"), QStringLiteral("星轮突破")});
  instanceHistoryTable_->setObjectName(QStringLiteral("KQAssetInstanceHistoryTable"));
  instanceLayout->addLayout(instanceToolbar);
  instanceHistoryState_ = new QLabel(instancePane);
  instanceHistoryState_->setWordWrap(true);
  instanceLayout->addWidget(instanceHistoryState_);
  instanceLayout->addWidget(instanceHistoryTable_, 1);
  historySplitter->addWidget(snapshotTable_);
  historySplitter->addWidget(instancePane);
  historySplitter->setStretchFactor(0, 1);
  historySplitter->setStretchFactor(1, 1);
  historyLayout->addWidget(historySplitter, 1);
  tabs_->addTab(historyPage, QStringLiteral("历史快照"));
  root->addWidget(tabs_, 1);

  status_ = new QLabel(this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(status_);

  connect(overviewTable_, &QTableWidget::cellActivated, this,
          &AssetAnalysisWindow::activateOverviewRow);
  connect(overviewTable_, &QTableWidget::cellClicked, this,
          &AssetAnalysisWindow::activateOverviewRow);
  connect(filter_, &QComboBox::currentIndexChanged, this,
          &AssetAnalysisWindow::applyDiagnosticFilter);
  connect(search_, &QLineEdit::textChanged, this,
          &AssetAnalysisWindow::applyDiagnosticFilter);
  connect(diagnosticTable_, &QTableView::doubleClicked, this,
          &AssetAnalysisWindow::activatePetIndex);
  connect(diagnosticTable_, &QTableView::clicked, this,
          &AssetAnalysisWindow::selectPetIndex);
  connect(refreshAnalysis_, &QPushButton::clicked, this,
          &AssetAnalysisWindow::refreshAnalysis);
  connect(cancelAnalysis_, &QPushButton::clicked, controller_, &AnalysisReadView::cancelAnalysis);
  connect(localStargodStatistics_, &QPushButton::clicked, this, [this] {
    tabs_->setCurrentIndex(0);
    emit localStargodStatisticsRequested();
  });
  connect(cancelLocalStargodStatistics_, &QPushButton::clicked, this, &AssetAnalysisWindow::localStargodStatisticsCancelled);
  connect(refreshHistory, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->requestSnapshotHistory();
  });
  connect(olderHistory_, &QPushButton::clicked, this, [this] {
    if (controller_ && !snapshots_.isEmpty()) controller_->requestSnapshotHistory(snapshots_.first().createdAt);
  });
  connect(compareHistory_, &QPushButton::clicked, this, &AssetAnalysisWindow::loadRecentComparison);
  connect(showAllRecommendations_, &QCheckBox::toggled, this,
          &AssetAnalysisWindow::rebuildRecommendations);
  connect(autoSnapshot_, &QCheckBox::toggled, controller_,
          &AnalysisReadView::setAutoSnapshotEnabled);
  connect(recordSnapshot_, &QPushButton::clicked, this,
          &AssetAnalysisWindow::recordSnapshotNow);
  connect(showInstance, &QPushButton::clicked, this,
          &AssetAnalysisWindow::showInstanceHistory);
  connect(instanceId_, &QLineEdit::returnPressed, this,
          &AssetAnalysisWindow::showInstanceHistory);
  if (controller_) {
    connect(controller_, &AnalysisReadView::analysisCompleted, this, &AssetAnalysisWindow::applyAnalysis);
    connect(controller_, &AnalysisReadView::analysisRunningChanged, this, [this](bool running) {
      refreshAnalysis_->setEnabled(!running);
      cancelAnalysis_->setEnabled(running);
      cancelAnalysis_->setVisible(running);
      if (running) status_->setText(QStringLiteral("正在计算本地分析……"));
    });
    connect(controller_, &AnalysisReadView::inventoryCountsChanged, this,
            &AssetAnalysisWindow::refreshInventory);
    connect(controller_, &AnalysisReadView::inventoryMembershipChanged, this,
            &AssetAnalysisWindow::markInventoryMembershipChanged);
    connect(controller_, &AnalysisReadView::petDetailChanged, this,
            &AssetAnalysisWindow::markPetDetailChanged);
    connect(controller_, &AnalysisReadView::shopAnalysisInvalidated, this,
            &AssetAnalysisWindow::markShopAnalysisInvalidated);
    connect(controller_, &AnalysisReadView::routineSummaryChanged, this,
            &AssetAnalysisWindow::refreshRoutineSummary);
    connect(controller_, &AnalysisReadView::accountAnalysisChanged, this,
            &AssetAnalysisWindow::refreshAccountAnalysis);
    connect(controller_, &AnalysisReadView::historyChanged, this,
            &AssetAnalysisWindow::rebuildHistory);
    connect(controller_, &AnalysisReadView::historyStateChanged, this,
            &AssetAnalysisWindow::refreshHistoryState);
    connect(controller_, &AnalysisReadView::instanceHistoryChanged, this,
            &AssetAnalysisWindow::rebuildInstanceHistory);
    connect(controller_, &AnalysisReadView::statusChanged, status_,
            &QLabel::setText);
  }
  refreshAccountAnalysis();
  refreshAnalysis_->setEnabled(!controller_ || !controller_->analysisRunning());
  cancelAnalysis_->setEnabled(controller_ && controller_->analysisRunning());
  cancelAnalysis_->setVisible(controller_ && controller_->analysisRunning());
  rebuildHistory();
  rebuildRecommendations();
}

void AssetAnalysisWindow::addOverviewRow(const QString& label,
                                         const QString& value,
                                         const QString& note, int action) {
  const int row = overviewTable_->rowCount();
  overviewTable_->insertRow(row);
  QTableWidgetItem* labelItem = item(label, QColor(QStringLiteral("#3156a3")));
  QFont font = labelItem->font();
  font.setBold(true);
  labelItem->setFont(font);
  labelItem->setData(Qt::UserRole, action);
  overviewTable_->setItem(row, 0, labelItem);
  overviewTable_->setItem(row, 1, item(value));
  overviewTable_->setItem(row, 2, item(note));
}

void AssetAnalysisWindow::refreshAnalysis() {
  if (!controller_) return;
  controller_->requestAnalysis();
}

void AssetAnalysisWindow::setLocalStargodStatistics(const LocalStargodStatistics& result) {
  if (!result.account.isEmpty() && !inventory_.account.isEmpty() && result.account != inventory_.account) return;
  localStargodStatistics_->setEnabled(!result.running);
  cancelLocalStargodStatistics_->setVisible(result.running);
  if (!result.error.isEmpty()) {
    localStargodSummary_->setText(QStringLiteral("本地红星统计未完成：%1").arg(result.error)); return;
  }
  const bool partial = result.running || !result.completed || result.incompletePets > 0 || result.skippedFiles > 0;
  const QString prefix = partial ? QStringLiteral("已确认") : QStringLiteral("共");
  QString text = QStringLiteral("普通红星：%1 %2 颗（已装备 %3，本宠背包 %4）\n万变红星：%1 %5 颗（已装备 %6，本宠背包 %7）")
      .arg(prefix).arg(result.ordinaryEquipped + result.ordinaryBackpack).arg(result.ordinaryEquipped).arg(result.ordinaryBackpack)
      .arg(result.changeableEquipped + result.changeableBackpack).arg(result.changeableEquipped).arg(result.changeableBackpack);
  text += QStringLiteral("\n已统计本地缓存 %1 只精灵；%2 只星神资料不全，%3 个缓存文件未计入。")
      .arg(result.countedPets).arg(result.incompletePets).arg(result.skippedFiles);
  if (result.running) text += QStringLiteral(" 正在统计……");
  else if (result.cancelled) text += QStringLiteral(" 已取消，仅显示已扫描部分。");
  else if (partial) text += QStringLiteral(" 数量为已确认下限，未读取或未知项未当成零。");
  if (result.completed && result.finishedAt.isValid()) text += QStringLiteral("\n统计时间：%1").arg(result.finishedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
  localStargodSummary_->setText(text);
}

void AssetAnalysisWindow::resetSessionContext() {
  emit localStargodStatisticsCancelled();
  localStargodStatistics_->setEnabled(true);
  cancelLocalStargodStatistics_->hide();
  localStargodSummary_->setText(QStringLiteral("本地红星：尚未统计"));
  inventory_ = {};
  overview_ = {};
  routineSummary_ = {};
  snapshots_.clear();
  analysisReady_ = false;
  sessionResetPending_ = true;
  analysisModel_->clear();
  for (RecommendationModel* model : {readyRecommendationModel_, missingRecommendationModel_,
                                     nearFullRecommendationModel_}) model->setRecommendations({});
  snapshotModel_->setSnapshots({});
  overviewTable_->setRowCount(0);
  instanceHistoryTable_->setRowCount(0);
  instanceId_->clear();
  accountSummary_->setText(QStringLiteral("等待当前账号数据"));
  diagnosticSummary_->clear();
  changeSummary_->clear();
  historyState_->clear();
  instanceHistoryState_->clear();
  analysisStatus_->setText(QStringLiteral("会话已更新，请重新分析当前账号"));
  status_->setText(QStringLiteral("原账号的分析与历史已从当前页面清除"));
  for (QTableView* table : {diagnosticTable_, readyRecommendations_, missingRecommendations_,
                           nearFullRecommendations_, snapshotTable_}) {
    table->clearSelection();
    table->setCurrentIndex({});
  }
}

void AssetAnalysisWindow::applyAnalysis() {
  if (!controller_) return;
  sessionResetPending_ = false;
  overview_ = controller_->overview();
  inventory_.account = overview_.account;
  inventory_.inventoryUpdatedAt = overview_.inventoryUpdatedAt;
  inventory_.totalPets = overview_.totalPets;
  inventory_.backpackPets = overview_.backpackPets;
  inventory_.normalWarehousePets = overview_.normalWarehousePets;
  inventory_.eliteWarehousePets = overview_.eliteWarehousePets;
  inventory_.missingDetailPets = overview_.missingDetailPets;
  analysisReady_ = true;
  routineSummary_ = controller_->routineSummary();
  rebuildOverview();
  rebuildDiagnostics();
  rebuildRecommendations();
  updateAnalysisStatus();
  status_->setText(QStringLiteral("分析已完成。各项独立判断，共享资源可能冲突；数据变化后需手动重新计算。"));
}

void AssetAnalysisWindow::recordSnapshotNow() {
  if (!controller_) return;
  controller_->requestSnapshot();
}

void AssetAnalysisWindow::refreshInventory() {
  if (!controller_) return;
  inventory_ = controller_->inventorySummary();
  rebuildOverview();
  scheduleAnalysisStatusUpdate();
}

void AssetAnalysisWindow::markInventoryMembershipChanged() {
  scheduleAnalysisStatusUpdate();
}

void AssetAnalysisWindow::markPetDetailChanged(qint64) {
  scheduleAnalysisStatusUpdate();
}

void AssetAnalysisWindow::markShopAnalysisInvalidated() {
  scheduleAnalysisStatusUpdate();
}

void AssetAnalysisWindow::refreshRoutineSummary() {
  if (!controller_) return;
  routineSummary_ = controller_->routineSummary();
  rebuildOverview();
}

void AssetAnalysisWindow::refreshAccountAnalysis() {
  if (!controller_) return;
  sessionResetPending_ = false;
  inventory_ = controller_->inventorySummary();
  routineSummary_ = controller_->routineSummary();
  analysisReady_ = controller_->hasAnalysis();
  overview_ = analysisReady_ ? controller_->overview() : AccountAssetOverview{};
  rebuildOverview();
  rebuildDiagnostics();
  rebuildRecommendations();
  updateAnalysisStatus();
}

void AssetAnalysisWindow::scheduleAnalysisStatusUpdate() {
  if (analysisStatusUpdatePending_) return;
  analysisStatusUpdatePending_ = true;
  QTimer::singleShot(0, this, [this]() {
    analysisStatusUpdatePending_ = false;
    updateAnalysisStatus();
    updateDiagnosticSummary();
    rebuildRecommendations();
  });
}

void AssetAnalysisWindow::updateAnalysisStatus() {
  if (!controller_ || !analysisStatus_) return;
  if (sessionResetPending_) {
    analysisStatus_->setText(QStringLiteral("会话已更新，请重新分析当前账号"));
    return;
  }
  if (!controller_->hasAnalysis()) {
    analysisStatus_->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    analysisStatus_->setText(QStringLiteral(
        "尚未分析，请点击“重新计算养成分析（仅本地）”。"));
    return;
  }
  const int dirtyCount = controller_->dirtyPetIds().size();
  QStringList cultivationReasons;
  if (controller_->inventoryAnalysisStale())
    cultivationReasons.append(QStringLiteral("资产列表已变化"));
  if (dirtyCount > 0)
    cultivationReasons.append(QStringLiteral("%1 只详情已变化").arg(dirtyCount));
  QString stateText;
  if (cultivationReasons.isEmpty()) {
    stateText = QStringLiteral("最新");
  } else {
    stateText = cultivationReasons.join(QStringLiteral("；")) +
                QStringLiteral("，培养结果可能已过期");
  }
  if (controller_->shopAnalysisStale()) {
    if (cultivationReasons.isEmpty()) stateText = QStringLiteral("培养分析最新");
    stateText += QStringLiteral("；商店适用性已变化，商店结果可能已过期");
  }
  const bool stale = !cultivationReasons.isEmpty() ||
                     controller_->shopAnalysisStale();
  analysisStatus_->setStyleSheet(
      stale ? QStringLiteral("color: #b26a00;") : QStringLiteral("color: #087a43;"));
  analysisStatus_->setText(
      QStringLiteral("上次分析：%1\n分析状态：%2")
          .arg(controller_->lastAnalyzedAt().toString(
                   QStringLiteral("yyyy-MM-dd HH:mm:ss")),
               stateText));
}

void AssetAnalysisWindow::rebuildOverview() {
  const QString pending = QStringLiteral("待手动刷新");
  accountSummary_->setText(
      QStringLiteral("账号：%1　资产缓存时间：%2　当前缓存总战力：%3")
          .arg(inventory_.account,
               inventory_.inventoryUpdatedAt.isValid()
                   ? inventory_.inventoryUpdatedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                   : QStringLiteral("尚未完成列表同步"),
               analysisReady_ ? (overview_.totalCurrentPowerKnown ? QString::number(overview_.totalCurrentPower)
                   : QStringLiteral("部分未知（已知合计 %1）").arg(overview_.totalCurrentPower)) : pending));
  overviewTable_->setRowCount(0);
  addOverviewRow(QStringLiteral("精灵总数"), QString::number(inventory_.totalPets),
                 QStringLiteral("查看当前账号全部实例"),
                 static_cast<int>(PetAssetFilter::All));
  addOverviewRow(QStringLiteral("背包数量"), QString::number(inventory_.backpackPets),
                 QStringLiteral("查看背包精灵"),
                 static_cast<int>(PetAssetFilter::Backpack));
  addOverviewRow(QStringLiteral("普通仓库数量"),
                 QString::number(inventory_.normalWarehousePets),
                 QStringLiteral("查看普通仓库精灵"),
                 static_cast<int>(PetAssetFilter::NormalWarehouse));
  addOverviewRow(QStringLiteral("精英仓库数量"),
                 QString::number(inventory_.eliteWarehousePets),
                 QStringLiteral("查看精英仓库精灵"),
                 static_cast<int>(PetAssetFilter::EliteWarehouse));
  addOverviewRow(QStringLiteral("至高已满数量"),
                 analysisReady_ ? QString::number(overview_.fullyCultivatedPets) : pending,
                 QStringLiteral("各适用培养项全部证实已满；达到官方极限不等于至高已满"),
                 static_cast<int>(PetAssetFilter::FullyCultivated));
  addOverviewRow(QStringLiteral("存在培养缺口数量"),
                 analysisReady_ ? QString::number(overview_.improvablePets) : pending,
                 QStringLiteral("查看本地可确认的培养缺口"),
                 static_cast<int>(PetAssetFilter::Improvable));
  addOverviewRow(QStringLiteral("缺少详情缓存数量"),
                 QString::number(inventory_.missingDetailPets),
                 QStringLiteral("先在精灵仓库刷新详情后可分析"),
                 static_cast<int>(PetAssetFilter::MissingDetail));
  addOverviewRow(QStringLiteral("确实缺少普通红星"),
                 analysisReady_ ? QString::number(overview_.redStarMissingPets) : pending,
                 QStringLiteral("已装备与本宠背包合并，按合法可用种类去重；已有未装备红星不再计缺货，万变另列"),
                 static_cast<int>(PetAssetFilter::RedStarMissing));
  const auto countFilter = [this](PetAssetFilter filter) {
    int count = 0;
    for (const auto& pet : overview_.pets) count += AssetDerivation::matchesFilter(pet, filter);
    return count;
  };
  for (const auto& row : QList<QPair<QString, PetAssetFilter>>{
        {QStringLiteral("已有星神待装备/调整"), PetAssetFilter::StargodEquipNeeded},
        {QStringLiteral("星神等级待提升"), PetAssetFilter::StargodUpgradeNeeded},
        {QStringLiteral("万变红星未具备"), PetAssetFilter::ChangeableMissing},
        {QStringLiteral("培养数据待补"), PetAssetFilter::AnalysisIncomplete}})
    addOverviewRow(row.first, analysisReady_ ? QString::number(countFilter(row.second)) : pending,
        row.second == PetAssetFilter::AnalysisIncomplete
            ? QStringLiteral("缺少详情或必要官方定义；不计为缺货，也不判为满培养")
            : QStringLiteral("点击查看对应精灵与具体动作；已具备的资源无需重复兑换"),
        static_cast<int>(row.second));
  addOverviewRow(QStringLiteral("星轮培养/突破未满"),
                 analysisReady_ ? QString::number(overview_.astrolabeMissingPets) : pending,
                 QStringLiteral("包含星轮培养缺口或未突破"),
                 static_cast<int>(PetAssetFilter::AstrolabeMissing));
  addOverviewRow(QStringLiteral("神源兽未满"),
                 analysisReady_ ? QString::number(overview_.sacredMissingPets) : pending,
                 QStringLiteral("按当前官方源兽星级、阶级计划判断"),
                 static_cast<int>(PetAssetFilter::SacredMissing));
  addOverviewRow(QStringLiteral("元魂未满"),
                 analysisReady_ ? QString::number(overview_.soulMissingPets) : pending,
                 QStringLiteral("按元魂战斗力分项缺口判断"),
                 static_cast<int>(PetAssetFilter::SoulMissing));
  addOverviewRow(QStringLiteral("当前商店可提升精灵"),
                 !analysisReady_ ? pending : overview_.shopDataKnown
                     ? QString::number(overview_.shopImprovablePets)
                     : QStringLiteral("未查询"),
                 controller_ && controller_->shopAnalysisStale()
                     ? QStringLiteral("商店适用性结果可能已过期；点击打开兑换商店")
                     : QStringLiteral("仅使用当前商店缓存和现有适用性规则；点击打开兑换商店"),
                 kOpenShop);
  addOverviewRow(QStringLiteral("今日任务 / 玩法剩余"),
                 routineSummary_.routineDataKnown
                     ? QStringLiteral("未完成任务 %1；玩法剩余 %2")
                           .arg(routineSummary_.dailyTasksKnown ? QString::number(routineSummary_.unfinishedDailyTasks) : QStringLiteral("周期未确认"))
                           .arg(opportunitySummaryText(routineSummary_.todayOpportunities))
                     : QStringLiteral("未查询"),
                 routineSummary_.routineDataKnown
                     ? opportunitySummaryNote(routineSummary_.todayOpportunities)
                     : QStringLiteral("点击打开日常活动窗口查看逐项来源"),
                 kOpenRoutine);
  addOverviewRow(QStringLiteral("本周任务 / 玩法剩余"),
                 routineSummary_.routineDataKnown
                     ? QStringLiteral("未完成任务 %1；玩法剩余 %2")
                           .arg(routineSummary_.weeklyTasksKnown ? QString::number(routineSummary_.unfinishedWeeklyTasks) : QStringLiteral("周期未确认"))
                           .arg(opportunitySummaryText(routineSummary_.weekOpportunities))
                     : QStringLiteral("未查询"),
                 routineSummary_.routineDataKnown
                     ? opportunitySummaryNote(routineSummary_.weekOpportunities)
                     : QStringLiteral("点击打开日常活动窗口查看逐项来源"),
                 kOpenRoutine);
}

void AssetAnalysisWindow::setDiagnosticFilter(PetAssetFilter filter) {
  const int index = filter_->findData(static_cast<int>(filter));
  if (index >= 0) filter_->setCurrentIndex(index);
  tabs_->setCurrentIndex(1);
  applyDiagnosticFilter();
}

void AssetAnalysisWindow::activateOverviewRow(int row, int) {
  QTableWidgetItem* first = overviewTable_->item(row, 0);
  if (!first) return;
  const int action = first->data(Qt::UserRole).toInt();
  if (action == kOpenShop) emit shopRequested();
  else if (action == kOpenRoutine) emit routineRequested();
  else setDiagnosticFilter(static_cast<PetAssetFilter>(action));
}

void AssetAnalysisWindow::rebuildDiagnostics() {
  if (!controller_ || !analysisModel_) return;
  if (!analysisReady_) {
    analysisModel_->clear();
    diagnosticSummary_->setText(QStringLiteral(
        "尚未分析，请点击“重新计算养成分析（仅本地）”。"));
    return;
  }
  analysisModel_->setOverview(overview_);
  applyDiagnosticFilter();
}

void AssetAnalysisWindow::applyDiagnosticFilter() {
  if (!analysisFilterModel_) return;
  analysisFilterModel_->setAssetFilter(
      static_cast<PetAssetFilter>(filter_->currentData().toInt()));
  analysisFilterModel_->setQuery(search_->text());
  updateDiagnosticSummary();
}

void AssetAnalysisWindow::updateDiagnosticSummary() {
  if (!analysisReady_ || !analysisFilterModel_ || !controller_) return;
  const bool cultivationStale = controller_->inventoryAnalysisStale() ||
                                !controller_->dirtyPetIds().isEmpty();
  QString staleNote;
  if (cultivationStale) staleNote += QStringLiteral("（培养结果可能已过期）");
  if (controller_->shopAnalysisStale())
    staleNote += QStringLiteral("（商店列可能已过期）");
  diagnosticSummary_->setText(
      QStringLiteral("显示 %1 / %2%3")
          .arg(analysisFilterModel_->rowCount())
          .arg(overview_.totalPets)
          .arg(staleNote));
}

void AssetAnalysisWindow::rebuildRecommendations() {
  if (!controller_ || !readyRecommendationModel_ ||
      !missingRecommendationModel_ || !nearFullRecommendationModel_)
    return;
  QList<ActionRecommendation> ready;
  QList<ActionRecommendation> missing;
  QList<ActionRecommendation> nearFull;
  if (analysisReady_) {
    for (const ActionRecommendation& recommendation :
         controller_->recommendations()) {
      if (recommendation.type == RecommendationType::ReadyNow)
        ready.append(recommendation);
      else if (recommendation.type == RecommendationType::NearFullCultivation ||
               recommendation.type == RecommendationType::LocalCultivation)
        nearFull.append(recommendation);
      else
        missing.append(recommendation);
    }
  }
  const bool showAll = showAllRecommendations_ &&
                       showAllRecommendations_->isChecked();
  const auto displayed = [showAll](const QList<ActionRecommendation>& values) {
    return showAll || values.size() <= 10 ? values : values.mid(0, 10);
  };
  const QList<ActionRecommendation> displayedReady = displayed(ready);
  const QList<ActionRecommendation> displayedMissing = displayed(missing);
  const QList<ActionRecommendation> displayedNearFull = displayed(nearFull);
  readyRecommendationModel_->setRecommendations(displayedReady);
  missingRecommendationModel_->setRecommendations(displayedMissing);
  nearFullRecommendationModel_->setRecommendations(displayedNearFull);
  if (recommendationTabs_) {
    recommendationTabs_->setTabText(
        0, QStringLiteral("现在可以处理（%1 / %2）")
               .arg(displayedReady.size()).arg(ready.size()));
    recommendationTabs_->setTabText(
        1, QStringLiteral("还缺资源 / 条件待确认（%1 / %2）")
               .arg(displayedMissing.size()).arg(missing.size()));
    recommendationTabs_->setTabText(
        2, QStringLiteral("本地培养建议（%1 / %2）")
               .arg(displayedNearFull.size()).arg(nearFull.size()));
  }
}

void AssetAnalysisWindow::activateRecommendationIndex(
    const QModelIndex& index) {
  if (!index.isValid()) return;
  if (index.column() == RecommendationModel::ViewPet) {
    const qint64 instanceId =
        index.data(RecommendationModel::PetInstanceIdRole).toLongLong();
    if (instanceId > 0) emit petRequested(instanceId);
  } else if (index.column() == RecommendationModel::ViewShop) {
    const QString key =
        index.data(RecommendationModel::ShopGoodKeyRole).toString();
    if (!key.isEmpty()) emit shopGoodRequested(key);
  }
}

void AssetAnalysisWindow::activatePetIndex(const QModelIndex& index) {
  const qint64 id = index.data(AssetAnalysisModel::InstanceIdRole).toLongLong();
  if (id > 0) emit petRequested(id);
}

void AssetAnalysisWindow::selectPetIndex(const QModelIndex& index) {
  const qint64 id = index.data(AssetAnalysisModel::InstanceIdRole).toLongLong();
  if (id > 0) instanceId_->setText(QString::number(id));
}

void AssetAnalysisWindow::rebuildHistory() {
  if (!controller_) return;
  snapshots_ = controller_->snapshots();
  {
    const QSignalBlocker blocker(autoSnapshot_);
    autoSnapshot_->setChecked(controller_->autoSnapshotEnabled());
  }
  snapshotModel_->setSnapshots(snapshots_);
  refreshHistoryState();
  if (snapshots_.size() >= 2) {
    if (!snapshots_.last().petsComplete || !snapshots_.at(snapshots_.size() - 2).petsComplete) {
      changeSummary_->setText(QStringLiteral("历史摘要已就绪；点击“比较最近两次”按需读取个体指标。"));
      return;
    }
    const AssetSnapshotDelta delta = AssetSnapshotComparator::compare(
        snapshots_.last(), snapshots_.at(snapshots_.size() - 2));
    if (!delta.accountComparable)
      changeSummary_->setText(QStringLiteral("账号或实例数据不可比较。"));
    else if (!delta.cultivationComparable)
      changeSummary_->setText(QStringLiteral("新增精灵 %1；移出 %2；算法版本不同，不计算新达标。")
          .arg(delta.newPets).arg(delta.removedPets));
    else changeSummary_->setText(
        QStringLiteral("最近两次快照变化：新增精灵 %1　达到满培养 %2　星神满战力 +%3　完成星轮突破 %4　可比个体战力 %5")
            .arg(delta.newPets).arg(delta.newlyFullyCultivated)
            .arg(delta.newlyRedStarComplete).arg(delta.newlyAstrolabeBreakthrough)
            .arg(delta.powerChangeKnown ? signedNumber(delta.totalPowerChange)
                                       : QStringLiteral("部分未知")));
  } else if (snapshots_.size() == 1) {
    changeSummary_->setText(QStringLiteral("已有 1 个快照；记录下一天快照后即可显示变化。"));
  } else {
    changeSummary_->setText(QStringLiteral("尚无历史快照。快照只保存轻量培养指标，不保存完整协议内容。"));
  }
}

void AssetAnalysisWindow::showInstanceHistory() {
  const qint64 id = instanceId_->text().trimmed().toLongLong();
  instanceHistoryTable_->setRowCount(0);
  if (id <= 0 || !controller_) {
    instanceHistoryState_->setText(QStringLiteral("请输入有效的实例 ID。"));
    return;
  }
  controller_->requestInstanceHistory(id);
  instanceHistoryState_->setText(QStringLiteral("正在按需读取这只精灵的历史……"));
}

void AssetAnalysisWindow::refreshHistoryState() {
  if (!controller_) return;
  autoSnapshot_->setEnabled(controller_->autoSnapshotSettingKnown());
  { const QSignalBlocker blocker(autoSnapshot_); autoSnapshot_->setChecked(controller_->autoSnapshotEnabled()); }
  olderHistory_->setEnabled(controller_->snapshotHistoryHasOlder() && !controller_->snapshotHistoryLoading());
  compareHistory_->setEnabled(snapshots_.size() >= 2);
  const QString error = controller_->snapshotHistoryError();
  historyState_->setText(!error.isEmpty() ? QStringLiteral("历史读取未完成：%1").arg(error)
      : controller_->snapshotHistoryLoading() ? QStringLiteral("正在分批读取历史摘要……")
      : QStringLiteral("已载入 %1 份摘要；仅按需读取完整指标。%2").arg(snapshots_.size())
          .arg(controller_->autoSnapshotSettingKnown() ? QString{} : QStringLiteral("快照设置尚未读取或保存完成。")));
}

void AssetAnalysisWindow::loadRecentComparison() {
  if (!controller_ || snapshots_.size() < 2) return;
  for (qsizetype row = snapshots_.size() - 2; row < snapshots_.size(); ++row)
    if (!snapshots_.at(row).petsComplete) controller_->requestSnapshotDetails(snapshots_.at(row).storageKey);
  changeSummary_->setText(QStringLiteral("正在读取最近两份快照的个体指标……"));
}

void AssetAnalysisWindow::rebuildInstanceHistory() {
  if (!controller_ || instanceId_->text().trimmed().toLongLong() != controller_->instanceHistoryId()) return;
  instanceHistoryTable_->setRowCount(0);
  const QString error = controller_->instanceHistoryError();
  const auto entries = controller_->instanceHistory();
  instanceHistoryState_->setText(!error.isEmpty() ? QStringLiteral("实例历史读取未完成：%1").arg(error)
      : controller_->instanceHistoryLoading() ? QStringLiteral("正在分批读取个体历史……")
      : QStringLiteral("已检查 %1 份快照；未知数据保留为未知。 ").arg(entries.size()));
  for (const auto& entry : entries) {
      const AssetSnapshotPet& pet = entry.pet;
      const int row = instanceHistoryTable_->rowCount();
      instanceHistoryTable_->insertRow(row);
      const QStringList values = {
          entry.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
          !entry.membershipKnown ? QStringLiteral("成员数据未知") : !entry.present ? QStringLiteral("当日不在列表") : pet.name,
          pet.currentPowerKnown ? QString::number(pet.currentPower) : QStringLiteral("未知"),
          pet.cultivationKnown ? QString::number(pet.highestPower) : QStringLiteral("未知"),
          pet.cultivationKnown ? QStringLiteral("%1%").arg(pet.completionPercent) : QStringLiteral("未知"),
          pet.redStarKnown ? yesNo(pet.redStarComplete) : QStringLiteral("未知"),
          pet.astrolabeKnown ? yesNo(pet.astrolabeBreakthrough) : QStringLiteral("未知")};
      for (int column = 0; column < values.size(); ++column)
        instanceHistoryTable_->setItem(row, column, item(values.at(column)));
  }
}
