#include "recommendation_model.h"

#include <QBrush>
#include <QColor>

namespace {

QString statusText(RecommendationType type) {
  switch (type) {
    case RecommendationType::ReadyNow: return QStringLiteral("按已知数据可处理");
    case RecommendationType::ResourceMissing: return QStringLiteral("还缺资源");
    case RecommendationType::ConditionUnknown: return QStringLiteral("条件待确认");
    case RecommendationType::NearFullCultivation: return QStringLiteral("接近满培养");
    case RecommendationType::LocalCultivation: return QStringLiteral("本地培养建议");
  }
  return {};
}

QString resourcesText(const ActionRecommendation& recommendation) {
  if (recommendation.type == RecommendationType::LocalCultivation)
    return QStringLiteral("按已有精灵资源与栏位判断；升级消耗请在游戏中查看");
  if (recommendation.type == RecommendationType::NearFullCultivation) {
    return QStringLiteral("本地战力 %1\n至高战力 %2\n潜在提升 +%3")
        .arg(recommendation.currentPower)
        .arg(recommendation.highestPower)
        .arg(qMax(0, recommendation.highestPower -
                         recommendation.currentPower));
  }
  QStringList lines;
  for (const ResourceRequirement& requirement : recommendation.requirements) {
    if (!requirement.ownedKnown) {
      lines.append(QStringLiteral("%1  未知 / %2")
                       .arg(requirement.resourceName)
                       .arg(requirement.required));
    } else if (requirement.owned < requirement.required) {
      lines.append(QStringLiteral("%1  %2 / %3  缺 %4")
                       .arg(requirement.resourceName)
                       .arg(requirement.owned)
                       .arg(requirement.required)
                       .arg(requirement.missing()));
    } else {
      lines.append(QStringLiteral("%1  %2 / %3  ✓")
                       .arg(requirement.resourceName)
                       .arg(requirement.owned)
                       .arg(requirement.required));
    }
  }
  return lines.join(QLatin1Char('\n'));
}

QString conclusionText(const ActionRecommendation& recommendation) {
  QString text;
  if (recommendation.cultivationStale)
    text = QStringLiteral("建议基于上次养成分析");
  else if (recommendation.shopStale)
    text = QStringLiteral("商店相关建议可能已过期");
  else if (recommendation.type == RecommendationType::ReadyNow)
    text = recommendation.actionableCountKnown
               ? QStringLiteral("当前最多可处理 %1；处理后预计剩余缺口 %2")
                     .arg(recommendation.actionableCount)
                     .arg(recommendation.remainingGapAfterAction)
               : QStringLiteral("当前资源满足一次兑换条件");
  else if (recommendation.type == RecommendationType::ResourceMissing) {
    QStringList missing;
    for (const ResourceRequirement& requirement : recommendation.requirements)
      if (requirement.missing() > 0)
        missing.append(QStringLiteral("%1 ×%2")
                           .arg(requirement.resourceName)
                           .arg(requirement.missing()));
    text = QStringLiteral("还缺 %1").arg(missing.join(QStringLiteral("、")));
  }
  else if (recommendation.type == RecommendationType::ConditionUnknown) {
    QStringList reasons;
    for (const ShopCondition* condition : {&recommendation.eligibilityCondition,
             &recommendation.costCondition, &recommendation.resourceCondition,
             &recommendation.limitCondition, &recommendation.unlockCondition}) {
      if (condition->effectiveState() != ShopConditionState::Satisfied && !condition->reason.isEmpty())
        reasons.append(condition->reason);
    }
    reasons.removeDuplicates();
    text = reasons.isEmpty() ? QStringLiteral("必要条件尚未确认") : reasons.join(QStringLiteral("；"));
  }
  else if (recommendation.type == RecommendationType::LocalCultivation)
    text = recommendation.gaps.isEmpty() ? QStringLiteral("先补齐详情，再确认剩余培养项")
        : QStringLiteral("按列出的缺口在游戏中培养；已经具备的星神无需重复兑换");
  else
    text = QStringLiteral("当前兑换商店没有发现可立即处理的真实项目");
  if (recommendation.alternateGoodCount > 0)
    text += QStringLiteral("；另有 %1 个适用兑换项目")
                .arg(recommendation.alternateGoodCount);
  return text;
}

}  // namespace

RecommendationModel::RecommendationModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int RecommendationModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : recommendations_.size();
}

int RecommendationModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

QVariant RecommendationModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= recommendations_.size())
    return {};
  const ActionRecommendation& recommendation = recommendations_.at(index.row());
  if (role == PetInstanceIdRole) return recommendation.petInstanceId;
  if (role == ShopGoodKeyRole) return recommendation.shopGoodKey;
  if (role == StableIdRole) return recommendation.stableId;
  if (role == Qt::ToolTipRole) return conclusionText(recommendation);
  if (role == Qt::ForegroundRole && index.column() == Status) {
    if (recommendation.type == RecommendationType::ReadyNow)
      return QBrush(QColor(QStringLiteral("#087a43")));
    if (recommendation.type == RecommendationType::ResourceMissing)
      return QBrush(QColor(QStringLiteral("#b42318")));
    return QBrush(QColor(QStringLiteral("#8a5a00")));
  }
  if (role != Qt::DisplayRole) return {};
  switch (index.column()) {
    case Status: return statusText(recommendation.type);
    case Pet:
      return QStringLiteral("%1\n实例 %2 · 完成度 %3")
          .arg(recommendation.petName)
          .arg(recommendation.petInstanceId)
          .arg(recommendation.completionKnown ? QStringLiteral("%1%").arg(recommendation.completionPercent)
                                              : QStringLiteral("待补数据"));
    case Gap: return recommendation.gaps.join(QLatin1Char('\n'));
    case Project:
      return recommendation.shopGoodKey.isEmpty()
                 ? (recommendation.type == RecommendationType::LocalCultivation
                       ? QStringLiteral("本地培养，无兑换项目") : QStringLiteral("未发现可立即处理的真实项目"))
                 : QStringLiteral("%1\n%2")
                       .arg(recommendation.goodName,
                            recommendation.shopName);
    case Resources: return resourcesText(recommendation);
    case Remaining:
      return recommendation.shopGoodKey.isEmpty()
                 ? QStringLiteral("—")
                 : recommendation.remainingExchangeCount < 0
                     ? QStringLiteral("次数待确认")
                     : QStringLiteral("剩余 %1 次").arg(recommendation.remainingExchangeCount);
    case Conclusion: return conclusionText(recommendation);
    case ViewPet: return QStringLiteral("查看精灵");
    case ViewShop:
      return recommendation.shopGoodKey.isEmpty()
                 ? QStringLiteral("—")
                 : QStringLiteral("查看兑换");
  }
  return {};
}

QVariant RecommendationModel::headerData(int section,
                                         Qt::Orientation orientation,
                                         int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const QStringList headers = {
      QStringLiteral("状态"), QStringLiteral("精灵"), QStringLiteral("培养缺口"),
      QStringLiteral("真实兑换项目 / 商店"), QStringLiteral("账号资源"),
      QStringLiteral("兑换次数"), QStringLiteral("结论"),
      QStringLiteral("精灵入口"), QStringLiteral("兑换入口")};
  return headers.value(section);
}

void RecommendationModel::setRecommendations(
    const QList<ActionRecommendation>& recommendations) {
  beginResetModel();
  recommendations_ = recommendations;
  endResetModel();
}

