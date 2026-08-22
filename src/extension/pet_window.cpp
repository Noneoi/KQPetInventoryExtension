#include "pet_window.h"

#include "build_info.h"

#include "pet_detail_analyzer.h"
#include "pet_detail_catalog.h"
#include "pet_detail_renderer.h"
#include "pet_identity.h"
#include "pet_image_cache.h"
#include "pet_move_policy.h"
#include "pet_repository.h"
#include "pet_search.h"
#include "pet_filter_proxy_model.h"
#include "pet_table_model.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextLayout>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

namespace {

QString valueText(const QJsonValue& value) {
  if (value.isString())
    return value.toString();
  if (value.isDouble())
    return QString::number(value.toDouble(), 'g', 16);
  if (value.isBool())
    return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
  if (value.isNull() || value.isUndefined())
    return QStringLiteral("—");
  if (value.isArray())
    return QStringLiteral("数组（%1 项）").arg(value.toArray().size());
  return QStringLiteral("对象");
}

QString friendlyKey(const QString& key) {
  static const QHash<QString, QString> names = {
      {QStringLiteral("id"), QStringLiteral("实例 ID")},
      {QStringLiteral("r"), QStringLiteral("精灵种族 ID")},
      {QStringLiteral("ri"), QStringLiteral("精灵种族 ID")},
      {QStringLiteral("n"), QStringLiteral("名称")},
      {QStringLiteral("customName"), QStringLiteral("自定义名称")},
      {QStringLiteral("lv"), QStringLiteral("等级")},
      {QStringLiteral("fr"), QStringLiteral("头像 ID")},
      {QStringLiteral("g"), QStringLiteral("性别")},
      {QStringLiteral("gd"), QStringLiteral("获得时间")},
      {QStringLiteral("pr"), QStringLiteral("精英标记/阶级")},
      {QStringLiteral("ce"), QStringLiteral("当前经验")},
      {QStringLiteral("cte"), QStringLiteral("累计经验")},
      {QStringLiteral("ne"), QStringLiteral("下级经验")},
      {QStringLiteral("chp"), QStringLiteral("当前生命")},
      {QStringLiteral("ip"), QStringLiteral("天赋序列")},
      {QStringLiteral("gps"), QStringLiteral("天赋进度")},
      {QStringLiteral("gt"), QStringLiteral("天赋等级")},
      {QStringLiteral("sgs"), QStringLiteral("星神序列")},
      {QStringLiteral("sgp"), QStringLiteral("星神战力")},
      {QStringLiteral("zdl"), QStringLiteral("当前战斗力")},
      {QStringLiteral("xzdl"), QStringLiteral("极限战斗力")},
      {QStringLiteral("czdlv"), QStringLiteral("当前战斗力分项")},
      {QStringLiteral("mzdlv"), QStringLiteral("极限战斗力分项")},
      {QStringLiteral("cps"), QStringLiteral("通灵师/职业序列")},
      {QStringLiteral("eps"), QStringLiteral("装备序列")},
      {QStringLiteral("lss"), QStringLiteral("传说石序列")},
      {QStringLiteral("badge"), QStringLiteral("元魂")},
      {QStringLiteral("astrolabe"), QStringLiteral("天迹星轮")},
      {QStringLiteral("astrolabebr"), QStringLiteral("天迹星轮突破")},
      {QStringLiteral("shenjue"), QStringLiteral("神源兽")},
      {QStringLiteral("us"), QStringLiteral("超必杀技能")},
      {QStringLiteral("cc"), QStringLiteral("培养次数")},
      {QStringLiteral("p"), QStringLiteral("位置")},
      {QStringLiteral("srpi"), QStringLiteral("召唤者实例 ID")},
      {QStringLiteral("srri"), QStringLiteral("召唤者种族 ID")},
      {QStringLiteral("sepi"), QStringLiteral("被召唤精灵实例 ID")},
      {QStringLiteral("sdpi"), QStringLiteral("仓库被召唤精灵实例 ID")},
      {QStringLiteral("sppl"), QStringLiteral("被召唤精灵详情")},
      {QStringLiteral("asps"), QStringLiteral("召唤精灵列表")},
      {QStringLiteral("cepi"), QStringLiteral("被携带精灵实例 ID")},
      {QStringLiteral("cppl"), QStringLiteral("被携带精灵详情")},
      {QStringLiteral("acps"), QStringLiteral("携带/神使精灵列表")},
      {QStringLiteral("crpis"), QStringLiteral("携带者/神使实例 ID 列表")},
      {QStringLiteral("ownership"), QStringLiteral("关系归属标记")},
      {QStringLiteral("relationEffect"), QStringLiteral("关系效果")},
      {QStringLiteral("_packType"), QStringLiteral("背包类型")},
      {QStringLiteral("_position"), QStringLiteral("背包位置")},
      {QStringLiteral("_warehouseGroup"), QStringLiteral("仓库分组")},
      {QStringLiteral("_location"), QStringLiteral("数据位置")}};
  return names.value(key, key);
}

class SearchHighlightDelegate final : public QStyledItemDelegate {
public:
  SearchHighlightDelegate(const QLineEdit* search, QObject* parent)
      : QStyledItemDelegate(parent), search_(search) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    if (index.column() != 0 || !search_ || search_->text().trimmed().isEmpty()) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    QStyleOptionViewItem styled(option);
    initStyleOption(&styled, index);
    const QString text = styled.text;
    const QPair<int, int> range = petQueryHighlightRange(search_->text(), text);
    if (range.first < 0 || range.second <= 0) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }

    const QWidget* widget = option.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, widget);
    QStyleOptionViewItem background(styled);
    background.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

    QTextLayout layout(text, styled.font);
    QList<QTextLayout::FormatRange> formats;
    QTextLayout::FormatRange normal;
    normal.start = 0;
    normal.length = text.size();
    normal.format.setForeground(styled.state & QStyle::State_Selected
                                    ? styled.palette.highlightedText()
                                    : styled.palette.text());
    formats.append(normal);
    QTextLayout::FormatRange highlight;
    highlight.start = range.first;
    highlight.length = range.second;
    highlight.format.setForeground(QColor(QStringLiteral("#e22929")));
    highlight.format.setFontWeight(QFont::Bold);
    formats.append(highlight);
    layout.setFormats(formats);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) line.setLineWidth(textRect.width());
    layout.endLayout();
    if (!line.isValid()) return;
    const qreal y = textRect.top() + (textRect.height() - line.height()) / 2.0;
    painter->save();
    painter->setClipRect(textRect);
    layout.draw(painter, QPointF(textRect.left(), y));
    painter->restore();
  }

private:
  const QLineEdit* search_ = nullptr;
};

void configurePetTable(QTableView* table, bool backpack) {
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideNone);
  table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  table->setIconSize(QSize(24, 24));
  table->verticalHeader()->setVisible(false);
  table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table->verticalHeader()->setDefaultSectionSize(27);
  table->horizontalHeader()->setStretchLastSection(false);
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
  table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
  table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
  table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
  if (backpack)
    table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Fixed);
  table->setColumnWidth(0, 235);
  table->setColumnWidth(1, 82);
  table->setColumnWidth(2, 145);
  table->setColumnWidth(3, 68);
  table->setColumnWidth(4, 50);
  table->setColumnWidth(5, 205);
  if (backpack) {
    table->setColumnWidth(6, 72);
    table->setColumnWidth(7, 100);
    table->setMinimumWidth(965);
  } else {
    table->setColumnWidth(6, 100);
    table->setMinimumWidth(900);
  }
}

QTableWidget* makeTable(QWidget* parent, bool backpack = false) {
  auto* table = new QTableWidget(parent);
  QStringList headers = {QStringLiteral("名称"), QStringLiteral("属性"),
                         QStringLiteral("职业"), QStringLiteral("时代"),
                         QStringLiteral("等级"),
                         QStringLiteral("战斗力 / 极限战斗力")};
  if (backpack) headers.append(QStringLiteral("是否出阵"));
  headers.append(QStringLiteral("位置"));
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  configurePetTable(table, backpack);
  return table;
}

QTableView* makeTableView(QWidget* parent) {
  return new QTableView(parent);
}

void resizeModelViewColumns(QTableView* table) {
  if (!table || !table->model()) return;
  const QFontMetrics metrics(table->font());
  int nameWidth = metrics.horizontalAdvance(QStringLiteral("名称")) + 48;
  int jobWidth = metrics.horizontalAdvance(QStringLiteral("职业")) + 24;
  int powerWidth = metrics.horizontalAdvance(QStringLiteral("战斗力 / 极限战斗力")) + 24;
  for (int row = 0; row < table->model()->rowCount(); ++row) {
    nameWidth = qMax(nameWidth,
                     metrics.horizontalAdvance(
                         table->model()->index(row, 0).data().toString()) + 48);
    jobWidth = qMax(jobWidth,
                    metrics.horizontalAdvance(
                        table->model()->index(row, 2).data().toString()) + 24);
    powerWidth = qMax(powerWidth,
                      metrics.horizontalAdvance(
                          table->model()->index(row, 5).data().toString()) + 24);
  }
  table->setColumnWidth(0, qBound(205, nameWidth, 285));
  table->setColumnWidth(2, qBound(105, jobWidth, 175));
  table->setColumnWidth(5, qBound(185, powerWidth, 225));
}

enum class PetSortMode {
  Default = 0,
  BattlePower = 1,
  ExtremePower = 2,
  CatalogSequence = 3,
  ObtainedAt = 4
};

void addSortChoices(QComboBox* box) {
  box->addItem(QStringLiteral("默认排序"), static_cast<int>(PetSortMode::Default));
  box->addItem(QStringLiteral("战斗力"), static_cast<int>(PetSortMode::BattlePower));
  box->addItem(QStringLiteral("极限战斗力"), static_cast<int>(PetSortMode::ExtremePower));
  box->addItem(QStringLiteral("图鉴序列"), static_cast<int>(PetSortMode::CatalogSequence));
  box->addItem(QStringLiteral("获得时间"), static_cast<int>(PetSortMode::ObtainedAt));
}

QStringList categoryParts(const QString& text) {
  QStringList result;
  for (QString part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    part = part.trimmed();
    if (!part.isEmpty()) result.append(part);
  }
  if (result.isEmpty() && !text.trimmed().isEmpty()) result.append(text.trimmed());
  return result;
}

QString eraForPet(const QJsonObject& pet) {
  return PetDetailCatalog::instance().resolvedEra(pet);
}

using BattlePowerState = PetBattlePowerState;

BattlePowerState battlePowerState(const QJsonObject& pet,
                                  const PetDetailCatalog&) {
  return PetDetailAnalyzer::analyzeBattlePower(pet);
}

QString battlePowerTableText(const BattlePowerState& state) {
  if (!state.hasCurrent && !state.hasExtreme)
    return QStringLiteral("—");
  const QString current = state.hasCurrent ? QString::number(state.current) : QStringLiteral("—");
  const QString extreme = state.hasExtreme ? QString::number(state.extreme) : QStringLiteral("—");
  if (state.isHighest)
    return QStringLiteral("%1 / %2（最高）").arg(current, extreme);
  if (state.hasCurrent && state.hasExtreme)
    return QStringLiteral("%1 / %2（距最高 %3）")
        .arg(current, extreme)
        .arg(qMax(0, state.highest - state.current));
  return QStringLiteral("%1 / %2").arg(current, extreme);
}

QString gridPositionText(const QJsonObject& pet, const QString& location) {
  if (location != QStringLiteral("backpack")) {
    return pet.value(QStringLiteral("_warehouseGroup")).toString() ==
                   QStringLiteral("elite")
               ? QStringLiteral("精英")
               : QStringLiteral("普通");
  }
  if (!pet.contains(QStringLiteral("_position")))
    return QStringLiteral("待刷新");
  const int index = qMax(0, pet.value(QStringLiteral("_position")).toInt());
  const int page = index / 12 + 1;
  const int positionOnPage = index % 12;
  const int row = positionOnPage / 6 + 1;
  return QStringLiteral("第%1页 第%2排").arg(page).arg(row);
}

}  // namespace

PetWindow::PetWindow(PetRepository* repository, QWidget* parent)
    : QDialog(parent), repository_(repository) {
  imageCache_ = new PetImageCache(repository_->dataRoot(), this);
  setObjectName(QStringLiteral("KQPetInventoryWindow"));
  setWindowTitle(QStringLiteral("原版氪奇 · 精灵背包与仓库 · %1")
                     .arg(BuildInfo::displayVersion()));
  resize(1560, 820);
  setMinimumSize(1280, 700);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(QStringLiteral("搜索名称、实例 ID 或种族 ID"));
  refresh_ = new QPushButton(QStringLiteral("刷新背包/仓库"), this);
  refreshDetails_ = new QPushButton(QStringLiteral("刷新仓库详情"), this);
  pauseDetails_ = new QPushButton(QStringLiteral("暂停详情刷新"), this);
  cancelDetails_ = new QPushButton(QStringLiteral("取消详情刷新"), this);
  settings_ = new QPushButton(QStringLiteral("设置"), this);
  diagnostics_ = new QPushButton(QStringLiteral("复制诊断"), this);
  pauseDetails_->setEnabled(false);
  cancelDetails_->setEnabled(false);
  toolbar->addWidget(new QLabel(QStringLiteral("精灵查询"), this));
  toolbar->addWidget(search_, 1);
  toolbar->addWidget(refresh_);
  toolbar->addWidget(refreshDetails_);
  toolbar->addWidget(pauseDetails_);
  toolbar->addWidget(cancelDetails_);
  toolbar->addWidget(settings_);
  toolbar->addWidget(diagnostics_);
  root->addLayout(toolbar);

  auto* filterBar = new QHBoxLayout();
  attributeFilter_ = new QComboBox(this);
  jobFilter_ = new QComboBox(this);
  eraFilter_ = new QComboBox(this);
  auto* resetFilters = new QPushButton(QStringLiteral("重置查询/筛选"), this);
  attributeFilter_->setMinimumWidth(115);
  jobFilter_->setMinimumWidth(145);
  eraFilter_->setMinimumWidth(105);
  filterBar->addWidget(new QLabel(QStringLiteral("属性"), this));
  filterBar->addWidget(attributeFilter_);
  filterBar->addWidget(new QLabel(QStringLiteral("职业"), this));
  filterBar->addWidget(jobFilter_);
  filterBar->addWidget(new QLabel(QStringLiteral("时代"), this));
  filterBar->addWidget(eraFilter_);
  filterBar->addWidget(resetFilters);
  filterBar->addStretch(1);
  root->addLayout(filterBar);

  progress_ = new QLabel(QStringLiteral("仓库详情刷新：未启动"), this);
  progress_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(progress_);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  auto* listSplitter = new QSplitter(Qt::Vertical, splitter);

  auto* backpackPane = new QWidget(listSplitter);
  auto* backpackLayout = new QVBoxLayout(backpackPane);
  backpackLayout->setContentsMargins(0, 0, 0, 0);
  backpackLayout->setSpacing(3);
  auto* backpackControls = new QHBoxLayout();
  backpackTitle_ = new QLabel(QStringLiteral("背包"), backpackPane);
  backpackTitle_->setStyleSheet(QStringLiteral("font-weight:600;"));
  backpackSort_ = new QComboBox(backpackPane);
  backpackSortDirection_ = new QComboBox(backpackPane);
  addSortChoices(backpackSort_);
  backpackSortDirection_->addItem(QStringLiteral("正序"), true);
  backpackSortDirection_->addItem(QStringLiteral("倒序"), false);
  backpackControls->addWidget(backpackTitle_);
  backpackControls->addStretch(1);
  backpackControls->addWidget(new QLabel(QStringLiteral("背包排序"), backpackPane));
  backpackControls->addWidget(backpackSort_);
  backpackControls->addWidget(backpackSortDirection_);
  backpackLayout->addLayout(backpackControls);
  backpackTable_ = makeTable(backpackPane, true);
  backpackTable_->setObjectName(QStringLiteral("KQPetBackpackTable"));
  backpackTable_->setMinimumHeight(220);
  backpackLayout->addWidget(backpackTable_, 1);
  backpackPages_ = new QHBoxLayout();
  backpackPages_->setSpacing(4);
  backpackPages_->addStretch(1);
  backpackLayout->addLayout(backpackPages_);

  auto* warehousePane = new QWidget(listSplitter);
  auto* warehouseLayout = new QVBoxLayout(warehousePane);
  warehouseLayout->setContentsMargins(0, 0, 0, 0);
  warehouseLayout->setSpacing(3);
  auto* warehouseControls = new QHBoxLayout();
  auto* warehouseTitle = new QLabel(QStringLiteral("仓库"), warehousePane);
  warehouseTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
  warehouseSort_ = new QComboBox(warehousePane);
  warehouseSortDirection_ = new QComboBox(warehousePane);
  addSortChoices(warehouseSort_);
  warehouseSortDirection_->addItem(QStringLiteral("正序"), true);
  warehouseSortDirection_->addItem(QStringLiteral("倒序"), false);
  warehouseControls->addWidget(warehouseTitle);
  warehouseControls->addStretch(1);
  warehouseControls->addWidget(new QLabel(QStringLiteral("仓库排序"), warehousePane));
  warehouseControls->addWidget(warehouseSort_);
  warehouseControls->addWidget(warehouseSortDirection_);
  warehouseLayout->addLayout(warehouseControls);
  warehouseTabs_ = new QTabWidget(warehousePane);
  warehouseTable_ = makeTableView(warehouseTabs_);
  eliteWarehouseTable_ = makeTableView(warehouseTabs_);
  warehouseTable_->setObjectName(QStringLiteral("KQPetWarehouseTable"));
  eliteWarehouseTable_->setObjectName(QStringLiteral("KQPetEliteWarehouseTable"));
  warehouseModel_ = new PetTableModel(PetTableModel::Location::Warehouse,
                                      repository_, imageCache_, this);
  eliteWarehouseModel_ = new PetTableModel(PetTableModel::Location::Warehouse,
                                           repository_, imageCache_, this);
  warehouseProxy_ = new PetFilterProxyModel(this);
  eliteWarehouseProxy_ = new PetFilterProxyModel(this);
  warehouseProxy_->setSourceModel(warehouseModel_);
  eliteWarehouseProxy_->setSourceModel(eliteWarehouseModel_);
  warehouseTable_->setModel(warehouseProxy_);
  eliteWarehouseTable_->setModel(eliteWarehouseProxy_);
  configurePetTable(warehouseTable_, false);
  configurePetTable(eliteWarehouseTable_, false);
  warehouseTabs_->addTab(warehouseTable_, QStringLiteral("普通仓库"));
  warehouseTabs_->addTab(eliteWarehouseTable_, QStringLiteral("精英仓库"));
  warehouseLayout->addWidget(warehouseTabs_, 1);

  backpackTable_->setItemDelegateForColumn(
      0, new SearchHighlightDelegate(search_, backpackTable_));
  for (QTableView* table : {warehouseTable_, eliteWarehouseTable_})
    table->setItemDelegateForColumn(0, new SearchHighlightDelegate(search_, table));

  listSplitter->addWidget(backpackPane);
  listSplitter->addWidget(warehousePane);
  listSplitter->setChildrenCollapsible(false);
  listSplitter->setStretchFactor(0, 1);
  listSplitter->setStretchFactor(1, 1);
  listSplitter->setSizes({330, 330});

  detailTabs_ = new QTabWidget(splitter);
  detailView_ = new QTextBrowser(detailTabs_);
  detailView_->setOpenExternalLinks(false);
  detailView_->setPlaceholderText(QStringLiteral("请选择一只精灵"));
  rawTree_ = new QTreeWidget(detailTabs_);
  rawTree_->setColumnCount(2);
  rawTree_->setHeaderLabels({QStringLiteral("原始字段"), QStringLiteral("值")});
  rawTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  rawTree_->header()->setStretchLastSection(true);
  rawTree_->setAlternatingRowColors(true);
  detailTabs_->addTab(detailView_, QStringLiteral("精灵详情"));
  detailTabs_->addTab(rawTree_, QStringLiteral("原始数据"));
  splitter->addWidget(listSplitter);
  splitter->addWidget(detailTabs_);
  splitter->setChildrenCollapsible(false);
  splitter->setStretchFactor(0, 8);
  splitter->setStretchFactor(1, 4);
  splitter->setSizes({1020, 520});
  root->addWidget(splitter, 1);

  auto* moveBar = new QHBoxLayout();
  moveBar->addStretch(1);
  moveToWarehouse_ = new QPushButton(QStringLiteral("放入仓库"), this);
  moveToBackpack_ = new QPushButton(QStringLiteral("进入背包"), this);
  moveToWarehouse_->setToolTip(
      QStringLiteral("写入前后都会强制刷新；召唤、携带和神使关系可以正常移动"));
  moveToBackpack_->setToolTip(
      QStringLiteral("背包已满时需要手动选择一只背包精灵交换"));
  moveBar->addWidget(moveToWarehouse_);
  moveBar->addWidget(moveToBackpack_);
  root->addLayout(moveBar);

  status_ = new QLabel(this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(status_);

  connect(refresh_, &QPushButton::clicked, this, &PetWindow::listRefreshRequested);
  connect(refreshDetails_, &QPushButton::clicked, this,
          &PetWindow::warehouseDetailRefreshRequested);
  connect(pauseDetails_, &QPushButton::clicked, this, [this]() {
    if (detailBatchPaused_)
      emit warehouseDetailResumeRequested();
    else
      emit warehouseDetailPauseRequested();
  });
  connect(cancelDetails_, &QPushButton::clicked, this,
          &PetWindow::warehouseDetailCancelRequested);
  connect(settings_, &QPushButton::clicked, this, &PetWindow::settingsRequested);
  connect(diagnostics_, &QPushButton::clicked, this,
          &PetWindow::copyDiagnosticsRequested);
  connect(moveToWarehouse_, &QPushButton::clicked, this,
          &PetWindow::moveCurrentToWarehouse);
  connect(moveToBackpack_, &QPushButton::clicked, this,
          &PetWindow::moveCurrentToBackpack);
  searchDebounce_ = new QTimer(this);
  searchDebounce_->setSingleShot(true);
  searchDebounce_->setInterval(180);
  connect(searchDebounce_, &QTimer::timeout, this, &PetWindow::applyFilter);
  connect(search_, &QLineEdit::textChanged, this, [this]() {
    searchDebounce_->start();
    backpackTable_->viewport()->update();
    for (QTableView* table : {warehouseTable_, eliteWarehouseTable_})
      table->viewport()->update();
  });
  for (QComboBox* filter : {attributeFilter_, jobFilter_, eraFilter_})
    connect(filter, &QComboBox::currentIndexChanged, this, &PetWindow::applyFilter);
  for (QComboBox* sort : {backpackSort_, backpackSortDirection_, warehouseSort_,
                          warehouseSortDirection_})
    connect(sort, &QComboBox::currentIndexChanged, this, &PetWindow::applyFilter);
  connect(backpackSort_, &QComboBox::currentIndexChanged, this,
          &PetWindow::updateSortDirectionState);
  connect(warehouseSort_, &QComboBox::currentIndexChanged, this,
          &PetWindow::updateSortDirectionState);
  connect(resetFilters, &QPushButton::clicked, this, [this]() {
    const QSignalBlocker searchBlocker(search_);
    const QSignalBlocker attributeBlocker(attributeFilter_);
    const QSignalBlocker jobBlocker(jobFilter_);
    const QSignalBlocker eraBlocker(eraFilter_);
    search_->clear();
    attributeFilter_->setCurrentIndex(0);
    jobFilter_->setCurrentIndex(0);
    eraFilter_->setCurrentIndex(0);
    backpackPage_ = 0;
    refreshViews();
  });
  connect(backpackTable_, &QTableWidget::cellClicked, this, &PetWindow::selectBackpack);
  connect(warehouseTable_, &QTableView::clicked, this, &PetWindow::selectWarehouse);
  connect(eliteWarehouseTable_, &QTableView::clicked, this,
          &PetWindow::selectWarehouse);
  connect(repository_, &PetRepository::dataChanged, this, &PetWindow::rebuild);
  connect(repository_, &PetRepository::detailChanged, this, &PetWindow::updateCurrentDetail);
  connect(repository_, &PetRepository::statusChanged, this, &PetWindow::setStatus);
  connect(imageCache_, &PetImageCache::petImageReady, this, &PetWindow::updateCurrentImage);

  updateSortDirectionState();
  updateMoveButtons();
  rebuild();
  if (repository_->updatedAt().isValid()) {
    setStatus(QStringLiteral("已读取账号 %1 的本地缓存（%2），正在等待线上刷新。缓存：%3")
                  .arg(repository_->accountKey(),
                       repository_->updatedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                       repository_->cachePath()));
  } else {
    setStatus(QStringLiteral("尚无本地缓存；登录游戏后会自动读取。"));
  }
}

void PetWindow::setStatus(const QString& status) { status_->setText(status); }

void PetWindow::setListRefreshRunning(bool running) {
  listRefreshRunning_ = running;
  refresh_->setEnabled(!running && !moveRunning_);
  refresh_->setText(running ? QStringLiteral("列表刷新中……")
                            : QStringLiteral("刷新背包/仓库"));
  updateMoveButtons();
}

void PetWindow::setDetailProgress(bool running, bool paused, int completed, int total,
                                  int succeeded, int failed, qint64 currentInstanceId,
                                  int estimatedSeconds) {
  detailBatchPaused_ = paused;
  detailBatchRunning_ = running;
  refreshDetails_->setEnabled(!running && !moveRunning_);
  pauseDetails_->setEnabled(running && !moveRunning_);
  cancelDetails_->setEnabled(running && !moveRunning_);
  pauseDetails_->setText(paused ? QStringLiteral("继续详情刷新")
                                : QStringLiteral("暂停详情刷新"));
  if (!running && total <= 0) {
    progress_->setText(QStringLiteral("仓库详情刷新：未启动"));
    return;
  }
  const QString current = currentInstanceId > 0 ? QString::number(currentInstanceId)
                                                 : QStringLiteral("等待中");
  progress_->setText(QStringLiteral("仓库详情：%1/%2　成功 %3　失败 %4　当前 %5　预计剩余 %6 秒%7")
                         .arg(completed).arg(total).arg(succeeded).arg(failed)
                         .arg(current).arg(estimatedSeconds)
                         .arg(paused ? QStringLiteral("　【已暂停】") : QString()));
}

void PetWindow::setMoveRunning(bool running) {
  moveRunning_ = running;
  refresh_->setEnabled(!listRefreshRunning_ && !running);
  refreshDetails_->setEnabled(!detailBatchRunning_ && !running);
  pauseDetails_->setEnabled(detailBatchRunning_ && !running);
  cancelDetails_->setEnabled(detailBatchRunning_ && !running);
  settings_->setEnabled(!running);
  updateMoveButtons();
}

QString PetWindow::displayName(const QJsonObject& pet) const {
  const QString custom = pet.value(QStringLiteral("customName")).toString();
  if (!custom.isEmpty())
    return custom;
  const QString name = pet.value(QStringLiteral("n")).toString();
  return name.isEmpty() ? QStringLiteral("未命名") : name;
}

void PetWindow::fillTable(QTableWidget* table, const QList<QJsonObject>& pets,
                          const QString& location) {
  const QSignalBlocker blocker(table);
  table->setUpdatesEnabled(false);
  table->setSortingEnabled(false);
  table->setRowCount(pets.size());
  int row = 0;
  for (const QJsonObject& pet : pets) fillRow(table, row++, pet, location);
  const QFontMetrics metrics(table->font());
  int nameWidth = metrics.horizontalAdvance(QStringLiteral("名称")) + 48;
  int jobWidth = metrics.horizontalAdvance(QStringLiteral("职业")) + 24;
  int powerWidth = metrics.horizontalAdvance(QStringLiteral("战斗力 / 极限战斗力")) + 24;
  for (const QJsonObject& pet : pets) {
    const QJsonObject metadata = PetDetailCatalog::instance().pet(petRaceId(pet));
    nameWidth = qMax(nameWidth, metrics.horizontalAdvance(displayName(pet)) + 48);
    jobWidth = qMax(jobWidth, metrics.horizontalAdvance(PetDetailCatalog::instance().jobs(
                                     metadata.value(QStringLiteral("jobs")).toString())) + 24);
    powerWidth = qMax(powerWidth, metrics.horizontalAdvance(battlePowerTableText(
                                         battlePowerState(pet, PetDetailCatalog::instance()))) + 24);
  }
  table->setColumnWidth(0, qBound(205, nameWidth, 285));
  table->setColumnWidth(2, qBound(105, jobWidth, 175));
  table->setColumnWidth(5, qBound(185, powerWidth, 225));
  table->setUpdatesEnabled(true);
  table->viewport()->update();
}

void PetWindow::fillRow(QTableWidget* table, int row, const QJsonObject& pet,
                        const QString& location) {
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const qint64 id = petInstanceId(pet);
  const int raceId = petRaceId(pet);
  const QJsonObject metadata = catalog.metadataFor(pet);
  const QString attributesSequence = metadata.value(QStringLiteral("attributes")).toString();
  const BattlePowerState power = battlePowerState(pet, catalog);
  QStringList values = {
      displayName(pet), catalog.resolvedAttributes(pet),
      catalog.resolvedJobs(pet),
      eraForPet(pet), pet.value(QStringLiteral("lv")).toVariant().toString(),
      battlePowerTableText(power)};
  if (location == QStringLiteral("backpack"))
    values.append(PetMovePolicy::deploymentText(pet));
  values.append(gridPositionText(pet, location));
  const QDateTime cachedAt = repository_->detailSavedAt(id);
  for (int column = 0; column < values.size(); ++column) {
    QTableWidgetItem* item = table->item(row, column);
    if (!item) {
      item = new QTableWidgetItem();
      table->setItem(row, column, item);
    }
    item->setText(values.at(column));
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, QStringLiteral("%1 %2").arg(id).arg(raceId));
    if (column == 0) {
      const QString original = catalog.resolvedOriginalName(pet);
      item->setToolTip(original.isEmpty() || original == values.at(0)
                           ? values.at(0)
                           : QStringLiteral("皮肤/当前名称：%1\n原名：%2")
                                 .arg(values.at(0), original));
    }
    if (column == 1) item->setIcon(imageCache_->attributeIcon(attributesSequence));
    if (location == QStringLiteral("backpack") && column == 6) {
      QFont font = item->font();
      if (values.at(column) == QStringLiteral("是")) {
        font.setBold(true);
        item->setFont(font);
        item->setForeground(QBrush(QColor(220, 38, 38)));
      } else {
        font.setBold(false);
        item->setFont(font);
        item->setForeground(table->palette().brush(QPalette::Text));
      }
    }
    if (column == 5) {
      if (location == QStringLiteral("warehouse")) {
        item->setToolTip(cachedAt.isValid()
                             ? QStringLiteral("战力来自本地详情缓存：%1")
                                   .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                             : QStringLiteral("尚无该实例ID的本地详情缓存"));
      } else {
        item->setToolTip(QStringLiteral("战力来自本次背包完整数据"));
      }
    }
  }
}

void PetWindow::updateWarehouseRow(qint64 instanceId) {
  const QJsonObject pet = repository_->warehousePet(instanceId);
  if (pet.isEmpty()) return;
  if (warehouseModel_->updatePet(pet)) resizeModelViewColumns(warehouseTable_);
  if (eliteWarehouseModel_->updatePet(pet))
    resizeModelViewColumns(eliteWarehouseTable_);
}

void PetWindow::rebuild() {
  refreshViews(true);
  if (currentId_ > 0)
    showDetail(repository_->detailFor(currentId_));
}

void PetWindow::applyFilter() {
  backpackPage_ = 0;
  updateSortDirectionState();
  refreshViews();
}

void PetWindow::updateSortDirectionState() {
  backpackSortDirection_->setEnabled(
      backpackSort_->currentData().toInt() != static_cast<int>(PetSortMode::Default));
  warehouseSortDirection_->setEnabled(
      warehouseSort_->currentData().toInt() != static_cast<int>(PetSortMode::Default));
}

bool PetWindow::matchesCurrentQueryAndFilters(const QJsonObject& pet) const {
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const int raceId = petRaceId(pet);
  const QStringList names = {
      displayName(pet), pet.value(QStringLiteral("n")).toString(),
      catalog.petName(raceId), catalog.resolvedOriginalName(pet)};
  const QStringList identifiers = {QString::number(petInstanceId(pet)),
                                   QString::number(raceId)};
  if (!petQueryMatches(search_->text(), names, identifiers)) return false;

  const QString selectedAttribute = attributeFilter_->currentText();
  if (attributeFilter_->currentIndex() > 0) {
    if (!categoryParts(catalog.resolvedAttributes(pet)).contains(selectedAttribute))
      return false;
  }

  const QString selectedJob = jobFilter_->currentText();
  if (jobFilter_->currentIndex() > 0) {
    if (!categoryParts(catalog.resolvedJobs(pet)).contains(selectedJob))
      return false;
  }

  if (eraFilter_->currentIndex() > 0 && eraForPet(pet) != eraFilter_->currentText())
    return false;
  return true;
}

QList<QJsonObject> PetWindow::filteredAndSorted(const QList<QJsonObject>& pets,
                                                bool backpack) const {
  QList<QJsonObject> result;
  result.reserve(pets.size());
  for (const QJsonObject& pet : pets)
    if (matchesCurrentQueryAndFilters(pet)) result.append(pet);

  const QComboBox* sortBox = backpack ? backpackSort_ : warehouseSort_;
  const QComboBox* directionBox =
      backpack ? backpackSortDirection_ : warehouseSortDirection_;
  const auto mode = static_cast<PetSortMode>(sortBox->currentData().toInt());
  const bool ascending = mode == PetSortMode::Default
                             ? true
                             : directionBox->currentData().toBool();

  auto numericKey = [mode](const QJsonObject& pet, bool* valid) -> qint64 {
    *valid = true;
    switch (mode) {
      case PetSortMode::BattlePower:
        *valid = pet.contains(QStringLiteral("zdl"));
        return pet.value(QStringLiteral("zdl")).toVariant().toLongLong();
      case PetSortMode::ExtremePower:
        *valid = pet.contains(QStringLiteral("xzdl"));
        return pet.value(QStringLiteral("xzdl")).toVariant().toLongLong();
      case PetSortMode::CatalogSequence:
        *valid = petRaceId(pet) > 0;
        return petRaceId(pet);
      case PetSortMode::ObtainedAt: {
        const QJsonValue value = pet.value(QStringLiteral("gd"));
        qint64 timestamp = value.toVariant().toLongLong();
        if (timestamp <= 0 && value.isString())
          timestamp = QDateTime::fromString(value.toString(), Qt::ISODate).toMSecsSinceEpoch();
        *valid = timestamp > 0;
        return timestamp;
      }
      case PetSortMode::Default:
        return pet.value(QStringLiteral("_position")).toInt(INT_MAX);
    }
    return 0;
  };

  std::stable_sort(result.begin(), result.end(), [&](const QJsonObject& left,
                                                     const QJsonObject& right) {
    bool leftValid = false;
    bool rightValid = false;
    const qint64 leftKey = numericKey(left, &leftValid);
    const qint64 rightKey = numericKey(right, &rightValid);
    if (leftValid != rightValid) return leftValid;
    if (leftValid && leftKey != rightKey)
      return ascending ? leftKey < rightKey : leftKey > rightKey;
    const int leftPosition = left.value(QStringLiteral("_position")).toInt(INT_MAX);
    const int rightPosition = right.value(QStringLiteral("_position")).toInt(INT_MAX);
    if (leftPosition != rightPosition) return leftPosition < rightPosition;
    return petInstanceId(left) < petInstanceId(right);
  });
  return result;
}

void PetWindow::rebuildFilterChoices() {
  QSet<QString> attributes;
  QSet<QString> jobs;
  QSet<QString> dualCareerJobs;
  QSet<QString> eras;
  QList<QJsonObject> all = repository_->backpackPets();
  all.append(repository_->warehousePets());
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  for (const QJsonObject& pet : all) {
    for (const QString& value : categoryParts(catalog.resolvedAttributes(pet)))
      attributes.insert(value);
    const QStringList petJobs = categoryParts(catalog.resolvedJobs(pet));
    for (const QString& value : petJobs)
      jobs.insert(value);
    if (petJobs.size() > 1)
      for (const QString& value : petJobs) dualCareerJobs.insert(value);
    eras.insert(eraForPet(pet));
  }

  auto refill = [](QComboBox* box, const QStringList& values) {
    const QString selected = box->currentIndex() > 0 ? box->currentText() : QString();
    const QSignalBlocker blocker(box);
    box->clear();
    box->addItem(QStringLiteral("全部"));
    box->addItems(values);
    const int selectedIndex = selected.isEmpty() ? 0 : box->findText(selected);
    box->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
  };

  QStringList orderedAttributes = attributes.values();
  std::sort(orderedAttributes.begin(), orderedAttributes.end(),
            [](const QString& left, const QString& right) {
              const bool leftDivine = left.startsWith(QStringLiteral("神"));
              const bool rightDivine = right.startsWith(QStringLiteral("神"));
              if (leftDivine != rightDivine) return leftDivine;
              return QString::localeAwareCompare(left, right) < 0;
            });

  static const QStringList preferredJobs = {
      QStringLiteral("神速"), QStringLiteral("神平衡"), QStringLiteral("神盾"),
      QStringLiteral("神攻"), QStringLiteral("元素师"), QStringLiteral("神召唤师"),
      QStringLiteral("通灵师"), QStringLiteral("赋能师"), QStringLiteral("幻元师")};
  QStringList orderedJobs = jobs.values();
  std::sort(orderedJobs.begin(), orderedJobs.end(),
            [&](const QString& left, const QString& right) {
              const int leftPreferred = preferredJobs.indexOf(left);
              const int rightPreferred = preferredJobs.indexOf(right);
              if ((leftPreferred >= 0) != (rightPreferred >= 0)) return leftPreferred >= 0;
              if (leftPreferred >= 0 && leftPreferred != rightPreferred)
                return leftPreferred < rightPreferred;
              const bool leftDual = dualCareerJobs.contains(left);
              const bool rightDual = dualCareerJobs.contains(right);
              if (leftDual != rightDual) return leftDual;
              return QString::localeAwareCompare(left, right) < 0;
            });

  QStringList orderedEras = eras.values();
  std::sort(orderedEras.begin(), orderedEras.end(),
            [](const QString& left, const QString& right) {
              if ((left == QStringLiteral("其它")) != (right == QStringLiteral("其它")))
                return right == QStringLiteral("其它");
              return QString::localeAwareCompare(left, right) < 0;
            });
  refill(attributeFilter_, orderedAttributes);
  refill(jobFilter_, orderedJobs);
  refill(eraFilter_, orderedEras);
}

void PetWindow::refreshViews(bool rebuildChoices) {
  if (rebuildChoices) rebuildFilterChoices();
  const QList<QJsonObject> allBackpack = repository_->backpackPets();
  QList<QJsonObject> ordinaryAll;
  QList<QJsonObject> eliteAll;
  for (const QJsonObject& pet : repository_->warehousePets()) {
    if (pet.value(QStringLiteral("_warehouseGroup")).toString() ==
        QStringLiteral("elite"))
      eliteAll.append(pet);
    else
      ordinaryAll.append(pet);
  }

  const QList<QJsonObject> backpack = filteredAndSorted(allBackpack, true);
  const int pageCount = qMax(1, (backpack.size() + 11) / 12);
  backpackPage_ = qBound(0, backpackPage_, pageCount - 1);
  fillTable(backpackTable_, backpack.mid(backpackPage_ * 12, 12),
            QStringLiteral("backpack"));

  const QString attribute = attributeFilter_->currentIndex() > 0
                                ? attributeFilter_->currentText()
                                : QString();
  const QString job = jobFilter_->currentIndex() > 0 ? jobFilter_->currentText()
                                                      : QString();
  const QString era = eraFilter_->currentIndex() > 0 ? eraFilter_->currentText()
                                                      : QString();
  const auto warehouseSortMode = static_cast<PetFilterProxyModel::SortMode>(
      warehouseSort_->currentData().toInt());
  const bool warehouseAscending =
      warehouseSortMode == PetFilterProxyModel::SortMode::Default
          ? true
          : warehouseSortDirection_->currentData().toBool();
  for (PetFilterProxyModel* proxy : {warehouseProxy_, eliteWarehouseProxy_}) {
    proxy->setQuery(search_->text());
    proxy->setAttributeFilter(attribute);
    proxy->setJobFilter(job);
    proxy->setEraFilter(era);
    proxy->setSortMode(warehouseSortMode, warehouseAscending);
  }
  warehouseModel_->setPets(ordinaryAll);
  eliteWarehouseModel_->setPets(eliteAll);
  resizeModelViewColumns(warehouseTable_);
  resizeModelViewColumns(eliteWarehouseTable_);
  rebuildPageButtons(pageCount);

  backpackTitle_->setText(
      QStringLiteral("背包：%1 / %2　第 %3/%4 页")
          .arg(backpack.size()).arg(allBackpack.size()).arg(backpackPage_ + 1).arg(pageCount));
  warehouseTabs_->setTabText(
      0, QStringLiteral("普通仓库（%1 / %2）")
             .arg(warehouseProxy_->rowCount()).arg(ordinaryAll.size()));
  warehouseTabs_->setTabText(
      1, QStringLiteral("精英仓库（%1 / %2）")
             .arg(eliteWarehouseProxy_->rowCount()).arg(eliteAll.size()));
}

void PetWindow::rebuildPageButtons(int pageCount) {
  while (QLayoutItem* item = backpackPages_->takeAt(0)) {
    if (item->widget()) item->widget()->deleteLater();
    delete item;
  }
  backpackPages_->addStretch(1);
  for (int page = 0; page < pageCount; ++page) {
    auto* button = new QPushButton(QString::number(page + 1), this);
    button->setFixedSize(32, 24);
    button->setCheckable(true);
    button->setChecked(page == backpackPage_);
    connect(button, &QPushButton::clicked, this, [this, page]() { setBackpackPage(page); });
    backpackPages_->addWidget(button);
  }
  backpackPages_->addStretch(1);
}

void PetWindow::setBackpackPage(int page) {
  if (page == backpackPage_) return;
  backpackPage_ = qMax(0, page);
  refreshViews();
}

qint64 PetWindow::rowId(QTableWidget* table, int row) {
  QTableWidgetItem* item = table->item(row, 0);
  return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void PetWindow::updateMoveButtons() {
  const bool selected = currentId_ > 0 && !currentLocation_.isEmpty();
  const bool available = selected && !moveRunning_ && !listRefreshRunning_;
  moveToWarehouse_->setEnabled(available &&
                               currentLocation_ == QStringLiteral("backpack"));
  moveToBackpack_->setEnabled(available &&
                              currentLocation_ == QStringLiteral("warehouse"));
  moveToWarehouse_->setText(moveRunning_ ? QStringLiteral("移动处理中……")
                                         : QStringLiteral("放入仓库"));
  moveToBackpack_->setText(moveRunning_ ? QStringLiteral("移动处理中……")
                                        : QStringLiteral("进入背包"));
}

void PetWindow::moveCurrentToWarehouse() {
  if (currentId_ <= 0 || currentLocation_ != QStringLiteral("backpack"))
    return;
  emit moveToWarehouseRequested(currentId_);
}

void PetWindow::moveCurrentToBackpack() {
  if (currentId_ <= 0 || currentLocation_ != QStringLiteral("warehouse"))
    return;
  emit moveToBackpackRequested(currentId_);
}

void PetWindow::requestReplacement(
    qint64 incomingInstanceId, const QList<qint64>& eligibleBackpackIds) {
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("背包已满 · 选择交换精灵"));
  dialog.resize(760, 460);
  auto* layout = new QVBoxLayout(&dialog);
  auto* explanation = new QLabel(
      QStringLiteral("选择一只背包精灵与仓库实例 %1 交换。\n"
                     "被选择的精灵会进入仓库，不会被删除。")
          .arg(incomingInstanceId),
      &dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  auto* table = new QTableWidget(&dialog);
  table->setColumnCount(5);
  table->setHorizontalHeaderLabels(
      {QStringLiteral("名称"), QStringLiteral("实例 ID"), QStringLiteral("等级"),
       QStringLiteral("战斗力 / 极限战斗力"), QStringLiteral("位置")});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setRowCount(eligibleBackpackIds.size());
  int row = 0;
  for (qint64 id : eligibleBackpackIds) {
    const QJsonObject pet = repository_->backpackPet(id);
    const BattlePowerState power = battlePowerState(pet, PetDetailCatalog::instance());
    const QStringList values = {
        displayName(pet), QString::number(id),
        pet.value(QStringLiteral("lv")).toVariant().toString(),
        battlePowerTableText(power), gridPositionText(pet, QStringLiteral("backpack"))};
    for (int column = 0; column < values.size(); ++column) {
      auto* item = new QTableWidgetItem(values.at(column));
      item->setData(Qt::UserRole, id);
      table->setItem(row, column, item);
    }
    ++row;
  }
  table->resizeColumnsToContents();
  layout->addWidget(table, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                            QDialogButtonBox::Cancel,
                                        &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认交换"));
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
  buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  connect(table, &QTableWidget::itemSelectionChanged, &dialog, [table, buttons]() {
    buttons->button(QDialogButtonBox::Ok)->setEnabled(
        !table->selectionModel()->selectedRows().isEmpty());
  });
  connect(table, &QTableWidget::cellDoubleClicked, &dialog,
          [&dialog](int, int) { dialog.accept(); });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted) {
    emit moveCancelRequested();
    return;
  }
  const QModelIndexList selectedRows = table->selectionModel()->selectedRows();
  if (selectedRows.isEmpty()) {
    emit moveCancelRequested();
    return;
  }
  const qint64 outgoingId =
      table->item(selectedRows.constFirst().row(), 0)->data(Qt::UserRole).toLongLong();
  if (outgoingId > 0) emit moveReplacementChosen(outgoingId);
}

void PetWindow::selectBackpack(int row, int) {
  currentId_ = rowId(backpackTable_, row);
  currentLocation_ = QStringLiteral("backpack");
  showDetail(repository_->detailFor(currentId_));
  updateMoveButtons();
}

void PetWindow::selectWarehouse(const QModelIndex& index) {
  currentId_ = index.data(PetTableModel::InstanceIdRole).toLongLong();
  currentLocation_ = QStringLiteral("warehouse");
  showDetail(repository_->detailFor(currentId_));
  updateMoveButtons();
  if (currentId_ > 0) {
    const QDateTime cachedAt = repository_->detailSavedAt(currentId_);
    setStatus(cachedAt.isValid()
                  ? QStringLiteral("已显示实例 %1 的本地缓存（%2），正在获取最新详情……")
                        .arg(currentId_)
                        .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                  : QStringLiteral("实例 %1 尚无详情缓存，正在获取线上详情……")
                        .arg(currentId_));
    emit detailRequested(currentId_);
  }
}

void PetWindow::updateCurrentDetail(qint64 instanceId) {
  updateWarehouseRow(instanceId);
  if (instanceId == currentId_)
    showDetail(repository_->detailFor(instanceId));
}

void PetWindow::updateCurrentImage(const QString& visualKey, const QString&) {
  if (visualKey == currentVisualKey_ && currentId_ > 0)
    showDetail(repository_->detailFor(currentId_));
}

void PetWindow::addJsonValue(const QString& key, const QJsonValue& value,
                             QTreeWidgetItem* parent) {
  auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(rawTree_);
  item->setText(0, friendlyKey(key));
  item->setText(1, valueText(value));
  item->setToolTip(0, key);
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
      addJsonValue(iterator.key(), iterator.value(), item);
  } else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (int index = 0; index < array.size(); ++index)
      addJsonValue(QStringLiteral("[%1]").arg(index), array.at(index), item);
  }
}

QString PetWindow::renderCachedDetailHtml(const QJsonObject& pet,
                                          const PetRepository* repository,
                                          const QString& imagePath,
                                          bool fetchingLatest) {
  const PetDetailViewModel model =
      PetDetailAnalyzer::analyze(pet, repository, imagePath, fetchingLatest);
  return PetDetailRenderer::render(model);
}

void PetWindow::showDetail(const QJsonObject& pet) {
  rawTree_->clear();
  if (pet.isEmpty()) {
    currentLocation_.clear();
    updateMoveButtons();
    currentVisualKey_.clear();
    detailView_->setHtml(QStringLiteral("<p style='color:#6b7280'>请选择一只精灵</p>"));
    auto* item = new QTreeWidgetItem(rawTree_);
    item->setText(0, QStringLiteral("提示"));
    item->setText(1, QStringLiteral("请选择一只精灵"));
    return;
  }

  currentLocation_ = pet.value(QStringLiteral("_location")).toString();
  updateMoveButtons();

  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const int raceId = petRaceId(pet);
  currentRaceId_ = raceId;
  currentVisualKey_ = petVisualKey(pet);
  QString name = pet.value(QStringLiteral("n")).toString();
  if (name.isEmpty())
    name = catalog.petName(raceId);
  const QString originalName = catalog.resolvedOriginalName(pet);
  const QString imagePath = imageCache_->ensurePetImage(
      pet, {name, displayName(pet), catalog.petName(raceId), originalName});
  const PetDetailViewModel model =
      PetDetailAnalyzer::analyze(pet, repository_, imagePath, false);
  PetDetailRenderOptions renderOptions;
  renderOptions.visualMismatchRefreshPending = true;
  detailView_->setHtml(PetDetailRenderer::render(model, renderOptions));
  detailView_->verticalScrollBar()->setValue(0);

  const QStringList priority = {QStringLiteral("id"), QStringLiteral("n"),
                                QStringLiteral("customName"), QStringLiteral("r"),
                                QStringLiteral("ri"), QStringLiteral("lv"),
                                QStringLiteral("fr"), QStringLiteral("pr")};
  QSet<QString> inserted;
  for (const QString& key : priority) {
    if (pet.contains(key)) {
      addJsonValue(key, pet.value(key), nullptr);
      inserted.insert(key);
    }
  }
  for (auto iterator = pet.begin(); iterator != pet.end(); ++iterator) {
    if (!inserted.contains(iterator.key()))
      addJsonValue(iterator.key(), iterator.value(), nullptr);
  }
  rawTree_->expandToDepth(0);
}
