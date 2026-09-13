#include "asset_analysis_model.h"

#include <QBrush>
#include <QColor>

AssetAnalysisModel::AssetAnalysisModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int AssetAnalysisModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : pets_.size();
}

int AssetAnalysisModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

const PetAssetRecord* AssetAnalysisModel::petAt(int row) const {
  return row >= 0 && row < pets_.size() ? &pets_.at(row) : nullptr;
}

QVariant AssetAnalysisModel::data(const QModelIndex& index, int role) const {
  const PetAssetRecord* pet = petAt(index.row());
  if (!pet || index.column() < 0 || index.column() >= ColumnCount) return {};
  if (role == InstanceIdRole) return pet->instanceId;
  if (role == Qt::ToolTipRole) {
    if (index.column() == CurrentPower)
      return QStringLiteral("按本宠已装备与对应背包中可用星神、实际槽位等级计算；不把官方返回总数当作全部培养状态。待装备调整会单独列出。");
    if (index.column() == MaximumPower)
      return QStringLiteral("官方极限是官方基准；至高按各适用培养分项、星神种类与栏位、等级和星轮突破独立计算。信息不足时不推断至高或满培养。");
    if (index.column() == Gaps) return pet->gaps.join(QLatin1Char('\n'));
  }
  if (role == SortRole) {
    switch (index.column()) {
      case Pet: return pet->name;
      case Location: return pet->location;
      case CurrentPower: return pet->currentPowerKnown ? pet->currentPower : -1;
      case MaximumPower: return pet->highestPowerKnown ? pet->highestPower : -1;
      case Completion: return pet->completionKnown ? pet->completionPercent : -1;
      case Gaps: return pet->gaps.size();
      case Shop: return pet->shopImprovable ? 1 : 0;
    }
  }
  if (role == Qt::ForegroundRole) {
    if (index.column() == Completion && pet->completionKnown)
      return QBrush(QColor(pet->fullyCultivated ? QStringLiteral("#087a43")
                                               : QStringLiteral("#b54708")));
    if (index.column() == Shop && pet->shopImprovable)
      return QBrush(QColor(QStringLiteral("#c62828")));
    return {};
  }
  if (role != Qt::DisplayRole) return {};
  switch (index.column()) {
    case Pet:
      return QStringLiteral("%1\n实例 %2").arg(pet->name).arg(pet->instanceId);
    case Location: return pet->location;
    case CurrentPower:
      return pet->currentPowerKnown ? QString::number(pet->currentPower)
                                  : QStringLiteral("—");
    case MaximumPower:
      return pet->detailAvailable
                 ? QStringLiteral("%1 / %2")
                       .arg(pet->extremePowerKnown ? QString::number(pet->extremePower) : QStringLiteral("—"),
                            pet->highestPowerKnown ? QString::number(pet->highestPower) : QStringLiteral("—"))
                 : QStringLiteral("缺少详情缓存");
    case Completion:
      return pet->completionKnown
                 ? QStringLiteral("%1%").arg(pet->completionPercent)
                 : QStringLiteral("—");
    case Gaps:
      return !pet->detailAvailable
                 ? QStringLiteral("等待详情刷新")
                 : pet->fullyCultivated
                       ? (pet->gapKeys.contains(QStringLiteral("sg_equip"))
                             ? QStringLiteral("培养已具备，待调整装备；%1").arg(pet->gaps.join(QStringLiteral("；")))
                             : QStringLiteral("全部适用培养项已满（至高）"))
                       : pet->gaps.isEmpty()
                             ? (pet->cultivationKnown ? QStringLiteral("查看精灵分析核对剩余动作")
                                                     : QStringLiteral("待补详情或官方数据，不能判断满培养"))
                             : pet->gaps.join(QStringLiteral("；")) +
                                   (pet->cultivationKnown ? QString{} : QStringLiteral("；部分培养数据待补"));
    case Shop:
      return !pet->detailAvailable ? QStringLiteral("待补详情")
                             : !shopDataKnown_ ? QStringLiteral("未查询")
                             : pet->shopImprovable ? QStringLiteral("有已知项目")
                                                   : QStringLiteral("未发现已知项目");
  }
  return {};
}

QVariant AssetAnalysisModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const QStringList headers = {
      QStringLiteral("精灵"), QStringLiteral("位置"),
      QStringLiteral("持有可达战斗力"), QStringLiteral("官方极限 / 至高战斗力"),
      QStringLiteral("完成度"), QStringLiteral("主要培养缺口"),
      QStringLiteral("商店可提升")};
  return headers.value(section);
}

void AssetAnalysisModel::setOverview(const AccountAssetOverview& overview) {
  beginResetModel();
  pets_ = overview.pets;
  shopDataKnown_ = overview.shopDataKnown;
  endResetModel();
}

void AssetAnalysisModel::clear() {
  beginResetModel();
  pets_.clear();
  shopDataKnown_ = false;
  endResetModel();
}
