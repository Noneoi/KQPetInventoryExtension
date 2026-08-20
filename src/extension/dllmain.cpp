#include "extension_context.h"

#include <windows.h>

#include <QCoreApplication>
#include <QMetaObject>

namespace {

DWORD WINAPI initializeExtension(void*) {
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
  return L"1.0.0-original-kqpro-v1.1.3";
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
