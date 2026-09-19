#include "snapshot_history_model.h"
#include "ui/common/display_text.h"

#include "domain/asset_snapshot_comparator.h"

SnapshotHistoryModel::SnapshotHistoryModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int SnapshotHistoryModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : snapshots_.size();
}

int SnapshotHistoryModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : 5;
}

QVariant SnapshotHistoryModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= snapshots_.size() ||
      role != Qt::DisplayRole)
    return {};
  const AccountAssetSnapshot& snapshot = snapshots_.at(index.row());
  switch (index.column()) {
    case 0: return snapshot.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case 1: return snapshot.totalPets;
    case 2: return snapshot.fullyCultivatedPets;
    case 3: return snapshot.totalCurrentPowerKnown ? QString::number(snapshot.totalCurrentPower)
        : QStringLiteral("部分未知（已记录 %1）").arg(snapshot.totalCurrentPower);
    case 4: {
      if (index.row() == 0) return QStringLiteral("首个快照");
      if (!snapshot.petsComplete || !snapshots_.at(index.row() - 1).petsComplete)
        return QStringLiteral("摘要；比较需读取个体指标");
      const AssetSnapshotDelta delta = AssetSnapshotComparator::compare(
          snapshot, snapshots_.at(index.row() - 1));
      if (!delta.accountComparable) return QStringLiteral("账号或实例数据不可比较");
      if (!delta.cultivationComparable)
        return QStringLiteral("新增 %1；移出 %2；算法版本不同，不计算新达标")
            .arg(delta.newPets).arg(delta.removedPets);
      return QStringLiteral("新增 %1；满培养 +%2；星神满战力 +%3；星轮 +%4；可比个体战力 %5")
          .arg(delta.newPets).arg(delta.newlyFullyCultivated)
          .arg(delta.newlyRedStarComplete).arg(delta.newlyAstrolabeBreakthrough)
          .arg(delta.powerChangeKnown ? signedNumberText(delta.totalPowerChange)
                                     : QStringLiteral("部分未知"));
    }
  }
  return {};
}

QVariant SnapshotHistoryModel::headerData(int section,
                                          Qt::Orientation orientation,
                                          int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const QStringList headers = {
      QStringLiteral("快照日期"), QStringLiteral("精灵总数"),
      QStringLiteral("已满培养"), QStringLiteral("账号总战力"),
      QStringLiteral("较上次变化")};
  return headers.value(section);
}

void SnapshotHistoryModel::setSnapshots(
    const QList<AccountAssetSnapshot>& snapshots) {
  beginResetModel();
  snapshots_ = snapshots;
  endResetModel();
}
