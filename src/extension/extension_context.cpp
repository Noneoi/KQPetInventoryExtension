#include "extension_context.h"

#include "original_bridge.h"
#include "pet_refresh_controller.h"
#include "pet_repository.h"
#include "pet_settings_dialog.h"
#include "pet_window.h"

#include <QApplication>
#include <QEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

ExtensionContext* ExtensionContext::instance_ = nullptr;

void ExtensionContext::start() {
  if (!instance_)
    instance_ = new ExtensionContext(QCoreApplication::instance());
}

ExtensionContext::ExtensionContext(QObject* parent) : QObject(parent) {
  repository_ = new PetRepository(this);
  bridge_ = new OriginalBridge(this);
  if (!bridge_->install()) {
    QMessageBox::critical(nullptr, QStringLiteral("原版氪奇精灵扩展"),
                          QStringLiteral("扩展没有启动：\n%1").arg(bridge_->lastError()));
    return;
  }

  connect(bridge_, &OriginalBridge::packetReceived, repository_, &PetRepository::handlePacket);
  refreshController_ = new PetRefreshController(repository_, this);
  refreshController_->setSender(
      [this](const QString& service, const QString& command, const QString& parameters) {
        return bridge_->send(service, command, parameters);
      });
  QTimer::singleShot(0, this, &ExtensionContext::attachToOriginalWindow);
}

void ExtensionContext::attachToOriginalWindow() {
  QWidget* best = nullptr;
  qint64 bestArea = 0;
  const QWidgetList windows = QApplication::topLevelWidgets();
  for (QWidget* window : windows) {
    if (!window || window->objectName() == QStringLiteral("KQPetInventoryWindow") ||
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
  positionButton();
  openButton_->show();
  openButton_->winId();
  openButton_->raise();
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
    connect(refreshController_, &PetRefreshController::statusChanged, petWindow_,
            &PetWindow::setStatus);
    connect(refreshController_, &PetRefreshController::listRefreshRunningChanged,
            petWindow_, &PetWindow::setListRefreshRunning);
    connect(refreshController_, &PetRefreshController::detailProgressChanged,
            petWindow_, &PetWindow::setDetailProgress);
    refreshController_->publishState();
  }
  petWindow_->show();
  petWindow_->raise();
  petWindow_->activateWindow();
}

void ExtensionContext::showSettings() {
  if (!petWindow_ || !refreshController_) return;
  PetSettingsDialog dialog(refreshController_->timings(), petWindow_);
  if (dialog.exec() == QDialog::Accepted)
    refreshController_->setTimings(dialog.timings());
}
