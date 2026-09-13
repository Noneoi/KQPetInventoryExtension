#include "extension_context.h"
#include "diagnostic_logger.h"
#include "target_compatibility_guard.h"
#include "version.h"
#include "original_bridge.h"
#include "startup_channel.h"
#include "target_check.h"

#include <windows.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QResource>
#include <atomic>
#include <memory>
#include <filesystem>

namespace {

struct ReceiverBarrier {
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<OriginalBridge*> bridge{nullptr};
  std::atomic_bool cancelled{false};
  ~ReceiverBarrier() { if (event) CloseHandle(event); }
};

DWORD WINAPI initializeExtension(void* module) {
  using kqpet::startup::State;
  const auto identity = kqpet::compatibility::identifyTargetFile(
      kqpet::compatibility::currentExecutablePath());
  const auto* profile = identity.supported ? identity.profile : nullptr;
  std::wstring channelError;
  auto channel = profile ? kqpet::startup::Channel::openForCurrentProcess(
      KQPET_RELEASE_ID_WSTRING, profile->id, &channelError) : nullptr;
  if (!channel || !channel->publish(State::ImageLoaded)) {
    OutputDebugStringW(L"KQPet: startup channel missing/mismatched; extension remains disabled.\n");
    return 5;
  }
  // Do not touch Qt objects until the runtime ABI and all bridge entry
  // points have passed the Win32-only compatibility guard.
  for (int attempt = 0; attempt < 120; ++attempt) {
    if (TargetCompatibilityGuard::requiredRuntimeModulesLoaded()) break;
    Sleep(250);
  }
  TargetCompatibilityReport report = TargetCompatibilityGuard::evaluate();
  TargetCompatibilityGuard::setLastReport(report);
  DiagnosticLogger::initialize(report);
  if (!report.supported) {
    channel->publish(State::Failed, ERROR_REVISION_MISMATCH);
    const std::wstring message =
        L"扩展与当前氪奇运行环境不兼容，已安全停止。\r\n"
        L"原版氪奇可以继续正常使用。\r\n\r\n" + report.format();
    MessageBoxW(nullptr, message.c_str(), L"KQPetInventory 兼容性检查",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    DiagnosticLogger::error(L"compatibility", report.format());
    return 1;
  }
  DiagnosticLogger::info(L"compatibility", L"all required checks passed");
  channel->publish(State::Compatible);
  // Qt's RCC global constructor must not execute before the ABI guard. The
  // binary RCC bytes remain embedded/mapped for the DLL's process lifetime.
  HMODULE ownModule = static_cast<HMODULE>(module);
  HRSRC resource = FindResourceW(ownModule, MAKEINTRESOURCEW(100), RT_RCDATA);
  HGLOBAL loaded = resource ? LoadResource(ownModule, resource) : nullptr;
  const auto* bytes = loaded ? static_cast<const uchar*>(LockResource(loaded)) : nullptr;
  if (!bytes || !SizeofResource(ownModule, resource) || !QResource::registerResource(bytes)) {
    channel->publish(State::Failed, ERROR_RESOURCE_DATA_NOT_FOUND);
    DiagnosticLogger::error(L"startup", L"embedded RCC registration failed after compatibility guard");
    return 2;
  }
  for (int attempt = 0; attempt < 120; ++attempt) {
    QCoreApplication* application = QCoreApplication::instance();
    if (application) {
      auto barrier = std::make_shared<ReceiverBarrier>();
      if (!barrier->event) { channel->publish(State::Failed, GetLastError()); return 3; }
      if (!QMetaObject::invokeMethod(application, [barrier] {
        if (!barrier->cancelled.load(std::memory_order_acquire) &&
            !QCoreApplication::closingDown() && !OriginalBridge::closing()) {
          ExtensionContext::prepareBridge([barrier](OriginalBridge* bridge) {
            if (!barrier->cancelled.load(std::memory_order_acquire))
              barrier->bridge.store(bridge, std::memory_order_release);
            SetEvent(barrier->event);
          });
        } else SetEvent(barrier->event);
      }, Qt::QueuedConnection)) { channel->publish(State::Failed, ERROR_INVALID_WINDOW_HANDLE); return 3; }
      if (WaitForSingleObject(barrier->event, 30000) != WAIT_OBJECT_0) {
        barrier->cancelled.store(true, std::memory_order_release);
        DiagnosticLogger::error(L"startup", L"GUI receiver initialization not confirmed");
        channel->publish(State::Failed, WAIT_TIMEOUT);
        return 3;
      }
      OriginalBridge* bridge = barrier->bridge.load(std::memory_order_acquire);
      if (!bridge || !bridge->install()) {
        channel->publish(State::Failed, ERROR_FUNCTION_FAILED);
        DiagnosticLogger::error(L"startup", L"Dispatch hook installation failed");
        return 4;
      }
      DiagnosticLogger::info(L"bridge", L"Dispatch hook enabled after GUI receiver barrier");
      channel->publish(State::BridgeReady);
      if (!QMetaObject::invokeMethod(bridge, [channel] {
        if (!QCoreApplication::closingDown() && !OriginalBridge::closing()) ExtensionContext::start(channel);
        else channel->publish(State::Failed, ERROR_SHUTDOWN_IN_PROGRESS);
      }, Qt::QueuedConnection)) {
        channel->publish(State::Failed, ERROR_INVALID_WINDOW_HANDLE);
        return 4;
      }
      return 0;
    }
    Sleep(250);
  }
  channel->publish(State::Failed, WAIT_TIMEOUT);
  return 1;
}
}  // namespace

extern "C" __declspec(dllexport) const wchar_t* KqPetExtensionVersion() {
  return KQPET_VERSION_WSTRING;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    HANDLE thread = CreateThread(nullptr, 0, initializeExtension, instance, 0, nullptr);
    if (thread)
      CloseHandle(thread);
  }
  return TRUE;
}
