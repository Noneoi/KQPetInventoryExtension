#include "pet_table_model.h"

#include "pet_detail_analyzer.h"
#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_image_cache.h"
#include "pet_move_policy.h"
#include "pet_repository.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QIcon>
#include <QPalette>

namespace {

QString battlePowerText(const PetBattlePowerState& state) {
  if (!state.hasCurrent && !state.hasExtreme) return QStringLiteral("—");
  const QString current = state.hasCurrent ? QString::number(state.current)
                                           : QStringLiteral("—");
  const QString extreme = state.hasExtreme ? QString::number(state.extreme)
                                           : QStringLiteral("—");
  if (state.isHighest) return QStringLiteral("%1 / %2（最高）").arg(current, extreme);
  if (state.hasCurrent && state.hasExtreme && state.hasHighest)
    return QStringLiteral("%1 / %2（距最高 %3）")
        .arg(current, extreme)
        .arg(state.highestGap);
  if (state.hasCurrent && state.hasExtreme)
    return QStringLiteral("%1 / %2（最高待确认）").arg(current, extreme);
  return QStringLiteral("%1 / %2").arg(current, extreme);
}

QString positionText(const QJsonObject& pet, PetTableModel::Location location) {
  if (location == PetTableModel::Location::Warehouse) {
    return pet.value(QStringLiteral("_warehouseGroup")).toString() ==
                   QStringLiteral("elite")
               ? QStringLiteral("精英")
               : QStringLiteral("普通");
  }
  if (!pet.contains(QStringLiteral("_position"))) return QStringLiteral("待刷新");
  const int index = qMax(0, pet.value(QStringLiteral("_position")).toInt());
  return QStringLiteral("第%1页 第%2排")
      .arg(index / 12 + 1)
      .arg(index % 12 / 6 + 1);
}

}  // namespace

PetTableModel::PetTableModel(Location location, PetRepository* repository,
                             PetImageCache* imageCache, QObject* parent)
    : QAbstractTableModel(parent),
      location_(location),
      repository_(repository),
      imageCache_(imageCache) {}

int PetTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : pets_.size();
}

int PetTableModel::columnCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return location_ == Location::Backpack ? 8 : 7;
}

QVariant PetTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= pets_.size() ||
      index.column() < 0 || index.column() >= columnCount())
    return {};
  const QJsonObject& pet = pets_.at(index.row());
  const qint64 instanceId = petInstanceId(pet);
  if (role == InstanceIdRole) return instanceId;
  if (role == IdentityTextRole)
    return QStringLiteral("%1 %2").arg(instanceId).arg(petRaceId(pet));
  if (role == PetObjectRole) return pet;
  if (role == Qt::DisplayRole) return displayValue(pet, index.column());

  if (role == Qt::ToolTipRole && index.column() == 0) {
    const QString name = displayName(pet);
    const QString original = PetDetailCatalog::instance().resolvedOriginalName(pet);
    return original.isEmpty() || original == name
               ? name
               : QStringLiteral("皮肤/当前名称：%1\n原名：%2").arg(name, original);
  }
  if (role == Qt::ToolTipRole && index.column() == 5) {
    if (location_ == Location::Backpack)
      return QStringLiteral("战力来自本次背包完整数据");
    const QDateTime cachedAt = repository_ ? repository_->detailSavedAt(instanceId)
                                           : QDateTime();
    return cachedAt.isValid()
               ? QStringLiteral("战力来自本地详情缓存：%1")
                     .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
               : QStringLiteral("尚无该实例ID的本地详情缓存");
  }
  if (role == Qt::DecorationRole && index.column() == 1 && imageCache_) {
    const QString attributes =
        PetDetailCatalog::instance().metadataFor(pet)
            .value(QStringLiteral("attributes"))
            .toString();
    return imageCache_->attributeIcon(attributes);
  }
  if (location_ == Location::Backpack && index.column() == 6) {
    const bool deployed = displayValue(pet, 6) == QStringLiteral("是");
    if (role == Qt::FontRole && deployed) {
      QFont font;
      font.setBold(true);
      return font;
    }
    if (role == Qt::ForegroundRole && deployed)
      return QBrush(QColor(220, 38, 38));
  }
  return {};
}

QVariant PetTableModel::headerData(int section, Qt::Orientation orientation,
                                   int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  QStringList headers = {QStringLiteral("名称"), QStringLiteral("属性"),
                         QStringLiteral("职业"), QStringLiteral("时代"),
                         QStringLiteral("等级"),
                         QStringLiteral("战斗力 / 极限战斗力")};
  if (location_ == Location::Backpack) headers.append(QStringLiteral("是否出阵"));
  headers.append(QStringLiteral("位置"));
  return section >= 0 && section < headers.size() ? QVariant(headers.at(section))
                                                   : QVariant();
}

void PetTableModel::setPets(const QList<QJsonObject>& pets) {
  beginResetModel();
  pets_ = pets;
  endResetModel();
}

bool PetTableModel::updatePet(const QJsonObject& pet) {
  const int row = rowForInstanceId(petInstanceId(pet));
  if (row < 0) return false;
  pets_[row] = pet;
  emit dataChanged(index(row, 0), index(row, columnCount() - 1));
  return true;
}

QJsonObject PetTableModel::petAt(int row) const {
  return row >= 0 && row < pets_.size() ? pets_.at(row) : QJsonObject();
}

qint64 PetTableModel::instanceIdAt(int row) const {
  return petInstanceId(petAt(row));
}

int PetTableModel::rowForInstanceId(qint64 instanceId) const {
  for (int row = 0; row < pets_.size(); ++row)
    if (petInstanceId(pets_.at(row)) == instanceId) return row;
  return -1;
}

PetTableModel::Location PetTableModel::location() const { return location_; }

QString PetTableModel::displayName(const QJsonObject& pet) const {
  const QString custom = pet.value(QStringLiteral("customName")).toString();
  if (!custom.isEmpty()) return custom;
  const QString name = pet.value(QStringLiteral("n")).toString();
  return name.isEmpty() ? QStringLiteral("未命名") : name;
}

QString PetTableModel::displayValue(const QJsonObject& pet, int column) const {
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  switch (column) {
    case 0: return displayName(pet);
    case 1: return catalog.resolvedAttributes(pet);
    case 2: return catalog.resolvedJobs(pet);
    case 3: return catalog.resolvedEra(pet);
    case 4: return pet.value(QStringLiteral("lv")).toVariant().toString();
    case 5: return battlePowerText(PetDetailAnalyzer::analyzeBattlePower(pet));
    case 6:
      return location_ == Location::Backpack
                 ? PetMovePolicy::deploymentText(pet)
                 : positionText(pet, location_);
    case 7: return positionText(pet, location_);
    default: return {};
  }
}
