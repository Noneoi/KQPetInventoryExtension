#include "snapshot_history_model.h"

#include "asset_snapshot_comparator.h"

namespace {
QString signedNumber(qint64 value) {
  return value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
}
}

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
    case 0: return snapshot.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case 1: return snapshot.totalPets;
    case 2: return snapshot.fullyCultivatedPets;
    case 3: return snapshot.totalCurrentPower;
    case 4: {
      if (index.row() == 0) return QStringLiteral("首个快照");
      const AssetSnapshotDelta delta = AssetSnapshotComparator::compare(
          snapshot, snapshots_.at(index.row() - 1));
      return QStringLiteral("新增 %1；满培养 +%2；星神满战力 +%3；星轮 +%4；战力 %5")
          .arg(delta.newPets).arg(delta.newlyFullyCultivated)
          .arg(delta.newlyRedStarComplete).arg(delta.newlyAstrolabeBreakthrough)
          .arg(signedNumber(delta.totalPowerChange));
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
