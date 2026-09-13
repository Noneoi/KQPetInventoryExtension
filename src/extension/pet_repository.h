#pragma once

#include "session_context.h"
#include "storage_types.h"
#include "inventory_read_view.h"
#include "pet_record_cache.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <deque>

class QJsonValue;
class StorageService;
class QTimer;

struct PetRepositoryIoMetrics {
  quint64 rawJsonDecodeCalls = 0;
  qint64 rawJsonDecodeNanoseconds = 0;
  qint64 rawApplyNanoseconds = 0; // Includes validation, admission and synchronous Core callbacks.
  qint64 inventoryJsonDecodeNanoseconds = 0;
};

class PetRepository final : public InventoryReadView {
  Q_OBJECT

public:
  explicit PetRepository(QObject* parent = nullptr, StorageService* storage = nullptr,
                         const QString& legacyDataRoot = {}, const PetRecordCacheLimits& rawLimits = {});
  ~PetRepository() override;

  QList<QJsonObject> backpackPets() const;
  QList<QJsonObject> warehousePets() const;
  QList<QJsonObject> backpackBriefs() const;
  QList<QJsonObject> warehouseBriefs() const;
  QList<qint64> currentInstanceIds() const;
  QJsonObject backpackPet(qint64 instanceId) const;
  QJsonObject detailFor(qint64 instanceId) const;
  QJsonObject warehousePet(qint64 instanceId) const;
  QList<qint64> backpackIds(int packType = 0) const;
  int backpackCapacity(int packType = 0) const;
  bool preserveBackpackDetail(qint64 instanceId);
  quint64 preserveBackpackDetailAsync(qint64 instanceId);
  QList<qint64> warehouseIdsByDetailAge() const;
  bool hasCachedDetail(qint64 instanceId) const;
  bool isDetailPersisted(qint64 instanceId) const;
  QDateTime detailSavedAt(qint64 instanceId) const;
  RawPetRecordHandle rawRecordHandle(qint64 instanceId) const;
  bool rawRecordResident(qint64 instanceId) const;
  quint64 detailMemoryRevision(qint64 instanceId) const;
  PetRecordVersion recordVersion(qint64 instanceId) const;
  PetRecordCacheStats rawCacheStats() const;
  PetRepositoryIoMetrics ioMetrics() const { return ioMetrics_; }
  bool markRecordDerived(const PetRecordKey& key);
  // Compact current list observation. New consumers must use this plus a
  // leased raw handle rather than retaining a naked merged detailFor() copy.
  QJsonObject recordBrief(qint64 instanceId) const;
  QJsonObject briefFor(qint64 instanceId) const { return recordBrief(instanceId); }

  QString accountKey() const { return accountKey_; }
  QString cachePath() const { return cachePath_; }
  QString dataRoot() const { return cacheRoot_; }
  StorageService* storageService() const;
  StorageContext storageContext() const { return storageContext_; }
  int pendingPersistenceCount() const { return static_cast<int>(pendingWrites_.size()); }
  int pendingReadCount() const;
  bool cacheLoading() const;
  QDateTime updatedAt() const { return updatedAt_; }
  bool isOnlineData() const { return onlineData_; }
  bool isAuthenticated() const { return authenticated_; }
  quint64 sessionGeneration() const { return sessionGeneration_; }
  quint64 inventoryRevision() const { return inventoryRevision_; }
  quint64 lastInboundSequence() const { return lastInboundSequence_; }
  SessionContext sessionContext() const { return session_; }
  InventoryObservation backpackObservation() const { return backpackObservation_; }
  InventoryObservation warehouseObservation(const QString& group) const {
    return warehouseObservations_.value(group);
  }
  bool listObservationsAuthoritativeForWrite() const;
  void setSessionSourceEvidence(const SessionSourceEvidence& source);
  void markSessionUncertain(const QString& reason);
  void setConnectionState(SessionConnectionState state, const QString& reason = {});
  bool formationKnown() const { return formationKnown_; }
  bool isDeployed(qint64 instanceId) const;

  void beginListRefresh(quint64 requestGeneration, const QString& account,
                        quint64 sessionGeneration);
  void expectListPart(const QString& command, quint64 requestGeneration,
                      const QString& account, quint64 sessionGeneration);
  void cancelListPart(const QString& command, quint64 requestGeneration);
  void expectDetail(qint64 instanceId, quint64 requestGeneration,
                    const QString& account, quint64 sessionGeneration);
  void cancelDetailRequest(qint64 instanceId, quint64 requestGeneration);
  void expectSequenceUpdate(quint64 requestGeneration, const QString& account,
                            quint64 sessionGeneration);
  void cancelSequenceUpdate(quint64 requestGeneration);

public slots:
  void handlePacket(const QString& method, const QString& payload);
  void handleEnvelope(const InboundEnvelope& envelope);
  bool requestCachedDetail(qint64 instanceId);

signals:
  void rawRecordAvailable(const RawPetRecordHandle& record);
  void rawRecordEvicted(const QString& account, quint64 epoch, qint64 instanceId,
                        quint64 detailMemoryRevision);
  void rawCachePressure(const QString& reason);
  // requested contains the exact account/epoch/memory version frozen at read
  // admission; a successful load may publish a new version before this signal.
  void rawRecordLoadFinished(const PetRecordKey& requested, bool loaded, const QString& error,
                            StorageStatus status = StorageStatus::ReadFailed);
  void persistenceChanged(const QString& account, const QString& relativePath,
                          quint64 revision, StorageStatus status, const QString& error, quint64 epoch);
  void detailPreservationFinished(quint64 taskId, qint64 instanceId,
                                  bool saved, const QString& reason);
  // A valid entity response has reached persistence. taskId == 0 means
  // admission/provenance failed; the following rejection is not a network retry.
  void detailPersistenceQueued(qint64 instanceId, quint64 requestGeneration,
                                quint64 storageTaskId);
  void cacheLoadingChanged(const QString& account, bool loading, const QString& error);
  // Emitted only after the shared JSON decoder and capture-source gate pass.
  // Domain subscribers validate their own fields. Locally caching an observed
  // response never establishes source trust or authorizes game writes.
  void packetObserved(const QJsonObject& packet, const InboundEnvelope& envelope);
  void listResponseAccepted(const QString& command, quint64 requestGeneration);
  void detailResponseAccepted(qint64 instanceId, quint64 requestGeneration);
  // The local task is satisfied by an entity observation, without proving
  // which retry (or host-originated request) produced the response.
  void detailObserved(qint64 instanceId, PacketCorrelationStrength correlation,
                      quint64 receiveSequence, bool persisted);
  void sessionTrustChanged(SessionConnectionState state, const QString& reason);
  void packetRejected(const QString& command, const QString& reason);
  void detailResponseRejected(qint64 instanceId, quint64 requestGeneration,
                              const QString& reason);
  void sequenceUpdateAccepted(quint64 requestGeneration);
  void sequenceUpdateRejected(quint64 requestGeneration, const QString& reason);
  void visualMismatchDetected(qint64 instanceId);

private:
  PetRepositoryIoMetrics ioMetrics_;
  enum class WriteKind { Inventory, Detail, Preservation, LastAccount, Migration, MigrationIndex, MigrationMarker };
  struct PendingWrite {
    WriteKind kind = WriteKind::Inventory;
    QString account;
    QString relativePath;
    quint64 epoch = 0;
    quint64 revision = 0;
    qint64 instanceId = 0;
    quint64 requestGeneration = 0;
    quint64 receiveSequence = 0;
    QDateTime savedAt;
    PetRecordKey recordKey;
    RawPetRecordHandle rawRecord;
  };
  enum class ReadKind { LastAccount, Inventory, Detail, Directory, MigrationMarker, Legacy };
  struct PendingRead {
    ReadKind kind = ReadKind::Detail;
    StorageContext context;
    QString account;
    QString relativePath;
    quint64 epoch = 0;
    quint64 loadGeneration = 0;
    quint64 inventoryRevision = 0;
    quint64 recordRevision = 0;
    quint64 detailMemoryRevision = 0;
    qint64 instanceId = 0;
    qint64 maximumBytes = 256 * 1024;
    bool selected = false;
    QString scanCursor;
  };
  struct RequestExpectation {
    QString account;
    quint64 sessionGeneration = 0;
    quint64 requestGeneration = 0;
    qint64 instanceId = 0;
    bool active = false;
    quint64 minimumReceiveSequence = 0;
  };

  static qint64 petId(const QJsonObject& pet);
  static QJsonObject merge(const QJsonObject& base, const QJsonObject& overlay);
  static QJsonObject warehouseBriefForCache(const QJsonObject& pet);
  static QJsonObject inventoryBrief(const QJsonObject& pet, bool backpack);
  static bool calculationOverlayDiffers(const QJsonObject& before, const QJsonObject& after);
  static bool calculationProjectionMatchesRaw(const QJsonObject& raw, const QJsonObject& brief);
  QJsonObject residentDetail(qint64 instanceId) const;
  bool admitRawRecords(const QList<RawPetRecordInput>& inputs, bool network);
  void reviseRawBrief(qint64 instanceId, const QJsonObject& brief, bool sourceKnown);
  void announceRaw(qint64 instanceId);
  static bool visualIdentityDiffers(const QJsonObject& detail,
                                    const QJsonObject& brief);

  bool parseBackpack(const QJsonObject& packet);
  bool parseWarehouse(const QJsonObject& packet);
  bool parseDetail(const QJsonObject& packet, quint64 requestGeneration);
  bool parseFormationLoad(const QJsonObject& packet);
  bool parseFormationPositionChange(const QJsonObject& packet);
  bool parseFormationChanged(const QJsonObject& packet);
  bool parseFormationSelection(const QJsonObject& packet);
  void updateDeployedPets(const QString& positions);
  QString formationKey(int id, int plan) const;
  QString currentFormationKey() const;
  QJsonObject withDeploymentState(const QJsonObject& pet) const;
  bool expectationMatches(const RequestExpectation& expectation) const;
  void handleDecodedPacket(const QJsonObject& packet);
  InventoryObservation currentObservation(bool complete) const;
  bool currentPacketCanPersist() const;
  bool canCacheAccountObservation() const;
  void activateAccountSession(const QString& account);
  void clearExpectations();
  void loadCache();
  void loadAccount();
  void loadDetails();
  void pumpCacheReads();
  void handleCacheRead(const StorageResult& result, const PendingRead& pending);
  bool queueCacheRead(PendingRead pending);
  bool cacheReadCurrent(const PendingRead& pending) const;
  void applyInventoryCache(const QJsonObject& object, const PendingRead& pending, const QByteArray& digest);
  void applyDetailCache(const QJsonObject& object, const PendingRead& pending,
                         const QDateTime& fileTime, const QByteArray& digest);
  void publishCacheLoading(const QString& error = {});
  void pumpMigration();
  void migrationWriteFinished(const PendingWrite& pending, const StorageResult& result);
  void saveInventory();
  quint64 saveDetail(qint64 instanceId, const QJsonObject& detail,
                     const QDateTime& savedAt = {}, quint64 requestGeneration = 0,
                     bool preservation = false);
  quint64 queueWrite(const StorageContext& context, const QString& relativePath,
                     const QByteArray& bytes, PendingWrite pending, bool required = false,
                     bool onlyIfMissing = false);
  quint64 queueJsonWrite(const StorageContext& context, const QString& relativePath,
                         const QJsonObject& object, PendingWrite pending, bool required = false,
                         bool onlyIfMissing = false);
  quint64 trackWrite(const StorageSubmission& admission, const PendingWrite& pending);
  void handleStorageResult(const StorageResult& result);
  void setAccountPaths();
  void writeLastAccount();
  void migrateLegacyCache();
  QString accountDirectory(const QString& account) const;
  QList<QJsonObject> sorted(const QHash<qint64, QJsonObject>& source) const;

  QHash<qint64, QJsonObject> backpack_;
  QHash<qint64, QJsonObject> warehouse_;
  std::unique_ptr<PetRecordCache> rawRecords_;
  QHash<qint64, PetRecordVersion> detailStates_;
  quint64 nextDetailMemoryRevision_ = 0;
  QElapsedTimer rawCacheClock_;
  qint64 rawReadRetryAfter_ = 0;
  QHash<qint64, QDateTime> detailSavedTimes_;
  QHash<qint64, quint64> detailRevisions_;
  QHash<qint64, quint64> savedDetailRevisions_;
  QHash<qint64, quint64> savedMemoryRevisions_;
  QHash<qint64, QJsonObject> detailIdentities_;
  QHash<quint64, PendingWrite> pendingWrites_;
  QPointer<StorageService> storage_;
  StorageContext storageContext_;
  StorageContext sharedStorageContext_;
  quint64 nextStoreRevision_ = 0;
  StorageStatus lastWriteAdmissionStatus_ = StorageStatus::Queued;
  QString lastWriteAdmissionError_;
  QString legacyDataRoot_;
  QHash<quint64, PendingRead> pendingReads_;
  std::deque<PendingRead> queuedReads_;
  QHash<qint64, quint64> pendingDetailReads_;
  QSet<qint64> queuedDetailReads_;
  QStringList pendingDetailNames_;
  QString detailScanCursor_;
  bool detailScanWanted_ = false;
  bool detailScanInFlight_ = false;
  bool lastPublishedCacheLoading_ = false;
  quint64 cacheLoadGeneration_ = 0;
  QTimer* cachePumpTimer_ = nullptr;
  StorageContext legacyReadContext_;
  bool migrationProbeStarted_ = false;
  bool migrationActive_ = false;
  bool migrationWriteInFlight_ = false;
  bool migrationIndexPending_ = false;
  bool migrationMarkerPending_ = false;
  QString migrationCompletedDigest_;
  QString migrationDigest_;
  QString migrationLastAccount_;
  QJsonObject migrationProfiles_;
  QStringList migrationAccounts_;
  int migrationAccountIndex_ = 0;
  QStringList migrationDetailKeys_;
  int migrationDetailIndex_ = -1;
  StorageContext migrationContext_;
  QHash<int, QStringList> packSequences_;
  QHash<int, int> packCapacities_;
  QHash<QString, QString> formationPositions_;
  QHash<int, int> formationPlans_;
  QSet<qint64> deployedPetIds_;
  int currentFormationId_ = 0;
  bool formationKnown_ = false;
  QString accountKey_ = QStringLiteral("default");
  QString cacheRoot_;
  QString cachePath_;
  QString detailsPath_;
  QDateTime updatedAt_;
  quint64 sessionGeneration_ = 0;
  quint64 inventoryRevision_ = 0;
  quint64 listRevision_ = 0;
  quint64 lastInboundSequence_ = 0;
  SessionContext session_;
  InboundEnvelope currentEnvelope_;
  bool processingEnvelope_ = false;
  InventoryObservation backpackObservation_;
  QHash<QString, InventoryObservation> warehouseObservations_;
  bool authenticated_ = false;
  // Only an uninterrupted, initially unverified observation stream may renew
  // a same-account read-only login. Hard loss of continuity never reopens here.
  bool weakReadContinuity_ = true;
  bool onlineData_ = false;
  RequestExpectation backpackExpectation_;
  RequestExpectation warehouseExpectation_;
  RequestExpectation detailExpectation_;
  RequestExpectation sequenceExpectation_;
};
