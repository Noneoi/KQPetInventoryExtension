#include "asset_analysis_window.h"

#include "build_info.h"

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
#include <QTabWidget>
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
  refreshAnalysis_ = new QPushButton(QStringLiteral("刷新资产分析"), this);
  refreshAnalysis_->setObjectName(QStringLiteral("KQAssetAnalysisRefresh"));
  refreshAnalysis_->setToolTip(
      QStringLiteral("使用当前本地缓存重新计算培养完成度、缺口、商店适用性和玩法汇总；不会发送协议"));
  summaryRow->addWidget(accountSummary_, 1);
  summaryRow->addWidget(refreshAnalysis_);
  root->addLayout(summaryRow);

  tabs_ = new QTabWidget(this);
  auto* overviewPage = new QWidget(tabs_);
  auto* overviewLayout = new QVBoxLayout(overviewPage);
  auto* overviewNote = new QLabel(
      QStringLiteral("前四项基础数量随列表同步轻量更新；其余指标仅在点击“刷新资产分析”后计算。点击精灵指标可进入诊断列表，商店和玩法指标会打开现有窗口。"),
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
      {QStringLiteral("缺红星"), PetAssetFilter::RedStarMissing},
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
  diagnosticTable_ = makeTable(
      diagnosticPage,
      {QStringLiteral("精灵"), QStringLiteral("位置"), QStringLiteral("当前战斗力"),
       QStringLiteral("极限 / 最高战斗力"), QStringLiteral("完成度"),
       QStringLiteral("主要培养缺口"), QStringLiteral("商店可提升")});
  diagnosticTable_->setObjectName(QStringLiteral("KQAssetDiagnosticTable"));
  diagnosticLayout->addWidget(diagnosticTable_, 1);
  tabs_->addTab(diagnosticPage, QStringLiteral("养成诊断中心"));

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
  snapshotTable_ = makeTable(
      historySplitter,
      {QStringLiteral("快照日期"), QStringLiteral("精灵总数"),
       QStringLiteral("已满培养"), QStringLiteral("账号总战力"),
       QStringLiteral("较上次变化")});
  snapshotTable_->setObjectName(QStringLiteral("KQAssetSnapshotTable"));
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
       QStringLiteral("红星"), QStringLiteral("星轮突破")});
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
          &AssetAnalysisWindow::rebuildDiagnostics);
  connect(search_, &QLineEdit::textChanged, this,
          &AssetAnalysisWindow::rebuildDiagnostics);
  connect(diagnosticTable_, &QTableWidget::cellDoubleClicked, this,
          &AssetAnalysisWindow::activatePetRow);
  connect(diagnosticTable_, &QTableWidget::cellClicked, this,
          [this](int row, int) {
            QTableWidgetItem* first = diagnosticTable_->item(row, 0);
            if (first) instanceId_->setText(first->data(Qt::UserRole).toString());
          });
  connect(refreshAnalysis_, &QPushButton::clicked, this,
          &AssetAnalysisWindow::refreshAnalysis);
  connect(autoSnapshot_, &QCheckBox::toggled, controller_,
          &AssetAnalysisController::setAutoSnapshotEnabled);
  connect(recordSnapshot_, &QPushButton::clicked, this,
          &AssetAnalysisWindow::recordSnapshotNow);
  connect(showInstance, &QPushButton::clicked, this,
          &AssetAnalysisWindow::showInstanceHistory);
  connect(instanceId_, &QLineEdit::returnPressed, this,
          &AssetAnalysisWindow::showInstanceHistory);
  if (controller_) {
    connect(controller_, &AssetAnalysisController::inventoryChanged, this,
            &AssetAnalysisWindow::refreshInventory);
    connect(controller_, &AssetAnalysisController::analysisInvalidated, this,
            &AssetAnalysisWindow::invalidateAnalysis);
    connect(controller_, &AssetAnalysisController::historyChanged, this,
            &AssetAnalysisWindow::rebuildHistory);
    connect(controller_, &AssetAnalysisController::statusChanged, status_,
            &QLabel::setText);
  }
  refreshInventory();
  rebuildHistory();
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
  overview_ = controller_->overview();
  inventory_.account = overview_.account;
  inventory_.inventoryUpdatedAt = overview_.inventoryUpdatedAt;
  inventory_.totalPets = overview_.totalPets;
  inventory_.backpackPets = overview_.backpackPets;
  inventory_.normalWarehousePets = overview_.normalWarehousePets;
  inventory_.eliteWarehousePets = overview_.eliteWarehousePets;
  analysisReady_ = true;
  rebuildOverview();
  rebuildDiagnostics();
  if (controller_->autoSnapshotEnabled()) {
    controller_->recordSnapshotFromOverview(overview_);
  } else {
    status_->setText(QStringLiteral(
        "资产分析已按当前本地缓存手动刷新；后续数据变化不会在后台自动重算。"));
  }
}

void AssetAnalysisWindow::recordSnapshotNow() {
  if (!controller_) return;
  overview_ = controller_->overview();
  inventory_.account = overview_.account;
  inventory_.inventoryUpdatedAt = overview_.inventoryUpdatedAt;
  inventory_.totalPets = overview_.totalPets;
  inventory_.backpackPets = overview_.backpackPets;
  inventory_.normalWarehousePets = overview_.normalWarehousePets;
  inventory_.eliteWarehousePets = overview_.eliteWarehousePets;
  analysisReady_ = true;
  rebuildOverview();
  rebuildDiagnostics();
  controller_->recordSnapshotFromOverview(overview_);
}

void AssetAnalysisWindow::refreshInventory() {
  if (!controller_) return;
  inventory_ = controller_->inventorySummary();
  analysisReady_ = false;
  overview_.pets.clear();
  rebuildOverview();
  rebuildDiagnostics();
  status_->setText(QStringLiteral(
      "背包和仓库基础数量已更新；培养、商店和玩法分析等待手动刷新。"));
}

void AssetAnalysisWindow::invalidateAnalysis() {
  if (!analysisReady_) return;
  analysisReady_ = false;
  overview_.pets.clear();
  rebuildOverview();
  rebuildDiagnostics();
  status_->setText(QStringLiteral(
      "底层缓存已有变化；为避免批量详情刷新期间反复计算，请手动刷新资产分析。"));
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
                 analysisReady_ ? QString::number(overview_.missingDetailPets) : pending,
                 QStringLiteral("先在精灵仓库刷新详情后可分析"),
                 static_cast<int>(PetAssetFilter::MissingDetail));
  addOverviewRow(QStringLiteral("红星未满"),
                 analysisReady_ ? QString::number(overview_.redStarMissingPets) : pending,
                 QStringLiteral("按已知红星战斗力增益判断"),
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
                 QStringLiteral("仅使用当前商店缓存和现有适用性规则；点击打开兑换商店"),
                 kOpenShop);
  addOverviewRow(QStringLiteral("今日任务 / 玩法剩余"),
                 !analysisReady_ ? pending : overview_.routineDataKnown
                     ? QStringLiteral("未完成任务 %1；玩法剩余 %2")
                           .arg(overview_.unfinishedDailyTasks)
                           .arg(overview_.todayOpportunityKnown
                                    ? QString::number(overview_.todayOpportunityRemaining)
                                    : QStringLiteral("—"))
                     : QStringLiteral("未查询"),
                 QStringLiteral("点击打开日常活动窗口查看逐项来源"), kOpenRoutine);
  addOverviewRow(QStringLiteral("本周任务 / 玩法剩余"),
                 !analysisReady_ ? pending : overview_.routineDataKnown
                     ? QStringLiteral("未完成任务 %1；玩法剩余 %2")
                           .arg(overview_.unfinishedWeeklyTasks)
                           .arg(overview_.weekOpportunityKnown
                                    ? QString::number(overview_.weekOpportunityRemaining)
                                    : QStringLiteral("—"))
                     : QStringLiteral("未查询"),
                 QStringLiteral("点击打开日常活动窗口查看逐项来源"), kOpenRoutine);
}

void AssetAnalysisWindow::setDiagnosticFilter(PetAssetFilter filter) {
  const int index = filter_->findData(static_cast<int>(filter));
  if (index >= 0) filter_->setCurrentIndex(index);
  tabs_->setCurrentIndex(1);
  rebuildDiagnostics();
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
  if (!controller_) return;
  diagnosticTable_->setRowCount(0);
  if (!analysisReady_) {
    diagnosticSummary_->setText(QStringLiteral("等待手动刷新资产分析"));
    return;
  }
  const auto filter = static_cast<PetAssetFilter>(filter_->currentData().toInt());
  const QString query = search_->text().trimmed();
  for (const PetAssetRecord& pet : overview_.pets) {
    if (!AssetAnalysisController::matchesFilter(pet, filter)) continue;
    if (!query.isEmpty() && !pet.name.contains(query, Qt::CaseInsensitive) &&
        !QString::number(pet.instanceId).contains(query))
      continue;
    const int row = diagnosticTable_->rowCount();
    diagnosticTable_->insertRow(row);
    QTableWidgetItem* name = item(QStringLiteral("%1\n实例 %2")
                                      .arg(pet.name).arg(pet.instanceId));
    name->setData(Qt::UserRole, QString::number(pet.instanceId));
    diagnosticTable_->setItem(row, 0, name);
    diagnosticTable_->setItem(row, 1, item(pet.location));
    diagnosticTable_->setItem(row, 2,
                              item(pet.detailAvailable ? QString::number(pet.currentPower)
                                                       : QStringLiteral("—")));
    diagnosticTable_->setItem(
        row, 3,
        item(pet.detailAvailable
                 ? QStringLiteral("%1 / %2").arg(pet.extremePower).arg(pet.highestPower)
                 : QStringLiteral("缺少详情缓存")));
    const QColor completionColor = pet.fullyCultivated
                                       ? QColor(QStringLiteral("#087a43"))
                                       : pet.detailAvailable
                                             ? QColor(QStringLiteral("#b54708"))
                                             : QColor();
    diagnosticTable_->setItem(
        row, 4,
        item(pet.detailAvailable ? QStringLiteral("%1%").arg(pet.completionPercent)
                                 : QStringLiteral("—"),
             completionColor));
    diagnosticTable_->setItem(
        row, 5,
        item(!pet.detailAvailable
                 ? QStringLiteral("等待详情刷新")
                 : pet.fullyCultivated
                       ? QStringLiteral("已达到当前已知最高培养")
                       : pet.gaps.isEmpty() ? QStringLiteral("存在未归类战斗力差距")
                                            : pet.gaps.join(QStringLiteral("；"))));
    diagnosticTable_->setItem(
        row, 6,
        item(!overview_.shopDataKnown ? QStringLiteral("未查询")
                                      : pet.shopImprovable ? QStringLiteral("是")
                                                           : QStringLiteral("否"),
             pet.shopImprovable ? QColor(QStringLiteral("#c62828")) : QColor()));
  }
  diagnosticSummary_->setText(QStringLiteral("显示 %1 / %2")
                                  .arg(diagnosticTable_->rowCount())
                                  .arg(overview_.totalPets));
}

void AssetAnalysisWindow::activatePetRow(int row, int) {
  QTableWidgetItem* first = diagnosticTable_->item(row, 0);
  if (!first) return;
  const qint64 id = first->data(Qt::UserRole).toString().toLongLong();
  if (id > 0) emit petRequested(id);
}

void AssetAnalysisWindow::rebuildHistory() {
  if (!controller_) return;
  snapshots_ = controller_->snapshots();
  {
    const QSignalBlocker blocker(autoSnapshot_);
    autoSnapshot_->setChecked(controller_->autoSnapshotEnabled());
  }
  snapshotTable_->setRowCount(snapshots_.size());
  for (int index = 0; index < snapshots_.size(); ++index) {
    const AccountAssetSnapshot& snapshot = snapshots_.at(index);
    QString deltaText = QStringLiteral("首个快照");
    if (index > 0) {
      const AssetSnapshotDelta delta =
          AssetAnalysisController::compareSnapshots(snapshot, snapshots_.at(index - 1));
      deltaText = QStringLiteral("新增 %1；满培养 +%2；红星 +%3；星轮 +%4；战力 %5")
                      .arg(delta.newPets).arg(delta.newlyFullyCultivated)
                      .arg(delta.newlyRedStarComplete)
                      .arg(delta.newlyAstrolabeBreakthrough)
                      .arg(signedNumber(delta.totalPowerChange));
    }
    snapshotTable_->setItem(index, 0,
                            item(snapshot.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    snapshotTable_->setItem(index, 1, item(QString::number(snapshot.totalPets)));
    snapshotTable_->setItem(index, 2,
                            item(QString::number(snapshot.fullyCultivatedPets)));
    snapshotTable_->setItem(index, 3,
                            item(QString::number(snapshot.totalCurrentPower)));
    snapshotTable_->setItem(index, 4, item(deltaText));
  }
  if (snapshots_.size() >= 2) {
    const AssetSnapshotDelta delta = AssetAnalysisController::compareSnapshots(
        snapshots_.last(), snapshots_.at(snapshots_.size() - 2));
    changeSummary_->setText(
        QStringLiteral("最近两次快照变化：新增精灵 %1　达到满培养 %2　新增红星 %3　完成星轮突破 %4　账号总战力 %5")
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
