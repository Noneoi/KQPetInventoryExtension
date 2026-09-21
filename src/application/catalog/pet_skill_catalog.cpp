#include "pet_skill_catalog.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <atomic>

namespace {
bool validRoot(const QJsonObject& root) {
  if (root.value(QStringLiteral("schema")).toInt() != 1) return false;
  const auto pets = root.value(QStringLiteral("pets")).toObject();
  const auto skills = root.value(QStringLiteral("skills")).toObject();
  const auto entries = root.value(QStringLiteral("entries")).toObject();
  const auto buffs = root.value(QStringLiteral("buffs")).toObject();
  if (pets.size() < 1000 || skills.size() < 1000 || entries.isEmpty() || buffs.isEmpty()) return false;
  for (auto it = pets.constBegin(); it != pets.constEnd(); ++it) {
    bool numeric = false;
    if (it.key().toInt(&numeric) <= 0 || !numeric || !it.value().isObject()) return false;
    const auto pet = it.value().toObject();
    if (!pet.value(QStringLiteral("name")).isString() || !pet.value(QStringLiteral("slots")).isObject()) return false;
  }
  for (auto it = skills.constBegin(); it != skills.constEnd(); ++it) {
    bool numeric = false;
    if (it.key().toLongLong(&numeric) <= 0 || !numeric || !it.value().isObject()) return false;
    const auto skill = it.value().toObject();
    if (!skill.value(QStringLiteral("name")).isString() || !skill.value(QStringLiteral("description")).isString()) return false;
  }
  return true;
}
}

PetSkillCatalog& PetSkillCatalog::instance() {
  static PetSkillCatalog value;
  return value;
}

PetSkillCatalog::PetSkillCatalog() {
  QFile file(QStringLiteral(":/kqpet/pet-skill-data.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) return;
  const QByteArray bytes = file.readAll();
  QJsonParseError parseError{};
  const auto document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
  auto value = prepare(document.object(), QStringLiteral("程序内置官方技能资料"), {}, nullptr);
  if (!value) return;
  auto embedded = std::make_shared<PetSkillCatalogSnapshot>(*value);
  embedded->contentDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  embedded->revision = 1;
  embedded_ = embedded;
  snapshot_ = std::move(embedded);
}

std::shared_ptr<const PetSkillCatalogSnapshot> PetSkillCatalog::prepare(
    const QJsonObject& root, const QString& source, const QDateTime& updatedAt, QString* error) {
  if (!validRoot(root)) {
    if (error) *error = QStringLiteral("技能资料结构不完整，继续使用上一版技能缓存");
    return {};
  }
  auto result = std::make_shared<PetSkillCatalogSnapshot>();
  const QByteArray compact = QJsonDocument(root).toJson(QJsonDocument::Compact);
  result->contentDigest = QCryptographicHash::hash(compact, QCryptographicHash::Sha256);
  result->root = root;
  result->sourceLabel = source;
  result->sourceUpdatedAt = updatedAt;
  result->loaded = true;
  static std::atomic<quint64> revisions{1};
  result->revision = ++revisions;
  return result;
}
