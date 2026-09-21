#pragma once

#include "domain/catalog_types.h"

#include <QDateTime>
#include <QJsonObject>
#include <atomic>
#include <memory>

class PetSkillCatalog final {
public:
  static PetSkillCatalog& instance();

  std::shared_ptr<const PetSkillCatalogSnapshot> snapshot() const { return std::atomic_load(&snapshot_); }
  std::shared_ptr<const PetSkillCatalogSnapshot> embeddedSnapshot() const { return embedded_; }
  static std::shared_ptr<const PetSkillCatalogSnapshot> prepare(
      const QJsonObject& root, const QString& source, const QDateTime& updatedAt,
      QString* error = nullptr);

private:
  PetSkillCatalog();
  friend class CatalogIoService;
  void publish(std::shared_ptr<const PetSkillCatalogSnapshot> value) {
    std::atomic_store(&snapshot_, std::move(value));
  }
  std::shared_ptr<const PetSkillCatalogSnapshot> snapshot_ = std::make_shared<PetSkillCatalogSnapshot>();
  std::shared_ptr<const PetSkillCatalogSnapshot> embedded_;
};
