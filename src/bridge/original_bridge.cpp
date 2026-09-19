#include "original_bridge.h"

#include "diagnostics/target_compatibility_guard.h"
#include "protocol/packet_contract.h"

#include <windows.h>

#include <QApplication>
#include <QCoreApplication>
#include <QList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <cstdint>
#include <cstring>
#include <chrono>

std::atomic<OriginalBridge*> OriginalBridge::instance_{nullptr};
std::atomic_bool OriginalBridge::captureEnabled_{false};
std::atomic<OriginalBridge::DispatchFunction> OriginalBridge::originalDispatch_{nullptr};
std::atomic_bool OriginalBridge::installationClaimed_{false};
std::atomic_bool OriginalBridge::shutdownRequested_{false};

OriginalBridge::OriginalBridge(QObject* parent) : QObject(parent) {}

bool OriginalBridge::install() {
  if (shutdownRequested_.load(std::memory_order_acquire)) {
    lastError_ = QStringLiteral("客户端已经进入退出状态。");
    return false;
  }
  bool unclaimed = false;
  if (!installationClaimed_.compare_exchange_strong(unclaimed, true, std::memory_order_acq_rel)) {
    lastError_ = QStringLiteral("扩展桥已经安装。");
    return false;
  }

  const TargetCompatibilityReport& compatibility = TargetCompatibilityGuard::lastReport();
  if (!compatibility.supported) {
    lastError_ = QStringLiteral("兼容性检查未通过，桥接和 Hook 已禁用。");
    return false;
  }
  void* dispatch = reinterpret_cast<void*>(compatibility.dispatch.address);
  void* serviceGetterAddress = reinterpret_cast<void*>(compatibility.serviceGetter.address);
  void* commandSenderAddress = reinterpret_cast<void*>(compatibility.commandSender.address);
  serviceGetter_ = reinterpret_cast<ServiceGetter>(serviceGetterAddress);
  commandSender_ = reinterpret_cast<CommandSender>(commandSenderAddress);

  if (!hook_.createDisabled(dispatch, reinterpret_cast<void*>(&OriginalBridge::dispatchDetour),
                     compatibility.dispatch.signature.data(),
                     compatibility.dispatch.signatureSize,
                     compatibility.dispatch.trampolinePolicy)) {
    lastError_ = QStringLiteral("原版消息入口校验失败：%1")
                     .arg(QString::fromLatin1(hook_.error()));
    return false;
  }
  originalDispatch_.store(reinterpret_cast<DispatchFunction>(hook_.trampoline()), std::memory_order_release);
  if (shutdownRequested_.load(std::memory_order_acquire)) {
    originalDispatch_.store(nullptr, std::memory_order_release);
    hook_.removeDisabled();
    return false;
  }
  instance_.store(this, std::memory_order_release);
  if (!hook_.enable()) {
    lastError_ = QStringLiteral("Dispatch Hook 启用失败：%1").arg(QString::fromLatin1(hook_.error()));
    instance_.store(nullptr, std::memory_order_release);
    originalDispatch_.store(nullptr, std::memory_order_release);
    hook_.removeDisabled();
    return false;
  }
  captureEnabled_.store(true, std::memory_order_release);
  if (shutdownRequested_.load(std::memory_order_acquire)) {
    captureEnabled_.store(false, std::memory_order_release);
    lastError_ = QStringLiteral("Hook 已保留为只转发状态，客户端正在退出。");
    return false;
  }
  return true;
}

void OriginalBridge::disableCapture() {
  shutdownRequested_.store(true, std::memory_order_release);
  captureEnabled_.store(false, std::memory_order_release);
}

bool OriginalBridge::send(const QString& extension, const QString& command,
                          const QString& json) {
  return sendSubmission(extension, command, json) == SubmissionOutcome::Submitted;
}

SubmissionOutcome OriginalBridge::sendSubmission(const QString& extension,
                                                   const QString& command,
                                                   const QString& json) {
  QString validationError;
  if (!PacketContracts::validateOutbound(extension, command, json, &validationError)) {
    lastError_ = QStringLiteral("出站协议校验失败：%1").arg(validationError);
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  if (!serviceGetter_ || !commandSender_) {
    lastError_ = QStringLiteral("原版发送入口尚未初始化。");
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  QCoreApplication* application = QCoreApplication::instance();
  if (!application || QThread::currentThread() != application->thread()) {
    lastError_ = QStringLiteral("原版发送必须在已就绪的GUI线程执行。");
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  void* service = serviceGetter_();
  if (!service) {
    lastError_ = QStringLiteral("原版 WebViewService 尚未就绪。");
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  const QString canonicalJson = json.trimmed() == QStringLiteral("null") ? QStringLiteral("null")
      : QString::fromUtf8(QJsonDocument::fromJson(json.toUtf8()).toJson(QJsonDocument::Compact));
  const QString wire = extension + QLatin1Char('|') + command + QLatin1Char('|') + canonicalJson;
  commandSender_(service, &wire);
  // The void host entry has accepted the call. It supplies no server result.
  return SubmissionOutcome::Submitted;
}

namespace {

void* findCefView() {
  const QWidgetList widgets = QApplication::allWidgets();
  for (QWidget* widget : widgets) {
    if (!widget) continue;
    const char* className = widget->metaObject()->className();
    if (className && std::strcmp(className, "QCefView") == 0) return widget;
  }
  return nullptr;
}

}  // namespace

SubmissionOutcome OriginalBridge::invokeFlashSubmission(const QString& method,
                                                          const QString& argument) {
  QString validationError;
  if (!PacketContracts::validateFlash(method, argument, &validationError)) {
    lastError_ = QStringLiteral("Flash协议校验失败：%1").arg(validationError);
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
  if (!application || QThread::currentThread() != application->thread()) {
    lastError_ = QStringLiteral("Flash调用必须在已就绪的GUI线程执行。");
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  const auto& compatibility = TargetCompatibilityGuard::lastReport();
  if (!compatibility.supported || !compatibility.qcefViewExecuteJavascript) {
    lastError_ = QStringLiteral("QCefView执行入口未经兼容性验证。");
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  void* view = findCefView();
  if (!view) {
    lastError_ = QStringLiteral("找不到原版 QCefView，无法调用 Flash 背包接口。");
    return SubmissionOutcome::DefinitelyNotSubmitted;
  }
  using ExecuteJavascript = bool(__fastcall*)(void*, const qint64*, const QString*,
                                              const QString*);
  auto* execute = reinterpret_cast<ExecuteJavascript>(compatibility.qcefViewExecuteJavascript);
  const QString arguments = QString::fromUtf8(
      QJsonDocument(QJsonArray{argument}).toJson(QJsonDocument::Compact));
  const QString script =
      QStringLiteral("(function(){var f=document.myFlash;"
                     "if(!f||typeof f.batchpet!=='function')return;"
                     "var a=%1;f.batchpet(a[0]);})();").arg(arguments);
  const QString scriptUrl;
  const qint64 frameId = 0;
  if (!execute(view, &frameId, &script, &scriptUrl)) {
    lastError_ = QStringLiteral("Flash执行入口返回未确认结果，请只读核对；不会重复提交。");
    return SubmissionOutcome::Unknown;
  }
  return SubmissionOutcome::Submitted;
}

void __fastcall OriginalBridge::dispatchDetour(quintptr a1, quintptr a2, quintptr a3,
                                                const QString& method,
                                                const QList<QVariant>& arguments) {
  OriginalBridge* bridge = instance_.load(std::memory_order_acquire);
  if (bridge && captureEnabled_.load(std::memory_order_acquire)) {
    try { bridge->capture(a1, a2, a3, method, arguments); }
    catch (...) {
      captureEnabled_.store(false, std::memory_order_release);
      try {
        bridge->inbound_.fail();
        if (bridge->overflowHandler_) bridge->overflowHandler_();
        bridge->scheduleDrain();
      } catch (...) { /* Capture failures must not suppress the host dispatch. */ }
    }
  }
  DispatchFunction dispatch = originalDispatch_.load(std::memory_order_acquire);
  if (dispatch)
    dispatch(a1, a2, a3, method, arguments);

}

void OriginalBridge::capture(quintptr, quintptr, quintptr, const QString& method,
                             const QList<QVariant>& arguments) {
  if (method != QStringLiteral("recivedata") && method != QStringLiteral("cutdata") &&
      method != QStringLiteral("otherreturn")) return;
  InboundEnvelope envelope;
  envelope.method = method;
  envelope.receivedMonotonicMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  // Dispatch arguments have not been proved to identify a connection/account.
  // Leave source/epoch unverified; never substitute Core's current account.
  if (!arguments.isEmpty() && arguments.constFirst().metaType().id() == QMetaType::QString)
    envelope.payload = arguments.constFirst().toString();
  if (envelope.payload.isEmpty() || !inbound_.push(std::move(envelope))) {
    inbound_.fail();
    captureEnabled_.store(false, std::memory_order_release);
    if (overflowHandler_) overflowHandler_();
  }
  scheduleDrain();
}

void OriginalBridge::scheduleDrain() {
  if (!drainScheduled_.exchange(true, std::memory_order_acq_rel))
    QMetaObject::invokeMethod(this, [this] { drainInbound(); }, Qt::QueuedConnection);
}

void OriginalBridge::drainInbound() {
  if (!inbound_.healthy()) {
    if (!uncertaintyReported_.exchange(true))
      emit captureUncertain(QStringLiteral("入站数据超出容量或无法安全捕获，会话来源待确认；新任务已停止。"));
  } else if (!closing()) {
    const auto batch = inbound_.takeBatch();
    for (const auto& envelope : batch) {
      if (!inbound_.healthy() || closing()) break;
      emit packetCaptured(envelope);
    }
  }
  drainScheduled_.store(false, std::memory_order_release);
  if (inbound_.state().queuedPackets && !closing()) scheduleDrain();
}
