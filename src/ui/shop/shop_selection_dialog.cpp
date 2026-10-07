#include "shop_selection_dialog.h"
#include "domain/compiled_shop_catalog.h"
#include "domain/pet_metadata_view.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int KeyRole = Qt::UserRole;
constexpr int ManualRole = Qt::UserRole + 1;
QString plain(QString value) { return value.replace(QChar('\n'), QChar(' ')); }
QString priceText(const ShopExchangeGood& good, const PetMetadataView& metadata) {
  if (good.cost.isEmpty()) return good.costDescription.isEmpty() ? QStringLiteral("需在游戏中查看") : good.costDescription;
  const auto compiled = CompiledShopCatalog::compile({good});
  const auto& value = compiled.goods().first();
  if (value.costCondition.effectiveState() != ShopConditionState::Satisfied) return QStringLiteral("价格条件待确认");
  QStringList prices;
  for (const auto& requirement : value.requirements) {
    const auto fields = requirement.resourceKey.split(QLatin1Char(':'));
    prices.append(QStringLiteral("%1 ×%2").arg(metadata.materialName(fields.at(0).toInt(), fields.at(1).toInt()))
        .arg(requirement.required));
  }
  return prices.join(QStringLiteral(" ＋ "));
}
}

ShopSelectionDialog::ShopSelectionDialog(QWidget* parent, std::shared_ptr<const PetDetailCatalogSnapshot> metadata)
    : QDialog(parent), metadata_(std::move(metadata)) {
  setWindowTitle(QStringLiteral("兑换商店 · 自动添加 + 手动补充"));
  resize(1180, 660);
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(QStringLiteral(
      "自动识别的精灵培养兑换默认勾选，可以取消；保存后会记住选择。手动补充列出指定精灵提升，包含签到、进度领取，以及指定养成与材料二选一；精灵自选、材料任选和自选礼包不显示。\n"
      "整店勾选只作用于本次已扫描的项目；以后新增的手动项目仍需选择。未到开放时间或已下架的项目暂不显示。"), this);
  explanation->setWordWrap(true); layout->addWidget(explanation);
  auto* actions = new QHBoxLayout;
  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("KQShopSelectionSearch"));
  search_->setPlaceholderText(QStringLiteral("搜索商店、兑换项目或来源"));
  search_->setClearButtonEnabled(true); actions->addWidget(search_, 1);
  selectAll_ = new QPushButton(QStringLiteral("勾选搜索结果"), this);
  clear_ = new QPushButton(QStringLiteral("取消勾选搜索结果"), this);
  actions->addWidget(selectAll_); actions->addWidget(clear_); layout->addLayout(actions);
  tree_ = new QTreeWidget(this);
  tree_->setObjectName(QStringLiteral("KQShopSelectionTree"));
  tree_->setHeaderLabels({QStringLiteral("商店 / 活动项目"), QStringLiteral("添加方式"), QStringLiteral("费用 / 领取条件"), QStringLiteral("有效期（北京时间）"), QStringLiteral("次数 / 限制"), QStringLiteral("获取方式")});
  tree_->setColumnWidth(0, 340); tree_->setColumnWidth(1, 140); tree_->setColumnWidth(2, 220);
  tree_->setColumnWidth(3, 230);
  tree_->setColumnWidth(4, 110);
  tree_->header()->setStretchLastSection(true);
  tree_->setAlternatingRowColors(true); tree_->setUniformRowHeights(true);
  layout->addWidget(tree_, 1);
  summary_ = new QLabel(this); layout->addWidget(summary_);
  diagnostics_ = new QPushButton(this);
  diagnostics_->setObjectName(QStringLiteral("KQShopParseDiagnostics"));
  diagnostics_->hide(); layout->addWidget(diagnostics_);
  connect(diagnostics_, &QPushButton::clicked, this, [this] {
    if (!catalog_) return;
    QStringList details;
    for (const auto& value : catalog_->root.value(QStringLiteral("activityPending")).toArray()) {
      const auto issue = value.toObject();
      const auto line = issue.value(QStringLiteral("activityName")).toString() + QStringLiteral("：") +
          issue.value(QStringLiteral("reason")).toString();
      if (!details.contains(line)) details.append(line);
    }
    QMessageBox box(QMessageBox::Information, QStringLiteral("未完整解析的活动"),
        QStringLiteral("已识别的项目可以正常添加。以下活动仍有部分奖励或适用条件尚未识别，未生成可勾选项目："), QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.setDetailedText(details.join(QLatin1Char('\n')));
    box.exec();
  });
  status_ = new QLabel(this); status_->setWordWrap(true); status_->setTextFormat(Qt::PlainText);
  layout->addWidget(status_);
  auto* buttons = new QDialogButtonBox(this);
  scan_ = buttons->addButton(QStringLiteral("更新并扫描商店"), QDialogButtonBox::ActionRole);
  auto* reload = buttons->addButton(QStringLiteral("读取已扫描商店"), QDialogButtonBox::ActionRole);
  save_ = buttons->addButton(QStringLiteral("保存选择"), QDialogButtonBox::ApplyRole);
  save_->setObjectName(QStringLiteral("KQSaveShopSelection"));
  auto* close = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
  layout->addWidget(buttons);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(reload, &QPushButton::clicked, this, &ShopSelectionDialog::selectionRequested);
  connect(scan_, &QPushButton::clicked, this, &ShopSelectionDialog::scanRequested);
  connect(search_, &QLineEdit::textChanged, this, [this] { filter(); });
  connect(selectAll_, &QPushButton::clicked, this, [this] { setChecks(Qt::Checked); });
  connect(clear_, &QPushButton::clicked, this, [this] { setChecks(Qt::Unchecked); });
  connect(save_, &QPushButton::clicked, this, [this] {
    saving_ = true; submitted_ = draft_; excludedSubmitted_ = excludedDraft_; updateSummary();
    emit selectionSaveRequested(draft_.values(), excludedDraft_.values());
  });
  connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
    if (column != 0 || !editable_ || saving_) return;
    const QSignalBlocker blocker(tree_);
    if (item->parent()) {
      changeCheck(item, item->checkState(0)); updateParent(item->parent());
    } else {
      const auto checked = item->checkState(0) == Qt::Unchecked ? Qt::Unchecked : Qt::Checked;
      for (int i = 0; i < item->childCount(); ++i)
        if (!item->child(i)->isHidden()) changeCheck(item->child(i), checked);
      updateParent(item);
    }
    updateDirty();
    updateSummary();
  });
  updateSummary();
  auto* availabilityTimer = new QTimer(this);
  availabilityTimer->setInterval(1000);
  connect(availabilityTimer, &QTimer::timeout, this, [this] {
    if (isVisible() && catalog_ && QDateTime::currentDateTimeUtc() >= nextAvailabilityChange_) {
      rebuild(); updateSummary();
    }
  });
  availabilityTimer->start();
}

void ShopSelectionDialog::setCatalog(std::shared_ptr<const ShopCatalogSnapshot> catalog,
                                    bool editable, const QString& message) {
  editable_ = editable;
  status_->setText(message);
  if (saving_ && editable) {
    saving_ = false;
    if (catalog && catalog->manualSelection == submitted_ && catalog->excludedAutomatic == excludedSubmitted_) dirty_ = false;
  }
  if (catalog) {
    const bool changed = catalog_ != catalog;
    catalog_ = std::move(catalog);
    if (!dirty_) { draft_ = catalog_->manualSelection; excludedDraft_ = catalog_->excludedAutomatic; }
    if (changed) rebuild();
  }
  updateSummary();
}
void ShopSelectionDialog::setUpdateBusy(bool busy) { updating_ = busy; updateSummary(); }

void ShopSelectionDialog::rebuild() {
  const QSignalBlocker blocker(tree_);
  tree_->clear();
  if (!catalog_) return;
  QSet<QString> incomplete;
  for (const auto& value : catalog_->root.value(QStringLiteral("activityPending")).toArray())
    incomplete.insert(value.toObject().value(QStringLiteral("module")).toString());
  diagnostics_->setText(QStringLiteral("%1 个活动尚未完整解析 · 查看原因").arg(incomplete.size()));
  diagnostics_->setVisible(!incomplete.isEmpty());
  const auto now = QDateTime::currentDateTimeUtc();
  const QDate today = now.toOffsetFromUtc(8 * 3600).date();
  nextAvailabilityChange_ = QDateTime(today.addDays(1), QTime(0, 0), Qt::OffsetFromUTC, 8 * 3600);
  const PetMetadataView metadata(metadata_);
  for (const auto& shop : catalog_->discoveredShops) {
    auto* parent = new QTreeWidgetItem(tree_, {shop.name});
    parent->setToolTip(0, shop.sourceKey.isEmpty() ? QStringLiteral("常驻商店 %1").arg(shop.shopId) : shop.sourceKey);
    for (const auto& good : shop.goods) {
      const auto change = good.nextAvailabilityChange(now);
      if (change.isValid() && change < nextAvailabilityChange_) nextAvailabilityChange_ = change;
      if (!good.isOnlineAt(now)) continue;
      const QString mode = good.manualSelectionRequired ? QStringLiteral("手动补充") : QStringLiteral("默认勾选（可取消）");
      const auto endAt = good.endsAt.toOffsetFromUtc(8 * 3600);
      const bool wholeLastDay = endAt.isValid() && endAt.time() == QTime(0, 0) &&
          good.hasRemovalDate && good.removalDate == endAt.date().addDays(-1);
      const QString period = QStringLiteral("%1 — %2").arg(
          good.startsAt.isValid() ? good.startsAt.toOffsetFromUtc(8 * 3600).toString(QStringLiteral("yyyy-MM-dd HH:mm")) :
              good.shelfDate.isValid() ? good.shelfDate.toString(Qt::ISODate) : QStringLiteral("开始时间未注明"),
          wholeLastDay ? good.removalDate.toString(Qt::ISODate) + QStringLiteral("（含当天）") :
          endAt.isValid() ? endAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) :
              good.hasRemovalDate ? good.removalDate.toString(Qt::ISODate) : QStringLiteral("结束时间未注明"));
      const QString price = priceText(good, metadata);
      const QString quota = !good.quotaDescription.isEmpty() ? good.quotaDescription :
          good.limitCount > 0 ? QStringLiteral("%1限 %2 次").arg(good.limitLabel).arg(good.limitCount) : QStringLiteral("次数未注明");
      auto* child = new QTreeWidgetItem(parent, {plain(good.displayName()), mode, price, period, quota, good.acquisitionLabel()});
      child->setToolTip(4, quota + QStringLiteral("\n此处为官方静态限制；账号已用/剩余次数请在商店中刷新。"));
      child->setToolTip(2, price);
      child->setToolTip(3, period + (endAt.isValid() ? QStringLiteral("\n截止时刻：%1，到点隐藏。").arg(endAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))) : QString{}) +
          QStringLiteral("\n按北京时间显示；未注明结束时间不代表永久开放。"));
      child->setData(0, KeyRole, good.stableKey());
      child->setData(0, ManualRole, good.manualSelectionRequired);
      child->setToolTip(0, good.displayName() + QStringLiteral("\n") + good.rewardRaw);
      child->setCheckState(0, !excludedDraft_.contains(good.stableKey()) &&
          (!good.manualSelectionRequired || draft_.contains(good.stableKey())) ? Qt::Checked : Qt::Unchecked);
      child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
    }
    if (parent->childCount() == 0) { delete parent; continue; }
    parent->setFlags(parent->flags() | Qt::ItemIsUserCheckable);
    updateParent(parent);
  }
  filter();
}
void ShopSelectionDialog::updateParent(QTreeWidgetItem* parent) {
  int checked = 0;
  for (int i = 0; i < parent->childCount(); ++i) checked += parent->child(i)->checkState(0) == Qt::Checked;
  parent->setCheckState(0, checked == parent->childCount() ? Qt::Checked : checked ? Qt::PartiallyChecked : Qt::Unchecked);
  parent->setText(1, QStringLiteral("已选 %1 / %2 项").arg(checked).arg(parent->childCount()));
}
void ShopSelectionDialog::filter() {
  const QString query = search_->text().trimmed();
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    auto* parent = tree_->topLevelItem(i);
    const bool shopMatch = parent->text(0).contains(query, Qt::CaseInsensitive) || parent->toolTip(0).contains(query, Qt::CaseInsensitive);
    int visible = 0;
    for (int j = 0; j < parent->childCount(); ++j) {
      auto* child = parent->child(j);
      const bool match = shopMatch || child->text(0).contains(query, Qt::CaseInsensitive);
      child->setHidden(!match); visible += match;
    }
    parent->setHidden(!visible);
    if (!query.isEmpty() && visible) parent->setExpanded(true);
  }
}
void ShopSelectionDialog::changeCheck(QTreeWidgetItem* child, Qt::CheckState state) {
  child->setCheckState(0, state);
  const auto key = child->data(0, KeyRole).toString();
  if (state == Qt::Checked) {
    excludedDraft_.remove(key);
    if (child->data(0, ManualRole).toBool()) draft_.insert(key);
  } else {
    draft_.remove(key);
    if (!child->data(0, ManualRole).toBool()) excludedDraft_.insert(key);
  }
}
void ShopSelectionDialog::updateDirty() {
  dirty_ = catalog_ && (draft_ != catalog_->manualSelection || excludedDraft_ != catalog_->excludedAutomatic);
}
void ShopSelectionDialog::setChecks(Qt::CheckState state) {
  if (!editable_ || saving_ || updating_) return;
  const QSignalBlocker blocker(tree_);
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    auto* parent = tree_->topLevelItem(i);
    if (parent->isHidden()) continue;
    for (int j = 0; j < parent->childCount(); ++j) {
      auto* child = parent->child(j);
      if (!child->isHidden()) changeCheck(child, state);
    }
    updateParent(parent);
  }
  updateDirty();
  updateSummary();
}
void ShopSelectionDialog::updateSummary() {
  int automatic = 0, automaticChosen = 0, manual = 0, chosen = 0;
  const auto now = QDateTime::currentDateTimeUtc();
  if (catalog_) for (const auto& shop : catalog_->discoveredShops) for (const auto& good : shop.goods) {
    if (!good.isOnlineAt(now)) continue;
    if (!good.manualSelectionRequired) { ++automatic; automaticChosen += !excludedDraft_.contains(good.stableKey()); }
    else { ++manual; chosen += draft_.contains(good.stableKey()) && !excludedDraft_.contains(good.stableKey()); }
  }
  summary_->setText(QStringLiteral("当前开放 %1 个商店 · 默认项目已选 %2 / %3 项 · 手动已选 %4 / %5 项%6")
      .arg(tree_ ? tree_->topLevelItemCount() : 0).arg(automaticChosen).arg(automatic).arg(chosen).arg(manual)
      .arg(dirty_ ? QStringLiteral(" · 尚未保存") : QString{}));
  const bool edit = editable_ && !saving_ && !updating_;
  tree_->setEnabled(edit); selectAll_->setEnabled(edit); clear_->setEnabled(edit);
  save_->setEnabled(edit && dirty_); scan_->setEnabled(!updating_ && !saving_);
}
