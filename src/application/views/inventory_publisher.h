#pragma once
#include <QObject>
#include <QSet>
#include <QHash>
#include <QPointer>
#include <memory>
#include "contracts/pet_derivation_types.h"
#include "contracts/pet_detail_types.h"

class PetRepository;
class InventoryProjection;
class PetDetailPreparationService;
class PetDerivationCache;
struct InventoryViewSnapshot;
struct PetDetailCatalogSnapshot;
struct PetSkillCatalogSnapshot;

// Construct on Core alongside Repository. Captures one immutable view per
// event-loop batch and hands it to the bounded GUI projection mailbox.
class InventoryPublisher final : public QObject {
  Q_OBJECT
public:
  InventoryPublisher(PetRepository* repository, InventoryProjection* projection,
                     QObject* parent = nullptr);
  void requestPublication(bool inventoryChanged = true, qint64 detailId = 0);
  void setDetailInterests(const QSet<qint64>& ids);
  void factsUpdated(qint64 id, const PetDerivedFactsHandle& facts);
  void factsFailed(const PetDerivationKey& key, const QString& error);
  void metadataUpdated(std::shared_ptr<const PetDetailCatalogSnapshot> metadata);
  void skillMetadataUpdated(std::shared_ptr<const PetSkillCatalogSnapshot> skills);
  void setDetailService(PetDetailPreparationService* service, PetDerivationCache* derivations);
  void selectDetail(int consumer, qint64 id);
  void requestDetailPage(int consumer, DetailSection section, int pageIndex);
private:
  void publish();
  void provideWaitingDetails();
  void detailInputsNeeded(quint64 request, int consumer, const DetailSelection& selection);
  void detailDependenciesChanged(const QSet<qint64>& ids);
  QSet<qint64> changedSelectedRecords();
  quint64 summaryRevision(qint64 id, const QJsonObject& brief);
  PetRepository* repository_;
  InventoryProjection* projection_;
  bool scheduled_ = false;
  bool inventoryChanged_ = false;
  QSet<qint64> detailsChanged_;
  QSet<qint64> detailInterests_;
  QHash<qint64, PetDerivedFactsHandle> facts_;
  struct FactFailure { PetDerivationKey key; QString error; };
  QHash<qint64, FactFailure> factFailures_;
  std::shared_ptr<const InventoryViewSnapshot> last_;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadata_;
  std::shared_ptr<const PetSkillCatalogSnapshot> skills_;
  QHash<qint64, int> backpackRows_;
  QHash<qint64, int> warehouseRows_;
  QPointer<PetDetailPreparationService> detailService_;
  QPointer<PetDerivationCache> derivations_;
  QHash<int, DetailSelection> detailSelections_;
  QHash<int, PetRecordKey> detailRecordVersions_;
  QHash<int, quint64> detailRequests_;
  QHash<quint64, int> waitingDetails_;
  QSet<quint64> requestedDetailReads_;
  QHash<int, PreparedPetDetailHandle> preparedDetails_;
  QHash<int, QString> detailErrors_;
  struct SummaryStamp { QJsonObject value; quint64 revision = 0; };
  QHash<qint64, SummaryStamp> summaryStamps_;
  quint64 nextSummaryRevision_ = 0;
};
