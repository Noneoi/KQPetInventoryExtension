#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>

class PetDetailCatalog final {
public:
  static const PetDetailCatalog& instance();

  bool isLoaded() const { return loaded_; }
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

  static int sacredMaxStar(int planId);
  static int sacredMaxStage(int planId);

private:
  PetDetailCatalog();
  QJsonObject item(const char* section, int defineId) const;
  QString mappedName(const char* section, int defineId, const QString& fallbackPrefix) const;

  static QString eraPrefix(const QString& name);
  static QString coreName(const QString& name);
  static int liveRaceId(const QJsonObject& pet);
  QJsonObject familyMetadata(const QString& liveName) const;
  void indexPets();

  QJsonObject root_;
  QHash<QString, int> coreToRace_;
  QHash<QString, int> eraCoreToRace_;
  bool loaded_ = false;
};
