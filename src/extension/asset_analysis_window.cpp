#include "asset_analysis_window.h"

#include "asset_analysis_filter_proxy_model.h"
#include "asset_analysis_model.h"
#include "build_info.h"
#include "recommendation_model.h"
#include "snapshot_history_model.h"

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

AssetAnalysisWindow::AssetAnalysisWindow(AssetAnalysisController* controller,
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
      {QStringLiteral("已满培养"), PetAssetFilter::FullyCultivated},
      {QStringLiteral("只看可提升"), PetAssetFilter::Improvable},
      {QStringLiteral("缺少详情缓存"), PetAssetFilter::MissingDetail},
      {QStringLiteral("星神未满"), PetAssetFilter::RedStarMissing},
      {QStringLiteral("星轮未突破"), PetAssetFilter::AstrolabeMissing},
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
  addRecommendationSection(QStringLiteral("还缺资源 / 资源未知"),
                           QStringLiteral("KQMissingRecommendationTable"),
                           &missingRecommendations_,
                           &missingRecommendationModel_);
  addRecommendationSection(QStringLiteral("接近满培养"),
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
  recordSnapshot_ = new QPushButton(QStringLiteral("立即记录快照"), historyPage);
  historyToolbar->addWidget(autoSnapshot_);
  historyToolbar->addWidget(recordSnapshot_);
  historyToolbar->addStretch(1);
  historyLayout->addLayout(historyToolbar);
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
       QStringLiteral("最高战斗力"), QStringLiteral("完成度"),
       QStringLiteral("星神满战力"), QStringLiteral("星轮突破")});
  instanceHistoryTable_->setObjectName(QStringLiteral("KQAssetInstanceHistoryTable"));
  instanceLayout->addLayout(instanceToolbar);
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
  connect(showAllRecommendations_, &QCheckBox::toggled, this,
          &AssetAnalysisWindow::rebuildRecommendations);
  connect(autoSnapshot_, &QCheckBox::toggled, controller_,
          &AssetAnalysisController::setAutoSnapshotEnabled);
  connect(recordSnapshot_, &QPushButton::clicked, this,
          &AssetAnalysisWindow::recordSnapshotNow);
  connect(showInstance, &QPushButton::clicked, this,
          &AssetAnalysisWindow::showInstanceHistory);
  connect(instanceId_, &QLineEdit::returnPressed, this,
          &AssetAnalysisWindow::showInstanceHistory);
  if (controller_) {
    connect(controller_, &AssetAnalysisController::inventoryCountsChanged, this,
            &AssetAnalysisWindow::refreshInventory);
    connect(controller_, &AssetAnalysisController::inventoryMembershipChanged, this,
            &AssetAnalysisWindow::markInventoryMembershipChanged);
    connect(controller_, &AssetAnalysisController::petDetailChanged, this,
            &AssetAnalysisWindow::markPetDetailChanged);
    connect(controller_, &AssetAnalysisController::shopAnalysisInvalidated, this,
            &AssetAnalysisWindow::markShopAnalysisInvalidated);
    connect(controller_, &AssetAnalysisController::routineSummaryChanged, this,
            &AssetAnalysisWindow::refreshRoutineSummary);
    connect(controller_, &AssetAnalysisController::accountAnalysisChanged, this,
            &AssetAnalysisWindow::refreshAccountAnalysis);
    connect(controller_, &AssetAnalysisController::historyChanged, this,
            &AssetAnalysisWindow::rebuildHistory);
    connect(controller_, &AssetAnalysisController::statusChanged, status_,
            &QLabel::setText);
  }
  refreshAccountAnalysis();
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
  overview_ = controller_->recalculateOverview();
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
  if (controller_->autoSnapshotEnabled()) {
    controller_->recordSnapshotFromOverview(overview_);
  } else {
    status_->setText(QStringLiteral(
        "资产分析已按当前本地缓存手动刷新；后续数据变化不会在后台自动重算。"));
  }
}

void AssetAnalysisWindow::recordSnapshotNow() {
  if (!controller_) return;
  controller_->recordSnapshot();
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
               analysisReady_ ? QString::number(overview_.totalCurrentPower) : pending));
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
  addOverviewRow(QStringLiteral("已满培养数量"),
                 analysisReady_ ? QString::number(overview_.fullyCultivatedPets) : pending,
                 QStringLiteral("以当前战斗力达到已知最高战斗力为准"),
                 static_cast<int>(PetAssetFilter::FullyCultivated));
  addOverviewRow(QStringLiteral("存在培养缺口数量"),
                 analysisReady_ ? QString::number(overview_.improvablePets) : pending,
                 QStringLiteral("查看本地可确认的培养缺口"),
                 static_cast<int>(PetAssetFilter::Improvable));
  addOverviewRow(QStringLiteral("缺少详情缓存数量"),
                 QString::number(inventory_.missingDetailPets),
                 QStringLiteral("先在精灵仓库刷新详情后可分析"),
                 static_cast<int>(PetAssetFilter::MissingDetail));
  addOverviewRow(QStringLiteral("星神满战力未满足"),
                 analysisReady_ ? QString::number(overview_.redStarMissingPets) : pending,
                 QStringLiteral("按每只精灵实际普通栏位数，合计已装备与精灵星神背包判断；固定万变不计入"),
                 static_cast<int>(PetAssetFilter::RedStarMissing));
  addOverviewRow(QStringLiteral("星轮未突破"),
                 analysisReady_ ? QString::number(overview_.astrolabeMissingPets) : pending,
                 QStringLiteral("包含星轮培养缺口或未突破"),
                 static_cast<int>(PetAssetFilter::AstrolabeMissing));
  addOverviewRow(QStringLiteral("神源兽未满"),
                 analysisReady_ ? QString::number(overview_.sacredMissingPets) : pending,
                 QStringLiteral("按神源兽战斗力分项缺口判断"),
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
                           .arg(routineSummary_.unfinishedDailyTasks)
                           .arg(routineSummary_.todayOpportunityKnown
                                    ? QString::number(routineSummary_.todayOpportunityRemaining)
                                    : QStringLiteral("—"))
                     : QStringLiteral("未查询"),
                 QStringLiteral("点击打开日常活动窗口查看逐项来源"), kOpenRoutine);
  addOverviewRow(QStringLiteral("本周任务 / 玩法剩余"),
                 routineSummary_.routineDataKnown
                     ? QStringLiteral("未完成任务 %1；玩法剩余 %2")
                           .arg(routineSummary_.unfinishedWeeklyTasks)
                           .arg(routineSummary_.weekOpportunityKnown
                                    ? QString::number(routineSummary_.weekOpportunityRemaining)
                                    : QStringLiteral("—"))
                     : QStringLiteral("未查询"),
                 QStringLiteral("点击打开日常活动窗口查看逐项来源"), kOpenRoutine);
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
      else if (recommendation.type ==
               RecommendationType::NearFullCultivation)
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
        1, QStringLiteral("还缺资源 / 资源未知（%1 / %2）")
               .arg(displayedMissing.size()).arg(missing.size()));
    recommendationTabs_->setTabText(
        2, QStringLiteral("接近满培养（%1 / %2）")
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
  if (snapshots_.size() >= 2) {
    const AssetSnapshotDelta delta = AssetAnalysisController::compareSnapshots(
        snapshots_.last(), snapshots_.at(snapshots_.size() - 2));
    changeSummary_->setText(
        QStringLiteral("最近两次快照变化：新增精灵 %1　达到满培养 %2　星神满战力 +%3　完成星轮突破 %4　账号总战力 %5")
            .arg(delta.newPets).arg(delta.newlyFullyCultivated)
            .arg(delta.newlyRedStarComplete).arg(delta.newlyAstrolabeBreakthrough)
            .arg(signedNumber(delta.totalPowerChange)));
  } else if (snapshots_.size() == 1) {
    changeSummary_->setText(QStringLiteral("已有 1 个快照；记录下一天快照后即可显示变化。"));
  } else {
    changeSummary_->setText(QStringLiteral("尚无历史快照。快照只保存轻量培养指标，不保存完整协议内容。"));
  }
  showInstanceHistory();
}

void AssetAnalysisWindow::showInstanceHistory() {
  const qint64 id = instanceId_->text().trimmed().toLongLong();
  instanceHistoryTable_->setRowCount(0);
  if (id <= 0) return;
  for (const AccountAssetSnapshot& snapshot : snapshots_) {
    for (const AssetSnapshotPet& pet : snapshot.pets) {
      if (pet.instanceId != id) continue;
      const int row = instanceHistoryTable_->rowCount();
      instanceHistoryTable_->insertRow(row);
      const QStringList values = {
          snapshot.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")), pet.name,
          QString::number(pet.currentPower), QString::number(pet.highestPower),
          QStringLiteral("%1%").arg(pet.completionPercent),
          yesNo(pet.redStarComplete), yesNo(pet.astrolabeBreakthrough)};
      for (int column = 0; column < values.size(); ++column)
        instanceHistoryTable_->setItem(row, column, item(values.at(column)));
    }
  }
}
