#include "pet_era.h"
#include "pet_identity.h"
#include "checked_json_numbers.h"
#include <QSet>
#include <QStringList>
#include <limits>

namespace {
PetEra namedEra(const QString& value) {
  const auto era = value.trimmed();
  if (era == QStringLiteral("灵初")) return PetEra::LingChu;
  if (era == QStringLiteral("神运")) return PetEra::ShenYun;
  if (era == QStringLiteral("星迹")) return PetEra::XingJi;
  if (era == QStringLiteral("启元")) return PetEra::QiYuan;
  if (era == QStringLiteral("其他") || era == QStringLiteral("其它")) return PetEra::Other;
  return PetEra::Unknown;
}
PetEra prefixedEra(const QString& value) {
  const auto name = value.trimmed();
  const int square = name.indexOf(QLatin1Char(']')), corner = name.indexOf(QChar(0x3011));
  if (name.startsWith(QLatin1Char('[')) && square > 1) return namedEra(name.mid(1,square-1));
  if (name.startsWith(QChar(0x3010)) && corner > 1) return namedEra(name.mid(1,corner-1));
  return PetEra::Unknown;
}
PetEra signedEra(const QString& value) {
  QSet<QString> tags;
  for (const auto& tag : value.split(QLatin1Char(','),Qt::SkipEmptyParts)) tags.insert(tag.trimmed());
  for (const auto& name : {QStringLiteral("灵初"),QStringLiteral("神运"),QStringLiteral("星迹"),QStringLiteral("启元")})
    if (tags.contains(name)) return namedEra(name);
  return PetEra::Other;
}
}

PetEra resolvePetEra(const QJsonObject& pet, const QJsonObject& pets) {
  const int race = petRaceId(pet);
  QJsonObject current = pets.value(QString::number(race)).toObject();
  QSet<int> visited;
  int id = race;
  while (!current.isEmpty() && !visited.contains(id)) {
    visited.insert(id);
    if (current.value(QStringLiteral("era")).isString()) return namedEra(current.value(QStringLiteral("era")).toString());
    if (current.value(QStringLiteral("sign")).isString()) return signedEra(current.value(QStringLiteral("sign")).toString());
    const auto officialName = current.value(QStringLiteral("name")).toString().trimmed();
    const auto era = prefixedEra(officialName);
    if (era != PetEra::Unknown) return era;
    if (officialName.startsWith(QLatin1Char('[')) || officialName.startsWith(QChar(0x3010))) return PetEra::Unknown;
    qint64 group = 0;
    if (!DomainNumeric::checkedInteger(current.value(QStringLiteral("groupRaceId")),&group,1,std::numeric_limits<int>::max()) || group == id) break;
    id = int(group); current = pets.value(QString::number(id)).toObject();
  }
  qint64 cachedRace = 0;
  const bool cacheMatches = !pet.contains(QStringLiteral("_metaRaceId")) ||
      (DomainNumeric::checkedInteger(pet.value(QStringLiteral("_metaRaceId")),&cachedRace,1,std::numeric_limits<int>::max()) && cachedRace == race);
  if (cacheMatches) {
    const auto explicitEra = pet.value(QStringLiteral("_metaEra"));
    if (explicitEra.isString() && !explicitEra.toString().trimmed().isEmpty()) return namedEra(explicitEra.toString());
    const auto original = prefixedEra(pet.value(QStringLiteral("_metaOriginalName")).toString());
    if (original != PetEra::Unknown) return original;
  }
  // Only an explicit protocol-name era prefix is useful without a dictionary;
  // nicknames, substrings and an unmarked skin name do not establish an era.
  return prefixedEra(pet.value(QStringLiteral("n")).toString());
}
