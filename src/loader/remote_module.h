#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace kqpet::launcher {
struct RemoteModule {
  std::uintptr_t base = 0;
  DWORD size = 0;
  std::wstring path;
};
struct ModuleProbeFailure {
  std::wstring stage;
  DWORD systemError = 0;
};
// These functions only inspect module metadata. They never write to or execute
// code in the inspected process. The deadline bounds startup snapshot retries.
RemoteModule findRemoteModule(DWORD pid, const std::wstring& filename,
                              ULONGLONG deadline, ModuleProbeFailure* failure);
LPTHREAD_START_ROUTINE resolveRemoteLoadLibrary(DWORD pid, ULONGLONG deadline,
                                               ModuleProbeFailure* failure);
}
