#include "pet_window.h"

#include "build_info.h"

#include "prepared_pet_detail_renderer.h"
#include "pet_power_analysis_renderer.h"
#include "../domain/pet_metadata_view.h"
#include "pet_raw_data_tree.h"
#include "pet_identity.h"
#include "pet_image_cache.h"
#include "pet_move_policy.h"
#include "inventory_read_view.h"
#include "pet_search.h"
#include "pet_filter_proxy_model.h"
#include "pet_table_model.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QCheckBox>

#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
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

class SearchHighlightDelegate final : public QStyledItemDelegate {
public:
  SearchHighlightDelegate(const QLineEdit* search, QObject* parent)
      : QStyledItemDelegate(parent), search_(search) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    if (index.column() > PetTableModel::OriginalNameColumn || !search_ || search_->text().trimmed().isEmpty()) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    QStyleOptionViewItem styled(option);
    initStyleOption(&styled, index);
    const QString text = styled.text;
    if (cachedQueryText_ != search_->text()) {
      cachedQueryText_ = search_->text();
      cachedQuery_ = preparePetSearchQuery(cachedQueryText_);
    }
    const QVariant cached = index.data(PetTableModel::NameSearchIndexRole);
    const QPair<int, int> range = cached.canConvert<PetSearchText>()
        ? petQueryHighlightRange(cachedQuery_, cached.value<PetSearchText>())
        : petQueryHighlightRange(search_->text(), text);
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
  mutable QString cachedQueryText_;
  mutable PetSearchQuery cachedQuery_;
};

void resizeModelViewColumns(QTableView* table) {
  if (!table || !table->model()) return;
  const int count = table->model()->columnCount();
  const int available = qMax(0, table->viewport()->width());
  if (!available || (count != 8 && count != 9)) return;
  // Fit the entire table to its own viewport. Long names keep their full text
  // in the tooltip, while every field and heading stays on screen.
  const int backpackWeights[]{176,176,76,110,48,42,128,56,78};
  const int warehouseWeights[]{192,192,76,110,48,42,128,78};
  const int* weights = count == 9 ? backpackWeights : warehouseWeights;
  int totalWeight = 0;
  for (int column=0; column<count; ++column) totalWeight += weights[column];
  const int powerColumn = PetTableModel::BattlePowerColumn;
  const int powerWidth = qMin(qMax(available*weights[powerColumn]/totalWeight,
      table->fontMetrics().horizontalAdvance(QStringLiteral("99999 / 99999（至高）")) + 12),
      available/3);
  const int otherWidth = available-powerWidth;
  const int otherWeight = totalWeight-weights[powerColumn];
  int remaining = available;
  for (int column=0; column<count; ++column) {
    const int width = column == count-1 ? remaining : column == powerColumn ? powerWidth
        : otherWidth*weights[column]/otherWeight;
    table->setColumnWidth(column, width);
    remaining -= width;
  }
}

class PetTableWidthFitter final : public QObject {
public:
  explicit PetTableWidthFitter(QTableView* table) : QObject(table), table_(table) {
    table->viewport()->installEventFilter(this);
  }
protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
      resizeModelViewColumns(table_);
    return false;
  }
private:
  QTableView* table_;
};

void configurePetTable(QTableView* table, bool) {
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setStyleSheet(QStringLiteral(
      "QTableView::item:selected { background-color:#2563eb; color:#ffffff; }"
      "QTableView::item:selected:!active { background-color:#3b82f6; color:#ffffff; }"
      "QHeaderView::section { padding:4px 2px; }"));
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideRight);
  table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  table->setIconSize(QSize(20, 20));
  table->verticalHeader()->setVisible(false);
  table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table->verticalHeader()->setDefaultSectionSize(28);
  table->horizontalHeader()->setStretchLastSection(false);
  table->horizontalHeader()->setMinimumSectionSize(1);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table->setMinimumWidth(0);
  new PetTableWidthFitter(table);
  resizeModelViewColumns(table);
}

QTableWidget* makeTable(QWidget* parent, bool backpack = false) {
  auto* table = new QTableWidget(parent);
  QStringList headers = {QStringLiteral("显示名称"), QStringLiteral("精灵原名"), QStringLiteral("属性"),
                         QStringLiteral("职业"), QStringLiteral("时代"),
                         QStringLiteral("等级"),
                         QStringLiteral("战斗力 /\n极限战斗力")};
  if (backpack) headers.append(QStringLiteral("是否\n出战"));
  headers.append(QStringLiteral("位置"));
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  configurePetTable(table, backpack);
  return table;
}

QTableView* makeTableView(QWidget* parent) {
  return new QTableView(parent);
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

PetWindow::PetWindow(InventoryReadView* repository, QWidget* parent, PetImageCache* sharedImages)
    : QDialog(parent), repository_(repository) {
  imageCache_ = sharedImages ? sharedImages : new PetImageCache(repository_->dataRoot(), this);
  setObjectName(QStringLiteral("KQPetInventoryWindow"));
  setWindowTitle(QStringLiteral("原版氪奇 · 精灵背包与仓库 · %1")
                     .arg(BuildInfo::displayVersion()));
  resize(1560, 820);
  setMinimumSize(1280, 700);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* root = new QVBoxLayout(this);
  root->setSpacing(5);
  toolbarLayout_ = new QGridLayout();
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(QStringLiteral("搜索名称、实例 ID 或种族 ID"));
  filterToggle_ = new QPushButton(QStringLiteral("筛选 / 刷新范围"), this);
  filterToggle_->setObjectName(QStringLiteral("KQPetFilterToggle"));
  filterToggle_->setCheckable(true);
  refresh_ = new QPushButton(QStringLiteral("刷新背包/仓库"), this);
  refreshDetails_ = new QPushButton(QStringLiteral("刷新仓库详情"), this);
  refreshDetails_->setObjectName(QStringLiteral("KQPetRefreshWarehouseDetails"));
  pauseDetails_ = new QPushButton(QStringLiteral("暂停详情刷新"), this);
  cancelDetails_ = new QPushButton(QStringLiteral("取消详情刷新"), this);
  settings_ = new QPushButton(QStringLiteral("设置"), this);
  diagnostics_ = new QPushButton(QStringLiteral("复制诊断"), this);
  pauseDetails_->setEnabled(false);
  cancelDetails_->setEnabled(false);
  pauseDetails_->hide();
  cancelDetails_->hide();
  toolbarWidgets_ = {new QLabel(QStringLiteral("精灵查询"), this), search_, refresh_,
                     refreshDetails_, pauseDetails_, cancelDetails_, settings_, diagnostics_};
  for (int column = 0; column < toolbarWidgets_.size(); ++column)
    toolbarLayout_->addWidget(toolbarWidgets_[column], 0, column);
  toolbarLayout_->setColumnStretch(1, 1);
  toolbarLayout_->addWidget(filterToggle_, 0, toolbarWidgets_.size());
  toolbarWidgets_[0]->hide();
  root->addLayout(toolbarLayout_);

  filterPanel_ = new QWidget(this);
  filterPanel_->setObjectName(QStringLiteral("KQPetFilterPanel"));
  auto* filters = new QVBoxLayout(filterPanel_);
  filters->setContentsMargins(0, 0, 0, 0);
  filters->setSpacing(4);
  filterLayout_ = new QGridLayout();
  attributeFilter_ = new QComboBox(this);
  jobFilter_ = new QComboBox(this);
  eraFilter_ = new QComboBox(this);
  auto* resetFilters = new QPushButton(QStringLiteral("重置查询/筛选"), this);
  attributeFilter_->setMinimumWidth(115);
  jobFilter_->setMinimumWidth(145);
  eraFilter_->setMinimumWidth(105);
  filterWidgets_ = {new QLabel(QStringLiteral("属性"), this), attributeFilter_,
                    new QLabel(QStringLiteral("职业"), this), jobFilter_,
                    new QLabel(QStringLiteral("时代"), this), eraFilter_, resetFilters};
  for (int column = 0; column < filterWidgets_.size(); ++column)
    filterLayout_->addWidget(filterWidgets_[column], 0, column);
  filterLayout_->setColumnStretch(7, 1);
  filters->addLayout(filterLayout_);

  detailEraFilters_ = new QWidget(this);
  detailEraFilters_->setObjectName(QStringLiteral("KQPetDetailEraFilters"));
  auto* detailEraLayout = new QHBoxLayout(detailEraFilters_);
  detailEraLayout->setContentsMargins(0, 0, 0, 0);
  detailEraLayout->addWidget(new QLabel(QStringLiteral("详情刷新范围"), detailEraFilters_));
  const QStringList detailEras{QStringLiteral("灵初"), QStringLiteral("神运"), QStringLiteral("星迹"),
                               QStringLiteral("启元"), QStringLiteral("其他")};
  for (int index = 0; index < detailEras.size(); ++index) {
    auto* check = new QCheckBox(detailEras[index], detailEraFilters_);
    check->setObjectName(QStringLiteral("KQPetDetailEra%1").arg(index));
    check->setChecked(true);
    check->setToolTip(QStringLiteral("启动时固定本轮精灵名单；暂停和继续不会改变范围"));
    detailEraChecks_.append(check);
    detailEraLayout->addWidget(check);
  }
  detailEraLayout->addStretch(1);
  filters->addWidget(detailEraFilters_);
  root->addWidget(filterPanel_);
  filterPanel_->hide();
  connect(filterToggle_, &QPushButton::toggled, this, [this](bool expanded) {
    filterPanel_->setVisible(expanded && (!workbenchCompact_ || !compactDetails_));
    QTimer::singleShot(0, this, &PetWindow::fitInventoryGeometry);
  });

  progress_ = new QLabel(QStringLiteral("仓库详情刷新：未启动"), this);
  progress_->setObjectName(QStringLiteral("KQPetInlineProgress"));
  progress_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(progress_);
  detailToggle_ = new QPushButton(QStringLiteral("查看精灵详情"), this);
  detailToggle_->setObjectName(QStringLiteral("KQPetCompactDetailToggle"));
  detailToggle_->setMinimumHeight(28);
  detailToggle_->hide();
  root->addWidget(detailToggle_);
  connect(detailToggle_, &QPushButton::clicked, this, [this] {
    compactDetails_ = !compactDetails_;
    updateWorkbenchPanels();
  });

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  auto* listSplitter = new QSplitter(Qt::Vertical, splitter);
  contentSplitter_ = splitter;
  inventorySplitter_ = listSplitter;
  splitter->setObjectName(QStringLiteral("KQPetContentSplitter"));
  listSplitter->setObjectName(QStringLiteral("KQPetInventorySplitter"));

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
  backpackTable_->setMinimumHeight(12 * 28 + 42);
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
  backpackModel_ = new PetTableModel(PetTableModel::Location::Backpack,
                                     repository_, imageCache_, this);
  backpackProxy_ = new PetFilterProxyModel(this);
  backpackProxy_->setSourceModel(backpackModel_);
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

  for (QTableView* table : {static_cast<QTableView*>(backpackTable_), warehouseTable_, eliteWarehouseTable_})
    for (int column : {PetTableModel::DisplayNameColumn, PetTableModel::OriginalNameColumn})
      table->setItemDelegateForColumn(column, new SearchHighlightDelegate(search_, table));

  listSplitter->addWidget(backpackPane);
  listSplitter->addWidget(warehousePane);
  listSplitter->setChildrenCollapsible(false);
  listSplitter->setStretchFactor(0, 0);
  listSplitter->setStretchFactor(1, 1);
  listSplitter->setSizes({440, 260});

  detailTabs_ = new QTabWidget(splitter);
  detailView_ = new PetImageBrowser(detailTabs_);
  detailView_->setObjectName(QStringLiteral("KQPetPreparedDetail"));
  detailView_->setOpenLinks(false);
  detailView_->setOpenExternalLinks(false);
  detailView_->setPlaceholderText(QStringLiteral("请选择一只精灵"));
  connect(detailView_, &PetImageBrowser::imageRetryRequested, this, [this] {
    if (currentId_ <= 0) return;
    const QJsonObject pet = repository_->detailFor(currentId_);
    const PetMetadataView catalog(repository_->metadataSnapshot());
    imageCache_->ensurePetImage(pet, {pet.value(QStringLiteral("n")).toString(), displayName(pet),
        catalog.petName(petRaceId(pet)), catalog.resolvedOriginalName(pet)}, detailView_->devicePixelRatioF(), true);
  });
  connect(detailView_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    DetailSection section; int page = 0;
    if (repository_ && PreparedPetDetailRenderer::pageLink(url, &section, &page)) {
      renderedDetailId_ = 0;
      repository_->requestDetailPage(0, section, page);
    }
  });
  rawTree_ = new PetRawDataTree(detailTabs_);
  detailTabs_->addTab(detailView_, QStringLiteral("精灵详情"));
  detailTabs_->addTab(rawTree_, QStringLiteral("原始数据"));
  analysisPage_ = new QWidget(detailTabs_);
  analysisPage_->setObjectName(QStringLiteral("KQPetAnalysisPage"));
  auto* analysisLayout = new QVBoxLayout(analysisPage_);
  analysisLayout->setContentsMargins(0,0,0,0);
  auto* materialBar = new QHBoxLayout;
  refreshMaterials_ = new QPushButton(QStringLiteral("刷新材料背包"),analysisPage_);
  refreshMaterials_->setObjectName(QStringLiteral("KQRefreshMaterialInventory"));
  refreshMaterials_->setAutoDefault(false);
  materialInventoryStatus_ = new QLabel(QStringLiteral("材料数量未读取"),analysisPage_);
  materialInventoryStatus_->setWordWrap(true);
  materialBar->addWidget(refreshMaterials_);
  materialBar->addWidget(materialInventoryStatus_,1);
  analysisLayout->addLayout(materialBar);
  connect(refreshMaterials_,&QPushButton::clicked,this,&PetWindow::cultivationMaterialsRefreshRequested);
  analysisView_ = new QTextBrowser(analysisPage_);
  analysisView_->setObjectName(QStringLiteral("KQPetPowerAnalysis"));
  analysisView_->setOpenLinks(false);
  analysisView_->setOpenExternalLinks(false);
  analysisView_->setPlaceholderText(QStringLiteral("请选择一只精灵"));
  analysisLayout->addWidget(analysisView_,1);
  detailTabs_->addTab(analysisPage_, QStringLiteral("精灵分析"));
  connect(detailTabs_, &QTabWidget::currentChanged, this, [this](int) {
    if (detailTabs_->currentWidget() != analysisPage_ || !repository_ || currentId_ <= 0) return;
    // Selecting this tab reuses the current detail subscription and local facts.
    // Online refresh remains exclusively behind the existing pet/list actions.
    repository_->watchDetail(0, currentId_);
    showAnalysis(repository_->preparedDetail(0, currentId_), currentId_,
                 displayName(repository_->detailFor(currentId_)));
  });
  connect(analysisView_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    if (!repository_ || currentId_ <= 0 || url.scheme() != QStringLiteral("kqanalysis")) return;
    if (url.host() == QStringLiteral("retry")) repository_->requestDetailPage(0, DetailSection::Overview, 0);
  });
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
  status_->setObjectName(QStringLiteral("KQPetInlineStatus"));
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(status_);

  connect(refresh_, &QPushButton::clicked, this, &PetWindow::listRefreshRequested);
  connect(refreshDetails_, &QPushButton::clicked, this,
          &PetWindow::requestSelectedWarehouseDetails);
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
  searchDebounce_->setInterval(150);
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
  connect(backpackTable_, &QTableWidget::cellClicked, this, [this](int row, int column) {
    selectBackpack(row, column);
    if (currentId_ > 0) emit detailRequested(currentId_);
  });
  const auto warehouseClicked = [this](const QModelIndex& index) {
    selectWarehouse(index);
    if (currentId_ > 0) emit detailRequested(currentId_);
  };
  connect(warehouseTable_, &QTableView::clicked, this, warehouseClicked);
  connect(eliteWarehouseTable_, &QTableView::clicked, this, warehouseClicked);
  connect(repository_, &InventoryReadView::dataChanged, this, &PetWindow::rebuild);
  connect(repository_, &InventoryReadView::detailChanged, this, &PetWindow::updateCurrentDetail);
  connect(repository_, &InventoryReadView::accountSessionChanged, this, &PetWindow::resetSessionContext);
  connect(backpackModel_,&QAbstractItemModel::dataChanged,this,[this] {
    if (backpackDisplayRefreshScheduled_) return;
    backpackDisplayRefreshScheduled_ = true;
    QTimer::singleShot(0,this,[this] {
      backpackDisplayRefreshScheduled_ = false;
      refreshViews(false); // The 12 visible cells consume Model values; no new request or cultivation parsing.
    });
  });
  for (PetTableModel* model : {backpackModel_, warehouseModel_, eliteWarehouseModel_}) {
    connect(model, &PetTableModel::preparationFinished, this, [this](quint64, bool accepted) {
      if (!accepted || backpackModel_->preparationRunning() || warehouseModel_->preparationRunning() ||
          eliteWarehouseModel_->preparationRunning()) return;
      const int ordinaryScroll = warehouseTable_->verticalScrollBar()->value();
      const int eliteScroll = eliteWarehouseTable_->verticalScrollBar()->value();
      const int ordinaryHorizontal = warehouseTable_->horizontalScrollBar()->value();
      const int eliteHorizontal = eliteWarehouseTable_->horizontalScrollBar()->value();
      rebuildFilterChoices(); refreshViews(false);
      resizeModelViewColumns(warehouseTable_); resizeModelViewColumns(eliteWarehouseTable_);
      warehouseTable_->verticalScrollBar()->setValue(ordinaryScroll);
      eliteWarehouseTable_->verticalScrollBar()->setValue(eliteScroll);
      warehouseTable_->horizontalScrollBar()->setValue(ordinaryHorizontal);
      eliteWarehouseTable_->horizontalScrollBar()->setValue(eliteHorizontal);
      const qint64 focus = pendingFocusId_; pendingFocusId_ = 0;
      if (focus > 0 && currentId_ == focus) focusPet(focus);
    });
    model->setMetadataSnapshot(repository_->metadataSnapshot());
  }
  connect(repository_, &InventoryReadView::metadataChanged, this, [this](quint64) {
    const auto metadata = repository_->metadataSnapshot();
    for (PetTableModel* model : {backpackModel_, warehouseModel_, eliteWarehouseModel_}) model->setMetadataSnapshot(metadata);
    if (currentId_ > 0) showDetail(repository_->detailFor(currentId_));
  });
  connect(repository_, &InventoryReadView::statusChanged, this, &PetWindow::setStatus);
  connect(imageCache_, &PetImageCache::petImageReady, this, &PetWindow::updateCurrentImage);
  connect(imageCache_, &PetImageCache::petImageStatusChanged, this, [this](const QString& key, const QString& message) {
    if (key == currentVisualKey_ && !message.isEmpty()) setStatus(message);
  });
  connect(imageCache_, &PetImageCache::attributeIconsReady, this, [this] { refreshViews(false); });

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

void PetWindow::setStatus(const QString& status) {
  status_->setText(status);
  emit sidebarStatusChanged(status);
}

bool PetWindow::event(QEvent* event) {
  const bool result = QDialog::event(event);
  if (imageCache_ && (event->type() == QEvent::Show || event->type() == QEvent::ScreenChangeInternal ||
                     event->type() == QEvent::DevicePixelRatioChange))
    imageCache_->setDevicePixelRatio(devicePixelRatioF());
  if (backpackTable_ && warehouseTable_ && eliteWarehouseTable_ &&
      (event->type() == QEvent::Show || event->type() == QEvent::FontChange ||
       event->type() == QEvent::StyleChange || event->type() == QEvent::ScreenChangeInternal ||
       event->type() == QEvent::DevicePixelRatioChange)) {
    for (QTableView* table : {static_cast<QTableView*>(backpackTable_), warehouseTable_, eliteWarehouseTable_})
      resizeModelViewColumns(table);
  }
  return result;
}

void PetWindow::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);
  QTimer::singleShot(0, this, &PetWindow::fitInventoryGeometry);
}

void PetWindow::fitInventoryGeometry() {
  if (!backpackTable_ || !inventorySplitter_) return;
  for (QTableView* table : {static_cast<QTableView*>(backpackTable_), warehouseTable_, eliteWarehouseTable_})
    resizeModelViewColumns(table);
  const int rowHeight = backpackTable_->verticalHeader()->defaultSectionSize();
  const int tableHeight = backpackTable_->horizontalHeader()->height() + 12*rowHeight + 2*backpackTable_->frameWidth();
  const auto* backpackLayout = backpackTable_->parentWidget()->layout();
  const int controlsHeight = backpackLayout->itemAt(0)->sizeHint().height() +
      backpackLayout->itemAt(2)->sizeHint().height() + 2*backpackLayout->spacing();
  // Give the backpack its whole twelve-row page first; the warehouse receives
  // the remaining height and keeps its own vertical scrollbar.
  const int available = inventorySplitter_->height();
  const int backpackHeight = tableHeight + controlsHeight;
  const bool enough = available >= backpackHeight + 110;
  backpackTable_->setMinimumHeight(enough ? tableHeight : qMin(tableHeight, qMax(120, available-180)));
  inventorySplitter_->setSizes({qMin(backpackHeight, qMax(150,available-110)), qMax(110,available-backpackHeight)});
}

void PetWindow::resetSessionContext() {
    cultivationMaterials_ = {};
    if (refreshMaterials_) { refreshMaterials_->setEnabled(true); refreshMaterials_->setText(QStringLiteral("刷新材料背包")); }
    if (materialInventoryStatus_) materialInventoryStatus_->setText(QStringLiteral("材料数量未读取"));
    currentId_ = 0;
    pendingFocusId_ = 0;
    currentRaceId_ = 0;
    currentLocation_.clear();
    currentVisualKey_.clear();
    backpackPage_ = 0;
    for (PetTableModel* model : {backpackModel_, warehouseModel_, eliteWarehouseModel_})
      model->setPets({});
    backpackTable_->clearSelection();
    backpackTable_->selectionModel()->clearCurrentIndex();
    backpackTable_->setRowCount(0);
    for (QTableView* table : {warehouseTable_, eliteWarehouseTable_}) {
      table->clearSelection();
      table->selectionModel()->clearCurrentIndex();
      table->verticalScrollBar()->setValue(0);
    }
    setDetailProgress(false, false, 0, 0, 0, 0, 0, 0);
    setMoveRunning(false);
    setListRefreshRunning(false);
    setStatus(QStringLiteral("会话已更新，请重新选择精灵。"));
    showDetail({});
    detailView_->verticalScrollBar()->setValue(0);
    rawTree_->verticalScrollBar()->setValue(0);
    updateMoveButtons();

  compactDetails_ = false;
  updateWorkbenchPanels();
}

void PetWindow::setWorkbenchMode(bool embedded, bool compact) {
  if (workbenchEmbedded_ == embedded && workbenchCompact_ == compact) return;
  if (!workbenchCompact_ && embedded && compact) workbenchWideSizes_ = contentSplitter_->sizes();
  workbenchEmbedded_ = embedded;
  workbenchCompact_ = embedded && compact;
  compactDetails_ = false;
  setMinimumSize(embedded ? QSize(0, 0) : QSize(1280, 700));
  setSizePolicy(QSizePolicy::Expanding, workbenchCompact_ ? QSizePolicy::Ignored : QSizePolicy::Preferred);
  contentSplitter_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
  for (int pane = 0; pane < inventorySplitter_->count(); ++pane)
    inventorySplitter_->widget(pane)->setSizePolicy(QSizePolicy::Expanding,
        workbenchCompact_ ? QSizePolicy::Ignored : QSizePolicy::Preferred);
  if (layout()) layout()->setContentsMargins(embedded ? 6 : 11, embedded ? 4 : 11,
                                            embedded ? 6 : 11, embedded ? 4 : 11);
  for (QGridLayout* grid : {toolbarLayout_, filterLayout_}) {
    while (QLayoutItem* item = grid->takeAt(0)) delete item;
    for (int column = 0; column < 8; ++column) grid->setColumnStretch(column, 0);
  }
  settings_->setVisible(!embedded);
  diagnostics_->setVisible(!embedded);
  if (workbenchCompact_) {
    toolbarLayout_->addWidget(toolbarWidgets_[0], 0, 0);
    toolbarLayout_->addWidget(search_, 0, 1, 1, 3);
    for (int column = 0; column < 4; ++column)
      toolbarLayout_->addWidget(toolbarWidgets_[column + 2], 1, column);
    toolbarLayout_->setColumnStretch(1, 1);
    toolbarLayout_->setColumnStretch(3, 1);
    for (int column = 0; column < 4; ++column)
      filterLayout_->addWidget(filterWidgets_[column], 0, column);
    filterLayout_->addWidget(filterWidgets_[4], 1, 0);
    filterLayout_->addWidget(filterWidgets_[5], 1, 1);
    filterLayout_->addWidget(filterWidgets_[6], 1, 2, 1, 2);
    filterLayout_->setColumnStretch(1, 1);
    filterLayout_->setColumnStretch(3, 1);
  } else {
    for (int column = 0; column < toolbarWidgets_.size(); ++column)
      toolbarLayout_->addWidget(toolbarWidgets_[column], 0, column);
    toolbarLayout_->setColumnStretch(1, 1);
    for (int column = 0; column < filterWidgets_.size(); ++column)
      filterLayout_->addWidget(filterWidgets_[column], 0, column);
    filterLayout_->setColumnStretch(7, 1);
  }
  toolbarWidgets_[0]->hide();
  toolbarLayout_->addWidget(filterToggle_, 0, workbenchCompact_ ? 4 : toolbarWidgets_.size());
  for (QTableView* table : {static_cast<QTableView*>(backpackTable_), warehouseTable_, eliteWarehouseTable_}) {
    table->setMinimumWidth(0);
    for (int column=0; column<table->model()->columnCount(); ++column) table->setColumnHidden(column, false);
    resizeModelViewColumns(table);
  }
  progress_->setVisible(!embedded && detailBatchRunning_);
  status_->setVisible(!embedded);
  emit sidebarStatusChanged(status_->text());
  emit sidebarDetailProgressChanged(progress_->text(), detailBatchRunning_, detailCompleted_, detailTotal_);
  updateWorkbenchPanels();
  if (!workbenchCompact_) contentSplitter_->setSizes(workbenchWideSizes_.isEmpty() ? QList<int>{760, 390} : workbenchWideSizes_);
  QTimer::singleShot(0, this, &PetWindow::fitInventoryGeometry);
}

void PetWindow::updateWorkbenchPanels() {
  if (!detailToggle_ || !contentSplitter_) return;
  detailToggle_->setVisible(workbenchCompact_);
  detailToggle_->setText(compactDetails_ ? QStringLiteral("返回精灵列表") : QStringLiteral("查看精灵详情"));
  const bool showListControls = !workbenchCompact_ || !compactDetails_;
  filterPanel_->setVisible(showListControls && filterToggle_->isChecked());
  detailEraFilters_->setVisible(showListControls);
  filterToggle_->setVisible(showListControls);
  for (QWidget* widget : filterWidgets_) widget->setVisible(showListControls);
  for (int index = 2; index < 4; ++index) toolbarWidgets_[index]->setVisible(showListControls);
  pauseDetails_->setVisible(showListControls && detailBatchRunning_);
  cancelDetails_->setVisible(showListControls && detailBatchRunning_);
  inventorySplitter_->setVisible(!workbenchCompact_ || !compactDetails_);
  detailTabs_->setVisible(!workbenchCompact_ || compactDetails_);
  moveToWarehouse_->setVisible(!workbenchCompact_ || compactDetails_);
  moveToBackpack_->setVisible(!workbenchCompact_ || compactDetails_);
}

void PetWindow::focusPet(qint64 instanceId) {
  if (!repository_ || instanceId <= 0) return;
  {
    const QSignalBlocker searchBlocker(search_);
    const QSignalBlocker attributeBlocker(attributeFilter_);
    const QSignalBlocker jobBlocker(jobFilter_);
    const QSignalBlocker eraBlocker(eraFilter_);
    search_->clear();
    attributeFilter_->setCurrentIndex(0);
    jobFilter_->setCurrentIndex(0);
    eraFilter_->setCurrentIndex(0);
  }
  refreshViews();

  const QJsonObject backpack = repository_->backpackPet(instanceId);
  if (backpackModel_->preparationRunning() || warehouseModel_->preparationRunning() || eliteWarehouseModel_->preparationRunning()) {
    const QJsonObject target = backpack.isEmpty() ? repository_->warehousePet(instanceId) : backpack;
    if (!target.isEmpty()) {
      currentId_ = instanceId; pendingFocusId_ = instanceId;
      showDetail(repository_->detailFor(instanceId));
    }
    return;
  }
  if (!backpack.isEmpty()) {
    const int sourceRow = backpackModel_->rowForInstanceId(instanceId);
    const QModelIndex mapped = backpackProxy_->mapFromSource(backpackModel_->index(sourceRow, 0));
    if (mapped.isValid()) {
      setBackpackPage(mapped.row() / 12);
      const int row = mapped.row() % 12;
      backpackTable_->selectRow(row);
      selectBackpack(row, 0);
      setStatus(QStringLiteral("已定位实例 %1；当前详情来源见左侧状态区。").arg(instanceId));
      return;
    }
  }

  const QJsonObject warehouse = repository_->warehousePet(instanceId);
  if (warehouse.isEmpty()) return;
  const bool elite = warehouse.value(QStringLiteral("_warehouseGroup")).toString() ==
                     QStringLiteral("elite");
  QTableView* table = elite ? eliteWarehouseTable_ : warehouseTable_;
  warehouseTabs_->setCurrentIndex(elite ? 1 : 0);
  PetTableModel* source = elite ? eliteWarehouseModel_ : warehouseModel_;
  PetFilterProxyModel* proxy = elite ? eliteWarehouseProxy_ : warehouseProxy_;
  const QModelIndex index = proxy->mapFromSource(source->index(source->rowForInstanceId(instanceId), 0));
  if (index.isValid()) {
    table->setCurrentIndex(index);
    table->scrollTo(index, QAbstractItemView::PositionAtCenter);
    selectWarehouse(index);
    setStatus(QStringLiteral("已定位实例 %1；当前详情来源见左侧状态区。").arg(instanceId));
  }
}

void PetWindow::setListRefreshRunning(bool running) {
  listRefreshRunning_ = running;
  refresh_->setEnabled(!running && !moveRunning_);
  refresh_->setText(running ? QStringLiteral("列表刷新中……")
                            : QStringLiteral("刷新背包/仓库"));
  updateMoveButtons();
}

void PetWindow::requestSelectedWarehouseDetails() {
  QSet<QString> selectedEras;
  for (QCheckBox* check : detailEraChecks_)
    if (check->isChecked()) selectedEras.insert(check->text());
  if (selectedEras.isEmpty()) {
    setStatus(QStringLiteral("请至少勾选一个详情刷新类别。"));
    return;
  }
  const PetMetadataView metadata(repository_->metadataSnapshot());
  const QSet<QString> namedEras{QStringLiteral("灵初"), QStringLiteral("神运"),
                               QStringLiteral("星迹"), QStringLiteral("启元")};
  QList<qint64> selectedIds;
  const auto warehouse = repository_->warehousePets();
  for (const QJsonObject& pet : warehouse) {
    const QString era = metadata.resolvedEra(pet);
    const QString category = namedEras.contains(era) ? era : QStringLiteral("其他");
    if (!selectedEras.contains(category)) continue;
    const QJsonValue rawId = pet.value(QStringLiteral("id"));
    const qint64 id = rawId.isString() ? rawId.toString().toLongLong() : rawId.toInteger();
    if (id > 0) selectedIds.append(id);
  }
  if (selectedIds.isEmpty()) {
    setStatus(QStringLiteral("当前仓库没有属于已勾选类别的精灵。"));
    return;
  }
  // Freeze membership here, before the intent crosses to Core. Subsequent
  // filter/metadata changes and pause/resume cannot broaden this batch.
  emit warehouseDetailRefreshRequested(selectedIds);
}

void PetWindow::setDetailProgress(bool running, bool paused, int completed, int total,
                                  int succeeded, int failed, qint64 currentInstanceId,
                                  int estimatedSeconds) {
  detailBatchPaused_ = paused;
  detailBatchRunning_ = running;
  detailCompleted_ = completed;
  detailTotal_ = total;
  pauseDetails_->setVisible(running && (!workbenchCompact_ || !compactDetails_));
  cancelDetails_->setVisible(running && (!workbenchCompact_ || !compactDetails_));
  progress_->setVisible(!workbenchEmbedded_ && (running || total > 0));
  refreshDetails_->setEnabled(!running && !moveRunning_);
  for (QCheckBox* check : detailEraChecks_) check->setEnabled(!running && !moveRunning_);
  pauseDetails_->setEnabled(running && !moveRunning_);
  cancelDetails_->setEnabled(running && !moveRunning_);
  pauseDetails_->setText(paused ? QStringLiteral("继续详情刷新")
                                : QStringLiteral("暂停详情刷新"));
  if (!running && total <= 0) {
    progress_->setText(QStringLiteral("仓库详情刷新：未启动"));
    emit sidebarDetailProgressChanged(progress_->text(), running, completed, total);
    return;
  }
  const QString current = currentInstanceId > 0 ? QString::number(currentInstanceId)
                                                 : QStringLiteral("等待中");
  progress_->setText(QStringLiteral("仓库详情：%1/%2　成功 %3　失败 %4　当前 %5　预计剩余 %6 秒%7")
                         .arg(completed).arg(total).arg(succeeded).arg(failed)
                         .arg(current).arg(estimatedSeconds)
                         .arg(paused ? QStringLiteral("　【已暂停】") : QString()));
  emit sidebarDetailProgressChanged(progress_->text(), running, completed, total);
}

void PetWindow::setMoveRunning(bool running) {
  moveRunning_ = running;
  refresh_->setEnabled(!listRefreshRunning_ && !running);
  refreshDetails_->setEnabled(!detailBatchRunning_ && !running);
  for (QCheckBox* check : detailEraChecks_) check->setEnabled(!detailBatchRunning_ && !running);
  pauseDetails_->setEnabled(detailBatchRunning_ && !running);
  cancelDetails_->setEnabled(detailBatchRunning_ && !running);
  settings_->setEnabled(!running);
  updateMoveButtons();
}

QString PetWindow::displayName(const QJsonObject& pet) const {
  const QString name = pet.value(QStringLiteral("n")).toString();
  return name.isEmpty() ? QStringLiteral("未返回名称") : name;
}

void PetWindow::fillTable(QTableWidget* table, const QList<QJsonObject>& pets,
                          const QString& location) {
  const QSignalBlocker blocker(table);
  table->setUpdatesEnabled(false);
  table->setSortingEnabled(false);
  table->setRowCount(pets.size());
  int row = 0;
  for (const QJsonObject& pet : pets) fillRow(table, row++, pet, location);
  resizeModelViewColumns(table);
  table->setUpdatesEnabled(true);
  table->viewport()->update();
}

void PetWindow::fillRow(QTableWidget* table, int row, const QJsonObject& pet,
                        const QString& location) {
  const QJsonValue identity = pet.value(QStringLiteral("id"));
  const qint64 id = identity.isString() ? identity.toString().toLongLong() : identity.toInteger();
  PetTableModel* source = location == QStringLiteral("backpack") ? backpackModel_
      : pet.value(QStringLiteral("_warehouseGroup")).toString() == QStringLiteral("elite")
          ? eliteWarehouseModel_ : warehouseModel_;
  const int sourceRow = source->rowForInstanceId(id);
  if (sourceRow < 0) return;
  for (int column = 0; column < source->columnCount(); ++column) {
    const QModelIndex index = source->index(sourceRow, column);
    QTableWidgetItem* item = table->item(row, column);
    if (!item) { item = new QTableWidgetItem(); table->setItem(row, column, item); }
    item->setText(index.data(Qt::DisplayRole).toString());
    item->setData(Qt::UserRole, id);
    item->setData(PetTableModel::IdentityTextRole, index.data(PetTableModel::IdentityTextRole));
    item->setToolTip(index.data(Qt::ToolTipRole).toString());
    if (column <= PetTableModel::OriginalNameColumn) {
      item->setData(PetTableModel::NameSearchIndexRole, index.data(PetTableModel::NameSearchIndexRole));
    }
    if (column == PetTableModel::AttributesColumn) item->setIcon(qvariant_cast<QIcon>(index.data(Qt::DecorationRole)));
    if (location == QStringLiteral("backpack") && column == PetTableModel::DeployedColumn) {
      QFont font = item->font();
      font.setBold(index.data().toString() == QStringLiteral("是"));
      item->setFont(font);
      const QVariant brush = index.data(Qt::ForegroundRole);
      item->setForeground(brush.isValid() ? qvariant_cast<QBrush>(brush)
                                         : table->palette().brush(QPalette::Text));
    }
  }
}

void PetWindow::updateWarehouseRow(qint64 instanceId) {
  const QJsonObject pet = repository_->warehousePet(instanceId);
  if (pet.isEmpty()) return;
  warehouseModel_->updatePet(pet);
  eliteWarehouseModel_->updatePet(pet);
}

void PetWindow::rebuild() {
  refreshViews(true);
  if (currentId_ > 0 && repository_->backpackPet(currentId_).isEmpty() &&
      repository_->warehousePet(currentId_).isEmpty()) {
    currentId_ = 0;
    showDetail({});
  }
  if (currentId_ > 0)
    showDetail(repository_->detailFor(currentId_));
}

void PetWindow::applyFilter() {
  backpackPage_ = 0;
  if (workbenchCompact_) { compactDetails_ = false; updateWorkbenchPanels(); }
  updateSortDirectionState();
  refreshViews();
}

void PetWindow::updateSortDirectionState() {
  backpackSortDirection_->setEnabled(
      backpackSort_->currentData().toInt() != static_cast<int>(PetSortMode::Default));
  warehouseSortDirection_->setEnabled(
      warehouseSort_->currentData().toInt() != static_cast<int>(PetSortMode::Default));
}

void PetWindow::rebuildFilterChoices() {
  QSet<QString> attributes;
  QSet<QString> jobs;
  QSet<QString> dualCareerJobs;
  QSet<QString> eras;
  for (const PetTableModel* model : {backpackModel_, warehouseModel_, eliteWarehouseModel_}) {
    for (int row = 0; row < model->rowCount(); ++row) {
      const auto* cached = model->cachedRow(row);
      for (const QString& value : cached->attributes) attributes.insert(value);
      for (const QString& value : cached->jobs) jobs.insert(value);
      if (cached->jobs.size() > 1)
        for (const QString& value : cached->jobs) dualCareerJobs.insert(value);
      eras.insert(cached->era);
    }
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
  if (rebuildChoices) {
    QList<QJsonObject> ordinary, elite;
    for (const QJsonObject& pet : repository_->warehousePets()) {
      if (pet.value(QStringLiteral("_warehouseGroup")).toString() == QStringLiteral("elite"))
        elite.append(pet);
      else ordinary.append(pet);
    }
    backpackModel_->setPetsAsync(repository_->backpackPets());
    warehouseModel_->setPetsAsync(ordinary);
    eliteWarehouseModel_->setPetsAsync(elite);
  }
  const QString attribute = attributeFilter_->currentIndex() > 0 ? attributeFilter_->currentText() : QString{};
  const QString job = jobFilter_->currentIndex() > 0 ? jobFilter_->currentText() : QString{};
  const QString era = eraFilter_->currentIndex() > 0 ? eraFilter_->currentText() : QString{};
  for (PetFilterProxyModel* proxy : {backpackProxy_, warehouseProxy_, eliteWarehouseProxy_}) {
    proxy->setFilters(search_->text(), attribute, job, era);
    const bool backpack = proxy == backpackProxy_;
    const auto mode = static_cast<PetFilterProxyModel::SortMode>(
        (backpack ? backpackSort_ : warehouseSort_)->currentData().toInt());
    const bool ascending = mode == PetFilterProxyModel::SortMode::Default ? true
        : (backpack ? backpackSortDirection_ : warehouseSortDirection_)->currentData().toBool();
    proxy->setSortMode(mode, ascending);
  }
  const int backpackCount = backpackProxy_->rowCount();
  const int pageCount = qMax(1, (backpackCount + 11) / 12);
  backpackPage_ = qBound(0, backpackPage_, pageCount - 1);
  QList<QJsonObject> visibleBackpack;
  for (int row = backpackPage_ * 12; row < qMin(backpackCount, backpackPage_ * 12 + 12); ++row)
    visibleBackpack.append(backpackProxy_->index(row, 0).data(PetTableModel::PetObjectRole).toJsonObject());
  fillTable(backpackTable_, visibleBackpack, QStringLiteral("backpack"));
  if (currentId_ > 0) {
    const int sourceBackpack = backpackModel_->rowForInstanceId(currentId_);
    const QModelIndex backpack = backpackProxy_->mapFromSource(backpackModel_->index(sourceBackpack, 0));
    if (backpack.isValid() && backpack.row() / 12 == backpackPage_) {
      backpackTable_->setCurrentCell(backpack.row() % 12, 0,
          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    } else {
      for (const auto& pair : {qMakePair(warehouseProxy_, warehouseTable_),
                               qMakePair(eliteWarehouseProxy_, eliteWarehouseTable_)}) {
        auto* source = qobject_cast<PetTableModel*>(pair.first->sourceModel());
        const QModelIndex selected = pair.first->mapFromSource(
            source->index(source->rowForInstanceId(currentId_), 0));
        if (selected.isValid()) {
          pair.second->setCurrentIndex(selected);
          pair.second->selectRow(selected.row());
          break;
        }
      }
    }
  }
  rebuildPageButtons(pageCount);
  backpackTitle_->setText(QStringLiteral("背包：%1 / %2　第 %3/%4 页")
      .arg(backpackCount).arg(backpackModel_->rowCount()).arg(backpackPage_ + 1).arg(pageCount));
  warehouseTabs_->setTabText(0, QStringLiteral("普通仓库（%1 / %2）")
      .arg(warehouseProxy_->rowCount()).arg(warehouseModel_->rowCount()));
  warehouseTabs_->setTabText(1, QStringLiteral("精英仓库（%1 / %2）")
      .arg(eliteWarehouseProxy_->rowCount()).arg(eliteWarehouseModel_->rowCount()));
}

void PetWindow::rebuildPageButtons(int pageCount) {
  while (QLayoutItem* item = backpackPages_->takeAt(0)) {
    if (item->widget()) item->widget()->deleteLater();
    delete item;
  }
  if (pageCount <= 1) return;
  backpackPages_->addStretch(1);
  for (int page = 0; page < pageCount; ++page) {
    auto* button = new QPushButton(QString::number(page + 1), this);
    button->setFixedSize(32, 28);
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

void PetWindow::selectBackpack(int row, int) {
  currentId_ = rowId(backpackTable_, row);
  currentLocation_ = QStringLiteral("backpack");
  showDetail(repository_->detailFor(currentId_));
  updateMoveButtons();
  if (workbenchCompact_) { compactDetails_ = true; updateWorkbenchPanels(); }
}

void PetWindow::selectWarehouse(const QModelIndex& index) {
  currentId_ = index.data(PetTableModel::InstanceIdRole).toLongLong();
  currentLocation_ = QStringLiteral("warehouse");
  showDetail(repository_->detailFor(currentId_));
  updateMoveButtons();
  if (workbenchCompact_) { compactDetails_ = true; updateWorkbenchPanels(); }
  if (currentId_ > 0) {
    const QDateTime cachedAt = repository_->detailSavedAt(currentId_);
    setStatus(cachedAt.isValid()
                  ? QStringLiteral("已显示实例 %1 的本地缓存（%2）。")
                        .arg(currentId_)
                        .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                  : QStringLiteral("实例 %1 尚无详情缓存。")
                        .arg(currentId_));
  }
}

void PetWindow::updateCurrentDetail(qint64 instanceId) {
  updateWarehouseRow(instanceId);
  if (instanceId == currentId_)
    showDetail(repository_->detailFor(instanceId));
}

void PetWindow::updateCurrentImage(const QString& visualKey, const QString& url) {
  if (visualKey == currentVisualKey_ && currentId_ > 0)
    imageCache_->updateDocumentImage(detailView_, url);
}

void PetWindow::showDetail(const QJsonObject& pet) {
  const qint64 id = petInstanceId(pet);
  if (repository_) repository_->watchDetail(0, id);
  const auto raw = repository_ && id > 0 ? repository_->rawRecordHandle(id) : RawPetRecordHandle{};
  rawTree_->setRecord(pet, raw);
  if (pet.isEmpty()) {
    currentLocation_.clear();
    updateMoveButtons();
    currentVisualKey_.clear();
    renderedDetailId_ = 0;
    renderedPreparedDetail_.reset();
    renderedAnalysisDetail_.reset();
    analysisObservedAt_ = {};
    ++analysisRenderGeneration_;
    pendingDetailScrollId_ = 0;
    pendingDetailScroll_ = 0;
    ++detailRenderGeneration_;
    detailView_->setHtml(QStringLiteral("<p style='color:#6b7280'>请选择一只精灵</p>"));
    analysisView_->setHtml(QStringLiteral("<p style='color:#6b7280'>请选择一只精灵</p>"));
    return;
  }

  currentLocation_ = pet.value(QStringLiteral("_location")).toString();
  updateMoveButtons();

  const PetMetadataView catalog(repository_->metadataSnapshot());
  const int raceId = petRaceId(pet);
  currentRaceId_ = raceId;
  currentVisualKey_ = petVisualKey(pet);
  QString name = pet.value(QStringLiteral("n")).toString();
  if (name.isEmpty())
    name = catalog.petName(raceId);
  const QString originalName = catalog.resolvedOriginalName(pet);
  const QString imagePath = imageCache_->ensurePetImage(
      pet, {name, displayName(pet), catalog.petName(raceId), originalName}, detailView_->devicePixelRatioF());
  const auto prepared = repository_->preparedDetail(0, id);
  const qint64 nextDetailId = id > 0 ? id : currentId_;
  showAnalysis(prepared, nextDetailId, name);
  const auto currentKey = repository_->recordVersion(nextDetailId).key;
  if (!prepared && renderedPreparedDetail_ && renderedDetailId_ == nextDetailId &&
      renderedPreparedDetail_->version.facts.record.account == currentKey.account &&
      renderedPreparedDetail_->version.facts.record.epoch == currentKey.epoch) {
    const auto error = repository_->detailPreparationError(0);
    if (!error.isEmpty()) setStatus(QStringLiteral("详情更新未完成：%1；当前保留上次详情。").arg(error));
    return;
  }
  const bool preserveScroll = renderedDetailId_ > 0 &&
                              renderedDetailId_ == nextDetailId;
  const int visibleScroll = detailView_->verticalScrollBar()->value();
  const int previousScroll = preserveScroll && visibleScroll == 0 &&
                                     pendingDetailScrollId_ == nextDetailId
                                 ? pendingDetailScroll_
                                 : visibleScroll;
  const auto rendered = prepared ? PreparedPetDetailRenderer::render(prepared, imagePath, detailView_->viewport()->width() < 440)
      : PreparedPetDetailRenderer::waiting(name, repository_->detailPreparationError(0));
  imageCache_->setDocumentImage(detailView_, rendered, imagePath);
  renderedPreparedDetail_ = prepared;
  renderedDetailId_ = nextDetailId;
  const quint64 renderGeneration = ++detailRenderGeneration_;
  if (preserveScroll) {
    pendingDetailScrollId_ = nextDetailId;
    pendingDetailScroll_ = previousScroll;
    QScrollBar* scrollBar = detailView_->verticalScrollBar();
    scrollBar->setValue(qMin(previousScroll, scrollBar->maximum()));
    QTimer::singleShot(0, detailView_, [this, nextDetailId, previousScroll,
                                       renderGeneration]() {
      if (renderedDetailId_ != nextDetailId ||
          detailRenderGeneration_ != renderGeneration)
        return;
      QScrollBar* scrollBar = detailView_->verticalScrollBar();
      scrollBar->setValue(qMin(previousScroll, scrollBar->maximum()));
      pendingDetailScrollId_ = 0;
      pendingDetailScroll_ = 0;
    });
  } else {
    pendingDetailScrollId_ = 0;
    pendingDetailScroll_ = 0;
    detailView_->verticalScrollBar()->setValue(0);
  }


}

void PetWindow::showAnalysis(const PreparedPetDetailHandle& detail, qint64 instanceId,
                             const QString& name) {
  if (!analysisView_ || !repository_ || instanceId <= 0) return;
  const auto version = repository_->recordVersion(instanceId);
  const bool sameSelection = renderedAnalysisDetail_ &&
      renderedAnalysisDetail_->identity.instanceId == instanceId &&
      renderedAnalysisDetail_->version.facts.record.account == version.key.account &&
      renderedAnalysisDetail_->version.facts.record.epoch == version.key.epoch;
  const int scroll = sameSelection ? analysisView_->verticalScrollBar()->value() : 0;
  QString error = repository_->detailPreparationError(0);
  bool retained = false;
  if (sameSelection && (!detail || (!detail->detailKnown && renderedAnalysisDetail_->detailKnown))) {
    retained = true;
    if (detail && error.isEmpty()) error = QStringLiteral("本次详情尚不完整");
  } else if (detail) {
    renderedAnalysisDetail_ = detail;
    analysisObservedAt_ = version.observedAt.isValid() ? version.observedAt
                                                     : repository_->detailSavedAt(instanceId);
  } else if (!sameSelection) {
    renderedAnalysisDetail_.reset();
    analysisObservedAt_ = {};
  }
  const QString html = renderedAnalysisDetail_
      ? PetPowerAnalysisRenderer::render(renderedAnalysisDetail_, analysisObservedAt_, retained, error, cultivationMaterials_)
      : PetPowerAnalysisRenderer::waiting(name, error);
  analysisView_->setHtml(html);
  const quint64 generation = ++analysisRenderGeneration_;
  analysisView_->verticalScrollBar()->setValue(scroll);
  if (sameSelection) QTimer::singleShot(0, analysisView_, [this, instanceId, scroll, generation] {
    if (currentId_ != instanceId || analysisRenderGeneration_ != generation) return;
    auto* bar = analysisView_->verticalScrollBar();
    bar->setValue(qMin(scroll, bar->maximum()));
  });
}

void PetWindow::setCultivationMaterials(const MaterialInventorySnapshot& materials) {
  if (cultivationMaterials_.counts == materials.counts && cultivationMaterials_.knownTypes == materials.knownTypes &&
      cultivationMaterials_.knownExtraGroups == materials.knownExtraGroups &&
      cultivationMaterials_.observedAt == materials.observedAt && cultivationMaterials_.running == materials.running) return;
  cultivationMaterials_ = materials;
  refreshMaterials_->setEnabled(!materials.running);
  refreshMaterials_->setText(materials.running ? QStringLiteral("正在刷新……") : QStringLiteral("刷新材料背包"));
  materialInventoryStatus_->setText(materials.running ? QStringLiteral("正在读取材料背包")
      : materials.observedAt.isValid() ? QStringLiteral("更新于 %1").arg(materials.observedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                      : QStringLiteral("材料数量未读取"));
  if (repository_ && currentId_ > 0) showAnalysis(repository_->preparedDetail(0,currentId_),currentId_,
      displayName(repository_->detailFor(currentId_)));
}
