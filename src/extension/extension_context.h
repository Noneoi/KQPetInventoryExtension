#pragma once
#include "application_runtime.h"
#include "pet_refresh_controller.h"
#include "workbench_types.h"
#include <QObject>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace kqpet::startup { class Channel; }
class OriginalBridge;
class OriginalWindowLocator;
class InventoryProjection;
class AnalysisProjection;
class PetImageCache;
class WorkbenchWindow;
class PetWindow;
class ShopWindow;
class RoutineOverviewWindow;
class AssetAnalysisWindow;
class QPushButton;
class QDialog;
class QWidget;

// GUI composition root only. All mutable repositories/controllers live inside
// ApplicationRuntime's Core thread; pages consume read-only projections/values.
class ExtensionContext final : public QObject {
  Q_OBJECT
public:
  using ReadyCallback = std::function<void(OriginalBridge*)>;
  static void prepareBridge(ReadyCallback callback);
  static void start(std::shared_ptr<kqpet::startup::Channel> channel);
private:
  explicit ExtensionContext(QObject* parent = nullptr);
  void finishCoreInitialization(bool ready, const QString& error = {});
  void attachToOriginalWindow();
  void positionButton();
  void ensureWorkbench();
  void showWorkbench();
  void openPage(WorkbenchPage page);
  QWidget* createPage(WorkbenchPage page, QWidget* parent);
  bool postCurrent(CoreAction action);
  bool matches(const QString& account, quint64 epoch) const;
  bool enqueueOutbound(const OutboundIntent& intent);
  void scheduleOutboundDrain();
  void drainOutbound();
  void refreshSessionDisplay();
  void refreshPageStates();
  void refreshPersistenceDisplay();
  void showSettings();
  void copyDiagnostics();
  void requestMove(qint64 id, bool toBackpack);
  void showReplacement();
  void clearReplacement();

  static ExtensionContext* instance_;
  OriginalBridge* bridge_ = nullptr;
  OriginalWindowLocator* locator_ = nullptr;
  ApplicationRuntime* runtime_ = nullptr;
  InventoryProjection* inventory_ = nullptr;
  AnalysisProjection* analysis_ = nullptr;
  PetImageCache* images_ = nullptr;
  std::shared_ptr<OutboundQueue> outbound_;
  std::atomic_bool outboundScheduled_{false};
  bool coreReady_ = false;
  QString initializationError_;
  std::vector<ReadyCallback> readyCallbacks_;
  std::shared_ptr<kqpet::startup::Channel> startupChannel_;
  QPointer<QWidget> originalWindow_;
  QPointer<QPushButton> entry_;
  QPointer<WorkbenchWindow> workbench_;
  QPointer<PetWindow> petPage_;
  QPointer<ShopWindow> shopPage_;
  QPointer<RoutineOverviewWindow> routinePage_;
  QPointer<AssetAnalysisWindow> assetPage_;
  QPointer<QDialog> replacement_;
  RuntimeShopState shopState_;
  RuntimeRoutineState routineState_;
  RuntimeDetailProgress detailState_;
  RuntimeMoveState moveState_;
  RuntimePersistenceState persistenceState_;
  bool listRunning_ = false;
  QString listAccount_;
  quint64 listEpoch_ = 0;
  bool analysisRunning_ = false;
  QString analysisAccount_;
  quint64 analysisEpoch_ = 0;
  bool settingsPending_ = false;
  quint64 replacementTask_ = 0;
  QString replacementAccount_;
  quint64 replacementEpoch_ = 0;
  qint64 replacementIncoming_ = 0;
  QList<qint64> replacementIds_;
};
