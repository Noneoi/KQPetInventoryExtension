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
  if (role == SortRole) {
    switch (index.column()) {
      case Pet: return pet->name;
      case Location: return pet->location;
      case CurrentPower: return pet->detailAvailable ? pet->currentPower : -1;
      case MaximumPower: return pet->detailAvailable ? pet->highestPower : -1;
      case Completion: return pet->detailAvailable ? pet->completionPercent : -1;
      case Gaps: return pet->gaps.size();
      case Shop: return pet->shopImprovable ? 1 : 0;
    }
  }
  if (role == Qt::ForegroundRole) {
    if (index.column() == Completion && pet->detailAvailable)
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
      return pet->detailAvailable ? QString::number(pet->currentPower)
                                  : QStringLiteral("—");
    case MaximumPower:
      return pet->detailAvailable
                 ? QStringLiteral("%1 / %2").arg(pet->extremePower).arg(pet->highestPower)
                 : QStringLiteral("缺少详情缓存");
    case Completion:
      return pet->detailAvailable
                 ? QStringLiteral("%1%").arg(pet->completionPercent)
                 : QStringLiteral("—");
    case Gaps:
      return !pet->detailAvailable
                 ? QStringLiteral("等待详情刷新")
                 : pet->fullyCultivated
                       ? QStringLiteral("已达到当前已知最高培养")
                       : pet->gaps.isEmpty()
                             ? QStringLiteral("存在未归类战斗力差距")
                             : pet->gaps.join(QStringLiteral("；"));
    case Shop:
      return !shopDataKnown_ ? QStringLiteral("未查询")
                             : pet->shopImprovable ? QStringLiteral("是")
                                                   : QStringLiteral("否");
  }
  return {};
}

QVariant AssetAnalysisModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const QStringList headers = {
      QStringLiteral("精灵"), QStringLiteral("位置"),
      QStringLiteral("当前战斗力"), QStringLiteral("极限 / 最高战斗力"),
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
