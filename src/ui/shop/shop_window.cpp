#include "shop_window.h"
#include <QComboBox>

#include "diagnostics/build_info.h"
#include "ui/common/ui_preferences.h"

#include "domain/pet_identity.h"
#include "ui/common/pet_image_cache.h"
#include "domain/pet_move_policy.h"
#include "application/views/inventory_read_view.h"
#include "ui/detail/prepared_pet_detail_renderer.h"
#include "ui/detail/pet_raw_data_tree.h"
#include "ui/common/pet_facts_ui.h"
#include "domain/pet_metadata_view.h"
#include "domain/shop_limit_facts.h"
#include "ui/pet/pet_search.h"

#include <QAbstractItemView>
#include <QElapsedTimer>
#include <QBrush>
#include <QColor>

#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QTabBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QScrollBar>

#include <algorithm>

namespace {

QTableWidget* makeTable(QWidget* parent, const QStringList& headers) {
  auto* table = new QTableWidget(parent);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->setVisible(false);
  table->setShowGrid(false);
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideNone);
  table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  return table;
}

QTableWidgetItem* textItem(const QString& text, bool emphasize = false) {
  auto* item = new QTableWidgetItem(text);
  item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  if (emphasize) {
    QFont font = item->font();
    font.setBold(true);
    item->setFont(font);
    item->setForeground(QBrush(QColor(QStringLiteral("#c62828"))));
  }
  return item;
}

// The backpack adds its grid position; everything else is the shared rule.
QString locationText(const QJsonObject& pet) {
  if (pet.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack")) {
    if (!pet.contains(QStringLiteral("_position"))) return QStringLiteral("背包");
    const int position = qMax(0, pet.value(QStringLiteral("_position")).toInt());
    return QStringLiteral("背包 第%1页第%2排")
        .arg(position / 12 + 1).arg(position % 12 / 6 + 1);
  }
  return petWarehouseGroupName(pet.value(QStringLiteral("_warehouseGroup")).toString());
}

QString html(const QString& text) { return text.toHtmlEscaped(); }

QString factRow(const QString& label, const QString& value) {
  return QStringLiteral("<tr><td class='label'>%1</td><td>%2</td></tr>")
      .arg(html(label), value.isEmpty() ? QStringLiteral("—") : html(value));
}

QString compactValue(const QJsonObject& pet, const QString& key) {
  const QJsonValue value = pet.value(key);
  if (value.isString()) return value.toString();
  if (value.isDouble()) return QString::number(value.toDouble(), 'g', 16);
  if (value.isBool()) return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
  if (value.isObject())
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  return QStringLiteral("—");
}

qint64 numericValue(const QJsonObject& pet, const QString& key) {
  const QJsonValue value = pet.value(key);
  if (value.isDouble()) return value.toInteger(-1);
  if (value.isString()) {
    bool known = false; const auto number = value.toString().toLongLong(&known);
    return known && number >= 0 ? number : -1;
  }
  return -1;
}

QString resourceName(const ResourceRequirement& requirement, const PetMetadataView& metadata) {
  // This is a canonical key emitted by the strict Domain cost parser. Retain
  // the key if an unexpected representation arrives; never turn bad text into
  // material type/id zero or infer a resource from an incomplete cost fragment.
  const auto fields = requirement.resourceKey.split(QLatin1Char(':'));
  bool typeKnown = false, idKnown = false;
  const int type = fields.size() == 2 ? fields.at(0).toInt(&typeKnown) : 0;
  const int id = fields.size() == 2 ? fields.at(1).toInt(&idKnown) : 0;
  return typeKnown && idKnown && type > 0 && id > 0
      ? metadata.materialName(type,id) : requirement.resourceKey;
}

}  // namespace

ShopWindow::ShopWindow(InventoryReadView* repository, QWidget* parent, PetImageCache* sharedImages)
    : QDialog(parent), repository_(repository), imageCache_(sharedImages) {
  setObjectName(QStringLiteral("KQPetShopWindow"));
  setWindowTitle(QStringLiteral("精灵工作台 · 培养兑换商店 · %1")
                     .arg(BuildInfo::displayVersion()));
  resize(1420, 860);
  setMinimumSize(1100, 680);
  setAttribute(Qt::WA_DeleteOnClose, false);
  auto* root = new QVBoxLayout(this);
  toolbarLayout_ = new QGridLayout();
  refresh_ = new QPushButton(QStringLiteral("刷新兑换次数"), this);
  refreshCatalog_ = new QPushButton(QStringLiteral("更新兑换项目/适用精灵"), this);
  refreshCatalog_->setToolTip(QStringLiteral("只检查兑换商店数据；其他数据请在设置里更新"));
  sourceFilter_ = new QComboBox(this);
  sourceFilter_->setObjectName(QStringLiteral("KQShopSourceFilter"));
  sourceFilter_->addItems({QStringLiteral("全部兑换"),QStringLiteral("常驻兑换"),QStringLiteral("活动兑换")});
  UiPreferences::bindComboBox(sourceFilter_, QStringLiteral("shop/sourceFilter"));
  connect(sourceFilter_,&QComboBox::currentIndexChanged,this,[this](int) {
    auto catalog = catalogSnapshot_; const auto date = catalogDate_;
    catalogSnapshot_.reset(); setCatalogSnapshot(std::move(catalog),date);
  });
  status_ = new QLabel(QStringLiteral("商店只在手动点击按钮时刷新"), this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  toolbarLayout_->addWidget(refresh_, 0, 0);
  toolbarLayout_->addWidget(refreshCatalog_, 0, 1);
  toolbarLayout_->addWidget(sourceFilter_, 0, 2);
  toolbarLayout_->addWidget(status_, 0, 3);
  toolbarLayout_->setColumnStretch(3, 1);
  root->addLayout(toolbarLayout_);
  detailToggle_ = new QPushButton(QStringLiteral("查看精灵详情"), this);
  detailToggle_->setObjectName(QStringLiteral("KQShopCompactDetailToggle"));
  detailToggle_->setMinimumHeight(28);
  detailToggle_->hide();
  root->addWidget(detailToggle_);
  connect(detailToggle_, &QPushButton::clicked, this, [this] {
    compactDetails_ = !compactDetails_;
    updateWorkbenchPanels();
  });

  auto* horizontal = new QSplitter(Qt::Horizontal, this);
  auto* left = new QSplitter(Qt::Vertical, horizontal);
  contentSplitter_ = horizontal;
  listingSplitter_ = left;
  auto* shopPane = new QWidget(left);
  auto* shopLayout = new QVBoxLayout(shopPane);
  shopLayout->setContentsMargins(0, 0, 0, 0);
  shopLayout->setSpacing(6);
  currencySummary_ = new QLabel(QStringLiteral("当前商店资源：未查询"), shopPane);
  currencySummary_->setObjectName(QStringLiteral("KQShopCurrencySummary"));
  currencySummary_->setWordWrap(true);
  currencySummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  currencySummary_->setStyleSheet(QStringLiteral(
      "QLabel{padding:8px 12px;border:1px solid #d7deea;border-radius:6px;"
      "background:#f6f8fc;color:#344054;font-weight:600;}"));
  shopTabs_ = new QTabWidget(shopPane);
  shopTabs_->setObjectName(QStringLiteral("KQShopTabs"));
  shopTabs_->setUsesScrollButtons(true);
  goodSearch_ = new QLineEdit(shopPane);
  goodSearch_->setObjectName(QStringLiteral("KQShopGoodSearch"));
  goodSearch_->setPlaceholderText(QStringLiteral("搜索兑换项目或商店名（支持拼音首字母）"));
  goodSearch_->setClearButtonEnabled(true);
  goodSearchDebounce_ = new QTimer(this);
  goodSearchDebounce_->setSingleShot(true);
  goodSearchDebounce_->setInterval(150);
  connect(goodSearchDebounce_, &QTimer::timeout, this, [this] { applyGoodSearch(true); });
  connect(goodSearch_, &QLineEdit::textChanged, goodSearchDebounce_, qOverload<>(&QTimer::start));
  shopLayout->addWidget(currencySummary_);
  shopLayout->addWidget(goodSearch_);
  shopLayout->addWidget(shopTabs_, 1);

  auto* petPane = new QWidget(left);
  auto* petLayout = new QVBoxLayout(petPane);
  petLayout->setContentsMargins(0, 0, 0, 0);
  petTitle_ = new QLabel(QStringLiteral("点击左侧兑换项目查看对应精灵"), petPane);
  petTitle_->setWordWrap(true);
  petTitle_->setStyleSheet(QStringLiteral("font-weight:600;"));
  petTable_ = makeTable(petPane,
                        {QStringLiteral("精灵"), QStringLiteral("等级"),
                         QStringLiteral("时代"), QStringLiteral("位置"),
                         QStringLiteral("实例 ID"), QStringLiteral("持有可达战力"),
                         QStringLiteral("官方极限 / 至高")});
  petTable_->setObjectName(QStringLiteral("KQShopPetTable"));
  petTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  petTable_->horizontalHeader()->setResizeContentsPrecision(48);
  petTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  petTable_->horizontalHeader()->setSectionsClickable(true);
  petTable_->horizontalHeader()->setSortIndicatorShown(false);
  petLayout->addWidget(petTitle_);
  petLayout->addWidget(petTable_, 1);

  auto* detailTabs = new QTabWidget(horizontal);
  detailTabs_ = detailTabs;
  petDetail_ = new PetImageBrowser(detailTabs);
  petDetail_->setObjectName(QStringLiteral("KQShopPreparedDetail"));
  petDetail_->setOpenLinks(false);
  connect(petDetail_, &PetImageBrowser::imageRetryRequested, this, [this] {
    if (!repository_ || currentInstanceId_ <= 0) return;
    const QJsonObject pet = repository_->detailFor(currentInstanceId_);
    const PetMetadataView catalog(repository_->metadataSnapshot());
    ensureImageCache();
    imageCache_->ensurePetImage(pet, {petDisplayName(pet), catalog.resolvedOriginalName(pet),
        catalog.petName(petRaceId(pet))}, petDetail_->devicePixelRatioF(), true);
  });
  connect(petDetail_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    DetailSection section; int page = 0;
    if (repository_ && PreparedPetDetailRenderer::pageLink(url, &section, &page))
      repository_->requestDetailPage(1, section, page);
  });
  rawDetail_ = new PetRawDataTree(detailTabs);


  detailTabs->addTab(petDetail_, QStringLiteral("本地详情与资格分析"));
  detailTabs->addTab(rawDetail_, QStringLiteral("原始本地缓存"));
  UiPreferences::bindTabWidget(detailTabs, QStringLiteral("shop/detailTab"));
  petDetail_->setHtml(QStringLiteral(
      "<p style='color:#718096'>点击上方精灵会立即显示本地缓存；账号在线时会按实例 ID 在后台刷新一次详情。</p>"));

  left->addWidget(shopPane);
  left->addWidget(petPane);
  left->setChildrenCollapsible(false);
  left->setStretchFactor(0, 3);
  left->setStretchFactor(1, 2);
  left->setSizes({500, 310});
  UiPreferences::bindSplitter(left, QStringLiteral("shop/listingSplitter"));
  horizontal->addWidget(left);
  horizontal->addWidget(detailTabs);
  horizontal->setChildrenCollapsible(false);
  horizontal->setStretchFactor(0, 6);
  horizontal->setStretchFactor(1, 5);
  horizontal->setSizes({780, 620});
  // Only the wide layout is remembered; the compact layout toggles panes instead.
  if (const QList<int> saved = UiPreferences::intList(QStringLiteral("shop/contentSplitter")); saved.size() == 2) {
    workbenchWideSizes_ = saved;
    horizontal->setSizes(saved);
  }
  connect(horizontal, &QSplitter::splitterMoved, this, [this](int, int) {
    if (workbenchCompact_) return;
    workbenchWideSizes_ = contentSplitter_->sizes();
    UiPreferences::setIntList(QStringLiteral("shop/contentSplitter"), workbenchWideSizes_);
  });
  root->addWidget(horizontal, 1);

  auto* moveBar = new QHBoxLayout();
  moveBar->addStretch(1);
  moveToBackpack_ = new QPushButton(QStringLiteral("放入背包"), this);
  moveToBackpack_->setToolTip(
      QStringLiteral("仅仓库精灵可用；移动前后会强制刷新背包/仓库，背包已满时需手动选择交换精灵"));
  moveBar->addWidget(moveToBackpack_);
  root->addLayout(moveBar);

  connect(refresh_, &QPushButton::clicked, this, &ShopWindow::refreshRequested);
  connect(refreshCatalog_, &QPushButton::clicked, this,
          &ShopWindow::catalogRefreshRequested);
  connect(petTable_, &QTableWidget::cellClicked, this, [this](int row, int) {
    if (petRowsPreparing_) return;
    QTableWidgetItem* item = petTable_->item(row, 0);
    if (item) showPetDetail(item->data(Qt::UserRole).toLongLong(), true);
  });
  connect(petTable_->horizontalHeader(), &QHeaderView::sectionClicked, this,
          &ShopWindow::changePetSort);
  connect(moveToBackpack_, &QPushButton::clicked, this,
          &ShopWindow::moveCurrentToBackpack);
  connect(shopTabs_, &QTabWidget::currentChanged, this, [this]() {
    updateCurrencySummary();
    currentEligibility_.clear();
    currentGood_ = {};
    currentGoodKey_.clear();
    updateSelectedGoodEmphasis();
    currentInstanceId_ = 0;
    pendingDetailId_ = 0;
    currentVisualKey_.clear();
    rebuildPetRows();
    petTitle_->setText(QStringLiteral("点击左侧兑换项目查看对应精灵"));
    petDetail_->setHtml(QStringLiteral("<p style='color:#718096'>请选择兑换项目和精灵。</p>"));
    rawDetail_->clearJson();
    updateMoveButton();
  });
  if (repository_) {
    connect(repository_, &InventoryReadView::dataChanged, this, [this] { petIndexDirty_ = true; scheduleRebuild(); });
    connect(repository_, &InventoryReadView::detailChanged, this,
            &ShopWindow::updateCurrentDetail);
    connect(repository_, &InventoryReadView::accountSessionChanged, this,
            &ShopWindow::resetSessionContext);
    connect(repository_, &InventoryReadView::metadataChanged, this, [this](quint64) {
      petIndexDirty_ = true;
      currentEligibility_.clear();
      scheduleRebuild(); // Reuses selected good/instance restoration and sends no query.
    });
  }
  updateMoveButton();
  scheduleRebuild();
}

void ShopWindow::resetSessionContext() {
  cancelPetRows();
  if (repository_) repository_->watchDetail(1, 0);
  packet_ = {};
  readOnlyPackets_ = {};
  readOnlyShopPacket_ = {};
  readOnlyMaterialCounts_.clear();
  readOnlyMaterialTypes_.clear();
  quotaValidity_.clear(); quotaRevision_ = 0; quotaInitialized_ = false;
  materialCounts_.clear();
  hasPacket_ = false;
  hasMaterialCounts_ = false;
  currentGood_ = {};
  currentGoodKey_.clear(); highlightedGoodKey_.clear(); goodLocations_.clear();
  activityGoods_.clear();
  currentEligibility_.clear();
  eligiblePetIdsByRace_.clear();
  petRowCache_.clear(); defaultPetOrder_.clear(); eligibleOrderByGood_.clear(); petIndexDirty_ = true;
  currentInstanceId_ = 0;
  pendingDetailId_ = 0;
  pendingFocusGoodKey_.clear();
  currentVisualKey_.clear();
  moveRunning_ = false;
  petTable_->clearSelection();
  petTable_->setRowCount(0);
  for (QTableWidget* table : shopTabs_->findChildren<QTableWidget*>()) {
    table->clearSelection();
    table->setRowCount(0);
  }
  shopRows_.clear();
  petTitle_->setText(QStringLiteral("请选择当前账号的兑换项目和精灵"));
  petDetail_->setHtml(QStringLiteral("<p style='color:#718096'>会话已更新，请重新选择精灵。</p>"));
  rawDetail_->clearJson();
  status_->setText(QStringLiteral("会话已更新，次数与资源等待当前账号确认"));
  compactDetails_ = false;
  updateMoveButton();
  updateCurrencySummary();
  updateWorkbenchPanels();
}

bool ShopWindow::event(QEvent* event) {
  const bool result = QDialog::event(event);
  if (imageCache_ && (event->type() == QEvent::Show || event->type() == QEvent::ScreenChangeInternal ||
                     event->type() == QEvent::DevicePixelRatioChange))
    imageCache_->setDevicePixelRatio(devicePixelRatioF());
  return result;
}

void ShopWindow::setWorkbenchMode(bool embedded, bool compact) {
  if (workbenchEmbedded_ == embedded && workbenchCompact_ == compact) return;
  if (!workbenchCompact_ && embedded && compact) workbenchWideSizes_ = contentSplitter_->sizes();
  workbenchEmbedded_ = embedded;
  workbenchCompact_ = embedded && compact;
  compactDetails_ = false;
  setMinimumSize(embedded ? QSize(0, 0) : QSize(1100, 680));
  if (layout()) layout()->setContentsMargins(embedded ? 6 : 11, embedded ? 4 : 11,
                                            embedded ? 6 : 11, embedded ? 4 : 11);
  while (QLayoutItem* item = toolbarLayout_->takeAt(0)) delete item;
  toolbarLayout_->setColumnStretch(2, 0);
  toolbarLayout_->setColumnStretch(3, 0);
  toolbarLayout_->addWidget(refresh_, 0, 0);
  toolbarLayout_->addWidget(refreshCatalog_, 0, 1);
  toolbarLayout_->addWidget(sourceFilter_, 0, 2);
  toolbarLayout_->addWidget(status_, workbenchCompact_ ? 1 : 0, workbenchCompact_ ? 0 : 3,
                             1, workbenchCompact_ ? 3 : 1);
  toolbarLayout_->setColumnStretch(workbenchCompact_ ? 1 : 3, 1);
  status_->setWordWrap(embedded);
  status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  for (QTableWidget* table : findChildren<QTableWidget*>()) {
    table->setMinimumWidth(0);
    table->setTextElideMode(embedded ? Qt::ElideRight : Qt::ElideNone);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  }
  petTable_->setColumnHidden(2, workbenchCompact_);
  petTable_->setColumnHidden(4, workbenchCompact_);
  petTable_->setColumnHidden(6, workbenchCompact_);
  updateWorkbenchPanels();
  if (!workbenchCompact_) contentSplitter_->setSizes(workbenchWideSizes_.isEmpty() ? QList<int>{680, 340} : workbenchWideSizes_);
}

void ShopWindow::updateWorkbenchPanels() {
  if (!detailToggle_ || !contentSplitter_) return;
  detailToggle_->setVisible(workbenchCompact_);
  detailToggle_->setText(compactDetails_ ? QStringLiteral("返回商品与精灵列表") : QStringLiteral("查看精灵详情"));
  listingSplitter_->setVisible(!workbenchCompact_ || !compactDetails_);
  detailTabs_->setVisible(!workbenchCompact_ || compactDetails_);
}

void ShopWindow::setPacket(const QJsonObject& packet, bool hasPacket) {
  if (packet_ == packet && hasPacket_ == hasPacket) return;
  packet_ = packet;
  hasPacket_ = hasPacket;
  scheduleRebuild();
}

void ShopWindow::setMaterialCounts(const QHash<QString, qint64>& counts, bool valid) {
  if (materialCounts_ == counts && hasMaterialCounts_ == valid) return;
  materialCounts_ = counts;
  hasMaterialCounts_ = valid;
  scheduleRebuild();
}

void ShopWindow::setReadOnlyObservations(const QJsonObject& packets) {
  if (readOnlyPackets_ == packets) return;
  readOnlyPackets_ = packets;
  readOnlyShopPacket_ = {};
  readOnlyMaterialCounts_.clear();
  readOnlyMaterialTypes_.clear();
  for (auto packet = packets.begin(); packet != packets.end(); ++packet) {
    const QJsonObject object = packet.value().toObject();
    if (packet.key() == QStringLiteral("3_11")) {
      for (auto group = object.begin(); group != object.end(); ++group) {
        if (!group.value().isObject()) continue;
        readOnlyMaterialTypes_.insert(group.key());
        const auto entries = group.value().toObject();
        for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
          bool valid = false;
          const qint64 count = entry.value().toString().toLongLong(&valid);
          if (valid && count >= 0)
            readOnlyMaterialCounts_.insert(group.key() + QLatin1Char(':') + entry.key(), count);
        }
      }
    } else {
      for (auto group = object.begin(); group != object.end(); ++group)
        if (group.key().startsWith(QStringLiteral("si")) && group.value().isObject())
          readOnlyShopPacket_.insert(group.key(), group.value());
    }
  }
  scheduleRebuild();
}

void ShopWindow::setCatalogSnapshot(std::shared_ptr<const ShopCatalogSnapshot> catalog, QDate businessDate) {
  if (catalogSnapshot_ == catalog && catalogDate_ == businessDate) return;
  catalogSnapshot_ = std::move(catalog);
  catalogDate_ = businessDate;
  visibleShops_.clear();
  QList<ShopExchangeGood> goods;
  if (catalogSnapshot_ && catalogDate_.isValid()) {
    visibleShops_.reserve(catalogSnapshot_->allShops.size());
    for (const auto& source : catalogSnapshot_->allShops) {
      if ((sourceFilter_->currentIndex() == 1 && !source.sourceKey.isEmpty()) ||
          (sourceFilter_->currentIndex() == 2 && source.sourceKey.isEmpty())) continue;
      ShopExchangeShop shop;
      shop.shopId = source.shopId; shop.name = source.name; shop.siKey = source.siKey; shop.sourceKey = source.sourceKey;
      shop.observation = source.observation;
      for (const auto& good : source.goods) if (good.isOnlineOn(catalogDate_)) {
        shop.goods.append(good); goods.append(good);
      }
      visibleShops_.append(std::move(shop));
    }
  }
  compiledCatalog_ = CompiledShopCatalog::compile(goods);
  compiledGoodsByKey_.clear();
  for (qsizetype index = 0; index < compiledCatalog_.goods().size(); ++index)
    compiledGoodsByKey_.insert(compiledCatalog_.goods().at(index).stableKey,index);
  scheduleRebuild();
}

void ShopWindow::setQuotaValidity(quint64 revision, const QHash<QString, ShopCondition>& validity) {
  if (quotaInitialized_ && quotaRevision_ == revision) return;
  quotaInitialized_ = true; quotaRevision_ = revision; quotaValidity_ = validity;
  scheduleRebuild();
}

void ShopWindow::scheduleRebuild() {
  if (rebuildScheduled_) return;
  rebuildScheduled_ = true;
  QTimer::singleShot(0, this, [this]() {
    rebuildScheduled_ = false;
    rebuild();
  });
}

void ShopWindow::setStatus(const QString& status, const QString& details) {
  status_->setText(status);
  status_->setToolTip(details.isEmpty() ? status : details);
}

void ShopWindow::setRefreshRunning(bool running) {
  refresh_->setEnabled(!running);
  refresh_->setText(running ? QStringLiteral("查询中…") : QStringLiteral("刷新兑换次数"));
}

void ShopWindow::focusGood(const QString& stableKey) {
  if (stableKey.isEmpty()) return;
  const bool activity = stableKey.startsWith(QStringLiteral("activity:"));
  if ((sourceFilter_->currentIndex() == 1 && activity) || (sourceFilter_->currentIndex() == 2 && !activity)) {
    pendingFocusGoodKey_ = stableKey; sourceFilter_->setCurrentIndex(0); return;
  }
  const auto location = goodLocations_.constFind(stableKey);
  if (location != goodLocations_.cend()) {
      const int shopIndex = location->first;
      const GoodRow& row = shopRows_.at(shopIndex).at(location->second);
      pendingFocusGoodKey_.clear();
      if (shopTabs_->currentIndex() != shopIndex)
        shopTabs_->setCurrentIndex(shopIndex);
      auto* table = qobject_cast<QTableWidget*>(shopTabs_->widget(shopIndex));
      if (table && table->isRowHidden(row.tableRow) && goodSearch_) {
        const QSignalBlocker blocker(goodSearch_);
        goodSearch_->clear();
        applyGoodSearch(false);
      }
      if (table) {
        table->selectRow(row.tableRow);
        table->scrollToItem(table->item(row.tableRow, 0),
                            QAbstractItemView::PositionAtCenter);
      }
      showGoodPets(row.good,row.stableKey);
      return;
  }
  if (shopRows_.isEmpty()) {
    pendingFocusGoodKey_ = stableKey;
    scheduleRebuild();
  } else {
    pendingFocusGoodKey_.clear();
    setStatus(QStringLiteral("建议中的兑换项目已不在当前真实商店目录中"));
  }
}

const CompiledShopGood* ShopWindow::compiledGood(const ShopExchangeGood& good) const {
  const auto found = compiledGoodsByKey_.constFind(&good == &currentGood_ ? currentGoodKey_ : good.stableKey());
  return found == compiledGoodsByKey_.cend() ? nullptr : &compiledCatalog_.goods().at(found.value());
}

QString ShopWindow::costText(const ShopExchangeGood& good) const {
  if (!good.sourceKey.isEmpty()) {
    const auto cached = activityGoods_.constFind(&good == &currentGood_ ? currentGoodKey_ : good.stableKey());
    if (cached != activityGoods_.cend()) return cached->costText;
    if (good.cost.isEmpty()) return good.costDescription.isEmpty() ? QStringLiteral("活动内查看") : good.costDescription;
  }
  const auto* compiled = compiledGood(good);
  if (!compiled || compiled->costCondition.effectiveState() != ShopConditionState::Satisfied)
    return good.cost.isEmpty() ? QStringLiteral("成本待确认（目录未明确免费）")
        : QStringLiteral("成本待确认：%1").arg(good.cost);
  if (compiled->requirements.isEmpty()) return QStringLiteral("无需资源（目录已确认）");
  QStringList costs;
  for (const auto& requirement : compiled->requirements)
    costs.append(QStringLiteral("%1 ×%2").arg(resourceName(requirement,materialMetadata_)).arg(requirement.required));
  return costs.join(QStringLiteral(" ＋ "));
}

ShopWindow::ResourceStatus ShopWindow::resourceStatus(const ShopExchangeGood& good) const {
  struct Balance { QString name; qint64 required = 0; qint64 owned = -1; bool confirmed = false; };
  QList<Balance> balances;
  const auto addStandard = [&](const QList<ResourceRequirement>& requirements) {
    for (const auto& requirement : requirements) {
      const QString& key = requirement.resourceKey;
      const bool readOnly = readOnlyMaterialTypes_.contains(key.section(QLatin1Char(':'), 0, 0));
      const auto& counts = readOnly ? readOnlyMaterialCounts_ : materialCounts_;
      const bool known = (readOnly || hasMaterialCounts_) && counts.contains(key) && counts.value(key) >= 0;
      balances.append({resourceName(requirement, materialMetadata_), requirement.required,
                       known ? counts.value(key) : -1, !readOnly});
    }
  };
  const QString pending = QStringLiteral("只判断一次兑换所需的资源数量；兑换次数和解锁条件请另外确认。");
  if (!good.sourceKey.isEmpty()) {
    const auto cached = activityGoods_.constFind(&good == &currentGood_ ? currentGoodKey_ : good.stableKey());
    if (cached == activityGoods_.cend())
      return {QStringLiteral("未读取"), QStringLiteral("点击“刷新兑换次数”读取活动价格和余额。"), {}};
    if (!cached->observation.priceKnown && !good.priceOptions.isEmpty())
      return {QStringLiteral("价格档位未读取"),
              QStringLiteral("该商品按活动购买次数分档计价，刷新兑换次数后才能判断资源是否足够。"), {}};
    if (cached->observation.priceKnown) addStandard(cached->currencyRequirements);
    for (const auto& cost : cached->observation.costs)
      balances.append({cost.name, cost.required, cost.ownedKnown ? cost.owned : -1, cost.current});
    if (balances.isEmpty())
      return good.provenFree ? ResourceStatus{QStringLiteral("无需资源"), pending, QStringLiteral("#087a43")}
                             : ResourceStatus{QStringLiteral("活动内查看"), QStringLiteral("活动没有提供可读取的价格。"), {}};
  } else {
    const auto* compiled = compiledGood(good);
    if (!compiled || compiled->costCondition.effectiveState() != ShopConditionState::Satisfied)
      return {QStringLiteral("成本待确认"), QStringLiteral("兑换目录没有给出可以解析的价格。"), {}};
    if (compiled->requirements.isEmpty())
      return {QStringLiteral("无需资源"), pending, QStringLiteral("#087a43")};
    addStandard(compiled->requirements);
  }

  QStringList lines, shortages;
  bool unknown = false, unconfirmed = false;
  for (const auto& balance : balances) {
    if (balance.owned < 0) {
      unknown = true;
      lines.append(QStringLiteral("%1：需要 %2，拥有 未读取").arg(balance.name).arg(balance.required));
      continue;
    }
    unconfirmed |= !balance.confirmed;
    const qint64 missing = qMax<qint64>(0, balance.required - balance.owned);
    lines.append(missing > 0
        ? QStringLiteral("%1：需要 %2，拥有 %3，还差 %4").arg(balance.name).arg(balance.required).arg(balance.owned).arg(missing)
        : QStringLiteral("%1：需要 %2，拥有 %3").arg(balance.name).arg(balance.required).arg(balance.owned));
    if (missing > 0) shortages.append(QStringLiteral("%1 %2").arg(balance.name).arg(missing));
  }
  if (unconfirmed) lines.append(QStringLiteral("部分余额是上次读取的数值，可能已变化。"));
  if (unknown) lines.append(QStringLiteral("点击“刷新兑换次数”可读取余额。"));
  lines.append(pending);
  const QString tip = lines.join(QLatin1Char('\n'));
  const QString suffix = unconfirmed ? QStringLiteral("（待确认）") : QString{};
  if (!shortages.isEmpty())
    return {QStringLiteral("还差 %1%2").arg(shortages.join(QStringLiteral("、")), suffix), tip, QStringLiteral("#b54708")};
  if (unknown) return {QStringLiteral("余额未读取"), tip, {}};
  // Only confirmed balances earn the green "enough" colour.
  return {QStringLiteral("足够%1").arg(suffix), tip, unconfirmed ? QString{} : QStringLiteral("#087a43")};
}

void ShopWindow::applyGoodSearch(bool selectMatchingTab) {
  const PetSearchQuery query = preparePetSearchQuery(goodSearch_ ? goodSearch_->text().trimmed() : QString{});
  int firstMatchingTab = -1;
  bool currentHasMatch = false;
  for (int shopIndex = 0; shopIndex < shopRows_.size() && shopIndex < shopTabs_->count(); ++shopIndex) {
    auto* table = qobject_cast<QTableWidget*>(shopTabs_->widget(shopIndex));
    if (!table) continue;
    int matches = 0;
    for (const GoodRow& row : shopRows_.at(shopIndex)) {
      const bool visible = query.needle.isEmpty() ||
          petQueryMatches(query, preparePetSearchIndex({row.good.description, row.good.shopName}));
      table->setRowHidden(row.tableRow, !visible);
      if (visible) ++matches;
    }
    shopTabs_->tabBar()->setTabTextColor(shopIndex, matches > 0 || query.needle.isEmpty()
        ? QColor{} : QColor(QStringLiteral("#9aa5b1")));
    if (matches > 0 && firstMatchingTab < 0) firstMatchingTab = shopIndex;
    if (matches > 0 && shopIndex == shopTabs_->currentIndex()) currentHasMatch = true;
  }
  if (selectMatchingTab && !currentHasMatch && firstMatchingTab >= 0) shopTabs_->setCurrentIndex(firstMatchingTab);
}

void ShopWindow::rebuildActivityGoods(const QJsonObject& packet) {
  activityGoods_.clear();
  for (const auto& compiled : compiledCatalog_.goods()) {
    const auto& good = compiled.good;
    if (good.sourceKey.isEmpty()) continue;
    CachedActivityGood value;
    value.observation = observeActivityShopGood(good,packet);
    activityObservedValue(good,good.quotaObservation,packet,nullptr,&value.quotaVerified);
    const auto parse = [&good](const QString& cost) {
      auto priced = good; priced.cost = cost;
      return CompiledShopCatalog::compile({priced});
    };
    QStringList costs;
    if (value.observation.priceKnown && (!value.observation.standardCost.isEmpty() || good.provenFree)) {
      const auto current = parse(value.observation.standardCost);
      const auto& price = current.goods().first();
      if (price.costCondition.effectiveState() == ShopConditionState::Satisfied) {
        value.currencyRequirements = price.requirements;
        for (const auto& requirement : price.requirements)
          costs.append(QStringLiteral("%1 ×%2").arg(resourceName(requirement,materialMetadata_)).arg(requirement.required));
        if (price.requirements.isEmpty()) costs.append(QStringLiteral("无需资源（目录已确认）"));
      } else costs.append(QStringLiteral("成本待确认：%1").arg(value.observation.standardCost));
      if (!good.priceOptions.isEmpty() && !value.observation.priceCurrent) costs.prepend(QStringLiteral("上次读取的档位"));
    } else if (!good.priceOptions.isEmpty()) {
      costs.append(good.costDescription.isEmpty() ? QStringLiteral("当前价格档位未读取") : good.costDescription);
      // Price tiers can differ in quantity while using the same currencies.
      // Retain their currency identities even before an active tier is known.
      for (const auto& option : good.priceOptions) {
        const auto parsed = parse(option.toObject().value(QStringLiteral("cost")).toString());
        if (parsed.goods().first().costCondition.effectiveState() == ShopConditionState::Satisfied)
          value.currencyRequirements.append(parsed.goods().first().requirements);
      }
    }
    for (const auto& cost : value.observation.costs)
      costs.append(QStringLiteral("%1 ×%2").arg(cost.name).arg(cost.required));
    if (costs.isEmpty()) costs.append(good.costDescription.isEmpty() ? QStringLiteral("活动内查看") : good.costDescription);
    value.costText = costs.join(QStringLiteral(" ＋ "));
    activityGoods_.insert(compiled.stableKey,std::move(value));
  }
}

QString ShopWindow::shopCurrencyText(const ShopExchangeShop& shop) const {
  QSet<QString> seen;
  QStringList currencies;
  bool unknownCosts = false, confirmedFree = false;
  const auto addStandard = [&](const QList<ResourceRequirement>& requirements) {
    for (const auto& requirement : requirements) {
      const QString& key = requirement.resourceKey;
      if (seen.contains(key)) continue;
      seen.insert(key);
      const QString name = resourceName(requirement,materialMetadata_);
      const bool readOnly = readOnlyMaterialTypes_.contains(key.section(QLatin1Char(':'), 0, 0));
      const auto& counts = readOnly ? readOnlyMaterialCounts_ : materialCounts_;
      currencies.append(QStringLiteral("%1 %2").arg(name,(readOnly || hasMaterialCounts_) && counts.contains(key) && counts.value(key) >= 0
          ? QString::number(counts.value(key)) : QStringLiteral("未读取")));
    }
  };
  for (const ShopExchangeGood& good : shop.goods) {
    if (!good.sourceKey.isEmpty()) {
      const auto value = activityGoods_.constFind(good.stableKey());
      if (value == activityGoods_.cend()) { unknownCosts = true; continue; }
      addStandard(value->currencyRequirements);
      unknownCosts |= !value->observation.priceKnown;
      confirmedFree |= good.provenFree;
      for (const auto& cost : value->observation.costs) {
        const QString key = cost.key + QChar(0x1f) + cost.name;
        if (seen.contains(key)) continue;
        seen.insert(key);
        currencies.append(QStringLiteral("%1 %2").arg(cost.name,cost.ownedKnown
            ? QString::number(cost.owned) + (cost.current ? QString{} : QStringLiteral("（待确认）")) : QStringLiteral("未读取")));
      }
      continue;
    }
    const auto* compiled = compiledGood(good);
    if (!compiled || compiled->costCondition.effectiveState() != ShopConditionState::Satisfied) {
      unknownCosts = true; continue;
    }
    confirmedFree |= compiled->requirements.isEmpty();
    addStandard(compiled->requirements);
  }
  if (unknownCosts) currencies.append(QStringLiteral("部分项目成本待确认"));
  if (currencies.isEmpty() && confirmedFree) return QStringLiteral("无需资源（目录已确认）");
  return currencies.join(QStringLiteral(" / "));
}

void ShopWindow::rebuild() {
  cancelPetRows();
  QJsonObject observedPacket = packet_;
  for (auto group = readOnlyShopPacket_.begin(); group != readOnlyShopPacket_.end(); ++group)
    observedPacket.insert(group.key(), group.value());
  materialMetadata_ = PetMetadataView(repository_ ? repository_->metadataSnapshot() : nullptr);
  rebuildActivityGoods(observedPacket);
  setProperty("rebuildCount", property("rebuildCount").toInt() + 1);
  const int current = shopTabs_->currentIndex();
  const QString selectedGoodKey = currentGoodKey_;
  const qint64 selectedInstanceId = currentInstanceId_;
  const bool detailWasPending = pendingDetailId_ == selectedInstanceId;
  QHash<QString, QPair<int, int>> previousScrolls;
  for (QTableWidget* table : shopTabs_->findChildren<QTableWidget*>())
    previousScrolls.insert(table->objectName(), {table->verticalScrollBar()->value(), table->horizontalScrollBar()->value()});
  const QPair<int, int> petScroll{petTable_->verticalScrollBar()->value(), petTable_->horizontalScrollBar()->value()};
  const QPair<int, int> detailScroll{petDetail_->verticalScrollBar()->value(), petDetail_->horizontalScrollBar()->value()};
  const QSignalBlocker tabSignals(shopTabs_);
  while (shopTabs_->count() > 0) {
    QWidget* page = shopTabs_->widget(0);
    shopTabs_->removeTab(0);
    delete page;
  }
  shopRows_.clear();
  goodLocations_.clear(); highlightedGoodKey_.clear();
  rebuildEligiblePetIndex();
  const auto& shops = visibleShops_;
  shopRows_.reserve(shops.size());
  for (const ShopExchangeShop& shop : shops) {
    auto* table = makeTable(shopTabs_,
                            {QStringLiteral("兑换项目"), QStringLiteral("所需资源"),
                             QStringLiteral("限次"), QStringLiteral("剩余"),
                             QStringLiteral("对应个体"), QStringLiteral("资源情况")});
    table->setObjectName(shop.sourceKey.isEmpty() ? QStringLiteral("KQShopGoodsTable-%1").arg(shop.shopId)
        : QStringLiteral("KQActivityGoodsTable-%1-%2").arg(shop.sourceKey).arg(shop.shopId));
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 220);
    table->setColumnWidth(2, 110);
    table->setColumnWidth(3, 90);
    table->setColumnWidth(4, 90);
    table->setColumnWidth(5, 170);
    QList<GoodRow> rows;
    table->setRowCount(shop.goods.size());
    int row = 0;
    for (const ShopExchangeGood& good : shop.goods) {
      const bool readOnly = good.sourceKey.isEmpty() && readOnlyShopPacket_.contains(QStringLiteral("si%1").arg(good.shopId));
      const int remaining = hasPacket_ || readOnly ? shopRemainingCount(observedPacket, good) : -1;
      table->setItem(row, 0, textItem(good.description));
      table->setItem(row, 1, textItem(costText(good)));
      const auto period = quotaValidity_.value(shopQuotaValidityKey(good));
      const bool current = !readOnly && period.state == ShopConditionState::Satisfied && period.freshness == ShopConditionFreshness::Current;
      table->setItem(row, 2, textItem(good.provenUnlimited ? QStringLiteral("不限次")
          : good.limitCount > 0 ? QStringLiteral("%1限 %2 次").arg(good.limitLabel).arg(good.limitCount) : QStringLiteral("未标注")));
      QTableWidgetItem* quota = good.provenUnlimited ? textItem(QStringLiteral("不限次"))
          : remaining < 0 ? textItem(QStringLiteral("未查询"))
          : textItem(QStringLiteral("%1 / %2").arg(remaining).arg(good.limitCount) +
                     (current ? QString{} : QStringLiteral("（待确认）")), current && remaining > 0);
      if (!good.provenUnlimited && !current) {
        quota->setForeground(QBrush(QColor(QStringLiteral("#718096"))));
        quota->setToolTip(period.reason.isEmpty() ? QStringLiteral("次数周期未确认，数值是上次读取的结果，可能已变化") : period.reason);
      }
      table->setItem(row, 3, quota);
      if (!good.sourceKey.isEmpty()) {
        const QString quotaRequest = good.quotaObservation.value(QStringLiteral("requestKey")).toString();
        const bool hasQuotaRequest = !quotaRequest.isEmpty() && good.activityQueries.value(quotaRequest).isObject();
        const auto cached = activityGoods_.constFind(good.stableKey());
        const auto observation = cached == activityGoods_.cend() ? ActivityShopObservation{} : cached->observation;
        if (observation.applicabilityKnown && !observation.applicable) quota->setText(QStringLiteral("不适用当前活动等级"));
        else if (good.provenUnlimited) quota->setText(QStringLiteral("不限次"));
        else if (observation.quotaKnown) quota->setText(QStringLiteral("%1 / %2").arg(observation.remaining).arg(good.limitCount) +
            (observation.historical ? QStringLiteral("（上次）") :
             cached != activityGoods_.cend() && cached->quotaVerified ? QString{} : QStringLiteral("（待确认）")));
        else quota->setText(!hasQuotaRequest ? QStringLiteral("需在活动中查看") : QStringLiteral("未查询"));
        quota->setToolTip(observation.observedAt.isValid()
            ? QStringLiteral("对应活动的独立次数，数据时间：%1").arg(observation.observedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
            : !hasQuotaRequest ? QStringLiteral("官方项目保留限次总额；对应活动没有可自动读取的次数入口") : QStringLiteral("对应活动的兑换次数尚未读取"));
        auto* title = table->item(row,0);
        title->setToolTip(good.shopName + (good.shelfDate.isValid() ? QString{} : QStringLiteral("\n活动配置未标注起始日期")));
      }
      table->setItem(row, 4, textItem(QString::number(eligiblePetCount(good))));
      const ResourceStatus balance = resourceStatus(good);
      auto* balanceItem = textItem(balance.text);
      balanceItem->setToolTip(balance.tip);
      if (!balance.color.isEmpty()) balanceItem->setForeground(QBrush(QColor(balance.color)));
      table->setItem(row, 5, balanceItem);
      const QString stableKey = good.stableKey();
      goodLocations_.insert(stableKey,{int(shopRows_.size()),row});
      rows.append({good, row, stableKey});
      ++row;
    }
    table->setProperty("shopIndex", shopRows_.size());
    connect(table, &QTableWidget::cellClicked, this, [this, table](int row, int) {
      const int shopIndex = table->property("shopIndex").toInt();
      if (shopIndex < 0 || shopIndex >= shopRows_.size() || row < 0 ||
          row >= shopRows_.at(shopIndex).size()) return;
      const auto& selected = shopRows_.at(shopIndex).at(row);
      showGoodPets(selected.good,selected.stableKey);
    });
    const QString currency = shopCurrencyText(shop);
    table->setProperty("currencyText",currency);
    const int tabIndex = shopTabs_->addTab(table, shop.name);
    shopTabs_->setTabToolTip(tabIndex,
                             currency.isEmpty() ? shop.name
                                                : QStringLiteral("%1｜账号拥有：%2")
                                                      .arg(shop.name, currency));
    shopRows_.append(rows);
  }
  if (current >= 0 && current < shopTabs_->count()) shopTabs_->setCurrentIndex(current);
  applyGoodSearch(false);
  updateCurrencySummary();

  bool restoredGood = false;
  const auto restoredLocation = goodLocations_.constFind(selectedGoodKey);
  if (restoredLocation != goodLocations_.cend()) {
        const auto& row = shopRows_.at(restoredLocation->first).at(restoredLocation->second);
        petScrollAfterRows_ = petScroll; detailScrollAfterRows_ = detailScroll;
        pendingDetailId_ = detailWasPending ? selectedInstanceId : 0;
        showGoodPets(row.good,row.stableKey,selectedInstanceId,true);
        restoredGood = true;
  }
  updateMoveButton();
  if (restoredGood) {
    for (QTableWidget* table : shopTabs_->findChildren<QTableWidget*>()) {
      const auto saved = previousScrolls.constFind(table->objectName());
      if (saved == previousScrolls.cend()) continue;
      table->verticalScrollBar()->setValue(saved->first);
      table->horizontalScrollBar()->setValue(saved->second);
    }
    petTable_->verticalScrollBar()->setValue(petScroll.first);
    petTable_->horizontalScrollBar()->setValue(petScroll.second);
    if (selectedInstanceId > 0 && selectedInstanceId == currentInstanceId_) {
      petDetail_->verticalScrollBar()->setValue(detailScroll.first);
      petDetail_->horizontalScrollBar()->setValue(detailScroll.second);
    }
  }
  if (!restoredGood && !selectedGoodKey.isEmpty()) {
    showGoodPets({});
    petTitle_->setText(QStringLiteral("该兑换项目已下架或目录已更新，请重新选择"));
  }
  if (!pendingFocusGoodKey_.isEmpty()) {
    const QString key = pendingFocusGoodKey_;
    pendingFocusGoodKey_.clear();
    focusGood(key);
  }
}

void ShopWindow::rebuildEligiblePetIndex() {
  if (!petIndexDirty_) return;
  petIndexDirty_ = false; ++petIndexBuilds_;
  eligiblePetIdsByRace_.clear();
  petRowCache_.clear(); defaultPetOrder_.clear(); eligibleOrderByGood_.clear();
  if (!repository_) return;
  const auto indexPets = [this](const QList<QJsonObject>& pets) {
    for (const QJsonObject& pet : pets) {
      const qint64 id = petInstanceId(pet);
      if (id <= 0 || petRowCache_.contains(id)) continue;
      CachedPetRow row; row.brief = PetFactsUi::rowSummary(pet); row.name = petDisplayName(row.brief);
      row.backpack = row.brief.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack");
      petRowCache_.insert(id,std::move(row));
      const int currentRace = petRaceId(pet);
      if (currentRace > 0) eligiblePetIdsByRace_[currentRace].insert(id);
      const int metadataRace = pet.value(QStringLiteral("_metaRaceId")).toInt();
      if (metadataRace > 0) eligiblePetIdsByRace_[metadataRace].insert(id);
    }
  };
  indexPets(repository_->backpackPets());
  indexPets(repository_->warehousePets());
  auto ids = petRowCache_.keys();
  std::sort(ids.begin(),ids.end(),[this](qint64 left,qint64 right) {
    const auto a = petRowCache_.constFind(left), b = petRowCache_.constFind(right);
    if (a->backpack != b->backpack) return a->backpack;
    const int order = QString::localeAwareCompare(a->name,b->name);
    return order ? order < 0 : left < right;
  });
  for (int index = 0; index < ids.size(); ++index) defaultPetOrder_.insert(ids[index],index);
}

int ShopWindow::eligiblePetCount(const ShopExchangeGood& good) const {
  QSet<qint64> ids;
  for (int raceId : good.raceIds) ids.unite(eligiblePetIdsByRace_.value(raceId));
  return ids.size();
}

void ShopWindow::updateCurrencySummary() {
  if (!currencySummary_) return;
  const auto& shops = visibleShops_;
  const int index = shopTabs_ ? shopTabs_->currentIndex() : -1;
  if (index < 0 || index >= shops.size()) {
    currencySummary_->setText(!catalogDate_.isValid() ? QStringLiteral("当前商店资源：正在确定商店日期")
        : QStringLiteral("当前商店资源：无可用商店"));
    return;
  }
  const auto cached = shopTabs_->widget(index)->property("currencyText");
  const QString currency = cached.isValid() ? cached.toString() : shopCurrencyText(shops.at(index));
  currencySummary_->setText(
      currency.isEmpty()
          ? QStringLiteral("%1 · 暂无资源信息").arg(shops.at(index).name)
          : QStringLiteral("%1 · 账号拥有：%2").arg(shops.at(index).name, currency));
}

void ShopWindow::ensureImageCache() {
  if (!imageCache_) imageCache_ = new PetImageCache(repository_ ? repository_->dataRoot() : QString(), this);
  connect(imageCache_, &PetImageCache::petImageReady, this,
          &ShopWindow::updateCurrentImage, Qt::UniqueConnection);
}

QList<qint64> ShopWindow::eligiblePets(const ShopExchangeGood& good) {
  const QString key = &good == &currentGood_ ? currentGoodKey_ : good.stableKey();
  const auto cached = eligibleOrderByGood_.constFind(key);
  if (cached != eligibleOrderByGood_.cend()) { ++petMembershipCacheHits_; return cached.value(); }
  QSet<qint64> ids;
  for (int race : good.raceIds) ids.unite(eligiblePetIdsByRace_.value(race));
  QList<qint64> result = ids.values();
  std::sort(result.begin(),result.end(),[this](qint64 a,qint64 b) { return defaultPetOrder_.value(a) < defaultPetOrder_.value(b); });
  eligibleOrderByGood_.insert(key,result);
  return result;
}

void ShopWindow::showGoodPets(const ShopExchangeGood& good, const QString& stableKey, qint64 preserveInstanceId, bool restoreScroll) {
  const QString key = good.hasIdentity() ? (stableKey.isEmpty() ? good.stableKey() : stableKey) : QString();
  if (!restoreScroll && !key.isEmpty() && key == currentGoodKey_) { updateSelectedGoodEmphasis(); return; }
  const bool pendingDetail = preserveInstanceId > 0 && pendingDetailId_ == preserveInstanceId;
  if (repository_) repository_->watchDetail(1, 0);
  currentGood_ = good;
  currentGoodKey_ = key;
  updateSelectedGoodEmphasis();
  currentEligibility_.clear();
  currentInstanceId_ = 0;
  pendingDetailId_ = pendingDetail ? preserveInstanceId : 0;
  currentVisualKey_.clear();
  petDetail_->setHtml(QStringLiteral("<p style='color:#718096'>点击左下方精灵会立即显示本地缓存，并按实例 ID 在后台刷新一次详情。</p>"));
  rawDetail_->clearJson();
  restoreScrollAfterRows_ = restoreScroll;
  rebuildPetRows(preserveInstanceId);
  updatePetTitle();
  updateMoveButton();
  compactDetails_ = false;
  updateWorkbenchPanels();
}

void ShopWindow::updateSelectedGoodEmphasis() {
  const auto emphasize = [this](const QString& key,bool selected) {
    const auto found = goodLocations_.constFind(key);
    if (found == goodLocations_.cend()) return;
    auto* table = qobject_cast<QTableWidget*>(shopTabs_->widget(found->first));
    if (!table) return;
    if (auto* name = table->item(found->second,0)) {
      QFont font = name->font();
      font.setBold(selected);
      name->setFont(font);
      if (selected) table->selectRow(found->second);
    }
  };
  if (highlightedGoodKey_ != currentGoodKey_) emphasize(highlightedGoodKey_,false);
  emphasize(currentGoodKey_,true); highlightedGoodKey_ = currentGoodKey_;
}

void ShopWindow::rebuildPetRows(qint64 preserveInstanceId) {
  cancelPetRows();
  auto pets = currentGood_.hasIdentity() ? eligiblePets(currentGood_) : QList<qint64>{};
  if (petSortColumn_ == 1 || petSortColumn_ == 5 || petSortColumn_ == 6) {
    const bool ascending = petSortAscending_;
    QHash<qint64,qint64> values;
    for (qint64 id : pets) {
      const auto& data = cachedPetRow(id);
      values.insert(id,petSortColumn_ == 1 ? data.level : petSortColumn_ == 5 ? data.power : data.highest);
    }
    std::sort(pets.begin(), pets.end(), [&values, ascending](qint64 left,qint64 right) {
      const qint64 leftValue = values.value(left,-1);
      const qint64 rightValue = values.value(right,-1);
      if ((leftValue < 0) != (rightValue < 0)) return leftValue >= 0;
      if (leftValue != rightValue)
        return ascending ? leftValue < rightValue : leftValue > rightValue;
      return left < right;
    });
  }
  currentEligibility_.clear();
  pendingPetIds_ = std::move(pets); pendingPetRow_ = 0; preservePetId_ = preserveInstanceId;
  synchronousPetRows_ = pendingPetIds_.size() <= 32 && petTable_->rowCount() <= 32;
  petRowsPreparing_ = true; petTable_->setEnabled(false);
  const auto generation = petRowsGeneration_;
  if (synchronousPetRows_) continuePetRows(generation);
  else QTimer::singleShot(0,this,[this,generation] { continuePetRows(generation); });
}

void ShopWindow::cancelPetRows() {
  ++petRowsGeneration_; petRowsPreparing_ = false; pendingPetIds_.clear(); pendingPetRow_ = 0;
  preservePetId_ = 0; visiblePetRows_.clear();
  petTable_->setEnabled(true);
}

void ShopWindow::continuePetRows(quint64 generation) {
  if (generation != petRowsGeneration_ || !petRowsPreparing_) return;
  QElapsedTimer slice; slice.start();
  const QSignalBlocker blockedSignals(petTable_);
  petTable_->setUpdatesEnabled(false);
  int units = 0;
  while (petTable_->rowCount() > pendingPetIds_.size() && (synchronousPetRows_ || (units < 32 && slice.elapsed() < 4))) {
    petTable_->removeRow(petTable_->rowCount()-1); ++units;
  }
  if (petTable_->rowCount() <= pendingPetIds_.size()) {
    if (petTable_->rowCount() < pendingPetIds_.size()) petTable_->setRowCount(pendingPetIds_.size());
    while (pendingPetRow_ < pendingPetIds_.size() && (synchronousPetRows_ || (units < 32 && slice.elapsed() < 4))) {
      const qint64 id = pendingPetIds_[pendingPetRow_];
      const auto eligibility = eligibilityFor(id); currentEligibility_.insert(id,eligibility);
      fillPetRow(pendingPetRow_,cachedPetRow(id).brief,eligibility);
      visiblePetRows_.insert(id,pendingPetRow_); ++pendingPetRow_; ++units;
    }
  }
  petTable_->setUpdatesEnabled(true);
  if (pendingPetRow_ < pendingPetIds_.size() || petTable_->rowCount() > pendingPetIds_.size()) {
    petTitle_->setText(currentGood_.hasIdentity()
        ? QStringLiteral("%1 · 正在准备精灵列表 %2/%3").arg(currentGood_.description).arg(pendingPetRow_).arg(pendingPetIds_.size())
        : QStringLiteral("请选择兑换项目，正在整理列表……"));
    QTimer::singleShot(0,this,[this,generation] { continuePetRows(generation); }); return;
  }
  petRowsPreparing_ = false; pendingPetIds_.clear(); petTable_->setEnabled(true);
  if (preservePetId_ > 0 && visiblePetRows_.contains(preservePetId_)) {
    petTable_->selectRow(visiblePetRows_.value(preservePetId_)); showPetDetail(preservePetId_,false);
  }
  preservePetId_ = 0;
  if (restoreScrollAfterRows_) {
    petTable_->verticalScrollBar()->setValue(petScrollAfterRows_.first);
    petTable_->horizontalScrollBar()->setValue(petScrollAfterRows_.second);
    petDetail_->verticalScrollBar()->setValue(detailScrollAfterRows_.first);
    petDetail_->horizontalScrollBar()->setValue(detailScrollAfterRows_.second);
    restoreScrollAfterRows_ = false;
  }
  updatePetTitle(); updateMoveButton();
}

ShopPetEligibility ShopWindow::eligibilityFor(qint64 id) {
  if (!currentGood_.hasIdentity()) return {ShopPetEligibilityState::Unknown,QStringLiteral("尚未选择有效项目"),{}};
  const auto cached = currentEligibility_.constFind(id);
  if (cached != currentEligibility_.cend()) return cached.value();
  if (!currentRuleKnown_ || currentRuleExpression_ != currentGood_.enhanceType) {
    auto rule = eligibilityRules_.find(currentGood_.enhanceType);
    if (rule == eligibilityRules_.end()) {
      rule = eligibilityRules_.insert(currentGood_.enhanceType,compileShopPetRule(currentGood_.enhanceType));
      ++compiledEligibilityRules_;
    }
    currentRule_ = rule.value();
    currentRuleExpression_ = currentGood_.enhanceType; currentRuleKnown_ = true;
  }
  const auto facts = PetFactsUi::current(repository_,id);
  if (!facts) return {ShopPetEligibilityState::Unknown,QStringLiteral("正在计算培养数据，请稍候"),{}};
  if (!currentGood_.raceIds.contains(facts->facts.eligibility.raceId) &&
      !currentGood_.raceIds.contains(facts->facts.eligibility.metadataRaceId))
    return {ShopPetEligibilityState::Unknown,QStringLiteral("精灵资料正在更新，请稍候"),{}};
  const auto observation = describeShopPetRule(currentRule_,facts->facts.eligibility);
  return observation;
}

void ShopWindow::changePetSort(int logicalColumn) {
  if (logicalColumn != 1 && logicalColumn != 5 && logicalColumn != 6) return;
  if (petSortColumn_ == logicalColumn)
    petSortAscending_ = !petSortAscending_;
  else {
    petSortColumn_ = logicalColumn;
    petSortAscending_ = true;
  }
  petTable_->horizontalHeader()->setSortIndicatorShown(true);
  petTable_->horizontalHeader()->setSortIndicator(
      petSortColumn_, petSortAscending_ ? Qt::AscendingOrder : Qt::DescendingOrder);
  rebuildPetRows(currentInstanceId_);
  updatePetTitle();
}

ShopWindow::CachedPetRow& ShopWindow::cachedPetRow(qint64 id) {
  auto& row = petRowCache_[id];
  if (!row.values.isEmpty()) return row;
  if (row.brief.isEmpty() && repository_) {
    auto brief = repository_->backpackPet(id);
    if (brief.isEmpty()) brief = repository_->warehousePet(id);
    row.brief = PetFactsUi::rowSummary(brief); row.name = petDisplayName(row.brief);
  }
  const auto facts = PetFactsUi::current(repository_,id);
  const auto& power = facts ? facts->facts.battlePower : PetBattlePowerState{};
  const PetMetadataView metadata(repository_ ? repository_->metadataSnapshot() : nullptr);
  row.level = numericValue(row.brief,QStringLiteral("lv"));
  row.power = power.hasCurrent ? power.current : -1;
  row.highest = power.hasHighest ? power.highest : -1;
  row.values = {
      row.name, compactValue(row.brief, QStringLiteral("lv")),
      metadata.resolvedEra(row.brief), locationText(row.brief), QString::number(id),
      power.hasCurrent ? QString::number(power.current) : QStringLiteral("—"),
      QStringLiteral("%1 / %2").arg(power.hasExtreme ? QString::number(power.extreme) : QStringLiteral("—"),
          power.hasHighest ? QString::number(power.highest) : QStringLiteral("待补数据"))};
  return row;
}

void ShopWindow::fillPetRow(int row, const QJsonObject& pet,
                            const ShopPetEligibility& eligibility) {
  const qint64 id = petInstanceId(pet);
  const auto values = cachedPetRow(id).values;
  for (int column = 0; column < values.size(); ++column) {
    QTableWidgetItem* item = textItem(values.at(column));
    item->setData(Qt::UserRole, id);
    item->setToolTip(eligibility.reason + QStringLiteral("\n这里只判断这只精灵能否用上该商品；资源、兑换次数和解锁条件请另外确认。"));
    if (eligibility.state == ShopPetEligibilityState::Usable) {
      item->setBackground(QColor(QStringLiteral("#e9f2fc")));
      item->setForeground(QColor(QStringLiteral("#35638d")));
      QFont font = item->font();
      font.setBold(true);
      item->setFont(font);
    }
    petTable_->setItem(row, column, item);
  }
}

void ShopWindow::updatePetTitle() {
  if (!currentGood_.hasIdentity()) return;
  if (petRowsPreparing_) {
    petTitle_->setText(QStringLiteral("%1 · 正在准备精灵列表 %2/%3").arg(currentGood_.description).arg(pendingPetRow_).arg(pendingPetIds_.size())); return;
  }
  int usableCount = 0;
  for (const ShopPetEligibility& value : currentEligibility_)
    if (value.state == ShopPetEligibilityState::Usable) ++usableCount;
  petTitle_->setText(
      QStringLiteral("%1 · %2 · 所需 %3　｜　对应 %4 只，可用上 %5 只")
          .arg(currentGood_.shopName, currentGood_.description,
               costText(currentGood_))
          .arg(petTable_->rowCount()).arg(usableCount));
}

void ShopWindow::showPetDetail(qint64 instanceId, bool requestLatest) {
  if (!repository_ || instanceId <= 0) return;
  repository_->watchDetail(1, instanceId);
  const auto raw = repository_->rawRecordHandle(instanceId);
  currentInstanceId_ = instanceId;
  if (requestLatest && workbenchCompact_) {
    compactDetails_ = true;
    updateWorkbenchPanels();
  }
  const bool willRefresh = requestLatest && repository_->isAuthenticated();
  if (willRefresh)
    pendingDetailId_ = instanceId;
  else if (requestLatest)
    pendingDetailId_ = 0;
  const QJsonObject pet = repository_->detailFor(instanceId);
  if (pet.isEmpty()) {
    petDetail_->setHtml(
        willRefresh
            ? QStringLiteral("<p>该实例没有本地数据，正在按实例 ID %1 获取详情……</p>")
                  .arg(instanceId)
            : QStringLiteral("<p>实例 ID %1 没有本地详情；登录账号后点击可刷新。</p>")
                  .arg(instanceId));
    rawDetail_->clearJson();
  } else {
    const PetMetadataView catalog(repository_->metadataSnapshot());
    currentVisualKey_ = petVisualKey(pet);
    ensureImageCache();
    const QString imagePath = imageCache_ ? imageCache_->ensurePetImage(
        pet, {petDisplayName(pet), catalog.resolvedOriginalName(pet),
              catalog.petName(petRaceId(pet))}, petDetail_->devicePixelRatioF()) : QString();
    const ShopPetEligibility eligibility = eligibilityFor(instanceId);
    currentEligibility_.insert(instanceId,eligibility);
    const bool usable = eligibility.state == ShopPetEligibilityState::Usable;
    const QString state = usable ? QStringLiteral("可以用上（次数和资源请另外确认）")
                                 : eligibility.state == ShopPetEligibilityState::NotUsable
                                       ? QStringLiteral("当前已满/不需要使用")
                                       : QStringLiteral("本地数据不足，无法确认");
    const auto prepared = repository_->preparedDetail(1, instanceId);
    QString rendered = prepared ? PreparedPetDetailRenderer::render(prepared, imagePath,
        petDetail_->viewport()->width() < 440, pendingDetailId_ == instanceId)
        : PreparedPetDetailRenderer::waiting(petDisplayName(pet), repository_->detailPreparationError(1));
    const QDateTime savedAt = repository_->detailSavedAt(instanceId);
    const QString qualification = QStringLiteral(
        "<table width='100%' cellpadding='9' cellspacing='0' bgcolor='%1'><tr><td style='color:white;font-size:14px;'>"
        "<b>%2：%3</b><p style='font-size:12px;'>判断依据：%4<br>缓存：%5<br>资源、次数、解锁需分别确认，来源见工作台顶部。</p>"
        "</td></tr></table><p style='background-color:white;color:#233044;font-size:2px;'>&nbsp;</p>")
        .arg(usable ? QStringLiteral("#35638d") : QStringLiteral("#667085"),
             html(currentGood_.description), html(state), html(eligibility.reason),
             savedAt.isValid() ? savedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                               : QStringLiteral("背包当前数据/时间未知"));
    rendered.replace(QStringLiteral("<body>"), QStringLiteral("<body>%1").arg(qualification));
    imageCache_->setDocumentImage(petDetail_, rendered, imagePath);
    rawDetail_->setRecord(pet, raw);
  }
  updateMoveButton();
  if (willRefresh) {
    setStatus(QStringLiteral("已显示实例 %1 的本地数据，正在获取最新详情……")
                  .arg(instanceId));
    emit detailRequested(instanceId);
  } else if (requestLatest) {
    setStatus(QStringLiteral("账号尚未在线：已显示实例 %1 的本地缓存，本次未发送详情请求。")
                  .arg(instanceId));
  }
}

void ShopWindow::updateCurrentDetail(qint64 instanceId) {
  if (instanceId <= 0 || !repository_) return;
  QJsonObject pet = repository_->backpackPet(instanceId);
  if (pet.isEmpty()) pet = repository_->warehousePet(instanceId);
  if (pet.isEmpty()) return;
  const auto brief = PetFactsUi::rowSummary(pet);
  auto old = petRowCache_.find(instanceId);
  const bool membershipOrOrderChanged = old == petRowCache_.end() || old->name != petDisplayName(brief) ||
      petRaceId(old->brief) != petRaceId(brief) ||
      old->brief.value(QStringLiteral("_metaRaceId")) != brief.value(QStringLiteral("_metaRaceId")) ||
      old->brief.value(QStringLiteral("_location")) != brief.value(QStringLiteral("_location"));
  if (old != petRowCache_.end()) { old->brief = brief; old->name = petDisplayName(brief); old->values.clear(); }
  currentEligibility_.remove(instanceId);
  if (membershipOrOrderChanged) { petIndexDirty_ = true; scheduleRebuild(); }
  if (!currentGood_.hasIdentity()) return;
  if (petRowsPreparing_ || ((petSortColumn_ == 1 || petSortColumn_ == 5 || petSortColumn_ == 6) && visiblePetRows_.contains(instanceId))) {
    const qint64 preserve = currentInstanceId_ > 0 ? currentInstanceId_ : preservePetId_;
    rebuildPetRows(preserve);
  } else if (visiblePetRows_.contains(instanceId)) {
    const ShopPetEligibility eligibility = eligibilityFor(instanceId);
    currentEligibility_.insert(instanceId, eligibility);
    fillPetRow(visiblePetRows_.value(instanceId), pet, eligibility);
  }
  if (pendingDetailId_ == instanceId) pendingDetailId_ = 0;
  updatePetTitle();
  if (currentInstanceId_ == instanceId) {
    showPetDetail(instanceId, false);
    setStatus(QStringLiteral("实例 %1 的缓存视图已更新。")
                  .arg(instanceId));
  }
}

void ShopWindow::updateCurrentImage(const QString& visualKey,
                                    const QString& url) {
  if (visualKey != currentVisualKey_ || currentInstanceId_ <= 0) return;
  imageCache_->updateDocumentImage(petDetail_, url);
}

void ShopWindow::finishDetailRefresh(qint64 instanceId, bool succeeded,
                                     const QString& reason) {
  if (pendingDetailId_ != instanceId) return;
  pendingDetailId_ = 0;
  if (currentInstanceId_ == instanceId) showPetDetail(instanceId, false);
  if (!succeeded)
    setStatus(QStringLiteral("实例 %1 详情刷新失败：%2；已保留旧缓存。")
                  .arg(instanceId).arg(reason));
}

void ShopWindow::setMoveRunning(bool running) {
  moveRunning_ = running;
  updateMoveButton();
}

void ShopWindow::updateMoveButton() {
  if (!moveToBackpack_) return;
  const QJsonObject pet = repository_ && currentInstanceId_ > 0
                              ? repository_->detailFor(currentInstanceId_)
                              : QJsonObject{};
  const bool warehouse = pet.value(QStringLiteral("_location")).toString() ==
                         QStringLiteral("warehouse");
  moveToBackpack_->setEnabled(!moveRunning_ && warehouse);
  moveToBackpack_->setText(moveRunning_ ? QStringLiteral("移动处理中……")
                                       : QStringLiteral("放入背包"));
}

void ShopWindow::moveCurrentToBackpack() {
  if (!repository_ || currentInstanceId_ <= 0 || moveRunning_) return;
  const QJsonObject pet = repository_->detailFor(currentInstanceId_);
  if (pet.value(QStringLiteral("_location")).toString() != QStringLiteral("warehouse"))
    return;
  emit moveToBackpackRequested(currentInstanceId_);
}
