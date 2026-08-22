#include "extension_context.h"

#include "diagnostic_logger.h"
#include "original_bridge.h"
#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "pet_settings_dialog.h"
#include "pet_window.h"
#include "shop_exchange_controller.h"
#include "shop_window.h"
#include "routine_overview_controller.h"
#include "routine_overview_window.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

ExtensionContext* ExtensionContext::instance_ = nullptr;

void ExtensionContext::start() {
  if (!instance_)
    // The protocol detour remains installed until Windows tears down the
    // process.  Keep its QObject graph alive for the same lifetime so a late
    // Flash callback cannot target freed Qt objects during application exit.
    instance_ = new ExtensionContext(nullptr);
}

ExtensionContext::ExtensionContext(QObject* parent) : QObject(parent) {
  repository_ = new PetRepository(this);
  bridge_ = new OriginalBridge(this);
  if (!bridge_->install()) {
    DiagnosticLogger::error(QStringLiteral("bridge"), bridge_->lastError());
    QMessageBox::critical(nullptr, QStringLiteral("原版氪奇精灵扩展"),
                          QStringLiteral("扩展没有启动：\n%1").arg(bridge_->lastError()));
    return;
  }
  DiagnosticLogger::info(QStringLiteral("bridge"),
                         QStringLiteral("protocol bridge and dispatch hook installed"));
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
          bridge_, &OriginalBridge::disableCapture, Qt::DirectConnection);

  connect(bridge_, &OriginalBridge::packetReceived, repository_, &PetRepository::handlePacket);
  refreshController_ = new PetRefreshController(repository_, this);
  refreshController_->setSender(
      [this](const QString& service, const QString& command, const QString& parameters) {
        return bridge_->send(service, command, parameters);
      });
  refreshController_->setFlashInvoker(
      [this](const QString& method, const QString& argument) {
        return bridge_->invokeFlash(method, argument);
      });
  connect(refreshController_, &PetRefreshController::replacementRequired,
          this, &ExtensionContext::routeMoveReplacement);
  connect(refreshController_, &PetRefreshController::moveFinished, this,
          [this](bool, const QString&) { moveUiOrigin_ = MoveUiOrigin::None; });
  shopController_ = new ShopExchangeController(repository_, this);
  shopController_->setSender(
      [this](const QString& service, const QString& command, const QString& parameters) {
        return bridge_->send(service, command, parameters);
      });
  connect(bridge_, &OriginalBridge::packetReceived, shopController_,
          &ShopExchangeController::handlePacket);
  routineController_ = new RoutineOverviewController(repository_, this);
  routineController_->setSender(
      [this](const QString& service, const QString& command, const QString& parameters) {
        return bridge_->send(service, command, parameters);
      });
  connect(bridge_, &OriginalBridge::packetReceived, routineController_,
          &RoutineOverviewController::handlePacket);
  connect(repository_, &PetRepository::accountSessionChanged, this,
          [](const QString& account, quint64 generation) {
            DiagnosticLogger::info(
                QStringLiteral("session"),
                QStringLiteral("account=%1 generation=%2")
                    .arg(DiagnosticLogger::maskedAccount(account))
                    .arg(generation));
          });
  connect(repository_, &PetRepository::detailResponseRejected, this,
          [](qint64 instanceId, quint64 generation, const QString& reason) {
            DiagnosticLogger::warning(
                QStringLiteral("detail"),
                QStringLiteral("rejected instance=%1 request_generation=%2 reason=%3")
                    .arg(instanceId).arg(generation).arg(reason));
          });
  connect(repository_, &PetRepository::sequenceUpdateRejected, this,
          [](quint64 generation, const QString& reason) {
            DiagnosticLogger::warning(
                QStringLiteral("move"),
                QStringLiteral("write response rejected request_generation=%1 reason=%2")
                    .arg(generation).arg(reason));
          });
  connect(refreshController_, &PetRefreshController::commandSent, this,
          [](const QString& command, qint64 instanceId, quint64 generation) {
            DiagnosticLogger::info(
                QStringLiteral("request"),
                QStringLiteral("started command=%1 instance=%2 request_generation=%3")
                    .arg(command).arg(instanceId).arg(generation));
          });
  connect(refreshController_, &PetRefreshController::moveFinished, this,
          [](bool succeeded, const QString& message) {
            if (succeeded)
              DiagnosticLogger::info(QStringLiteral("move"), message);
            else
              DiagnosticLogger::error(QStringLiteral("move"), message);
          });
  connect(shopController_, &ShopExchangeController::statusChanged, this,
          [](const QString& status) {
            DiagnosticLogger::info(QStringLiteral("shop"), status);
          });
  connect(routineController_, &RoutineOverviewController::statusChanged, this,
          [](const QString& status) {
            DiagnosticLogger::info(QStringLiteral("routine"), status);
          });
  QTimer::singleShot(0, this, &ExtensionContext::attachToOriginalWindow);
}

void ExtensionContext::attachToOriginalWindow() {
  QWidget* best = nullptr;
  qint64 bestArea = 0;
  const QWidgetList windows = QApplication::topLevelWidgets();
  for (QWidget* window : windows) {
    if (!window || window->objectName() == QStringLiteral("KQPetInventoryWindow") ||
        window->objectName() == QStringLiteral("KQPetShopWindow") ||
        window->objectName() == QStringLiteral("KQRoutineOverviewWindow") ||
        !window->isVisible())
      continue;
    const qint64 area = static_cast<qint64>(window->width()) * window->height();
    if (area > bestArea) {
      best = window;
      bestArea = area;
    }
  }
  if (!best) {
    QTimer::singleShot(1000, this, &ExtensionContext::attachToOriginalWindow);
    return;
  }

  originalWindow_ = best;
  originalWindow_->installEventFilter(this);
  openButton_ = new QPushButton(QStringLiteral("精灵仓库"), originalWindow_);
  openButton_->setObjectName(QStringLiteral("KQPetInventoryButton"));
  // The original client embeds a native WebView child window.  Give the
  // extension entry its own child HWND so mouse input cannot be swallowed by
  // that native surface even though Qt paints the button above it.
  openButton_->setAttribute(Qt::WA_NativeWindow, true);
  openButton_->setFixedSize(74, 26);
  openButton_->setFocusPolicy(Qt::StrongFocus);
  openButton_->setCursor(Qt::PointingHandCursor);
  openButton_->setStyleSheet(QStringLiteral(
      "QPushButton#KQPetInventoryButton{color:white;background:#5d6dfd;border:0;border-radius:5px;"
      "font-size:11px;font-weight:600;}QPushButton#KQPetInventoryButton:hover{background:#7180ff;}"
      "QPushButton#KQPetInventoryButton:pressed{background:#4656db;}"));
  connect(openButton_, &QPushButton::clicked, this, &ExtensionContext::showPetWindow);

  shopButton_ = new QPushButton(QStringLiteral("兑换商店"), originalWindow_);
  shopButton_->setObjectName(QStringLiteral("KQPetShopButton"));
  shopButton_->setAttribute(Qt::WA_NativeWindow, true);
  shopButton_->setFixedSize(74, 26);
  shopButton_->setFocusPolicy(Qt::StrongFocus);
  shopButton_->setCursor(Qt::PointingHandCursor);
  shopButton_->setStyleSheet(QStringLiteral(
      "QPushButton#KQPetShopButton{color:white;background:#2f9e7a;border:0;border-radius:5px;"
      "font-size:11px;font-weight:600;}QPushButton#KQPetShopButton:hover{background:#3cb58c;}"
      "QPushButton#KQPetShopButton:pressed{background:#248868;}"));
  connect(shopButton_, &QPushButton::clicked, this, &ExtensionContext::showShopWindow);

  routineButton_ = new QPushButton(QStringLiteral("日常活动"), originalWindow_);
  routineButton_->setObjectName(QStringLiteral("KQRoutineOverviewButton"));
  routineButton_->setAttribute(Qt::WA_NativeWindow, true);
  routineButton_->setFixedSize(74, 26);
  routineButton_->setFocusPolicy(Qt::StrongFocus);
  routineButton_->setCursor(Qt::PointingHandCursor);
  routineButton_->setStyleSheet(QStringLiteral(
      "QPushButton#KQRoutineOverviewButton{color:white;background:#d97706;border:0;border-radius:5px;"
      "font-size:11px;font-weight:600;}QPushButton#KQRoutineOverviewButton:hover{background:#ea8c16;}"
      "QPushButton#KQRoutineOverviewButton:pressed{background:#b85f00;}"));
  connect(routineButton_, &QPushButton::clicked, this,
          &ExtensionContext::showRoutineWindow);

  positionButton();
  openButton_->show();
  openButton_->winId();
  openButton_->raise();
  shopButton_->show();
  shopButton_->winId();
  shopButton_->raise();
  routineButton_->show();
  routineButton_->winId();
  routineButton_->raise();
}

bool ExtensionContext::eventFilter(QObject* watched, QEvent* event) {
  if (watched == originalWindow_ && event->type() == QEvent::Resize)
    positionButton();
  return QObject::eventFilter(watched, event);
}

void ExtensionContext::positionButton() {
  if (!originalWindow_ || !openButton_)
    return;
  // The original navigation bar places “资源” at roughly 79.4% of the
  // client width. Keep the smaller entry centered immediately above it.
  const int resourceCenter = originalWindow_->width() * 794 / 1000;
  const int x = qBound(8, resourceCenter - openButton_->width() / 2,
                       originalWindow_->width() - openButton_->width() - 8);
  openButton_->move(x, 3);
  openButton_->raise();
  if (shopButton_) {
    shopButton_->move(qMax(8, x - shopButton_->width() - 8), 3);
    shopButton_->raise();
  }
  if (routineButton_) {
    const int shopX = shopButton_ ? shopButton_->x() : x - routineButton_->width() - 8;
    routineButton_->move(qMax(8, shopX - routineButton_->width() - 8), 3);
    routineButton_->raise();
  }
}

void ExtensionContext::showPetWindow() {
  if (!petWindow_) {
    // An independent top-level window participates in the normal desktop
    // stacking order. Whichever of the original client and this window the
    // user clicks becomes the foreground window.
    petWindow_ = new PetWindow(repository_, nullptr);
    connect(petWindow_, &PetWindow::listRefreshRequested, refreshController_,
            &PetRefreshController::requestManualListRefresh);
    connect(petWindow_, &PetWindow::warehouseDetailRefreshRequested, refreshController_,
            &PetRefreshController::startWarehouseDetailRefresh);
    connect(petWindow_, &PetWindow::warehouseDetailPauseRequested, refreshController_,
            &PetRefreshController::pauseWarehouseDetailRefresh);
    connect(petWindow_, &PetWindow::warehouseDetailResumeRequested, refreshController_,
            &PetRefreshController::resumeWarehouseDetailRefresh);
    connect(petWindow_, &PetWindow::warehouseDetailCancelRequested, refreshController_,
            &PetRefreshController::cancelWarehouseDetailRefresh);
    connect(petWindow_, &PetWindow::detailRequested, refreshController_,
            &PetRefreshController::requestSingleDetail);
    connect(petWindow_, &PetWindow::settingsRequested, this,
            &ExtensionContext::showSettings);
    connect(petWindow_, &PetWindow::copyDiagnosticsRequested, this, [this]() {
      QApplication::clipboard()->setText(DiagnosticLogger::diagnosticText());
      if (petWindow_)
        petWindow_->setStatus(QStringLiteral("诊断信息已复制到剪贴板；日志：%1")
                                  .arg(DiagnosticLogger::logPath()));
    });
    connect(petWindow_, &PetWindow::moveToWarehouseRequested, this,
            [this](qint64 id) {
              if (!refreshController_->moveRunning())
                moveUiOrigin_ = MoveUiOrigin::PetWindow;
              refreshController_->requestMoveToWarehouse(id);
            });
    connect(petWindow_, &PetWindow::moveToBackpackRequested, this,
            [this](qint64 id) {
              if (!refreshController_->moveRunning())
                moveUiOrigin_ = MoveUiOrigin::PetWindow;
              refreshController_->requestMoveToBackpack(id);
            });
    connect(petWindow_, &PetWindow::moveReplacementChosen, refreshController_,
            &PetRefreshController::chooseMoveReplacement);
    connect(petWindow_, &PetWindow::moveCancelRequested, refreshController_,
            &PetRefreshController::cancelMove);
    connect(refreshController_, &PetRefreshController::statusChanged, petWindow_,
            &PetWindow::setStatus);
    connect(refreshController_, &PetRefreshController::listRefreshRunningChanged,
            petWindow_, &PetWindow::setListRefreshRunning);
    connect(refreshController_, &PetRefreshController::detailProgressChanged,
            petWindow_, &PetWindow::setDetailProgress);
    connect(refreshController_, &PetRefreshController::moveRunningChanged,
            petWindow_, &PetWindow::setMoveRunning);
    refreshController_->publishState();
  }
  refreshController_->requestFormationLoad();
  petWindow_->show();
  petWindow_->raise();
  petWindow_->activateWindow();
}

void ExtensionContext::showShopWindow() {
  if (!shopWindow_) {
    shopWindow_ = new ShopWindow(repository_, nullptr);
    connect(shopWindow_, &ShopWindow::refreshRequested, shopController_,
            &ShopExchangeController::requestInfo);
    connect(shopWindow_, &ShopWindow::catalogRefreshRequested, shopController_,
            &ShopExchangeController::updateCatalog);
    connect(shopWindow_, &ShopWindow::detailRequested, refreshController_,
            &PetRefreshController::requestSingleDetail);
    connect(shopWindow_, &ShopWindow::moveToBackpackRequested, this,
            [this](qint64 id) {
              if (!refreshController_->moveRunning())
                moveUiOrigin_ = MoveUiOrigin::ShopWindow;
              refreshController_->requestMoveToBackpack(id);
            });
    connect(shopWindow_, &ShopWindow::moveReplacementChosen, refreshController_,
            &PetRefreshController::chooseMoveReplacement);
    connect(shopWindow_, &ShopWindow::moveCancelRequested, refreshController_,
            &PetRefreshController::cancelMove);
    connect(shopController_, &ShopExchangeController::statusChanged, shopWindow_,
            &ShopWindow::setStatus);
    connect(shopController_, &ShopExchangeController::runningChanged, shopWindow_,
            &ShopWindow::setRefreshRunning);
    connect(refreshController_, &PetRefreshController::statusChanged, shopWindow_,
            &ShopWindow::setStatus);
    connect(refreshController_, &PetRefreshController::moveRunningChanged,
            shopWindow_, &ShopWindow::setMoveRunning);
    connect(refreshController_, &PetRefreshController::detailRequestFinished,
            shopWindow_, &ShopWindow::finishDetailRefresh);
    connect(shopController_, &ShopExchangeController::infoUpdated, shopWindow_, [this]() {
      if (shopWindow_) {
        shopWindow_->setPacket(shopController_->packet(), shopController_->hasPacket());
        shopWindow_->setMaterialCounts(shopController_->materialCounts(),
                                       shopController_->hasMaterialCounts());
      }
    });
    connect(shopController_, &ShopExchangeController::catalogUpdated, shopWindow_, [this]() {
      if (shopWindow_)
        shopWindow_->setPacket(shopController_->packet(), shopController_->hasPacket());
    });
    refreshController_->publishState();
  }
  shopWindow_->setPacket(shopController_->packet(), shopController_->hasPacket());
  shopWindow_->setMaterialCounts(shopController_->materialCounts(),
                                 shopController_->hasMaterialCounts());
  shopWindow_->show();
  shopWindow_->raise();
  shopWindow_->activateWindow();
}

void ExtensionContext::showRoutineWindow() {
  if (!routineWindow_) {
    routineWindow_ = new RoutineOverviewWindow(nullptr);
    connect(routineWindow_, &RoutineOverviewWindow::refreshRequested,
            routineController_, &RoutineOverviewController::requestRefresh);
    connect(routineController_, &RoutineOverviewController::statusChanged,
            routineWindow_, &RoutineOverviewWindow::setStatus);
    connect(routineController_, &RoutineOverviewController::runningChanged,
            routineWindow_, &RoutineOverviewWindow::setRunning);
    const auto publish = [this]() {
      if (!routineWindow_) return;
      routineWindow_->setData(routineController_->dailyPacket(),
                              routineController_->hasDailyPacket(),
                              routineController_->activeRedPoints(),
                              routineController_->hasRedPointPacket(),
                              routineController_->opportunityPackets());
    };
    connect(routineController_, &RoutineOverviewController::dataUpdated,
            routineWindow_, publish);
    connect(routineController_, &RoutineOverviewController::catalogUpdated,
            routineWindow_, &RoutineOverviewWindow::rebuild);
    publish();
  }
  routineWindow_->show();
  routineWindow_->raise();
  routineWindow_->activateWindow();
}

void ExtensionContext::routeMoveReplacement(
    qint64 incomingInstanceId, const QList<qint64>& eligibleBackpackIds) {
  if (moveUiOrigin_ == MoveUiOrigin::ShopWindow && shopWindow_) {
    shopWindow_->requestReplacement(incomingInstanceId, eligibleBackpackIds);
    return;
  }
  if (moveUiOrigin_ == MoveUiOrigin::PetWindow && petWindow_) {
    petWindow_->requestReplacement(incomingInstanceId, eligibleBackpackIds);
    return;
  }
  refreshController_->cancelMove();
}

void ExtensionContext::showSettings() {
  if (!petWindow_ || !refreshController_) return;
  PetSettingsDialog dialog(refreshController_->timings(), petWindow_);
  if (dialog.exec() == QDialog::Accepted)
    refreshController_->setTimings(dialog.timings());
}
