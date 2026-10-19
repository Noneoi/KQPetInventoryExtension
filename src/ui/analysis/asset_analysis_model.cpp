#include "asset_analysis_model.h"
#include "domain/asset_derivation.h"

#include <QBrush>
#include <QColor>

namespace {
bool gapInCategory(const QString& key, const QString& category) {
  if (category.isEmpty()) return true;
  if (category == "astrolabe") return key == "asv" || key.startsWith("astrolabe_");
  if (category == "sacred") return key == "sjv" || key.startsWith("sacred_");
  if (category == "badges") return key == "bsv" || key.startsWith("badge_");
  if (category == "stargods") return key == "sgv" || key.startsWith("sg_");
  return false;
}

QString conciseGaps(const PetAssetRecord& pet, const QString& category) {
  if (!pet.detailAvailable) return QStringLiteral("等待详情刷新");
  const auto has = [&pet,&category](const char* key) {
    return gapInCategory(QLatin1String(key),category) && pet.gapKeys.contains(QLatin1String(key));
  };
  if (pet.fullyCultivated)
    return has("sg_equip") ? QStringLiteral("星神待装备/调整") :
        category.isEmpty() ? QStringLiteral("培养已满") : QStringLiteral("暂无该项缺口");
  QStringList parts;
  if (has("sg_changeable")) parts.append(QStringLiteral("缺少万变红星"));
  if ((category.isEmpty() || category == "stargods") && pet.redStarKnown && pet.missingRedStars > 0)
    parts.append(QStringLiteral("缺少%1颗普通红星").arg(pet.missingRedStars));
  if (has("sg_level")) parts.append(QStringLiteral("星神等级未满"));
  if (has("sg_equip")) parts.append(QStringLiteral("星神待装备/调整"));
  if (parts.isEmpty() && (has("sgv") || has("sg_acquire"))) parts.append(QStringLiteral("星神待提升"));
  const QList<QPair<QString,QString>> labels{
      {QStringLiteral("lv"),QStringLiteral("等级未满")},
      {QStringLiteral("iv"),QStringLiteral("天赋/星能待提升")},
      {QStringLiteral("pl"),QStringLiteral("潜能未满")},
      {QStringLiteral("ep"),QStringLiteral("源兽装备待提升")},
      {QStringLiteral("lsv"),QStringLiteral("传说石待提升")},
      {QStringLiteral("sacred_star"),QStringLiteral("神源兽星级未满")},
      {QStringLiteral("sacred_stage"),QStringLiteral("神源兽阶级未满")},
      {QStringLiteral("badge_level"),QStringLiteral("元魂等级未满")},
      {QStringLiteral("badge_awaken"),QStringLiteral("专属元魂未觉醒")},
      {QStringLiteral("astrolabe_light"),QStringLiteral("星轮未点满")},
      {QStringLiteral("astrolabe_breakthrough"),QStringLiteral("星轮未突破")}};
  for (const auto& label : labels)
    if (gapInCategory(label.first,category) && pet.gapKeys.contains(label.first)) parts.append(label.second);
  if (has("sjv") && !has("sacred_star") && !has("sacred_stage")) parts.append(QStringLiteral("神源兽待提升"));
  if (has("bsv") && !has("badge_level") && !has("badge_awaken")) parts.append(QStringLiteral("元魂待提升"));
  if (has("asv") && !has("astrolabe_light") && !has("astrolabe_breakthrough")) parts.append(QStringLiteral("星轮待提升"));
  const QStringList handled{"sgv","sg_acquire","sg_changeable","sg_level","sg_equip","sjv","bsv","asv"};
  for (const auto& key : pet.gapKeys) {
    if (!gapInCategory(key,category)) continue;
    bool known = handled.contains(key);
    for (const auto& label : labels) known |= key == label.first;
    if (!known) { parts.append(QStringLiteral("其他培养项待提升")); break; }
  }
  bool unknown = category.isEmpty() ? !pet.cultivationKnown : category == "stargods" && !pet.redStarKnown;
  if (!category.isEmpty()) for (const auto& requirement : pet.cultivationRequirements.items) {
    if (requirement.category != category) continue;
    unknown |= !requirement.known;
  }
  if (unknown) parts.append(QStringLiteral("部分信息待补"));
  if (parts.isEmpty()) return QStringLiteral("待核对培养详情");
  return parts.join(QStringLiteral("；"));
}
} // namespace

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
  if (index.column() == Era && (role == Qt::DisplayRole || role == SortRole)) {
    const auto era = pet->pet.value(QStringLiteral("_metaEra")).toString();
    return era.isEmpty() ? QStringLiteral("未知") : era;
  }
  if (index.column() == Rating && (role == Qt::DisplayRole || role == SortRole)) {
    const auto rating = pet->pet.value(QStringLiteral("_metaRating")).toString();
    return rating.isEmpty() ? QStringLiteral("未知") : rating;
  }
  if (index.column() == Materials && (role == Qt::DisplayRole || role == Qt::ToolTipRole || role == MaterialsHtmlRole)) {
    auto found = materialCells_.constFind(index.row());
    if (found == materialCells_.cend()) {
      materialCells_.insert(index.row(),cultivationMaterialCell(*pet,category_));
      found = materialCells_.constFind(index.row());
    }
    return role == MaterialsHtmlRole ? found->html : found->text;
  }
  if (role == Qt::ToolTipRole) {
    if (index.column() == CurrentPower)
      return QStringLiteral("按本宠已装备与对应背包中可用星神、实际槽位等级计算；不把官方返回总数当作全部培养状态。待装备调整会单独列出。");
    if (index.column() == MaximumPower)
      return QStringLiteral("官方极限是官方基准；至高按各适用培养分项、星神种类与栏位、等级和星轮突破独立计算。信息不足时不推断至高或满培养。");
    if (index.column() == Gaps) return category_.isEmpty() ? pet->gaps.join(QLatin1Char('\n')) : conciseGaps(*pet,category_);
  }
  if (role == SortRole) {
    switch (index.column()) {
      case Pet: return pet->name;
      case Location: return pet->location;
      case CurrentPower: return pet->currentPowerKnown ? pet->currentPower : -1;
      case MaximumPower: return pet->highestPowerKnown ? pet->highestPower : -1;
      case Completion: return pet->completionKnown ? pet->completionPercent : -1;
      case Gaps: {
        if (category_.isEmpty()) return pet->gaps.size();
        int count = 0;
        for (const auto& key : pet->gapKeys) if (gapInCategory(key,category_)) ++count;
        return count;
      }
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
      return conciseGaps(*pet,category_);
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
  if (section == Gaps && !category_.isEmpty())
    return (category_ == "astrolabe" ? QStringLiteral("星轮") : cultivationMaterialCategoryName(category_)) + QStringLiteral("缺口");
  static const QStringList headers = {
      QStringLiteral("精灵"), QStringLiteral("位置"),
      QStringLiteral("持有可达战斗力"), QStringLiteral("官方极限 / 至高战斗力"),
      QStringLiteral("完成度"), QStringLiteral("主要培养缺口"),
      QStringLiteral("商店可提升"), QStringLiteral("时代"), QStringLiteral("评级"), QStringLiteral("所需养成材料")};
  return headers.value(section);
}

void AssetAnalysisModel::setCultivationCategory(const QString& category) {
  if (category_ == category) return;
  category_ = category;
  materialCells_.clear();
  if (!pets_.isEmpty()) {
    emit dataChanged(index(0,Materials),index(pets_.size()-1,Materials));
    emit dataChanged(index(0,Gaps),index(pets_.size()-1,Gaps),{Qt::DisplayRole,Qt::ToolTipRole,SortRole});
  }
  emit headerDataChanged(Qt::Horizontal,Gaps,Gaps);
}

void AssetAnalysisModel::setOverview(const AccountAssetOverview& overview) {
  beginResetModel();
  pets_ = overview.pets;
  materialCells_.clear();
  shopDataKnown_ = overview.shopDataKnown;
  endResetModel();
}

void AssetAnalysisModel::clear() {
  beginResetModel();
  pets_.clear();
  materialCells_.clear();
  shopDataKnown_ = false;
  endResetModel();
}
