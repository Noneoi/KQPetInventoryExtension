#include "original_bridge.h"

#include "target_compatibility_guard.h"

#include <windows.h>

#include <QApplication>
#include <QCoreApplication>
#include <QList>
#include <QMetaObject>
#include <QVariant>
#include <QWidget>

#include <cstdint>
#include <cstring>

std::atomic<OriginalBridge*> OriginalBridge::instance_{nullptr};
std::atomic_bool OriginalBridge::captureEnabled_{false};
OriginalBridge::DispatchFunction OriginalBridge::originalDispatch_ = nullptr;

OriginalBridge::OriginalBridge(QObject* parent) : QObject(parent) {}

bool OriginalBridge::install() {
  if (instance_.load(std::memory_order_acquire)) {
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

  instance_.store(this, std::memory_order_release);
  if (!hook_.install(dispatch, reinterpret_cast<void*>(&OriginalBridge::dispatchDetour),
                     compatibility.dispatch.signature.data(),
                     compatibility.dispatch.signatureSize)) {
    instance_.store(nullptr, std::memory_order_release);
    lastError_ = QStringLiteral("原版消息入口校验失败：%1")
                     .arg(QString::fromLatin1(hook_.error()));
    return false;
  }
  originalDispatch_ = reinterpret_cast<DispatchFunction>(hook_.trampoline());
  captureEnabled_.store(true, std::memory_order_release);
  return true;
}

void OriginalBridge::disableCapture() {
  captureEnabled_.store(false, std::memory_order_release);
}

bool OriginalBridge::send(const QString& extension, const QString& command,
                          const QString& json) {
  if (!serviceGetter_ || !commandSender_) {
    lastError_ = QStringLiteral("原版发送入口尚未初始化。");
    return false;
  }
  if (extension.contains(QLatin1Char('|')) || command.contains(QLatin1Char('|')) ||
      json.contains(QLatin1Char('|'))) {
    lastError_ = QStringLiteral("命令包含原版协议分隔符。");
    return false;
  }
  void* service = serviceGetter_();
  if (!service) {
    lastError_ = QStringLiteral("原版 WebViewService 尚未就绪。");
    return false;
  }
  const QString wire = extension + QLatin1Char('|') + command + QLatin1Char('|') + json;
  commandSender_(service, &wire);
  return true;
}

namespace {

QString escapeJsString(const QString& value) {
  QString escaped;
  escaped.reserve(value.size());
  for (QChar character : value) {
    if (character == QLatin1Char('\\') || character == QLatin1Char('\'') ||
        character == QLatin1Char('"'))
      escaped.append(QLatin1Char('\\'));
    escaped.append(character);
  }
  return escaped;
}

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

bool OriginalBridge::invokeFlash(const QString& method, const QString& argument) {
  bool methodOk = !method.isEmpty();
  for (QChar character : method) {
    const ushort value = character.unicode();
    const bool letter = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    const bool digit = value >= '0' && value <= '9';
    if (!letter && !digit && value != '_') {
      methodOk = false;
      break;
    }
  }
  if (!methodOk) {
    lastError_ = QStringLiteral("Flash 方法名非法。");
    return false;
  }
  void* view = findCefView();
  if (!view) {
    lastError_ = QStringLiteral("找不到原版 QCefView，无法调用 Flash 背包接口。");
    return false;
  }
  const HMODULE module = GetModuleHandleW(L"QCefView.dll");
  if (!module) {
    lastError_ = QStringLiteral("原版进程未加载 QCefView.dll。");
    return false;
  }
  using ExecuteJavascript = bool(__fastcall*)(void*, const qint64*, const QString*,
                                              const QString*);
  auto* execute = reinterpret_cast<ExecuteJavascript>(GetProcAddress(
      module, "?executeJavascript@QCefView@@QEAA_NAEB_JAEBVQString@@1@Z"));
  if (!execute) {
    lastError_ = QStringLiteral("QCefView::executeJavascript 入口无法解析。");
    return false;
  }
  const QString script =
      QStringLiteral("(function(){var f=document.myFlash;"
                     "if(!f||typeof f.%1!=='function')return;"
                     "f.%1('%2');})();")
          .arg(method, escapeJsString(argument));
  const QString scriptUrl;
  const qint64 frameId = 0;
  if (!execute(view, &frameId, &script, &scriptUrl)) {
    lastError_ = QStringLiteral("向原版网页注入 Flash 调用失败。");
    return false;
  }
  return true;
}

void __fastcall OriginalBridge::dispatchDetour(quintptr a1, quintptr a2, quintptr a3,
                                                const QString& method,
                                                const QList<QVariant>& arguments) {
  QString payload;
  if ((method == QStringLiteral("recivedata") || method == QStringLiteral("cutdata") ||
       method == QStringLiteral("otherreturn")) &&
      !arguments.isEmpty()) {
    payload = arguments.constFirst().toString();
  }

  OriginalBridge* bridge = instance_.load(std::memory_order_acquire);
  DispatchFunction dispatch = originalDispatch_;
  if (!dispatch && bridge)
    dispatch = reinterpret_cast<DispatchFunction>(bridge->hook_.trampoline());
  if (dispatch)
    dispatch(a1, a2, a3, method, arguments);

  // The original dispatcher may run on a WebView/Flash worker thread.  Never
  // execute extension QObject code on that thread and never enqueue work while
  // Qt is shutting down.  ExtensionContext deliberately owns this bridge for
  // the lifetime of the process because the inline hook itself is process-wide.
  if (bridge && captureEnabled_.load(std::memory_order_acquire) &&
      !payload.isEmpty() && QCoreApplication::instance()) {
    QMetaObject::invokeMethod(
        bridge,
        [bridge, method, payload]() {
          if (captureEnabled_.load(std::memory_order_acquire))
            emit bridge->packetReceived(method, payload);
        },
        Qt::QueuedConnection);
  }
}
