#include "extension_context.h"
#include "diagnostic_logger.h"
#include "target_compatibility_guard.h"
#include "version.h"

#include <windows.h>

#include <QCoreApplication>
#include <QMetaObject>

namespace {

DWORD WINAPI initializeExtension(void*) {
  // Do not touch Qt objects until the exact runtime ABI and all bridge entry
  // points have passed the Win32-only compatibility guard.
  for (int attempt = 0; attempt < 120; ++attempt) {
    if (TargetCompatibilityGuard::requiredRuntimeModulesLoaded()) break;
    Sleep(250);
  }
  TargetCompatibilityReport report = TargetCompatibilityGuard::evaluate();
  TargetCompatibilityGuard::setLastReport(report);
  DiagnosticLogger::initialize(report);
  if (!report.supported) {
    const std::wstring message =
        L"扩展与当前氪奇运行环境不兼容，已安全停止。\r\n"
        L"原版氪奇可以继续正常使用。\r\n\r\n" + report.format();
    MessageBoxW(nullptr, message.c_str(), L"KQPetInventory 兼容性检查",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    DiagnosticLogger::error(L"compatibility", report.format());
    return 1;
  }
  DiagnosticLogger::info(L"compatibility", L"all required checks passed");
  for (int attempt = 0; attempt < 120; ++attempt) {
    QCoreApplication* application = QCoreApplication::instance();
    if (application) {
      QMetaObject::invokeMethod(application, [] { ExtensionContext::start(); },
                                Qt::QueuedConnection);
      return 0;
    }
    Sleep(250);
  }
  return 1;
}
}  // namespace

extern "C" __declspec(dllexport) const wchar_t* KqPetExtensionVersion() {
  return KQPET_VERSION_WSTRING;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    HANDLE thread = CreateThread(nullptr, 0, initializeExtension, nullptr, 0, nullptr);
    if (thread)
      CloseHandle(thread);
  }
  return TRUE;
}
