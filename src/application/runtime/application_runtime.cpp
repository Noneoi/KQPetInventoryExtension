#include "application_runtime.h"

#include "application/views/inventory_projection.h"
#include "application/views/inventory_publisher.h"
#include "application/views/analysis_projection.h"
#include "application/views/analysis_publisher.h"
#include "application/pet/pet_repository.h"
#include "application/pet/pet_refresh_controller.h"
#include "application/shop/shop_exchange_controller.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "application/routine/routine_overview_controller.h"
#include "application/catalog/routine_overview_catalog.h"
#include "application/analysis/asset_analysis_controller.h"
#include "storage/storage_service.h"
#include "application/images/image_service.h"
#include "diagnostics/diagnostic_logger.h"
#include "persistence_summary.h"
#include "application/catalog/catalog_io_service.h"
#include "application/catalog/pet_detail_catalog.h"
#include "application/pet/pet_derivation_cache.h"
#include "application/pet/pet_detail_preparation_service.h"
#include "data_update_service.h"
#include "cache_management_service.h"
#include "application/analysis/local_stargod_statistics_service.h"
#include "domain/pet_identity.h"

#include <QDir>
#include <QJsonArray>
#include <QMetaObject>
#include <QTimer>
#include <atomic>
#include <deque>
#include <mutex>
#include <utility>

namespace ApplicationRuntimeInternal {
using GuiAction = std::function<void(ApplicationRuntime*)>;
struct State {
  explicit State(RuntimeOptions value) : options(std::move(value)) {}
  const RuntimeOptions options;
  std::mutex mutex;
  ApplicationRuntime* facade = nullptr;
  QObject* receiver = nullptr;
  CoreServices services;
  std::atomic_bool closing{false};
  std::atomic_bool inputUncertain{false};
  std::atomic<qint64> shutdownDeadline{0};
  bool started = false;
  bool finished = false;
  bool clean = false;
  bool imageBatchRunning = false; // Core thread only.
  int queuedTasks = 0;
  qint64 queuedBytes = 0;
  bool guiScheduled = false;
  std::deque<std::pair<QString, GuiAction>> guiActions;
};

bool inputValid(const std::shared_ptr<State>& state) {
  if (state->closing.load(std::memory_order_acquire) || state->inputUncertain.load(std::memory_order_acquire)) return false;
  try { return !state->options.inputValid || state->options.inputValid(); }
  catch (...) { return false; }
}
void revoke(const std::shared_ptr<State>& state) {
  try { if (state->options.revokePending) state->options.revokePending(); }
  catch (...) { /* Cancellation must not escape a host shutdown callback. */ }
}
void drainGui(const std::shared_ptr<State>& state);
void notify(const std::shared_ptr<State>& state, GuiAction action, const QString& coalesce = {}) {
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->facade) return;
    if (!coalesce.isEmpty()) {
      for (auto& pending : state->guiActions) {
        if (pending.first == coalesce) { pending.second = std::move(action); return; }
      }
    }
    if (state->guiActions.size() >= 256) overflow = true;
    else {
      state->guiActions.emplace_back(coalesce, std::move(action));
      if (!state->guiScheduled) {
        state->guiScheduled = true;
        QMetaObject::invokeMethod(state->facade, [state] { drainGui(state); }, Qt::QueuedConnection);
      }
    }
  }
  if (overflow && !state->inputUncertain.exchange(true, std::memory_order_acq_rel)) revoke(state);
}
void drainGui(const std::shared_ptr<State>& state) {
  std::deque<GuiAction> actions;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    for (int count = 0; count < 16 && !state->guiActions.empty(); ++count) {
      actions.push_back(std::move(state->guiActions.front().second));
      state->guiActions.pop_front();
    }
  }
  for (const auto& action : actions) {
    ApplicationRuntime* facade = nullptr;
    { std::lock_guard<std::mutex> lock(state->mutex); facade = state->facade; }
    if (!facade) return;
    action(facade); // GUI thread; each action emits at most one signal.
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->facade && !state->guiActions.empty())
    QMetaObject::invokeMethod(state->facade, [state] { drainGui(state); }, Qt::QueuedConnection);
  else state->guiScheduled = false;
}
struct Reservation {
  Reservation(std::shared_ptr<State> value, qint64 count) : state(std::move(value)), bytes(count) {}
  std::shared_ptr<State> state;
  qint64 bytes;
  bool armed = false;
  ~Reservation() {
    if (!armed) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    --state->queuedTasks; state->queuedBytes -= bytes;
  }
};
struct CoreViews {
  quint64 shopPublication = 0;
  quint64 routinePublication = 0;
  QString shopStatus;
  QString routineStatus;
  RuntimeMoveState move;
  QDate shopDate;
};

void closeCore(const std::shared_ptr<State>& state, const CoreServices& services, QThread* thread) {
  if (services.starStatistics) services.starStatistics->close();
  if (services.dataUpdater) services.dataUpdater->close();
  if (services.cacheManager) services.cacheManager->close();
  if (services.details) services.details->shutdown();
  services.repo->setConnectionState(SessionConnectionState::Closing, QStringLiteral("扩展正在退出"));
  services.refresh->cancelWarehouseDetailRefresh();
  services.refresh->cancelMove();
  if (services.images) services.images->shutdown();
  if (services.catalogs) services.catalogs->close();
  qint64 remaining = qMax<qint64>(0, state->shutdownDeadline.load(std::memory_order_acquire) - transportMonotonicMs());
  const bool computeClean = services.analysis->shutdownAnalysis(int(qMin<qint64>(remaining, 2000)));
  DiagnosticLogger::closeStorage(services.storage);
  remaining = qMax<qint64>(0, state->shutdownDeadline.load(std::memory_order_acquire) - transportMonotonicMs());
  const bool storageClean = services.storage->shutdown(static_cast<unsigned long>(qMin<qint64>(remaining, 2000)));
  const bool clean = computeClean && storageClean;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->clean = clean;
    state->receiver = nullptr;
  }
  thread->quit();
}

class CoreThread final : public QThread {
public:
  explicit CoreThread(std::shared_ptr<State> state) : state_(std::move(state)) { setObjectName(QStringLiteral("KQPetCore")); }
protected:
  void run() override {
    const auto state = state_;
    CoreServices services;
    services.root = new QObject;
    services.root->setObjectName(QStringLiteral("KQPetCoreRoot"));
    InventoryPublisher* inventoryPublisher = nullptr;
    AnalysisPublisher* analysisPublisher = nullptr;
    bool initialized = false;
    try {
      services.storage = new StorageService(state->options.dataRoot, {}, {}, services.root);
      DiagnosticLogger::attachStorage(services.storage, services.root);
      services.catalogs = new CatalogIoService(services.storage, {}, services.root);
      services.dataUpdater = new DataUpdateService(services.storage, services.root);
      services.cacheManager = new CacheManagementService(services.storage, state->options.clientRoot, services.root);
      services.repo = new PetRepository(services.root, services.storage, state->options.legacyDataRoot);
      services.starStatistics = new LocalStargodStatisticsService(services.storage,services.root);
      QObject::connect(services.starStatistics,&LocalStargodStatisticsService::updated,services.root,
          [state,services](const LocalStargodStatistics& result) {
        if (result.account != services.repo->accountKey() || result.epoch != services.repo->sessionGeneration()) return;
        notify(state,[result](ApplicationRuntime* target) { emit target->localStargodStatisticsChanged(result); },QStringLiteral("local-star-statistics"));
      });
      QObject::connect(services.repo,&PetRepository::accountSessionChanged,services.starStatistics,
          [statistics = services.starStatistics] { statistics->cancel(); });
      services.refresh = new PetRefreshController(services.repo, services.root);
      services.shop = new ShopExchangeController(services.repo, services.root);
      services.routine = new RoutineOverviewController(services.repo, services.root);
      services.shop->setCatalogIoService(services.catalogs);
      services.routine->setCatalogIoService(services.catalogs);
      services.analysis = new AssetAnalysisController(services.repo, services.shop, services.routine, services.root);
      services.derivations = new PetDerivationCache(
          [analysis = services.analysis](std::function<void()> job) { return analysis->postPriorityCompute(std::move(job)); },
          services.storage, {}, services.root);
      services.analysis->setDerivationCache(services.derivations);
      services.analysis->setCompatibilityIdentity(state->options.buildIdentity, state->options.profileIdentity,
                                                   state->options.compatibilityVerified);
      QObject::connect(services.catalogs, &CatalogIoService::catalogUpdated, services.root,
          [services](CatalogKind kind, quint64) {
        if (kind == CatalogKind::PetDetail) services.analysis->metadataChanged();
      });
      for (auto kind : {CatalogKind::Shop, CatalogKind::Routine, CatalogKind::PetDetail}) services.catalogs->requestReload(kind);
      ImageServiceOptions imageOptions;
      imageOptions.dataRoot = state->options.dataRoot;
      imageOptions.resourceVersion = state->options.imageResourceVersion.isEmpty() ? QStringLiteral("1") : state->options.imageResourceVersion;
      ImageExecutors executors;
      executors.io = [storage = services.storage](std::function<void(QObject*)> job) {
        return storage->postAuxiliary(std::move(job));
      };
      executors.compute = [analysis = services.analysis](std::function<void()> job) {
        return analysis->postPriorityCompute(std::move(job));
      };
      services.images = new ImageService(std::move(imageOptions), std::move(executors), services.root);
      QObject::connect(services.dataUpdater, &DataUpdateService::progress, services.root,
          [state](const QString& text) { notify(state, [text](ApplicationRuntime* target) {
        emit target->dataUpdateStatusChanged(text, true);
      }, QStringLiteral("data-update")); });
      QObject::connect(services.dataUpdater, &DataUpdateService::finished, services.root,
          [state, services](bool, const QStringList& components, const QString& text) {
        if (components.contains(QStringLiteral("pets"))) services.catalogs->requestReload(CatalogKind::PetDetail);
        if (components.contains(QStringLiteral("shop"))) services.catalogs->requestReload(CatalogKind::Shop);
        if (components.contains(QStringLiteral("routines"))) services.catalogs->requestReload(CatalogKind::Routine);
        if (components.contains(QStringLiteral("images")) || components.contains(QStringLiteral("icons"))) services.images->reloadImageIndex();
        notify(state, [text](ApplicationRuntime* target) { emit target->dataUpdateStatusChanged(text, false); }, QStringLiteral("data-update"));
      });
      QObject::connect(services.cacheManager, &CacheManagementService::finished, services.root,
          [state](const QString& action, const QJsonObject& result) { notify(state, [action, result](ApplicationRuntime* target) {
        emit target->cacheActionFinished(action, result);
      }); });
      QObject::connect(services.images, &ImageService::batchProgress, services.root,
          [state](int completed, int total, int failed) { notify(state, [completed, total, failed](ApplicationRuntime* target) {
        emit target->imageBatchProgress(completed, total, failed);
      }, QStringLiteral("image-batch")); });
      QObject::connect(services.images, &ImageService::batchFinished, services.root,
          [state](bool cancelled, int failed) { state->imageBatchRunning = false;
        notify(state, [cancelled, failed](ApplicationRuntime* target) { emit target->imageBatchFinished(cancelled, failed); });
      });
      const AsyncSender sender = [state](const OutboundIntent& intent) {
        return inputValid(state) && state->options.sender && state->options.sender(intent);
      };
      services.refresh->setAsyncSender(sender);
      services.shop->setAsyncSender(sender);
      services.routine->setAsyncSender(sender);
      QObject::connect(services.repo, &PetRepository::packetObserved, services.root,
          [services](const QJsonObject& packet, const InboundEnvelope& envelope) {
        services.shop->handleDecodedEnvelope(envelope, packet);
        services.routine->handleDecodedEnvelope(envelope, packet);
      });
      inventoryPublisher = new InventoryPublisher(services.repo, state->options.inventoryProjection, services.root);
      services.inventoryPublisher = inventoryPublisher;
      services.details = new PetDetailPreparationService(
          [analysis = services.analysis](std::function<void()> job) { return analysis->postPriorityCompute(std::move(job)); },
          {}, services.root);
      inventoryPublisher->setDetailService(services.details, services.derivations);
      QObject::connect(services.analysis, &AssetAnalysisController::derivedFactsChanged, inventoryPublisher,
          [inventoryPublisher](qint64 id, const PetDerivedFactsHandle& facts) { inventoryPublisher->factsUpdated(id, facts); });
      QObject::connect(services.analysis, &AssetAnalysisController::derivedFactsFailed,
          inventoryPublisher, &InventoryPublisher::factsFailed);
      QObject::connect(services.catalogs, &CatalogIoService::catalogUpdated, inventoryPublisher,
          [inventoryPublisher](CatalogKind kind, quint64) {
        if (kind == CatalogKind::PetDetail)
          inventoryPublisher->metadataUpdated(PetDetailCatalog::instance().snapshot());
      });
      analysisPublisher = new AnalysisPublisher(services.repo, services.analysis, state->options.analysisProjection, services.root);
      auto* validity = new QTimer(services.root);
      validity->setInterval(100);
      QObject::connect(validity, &QTimer::timeout, services.root, [state, services] {
        if (!state->closing.load() && !inputValid(state)) {
          if (!state->inputUncertain.exchange(true)) revoke(state);
          if (services.repo->sessionContext().state != SessionConnectionState::Uncertain)
            services.repo->markSessionUncertain(QStringLiteral("输入来源或队列完整性已失效"));
        }
      });
      validity->start();
      auto* freshnessTick = new QTimer(services.root);
      freshnessTick->setInterval(1000);
      QObject::connect(freshnessTick, &QTimer::timeout, services.root, [services] {
        services.analysis->checkInputFreshness();
      });
      freshnessTick->start();
      const auto views = std::make_shared<CoreViews>();
      const auto persistenceSummary = std::make_shared<PersistenceSummary>();
      const auto persistence = [state, services, persistenceSummary](const QString& account, quint64 epoch, const QString& record,
                                                 quint64 revision, StorageStatus status, const QString& error) {
        if (!record.isEmpty() && status != StorageStatus::Queued && status != StorageStatus::Saved &&
            status != StorageStatus::Superseded) {
          DiagnosticLogger::event({QStringLiteral("record_write_failed"), QStringLiteral("storage"), 0,
              QStringLiteral("commit"), QStringLiteral("account=%1 record=%2 revision=%3: %4")
                  .arg(DiagnosticLogger::maskedAccount(account), record).arg(revision).arg(error),
              QStringLiteral("核对磁盘空间、权限和本机写入状态；仅在必要时手动重试")}, true);
        }
        const int pending = services.repo->pendingPersistenceCount() + services.shop->pendingWriteCount() +
            services.routine->pendingWriteCount() + services.refresh->pendingSettingsWriteCount() +
            services.analysis->pendingWriteCount();
        const auto value = persistenceSummary->update(services.repo->accountKey(), services.repo->sessionGeneration(),
            account, epoch, record, revision, status, error, pending);
        notify(state, [value](ApplicationRuntime* target) { emit target->persistenceChanged(value); }, QStringLiteral("persistence"));
      };
      QObject::connect(services.repo, &PetRepository::accountSessionChanged, services.root,
          [persistence](const QString& account, quint64 epoch) {
        persistence(account, epoch, {}, 0, StorageStatus::Superseded, {});
      });
      // All record owners registered their completion handlers before this.
      // Re-publish the current global count even when an old account write
      // completes, so a new account cannot remain stuck on an old Pending.
      QObject::connect(services.storage, &StorageService::completed, services.root,
          [persistence, services](const StorageResult&) {
        persistence(services.repo->accountKey(), services.repo->sessionGeneration(), {}, 0, StorageStatus::Superseded, {});
      });
      QObject::connect(services.repo, &PetRepository::persistenceChanged, services.root,
          [persistence](const QString& account, const QString& record, quint64 revision,
                         StorageStatus status, const QString& error, quint64 epoch) {
        persistence(account, epoch, record, revision, status, error);
      });
      QObject::connect(services.shop, &ShopExchangeController::persistenceChanged, services.root,
          [persistence](const QString& account, quint64 epoch, quint64 revision, StorageStatus status, const QString& error) {
        persistence(account, epoch, QStringLiteral("shop-cache.json"), revision, status, error);
      });
      QObject::connect(services.routine, &RoutineOverviewController::persistenceChanged, services.root,
          [persistence](const QString& account, quint64 epoch, quint64 revision, StorageStatus status, const QString& error) {
        persistence(account, epoch, QStringLiteral("routine-cache.json"), revision, status, error);
      });
      QObject::connect(services.refresh, &PetRefreshController::timingsPersistenceChanged, services.root,
          [persistence, services](quint64 revision, StorageStatus status, const QString& error) {
        persistence(services.repo->accountKey(), services.repo->sessionGeneration(), QStringLiteral("settings.json"), revision, status, error);
      });
      QObject::connect(services.analysis, &AssetAnalysisController::persistenceChanged, services.root, persistence);
      const auto shop = [state, services, views] {
        RuntimeShopState value{services.repo->accountKey(), services.repo->sessionGeneration(), ++views->shopPublication,
            services.shop->packet(), services.shop->materialCounts(), services.shop->cachedMaterialCounts(),
            services.shop->unverifiedPackets(), services.shop->hasPacket(), services.shop->hasMaterialCounts(),
            services.shop->isRunning(), views->shopStatus};
        value.freshnessSummary = services.shop->freshnessSummary();
        value.freshnessRevision = services.shop->freshnessRevision();
        value.quotaValidity = services.shop->quotaValiditySnapshot();
        value.catalog = ShopExchangeCatalog::instance().snapshot();
        value.catalogDate = currentCatalogBusinessDate(); views->shopDate = value.catalogDate;
        value.cultivationMaterials = services.shop->cultivationMaterialInventory();
        notify(state, [value](ApplicationRuntime* target) { emit target->shopStateChanged(value); }, QStringLiteral("shop-state"));
      };
      QObject::connect(freshnessTick, &QTimer::timeout, services.root, [views, shop] {
        if (views->shopDate != currentCatalogBusinessDate()) shop();
      });
      const auto routine = [state, services, views] {
        RuntimeRoutineState value{services.repo->accountKey(), services.repo->sessionGeneration(), ++views->routinePublication,
            services.routine->dailyPacket(), services.routine->activeRedPoints(), services.routine->opportunityPackets(),
            services.routine->cachedOpportunityPackets(), services.routine->unverifiedPackets(),
            services.routine->hasDailyPacket(), services.routine->hasRedPointPacket(), services.routine->isRunning(), views->routineStatus};
        value.freshnessSummary = services.routine->freshnessSummary();
        value.catalog = RoutineOverviewCatalog::instance().snapshot();
        value.periodValidity = services.routine->periodValiditySnapshot();
        notify(state, [value](ApplicationRuntime* target) { emit target->routineStateChanged(value); }, QStringLiteral("routine-state"));
      };
      const auto status = [state, services](const QString& module, const QString& message) {
        RuntimeStatus value{services.repo->accountKey(), services.repo->sessionGeneration(), module, message};
        notify(state, [value](ApplicationRuntime* target) { emit target->statusChanged(value); }, QStringLiteral("status:") + module);
      };
      QObject::connect(services.repo, &PetRepository::sessionTrustChanged, services.root,
          [repository = services.repo](SessionConnectionState connection, const QString& reason) {
        DiagnosticLogger::event({QStringLiteral("session_changed"), QStringLiteral("session"), 0,
            QStringLiteral("observe"),
            QStringLiteral("state=%1 epoch=%2 readAvailable=%3 persistAllowed=%4; %5")
                .arg(static_cast<int>(connection)).arg(repository->sessionGeneration())
                .arg(repository->isAuthenticated()).arg(repository->sessionContext().canPersist()).arg(reason),
            QStringLiteral("inspect session observation")});
      });
      QObject::connect(services.shop, &ShopExchangeController::infoUpdated, services.root, shop);
      QObject::connect(services.shop, &ShopExchangeController::runningChanged, services.root, [shop](bool) { shop(); });
      QObject::connect(services.shop, &ShopExchangeController::statusChanged, services.root,
          [shop, status, views](const QString& text) { views->shopStatus = text; shop(); status(QStringLiteral("shop"), text); });
      QObject::connect(services.routine, &RoutineOverviewController::dataUpdated, services.root, routine);
      QObject::connect(services.routine, &RoutineOverviewController::runningChanged, services.root, [routine](bool) { routine(); });
      QObject::connect(services.routine, &RoutineOverviewController::statusChanged, services.root,
          [routine, status, views](const QString& text) { views->routineStatus = text; routine(); status(QStringLiteral("routine"), text); });
      QObject::connect(services.refresh, &PetRefreshController::statusChanged, services.root,
          [status](const QString& text) { status(QStringLiteral("inventory"), text); });
      QObject::connect(services.analysis, &AssetAnalysisController::statusChanged, services.root,
          [status](const QString& text) { status(QStringLiteral("analysis"), text); });
      QObject::connect(services.analysis, &AssetAnalysisController::analysisRunningChanged, services.root,
          [state, services](bool running) {
        const auto account = services.repo->accountKey(); const auto epoch = services.repo->sessionGeneration();
        notify(state, [account, epoch, running](ApplicationRuntime* target) {
          emit target->analysisRunningChanged(account, epoch, running);
        }, QStringLiteral("analysis-running"));
      });
      QObject::connect(services.refresh, &PetRefreshController::listRefreshRunningChanged, services.root,
          [state, services](bool running) {
        const QString account = services.repo->accountKey(); const quint64 epoch = services.repo->sessionGeneration();
        notify(state, [account, epoch, running](ApplicationRuntime* target) {
          emit target->listRefreshRunningChanged(account, epoch, running);
        }, QStringLiteral("list-running"));
      });
      QObject::connect(services.refresh, &PetRefreshController::detailProgressChanged, services.root,
          [state, services](bool running, bool paused, int completed, int total, int succeeded, int failed,
                             qint64 instance, int seconds) {
        RuntimeDetailProgress value{services.repo->accountKey(), services.repo->sessionGeneration(), running, paused,
                                    completed, total, succeeded, failed, instance, seconds};
        notify(state, [value](ApplicationRuntime* target) { emit target->detailProgressChanged(value); }, QStringLiteral("detail-progress"));
      });
      QObject::connect(services.refresh, &PetRefreshController::detailRequestFinished, services.root,
          [state, services](qint64 instance, bool succeeded, const QString& reason) {
        const QString account = services.repo->accountKey(); const quint64 epoch = services.repo->sessionGeneration();
        notify(state, [account, epoch, instance, succeeded, reason](ApplicationRuntime* target) {
          emit target->detailRequestFinished(account, epoch, instance, succeeded, reason);
        });
      });
      QObject::connect(services.refresh, &PetRefreshController::moveRunningChanged, services.root,
          [state, services, views](bool running) {
        if (running) {
          views->move = {services.repo->accountKey(), services.repo->sessionGeneration(),
                         services.refresh->currentMoveTaskId(), {}, true, MoveOutcome::NotSent, {}};
        } else views->move.running = false;
        const auto value = views->move;
        notify(state, [value](ApplicationRuntime* target) { emit target->moveStateChanged(value); });
      });
      QObject::connect(services.refresh, &PetRefreshController::moveOutcomeChanged, services.root,
          [state, services, views](const QString& operation, const QString& account, MoveOutcome outcome, const QString& text) {
        views->move.account = account; views->move.operationId = operation; views->move.outcome = outcome;
        views->move.message = text; views->move.running = services.refresh->moveRunning();
        const auto value = views->move;
        notify(state, [value](ApplicationRuntime* target) { emit target->moveStateChanged(value); });
      });
      QObject::connect(services.refresh, &PetRefreshController::replacementSelectionRequired, services.root,
          [state](quint64 task, const QString& account, quint64 epoch, qint64 incoming, const QList<qint64>& eligible) {
        notify(state, [task, account, epoch, incoming, eligible](ApplicationRuntime* target) {
          emit target->replacementSelectionRequired(task, account, epoch, incoming, eligible);
        });
      });
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->services = services;
        state->receiver = services.root;
      }
      initialized = true;
      shop(); routine(); services.refresh->publishState();
      if (!state->closing.load(std::memory_order_acquire))
        notify(state, [state, images = services.images](ApplicationRuntime* target) {
          if (!state->closing.load(std::memory_order_acquire)) {
            QPointer<ApplicationRuntime> alive(target);
            emit target->imageServiceReady(images);
            if (alive && !state->closing.load(std::memory_order_acquire)) emit target->ready();
          }
        });
      else QTimer::singleShot(0, services.root, [state, services, this] { closeCore(state, services, this); });
      exec();
    } catch (const std::exception& error) {
      const QString message = QString::fromUtf8(error.what());
      notify(state, [message](ApplicationRuntime* target) { emit target->initializationFailed(message); });
    } catch (...) {
      notify(state, [](ApplicationRuntime* target) { emit target->initializationFailed(QStringLiteral("Core initialization failed")); });
    }
    bool clean = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->receiver = nullptr;
      clean = initialized && state->clean;
    }
    if (!initialized) {
      state->closing.store(true, std::memory_order_release); revoke(state);
      if (services.dataUpdater) services.dataUpdater->close();
      if (services.cacheManager) services.cacheManager->close();
      if (services.starStatistics) services.starStatistics->close();
      if (services.images) services.images->shutdown();
      if (services.catalogs) services.catalogs->close();
      if (!state->shutdownDeadline.load()) state->shutdownDeadline.store(transportMonotonicMs() + 2000);
      qint64 remaining = qMax<qint64>(0, state->shutdownDeadline.load() - transportMonotonicMs());
      const bool computeClean = !services.analysis || services.analysis->shutdownAnalysis(int(qMin<qint64>(remaining, 2000)));
      if (services.storage) DiagnosticLogger::closeStorage(services.storage);
      remaining = qMax<qint64>(0, state->shutdownDeadline.load() - transportMonotonicMs());
      const bool storageClean = !services.storage || services.storage->shutdown(static_cast<unsigned long>(qMin<qint64>(remaining, 2000)));
      clean = computeClean && storageClean;
    }
    if (!state->closing.exchange(true)) revoke(state);
    if (clean) {
      delete analysisPublisher; delete inventoryPublisher;
      delete services.images;
      delete services.dataUpdater;
      delete services.cacheManager;
      delete services.starStatistics;
      delete services.catalogs;
      delete services.derivations;
      delete services.details;
      delete services.analysis; delete services.routine; delete services.shop;
      delete services.refresh; delete services.repo; delete services.storage; delete services.root;
    }
    // If an I/O operation exceeded the shared deadline, retain its Core object
    // graph and leases until process exit. No live QThread is destructed.
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->services = {};
      state->finished = true; state->clean = clean;
    }
    notify(state, [clean](ApplicationRuntime* target) { emit target->stopped(clean); });
  }
private:
  std::shared_ptr<State> state_;
};
}

using namespace ApplicationRuntimeInternal;

ApplicationRuntime::ApplicationRuntime(RuntimeOptions options, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>(std::move(options))) {
  state_->facade = this;
  qRegisterMetaType<OutboundIntent>(); qRegisterMetaType<SendReceipt>();
  qRegisterMetaType<RuntimeStatus>(); qRegisterMetaType<RuntimeShopState>(); qRegisterMetaType<RuntimeRoutineState>();
  qRegisterMetaType<RuntimeDetailProgress>(); qRegisterMetaType<RuntimeMoveState>();
  if (state_->options.inventoryProjection) {
    connect(state_->options.inventoryProjection, &InventoryProjection::detailSelectionChanged, this,
        [this](const QString& account, quint64 epoch, int consumer, qint64 id) {
      post(account, epoch, [consumer, id](const CoreServices& services) { services.inventoryPublisher->selectDetail(consumer, id); });
    });
    connect(state_->options.inventoryProjection, &InventoryProjection::detailPageRequested, this,
        [this](const QString& account, quint64 epoch, int consumer, DetailSection section, int pageIndex) {
      post(account, epoch, [consumer, section, pageIndex](const CoreServices& services) {
        services.inventoryPublisher->requestDetailPage(consumer, section, pageIndex);
      });
    });
    connect(state_->options.inventoryProjection, &InventoryProjection::detailInterestsChanged, this,
        [this](const QString& account, quint64 epoch, const QSet<qint64>& ids) {
      post(account, epoch, [ids](const CoreServices& services) { services.inventoryPublisher->setDetailInterests(ids); });
    });
  }
  if (state_->options.analysisProjection) {
    const auto scoped = [this](CoreAction action) {
      const auto* projection = state_->options.analysisProjection;
      post(projection->accountKey(), projection->sessionEpoch(), std::move(action));
    };
    connect(state_->options.analysisProjection, &AnalysisProjection::analysisRequested, this,
            [scoped] { scoped([](const CoreServices& services) { services.analysis->requestAnalysis(); }); });
    connect(state_->options.analysisProjection, &AnalysisProjection::analysisCancellationRequested, this,
            [scoped] { scoped([](const CoreServices& services) { services.analysis->cancelAnalysis(); }); });
    connect(state_->options.analysisProjection, &AnalysisProjection::snapshotRequested, this,
            [scoped] { scoped([](const CoreServices& services) { services.analysis->requestSnapshot(); }); });
    connect(state_->options.analysisProjection, &AnalysisProjection::autoSnapshotChangeRequested, this,
            [scoped](bool enabled) { scoped([enabled](const CoreServices& services) { services.analysis->setAutoSnapshotEnabled(enabled); }); });
    connect(state_->options.analysisProjection, &AnalysisProjection::historyRequested, this,
            [scoped](const QDateTime& before) { scoped([before](const CoreServices& services) { services.analysis->requestSnapshotHistory(before); }); });
    connect(state_->options.analysisProjection, &AnalysisProjection::snapshotDetailsRequested, this,
            [scoped](const QString& key) { scoped([key](const CoreServices& services) { services.analysis->requestSnapshotDetails(key); }); });
    connect(state_->options.analysisProjection, &AnalysisProjection::instanceHistoryRequested, this,
            [scoped](qint64 id) { scoped([id](const CoreServices& services) { services.analysis->requestInstanceHistory(id); }); });
  }
}

ApplicationRuntime::~ApplicationRuntime() {
  Q_ASSERT(QThread::currentThread() == thread());
  shutdown();
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->facade = nullptr;
  state_->guiActions.clear();
}

bool ApplicationRuntime::start() {
  if (QThread::currentThread() != thread() || !QDir::isAbsolutePath(state_->options.dataRoot) ||
      state_->options.maximumQueuedTasks <= 0 || state_->options.maximumQueuedBytes <= 256 ||
      !state_->options.inventoryProjection || !state_->options.analysisProjection ||
      state_->options.inventoryProjection->thread() != thread() || state_->options.analysisProjection->thread() != thread()) return false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->started || state_->closing.load()) return false;
    state_->started = true;
  }
  auto* worker = new CoreThread(state_);
  thread_ = worker;
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
  return true;
}

bool ApplicationRuntime::enqueue(const QString& account, quint64 epoch, bool scoped,
                                 CoreAction action, qint64 bytes, bool packet) {
  if (!action || bytes < 0 || !inputValid(state_)) return false;
  const auto state = state_;
  bool full = false;
  bool posted = false;
  std::shared_ptr<Reservation> reservation;
  try { reservation = std::make_shared<Reservation>(state, bytes); }
  catch (...) { return false; }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->receiver || state->closing.load()) return false;
    full = state->queuedTasks >= state->options.maximumQueuedTasks ||
           state->options.maximumQueuedBytes < bytes || state->queuedBytes > state->options.maximumQueuedBytes - bytes;
    if (!full) {
      ++state->queuedTasks; state->queuedBytes += bytes;
      reservation->armed = true;
      try { posted = QMetaObject::invokeMethod(state->receiver, [state, reservation, account, epoch, scoped, action = std::move(action)] {
        if (!inputValid(state)) {
          if (!state->closing.load()) state->services.repo->markSessionUncertain(QStringLiteral("入站来源或队列完整性已失效"));
          return;
        }
        const CoreServices services = state->services;
        if (scoped && (services.repo->accountKey() != account || services.repo->sessionGeneration() != epoch)) {
          notify(state, [account, epoch](ApplicationRuntime* target) {
            emit target->commandRejected(account, epoch, QStringLiteral("用户动作所属账号或会话已经变化"));
          }, QStringLiteral("rejected-command"));
          return;
        }
        try { action(services); }
        catch (...) {
          notify(state, [account, epoch](ApplicationRuntime* target) {
            emit target->commandRejected(account, epoch, QStringLiteral("Core用例执行失败"));
          }, QStringLiteral("rejected-command"));
        }
      }, Qt::QueuedConnection); }
      catch (...) { posted = false; }
    }
  }
  if (full && packet && !state->inputUncertain.exchange(true, std::memory_order_acq_rel)) revoke(state);
  return posted;
}

bool ApplicationRuntime::post(const QString& account, quint64 epoch, CoreAction action) {
  return enqueue(account, epoch, true, std::move(action));
}
bool ApplicationRuntime::postUnscoped(CoreAction action) { return enqueue({}, 0, false, std::move(action)); }
bool ApplicationRuntime::deliverPacket(const InboundEnvelope& envelope) {
  return enqueue({}, 0, false, [envelope](const CoreServices& services) { services.repo->handleEnvelope(envelope); },
                 envelope.payload.size() * 2 + envelope.method.size() * 2 + 256, true);
}
bool ApplicationRuntime::deliverReceipt(const SendReceipt& receipt) {
  return postUnscoped([receipt](const CoreServices& services) {
    services.refresh->handleSendReceipt(receipt);
    services.shop->handleSendReceipt(receipt);
    services.routine->handleSendReceipt(receipt);
  });
}
bool ApplicationRuntime::closing() const { return state_->closing.load(std::memory_order_acquire); }

void ApplicationRuntime::requestDataUpdate() {
  const auto state = state_;
  if (!postUnscoped([state](const CoreServices& services) {
    if (services.cacheManager->busy() || state->imageBatchRunning || !services.dataUpdater->requestUpdate()) {
      const bool busy = services.dataUpdater->busy();
      notify(state, [busy](ApplicationRuntime* target) {
        emit target->dataUpdateStatusChanged(QStringLiteral("已有数据更新、补图或缓存管理任务，请完成后再检查。"), busy);
      });
    }
  })) emit dataUpdateStatusChanged(QStringLiteral("当前无法开始数据更新，请稍后重试。"), false);
}

void ApplicationRuntime::requestCacheAction(const QString& action, const QJsonObject& options) {
  const auto state = state_;
  if (!postUnscoped([state, action, options](const CoreServices& services) {
    const auto fail = [state, action](const QString& message) {
      const QJsonObject result{{QStringLiteral("ok"), false}, {QStringLiteral("message"), message}};
      notify(state, [action, result](ApplicationRuntime* target) { emit target->cacheActionFinished(action, result); });
    };
    const bool inspect = action == QStringLiteral("inspect");
    const auto imageState = services.images->memoryUsage();
    if (!inspect && (services.dataUpdater->busy() || state->imageBatchRunning ||
        services.refresh->listRefreshRunning() || services.refresh->detailBatchRunning() || services.refresh->moveRunning() ||
        services.shop->isRunning() || services.routine->isRunning() || services.analysis->analysisRunning() ||
        services.storage->state().outstandingTasks > 0 || imageState.downloads > 0 || imageState.waiting > 0 || imageState.decoding)) {
      fail(QStringLiteral("刷新、保存或图片任务尚未结束，请完成后再管理缓存。")); return;
    }
    QJsonObject request = options;
    if (inspect) {
      QJsonArray keys; QSet<QString> seen;
      QList<QJsonObject> briefs = services.repo->backpackBriefs();
      briefs.append(services.repo->warehouseBriefs());
      for (const auto& pet : briefs) {
        const auto key = ImageService::storageKey(petVisualKey(pet));
        if (!key.isEmpty() && !seen.contains(key)) { keys.append(key); seen.insert(key); }
      }
      request.insert(QStringLiteral("imageKeys"), keys);
    }
    if (!services.cacheManager->request(action, request)) fail(QStringLiteral("已有缓存管理任务，或请求无法开始，请稍后重试。"));
  })) emit cacheActionFinished(action, {{QStringLiteral("ok"), false}, {QStringLiteral("message"), QStringLiteral("当前无法开始缓存管理，请稍后重试。")}});
}

void ApplicationRuntime::requestMissingImages() {
  const auto state = state_;
  if (!postUnscoped([state](const CoreServices& services) {
    if (services.cacheManager->busy() || services.dataUpdater->busy() || state->imageBatchRunning) {
      const bool batchRunning = state->imageBatchRunning, updateBusy = services.dataUpdater->busy();
      notify(state, [batchRunning, updateBusy](ApplicationRuntime* target) {
        if (!batchRunning) emit target->imageBatchFinished(true, 0);
        emit target->dataUpdateStatusChanged(QStringLiteral("请先完成当前数据更新、补图或缓存管理任务。"), updateBusy);
      });
      return;
    }
    QList<ImageRequest> requests; QSet<QString> seen;
    QList<QJsonObject> briefs = services.repo->backpackBriefs();
    briefs.append(services.repo->warehouseBriefs());
    for (const auto& pet : briefs) {
      ImageRequest request; request.visualKey = petVisualKey(pet);
      const auto key = ImageService::storageKey(request.visualKey);
      if (key.isEmpty() || petRaceId(pet) <= 0 || seen.contains(key)) continue;
      seen.insert(key); request.candidateNames = {petProtocolName(pet)};
      request.selected = false; request.retry = true; requests.append(request);
    }
    state->imageBatchRunning = true;
    services.images->requestBatch(requests);
  })) emit imageBatchFinished(true, 0);
}

void ApplicationRuntime::pauseImageBatch(bool paused) {
  postUnscoped([paused](const CoreServices& services) { services.images->pauseBatch(paused); });
}
void ApplicationRuntime::cancelImageBatch() {
  postUnscoped([](const CoreServices& services) { services.images->cancelBatch(); });
}

void ApplicationRuntime::requestLocalStargodStatistics() {
  const auto state = state_;
  const auto snapshot = state->options.inventoryProjection
      ? state->options.inventoryProjection->snapshot() : nullptr;
  const QString account = snapshot ? snapshot->account : QString{};
  const quint64 epoch = snapshot ? snapshot->sessionEpoch : 0;
  if (!post(account,epoch,[state](const CoreServices& services) {
    if (!services.starStatistics->request(services.repo->storageContext(),services.repo->sessionGeneration(),
        PetDetailCatalog::instance().stargodDefinitions())) {
      LocalStargodStatistics result; result.account = services.repo->accountKey(); result.epoch = services.repo->sessionGeneration();
      result.error = QStringLiteral("本地统计暂时无法开始，请稍后重试");
      notify(state,[result](ApplicationRuntime* target) { emit target->localStargodStatisticsChanged(result); });
    }
  })) {
    LocalStargodStatistics result; result.account = account; result.epoch = epoch;
    result.error = QStringLiteral("当前账号本地缓存尚未就绪"); emit localStargodStatisticsChanged(result);
  }
}
void ApplicationRuntime::cancelLocalStargodStatistics() {
  postUnscoped([](const CoreServices& services) { services.starStatistics->cancel(); });
}

bool ApplicationRuntime::shutdown(unsigned long waitMilliseconds) {
  if (QThread::currentThread() != thread()) return false;
  const qint64 now = transportMonotonicMs();
  const auto budget = qMin<unsigned long>(waitMilliseconds, 2000);
  if (!state_->closing.exchange(true, std::memory_order_acq_rel)) {
    state_->shutdownDeadline.store(now + budget, std::memory_order_release);
    revoke(state_); // Independent atomic permit revocation precedes the Core queue.
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->receiver) {
      const auto state = state_; const auto services = state->services;
      QThread* worker = thread_.data();
      QMetaObject::invokeMethod(state->receiver, [state, services, worker] { closeCore(state, services, worker); }, Qt::QueuedConnection);
    } else if (!state_->started) { state_->finished = true; state_->clean = true; }
  }
  if (thread_ && thread_ != QThread::currentThread()) {
    const qint64 remaining = qMax<qint64>(0, state_->shutdownDeadline.load(std::memory_order_acquire) - transportMonotonicMs());
    if (!thread_->wait(static_cast<unsigned long>(qMin<qint64>(remaining, budget)))) return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->finished && state_->clean;
}
