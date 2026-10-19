#include "pet_relation_options.h"
#include "checked_json_numbers.h"
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <climits>

PetRelationCandidateOptions petRelationCandidateOptions(const QJsonObject& root, int sourceRaceId, bool carry) {
  if (sourceRaceId <= 0) return {};
  const auto rules = root.value(QStringLiteral("relationRules")).toObject();
  if (rules.value(QStringLiteral("schema")) != QJsonValue(1)) return {};
  const auto section = rules.value(carry ? QStringLiteral("carry") : QStringLiteral("summon")).toObject();
  if (section.value(QStringLiteral("available")) != QJsonValue(true) ||
      !section.value(QStringLiteral("bySourceRaceId")).isObject()) return {};
  const auto sources = section.value(QStringLiteral("bySourceRaceId")).toObject();
  const auto value = sources.value(QString::number(sourceRaceId));
  if (value.isUndefined()) return {true,{}};
  if (!value.isArray() || value.toArray().size() > 16384) return {};
  QMap<QPair<int,int>, PetRelationCandidateFamily> families;
  QSet<int> targets;
  for (const auto& item : value.toArray()) {
    if (!item.isObject()) return {};
    const auto row = item.toObject();
    qint64 target = 0, base = 0, type = 0;
    if (!DomainNumeric::checkedInteger(row.value(QStringLiteral("targetRaceId")),&target,1,INT_MAX) ||
        !DomainNumeric::checkedInteger(row.value(QStringLiteral("baseRaceId")),&base,1,INT_MAX) ||
        !DomainNumeric::checkedInteger(row.value(QStringLiteral("type")),&type,0,INT_MAX) ||
        !row.value(QStringLiteral("originalName")).isString() ||
        !row.value(QStringLiteral("isShow")).isBool() ||
        !row.value(QStringLiteral("listedWithoutOwnership")).isBool() || targets.contains(int(target))) return {};
    const QString name = row.value(QStringLiteral("originalName")).toString();
    if (name.size() > 1024) return {};
    targets.insert(int(target));
    auto& family = families[qMakePair(int(base),int(type))];
    if (!family.targetRaceIds.isEmpty() && family.originalName != name) return {};
    family.baseRaceId = int(base); family.targetType = int(type); family.originalName = name;
    family.targetRaceIds.append(int(target));
    family.listedWithoutOwnership |= row.value(QStringLiteral("listedWithoutOwnership")).toBool();
  }
  PetRelationCandidateOptions result; result.available = true;
  for (auto& family : families) {
    std::sort(family.targetRaceIds.begin(),family.targetRaceIds.end());
    result.families.append(std::move(family));
  }
  return result;
}
