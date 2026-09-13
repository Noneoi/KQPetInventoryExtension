#pragma once

#include "protocol_transport.h"
#include "runtime_types.h"
#include "../contracts/local_stargod_statistics.h"

#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <functional>
#include <memory>

class PetRepository;
class PetRefreshController;
class ShopExchangeController;
class RoutineOverviewController;
class AssetAnalysisController;
class StorageService;
class InventoryProjection;
class InventoryPublisher;
class AnalysisProjection;
class ImageService;
class CatalogIoService;
class PetDerivationCache;
class PetDetailPreparationService;
class DataUpdateService;
class CacheManagementService;
class LocalStargodStatisticsService;

// Valid only while a CoreAction is executing on the Core thread. Never retain
// these pointers in a GUI page or dereference them from a GUI callback.
struct CoreServices {
  QObject* root = nullptr;
  PetRepository* repo = nullptr;
  PetRefreshController* refresh = nullptr;
  ShopExchangeController* shop = nullptr;
  RoutineOverviewController* routine = nullptr;
  AssetAnalysisController* analysis = nullptr;
  StorageService* storage = nullptr;
  ImageService* images = nullptr;
  CatalogIoService* catalogs = nullptr;
  InventoryPublisher* inventoryPublisher = nullptr;
  PetDerivationCache* derivations = nullptr;
  PetDetailPreparationService* details = nullptr;
  DataUpdateService* dataUpdater = nullptr;
  CacheManagementService* cacheManager = nullptr;
  LocalStargodStatisticsService* starStatistics = nullptr;
};
using CoreAction = std::function<void(const CoreServices&)>;
using InputValidity = std::function<bool()>;

struct RuntimeOptions {
  QString dataRoot;
  QString clientRoot;
  QString buildIdentity;
  QString imageResourceVersion;
  QString profileIdentity;
  bool compatibilityVerified = false;
  InventoryProjection* inventoryProjection = nullptr;
  AnalysisProjection* analysisProjection = nullptr;
  AsyncSender sender;
  // Stream continuity (e.g. no overflow), not captureEnabled/authentication:
  // Core readiness necessarily precedes installing/enabling the host hook.
  InputValidity inputValid;
  std::function<void()> revokePending;
  QString legacyDataRoot;
  int maximumQueuedTasks = 256;
  qint64 maximumQueuedBytes = 32 * 1024 * 1024;
};

namespace ApplicationRuntimeInternal { struct State; }

class ApplicationRuntime final : public QObject {
  Q_OBJECT
public:
  explicit ApplicationRuntime(RuntimeOptions options, QObject* parent = nullptr);
  ~ApplicationRuntime() override;
  // Construction, start, shutdown and destruction belong to this GUI thread.
  // post/deliver/closing are safe cross-thread value entry points.
  bool start();
  bool post(const QString& expectedAccount, quint64 epoch, CoreAction action);
  bool postUnscoped(CoreAction action);
  bool deliverPacket(const InboundEnvelope& envelope);
  bool deliverReceipt(const SendReceipt& receipt);
  bool closing() const;
  void requestDataUpdate();
  void requestCacheAction(const QString& action, const QJsonObject& options = {});
  void requestMissingImages();
  void pauseImageBatch(bool paused);
  void cancelImageBatch();
  void requestLocalStargodStatistics();
  void cancelLocalStargodStatistics();
  // One total wait budget, including Core cancellation and Storage draining.
  // False requires the caller to retain GUI projections until Core has stopped;
  // production projections/context deliberately live until process exit.
  bool shutdown(unsigned long waitMilliseconds = 2000);

signals:
  void ready();
  // Only a queued image request/result capability is exposed to the GUI proxy.
  void imageServiceReady(ImageService* service);
  void dataUpdateStatusChanged(const QString& message, bool busy);
  void cacheActionFinished(const QString& action, const QJsonObject& result);
  void imageBatchProgress(int completed, int total, int failed);
  void imageBatchFinished(bool cancelled, int failed);
  void localStargodStatisticsChanged(const LocalStargodStatistics& result);
  void stopped(bool clean);
  void initializationFailed(const QString& error);
  void commandRejected(const QString& account, quint64 epoch, const QString& reason);
  void statusChanged(const RuntimeStatus& status);
  void shopStateChanged(const RuntimeShopState& state);
  void routineStateChanged(const RuntimeRoutineState& state);
  void listRefreshRunningChanged(const QString& account, quint64 epoch, bool running);
  void analysisRunningChanged(const QString& account, quint64 epoch, bool running);
  void detailProgressChanged(const RuntimeDetailProgress& state);
  void detailRequestFinished(const QString& account, quint64 epoch, qint64 instanceId,
                             bool succeeded, const QString& reason);
  void moveStateChanged(const RuntimeMoveState& state);
  void persistenceChanged(const RuntimePersistenceState& state);
  void replacementSelectionRequired(quint64 moveTaskId, const QString& account, quint64 epoch,
                                     qint64 incomingInstanceId, const QList<qint64>& eligibleBackpackIds);

private:
  bool enqueue(const QString& expectedAccount, quint64 epoch, bool scoped,
               CoreAction action, qint64 bytes = 256, bool packet = false);
  std::shared_ptr<ApplicationRuntimeInternal::State> state_;
  QPointer<QThread> thread_;
};
