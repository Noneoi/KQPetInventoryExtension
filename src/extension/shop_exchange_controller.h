#pragma once

#include "shop_exchange_catalog.h"
#include "packet_contract.h"
#include "protocol_transport.h"
#include "storage_types.h"
#include "observation_freshness.h"
#include "../domain/shop_actionability.h"
#include "../contracts/cultivation_material_inventory.h"

#include <QDateTime>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <functional>

class QTimer;
class PetRepository;
class ControllerCacheStorage;
class CatalogIoService;

class ShopExchangeController final : public QObject {
  Q_OBJECT

public:
  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;

  explicit ShopExchangeController(PetRepository* repository, QObject* parent = nullptr);
  ~ShopExchangeController() override;

  void setSender(Sender sender);
  void setCatalogIoService(CatalogIoService* service);
  void setAsyncSender(AsyncSender sender) { asyncSender_ = std::move(sender); }
  bool requestInfo();
  bool requestCultivationMaterials();
  MaterialInventorySnapshot cultivationMaterialInventory() const;
  bool updateCatalog();
  bool isRunning() const { return running_; }
  bool cacheLoading() const;
  int pendingStorageCount() const;
  int pendingWriteCount() const;
  QString cacheStorageError() const;
  QJsonObject packet() const { auto result = packet_; if (!activityObservations_.isEmpty()) result.insert(QStringLiteral("_activities"),activityObservations_); return result; }
  bool hasPacket() const { return hasPacket_; }
  bool hasObservedPacket() const { return hasPacket_; }
  void setObservationClock(ObservationClock clock);
  bool acceptQuotaValidityEvidence(const TrustedObservationValidity& evidence, QString* error = nullptr);
  bool checkFreshness();
  quint64 freshnessRevision() const { return freshness_.revision(); }
  QHash<QString, ShopCondition> quotaValiditySnapshot() const;
  quint64 observedSequence(const QString& group) const { return freshness_.observationSequence(group); }
  QString freshnessSummary() const;
  QHash<QString, qint64> materialCounts() const;
  QHash<QString, qint64> cachedMaterialCounts() const { return materialCounts_; }
  bool hasMaterialCounts() const { return hasMaterialCounts_; }
  PacketFieldState fieldState(const QString& field) const {
    return fieldStates_.value(field, PacketFieldState::Missing);
  }
  QDateTime observedAt(const QString& field) const { return observedAt_.value(field); }
  QJsonObject unverifiedPackets() const { return unverifiedPackets_; }

public slots:
  void handlePacket(const QString& method, const QString& payload);
  void handleDecodedEnvelope(const InboundEnvelope& envelope, const QJsonObject& packet);
  void handleSendReceipt(const SendReceipt& receipt);

signals:
  void statusChanged(const QString& status);
  void runningChanged(bool running);
  void infoUpdated();
  void catalogUpdated();
  void cacheStateChanged();
  void freshnessChanged();
  void persistenceChanged(const QString& account, quint64 epoch, quint64 revision,
                          StorageStatus status, const QString& error);

private:
  void finish(bool ok, const QString& status);
  void handleVerifiedPacket(const QJsonObject& packet);
  bool matchesReadOnlyRequest(const InboundEnvelope& envelope, const QString& command) const;
  void completeRequest(const QString& command, bool updated, const QStringList& warnings, bool skipSharedCommand = false);
  bool queueRequest(const QString& extension, const QString& command, const QString& parameters);
  void pollAsyncRequests();
  void revokeAsyncRequests();
  void changeSession(const QString& account, quint64 generation);
  void loadCache();
  void saveCache();
  bool applyCachedObject(const QJsonObject& object);
  QJsonObject cacheObject() const;
  void initializeCacheStorage();
  bool acceptShopPacket(const QJsonObject& packet, QStringList* warnings,
                        QJsonObject* readOnly = nullptr);
  bool acceptMaterialPacket(const QJsonObject& packet, QStringList* warnings,
                            QJsonObject* readOnly = nullptr);
  bool acceptCultivationMaterialPacket(const QJsonObject& packet, QStringList* warnings);
  bool acceptSourceBeastInventoryPacket(const QJsonObject& packet, QStringList* warnings);
  bool applyCultivationMaterialCache(const QJsonObject& object);
  QJsonObject cultivationMaterialCacheObject() const;
  QSet<QString> requiredMaterialTypes() const;
  void observeGroup(const QString& group);
  void publishFreshness();
  struct ActivityReadRequest { QString sourceKey, sourceName; QJsonObject request; };
  // Server-refused activities (negative result code, e.g. a return-player
  // activity the account does not qualify for). They carry no data, must never
  // be cached, and the same session cannot change their eligibility, so they are
  // not re-queried on every refresh and are reported as "not applicable"
  // instead of a parse failure.
  QHash<QString, QString> notApplicableActivities_;
  void collectActivityReads();
  void continueRequests();
  void startNextActivityRead();
  bool acceptActivityPacket(const QJsonObject& packet, QStringList* warnings, bool verified);
  bool applyActivityCache(const QJsonObject& object);

  PetRepository* repository_ = nullptr;
  ControllerCacheStorage* cacheStorage_ = nullptr;
  ControllerCacheStorage* cultivationMaterialStorage_ = nullptr;
  ControllerCacheStorage* activityStorage_ = nullptr;
  QJsonObject activityObservations_;
  QList<ActivityReadRequest> activityReads_;
  ActivityReadRequest activeActivity_;
  MaterialInventorySnapshot cultivationMaterials_;
  bool materialsOnly_ = false;
  QPointer<CatalogIoService> catalogIo_;
  Sender sender_;
  AsyncSender asyncSender_;
  QHash<quint64, OutboundIntent> pendingSends_;
  QHash<QString, qint64> responseDeadlines_;
  QHash<QString, qint64> requestDispatchTimes_;
  QTimer* timeout_ = nullptr;
  QJsonObject packet_;
  QHash<QString, qint64> materialCounts_;
  bool running_ = false;
  bool hasPacket_ = false;
  bool hasMaterialCounts_ = false;
  bool anyUpdated_ = false;
  bool anyReadOnly_ = false;
  bool startingRequests_ = false;
  QSet<QString> pendingCommands_;
  QStringList requestWarnings_;
  QHash<QString, PacketFieldState> fieldStates_;
  QHash<QString, QDateTime> observedAt_;
  QJsonObject unverifiedPackets_;
  quint64 lastInboundSequence_ = 0;
  qint64 capturedMonotonicMs_ = -1;
  ObservationFreshness freshness_;
  bool acceptingVerifiedPacket_ = false;
  QString account_;
  QString requestAccount_;
  quint64 sessionGeneration_ = 0;
  quint64 requestSessionGeneration_ = 0;
};
