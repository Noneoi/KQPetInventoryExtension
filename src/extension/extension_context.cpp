#include "extension_context.h"
#include "application/views/analysis_projection.h"
#include "application/analysis/asset_analysis_controller.h"
#include "ui/analysis/asset_analysis_window.h"
#include "diagnostics/diagnostic_logger.h"
#include "application/views/inventory_projection.h"
#include "bridge/original_bridge.h"
#include "bridge/original_window_locator.h"
#include "application/pet/pet_repository.h"
#include "ui/workbench/pet_settings_dialog.h"
#include "ui/pet/pet_window.h"
#include "ui/common/pet_image_cache.h"
#include "ui/common/ui_preferences.h"
#include "application/routine/routine_overview_controller.h"
#include "ui/routine/routine_overview_window.h"
#include "application/shop/shop_exchange_controller.h"
#include "application/catalog/shop_exchange_catalog.h"
#include "ui/shop/shop_window.h"
#include "runtime/startup_channel.h"
#include "diagnostics/target_compatibility_guard.h"
#include "ui/workbench/workbench_window.h"
#include "version.h"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

ExtensionContext* ExtensionContext::instance_ = nullptr;

void ExtensionContext::prepareBridge(ReadyCallback callback) {
  if (OriginalBridge::closing()) { callback(nullptr); return; }
  if (!instance_) instance_ = new ExtensionContext;
  if (instance_->coreReady_) callback(instance_->bridge_);
  else if (!instance_->initializationError_.isEmpty()) callback(nullptr);
  else instance_->readyCallbacks_.push_back(std::move(callback));
}
void ExtensionContext::start(std::shared_ptr<kqpet::startup::Channel> channel) {
  if (!instance_ || !instance_->coreReady_ || OriginalBridge::closing() || instance_->runtime_->closing()) {
    if (channel) channel->publish(kqpet::startup::State::Failed, ERROR_SHUTDOWN_IN_PROGRESS);
    return;
  }
  instance_->startupChannel_ = std::move(channel);
  instance_->locator_->start();
}

ExtensionContext::ExtensionContext(QObject* parent) : QObject(parent) {
  QString dataRoot = qEnvironmentVariable("KQPET_DATA_ROOT");
  if (dataRoot.isEmpty()) dataRoot = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("KQPetData"));
  UiPreferences::setFilePath(QDir(dataRoot).filePath(QStringLiteral("ui-preferences.ini")));
  inventory_ = new InventoryProjection(dataRoot, this);
  analysis_ = new AnalysisProjection(this);
  images_ = new PetImageCache(dataRoot, this);
  bridge_ = new OriginalBridge(this);
  outbound_ = std::make_shared<OutboundQueue>();
  const auto outgoing = outbound_;
  bridge_->setOverflowHandler([outgoing] { outgoing->revokeAll(); });
  RuntimeOptions options;
  options.dataRoot = dataRoot;
  options.clientRoot = qEnvironmentVariable("KQPET_CLIENT_ROOT", QCoreApplication::applicationDirPath());
  options.buildIdentity = QString::fromLatin1(KQPET_RELEASE_ID);
  options.imageResourceVersion = QString::fromLatin1(KQPET_IMAGE_RESOURCE_VERSION);
  options.profileIdentity = QString::fromStdWString(TargetCompatibilityGuard::lastReport().targetProfileId);
  options.compatibilityVerified = TargetCompatibilityGuard::lastReport().supported;
  options.inventoryProjection = inventory_;
  options.analysisProjection = analysis_;
  options.sender = [this](const OutboundIntent& intent) { return enqueueOutbound(intent); };
  options.inputValid = [bridge = bridge_] { return bridge->captureHealthy() && !OriginalBridge::closing(); };
  options.revokePending = [outgoing] { outgoing->revokeAll(); };
  runtime_ = new ApplicationRuntime(std::move(options), this);
  connect(runtime_, &ApplicationRuntime::imageServiceReady, this, [this](ImageService* service) {
    images_->setService(service, QString::fromLatin1(KQPET_IMAGE_RESOURCE_VERSION));
  });
  connect(runtime_, &ApplicationRuntime::ready, this, [this] { finishCoreInitialization(true); });
  connect(runtime_, &ApplicationRuntime::dataUpdateStatusChanged, this, [this](const QString& message, bool busy) {
    if (workbench_) workbench_->setTask({message, busy});
    if (shopPage_) shopPage_->setStatus(message);
  });
  connect(runtime_, &ApplicationRuntime::cacheActionFinished, this, [this](const QString&, const QJsonObject& result) {
    if (workbench_) workbench_->setTask({result.value(QStringLiteral("message")).toString(), false});
  });
  connect(runtime_, &ApplicationRuntime::initializationFailed, this,
          [this](const QString& error) { finishCoreInitialization(false, error); });
  connect(bridge_, &OriginalBridge::packetCaptured, this, [this](const InboundEnvelope& envelope) {
    if (!runtime_->deliverPacket(envelope)) outbound_->revokeAll();
  });
  connect(bridge_, &OriginalBridge::captureUncertain, this, [this](const QString& reason) {
    outbound_->revokeAll();
    runtime_->postUnscoped([reason](const CoreServices& core) { core.repo->markSessionUncertain(reason); });
    if (workbench_) workbench_->setTask({reason, false});
  });
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
    bridge_->disableCapture();
    outbound_->revokeAll();
    clearReplacement();
    if (workbench_) workbench_->saveUiPreferences();
    UiPreferences::sync();
    runtime_->shutdown(2000);
  }, Qt::DirectConnection);
  connect(QCoreApplication::instance(), &QObject::destroyed, bridge_, &OriginalBridge::disableCapture,
          Qt::DirectConnection);
  connect(inventory_, &InventoryReadView::accountSessionChanged, this, [this] {
    clearReplacement(); replacementTask_ = 0;
    if (!matches(listAccount_, listEpoch_)) listRunning_ = false;
    if (!matches(analysisAccount_, analysisEpoch_)) analysisRunning_ = false;
    if (!matches(detailState_.account, detailState_.epoch)) detailState_ = {};
    if (!matches(moveState_.account, moveState_.epoch)) moveState_ = {};
    refreshSessionDisplay(); refreshPageStates(); refreshPersistenceDisplay();
  });
  connect(inventory_, &InventoryReadView::dataChanged, this, [this] { refreshSessionDisplay(); });
  connect(runtime_, &ApplicationRuntime::commandRejected, this,
          [this](const QString& account, quint64 epoch, const QString& reason) {
    if (workbench_ && matches(account, epoch)) workbench_->setTask({reason, false});
  });
  connect(runtime_, &ApplicationRuntime::statusChanged, this, [this](const RuntimeStatus& status) {
    if (!matches(status.account, status.epoch)) return;
    if (petPage_ && status.module == QStringLiteral("inventory")) petPage_->setStatus(status.message);
    if (shopPage_ && (status.module == QStringLiteral("shop") || status.module == QStringLiteral("inventory")))
      shopPage_->setStatus(status.message);
    if (routinePage_ && status.module == QStringLiteral("routine")) routinePage_->setStatus(status.message);
    const bool detailsCurrent = matches(detailState_.account, detailState_.epoch);
    if (workbench_) workbench_->setTask({status.message,
        (matches(listAccount_, listEpoch_) && listRunning_) || (detailsCurrent && detailState_.running) ||
        (matches(analysisAccount_, analysisEpoch_) && analysisRunning_) ||
        (matches(moveState_.account, moveState_.epoch) && moveState_.running),
        detailsCurrent ? detailState_.completed : 0, detailsCurrent ? detailState_.total : 0});
  });
  connect(runtime_, &ApplicationRuntime::shopStateChanged, this, [this](const RuntimeShopState& state) {
    shopState_ = state; refreshPageStates();
  });
  connect(runtime_, &ApplicationRuntime::localStargodStatisticsChanged, this, [this](const LocalStargodStatistics& result) {
    if (assetPage_ && matches(result.account,result.epoch)) assetPage_->setLocalStargodStatistics(result);
  });
  connect(runtime_, &ApplicationRuntime::routineStateChanged, this, [this](const RuntimeRoutineState& state) {
    routineState_ = state; refreshPageStates();
  });
  connect(runtime_, &ApplicationRuntime::listRefreshRunningChanged, this,
          [this](const QString& account, quint64 epoch, bool running) {
    listAccount_ = account; listEpoch_ = epoch; listRunning_ = running;
    if (petPage_) petPage_->setListRefreshRunning(matches(account, epoch) && running);
  });
  connect(runtime_, &ApplicationRuntime::analysisRunningChanged, this,
      [this](const QString& account, quint64 epoch, bool running) {
    analysisAccount_ = account; analysisEpoch_ = epoch; analysisRunning_ = running;
    if (workbench_ && matches(account, epoch))
      workbench_->setTask({running ? QStringLiteral("正在计算本地分析……") : QStringLiteral("本地分析任务已结束"), running});
  });
  connect(runtime_, &ApplicationRuntime::detailProgressChanged, this, [this](const RuntimeDetailProgress& state) {
    detailState_ = state; refreshPageStates();
  });
  connect(runtime_, &ApplicationRuntime::detailRequestFinished, this,
      [this](const QString& account, quint64 epoch, qint64 id, bool succeeded, const QString& reason) {
    if (shopPage_ && matches(account, epoch)) shopPage_->finishDetailRefresh(id, succeeded, reason);
  });
  connect(runtime_, &ApplicationRuntime::moveStateChanged, this, [this](const RuntimeMoveState& state) {
    moveState_ = state;
    if (!state.running) {
      if (replacementTask_ == state.moveTaskId) { clearReplacement(); replacementTask_ = 0; }
    }
    refreshPageStates();
  });
  connect(runtime_, &ApplicationRuntime::replacementSelectionRequired, this,
          [this](quint64 task, const QString& account, quint64 epoch, qint64 incoming, const QList<qint64>& ids) {
    if (!matches(account, epoch)) return;
    clearReplacement();
    replacementTask_ = task; replacementAccount_ = account; replacementEpoch_ = epoch;
    replacementIncoming_ = incoming; replacementIds_ = ids;
    if (workbench_ && workbench_->isVisible()) showReplacement();
  });
  connect(runtime_, &ApplicationRuntime::persistenceChanged, this, [this](const RuntimePersistenceState& state) {
    persistenceState_ = state;
    refreshPersistenceDisplay();
  });
  locator_ = new OriginalWindowLocator(this);
  connect(locator_, &OriginalWindowLocator::windowChanged, this, [this] { attachToOriginalWindow(); });
  connect(locator_, &OriginalWindowLocator::layoutNeeded, this, [this] { attachToOriginalWindow(); });
  if (!runtime_->start()) finishCoreInitialization(false, QStringLiteral("Core线程未能启动"));
}

void ExtensionContext::finishCoreInitialization(bool ready, const QString& error) {
  coreReady_ = ready;
  initializationError_ = error;
  if (!ready && initializationError_.isEmpty()) initializationError_ = QStringLiteral("Core初始化失败");
  auto callbacks = std::move(readyCallbacks_);
  for (auto& callback : callbacks) callback(ready && !OriginalBridge::closing() ? bridge_ : nullptr);
  if (!ready && startupChannel_) startupChannel_->publish(kqpet::startup::State::Failed, ERROR_FUNCTION_FAILED);
}
bool ExtensionContext::matches(const QString& account, quint64 epoch) const {
  const auto snapshot = inventory_->snapshot();
  return snapshot && snapshot->account == account && snapshot->sessionEpoch == epoch;
}
bool ExtensionContext::postCurrent(CoreAction action) {
  const auto snapshot = inventory_->snapshot();
  return snapshot && runtime_->post(snapshot->account, snapshot->sessionEpoch, std::move(action));
}
bool ExtensionContext::enqueueOutbound(const OutboundIntent& intent) {
  if (OriginalBridge::closing() || !bridge_->captureHealthy() || runtime_->closing()) return false;
  if (!outbound_->tryPush(intent)) return false;
  scheduleOutboundDrain();
  return true;
}
void ExtensionContext::scheduleOutboundDrain() {
  if (!outboundScheduled_.exchange(true, std::memory_order_acq_rel))
    QMetaObject::invokeMethod(this, [this] { drainOutbound(); }, Qt::QueuedConnection);
}
void ExtensionContext::drainOutbound() {
  // No reviewed host connection/page identity is currently available, so no
  // Core account string is copied here as actual GUI source evidence. Reads run
  // as unverified observations; the pet move runs at the v1.3 level that Core
  // enforces before queueing it (see ActualSendSource::allowUnverifiedWrite).
  for (int count = 0; count < 8; ++count) {
    auto intent = outbound_->takeNext();
    if (!intent) break;
    ActualSendSource source;
    source.closing = OriginalBridge::closing() || runtime_->closing() || !bridge_->captureHealthy();
    source.allowUnverifiedRead = TargetCompatibilityGuard::lastReport().supported && !source.closing;
    source.allowUnverifiedWrite = source.allowUnverifiedRead;
    const SendReceipt receipt = executeIntent(*intent, source, transportMonotonicMs(), [this](const OutboundIntent& value) {
      return value.flashMethod.isEmpty()
          ? bridge_->sendSubmission(value.extension, value.ticket.command, value.parameters)
          : bridge_->invokeFlashSubmission(value.flashMethod, value.flashArgument);
    });
    outbound_->complete(intent->ticket.taskId);
    runtime_->deliverReceipt(receipt);
  }
  outboundScheduled_.store(false, std::memory_order_release);
  if (outbound_->outstanding() && !runtime_->closing()) scheduleOutboundDrain();
}

void ExtensionContext::attachToOriginalWindow() {
  if (OriginalBridge::closing() || runtime_->closing()) return;
  QWidget* window = locator_->window();
  if (!window) return;
  const QRect geometry = OriginalWindowLocator::singleEntryGeometry(window);
  if (geometry.isEmpty()) return;
  if (originalWindow_ != window || !entry_) {
    if (entry_) delete entry_.data();
    originalWindow_ = window;
    entry_ = new QPushButton(QStringLiteral("精灵扩展"), window);
    entry_->setObjectName(QStringLiteral("KQPetWorkbenchEntry"));
    entry_->setAttribute(Qt::WA_NativeWindow, true);
    entry_->setFocusPolicy(Qt::StrongFocus);
    entry_->setCursor(Qt::PointingHandCursor);
    entry_->setStyleSheet(QStringLiteral(
        "QPushButton#KQPetWorkbenchEntry{background:#2866a8;color:white;border:0;border-radius:5px;padding:0 8px;font-size:12px;}"
        "QPushButton#KQPetWorkbenchEntry:hover{background:#367bc1;}QPushButton#KQPetWorkbenchEntry:focus{border:2px solid #a6d0ff;}"));
    connect(entry_, &QPushButton::clicked, this, [this] { showWorkbench(); });
  }
  positionButton();
  if (startupChannel_ && entry_->isVisibleTo(window) && !OriginalBridge::closing())
    startupChannel_->publish(kqpet::startup::State::Ready);
}
void ExtensionContext::positionButton() {
  if (!originalWindow_ || !entry_) return;
  const QRect rect = OriginalWindowLocator::singleEntryGeometry(originalWindow_);
  if (rect.isEmpty()) { entry_->hide(); return; }
  entry_->setFixedSize(rect.size()); entry_->setGeometry(rect);
  entry_->show(); entry_->raise();
}

void ExtensionContext::ensureWorkbench() {
  if (workbench_) return;
  workbench_ = new WorkbenchWindow([this](WorkbenchPage page, QWidget* parent) { return createPage(page, parent); });
  workbench_->setTargetValidator([this](const NavigationTarget& target) {
    if (!matches(target.account, target.epoch)) return false;
    if (target.petInstanceId > 0 && inventory_->backpackPet(target.petInstanceId).isEmpty() &&
        inventory_->warehousePet(target.petInstanceId).isEmpty()) return false;
    if (!target.goodKey.isEmpty()) {
      bool exists = false;
      if (shopState_.catalog)
        for (const auto& shop : shopState_.catalog->allShops)
          for (const auto& good : shop.goods)
            if (good.isOnlineOn(shopState_.catalogDate) && good.stableKey() == target.goodKey) { exists = true; break; }
      if (!exists) return false;
    }
    return true;
  });
  workbench_->setTargetHandler([](QWidget* page, const NavigationTarget& target) {
    if (auto* pets = qobject_cast<PetWindow*>(page); pets && target.petInstanceId > 0) {
      pets->focusPet(target.petInstanceId); return true;
    }
    if (auto* shop = qobject_cast<ShopWindow*>(page); shop && !target.goodKey.isEmpty()) {
      shop->focusGood(target.goodKey); return true;
    }
    return target.petInstanceId <= 0 && target.goodKey.isEmpty();
  });
  connect(workbench_, &WorkbenchWindow::settingsRequested, this, [this] { showSettings(); });
  connect(workbench_, &WorkbenchWindow::diagnosticsRequested, this, [this] { copyDiagnostics(); });
  refreshSessionDisplay();
  refreshPersistenceDisplay();
  if (!workbench_->showPage(WorkbenchWindow::rememberedPage())) workbench_->showPage(WorkbenchPage::Pets);
}
void ExtensionContext::showWorkbench() {
  if (!coreReady_ || runtime_->closing()) return;
  ensureWorkbench();
  workbench_->show(); workbench_->raise(); workbench_->activateWindow();
  refreshPageStates(); showReplacement();
}
void ExtensionContext::openPage(WorkbenchPage page) {
  showWorkbench();
  if (workbench_) workbench_->showPage(page);
}
QWidget* ExtensionContext::createPage(WorkbenchPage page, QWidget* parent) {
  if (page == WorkbenchPage::Pets) {
    auto* widget = new PetWindow(inventory_, parent, images_); petPage_ = widget;
    connect(widget, &PetWindow::sidebarStatusChanged, workbench_, &WorkbenchWindow::setPageStatus);
    connect(widget, &PetWindow::sidebarDetailProgressChanged, workbench_, &WorkbenchWindow::setDetailTask);
    connect(widget, &PetWindow::listRefreshRequested, this, [this] { postCurrent([](const CoreServices& c) { c.refresh->requestManualListRefresh(); }); });
    connect(widget, &PetWindow::warehouseDetailRefreshRequested, this, [this](const QList<qint64>& instanceIds) {
      postCurrent([instanceIds](const CoreServices& c) { c.refresh->startWarehouseDetailRefreshForIds(instanceIds); });
    });
    connect(widget, &PetWindow::warehouseDetailPauseRequested, this, [this] { postCurrent([](const CoreServices& c) { c.refresh->pauseWarehouseDetailRefresh(); }); });
    connect(widget, &PetWindow::warehouseDetailResumeRequested, this, [this] { postCurrent([](const CoreServices& c) { c.refresh->resumeWarehouseDetailRefresh(); }); });
    connect(widget, &PetWindow::warehouseDetailCancelRequested, this, [this] { postCurrent([](const CoreServices& c) { c.refresh->cancelWarehouseDetailRefresh(); }); });
    connect(widget, &PetWindow::detailRequested, this, [this](qint64 id) { postCurrent([id](const CoreServices& c) { c.repo->requestCachedDetail(id); c.refresh->requestSingleDetail(id); }); });
    connect(widget, &PetWindow::cultivationMaterialsRefreshRequested, this, [this] {
      postCurrent([](const CoreServices& c) { c.shop->requestCultivationMaterials(); });
    });
    connect(widget, &PetWindow::moveToWarehouseRequested, this, [this](qint64 id) { requestMove(id, false); });
    connect(widget, &PetWindow::moveToBackpackRequested, this, [this](qint64 id) { requestMove(id, true); });
    connect(widget, &PetWindow::settingsRequested, this, [this] { showSettings(); });
    connect(widget, &PetWindow::copyDiagnosticsRequested, this, [this] { copyDiagnostics(); });
    refreshPageStates(); return widget;
  }
  if (page == WorkbenchPage::Shop) {
    auto* widget = new ShopWindow(inventory_, parent, images_); shopPage_ = widget;
    connect(widget, &ShopWindow::refreshRequested, this, [this] { postCurrent([](const CoreServices& c) { c.shop->requestInfo(); }); });
    // The shop button only needs the exchange catalog, not every public data part.
    connect(widget, &ShopWindow::catalogRefreshRequested, runtime_,
            [this] { runtime_->requestDataUpdate({QStringLiteral("shop")}); });
    connect(widget, &ShopWindow::detailRequested, this, [this](qint64 id) { postCurrent([id](const CoreServices& c) { c.repo->requestCachedDetail(id); c.refresh->requestSingleDetail(id); }); });
    connect(widget, &ShopWindow::moveToBackpackRequested, this, [this](qint64 id) { requestMove(id, true); });
    refreshPageStates(); return widget;
  }
  if (page == WorkbenchPage::Routine) {
    auto* widget = new RoutineOverviewWindow(parent); routinePage_ = widget;
    connect(widget, &RoutineOverviewWindow::refreshRequested, this, [this] { postCurrent([](const CoreServices& c) { c.routine->requestRefresh(); }); });
    refreshPageStates(); return widget;
  }
  auto* widget = new AssetAnalysisWindow(analysis_, parent); assetPage_ = widget;
  connect(widget,&AssetAnalysisWindow::localStargodStatisticsRequested,runtime_,&ApplicationRuntime::requestLocalStargodStatistics);
  connect(widget,&AssetAnalysisWindow::localStargodStatisticsCancelled,runtime_,&ApplicationRuntime::cancelLocalStargodStatistics);
  const auto navigate = [this](WorkbenchPage destination, qint64 id, const QString& good) {
    NavigationTarget target{destination, analysis_->accountKey(), analysis_->sessionEpoch(), id, good};
    ensureWorkbench(); workbench_->navigate(target);
  };
  connect(widget, &AssetAnalysisWindow::petRequested, this, [navigate](qint64 id) { navigate(WorkbenchPage::Pets, id, {}); });
  connect(widget, &AssetAnalysisWindow::shopRequested, this, [this] { openPage(WorkbenchPage::Shop); });
  connect(widget, &AssetAnalysisWindow::shopGoodRequested, this, [navigate](const QString& key) { navigate(WorkbenchPage::Shop, 0, key); });
  connect(widget, &AssetAnalysisWindow::routineRequested, this, [this] { openPage(WorkbenchPage::Routine); });
  return widget;
}
void ExtensionContext::refreshSessionDisplay() {
  if (!workbench_) return;
  const auto state = inventory_->snapshot();
  if (!state) return;
  QString source = state->account.isEmpty() ? QStringLiteral("等待游戏登录") : QStringLiteral("离线 · 浏览本地缓存");
  if (state->authenticated)
    source = state->sourceVerified ? QStringLiteral("在线 · 来源已确认")
        : QStringLiteral("在线只读 · 可刷新和分析，精灵移动暂未启用");
  else if (state->sessionState == SessionConnectionState::Uncertain) source = QStringLiteral("当前会话不可读取，请重新登录后刷新");
  else if (state->sessionState == SessionConnectionState::Authenticating) source = QStringLiteral("等待登录边界");
  else if (state->sessionState == SessionConnectionState::Closing) source = QStringLiteral("正在关闭");
  workbench_->setSession({state->account, state->sessionEpoch, source,
      QString::fromStdWString(TargetCompatibilityGuard::lastReport().kqProVersion), state->sourceVerified});
}
void ExtensionContext::refreshPageStates() {
  if (petPage_) {
    petPage_->setCultivationMaterials(matches(shopState_.account,shopState_.epoch) ? shopState_.cultivationMaterials : MaterialInventorySnapshot{});
    petPage_->setListRefreshRunning(matches(listAccount_, listEpoch_) && listRunning_);
    if (matches(detailState_.account, detailState_.epoch))
      petPage_->setDetailProgress(detailState_.running, detailState_.paused, detailState_.completed,
          detailState_.total, detailState_.succeeded, detailState_.failed, detailState_.instanceId, detailState_.estimatedSeconds);
    petPage_->setMoveRunning(matches(moveState_.account, moveState_.epoch) && moveState_.running);
  }
  if (shopPage_ && matches(shopState_.account, shopState_.epoch)) {
    shopPage_->setCatalogSnapshot(shopState_.catalog, shopState_.catalogDate);
    shopPage_->setPacket(shopState_.packet, shopState_.hasPacket);
    shopPage_->setMaterialCounts(shopState_.materialCounts, shopState_.hasMaterialCounts);
    shopPage_->setReadOnlyObservations(shopState_.unverifiedPackets);
    shopPage_->setQuotaValidity(shopState_.freshnessRevision, shopState_.quotaValidity);
    shopPage_->setStatus(shopState_.status.isEmpty() ? QStringLiteral("可刷新兑换次数与所需资源") : shopState_.status,
        shopState_.freshnessSummary + QStringLiteral("；目录日期 %1（本机时钟估计）").arg(shopState_.catalogDate.toString(Qt::ISODate))
        + (shopState_.status.isEmpty() ? QString{} : QStringLiteral("；") + shopState_.status));
    shopPage_->setRefreshRunning(shopState_.running);
    shopPage_->setMoveRunning(matches(moveState_.account, moveState_.epoch) && moveState_.running);
  }
  if (routinePage_ && matches(routineState_.account, routineState_.epoch)) {
    routinePage_->setCatalogSnapshot(routineState_.catalog);
    routinePage_->setPeriodValidity(routineState_.periodValidity);
    routinePage_->setData(routineState_.dailyPacket, routineState_.hasDailyPacket,
        routineState_.activeRedPoints, routineState_.hasRedPointPacket, routineState_.opportunityPackets);
    routinePage_->setReadOnlyObservations(routineState_.unverifiedPackets);
    routinePage_->setRunning(routineState_.running);
    routinePage_->setStatus(routineState_.freshnessSummary + (routineState_.status.isEmpty() ? QString{} : QStringLiteral("；") + routineState_.status));
  }
}
void ExtensionContext::refreshPersistenceDisplay() {
  if (!workbench_) return;
  const auto& state = persistenceState_;
  if (!matches(state.account, state.epoch)) { workbench_->setPersistence({}); return; }
  PersistenceState status = PersistenceState::Unknown;
  if (state.failedRecords || state.failuresTruncated) status = PersistenceState::Failed;
  else if (state.pendingWrites) status = PersistenceState::Pending;
  else if (state.hasSaved) status = PersistenceState::Saved;
  const QString detail = QStringLiteral("后台待写入 %1；失败记录 %2%3\n%4")
      .arg(state.pendingWrites).arg(state.failedRecords).arg(state.failuresTruncated ? QStringLiteral("以上") : QString{})
      .arg(state.error.isEmpty() ? state.record : state.error);
  workbench_->setPersistence({status, detail});
}
void ExtensionContext::requestMove(qint64 id, bool toBackpack) {
  if (id <= 0) return;
  postCurrent([id, toBackpack](const CoreServices& c) {
    if (toBackpack) c.refresh->requestMoveToBackpack(id);
    else c.refresh->requestMoveToWarehouse(id);
  });
}
void ExtensionContext::clearReplacement() {
  if (!replacement_) return;
  QSignalBlocker blocker(replacement_);
  replacement_->reject(); replacement_->deleteLater(); replacement_.clear();
}
void ExtensionContext::showReplacement() {
  if (!replacementTask_ || !workbench_ || !workbench_->isVisible() || replacement_ ||
      !matches(replacementAccount_, replacementEpoch_)) return;
  auto* dialog = new QDialog(workbench_); replacement_ = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("选择入库精灵"));
  dialog->setWindowModality(Qt::WindowModal);
  auto* layout = new QVBoxLayout(dialog);
  auto* text = new QLabel(QStringLiteral("背包已满。请选择一只入库，与实例 %1 交换；提交前会再次核验账号和列表。")
                             .arg(replacementIncoming_), dialog);
  text->setWordWrap(true); layout->addWidget(text);
  auto* table = new QTableWidget(replacementIds_.size(), 3, dialog);
  table->setHorizontalHeaderLabels({QStringLiteral("精灵"), QStringLiteral("实例"), QStringLiteral("等级")});
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  for (qsizetype row = 0; row < replacementIds_.size(); ++row) {
    const qint64 id = replacementIds_.at(row); const auto pet = inventory_->backpackPet(id);
    auto* name = new QTableWidgetItem(pet.value(QStringLiteral("n")).toString()); name->setData(Qt::UserRole, id);
    table->setItem(row, 0, name);
    table->setItem(row, 1, new QTableWidgetItem(QString::number(id)));
    table->setItem(row, 2, new QTableWidgetItem(pet.value(QStringLiteral("lv")).toVariant().toString()));
  }
  layout->addWidget(table);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Ok)->setEnabled(false); layout->addWidget(buttons);
  connect(table, &QTableWidget::itemSelectionChanged, dialog, [table, buttons] {
    buttons->button(QDialogButtonBox::Ok)->setEnabled(table->currentRow() >= 0);
  });
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  const quint64 task = replacementTask_, epoch = replacementEpoch_;
  const QString account = replacementAccount_;
  connect(dialog, &QDialog::finished, this, [this, table, task, epoch, account](int result) {
    const qint64 id = result == QDialog::Accepted && table->currentRow() >= 0
        ? table->item(table->currentRow(), 0)->data(Qt::UserRole).toLongLong() : 0;
    runtime_->post(account, epoch, [task, id](const CoreServices& c) {
      if (c.refresh->currentMoveTaskId() != task) return;
      if (id > 0) c.refresh->chooseMoveReplacement(id); else c.refresh->cancelMove();
    });
    if (replacementTask_ == task) replacementTask_ = 0;
  });
  connect(inventory_, &InventoryReadView::accountSessionChanged, dialog, &QDialog::reject);
  const QSize screen = workbench_->screen()->availableGeometry().size() - QSize(32, 64);
  dialog->resize(QSize(560, 360).boundedTo(screen));
  dialog->open();
}
void ExtensionContext::showSettings() {
  if (!workbench_ || !workbench_->isVisible() || settingsPending_) return;
  settingsPending_ = true;
  if (!runtime_->postUnscoped([this](const CoreServices& c) {
    const auto timings = c.refresh->timings();
    const bool ready = c.refresh->timingsKnown() && !c.refresh->timingsPending();
    const QString error = c.refresh->timingsStorageError();
    QMetaObject::invokeMethod(this, [this, timings, ready, error] {
      settingsPending_ = false;
      if (!workbench_ || !workbench_->isVisible() || runtime_->closing()) return;
      if (!ready) {
        workbench_->setTask({error.isEmpty() ? QStringLiteral("设置正在读取或保存，请稍后打开。")
                                            : QStringLiteral("设置尚未确认：%1").arg(error), false});
        return;
      }
      auto* dialog = new PetSettingsDialog(timings, workbench_);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->setCacheRoot(inventory_->dataRoot());
      connect(dialog, &PetSettingsDialog::cacheActionRequested, runtime_, &ApplicationRuntime::requestCacheAction);
      connect(dialog, &PetSettingsDialog::dataUpdateRequested, runtime_, &ApplicationRuntime::requestDataUpdate);
      connect(dialog, &PetSettingsDialog::missingImagesRequested, runtime_, &ApplicationRuntime::requestMissingImages);
      connect(dialog, &PetSettingsDialog::imageBatchPauseRequested, runtime_, &ApplicationRuntime::pauseImageBatch);
      connect(dialog, &PetSettingsDialog::imageBatchCancelRequested, runtime_, &ApplicationRuntime::cancelImageBatch);
      connect(runtime_, &ApplicationRuntime::cacheActionFinished, dialog, &PetSettingsDialog::applyCacheResult);
      connect(runtime_, &ApplicationRuntime::dataUpdateStatusChanged, dialog, &PetSettingsDialog::setDataUpdateStatus);
      connect(runtime_, &ApplicationRuntime::imageBatchProgress, dialog, &PetSettingsDialog::setImageBatchProgress);
      connect(runtime_, &ApplicationRuntime::imageBatchFinished, dialog, &PetSettingsDialog::finishImageBatch);
      connect(dialog, &QDialog::accepted, this, [this, dialog] {
        const auto selected = dialog->timings();
        runtime_->postUnscoped([selected](const CoreServices& core) { core.refresh->setTimings(selected); });
      });
      dialog->open();
      runtime_->requestCacheAction(QStringLiteral("inspect"));
    }, Qt::QueuedConnection);
  })) settingsPending_ = false;
}
void ExtensionContext::copyDiagnostics() {
  QApplication::clipboard()->setText(DiagnosticLogger::diagnosticText());
  if (workbench_) workbench_->setTask({QStringLiteral("本机诊断已复制。"), false});
}
