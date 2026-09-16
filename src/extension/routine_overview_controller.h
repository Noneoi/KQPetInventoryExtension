#pragma once

#include "packet_contract.h"
#include "protocol_transport.h"
#include "storage_types.h"
#include "observation_freshness.h"

#include <QDateTime>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>

class PetRepository;
class ControllerCacheStorage;
class CatalogIoService;
class QTimer;

class RoutineOverviewController final : public QObject {
  Q_OBJECT

public:
  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;

  explicit RoutineOverviewController(PetRepository* repository, QObject* parent = nullptr);
  ~RoutineOverviewController() override;
  void setSender(Sender sender);
  void setCatalogIoService(CatalogIoService* service);
  void setAsyncSender(AsyncSender sender) { asyncSender_ = std::move(sender); }
  bool requestRefresh();
  QJsonObject dailyPacket() const { return dailyPacket_; }
  QSet<int> activeRedPoints() const { return activeRedPoints_; }
  QJsonObject opportunityPackets() const;
  QJsonObject cachedOpportunityPackets() const { return opportunityPackets_; }
  bool hasDailyPacket() const { return hasDailyPacket_; }
  bool hasRedPointPacket() const { return hasRedPointPacket_; }
  bool hasObservedDailyPacket() const { return hasDailyPacket_; }
  void setObservationClock(ObservationClock clock);
  bool acceptPeriodValidityEvidence(const TrustedObservationValidity& evidence, QString* error = nullptr);
  bool checkFreshness();
  quint64 freshnessRevision() const { return freshness_.revision(); }
  QHash<QString, ObservationValidity> periodValiditySnapshot() const;
  quint64 observedSequence(const QString& group) const { return freshness_.observationSequence(group); }
  QString freshnessSummary() const;
  bool isRunning() const { return running_; }
  bool cacheLoading() const;
  int pendingStorageCount() const;
  int pendingWriteCount() const;
  QString cacheStorageError() const;
  PacketFieldState fieldState(const QString& field) const {
    return fieldStates_.value(field, PacketFieldState::Missing);
  }
  // True once this session recorded a state for the group (queried, returned,
  // invalid or explicitly missing). Groups that no query ever covered are
  // outside the current expectation instead of forcing a permanent partial
  // result. Never true for an unobserved group just because it is known here.
  bool hasRecordedState(const QString& group) const { return fieldStates_.contains(group); }
  QDateTime observedAt(const QString& field) const { return observedAt_.value(field); }
  QJsonObject unverifiedPackets() const { return unverifiedPackets_; }

public slots:
  void handlePacket(const QString& method, const QString& payload);
  void handleDecodedEnvelope(const InboundEnvelope& envelope, const QJsonObject& packet);
  void handleSendReceipt(const SendReceipt& receipt);

signals:
  void statusChanged(const QString& status);
  void runningChanged(bool running);
  void dataUpdated();
  void catalogUpdated();
  void cacheStateChanged();
  void freshnessChanged();
  void persistenceChanged(const QString& account, quint64 epoch, quint64 revision,
                          StorageStatus status, const QString& error);

private:
  void changeSession(const QString& account, quint64 generation);
  void handleVerifiedPacket(const QJsonObject& packet);
  bool matchesReadOnlyRequest(const InboundEnvelope& envelope, const QString& command) const;
  bool queueRequest(const QString& extension, const QString& command, const QString& parameters);
  void pollAsyncRequests();
  void revokeAsyncRequests();
  void completeRequest(const QString& command, bool updated,
                       const QString& warning = {});
  void finish(bool publish, const QString& status);
  void loadCache();
  void saveCache();
  bool applyCachedObject(const QJsonObject& object);
  QJsonObject cacheObject() const;
  void initializeCacheStorage();
  bool acceptDailyPacket(const QJsonObject& packet, QStringList* warnings,
                         QJsonObject* readOnly = nullptr);
  void observeGroup(const QString& group);
  void publishFreshness();

  PetRepository* repository_ = nullptr;
  ControllerCacheStorage* cacheStorage_ = nullptr;
  QPointer<CatalogIoService> catalogIo_;
  Sender sender_;
  AsyncSender asyncSender_;
  QHash<quint64, OutboundIntent> pendingSends_;
  QHash<QString, qint64> responseDeadlines_;
  QHash<QString, qint64> requestDispatchTimes_;
  QTimer* timeout_ = nullptr;
  QJsonObject dailyPacket_;
  QSet<int> activeRedPoints_;
  QJsonObject opportunityPackets_;
  bool hasDailyPacket_ = false;
  bool hasRedPointPacket_ = false;
  bool running_ = false;
  bool anyUpdated_ = false;
  bool anyReadOnly_ = false;
  bool startingRequests_ = false;
  QHash<QString, PacketFieldState> fieldStates_;
  QHash<QString, QDateTime> observedAt_;
  QJsonObject unverifiedPackets_;
  quint64 lastInboundSequence_ = 0;
  qint64 capturedMonotonicMs_ = -1;
  ObservationFreshness freshness_;
  bool acceptingVerifiedPacket_ = false;
  QSet<QString> pendingCommands_;
  QHash<QString, QString> requestLabels_;
  QStringList requestWarnings_;
  QString account_;
  QString requestAccount_;
  quint64 sessionGeneration_ = 0;
  quint64 requestSessionGeneration_ = 0;
};
