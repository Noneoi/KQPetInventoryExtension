#include "shop_pet_eligibility.h"

#include "pet_detail_catalog.h"

#include <QHash>
#include <QJsonObject>
#include <QStringList>

namespace {

enum class ComponentState { Useful, Full, Unknown };

ComponentState powerGap(const QJsonObject& pet, const QString& key) {
  const QJsonObject current = pet.value(QStringLiteral("czdlv")).toObject();
  const QJsonObject maximum = pet.value(QStringLiteral("mzdlv")).toObject();
  if (!current.contains(key) || !maximum.contains(key)) return ComponentState::Unknown;
  const int currentValue = current.value(key).toInt();
  const int maximumValue = maximum.value(key).toInt();
  if (maximumValue <= 0) return ComponentState::Unknown;
  return currentValue < maximumValue ? ComponentState::Useful : ComponentState::Full;
}

ComponentState oneRedStargod(const QJsonObject& pet) {
  const QString sequence = pet.value(QStringLiteral("sgs")).toString();
  if (sequence.isEmpty()) return ComponentState::Unknown;
  bool sawSlot = false;
  for (const QString& slot : sequence.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
    const QStringList fields = slot.split(QLatin1Char(':'));
    const int defineId = fields.value(0).toInt();
    if (defineId <= 0) return ComponentState::Useful;
    sawSlot = true;
    const int sourceId = fields.size() >= 3 ? fields.value(2).toInt() : 0;
    const QJsonObject define = PetDetailCatalog::instance().stargod(defineId);
    const QJsonObject source = sourceId > 0
                                   ? PetDetailCatalog::instance().stargod(sourceId)
                                   : QJsonObject{};
    int quality = define.value(QStringLiteral("quality")).toInt();
    if (source.value(QStringLiteral("changeable")).toBool())
      quality = source.value(QStringLiteral("quality")).toInt(quality);
    if (quality < 6) return ComponentState::Useful;
  }
  return sawSlot ? ComponentState::Full : ComponentState::Unknown;
}

ComponentState exclusiveBadge(const QJsonObject& pet) {
  const QString sequence = pet.value(QStringLiteral("badge")).toString();
  if (sequence.isEmpty()) return ComponentState::Unknown;
  bool sawSlot = false;
  for (const QString& slot : sequence.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
    sawSlot = true;
    const QString exclusive = slot.section(QLatin1Char('#'), 1, 1);
    const int exclusiveId = exclusive.section(QLatin1Char(':'), 0, 0).toInt();
    const bool activated = exclusive.section(QLatin1Char(':'), 1, 1).toInt() > 0;
    if (exclusiveId <= 0 || !activated) return ComponentState::Useful;
  }
  return sawSlot ? ComponentState::Full : ComponentState::Unknown;
}

struct SacredEquipmentLevels {
  int starPlan = 0;
  int stagePlan = 0;
  int star = 0;
  int stage = 0;
  bool valid = false;
};

SacredEquipmentLevels sacredEquipmentLevels(const QJsonObject& pet) {
  const QString sequence = pet.value(QStringLiteral("shenjue")).toString();
  const QString definePart = sequence.section(QLatin1Char('|'), 0, 0);
  const QString levelsPart = sequence.section(QLatin1Char('|'), 1, 1);
  SacredEquipmentLevels levels;
  levels.starPlan = definePart.section(QLatin1Char('#'), 1, 1).toInt();
  levels.stagePlan = definePart.section(QLatin1Char('#'), 2, 2).toInt();
  levels.star = levelsPart.section(QLatin1Char(':'), 0, 0).toInt();
  levels.stage = levelsPart.section(QLatin1Char(':'), 1, 1).toInt();
  levels.valid = definePart.section(QLatin1Char('#'), 0, 0).toInt() > 0 &&
                 levels.starPlan > 0 && levels.stagePlan > 0 &&
                 !levelsPart.isEmpty();
  return levels;
}

int maximumSacredStar(int plan) {
  // Official 2026 plans: plan 1/2/3 cap at 8/10/9 stars. Unknown future
  // plans deliberately return 0 so they cannot produce a false green row.
  if (plan == 1) return 8;
  if (plan == 2) return 10;
  if (plan == 3) return 9;
  return 0;
}

int maximumSacredStage(int plan) {
  // Stage plans are grouped in tiers of 5/6/7. Keep unknown layouts
  // conservative; the current/extreme power gap is not enough to tell
  // whether the missing power comes from stars or stages.
  if (plan >= 1 && plan <= 24) return 5 + ((plan - 1) % 3);
  return 0;
}

ComponentState sourceBeastStars(const QJsonObject& pet) {
  const SacredEquipmentLevels levels = sacredEquipmentLevels(pet);
  if (!levels.valid) return ComponentState::Unknown;
  const int maximum = maximumSacredStar(levels.starPlan);
  if (maximum <= 0) return ComponentState::Unknown;
  return levels.star < maximum ? ComponentState::Useful : ComponentState::Full;
}

ComponentState sourceBeastStage(const QJsonObject& pet) {
  const SacredEquipmentLevels levels = sacredEquipmentLevels(pet);
  if (!levels.valid) return ComponentState::Unknown;
  const int maximum = maximumSacredStage(levels.stagePlan);
  if (maximum <= 0) return ComponentState::Unknown;
  return levels.stage < maximum ? ComponentState::Useful : ComponentState::Full;
}

ComponentState component(const QString& code, const QJsonObject& pet) {
  if (code == QStringLiteral("11")) return powerGap(pet, QStringLiteral("lv"));
  if (code == QStringLiteral("31")) return powerGap(pet, QStringLiteral("sgv"));
  if (code == QStringLiteral("34")) return oneRedStargod(pet);
  if (code == QStringLiteral("41")) return powerGap(pet, QStringLiteral("bsv"));
  if (code == QStringLiteral("44")) return exclusiveBadge(pet);
  if (code == QStringLiteral("62")) return powerGap(pet, QStringLiteral("iv"));
  if (code == QStringLiteral("84")) return powerGap(pet, QStringLiteral("asv"));
  if (code == QStringLiteral("91")) return sourceBeastStars(pet);
  if (code == QStringLiteral("92")) return sourceBeastStage(pet);
  return ComponentState::Unknown;
}

QString componentName(const QString& code) {
  static const QHash<QString, QString> names = {
      {QStringLiteral("11"), QStringLiteral("等级")},
      {QStringLiteral("31"), QStringLiteral("满金星+万变金星")},
      {QStringLiteral("34"), QStringLiteral("红色星神")},
      {QStringLiteral("41"), QStringLiteral("元魂等级")},
      {QStringLiteral("44"), QStringLiteral("专属元魂觉醒")},
      {QStringLiteral("62"), QStringLiteral("天赋")},
      {QStringLiteral("84"), QStringLiteral("天迹星轮")},
      {QStringLiteral("91"), QStringLiteral("源兽星级")},
      {QStringLiteral("92"), QStringLiteral("源兽/神源兽阶级")}};
  return names.value(code, QStringLiteral("未知培养类型 %1").arg(code));
}

}  // namespace

ShopPetEligibility analyzeShopPetEligibility(const ShopExchangeGood& good,
                                              const QJsonObject& pet,
                                              bool hasFullDetail) {
  if (!hasFullDetail)
    return {ShopPetEligibilityState::Unknown,
            QStringLiteral("缺少该实例的完整本地详情，无法安全判断")};
  if (pet.isEmpty())
    return {ShopPetEligibilityState::Unknown, QStringLiteral("本地详情为空")};

  const QStringList codes = good.enhanceType.split(QLatin1Char('-'), Qt::SkipEmptyParts);
  if (codes.isEmpty())
    return {ShopPetEligibilityState::Unknown,
            QStringLiteral("官方项目没有可识别的培养类型")};

  QStringList useful;
  QStringList usefulCodes;
  QStringList full;
  QStringList unknown;
  for (const QString& code : codes) {
    const ComponentState state = component(code.trimmed(), pet);
    const QString name = componentName(code.trimmed());
    if (state == ComponentState::Useful) {
      useful.append(name);
      usefulCodes.append(code.trimmed());
    } else if (state == ComponentState::Full)
      full.append(name);
    else
      unknown.append(name);
  }
  if (!useful.isEmpty())
    return {ShopPetEligibilityState::Usable,
            QStringLiteral("可提升：%1").arg(useful.join(QStringLiteral("、"))),
            usefulCodes};
  if (unknown.isEmpty())
    return {ShopPetEligibilityState::NotUsable,
            QStringLiteral("对应培养项已满：%1").arg(full.join(QStringLiteral("、")))};
  return {ShopPetEligibilityState::Unknown,
          QStringLiteral("无法确认：%1").arg(unknown.join(QStringLiteral("、")))};
}
