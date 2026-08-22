#include "shop_window.h"

#include "build_info.h"

#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_image_cache.h"
#include "pet_move_policy.h"
#include "pet_repository.h"
#include "pet_window.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QVariant>

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

QString displayName(const QJsonObject& pet) {
  QString name = pet.value(QStringLiteral("customName")).toString().trimmed();
  if (name.isEmpty()) name = pet.value(QStringLiteral("n")).toString().trimmed();
  if (name.isEmpty()) name = PetDetailCatalog::instance().resolvedOriginalName(pet);
  return name.isEmpty() ? QStringLiteral("未知精灵") : name;
}

QString locationText(const QJsonObject& pet) {
  if (pet.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack")) {
    if (!pet.contains(QStringLiteral("_position"))) return QStringLiteral("背包");
    const int position = qMax(0, pet.value(QStringLiteral("_position")).toInt());
    return QStringLiteral("背包 第%1页第%2排")
        .arg(position / 12 + 1).arg(position % 12 / 6 + 1);
  }
  return pet.value(QStringLiteral("_warehouseGroup")).toString() == QStringLiteral("elite")
             ? QStringLiteral("精英仓库")
             : QStringLiteral("普通仓库");
}

QString html(const QString& text) { return text.toHtmlEscaped(); }

QString factRow(const QString& label, const QString& value) {
  return QStringLiteral("<tr><td class='label'>%1</td><td>%2</td></tr>")
      .arg(html(label), value.isEmpty() ? QStringLiteral("—") : html(value));
}

bool isFullDetail(const PetRepository* repository, const QJsonObject& pet) {
  if (!repository) return false;
  if (pet.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack"))
    return pet.contains(QStringLiteral("czdlv")) && pet.contains(QStringLiteral("mzdlv"));
  return repository->hasCachedDetail(petInstanceId(pet));
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
  if (value.isDouble()) return static_cast<qint64>(value.toDouble());
  if (value.isString()) return value.toString().toLongLong();
  return 0;
}

}  // namespace

ShopWindow::ShopWindow(PetRepository* repository, QWidget* parent)
    : QDialog(parent), repository_(repository) {
  setObjectName(QStringLiteral("KQPetShopWindow"));
  setWindowTitle(QStringLiteral("原版氪奇 · 培养兑换商店 · %1")
                     .arg(BuildInfo::displayVersion()));
  resize(1420, 860);
  setMinimumSize(1100, 680);
  setAttribute(Qt::WA_DeleteOnClose, false);
  imageCache_ = new PetImageCache(repository_ ? repository_->dataRoot() : QString(), this);

  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  refresh_ = new QPushButton(QStringLiteral("刷新兑换次数"), this);
  refreshCatalog_ = new QPushButton(QStringLiteral("更新兑换项目/适用精灵"), this);
  status_ = new QLabel(QStringLiteral("商店只在手动点击按钮时刷新"), this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  toolbar->addWidget(refresh_);
  toolbar->addWidget(refreshCatalog_);
  toolbar->addWidget(status_, 1);
  root->addLayout(toolbar);

  auto* horizontal = new QSplitter(Qt::Horizontal, this);
  auto* left = new QSplitter(Qt::Vertical, horizontal);
  shopTabs_ = new QTabWidget(left);
  shopTabs_->setUsesScrollButtons(true);

  auto* petPane = new QWidget(left);
  auto* petLayout = new QVBoxLayout(petPane);
  petLayout->setContentsMargins(0, 0, 0, 0);
  petTitle_ = new QLabel(QStringLiteral("点击左侧兑换项目查看对应精灵"), petPane);
  petTitle_->setWordWrap(true);
  petTitle_->setStyleSheet(QStringLiteral("font-weight:600;"));
  petTable_ = makeTable(petPane,
                        {QStringLiteral("精灵"), QStringLiteral("等级"),
                         QStringLiteral("时代"), QStringLiteral("位置"),
                         QStringLiteral("实例 ID"), QStringLiteral("战斗力"),
                         QStringLiteral("极限战斗力")});
  petTable_->setObjectName(QStringLiteral("KQShopPetTable"));
  petTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  petTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  petTable_->horizontalHeader()->setSectionsClickable(true);
  petTable_->horizontalHeader()->setSortIndicatorShown(false);
  petLayout->addWidget(petTitle_);
  petLayout->addWidget(petTable_, 1);

  auto* detailTabs = new QTabWidget(horizontal);
  petDetail_ = new QTextBrowser(detailTabs);
  rawDetail_ = new QPlainTextEdit(detailTabs);
  rawDetail_->setReadOnly(true);
  rawDetail_->setLineWrapMode(QPlainTextEdit::NoWrap);
  detailTabs->addTab(petDetail_, QStringLiteral("本地详情与资格分析"));
  detailTabs->addTab(rawDetail_, QStringLiteral("原始本地缓存"));
  petDetail_->setHtml(QStringLiteral(
      "<p style='color:#718096'>点击上方精灵会立即显示本地缓存；账号在线时会按实例 ID 在后台刷新一次详情。</p>"));

  left->addWidget(shopTabs_);
  left->addWidget(petPane);
  left->setChildrenCollapsible(false);
  left->setStretchFactor(0, 3);
  left->setStretchFactor(1, 2);
  left->setSizes({500, 310});
  horizontal->addWidget(left);
  horizontal->addWidget(detailTabs);
  horizontal->setChildrenCollapsible(false);
  horizontal->setStretchFactor(0, 6);
  horizontal->setStretchFactor(1, 5);
  horizontal->setSizes({780, 620});
  root->addWidget(horizontal, 1);

  auto* moveBar = new QHBoxLayout();
  moveBar->addStretch(1);
  moveToBackpack_ = new QPushButton(QStringLiteral("进入背包"), this);
  moveToBackpack_->setToolTip(
      QStringLiteral("仅仓库精灵可用；移动前后会强制刷新背包/仓库，背包已满时需手动选择交换精灵"));
  moveBar->addWidget(moveToBackpack_);
  root->addLayout(moveBar);

  connect(refresh_, &QPushButton::clicked, this, &ShopWindow::refreshRequested);
  connect(refreshCatalog_, &QPushButton::clicked, this,
          &ShopWindow::catalogRefreshRequested);
  connect(petTable_, &QTableWidget::cellClicked, this, [this](int row, int) {
    QTableWidgetItem* item = petTable_->item(row, 0);
    if (item) showPetDetail(item->data(Qt::UserRole).toLongLong(), true);
  });
  connect(petTable_->horizontalHeader(), &QHeaderView::sectionClicked, this,
          &ShopWindow::changePetSort);
  connect(moveToBackpack_, &QPushButton::clicked, this,
          &ShopWindow::moveCurrentToBackpack);
  connect(shopTabs_, &QTabWidget::currentChanged, this, [this]() {
    petTable_->setRowCount(0);
    currentEligibility_.clear();
    currentGood_ = {};
    currentInstanceId_ = 0;
    pendingDetailId_ = 0;
    currentVisualKey_.clear();
    petTitle_->setText(QStringLiteral("点击左侧兑换项目查看对应精灵"));
    petDetail_->setHtml(QStringLiteral("<p style='color:#718096'>请选择兑换项目和精灵。</p>"));
    rawDetail_->clear();
    updateMoveButton();
  });
  if (repository_) {
    connect(repository_, &PetRepository::dataChanged, this, &ShopWindow::rebuild);
    connect(repository_, &PetRepository::detailChanged, this,
            &ShopWindow::updateCurrentDetail);
  }
  connect(imageCache_, &PetImageCache::petImageReady, this,
          &ShopWindow::updateCurrentImage);
  updateMoveButton();
  rebuild();
}

void ShopWindow::setPacket(const QJsonObject& packet, bool hasPacket) {
  packet_ = packet;
  hasPacket_ = hasPacket;
  rebuild();
}

void ShopWindow::setMaterialCounts(const QHash<QString, qint64>& counts, bool valid) {
  materialCounts_ = counts;
  hasMaterialCounts_ = valid;
  rebuild();
}

void ShopWindow::setStatus(const QString& status) { status_->setText(status); }

void ShopWindow::setRefreshRunning(bool running) {
  refresh_->setEnabled(!running);
  refresh_->setText(running ? QStringLiteral("查询中…") : QStringLiteral("刷新兑换次数"));
}

QString ShopWindow::costText(const ShopExchangeGood& good) const {
  QStringList costs;
  QString normalized = good.cost;
  normalized.replace(QLatin1Char('|'), QLatin1Char('#'));
  for (const QString& part : normalized.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
    const QStringList fields = part.split(QLatin1Char(':'));
    if (fields.size() < 3) continue;
    costs.append(PetDetailCatalog::instance().materialCostText(
        fields.at(0).toInt(), fields.at(1).toInt(), fields.at(2).toInt()));
  }
  return costs.isEmpty() ? good.cost : costs.join(QStringLiteral(" ＋ "));
}

QString ShopWindow::shopCurrencyText(const ShopExchangeShop& shop) const {
  QSet<QString> seen;
  QStringList currencies;
  for (const ShopExchangeGood& good : shop.goods) {
    QString normalized = good.cost;
    normalized.replace(QLatin1Char('|'), QLatin1Char('#'));
    for (const QString& part : normalized.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
      const QStringList fields = part.split(QLatin1Char(':'));
      if (fields.size() < 2) continue;
      const QString key = fields.at(0) + QLatin1Char(':') + fields.at(1);
      if (seen.contains(key)) continue;
      seen.insert(key);
      const QString name = PetDetailCatalog::instance().materialName(
          fields.at(0).toInt(), fields.at(1).toInt());
      currencies.append(QStringLiteral("%1 %2")
                            .arg(name, hasMaterialCounts_
                                           ? QString::number(materialCounts_.value(key, 0))
                                           : QStringLiteral("未查询")));
    }
  }
  return currencies.join(QStringLiteral(" / "));
}

void ShopWindow::rebuild() {
  const int current = shopTabs_->currentIndex();
  const QString selectedGoodKey = currentGood_.itemServerId > 0
                                      ? currentGood_.stableKey() : QString();
  const qint64 selectedInstanceId = currentInstanceId_;
  const bool detailWasPending = pendingDetailId_ == selectedInstanceId;
  const QSignalBlocker tabSignals(shopTabs_);
  while (shopTabs_->count() > 0) {
    QWidget* page = shopTabs_->widget(0);
    shopTabs_->removeTab(0);
    delete page;
  }
  shopRows_.clear();
  const auto shops = ShopExchangeCatalog::instance().shops();
  shopRows_.reserve(shops.size());
  for (const ShopExchangeShop& shop : shops) {
    auto* table = makeTable(shopTabs_,
                            {QStringLiteral("兑换项目"), QStringLiteral("所需资源"),
                             QStringLiteral("限次"), QStringLiteral("剩余"),
                             QStringLiteral("对应个体")});
    table->setObjectName(QStringLiteral("KQShopGoodsTable-%1").arg(shop.shopId));
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    QList<GoodRow> rows;
    table->setRowCount(shop.goods.size());
    int row = 0;
    for (const ShopExchangeGood& good : shop.goods) {
      const int remaining = hasPacket_ ? ShopExchangeCatalog::remainingCount(packet_, good) : -1;
      table->setItem(row, 0, textItem(good.description));
      table->setItem(row, 1, textItem(costText(good)));
      table->setItem(row, 2, textItem(QStringLiteral("%1限 %2 次")
                                          .arg(good.limitLabel).arg(good.limitCount)));
      table->setItem(row, 3, remaining < 0
                                 ? textItem(QStringLiteral("未查询"))
                                 : textItem(QStringLiteral("%1 / %2").arg(remaining).arg(good.limitCount),
                                            remaining > 0));
      table->setItem(row, 4, textItem(QString::number(eligiblePets(good).size())));
      rows.append({good, row});
      ++row;
    }
    table->setProperty("shopIndex", shopRows_.size());
    connect(table, &QTableWidget::cellClicked, this, [this, table](int row, int) {
      const int shopIndex = table->property("shopIndex").toInt();
      if (shopIndex < 0 || shopIndex >= shopRows_.size() || row < 0 ||
          row >= shopRows_.at(shopIndex).size()) return;
      showGoodPets(shopRows_.at(shopIndex).at(row).good);
    });
    const QString currency = shopCurrencyText(shop);
    const int tabIndex = shopTabs_->addTab(
        table, currency.isEmpty() ? shop.name
                                  : QStringLiteral("%1（%2）").arg(shop.name, currency));
    shopTabs_->setTabToolTip(tabIndex,
                             currency.isEmpty() ? shop.name
                                                : QStringLiteral("%1｜账号拥有：%2")
                                                      .arg(shop.name, currency));
    shopRows_.append(rows);
  }
  if (current >= 0 && current < shopTabs_->count()) shopTabs_->setCurrentIndex(current);

  bool restoredGood = false;
  if (!selectedGoodKey.isEmpty()) {
    for (const QList<GoodRow>& rows : shopRows_) {
      for (const GoodRow& row : rows) {
        if (row.good.stableKey() != selectedGoodKey) continue;
        showGoodPets(row.good);
        restoredGood = true;
        break;
      }
      if (restoredGood) break;
    }
  }
  if (restoredGood && selectedInstanceId > 0) {
    for (int row = 0; row < petTable_->rowCount(); ++row) {
      QTableWidgetItem* item = petTable_->item(row, 0);
      if (!item || item->data(Qt::UserRole).toLongLong() != selectedInstanceId)
        continue;
      petTable_->selectRow(row);
      pendingDetailId_ = detailWasPending ? selectedInstanceId : 0;
      showPetDetail(selectedInstanceId, false);
      break;
    }
  }
  updateMoveButton();
}

QList<QJsonObject> ShopWindow::eligiblePets(const ShopExchangeGood& good) const {
  QList<QJsonObject> result;
  if (!repository_) return result;
  QSet<int> allowed;
  for (int raceId : good.raceIds) allowed.insert(raceId);
  const auto append = [&](const QList<QJsonObject>& pets) {
    for (const QJsonObject& brief : pets) {
      const int currentRace = petRaceId(brief);
      const int metadataRace = brief.value(QStringLiteral("_metaRaceId")).toInt();
      if (!allowed.contains(currentRace) &&
          !(metadataRace > 0 && allowed.contains(metadataRace))) continue;
      QJsonObject detail = repository_->detailFor(petInstanceId(brief));
      for (auto iterator = brief.begin(); iterator != brief.end(); ++iterator)
        detail.insert(iterator.key(), iterator.value());
      result.append(detail);
    }
  };
  append(repository_->backpackPets());
  append(repository_->warehousePets());
  std::sort(result.begin(), result.end(), [](const QJsonObject& left, const QJsonObject& right) {
    const bool leftPack = left.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack");
    const bool rightPack = right.value(QStringLiteral("_location")).toString() == QStringLiteral("backpack");
    if (leftPack != rightPack) return leftPack;
    const int nameOrder = QString::localeAwareCompare(displayName(left), displayName(right));
    return nameOrder == 0 ? petInstanceId(left) < petInstanceId(right) : nameOrder < 0;
  });
  return result;
}

void ShopWindow::showGoodPets(const ShopExchangeGood& good) {
  currentGood_ = good;
  updateSelectedGoodEmphasis();
  currentEligibility_.clear();
  currentInstanceId_ = 0;
  pendingDetailId_ = 0;
  currentVisualKey_.clear();
  rebuildPetRows();
  updatePetTitle();
  petDetail_->setHtml(QStringLiteral("<p style='color:#718096'>点击左下方精灵会立即显示本地缓存，并按实例 ID 在后台刷新一次详情。</p>"));
  rawDetail_->clear();
  updateMoveButton();
}

void ShopWindow::updateSelectedGoodEmphasis() {
  const QString selectedKey = currentGood_.itemServerId > 0
                                  ? currentGood_.stableKey() : QString();
  for (int shopIndex = 0; shopIndex < shopRows_.size(); ++shopIndex) {
    auto* table = qobject_cast<QTableWidget*>(shopTabs_->widget(shopIndex));
    if (!table) continue;
    for (const GoodRow& row : shopRows_.at(shopIndex)) {
      QTableWidgetItem* name = table->item(row.tableRow, 0);
      if (!name) continue;
      const bool selected = !selectedKey.isEmpty() && row.good.stableKey() == selectedKey;
      QFont font = name->font();
      font.setBold(selected);
      name->setFont(font);
      if (selected) table->selectRow(row.tableRow);
    }
  }
}

void ShopWindow::rebuildPetRows(qint64 preserveInstanceId) {
  if (currentGood_.itemServerId <= 0) {
    petTable_->setRowCount(0);
    return;
  }
  QList<QJsonObject> pets = eligiblePets(currentGood_);
  if (petSortColumn_ == 1 || petSortColumn_ == 5 || petSortColumn_ == 6) {
    const QString key = petSortColumn_ == 1   ? QStringLiteral("lv")
                        : petSortColumn_ == 5 ? QStringLiteral("zdl")
                                              : QStringLiteral("xzdl");
    const bool ascending = petSortAscending_;
    std::stable_sort(pets.begin(), pets.end(), [key, ascending](const QJsonObject& left,
                                                                const QJsonObject& right) {
      const qint64 leftValue = numericValue(left, key);
      const qint64 rightValue = numericValue(right, key);
      if (leftValue != rightValue)
        return ascending ? leftValue < rightValue : leftValue > rightValue;
      return petInstanceId(left) < petInstanceId(right);
    });
  }
  currentEligibility_.clear();
  petTable_->setRowCount(pets.size());
  int row = 0;
  for (const QJsonObject& pet : pets) {
    const qint64 id = petInstanceId(pet);
    const ShopPetEligibility eligibility =
        analyzeShopPetEligibility(currentGood_, pet, isFullDetail(repository_, pet));
    currentEligibility_.insert(id, eligibility);
    fillPetRow(row, pet, eligibility);
    ++row;
  }
  if (preserveInstanceId > 0) {
    for (int currentRow = 0; currentRow < petTable_->rowCount(); ++currentRow) {
      QTableWidgetItem* item = petTable_->item(currentRow, 0);
      if (item && item->data(Qt::UserRole).toLongLong() == preserveInstanceId) {
        petTable_->selectRow(currentRow);
        break;
      }
    }
  }
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

void ShopWindow::fillPetRow(int row, const QJsonObject& pet,
                            const ShopPetEligibility& eligibility) {
  const qint64 id = petInstanceId(pet);
  const QStringList values = {
      displayName(pet), compactValue(pet, QStringLiteral("lv")),
      PetDetailCatalog::instance().resolvedEra(pet), locationText(pet),
      QString::number(id), compactValue(pet, QStringLiteral("zdl")),
      compactValue(pet, QStringLiteral("xzdl"))};
  for (int column = 0; column < values.size(); ++column) {
    QTableWidgetItem* item = textItem(values.at(column));
    item->setData(Qt::UserRole, id);
    item->setToolTip(eligibility.reason);
    if (eligibility.state == ShopPetEligibilityState::Usable) {
      item->setBackground(QColor(QStringLiteral("#dff5e8")));
      item->setForeground(QColor(QStringLiteral("#087a43")));
      QFont font = item->font();
      font.setBold(true);
      item->setFont(font);
    }
    petTable_->setItem(row, column, item);
  }
}

void ShopWindow::updatePetTitle() {
  if (currentGood_.itemServerId <= 0) return;
  int usableCount = 0;
  for (const ShopPetEligibility& value : currentEligibility_)
    if (value.state == ShopPetEligibilityState::Usable) ++usableCount;
  petTitle_->setText(
      QStringLiteral("%1 · %2 · 所需 %3（对应 %4 只，明确可提升 %5 只；绿色表示可用）")
          .arg(currentGood_.shopName, currentGood_.description,
               costText(currentGood_))
          .arg(petTable_->rowCount()).arg(usableCount));
}

void ShopWindow::showPetDetail(qint64 instanceId, bool requestLatest) {
  if (!repository_ || instanceId <= 0) return;
  currentInstanceId_ = instanceId;
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
    rawDetail_->clear();
  } else {
    const PetDetailCatalog& catalog = PetDetailCatalog::instance();
    currentVisualKey_ = petVisualKey(pet);
    const QString imagePath = imageCache_->ensurePetImage(
        pet, {displayName(pet), catalog.resolvedOriginalName(pet),
              catalog.petName(petRaceId(pet))});
    const ShopPetEligibility eligibility = currentEligibility_.value(instanceId);
    const bool usable = eligibility.state == ShopPetEligibilityState::Usable;
    const QString state = usable ? QStringLiteral("可以使用")
                                 : eligibility.state == ShopPetEligibilityState::NotUsable
                                       ? QStringLiteral("当前已满/不需要使用")
                                       : QStringLiteral("本地数据不足，无法确认");
    QString rendered = PetWindow::renderCachedDetailHtml(
        pet, repository_, imagePath, pendingDetailId_ == instanceId);
    const QDateTime savedAt = repository_->detailSavedAt(instanceId);
    const QString qualification = QStringLiteral(
        "<div style='padding:10px 14px;color:white;background:%1;font-size:15px;font-weight:700;'>"
        "%2：%3<br><span style='font-size:12px;font-weight:400'>判断依据：%4　缓存：%5</span></div>")
        .arg(usable ? QStringLiteral("#16824b") : QStringLiteral("#667085"),
             html(currentGood_.description), html(state), html(eligibility.reason),
             savedAt.isValid() ? savedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                               : QStringLiteral("背包当前数据/时间未知"));
    rendered.replace(QStringLiteral("<body>"), QStringLiteral("<body>%1").arg(qualification));
    petDetail_->setHtml(rendered);
    rawDetail_->setPlainText(
        QString::fromUtf8(QJsonDocument(pet).toJson(QJsonDocument::Indented)));
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
  if (instanceId <= 0 || currentGood_.itemServerId <= 0) return;
  const QJsonObject pet = repository_->detailFor(instanceId);
  if (pet.isEmpty()) return;
  if ((petSortColumn_ == 5 || petSortColumn_ == 6) &&
      currentEligibility_.contains(instanceId)) {
    rebuildPetRows(currentInstanceId_);
  } else for (int row = 0; row < petTable_->rowCount(); ++row) {
    QTableWidgetItem* item = petTable_->item(row, 0);
    if (!item || item->data(Qt::UserRole).toLongLong() != instanceId) continue;
    const ShopPetEligibility eligibility = analyzeShopPetEligibility(
        currentGood_, pet, isFullDetail(repository_, pet));
    currentEligibility_.insert(instanceId, eligibility);
    fillPetRow(row, pet, eligibility);
    break;
  }
  if (pendingDetailId_ == instanceId) pendingDetailId_ = 0;
  updatePetTitle();
  if (currentInstanceId_ == instanceId) {
    showPetDetail(instanceId, false);
    setStatus(QStringLiteral("实例 %1 的最新详情已覆盖当前账号本地缓存。")
                  .arg(instanceId));
  }
}

void ShopWindow::updateCurrentImage(const QString& visualKey,
                                    const QString&) {
  if (visualKey != currentVisualKey_ || currentInstanceId_ <= 0) return;
  showPetDetail(currentInstanceId_, false);
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
                                       : QStringLiteral("进入背包"));
}

void ShopWindow::moveCurrentToBackpack() {
  if (!repository_ || currentInstanceId_ <= 0 || moveRunning_) return;
  const QJsonObject pet = repository_->detailFor(currentInstanceId_);
  if (pet.value(QStringLiteral("_location")).toString() != QStringLiteral("warehouse"))
    return;
  emit moveToBackpackRequested(currentInstanceId_);
}

void ShopWindow::requestReplacement(
    qint64 incomingInstanceId, const QList<qint64>& eligibleBackpackIds) {
  if (incomingInstanceId != currentInstanceId_ || eligibleBackpackIds.isEmpty()) {
    emit moveCancelRequested();
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("背包已满 · 选择交换精灵"));
  dialog.resize(820, 480);
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(
      QStringLiteral("选择一只背包精灵放入仓库，再将实例 %1 放入原位置。")
          .arg(incomingInstanceId), &dialog));
  QTableWidget* table = makeTable(
      &dialog, {QStringLiteral("精灵"), QStringLiteral("等级"),
                QStringLiteral("时代"), QStringLiteral("位置"),
                QStringLiteral("实例 ID"), QStringLiteral("战斗力"),
                QStringLiteral("极限战斗力")});
  table->setRowCount(eligibleBackpackIds.size());
  int row = 0;
  for (qint64 id : eligibleBackpackIds) {
    const QJsonObject pet = repository_->backpackPet(id);
    const QStringList values = {displayName(pet), compactValue(pet, QStringLiteral("lv")),
                                PetDetailCatalog::instance().resolvedEra(pet),
                                locationText(pet), QString::number(id),
                                compactValue(pet, QStringLiteral("zdl")),
                                compactValue(pet, QStringLiteral("xzdl"))};
    for (int column = 0; column < values.size(); ++column) {
      QTableWidgetItem* item = textItem(values.at(column));
      item->setData(Qt::UserRole, id);
      table->setItem(row, column, item);
    }
    ++row;
  }
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  if (table->rowCount() > 0) table->selectRow(0);
  layout->addWidget(table, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                           QDialogButtonBox::Cancel,
                                       &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) {
    emit moveCancelRequested();
    return;
  }
  const QModelIndexList selected = table->selectionModel()->selectedRows();
  if (selected.isEmpty()) {
    emit moveCancelRequested();
    return;
  }
  const qint64 outgoingId =
      table->item(selected.constFirst().row(), 0)->data(Qt::UserRole).toLongLong();
  if (outgoingId > 0) emit moveReplacementChosen(outgoingId);
}
