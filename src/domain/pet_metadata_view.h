#pragma once
#include "catalog_types.h"
#include <memory>

// Immutable query view. Construction never reads resources or consults globals.
class PetMetadataView final {
public:
  explicit PetMetadataView(std::shared_ptr<const PetDetailCatalogSnapshot> value)
      : value_(value ? std::move(value) : std::make_shared<PetDetailCatalogSnapshot>()) {}
  const std::shared_ptr<const PetDetailCatalogSnapshot>& snapshot() const { return value_; }
  static void indexPets(PetDetailCatalogSnapshot* value);
  QJsonObject pet(int raceId) const;
  QString petName(int raceId) const;
  QString originalName(int raceId) const;
  QString attributes(const QString& sequence) const;
  QString jobs(const QString& sequence) const;
  QString badgeName(int defineId) const;
  QJsonObject badge(int defineId) const;
  QString sacredEquipmentName(int defineId) const;
  QString astrolabeName(int defineId) const;
  QJsonObject astrolabe(int defineId) const;
  QJsonObject stargod(int defineId) const;
  QJsonObject stargodDefinitions() const;
  QJsonObject astrolabeDefinitions() const { return snapshot()->root.value(QStringLiteral("astrolabe")).toObject(); }
  QJsonObject petDefinitions() const { return snapshot()->root.value(QStringLiteral("pets")).toObject(); }
  QJsonObject sacredStarPlans() const { return snapshot()->root.value(QStringLiteral("sacredStarPlans")).toObject(); }
  QJsonObject sacredStagePlans() const { return snapshot()->root.value(QStringLiteral("sacredStagePlans")).toObject(); }
  QJsonObject badgeDefinitions() const { return snapshot()->root.value(QStringLiteral("badges")).toObject(); }
  QJsonObject materialDefinitions() const;
  QString itemName(int itemId) const;
  QString moneyName(int moneyId) const;
  QString materialName(int type, int materialId) const;
  QString materialCostText(int type, int materialId, int count) const;
  QJsonObject metadataFor(const QJsonObject& pet) const;
  QString resolvedOriginalName(const QJsonObject& pet) const;
  QString resolvedAttributes(const QJsonObject& pet) const;
  QString resolvedJobs(const QJsonObject& pet) const;
  QString resolvedEra(const QJsonObject& pet) const;
  QJsonObject enrichMetadata(const QJsonObject& pet,
                             const QJsonObject& previous = {}) const;

  int sacredMaxStar(int planId) const;
  int sacredMaxStage(int planId) const;
  static int sacredPlanMaximum(const QJsonObject& plans, int planId);

private:
  QJsonObject item(const char* section, int defineId) const;
  QString mappedName(const char* section, int defineId, const QString& fallbackPrefix) const;

  static QString eraPrefix(const QString& name);
  static QString coreName(const QString& name);
  static int liveRaceId(const QJsonObject& pet);
  QJsonObject familyMetadata(const QString& liveName) const;
  static QJsonObject familyMetadata(const PetDetailCatalogSnapshot& snapshot, const QString& liveName);
  static QString originalName(const PetDetailCatalogSnapshot& snapshot, int raceId);


  std::shared_ptr<const PetDetailCatalogSnapshot> value_;
};
