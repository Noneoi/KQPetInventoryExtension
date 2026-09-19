#pragma once

#include <QAbstractTableModel>
#include <QJsonObject>
#include <QList>
#include <QCache>
#include <QHash>
#include <QIcon>
#include <array>
#include <memory>
#include "pet_search.h"
#include "domain/pet_metadata_view.h"
#include "contracts/pet_derivation_types.h"

struct PetSortKey {
  qint64 value = 0;
  bool known = false;
  bool operator==(const PetSortKey& other) const { return value == other.value && known == other.known; }
};

struct PetRowCache {
  qint64 instanceId = 0;
  quint64 revision = 0;
  quint64 metadataRevision = 0;
  QByteArray metadataDigest;
  std::array<QString, 9> display;
  std::array<PetSortKey, 5> sortKeys;
  PetSearchIndex search;
  std::array<PetSearchText, 2> displayedNameSearch;
  QStringList attributes;
  QStringList jobs;
  QString era;
  QString nameToolTip;
  QString identityText;
  QString attributeVisualKey;
  QString powerToolTip;
  bool powerFromFacts = false;
  bool currentSourceVerified = false;
  bool deployed = false;
};

struct PetModelCacheStats {
  quint64 rowRevisions = 0;
  quint64 searchIndexesBuilt = 0;
  quint64 sortKeysBuilt = 0;
  quint64 battlePowerAnalyses = 0;
  quint64 preparedFactsConsumed = 0;
  quint64 preparedFactsUnavailable = 0;
  quint64 metadataCachesBuilt = 0;
  quint64 attributeIconsBuilt = 0;
  quint64 positionPasses = 0;
  quint64 idLookups = 0;
  quint64 invalidSnapshots = 0;
  int attributeIconEntries = 0;
  quint64 preparationBatches = 0;
  quint64 preparationRows = 0;
  quint64 completedPreparations = 0;
  quint64 supersededPreparations = 0;
  qint64 maximumBatchNanoseconds = 0;
  qint64 maximumAtomicNanoseconds = 0;
};

class PetImageCache;
class InventoryReadView;

class PetTableModel final : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column {
    DisplayNameColumn = 0,
    OriginalNameColumn,
    AttributesColumn,
    JobsColumn,
    EraColumn,
    LevelColumn,
    BattlePowerColumn,
    DeployedColumn,
    BackpackPositionColumn,
    WarehousePositionColumn = DeployedColumn,
  };

  enum DataRole {
    InstanceIdRole = Qt::UserRole,
    IdentityTextRole = Qt::UserRole + 1,
    PetObjectRole = Qt::UserRole + 2,
    FilterCacheRevisionRole = Qt::UserRole + 3,
    SortCacheRevisionRole = Qt::UserRole + 4,
    NameSearchIndexRole = Qt::UserRole + 5,
  };

  enum class Location {
    Backpack,
    Warehouse,
  };

  explicit PetTableModel(Location location, InventoryReadView* repository = nullptr,
                         PetImageCache* imageCache = nullptr,
                         QObject* parent = nullptr);
  ~PetTableModel() override;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  void setPets(const QList<QJsonObject>& pets);
  bool applyPets(const QList<QJsonObject>& pets);
  bool applyChanges(const QList<QJsonObject>& upserts, const QList<qint64>& removedIds = {});
  bool updatePet(const QJsonObject& pet);
  QJsonObject petAt(int row) const;
  qint64 instanceIdAt(int row) const;
  int rowForInstanceId(qint64 instanceId) const;
  Location location() const;
  const PetRowCache* cachedRow(int row) const;
  PetModelCacheStats cacheStats() const;
  // Compatibility invalidation of the explicitly supplied snapshot. A read
  // view must publish that exact revision; this method never fetches a catalog.
  void setMetadataRevision(quint64 revision);
  // Production path. Input validation and cache derivation yield between
  // bounded work slices; no timer callback runs the old complete derive loop.
  void setPetsAsync(const QList<QJsonObject>& pets);
  void setMetadataSnapshot(std::shared_ptr<const PetDetailCatalogSnapshot> metadata);
  bool preparationRunning() const;

signals:
  void preparationStateChanged(bool running);
  void preparationFinished(quint64 generation, bool accepted);

private:
  struct Row {
    QJsonObject pet;
    PetRowCache cache;
    std::weak_ptr<const PetDerivedFactsRecord> factsIdentity;
    int position = -1;
  };
  struct Change { int row; int first; int last; QList<int> roles; };
  void derive(Row& row, const QJsonObject& pet, bool force, QList<Change>* changes,
              const PetMetadataView* metadata = nullptr);
  struct Preparation;
  void prepareSlice();
  void schedulePreparation();
  void cancelPreparation();
  void refreshFacts(qint64 id);
  void reorder(const QList<std::shared_ptr<Row>>& rows);
  void notifyChanges(QList<Change> changes);
  QIcon attributeIcon(const QString& key) const;

  Location location_ = Location::Warehouse;
  InventoryReadView* repository_ = nullptr;
  PetImageCache* imageCache_ = nullptr;
  QList<std::shared_ptr<Row>> rows_;
  QHash<qint64, Row*> rowsById_;
  mutable QCache<QString, QIcon> attributeIcons_{64};
  mutable PetModelCacheStats stats_;
  quint64 metadataRevision_ = 0;
  quint64 nextRevision_ = 0;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadataSnapshot_;
  std::shared_ptr<const PetDetailCatalogSnapshot> appliedMetadata_;
  QList<QJsonObject> requestedPets_;
  QHash<qint64, QJsonObject> pendingOverrides_;
  std::unique_ptr<Preparation> preparation_;
  quint64 requestedGeneration_ = 0;
  bool preparationScheduled_ = false;
  bool preparationRunning_ = false;
};
