#pragma once

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
  QString sacredEquipmentName(int defineId) const;
  QString astrolabeName(int defineId) const;
  QJsonObject astrolabe(int defineId) const;
  QJsonObject stargod(int defineId) const;

  static int sacredMaxStar(int planId);
  static int sacredMaxStage(int planId);

private:
  PetDetailCatalog();
  QJsonObject item(const char* section, int defineId) const;
  QString mappedName(const char* section, int defineId, const QString& fallbackPrefix) const;

  QJsonObject root_;
  bool loaded_ = false;
};
