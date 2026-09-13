#include "pet_raw_data_tree.h"

#include <QHeaderView>
#include <QJsonArray>
#include <QShowEvent>
#include <iterator>

namespace {
QString valueText(const QJsonValue& value) {
  if (value.isString())
    return value.toString();
  if (value.isDouble())
    return value.toVariant().toString();
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
      {QStringLiteral("sgsp"), QStringLiteral("精灵星神背包")},
      {QStringLiteral("sguln"), QStringLiteral("精灵星神背包已解锁扩展格")},
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



}

namespace {
constexpr int kRawValueRole = Qt::UserRole;
constexpr int kRawOffsetRole = Qt::UserRole + 1;
constexpr int kRawPopulatedRole = Qt::UserRole + 2;
constexpr int kRawPageSize = 128;
}

PetRawDataTree::PetRawDataTree(QWidget* parent) : QTreeWidget(parent) {
  setObjectName(QStringLiteral("KQPetRawDataTree"));
  setColumnCount(2);
  setHeaderLabels({QStringLiteral("原始字段"), QStringLiteral("值")});
  header()->setSectionResizeMode(0, QHeaderView::Interactive);
  header()->resizeSection(0, 210);
  header()->setStretchLastSection(true);
  setAlternatingRowColors(true);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setUniformRowHeights(true);
  connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { populateItem(item); });
}
void PetRawDataTree::setJson(const QJsonObject& value) {
  setRecord(value, {});
}
PetRawDataTree::~PetRawDataTree() {
  blockSignals(true);
  clear(); value_ = {}; record_.reset();
}
void PetRawDataTree::setRecord(const QJsonObject& value, RawPetRecordHandle record) {
  if (value_ == value && !dirty_) { record_ = std::move(record); return; }
  // Keep the old handle until both old nodes and the old JSON are released.
  clear();
  value_ = value;
  record_ = std::move(record);
  dirty_ = true;
  if (value_.isEmpty() || isVisible()) populateRoot();
}
void PetRawDataTree::clearJson() { setJson({}); }
void PetRawDataTree::showEvent(QShowEvent* event) {
  if (dirty_) populateRoot();
  QTreeWidget::showEvent(event);
}
void PetRawDataTree::populateRoot() {
  clear();
  dirty_ = false;
  if (value_.isEmpty()) {
    auto* item = new QTreeWidgetItem(this);
    item->setText(0, QStringLiteral("提示"));
    item->setText(1, QStringLiteral("请选择一只精灵"));
    return;
  }
  appendPage(value_, 0, nullptr);
}
void PetRawDataTree::addValue(const QString& key, const QJsonValue& value, QTreeWidgetItem* parent) {
  auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
  item->setText(0, friendlyKey(key));
  item->setText(1, valueText(value));
  item->setToolTip(0, key);
  item->setToolTip(1, valueText(value));
  const bool children = (value.isObject() && !value.toObject().isEmpty()) ||
                        (value.isArray() && !value.toArray().isEmpty());
  if (children) {
    item->setData(0, kRawValueRole, QVariant::fromValue(value));
    item->setData(0, kRawOffsetRole, 0);
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
  }
}
void PetRawDataTree::appendPage(const QJsonValue& value, int offset, QTreeWidgetItem* parent) {
  const int count = value.isObject() ? int(value.toObject().size()) : int(value.toArray().size());
  const int end = qMin(count, offset + kRawPageSize);
  if (value.isObject()) {
    const auto object = value.toObject();
    auto iterator = object.constBegin();
    std::advance(iterator, offset);
    for (int index = offset; index < end; ++index, ++iterator)
      addValue(iterator.key(), iterator.value(), parent);
  } else {
    const auto array = value.toArray();
    for (int index = offset; index < end; ++index)
      addValue(QStringLiteral("[%1]").arg(index), array.at(index), parent);
  }
  if (end < count) {
    auto* more = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
    more->setText(0, QStringLiteral("展开后续 %1 项").arg(count - end));
    more->setText(1, QStringLiteral("按需显示，每次最多 %1 项").arg(kRawPageSize));
    more->setData(0, kRawValueRole, QVariant::fromValue(value));
    more->setData(0, kRawOffsetRole, end);
    more->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
  }
}
void PetRawDataTree::populateItem(QTreeWidgetItem* item) {
  if (!item || item->data(0, kRawPopulatedRole).toBool()) return;
  const auto value = item->data(0, kRawValueRole).value<QJsonValue>();
  if (!value.isArray() && !value.isObject()) return;
  item->setData(0, kRawPopulatedRole, true);
  appendPage(value, item->data(0, kRawOffsetRole).toInt(), item);
}
