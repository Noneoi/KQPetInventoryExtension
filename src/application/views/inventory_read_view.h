#pragma once
#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include "domain/catalog_types.h"
#include "contracts/pet_record_types.h"
#include "contracts/pet_derivation_types.h"
#include "contracts/pet_detail_types.h"
#include <memory>

// Read port used by GUI components. Production UI receives InventoryProjection;
// only Application owns the mutable repository and protocol use cases.
class InventoryReadView : public QObject {
  Q_OBJECT
public:
  explicit InventoryReadView(QObject* parent = nullptr) : QObject(parent) {}
  virtual QList<QJsonObject> backpackPets() const = 0;
  virtual QList<QJsonObject> warehousePets() const = 0;
  virtual QJsonObject backpackPet(qint64 id) const = 0;
  virtual QJsonObject warehousePet(qint64 id) const = 0;
  virtual QJsonObject detailFor(qint64 id) const = 0;
  virtual bool hasCachedDetail(qint64 id) const = 0;
  virtual QDateTime detailSavedAt(qint64 id) const = 0;
  virtual QString accountKey() const = 0;
  virtual QString cachePath() const = 0;
  virtual QString dataRoot() const = 0;
  virtual QDateTime updatedAt() const = 0;
  virtual bool isAuthenticated() const = 0;
  virtual bool currentSourceVerified() const { return false; }
  virtual std::shared_ptr<const PetDetailCatalogSnapshot> metadataSnapshot() const { return {}; }
  virtual RawPetRecordHandle rawRecordHandle(qint64) const { return {}; }
  virtual PetRecordVersion recordVersion(qint64) const { return {}; }
  virtual PetDerivedFactsHandle derivedFactsFor(qint64) const { return {}; }
  // Workbench detail consumers: 0=pet detail, 1=shop detail, 2=related-pet
  // popup (see kDetailConsumerCount). Only selected raw records are requested
  // for GUI, so the projection cannot pin the whole LRU.
  virtual void watchDetail(int, qint64) {}
  virtual PreparedPetDetailHandle preparedDetail(int, qint64) const { return {}; }
  virtual QString detailPreparationError(int) const { return {}; }
  virtual void requestDetailPage(int, DetailSection, int) {}
signals:
  void dataChanged();
  void detailChanged(qint64 instanceId);
  void accountSessionChanged(const QString& account, quint64 sessionGeneration);
  void statusChanged(const QString& status);
  void metadataChanged(quint64 revision);
};
