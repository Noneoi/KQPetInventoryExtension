#include "pet_detail_catalog.h"

#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>

namespace {

QList<int> idsOf(const QString& text) {
  QList<int> ids;
  for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
    ids.append(part.toInt());
  return ids;
}

bool hasAny(const QList<int>& ids, std::initializer_list<int> candidates) {
  for (int candidate : candidates) {
    if (ids.contains(candidate))
      return true;
  }
  return false;
}

QString fusionName(const QList<int>& ids) {
  if (hasAny(ids, {26, 36}) && hasAny(ids, {29, 39}))
    return QStringLiteral("召唤英雄");
  if (hasAny(ids, {19, 20}) && hasAny(ids, {26, 36}))
    return QStringLiteral("元素召唤");
  if (hasAny(ids, {19, 20}) && hasAny(ids, {29, 39}))
    return QStringLiteral("元素英雄");
  if (hasAny(ids, {19, 20}) && hasAny(ids, {28, 38}))
    return QStringLiteral("元素通灵");
  if (hasAny(ids, {17, 18}) && hasAny(ids, {28, 38}))
    return QStringLiteral("赋能通灵");
  if (hasAny(ids, {17, 18}) && hasAny(ids, {26, 36}))
    return QStringLiteral("赋能召唤");
  if (hasAny(ids, {42, 43}) && hasAny(ids, {28, 38}))
    return QStringLiteral("幻元通灵");
  return {};
}

}  // namespace

const PetDetailCatalog& PetDetailCatalog::instance() {
  static const PetDetailCatalog catalog;
  return catalog;
}

PetDetailCatalog::PetDetailCatalog() {
  QFile file(QStringLiteral(":/kqpet/pet-detail-data.json"));
  if (!file.open(QIODevice::ReadOnly))
    return;
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return;
  root_ = document.object();
  loaded_ = !root_.value(QStringLiteral("pets")).toObject().isEmpty();
}

QJsonObject PetDetailCatalog::item(const char* section, int defineId) const {
  return root_.value(QString::fromLatin1(section))
      .toObject()
      .value(QString::number(defineId))
      .toObject();
}

QJsonObject PetDetailCatalog::pet(int raceId) const { return item("pets", raceId); }

QString PetDetailCatalog::petName(int raceId) const {
  return pet(raceId).value(QStringLiteral("name")).toString();
}

QString PetDetailCatalog::originalName(int raceId) const {
  QSet<int> visited;
  QJsonObject current = pet(raceId);
  QString name = current.value(QStringLiteral("name")).toString();
  int groupRaceId = current.value(QStringLiteral("groupRaceId")).toInt();
  while (groupRaceId > 0 && !visited.contains(groupRaceId)) {
    visited.insert(groupRaceId);
    const QJsonObject base = pet(groupRaceId);
    if (base.isEmpty())
      break;
    name = base.value(QStringLiteral("name")).toString(name);
    groupRaceId = base.value(QStringLiteral("groupRaceId")).toInt();
  }
  return name;
}

QString PetDetailCatalog::attributes(const QString& sequence) const {
  const QJsonObject names = root_.value(QStringLiteral("attributes")).toObject();
  QStringList result;
  for (const QString& part : sequence.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString id = part.trimmed();
    result.append(names.value(id).toString(QStringLiteral("属性 %1").arg(id)));
  }
  result.removeDuplicates();
  return result.isEmpty() ? QStringLiteral("—") : result.join(QStringLiteral(" + "));
}

QString PetDetailCatalog::jobs(const QString& sequence) const {
  const QJsonObject names = root_.value(QStringLiteral("jobs")).toObject();
  QStringList groups;
  for (const QString& group : sequence.split(QLatin1Char('-'), Qt::SkipEmptyParts)) {
    const QList<int> ids = idsOf(group);
    QString name = fusionName(ids);
    if (name.isEmpty()) {
      QStringList parts;
      for (int id : ids)
        parts.append(names.value(QString::number(id)).toString(QStringLiteral("职业 %1").arg(id)));
      parts.removeDuplicates();
      name = parts.join(QStringLiteral(" + "));
    }
    if (!name.isEmpty())
      groups.append(name);
  }
  return groups.isEmpty() ? QStringLiteral("—") : groups.join(QStringLiteral(" / "));
}

QString PetDetailCatalog::mappedName(const char* section, int defineId,
                                     const QString& fallbackPrefix) const {
  const QString name = item(section, defineId).value(QStringLiteral("name")).toString();
  return name.isEmpty() ? QStringLiteral("%1 %2").arg(fallbackPrefix).arg(defineId) : name;
}

QString PetDetailCatalog::badgeName(int defineId) const {
  return mappedName("badges", defineId, QStringLiteral("元魂"));
}

QString PetDetailCatalog::sacredEquipmentName(int defineId) const {
  return mappedName("sacredEquipment", defineId, QStringLiteral("神源兽"));
}

QString PetDetailCatalog::astrolabeName(int defineId) const {
  return mappedName("astrolabe", defineId, QStringLiteral("星灵"));
}

QJsonObject PetDetailCatalog::astrolabe(int defineId) const {
  return item("astrolabe", defineId);
}

QJsonObject PetDetailCatalog::stargod(int defineId) const { return item("stargods", defineId); }

int PetDetailCatalog::sacredMaxStar(int planId) {
  switch (planId) {
    case 1:
      return 8;
    case 2:
      return 10;
    case 3:
      return 9;
    default:
      return 0;
  }
}

int PetDetailCatalog::sacredMaxStage(int planId) {
  if (planId < 1 || planId > 24)
    return 0;
  return 5 + ((planId - 1) % 3);
}
