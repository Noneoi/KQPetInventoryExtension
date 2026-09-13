#pragma once

#include "../domain/catalog_types.h"
#include "../domain/pet_metadata_view.h"

#include <QHash>
#include <QJsonObject>
#include <QDateTime>
#include <QString>
#include <atomic>
#include <memory>


class PetDetailCatalog final {
public:
  static PetDetailCatalog& instance();

  std::shared_ptr<const PetDetailCatalogSnapshot> snapshot() const { return std::atomic_load(&snapshot_); }
  std::shared_ptr<const PetDetailCatalogSnapshot> embeddedSnapshot() const { return embedded_; }
  bool isLoaded() const { return snapshot()->loaded; }
  static std::shared_ptr<const PetDetailCatalogSnapshot> prepareOverlay(
      const std::shared_ptr<const PetDetailCatalogSnapshot>& embedded, const QJsonObject& overlay,
      const QString& source, const QDateTime& updatedAt, QString* error = nullptr);
  QJsonObject pet(int raceId) const { return PetMetadataView(snapshot()).pet(raceId); }
  QString petName(int raceId) const { return PetMetadataView(snapshot()).petName(raceId); }
  QString originalName(int raceId) const { return PetMetadataView(snapshot()).originalName(raceId); }
  QString attributes(const QString& sequence) const { return PetMetadataView(snapshot()).attributes(sequence); }
  QString jobs(const QString& sequence) const { return PetMetadataView(snapshot()).jobs(sequence); }
  QString badgeName(int defineId) const { return PetMetadataView(snapshot()).badgeName(defineId); }
  QJsonObject badge(int defineId) const { return PetMetadataView(snapshot()).badge(defineId); }
  QString sacredEquipmentName(int defineId) const { return PetMetadataView(snapshot()).sacredEquipmentName(defineId); }
  QString astrolabeName(int defineId) const { return PetMetadataView(snapshot()).astrolabeName(defineId); }
  QJsonObject astrolabe(int defineId) const { return PetMetadataView(snapshot()).astrolabe(defineId); }
  QJsonObject stargod(int defineId) const { return PetMetadataView(snapshot()).stargod(defineId); }
  QJsonObject stargodDefinitions() const { return PetMetadataView(snapshot()).stargodDefinitions(); }
  QJsonObject astrolabeDefinitions() const { return PetMetadataView(snapshot()).astrolabeDefinitions(); }
  QJsonObject petDefinitions() const { return PetMetadataView(snapshot()).petDefinitions(); }
  QJsonObject sacredStarPlans() const { return PetMetadataView(snapshot()).sacredStarPlans(); }
  QJsonObject sacredStagePlans() const { return PetMetadataView(snapshot()).sacredStagePlans(); }
  QJsonObject badgeDefinitions() const { return PetMetadataView(snapshot()).badgeDefinitions(); }
  QJsonObject materialDefinitions() const { return PetMetadataView(snapshot()).materialDefinitions(); }
  QString itemName(int itemId) const { return PetMetadataView(snapshot()).itemName(itemId); }
  QString moneyName(int moneyId) const { return PetMetadataView(snapshot()).moneyName(moneyId); }
  QString materialName(int type, int materialId) const { return PetMetadataView(snapshot()).materialName(type, materialId); }
  QString materialCostText(int type, int materialId, int count) const { return PetMetadataView(snapshot()).materialCostText(type, materialId, count); }
  QJsonObject metadataFor(const QJsonObject& pet) const { return PetMetadataView(snapshot()).metadataFor(pet); }
  QString resolvedOriginalName(const QJsonObject& pet) const { return PetMetadataView(snapshot()).resolvedOriginalName(pet); }
  QString resolvedAttributes(const QJsonObject& pet) const { return PetMetadataView(snapshot()).resolvedAttributes(pet); }
  QString resolvedJobs(const QJsonObject& pet) const { return PetMetadataView(snapshot()).resolvedJobs(pet); }
  QString resolvedEra(const QJsonObject& pet) const { return PetMetadataView(snapshot()).resolvedEra(pet); }
  QJsonObject enrichMetadata(const QJsonObject& pet,
                             const QJsonObject& previous = {}) const { return PetMetadataView(snapshot()).enrichMetadata(pet, previous); }

  int sacredMaxStar(int planId) const { return PetMetadataView(snapshot()).sacredMaxStar(planId); }
  int sacredMaxStage(int planId) const { return PetMetadataView(snapshot()).sacredMaxStage(planId); }

private:
  PetDetailCatalog();
  friend class CatalogIoService;
  void publish(std::shared_ptr<const PetDetailCatalogSnapshot> value) { std::atomic_store(&snapshot_, std::move(value)); }
  static void indexPets(PetDetailCatalogSnapshot* value) { PetMetadataView::indexPets(value); }
  std::shared_ptr<const PetDetailCatalogSnapshot> snapshot_ = std::make_shared<PetDetailCatalogSnapshot>();
  std::shared_ptr<const PetDetailCatalogSnapshot> embedded_;
};
