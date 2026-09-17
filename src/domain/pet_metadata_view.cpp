#include "pet_metadata_view.h"
#include "pet_identity.h"
#include "checked_json_numbers.h"
#include "pet_era.h"
#include <QRegularExpression>
#include <QSet>
#include <QJsonArray>
#include <QStringList>

namespace {

QList<int> idsOf(const QString& text) {
  QList<int> ids;
  for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    bool valid = false; const int id = part.toInt(&valid);
    if (valid && id > 0) ids.append(id);
  }
  return ids;
}

QString fusionName(const QList<int>& ids, const QJsonArray& definitions) {
  for (const auto& value : definitions) {
    const auto definition = value.toObject();
    const auto groups = definition.value(QStringLiteral("jobs")).toArray();
    if (groups.isEmpty() || groups.size() != ids.size()) continue;
    bool matched = true;
    for (const auto& group : groups) {
      bool present = false;
      for (const auto& candidate : group.toArray()) present |= ids.contains(candidate.toInt(-1));
      matched &= present;
    }
    if (matched) return definition.value(QStringLiteral("name")).toString();
  }
  return {};
}

}  // namespace

QString PetMetadataView::eraPrefix(const QString& name) {
  static const QRegularExpression pattern(QStringLiteral("^[\\[【]([^\\]】]+)[\\]】]"));
  const QRegularExpressionMatch match = pattern.match(name.trimmed());
  return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString PetMetadataView::coreName(const QString& name) {
  QString trimmed = name.trimmed();
  static const QRegularExpression era(QStringLiteral("^[\\[【][^\\]】]+[\\]】]"));
  trimmed.remove(era);
  trimmed = trimmed.trimmed();
  const int separator = trimmed.lastIndexOf(QChar(0x00B7));
  if (separator >= 0)
    return trimmed.mid(separator + 1).trimmed();
  return trimmed;
}

int PetMetadataView::liveRaceId(const QJsonObject& pet) {
  return petRaceId(pet);
}

void PetMetadataView::indexPets(PetDetailCatalogSnapshot* value) {
  value->coreToRace.clear();
  value->eraCoreToRace.clear();
  const QJsonObject pets = value->root.value(QStringLiteral("pets")).toObject();
  auto better = [](int candidate, int current) {
    if (current <= 0) return true;
    const bool candidateNormal = candidate > 0 && candidate < 20000;
    const bool currentNormal = current > 0 && current < 20000;
    if (candidateNormal != currentNormal) return candidateNormal;
    return candidate > current;
  };
  for (auto iterator = pets.begin(); iterator != pets.end(); ++iterator) {
    const int raceId = iterator.key().toInt();
    if (raceId <= 0 || !iterator.value().isObject()) continue;
    const QString name = iterator.value().toObject().value(QStringLiteral("name")).toString();
    const QString core = coreName(name);
    const QString era = eraPrefix(name);
    if (core.isEmpty()) continue;
    if (better(raceId, value->coreToRace.value(core)))
      value->coreToRace.insert(core, raceId);
    if (!era.isEmpty()) {
      const QString eraKey = era + QLatin1Char('/') + core;
      if (better(raceId, value->eraCoreToRace.value(eraKey)))
        value->eraCoreToRace.insert(eraKey, raceId);
    }
  }
}

QJsonObject PetMetadataView::familyMetadata(const QString& liveName) const {
  return familyMetadata(*snapshot(), liveName);
}

QJsonObject PetMetadataView::familyMetadata(const PetDetailCatalogSnapshot& current, const QString& liveName) {
  const auto pet = [&current](int race) { return current.root.value(QStringLiteral("pets")).toObject().value(QString::number(race)).toObject(); };
  const QString core = coreName(liveName);
  if (core.isEmpty()) return {};
  const QString era = eraPrefix(liveName);
  if (!era.isEmpty()) {
    const int eraRace = current.eraCoreToRace.value(era + QLatin1Char('/') + core);
    const QJsonObject candidate = pet(eraRace);
    if (candidate.value(QStringLiteral("name")).toString().trimmed() == liveName.trimmed()) return candidate;
    return {};
  }
  const int familyRace = current.coreToRace.value(core);
  const QJsonObject candidate = pet(familyRace);
  return candidate.value(QStringLiteral("name")).toString().trimmed() == liveName.trimmed() ? candidate : QJsonObject{};
}

QJsonObject PetMetadataView::metadataFor(const QJsonObject& petObject) const {
  const auto frozen = snapshot();
  const auto pet = [&frozen](int id) { return frozen->root.value(QStringLiteral("pets")).toObject().value(QString::number(id)).toObject(); };
  const auto familyMetadata = [&frozen](const QString& name) { return PetMetadataView::familyMetadata(*frozen, name); };

  const QJsonObject direct = pet(liveRaceId(petObject));
  if (!direct.isEmpty()) return direct;
  const QString snapshotAttributes =
      petObject.value(QStringLiteral("_metaAttributes")).toString();
  const QString snapshotJobs = petObject.value(QStringLiteral("_metaJobs")).toString();
  const QString snapshotName =
      petObject.value(QStringLiteral("_metaOriginalName")).toString();
  if (!snapshotName.isEmpty() || !snapshotAttributes.isEmpty() || !snapshotJobs.isEmpty()) {
    return {{QStringLiteral("name"), snapshotName},
            {QStringLiteral("attributes"), snapshotAttributes},
            {QStringLiteral("jobs"), snapshotJobs}};
  }
  return familyMetadata(petObject.value(QStringLiteral("n")).toString());
}

QString PetMetadataView::resolvedOriginalName(const QJsonObject& petObject) const {
  const auto frozen = snapshot();
  const auto pet = [&frozen](int id) { return frozen->root.value(QStringLiteral("pets")).toObject().value(QString::number(id)).toObject(); };
  const auto originalName = [&frozen](int id) { return PetMetadataView::originalName(*frozen, id); };
  const auto petName = [&pet](int id) { return pet(id).value(QStringLiteral("name")).toString(); };

  const int raceId = liveRaceId(petObject);
  if (!pet(raceId).isEmpty()) {
    const QString original = originalName(raceId);
    if (!original.isEmpty()) return original;
  }
  const QString snapshot =
      petObject.value(QStringLiteral("_metaOriginalName")).toString().trimmed();
  if (!snapshot.isEmpty()) return snapshot;
  const QString live = petObject.value(QStringLiteral("n")).toString().trimmed();
  return live.isEmpty() ? petName(raceId) : live;
}

QString PetMetadataView::resolvedAttributes(const QJsonObject& petObject) const {
  return attributes(metadataFor(petObject).value(QStringLiteral("attributes")).toString());
}

QString PetMetadataView::resolvedJobs(const QJsonObject& petObject) const {
  const QJsonObject direct = pet(liveRaceId(petObject));
  if (!direct.isEmpty())
    return jobs(direct.value(QStringLiteral("jobs")).toString());
  const QString snapshot = petObject.value(QStringLiteral("_metaJobs")).toString();
  if (!snapshot.isEmpty()) return jobs(snapshot);
  // The official client resolves jobs from its dictionary. The compressed rt
  // field is not that dictionary sequence (7529 rt=26, official job=32).
  return jobs(familyMetadata(petObject.value(QStringLiteral("n")).toString())
                  .value(QStringLiteral("jobs"))
                  .toString());
}

QString PetMetadataView::resolvedEra(const QJsonObject& petObject) const {
  const QString resolved = petEraDisplayName(resolvePetEra(petObject,petDefinitions()));
  if (!resolved.isEmpty()) return resolved;
  const QString snapshot = petObject.value(QStringLiteral("_metaEra")).toString().trimmed();
  if (!snapshot.isEmpty()) return snapshot;
  const QStringList names = {resolvedOriginalName(petObject),
                             petName(liveRaceId(petObject)),
                             petObject.value(QStringLiteral("n")).toString()};
  for (const QString& name : names) {
    const QString era = eraPrefix(name);
    if (!era.isEmpty()) return era;
  }
  return QStringLiteral("其它");
}

QJsonObject PetMetadataView::enrichMetadata(const QJsonObject& petObject,
                                             const QJsonObject& previous) const {
  const auto frozen = snapshot();
  const auto pet = [&frozen](int id) { return frozen->root.value(QStringLiteral("pets")).toObject().value(QString::number(id)).toObject(); };
  const auto familyMetadata = [&frozen](const QString& name) { return PetMetadataView::familyMetadata(*frozen, name); };
  const auto originalName = [&frozen](int id) { return PetMetadataView::originalName(*frozen, id); };

  QJsonObject result = petObject;
  QJsonObject metadata = pet(liveRaceId(petObject));
  int metadataRace = liveRaceId(petObject);
  if (metadata.isEmpty()) {
    const QStringList snapshotKeys = {
        QStringLiteral("_metaOriginalName"), QStringLiteral("_metaAttributes"),
        QStringLiteral("_metaJobs"), QStringLiteral("_metaEra"),
        QStringLiteral("_metaRaceId")};
    bool copied = false;
    for (const QString& key : snapshotKeys) {
      if (previous.contains(key)) {
        result.insert(key, previous.value(key));
        copied = true;
      }
    }
    if (copied) return result;
    metadata = familyMetadata(petObject.value(QStringLiteral("n")).toString());
    metadataRace = 0;
  }

  QString original;
  if (metadataRace > 0)
    original = originalName(metadataRace);
  if (original.isEmpty()) original = metadata.value(QStringLiteral("name")).toString();
  if (original.isEmpty()) original = petObject.value(QStringLiteral("n")).toString();
  const QString attributesSequence = metadata.value(QStringLiteral("attributes")).toString();
  QString jobsSequence = metadata.value(QStringLiteral("jobs")).toString();
  QString era = eraPrefix(original);
  if (era.isEmpty()) era = eraPrefix(petObject.value(QStringLiteral("n")).toString());

  if (!original.isEmpty()) result.insert(QStringLiteral("_metaOriginalName"), original);
  if (!attributesSequence.isEmpty())
    result.insert(QStringLiteral("_metaAttributes"), attributesSequence);
  if (!jobsSequence.isEmpty() && jobsSequence != QStringLiteral("0"))
    result.insert(QStringLiteral("_metaJobs"), jobsSequence);
  if (!era.isEmpty()) result.insert(QStringLiteral("_metaEra"), era);
  if (metadataRace > 0) {
    int canonicalRace = metadataRace;
    QSet<int> visited;
    while (canonicalRace > 0 && !visited.contains(canonicalRace)) {
      visited.insert(canonicalRace);
      const int parentRace =
          pet(canonicalRace).value(QStringLiteral("groupRaceId")).toInt();
      if (parentRace <= 0 || pet(parentRace).isEmpty()) break;
      canonicalRace = parentRace;
    }
    result.insert(QStringLiteral("_metaRaceId"), canonicalRace);
  }
  return result;
}

QJsonObject PetMetadataView::item(const char* section, int defineId) const {
  return snapshot()->root.value(QString::fromLatin1(section))
      .toObject()
      .value(QString::number(defineId))
      .toObject();
}

QJsonObject PetMetadataView::pet(int raceId) const { return item("pets", raceId); }

QString PetMetadataView::petName(int raceId) const {
  return pet(raceId).value(QStringLiteral("name")).toString();
}

QString PetMetadataView::originalName(int raceId) const {
  return originalName(*snapshot(), raceId);
}

QString PetMetadataView::originalName(const PetDetailCatalogSnapshot& snapshot, int raceId) {
  const auto pet = [&snapshot](int id) { return snapshot.root.value(QStringLiteral("pets")).toObject().value(QString::number(id)).toObject(); };
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

QString PetMetadataView::attributes(const QString& sequence) const {
  const QJsonObject names = snapshot()->root.value(QStringLiteral("attributes")).toObject();
  QStringList result;
  for (const QString& part : sequence.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString id = part.trimmed();
    result.append(names.value(id).toString(QStringLiteral("属性 %1").arg(id)));
  }
  result.removeDuplicates();
  return result.isEmpty() ? QStringLiteral("—") : result.join(QStringLiteral(" + "));
}

QString PetMetadataView::jobs(const QString& sequence) const {
  const QJsonObject names = snapshot()->root.value(QStringLiteral("jobs")).toObject();
  QStringList groups;
  for (const QString& group : sequence.split(QLatin1Char('-'), Qt::SkipEmptyParts)) {
    const QList<int> ids = idsOf(group);
    QString name = fusionName(ids,snapshot()->root.value(QStringLiteral("fusionJobs")).toArray());
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

QString PetMetadataView::mappedName(const char* section, int defineId,
                                     const QString& fallbackPrefix) const {
  const QString name = item(section, defineId).value(QStringLiteral("name")).toString();
  return name.isEmpty() ? QStringLiteral("%1 %2").arg(fallbackPrefix).arg(defineId) : name;
}

QString PetMetadataView::badgeName(int defineId) const {
  return mappedName("badges", defineId, QStringLiteral("元魂"));
}

QJsonObject PetMetadataView::badge(int defineId) const {
  return item("badges", defineId);
}

QString PetMetadataView::sacredEquipmentName(int defineId) const {
  return mappedName("sacredEquipment", defineId, QStringLiteral("神源兽"));
}

QString PetMetadataView::sourceBeastName(int defineId) const {
  return mappedName("sourceBeasts", defineId, QStringLiteral("源兽"));
}

QJsonObject PetMetadataView::sourceBeast(int defineId) const {
  return item("sourceBeasts", defineId);
}

QString PetMetadataView::legendStoneName(int defineId) const {
  return mappedName("legendStones", defineId, QStringLiteral("传说石"));
}

QJsonObject PetMetadataView::legendStone(int defineId) const {
  return item("legendStones", defineId);
}

QString PetMetadataView::proficientName(int defineId) const {
  return mappedName("proficiencies", defineId, QStringLiteral("潜能"));
}

QJsonObject PetMetadataView::proficient(int defineId) const {
  return item("proficiencies", defineId);
}

QString PetMetadataView::astrolabeName(int defineId) const {
  return mappedName("astrolabe", defineId, QStringLiteral("星灵"));
}

QJsonObject PetMetadataView::astrolabe(int defineId) const {
  return item("astrolabe", defineId);
}

QJsonObject PetMetadataView::stargod(int defineId) const { return item("stargods", defineId); }

QJsonObject PetMetadataView::stargodDefinitions() const {
  return snapshot()->root.value(QStringLiteral("stargods")).toObject();
}

QJsonObject PetMetadataView::materialDefinitions() const {
  const auto current = snapshot();
  return {{QStringLiteral("items"), current->root.value(QStringLiteral("items"))},
          {QStringLiteral("money"), current->root.value(QStringLiteral("money"))}};
}

QString PetMetadataView::itemName(int itemId) const {
  return mappedName("items", itemId, QStringLiteral("道具"));
}

QString PetMetadataView::moneyName(int moneyId) const {
  return mappedName("money", moneyId, QStringLiteral("货币"));
}

QString PetMetadataView::materialName(int type, int materialId) const {
  if (type == 4) return itemName(materialId);
  if (type == 8) return moneyName(materialId);
  if (type == 134 && materialId == 1) return QStringLiteral("个人贡献币");
  if (type == 134 && materialId == 2) return QStringLiteral("联盟资金");
  return QStringLiteral("材料%1:%2").arg(type).arg(materialId);
}

QString PetMetadataView::materialCostText(int type, int materialId, int count) const {
  return QStringLiteral("%1 ×%2").arg(materialName(type, materialId)).arg(count);
}

int PetMetadataView::sacredPlanMaximum(const QJsonObject& plans, int planId) {
  if (planId <= 0) return 0;
  qint64 maximum = 0;
  return DomainNumeric::checkedInteger(plans.value(QString::number(planId)).toObject()
      .value(QStringLiteral("maxLevel")), &maximum, 1, std::numeric_limits<int>::max())
      ? int(maximum) : 0;
}

int PetMetadataView::sacredMaxStar(int planId) const {
  return sacredPlanMaximum(sacredStarPlans(), planId);
}

int PetMetadataView::sacredMaxStage(int planId) const {
  return sacredPlanMaximum(sacredStagePlans(), planId);
}
