#include "pet_power_calculator.h"
#include "pet_detail_analyzer.h"

#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "inventory_read_view.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace {

QStringList splitSequence(const QString& value, QChar separator) {
  return value.split(separator, Qt::SkipEmptyParts);
}

int stargodId(const QJsonValue& value) {
  if (value.isDouble()) return value.toInt();
  if (value.isString()) return value.toString().toInt();
  return 0;
}

QString stargodCategory(const PetStargodEntry& entry) {
  static const QStringList functionalNames = {
      QStringLiteral("如有神助"), QStringLiteral("顺应天命"),
      QStringLiteral("天煞孤星"), QStringLiteral("气贯星河")};
  static const QStringList attackNames = {QStringLiteral("乘胜追击")};
  static const QStringList defenseNames = {QStringLiteral("福虎佑灵")};
  for (const QString& name : functionalNames)
    if (entry.name.contains(name)) return QStringLiteral("功能性");
  for (const QString& name : attackNames)
    if (entry.name.contains(name)) return QStringLiteral("进攻");
  for (const QString& name : defenseNames)
    if (entry.name.contains(name)) return QStringLiteral("防御");
  static const QSet<int> defenseIds = {
      10, 15, 16, 17, 18, 19, 21, 26, 34, 35, 37, 38, 39, 40,
      46, 47, 48, 49, 54, 55, 57, 60, 63, 64, 70, 72, 73, 74,
      76, 85, 86};
  static const QSet<int> functionalIds = {11, 20, 22, 29, 33, 45, 67, 68, 81, 84, 88};
  if (defenseIds.contains(entry.defineId)) return QStringLiteral("防御");
  if (functionalIds.contains(entry.defineId)) return QStringLiteral("功能性");
  if (entry.name.contains(QStringLiteral("盾")) ||
      entry.name.contains(QStringLiteral("防")) ||
      entry.name.contains(QStringLiteral("护")) ||
      entry.name.contains(QStringLiteral("躯")) ||
      entry.name.contains(QStringLiteral("霸体")) ||
      entry.name.contains(QStringLiteral("闪")))
    return QStringLiteral("防御");
  if (entry.name.contains(QStringLiteral("人品")) ||
      entry.name.contains(QStringLiteral("命中")) ||
      entry.name.contains(QStringLiteral("幸运")) ||
      entry.name.contains(QStringLiteral("追击")) ||
      entry.name.contains(QStringLiteral("连打")))
    return QStringLiteral("功能性");
  return QStringLiteral("进攻");
}

qint64 relationId(const QJsonValue& value) {
  if (value.isDouble()) return static_cast<qint64>(value.toDouble());
  if (value.isString()) return value.toString().toLongLong();
  if (value.isObject()) return petInstanceId(value.toObject());
  return 0;
}

PetRelatedDisplay relatedDisplay(const QJsonObject& embedded, qint64 fallbackId,
                                 int fallbackRaceId,
                                 const InventoryReadView* repository) {
  QJsonObject related = embedded;
  qint64 id = petInstanceId(related);
  if (id <= 0) id = fallbackId;
  if (id > 0 && repository) {
    QJsonObject merged = repository->detailFor(id);
    for (auto iterator = related.begin(); iterator != related.end(); ++iterator)
      merged.insert(iterator.key(), iterator.value());
    related = merged;
  }

  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  int raceId = petRaceId(related);
  if (raceId <= 0) raceId = fallbackRaceId;
  PetRelatedDisplay result;
  result.name = related.value(QStringLiteral("customName")).toString();
  if (result.name.isEmpty()) result.name = related.value(QStringLiteral("n")).toString();
  if (result.name.isEmpty() && raceId > 0) result.name = catalog.petName(raceId);
  if (result.name.isEmpty()) result.name = QStringLiteral("未知精灵");

  QJsonObject eraPet = related;
  if (petRaceId(eraPet) <= 0 && raceId > 0)
    eraPet.insert(QStringLiteral("ri"), raceId);
  result.facts.append(catalog.resolvedEra(eraPet));
  if (related.contains(QStringLiteral("lv")))
    result.facts.append(QStringLiteral("LV.%1").arg(related.value(QStringLiteral("lv")).toInt()));
  const QString currentPower = related.contains(QStringLiteral("zdl"))
                                   ? QString::number(related.value(QStringLiteral("zdl")).toInt())
                                   : QStringLiteral("—");
  const QString extremePower = related.contains(QStringLiteral("xzdl"))
                                   ? QString::number(related.value(QStringLiteral("xzdl")).toInt())
                                   : QStringLiteral("—");
  result.facts.append(QStringLiteral("%1 / %2").arg(currentPower, extremePower));
  return result;
}

QList<PetRelatedDisplay> relatedList(const QJsonArray& values,
                                     const InventoryReadView* repository) {
  QList<PetRelatedDisplay> result;
  QSet<qint64> seenIds;
  for (const QJsonValue& value : values) {
    const QJsonObject embedded = value.toObject();
    const qint64 id = relationId(value);
    if (id > 0 && seenIds.contains(id)) continue;
    if (id > 0) seenIds.insert(id);
    if (embedded.isEmpty() && id <= 0) continue;
    result.append(relatedDisplay(embedded, id, petRaceId(embedded), repository));
  }
  return result;
}

}  // namespace

PetBattlePowerState PetDetailAnalyzer::analyzeBattlePower(const QJsonObject& pet) {
  const auto& catalog = PetDetailCatalog::instance();
  return calculatePetBattlePower(pet, petPowerMetadataFromCatalog(pet,
      catalog.metadataFor(pet).value(QStringLiteral("stargodSlotMaxLevel")).toInt(),
      catalog.stargodDefinitions(), catalog.astrolabeDefinitions(), catalog.petDefinitions()));
}
PetDetailViewModel PetDetailAnalyzer::analyze(const QJsonObject& pet,
                                              const InventoryReadView* repository,
                                              const QString& imagePath,
                                              bool fetchingLatest) {
  PetDetailViewModel result;
  result.imagePath = imagePath;
  result.fetchingLatest = fetchingLatest;
  if (pet.isEmpty()) return result;

  result.available = true;
  result.visualMismatch = pet.value(QStringLiteral("_visualMismatch")).toBool();
  result.level = pet.value(QStringLiteral("lv")).toInt();
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  result.raceId = petRaceId(pet);
  result.instanceId = petInstanceId(pet);
  result.name = pet.value(QStringLiteral("n")).toString();
  if (result.name.isEmpty()) result.name = catalog.petName(result.raceId);
  result.originalName = catalog.resolvedOriginalName(pet);
  result.customName = pet.value(QStringLiteral("customName")).toString();
  result.attributes = catalog.resolvedAttributes(pet);
  result.jobs = catalog.resolvedJobs(pet);
  result.era = catalog.resolvedEra(pet);
  result.battlePower = analyzeBattlePower(pet);

  static const QStringList propertyNames = {
      QStringLiteral("生命"),   QStringLiteral("物攻"),   QStringLiteral("物防"),
      QStringLiteral("魔攻"),   QStringLiteral("魔防"),   QStringLiteral("超攻"),
      QStringLiteral("超防"),   QStringLiteral("速度"),   QStringLiteral("超物攻"),
      QStringLiteral("超物防"), QStringLiteral("超魔攻"), QStringLiteral("超魔防")};
  static const QStringList talentLevelNames = {
      QStringLiteral("一无是处"), QStringLiteral("十分常见"), QStringLiteral("百里挑一"),
      QStringLiteral("千载难逢"), QStringLiteral("万众瞩目"), QStringLiteral("王者无敌"),
      QStringLiteral("超凡入圣")};
  static const QStringList energyNames = {
      QString(), QStringLiteral("无星能"), QStringLiteral("单星能"), QStringLiteral("双星能")};
  result.talent.level = pet.value(QStringLiteral("gt")).toInt();
  result.talent.levelName = result.talent.level >= 0 &&
                                    result.talent.level < talentLevelNames.size()
                                ? talentLevelNames.at(result.talent.level)
                                : QStringLiteral("等级 %1").arg(result.talent.level);
  const QStringList talentValues =
      splitSequence(pet.value(QStringLiteral("ip")).toString(), QLatin1Char('#'));
  const QStringList energies =
      splitSequence(pet.value(QStringLiteral("gps")).toString(), QLatin1Char('#'));
  for (int index = 0; index < talentValues.size() && index < propertyNames.size(); ++index) {
    const int value = talentValues.at(index).toInt();
    if (value <= 0) continue;
    const int energy = index < energies.size() ? energies.at(index).toInt() : 0;
    const QString suffix = energy >= 0 && energy < energyNames.size() &&
                                   !energyNames.at(energy).isEmpty()
                               ? QStringLiteral(" · %1").arg(energyNames.at(energy))
                               : QString();
    const QString line = QStringLiteral("%1 %2%3")
                             .arg(propertyNames.at(index)).arg(value).arg(suffix);
    (energy == 3 ? result.talent.doubleEnergyLines : result.talent.normalLines)
        .append(line);
  }
  const QJsonObject currentParts = pet.value(QStringLiteral("czdlv")).toObject();
  const QJsonObject fullParts = pet.value(QStringLiteral("mzdlv")).toObject();
  result.talent.currentPower = currentParts.contains(QStringLiteral("iv"))
                                   ? QString::number(currentParts.value(QStringLiteral("iv")).toInt())
                                   : QStringLiteral("—");
  result.talent.fullPower = fullParts.contains(QStringLiteral("iv"))
                                ? QString::number(fullParts.value(QStringLiteral("iv")).toInt())
                                : QStringLiteral("—");

  for (const QString& slot : splitSequence(pet.value(QStringLiteral("badge")).toString(),
                                           QLatin1Char('|'))) {
    const QStringList parts = splitSequence(slot, QLatin1Char('#'));
    if (parts.isEmpty()) continue;
    const QStringList job = parts.at(0).split(QLatin1Char(':'));
    PetBadgeSlot badge;
    badge.jobName = catalog.badgeName(job.value(0).toInt());
    badge.level = job.value(1).toInt();
    if (parts.size() > 1) {
      const QStringList exclusive = parts.at(1).split(QLatin1Char(':'));
      const int exclusiveId = exclusive.value(0).toInt();
      if (exclusiveId > 0) {
        badge.exclusiveName = catalog.badgeName(exclusiveId);
        badge.exclusiveAwakened = exclusive.value(1).toInt() > 0;
      }
    }
    result.badges.append(badge);
  }

  const QString sacredSequence = pet.value(QStringLiteral("shenjue")).toString();
  if (!sacredSequence.isEmpty()) {
    const QStringList parts = sacredSequence.split(QLatin1Char('|'));
    const QStringList define = parts.value(0).split(QLatin1Char('#'));
    const QStringList levels = parts.value(1).split(QLatin1Char(':'));
    result.sacred.equipped = true;
    result.sacred.name = catalog.sacredEquipmentName(define.value(0).toInt());
    const PetMetadataView sacredMetadata(catalog.snapshot());
    result.sacred.maxStar = sacredMetadata.sacredMaxStar(define.value(1).toInt());
    result.sacred.maxStage = sacredMetadata.sacredMaxStage(define.value(2).toInt());
    result.sacred.star = levels.value(0).toInt();
    result.sacred.stage = levels.value(1).toInt();
    result.sacred.fullStar = result.sacred.maxStar > 0 &&
                             result.sacred.star == result.sacred.maxStar;
    result.sacred.fullStage = result.sacred.maxStage > 0 &&
                              result.sacred.stage == result.sacred.maxStage;
  }

  for (const QString& chain : splitSequence(pet.value(QStringLiteral("astrolabe")).toString(),
                                            QLatin1Char('|'))) {
    for (const QString& slot : splitSequence(chain, QLatin1Char('#'))) {
      const QStringList fields = slot.split(QLatin1Char(':'));
      const int defineId = fields.value(0).toInt();
      if (fields.size() < 3 || defineId <= 0) continue;
      PetAstrolabeStar star;
      star.name = catalog.astrolabeName(defineId);
      const QJsonObject definition = catalog.astrolabe(defineId);
      star.exclusive = definition.value(QStringLiteral("exclusive")).toBool();
      if (star.exclusive) {
        for (const QString& material : splitSequence(
                 definition.value(QStringLiteral("lightUpCost")).toString(),
                 QLatin1Char('#'))) {
          const QStringList cost = material.split(QLatin1Char(':'));
          if (cost.size() < 3 || cost.value(2).toInt() <= 0) continue;
          star.lightUpCosts.append(catalog.materialCostText(
              cost.value(0).toInt(), cost.value(1).toInt(), cost.value(2).toInt()));
        }
      }
      star.activated = fields.at(1).toInt() > 0;
      if (star.activated) ++result.astrolabe.activatedCount;
      star.selected = fields.at(2).toInt() > 0;
      if (star.selected) ++result.astrolabe.selectedCount;
      result.astrolabe.stars.append(star);
    }
  }
  result.astrolabe.breakthrough = pet.value(QStringLiteral("astrolabebr")).toBool();

  for (const QString& slot : splitSequence(pet.value(QStringLiteral("sgs")).toString(),
                                           QLatin1Char('#'))) {
    const QStringList fields = slot.split(QLatin1Char(':'));
    const int defineId = fields.value(0).toInt();
    if (defineId <= 0) continue;
    const int sourceId = fields.size() >= 3 ? fields.value(2).toInt() : -1;
    const QJsonObject item = catalog.stargod(defineId);
    const QJsonObject source = sourceId > 0 ? catalog.stargod(sourceId) : QJsonObject();
    PetStargodEntry entry;
    entry.defineId = defineId;
    entry.level = fields.value(1).toInt();
    entry.name = item.value(QStringLiteral("name"))
                     .toString(QStringLiteral("星神 %1").arg(defineId));
    entry.sourceName = source.value(QStringLiteral("name"))
                           .toString(QStringLiteral("万变星神"));
    entry.changeable = item.value(QStringLiteral("changeable")).toBool() ||
                       source.value(QStringLiteral("changeable")).toBool();
    entry.quality = item.value(QStringLiteral("quality")).toInt();
    if (entry.changeable && source.contains(QStringLiteral("quality")))
      entry.quality = source.value(QStringLiteral("quality")).toInt(entry.quality);
    entry.category = stargodCategory(entry);
    result.stargods.append(entry);
  }

  for (const QJsonValue& value : pet.value(QStringLiteral("sgsp")).toArray()) {
    const int defineId = stargodId(value);
    if (defineId <= 0) continue;
    const QJsonObject item = catalog.stargod(defineId);
    PetStargodBackpackEntry entry;
    entry.defineId = defineId;
    entry.name = item.value(QStringLiteral("name"))
                     .toString(QStringLiteral("星神 %1").arg(defineId));
    entry.quality = item.value(QStringLiteral("quality")).toInt();
    entry.changeable = item.value(QStringLiteral("changeable")).toBool();
    result.stargodBackpack.append(entry);
  }

  const QJsonObject summoned = pet.value(QStringLiteral("sppl")).toObject();
  qint64 summonedId = relationId(pet.value(QStringLiteral("sepi")));
  if (summonedId <= 0) summonedId = relationId(pet.value(QStringLiteral("sdpi")));
  if (summonedId <= 0) summonedId = petInstanceId(summoned);
  if (summonedId > 0 || !summoned.isEmpty())
    result.relationships.summonRows.append(
        {QStringLiteral("被召唤精灵"),
         {relatedDisplay(summoned, summonedId, petRaceId(summoned), repository)}});
  const QList<PetRelatedDisplay> summons =
      relatedList(pet.value(QStringLiteral("asps")).toArray(), repository);
  if (!summons.isEmpty())
    result.relationships.summonRows.append({QStringLiteral("召唤关联列表"), summons});
  const qint64 summonerId = relationId(pet.value(QStringLiteral("srpi")));
  const int summonerRaceId = pet.value(QStringLiteral("srri")).toInt();
  if (summonerId > 0 || summonerRaceId > 0)
    result.relationships.summonRows.append(
        {QStringLiteral("召唤者"),
         {relatedDisplay({}, summonerId, summonerRaceId, repository)}});

  const QJsonObject carried = pet.value(QStringLiteral("cppl")).toObject();
  qint64 carriedId = relationId(pet.value(QStringLiteral("cepi")));
  if (carriedId <= 0) carriedId = petInstanceId(carried);
  if (carriedId > 0 || !carried.isEmpty())
    result.relationships.carryRows.append(
        {QStringLiteral("被携带精灵"),
         {relatedDisplay(carried, carriedId, petRaceId(carried), repository)}});
  const QList<PetRelatedDisplay> carryList =
      relatedList(pet.value(QStringLiteral("acps")).toArray(), repository);
  if (!carryList.isEmpty())
    result.relationships.carryRows.append({QStringLiteral("携带 / 神使列表"), carryList});
  const QList<PetRelatedDisplay> carriers =
      relatedList(pet.value(QStringLiteral("crpis")).toArray(), repository);
  if (!carriers.isEmpty())
    result.relationships.carryRows.append({QStringLiteral("携带者 / 神使"), carriers});
  return result;
}
