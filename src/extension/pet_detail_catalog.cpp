#include "pet_detail_catalog.h"

#include "pet_identity.h"

#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <cmath>
#include <limits>

PetDetailCatalog& PetDetailCatalog::instance() {
  static PetDetailCatalog catalog;
  return catalog;
}

PetDetailCatalog::PetDetailCatalog() {
  QFile file(QStringLiteral(":/kqpet/pet-detail-data.json"));
  if (!file.open(QIODevice::ReadOnly))
    return;
  QJsonParseError error;
  const QByteArray embeddedBytes = file.readAll();
  const QJsonDocument document = QJsonDocument::fromJson(embeddedBytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return;
  auto embedded = std::make_shared<PetDetailCatalogSnapshot>();
  embedded->contentDigest = QCryptographicHash::hash(embeddedBytes, QCryptographicHash::Sha256);
  embedded->root = document.object();
  embedded->sourceLabel = QStringLiteral("程序内置精灵元数据");
  embedded->loaded = !embedded->root.value(QStringLiteral("pets")).toObject().isEmpty();
  embedded->revision = 1;
  indexPets(embedded.get());
  embedded_ = embedded;
  snapshot_ = std::move(embedded);
}

std::shared_ptr<const PetDetailCatalogSnapshot> PetDetailCatalog::prepareOverlay(
    const std::shared_ptr<const PetDetailCatalogSnapshot>& embedded, const QJsonObject& overlay,
    const QString& source, const QDateTime& updatedAt, QString* error) {
  const auto invalid = [error]() -> std::shared_ptr<const PetDetailCatalogSnapshot> {
    if (error) *error = QStringLiteral("精灵目录覆盖结构或数字无效，保留上一份完整目录");
    return {};
  };
  if (!embedded || !embedded->loaded || overlay.isEmpty()) return invalid();
  const auto integer = [](const QJsonValue& value) {
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 0 &&
        number <= std::numeric_limits<int>::max() && std::floor(number) == number;
  };
  if (overlay.contains(QStringLiteral("schema")) &&
      (!integer(overlay.value(QStringLiteral("schema"))) || overlay.value(QStringLiteral("schema")).toInt() != 1)) return invalid();
  const QSet<QString> namedSections{QStringLiteral("attributes"), QStringLiteral("jobs")};
  if (overlay.contains(QStringLiteral("fusionJobs"))) {
    if (!overlay.value(QStringLiteral("fusionJobs")).isArray()) return invalid();
    for (const auto& rule : overlay.value(QStringLiteral("fusionJobs")).toArray()) {
      if (!rule.isObject()) return invalid();
      const auto value = rule.toObject();
      if (!value.value(QStringLiteral("name")).isString() || !value.value(QStringLiteral("jobs")).isArray()) return invalid();
      const auto groups = value.value(QStringLiteral("jobs")).toArray();
      if (groups.size() < 2) return invalid();
      for (const auto& group : groups) {
        if (!group.isArray() || group.toArray().isEmpty()) return invalid();
        for (const auto& job : group.toArray()) if (!integer(job) || job.toInt() <= 0) return invalid();
      }
    }
  }
  const QSet<QString> recordSections{QStringLiteral("pets"), QStringLiteral("badges"),
      QStringLiteral("sacredEquipment"), QStringLiteral("astrolabe"), QStringLiteral("stargods"),
      QStringLiteral("items"), QStringLiteral("money")};
  const QSet<QString> planSections{QStringLiteral("sacredStarPlans"),QStringLiteral("sacredStagePlans")};
  const auto validCosts = [&integer](const QJsonValue& value) {
    if (!value.isArray()) return false;
    for (const auto& entry : value.toArray()) {
      if (!entry.isObject()) return false;
      const auto cost = entry.toObject();
      for (const auto& key : {QStringLiteral("type"),QStringLiteral("id"),QStringLiteral("count")})
        if (!integer(cost.value(key))) return false;
      if (cost.contains(QStringLiteral("extra")) && !integer(cost.value(QStringLiteral("extra")))) return false;
    }
    return true;
  };
  for (auto section = overlay.begin(); section != overlay.end(); ++section) {
    if (!namedSections.contains(section.key()) && !recordSections.contains(section.key()) && !planSections.contains(section.key())) continue;
    if (!section.value().isObject()) return invalid();
    const QJsonObject records = section.value().toObject();
    for (auto row = records.begin(); row != records.end(); ++row) {
      bool keyValid = false;
      const int id = row.key().toInt(&keyValid);
      if (!keyValid || id < 0 || QString::number(id) != row.key()) return invalid();
      if (namedSections.contains(section.key())) {
        if (!row.value().isString()) return invalid();
        continue;
      }
      if (!row.value().isObject()) return invalid();
      const QJsonObject item = row.value().toObject();
      if (!planSections.contains(section.key()) && !item.value(QStringLiteral("name")).isString()) return invalid();
      for (const QString& field : {QStringLiteral("attributes"), QStringLiteral("jobs"), QStringLiteral("lightUpCost"),QStringLiteral("sourceName"),QStringLiteral("astrolabeBreakCosts"),QStringLiteral("sign")})
        if (item.contains(field) && !item.value(field).isString()) return invalid();
      for (const QString& field : {QStringLiteral("groupRaceId"), QStringLiteral("stargodSlotMaxLevel"),
           QStringLiteral("type"), QStringLiteral("maxLevel"), QStringLiteral("quality"), QStringLiteral("supplyExp"),
           QStringLiteral("locatedType"),QStringLiteral("sourceId")})
        if (item.contains(field) && !integer(item.value(field))) return invalid();
      for (const QString& field : {QStringLiteral("exclusive"), QStringLiteral("changeable"), QStringLiteral("limited"),QStringLiteral("isTBD")})
        if (item.contains(field) && !item.value(field).isBool()) return invalid();
      if (item.contains(QStringLiteral("limitJobs"))) {
        if (!item.value(QStringLiteral("limitJobs")).isArray()) return invalid();
        for (const auto& job : item.value(QStringLiteral("limitJobs")).toArray()) if (!integer(job)) return invalid();
      }
      if (item.contains(QStringLiteral("battlePower"))) {
        if (section.key() == QStringLiteral("astrolabe") || section.key() == QStringLiteral("badges")) {
          if (!integer(item.value(QStringLiteral("battlePower")))) return invalid();
        } else {
          if (!item.value(QStringLiteral("battlePower")).isObject()) return invalid();
          const auto powers = item.value(QStringLiteral("battlePower")).toObject();
          for (auto power = powers.begin(); power != powers.end(); ++power)
            if (!integer(power.value())) return invalid();
        }
      }
      if (item.contains(QStringLiteral("activationCost")) && !validCosts(item.value(QStringLiteral("activationCost")))) return invalid();
      if (item.contains(QStringLiteral("levels")) &&
          (section.key() == QStringLiteral("badges") || planSections.contains(section.key()))) {
        if (!item.value(QStringLiteral("levels")).isObject()) return invalid();
        const auto levels = item.value(QStringLiteral("levels")).toObject();
        for (auto level = levels.begin(); level != levels.end(); ++level) {
          bool valid = false; const int number = level.key().toInt(&valid);
          if (!valid || number < 1 || QString::number(number) != level.key() || !level.value().isObject()) return invalid();
          const auto value = level.value().toObject();
          if (!validCosts(value.value(QStringLiteral("cost")))) return invalid();
          for (const auto& key : {QStringLiteral("battlePower"),QStringLiteral("equipmentCount")})
            if (value.contains(key) && !integer(value.value(key))) return invalid();
        }
      }
    }
  }
  auto next = std::make_shared<PetDetailCatalogSnapshot>();
  // prepareOverlay runs on CatalogIoService's I/O worker in production. Bind
  // both controlled defaults and the complete overlay, independent of mtime.
  next->contentDigest = QCryptographicHash::hash(embedded->contentDigest +
      QJsonDocument(overlay).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
  next->root = embedded->root;
  for (auto section = overlay.begin(); section != overlay.end(); ++section) {
    if (section.value().isObject() && next->root.value(section.key()).isObject()) {
      QJsonObject merged = next->root.value(section.key()).toObject();
      const auto additions = section.value().toObject();
      for (auto row = additions.begin(); row != additions.end(); ++row) {
        QJsonValue value = row.value();
        // Older durable catalogs did not contain these rule fields. Retain
        // their names/data while supplying official bundled rules for the
        // same definition ID; unknown new IDs still remain unknown.
        if (value.isObject() && merged.value(row.key()).isObject() &&
            (section.key() == QStringLiteral("stargods") || section.key() == QStringLiteral("astrolabe") ||
             section.key() == QStringLiteral("badges") || section.key() == QStringLiteral("sacredEquipment") || section.key() == QStringLiteral("pets"))) {
          auto upgraded = value.toObject(); const auto baseline = merged.value(row.key()).toObject();
          const auto fields = section.key() == QStringLiteral("stargods")
              ? QStringList{QStringLiteral("type"),QStringLiteral("limitJobs"),QStringLiteral("limited")}
              : section.key() == QStringLiteral("astrolabe") ? QStringList{QStringLiteral("battlePower"),QStringLiteral("locatedType"),QStringLiteral("isTBD")}
              : section.key() == QStringLiteral("badges") ? QStringList{QStringLiteral("levels"),QStringLiteral("activationCost"),QStringLiteral("battlePower")}
              : section.key() == QStringLiteral("pets") ? QStringList{QStringLiteral("astrolabeBreakCosts"),QStringLiteral("sign")}
              : QStringList{QStringLiteral("sourceId"),QStringLiteral("sourceName")};
          for (const auto& field : fields) if (!upgraded.contains(field) && baseline.contains(field))
            upgraded.insert(field,baseline.value(field));
          value = upgraded;
        }
        merged.insert(row.key(), value);
      }
      next->root.insert(section.key(), merged);
    } else next->root.insert(section.key(), section.value());
  }
  next->loaded = !next->root.value(QStringLiteral("pets")).toObject().isEmpty();
  if (!next->loaded) return invalid();
  next->sourceLabel = source;
  next->sourceUpdatedAt = updatedAt;
  static std::atomic<quint64> revisions{1};
  next->revision = ++revisions;
  indexPets(next.get());
  return next;
}
