#include "remote_module.h"
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>

int wmain(int argc, wchar_t** argv) {
  using namespace kqpet::launcher;
  if (argc == 3 && std::wstring(argv[1]) == L"--module-child") {
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    if (!event) return 2;
    const DWORD result = WaitForSingleObject(event, 5000);
    CloseHandle(event);
    return result == WAIT_OBJECT_0 ? 0 : 3;
  }
  ModuleProbeFailure failure;
  const auto own = resolveRemoteLoadLibrary(GetCurrentProcessId(), GetTickCount64() + 1000, &failure);
  const auto expected = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  if (!own || reinterpret_cast<std::uintptr_t>(own) != reinterpret_cast<std::uintptr_t>(expected)) {
    std::fwprintf(stderr, L"FAIL: self address resolution: %ls error=%lu\n", failure.stage.c_str(), failure.systemError);
    return 1;
  }
  if (findRemoteModule(GetCurrentProcessId(), L"kq-no-such-module-fixture.dll", GetTickCount64() + 20, &failure).base ||
      failure.systemError != ERROR_MOD_NOT_FOUND) {
    std::fputs("FAIL: missing module did not report its own error\n", stderr); return 1;
  }
  wchar_t executable[32768]{};
  if (!GetModuleFileNameW(nullptr, executable, 32768)) return 1;
  const std::wstring eventName = L"Local\\KQLoaderModuleSmoke-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(GetTickCount64());
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
  if (!event) return 1;
  std::wstring command = L"\"" + std::wstring(executable) + L"\" --module-child " + eventName;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child)) {
    CloseHandle(event); return 1;
  }
  // Query the actual native loader while this owned child is starting. It
  // contains no game code; no remote memory is written and no code is injected.
  std::thread resume([thread = child.hThread] { Sleep(100); ResumeThread(thread); });
  const auto remote = resolveRemoteLoadLibrary(child.dwProcessId, GetTickCount64() + 2500, &failure);
  resume.join();
  const auto missing = findRemoteModule(child.dwProcessId, L"kq-no-such-module-fixture.dll",
                                       GetTickCount64(), nullptr);
  SetEvent(event);
  const DWORD waited = WaitForSingleObject(child.hProcess, 5000);
  DWORD childExit = 1;
  GetExitCodeProcess(child.hProcess, &childExit);
  CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(event);
  if (!remote || missing.base || waited != WAIT_OBJECT_0 || childExit != 0) {
    std::fwprintf(stderr, L"FAIL: starting-child resolution: %ls error=%lu child=%lu\n",
                  failure.stage.c_str(), failure.systemError, childExit);
    return 1;
  }
  std::puts("PASS: native LoadLibrary address, startup module retry, missing-module error; no remote writes");
  return 0;
}
