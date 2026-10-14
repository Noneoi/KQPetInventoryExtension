#include "pet_relation_rules.h"

#include <QJsonArray>
#include <QMap>
#include <QPair>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
bool integer(const QJsonValue& value, int* output, int minimum) {
  if (value.isString()) {
    const auto text = value.toString();
    if (text.isEmpty()) return false;
    int begin = text.startsWith(QLatin1Char('-')) && minimum < 0 ? 1 : 0;
    if (begin == text.size()) return false;
    for (int i = begin; i < text.size(); ++i)
      if (text[i] < QLatin1Char('0') || text[i] > QLatin1Char('9')) return false;
    bool ok = false; const auto number = text.toLongLong(&ok);
    if (!ok || number < minimum || number > std::numeric_limits<int>::max()) return false;
    *output = int(number); return true;
  }
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number || number < minimum ||
      number > std::numeric_limits<int>::max()) return false;
  *output = int(number); return true;
}

bool containsRace(const QJsonValue& value, int race) {
  if (!value.isArray()) return false;
  for (const auto& candidate : value.toArray()) {
    int id = 0;
    if (integer(candidate, &id, 1) && id == race) return true;
  }
  return false;
}

QPair<int, QString> original(const QJsonObject& pets, int target) {
  const auto identity = pets.value(QString::number(target)).toObject().value(QStringLiteral("identity")).toObject();
  const auto status = identity.value(QStringLiteral("status")).toString();
  int base = 0;
  const auto name = identity.value(QStringLiteral("canonicalName")).toString();
  if ((status != QStringLiteral("native") && status != QStringLiteral("skin")) || name.isEmpty() ||
      !integer(identity.value(QStringLiteral("baseRaceId")), &base, 1) ||
      (status == QStringLiteral("native") && base != target)) return {target, {}};
  // Do not let inconsistent or incomplete identity records merge two targets.
  // A skin mapping must resolve to the same verified native dictionary entry.
  const auto baseIdentity = pets.value(QString::number(base)).toObject().value(QStringLiteral("identity")).toObject();
  int verifiedBase = 0;
  if (baseIdentity.value(QStringLiteral("status")).toString() != QStringLiteral("native") ||
      !integer(baseIdentity.value(QStringLiteral("baseRaceId")), &verifiedBase, 1) || verifiedBase != base ||
      baseIdentity.value(QStringLiteral("canonicalName")).toString() != name) return {target, {}};
  return {base, name};
}
} // namespace

PetRelationOptions petRelationOptions(const QJsonObject& replayRoot, int raceId, PetRelationKind kind) {
  if (raceId <= 0 || (kind != PetRelationKind::Summon && kind != PetRelationKind::Carry)) return {};
  const auto supplements = replayRoot.value(QStringLiteral("replaySupplements"));
  const auto petValue = replayRoot.value(QStringLiteral("pets"));
  if (!supplements.isObject() || !petValue.isObject()) return {};
  const auto groupValue = supplements.toObject().value(kind == PetRelationKind::Summon ? QStringLiteral("summon") : QStringLiteral("carry"));
  if (!groupValue.isObject()) return {};
  const auto group = groupValue.toObject();
  if (!group.value(QStringLiteral("rows")).isArray() || !group.value(QStringLiteral("byPair")).isObject()) return {};
  const auto rows = group.value(QStringLiteral("rows")).toArray();
  const auto pairs = group.value(QStringLiteral("byPair")).toObject();
  const auto pets = petValue.toObject();
  const auto mechanics = replayRoot.value(QStringLiteral("petMechanics")).toObject();
  const QString prefix = QString::number(raceId) + QLatin1Char('_');
  QMap<QPair<int, int>, PetRelationFamily> families;
  for (auto pair = pairs.constBegin(); pair != pairs.constEnd(); ++pair) {
    if (!pair.key().startsWith(prefix)) continue;
    int target = 0, rowIndex = -1, recordedRow = -1;
    const auto targetText = pair.key().mid(prefix.size());
    if (!integer(targetText, &target, 1) || targetText != QString::number(target) ||
        !integer(pair.value(), &rowIndex, 0) || rowIndex >= rows.size() || !rows[rowIndex].isObject()) return {};
    const auto row = rows[rowIndex].toObject();
    if (!integer(row.value(QStringLiteral("rowId")), &recordedRow, 0) || recordedRow != rowIndex ||
        !containsRace(row.value(QStringLiteral("sourceRaceIds")), raceId) ||
        !containsRace(row.value(QStringLiteral("targetRaceIds")), target)) return {};
    int type = 0;
    if (kind == PetRelationKind::Carry &&
        (row.value(QStringLiteral("expansionStatus")).toString() != QStringLiteral("resolved") ||
         !integer(row.value(QStringLiteral("type")), &type, std::numeric_limits<int>::min()))) return {};
    // Use the final byPair entry, not every row in bySourceRaceId. The official
    // XML loader replaces duplicate pairs with the later definition.
    // PetDataService.markSummonPet/markCarryPet match these definitions against
    // the owned pet's raceId without isShow/hide filtering. Those flags govern
    // other official selection screens, not this complete eligibility query.
    const auto identity = original(pets, target);
    const auto key = qMakePair(identity.first, type);
    auto& family = families[key];
    if (family.raceIds.isEmpty()) {
      family.originalRaceId = identity.first; family.originalName = identity.second; family.carryType = type;
      family.listedWithoutOwnership = false;
    }
    if (family.originalName != identity.second) return {};
    family.raceIds.append(target);
    // PetDictionaryDataItem.isSkin() is findSign("皮肤"). The verified raw
    // dictionary sign is retained in petMechanics; identity grouping alone is
    // not a substitute for this different consumer predicate. Missing evidence
    // must not manufacture a selectable non-owned family.
    const auto signs = mechanics.value(QString::number(target)).toObject().value(QStringLiteral("sign"));
    const bool nonSkin = signs.isString() && !signs.toString().split(QLatin1Char(',')).contains(QStringLiteral("皮肤"));
    const bool listed = nonSkin && (kind == PetRelationKind::Carry || row.value(QStringLiteral("isShow")) == QJsonValue(true));
    family.listedWithoutOwnership = family.listedWithoutOwnership || listed;
  }
  PetRelationOptions result; result.available = true;
  for (auto& family : families) {
    std::sort(family.raceIds.begin(), family.raceIds.end());
    result.families.append(std::move(family));
  }
  return result;
}
