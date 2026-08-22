#include "original_bridge.h"

#include <windows.h>

#include <QApplication>
#include <QCoreApplication>
#include <QList>
#include <QMetaObject>
#include <QVariant>
#include <QWidget>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uintptr_t kDispatchRva = 0x115920;
constexpr std::uintptr_t kServiceGetterRva = 0x115450;
constexpr std::uintptr_t kCommandSenderRva = 0x116470;

constexpr std::array<unsigned char, 17> kDispatchPrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD1};

constexpr std::array<unsigned char, 21> kCommandSenderPrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD9};

constexpr std::array<unsigned char, 16> kServiceGetterPrologue = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
    0x05, 0x8B, 0x7F, 0x10, 0x00, 0x48, 0x85, 0xC0};

template <size_t Size>
void* scanExecutableSections(HMODULE module,
                             const std::array<unsigned char, Size>& pattern) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
  unsigned char* match = nullptr;
  for (WORD sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections;
       ++sectionIndex) {
    const IMAGE_SECTION_HEADER& section = sections[sectionIndex];
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    const size_t sectionSize = (std::min)(
        static_cast<size_t>(section.Misc.VirtualSize),
        static_cast<size_t>(nt->OptionalHeader.SizeOfImage - section.VirtualAddress));
    if (sectionSize < Size) continue;
    unsigned char* begin = base + section.VirtualAddress;
    for (size_t offset = 0; offset + Size <= sectionSize; ++offset) {
      if (std::memcmp(begin + offset, pattern.data(), Size) != 0) continue;
      if (match) return nullptr;  // Ambiguous signatures are unsafe to hook.
      match = begin + offset;
    }
  }
  return match;
}

template <size_t Size>
void* resolveFunction(HMODULE module, std::uintptr_t knownRva,
                      const std::array<unsigned char, Size>& pattern) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  if (knownRva + Size <= nt->OptionalHeader.SizeOfImage) {
    auto* known = base + knownRva;
    if (std::memcmp(known, pattern.data(), Size) == 0) return known;
  }
  return scanExecutableSections(module, pattern);
}

}  // namespace

std::atomic<OriginalBridge*> OriginalBridge::instance_{nullptr};
std::atomic_bool OriginalBridge::captureEnabled_{false};
OriginalBridge::DispatchFunction OriginalBridge::originalDispatch_ = nullptr;

OriginalBridge::OriginalBridge(QObject* parent) : QObject(parent) {}

bool OriginalBridge::install() {
  if (instance_.load(std::memory_order_acquire)) {
    lastError_ = QStringLiteral("扩展桥已经安装。");
    return false;
  }

  const HMODULE module = GetModuleHandleW(nullptr);
  if (!module) {
    lastError_ = QStringLiteral("无法取得当前氪奇主程序模块。");
    return false;
  }
  void* dispatch = resolveFunction(module, kDispatchRva, kDispatchPrologue);
  void* serviceGetterAddress =
      resolveFunction(module, kServiceGetterRva, kServiceGetterPrologue);
  void* commandSenderAddress =
      resolveFunction(module, kCommandSenderRva, kCommandSenderPrologue);
  if (!dispatch || !serviceGetterAddress || !commandSenderAddress) {
    lastError_ = QStringLiteral(
        "当前氪奇版本的协议入口签名无法唯一识别。为避免未知偏移导致崩溃，扩展已安全停止；"
        "请使用该版本程序更新一次扩展签名。");
    return false;
  }
  serviceGetter_ = reinterpret_cast<ServiceGetter>(serviceGetterAddress);
  commandSender_ = reinterpret_cast<CommandSender>(commandSenderAddress);

  instance_.store(this, std::memory_order_release);
  if (!hook_.install(dispatch, reinterpret_cast<void*>(&OriginalBridge::dispatchDetour),
                     kDispatchPrologue.data(), kDispatchPrologue.size())) {
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
