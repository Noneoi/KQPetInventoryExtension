#include "remote_module.h"
#include <tlhelp32.h>
#include <filesystem>

namespace kqpet::launcher {
namespace {
void fail(ModuleProbeFailure* failure, const wchar_t* stage, DWORD code) {
  if (failure) *failure = {stage, code};
}
bool retryable(DWORD code) {
  return code == ERROR_BAD_LENGTH || code == ERROR_PARTIAL_COPY ||
      code == ERROR_MOD_NOT_FOUND || code == ERROR_NO_MORE_FILES || code == ERROR_NOT_READY;
}
bool sameFile(const std::wstring& left, const std::wstring& right, DWORD* error) {
  const auto open = [](const std::wstring& name) {
    return CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  };
  HANDLE a = open(left);
  if (a == INVALID_HANDLE_VALUE) { *error = GetLastError(); return false; }
  HANDLE b = open(right);
  if (b == INVALID_HANDLE_VALUE) { *error = GetLastError(); CloseHandle(a); return false; }
  BY_HANDLE_FILE_INFORMATION ai{}, bi{};
  const bool readable = GetFileInformationByHandle(a, &ai) && GetFileInformationByHandle(b, &bi);
  *error = readable ? ERROR_SUCCESS : GetLastError();
  CloseHandle(b); CloseHandle(a);
  if (!readable) return false;
  const bool equal = ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber &&
      ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
  if (!equal) *error = ERROR_REVISION_MISMATCH;
  return equal;
}
}
RemoteModule findRemoteModule(DWORD pid, const std::wstring& filename,
                              ULONGLONG deadline, ModuleProbeFailure* failure) {
  if (failure) *failure = {};
  DWORD error = ERROR_MOD_NOT_FOUND;
  do {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) error = GetLastError();
    else {
      MODULEENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      BOOL more = Module32FirstW(snapshot, &entry);
      error = more ? ERROR_MOD_NOT_FOUND : GetLastError();
      while (more) {
        if (!_wcsicmp(entry.szModule, filename.c_str())) {
          RemoteModule result{reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
                              entry.modBaseSize, entry.szExePath};
          CloseHandle(snapshot);
          return result;
        }
        entry.dwSize = sizeof(entry);
        more = Module32NextW(snapshot, &entry);
        if (!more) error = GetLastError();
      }
      CloseHandle(snapshot);
      if (error == ERROR_NO_MORE_FILES) error = ERROR_MOD_NOT_FOUND;
    }
    if (!retryable(error) || GetTickCount64() >= deadline) break;
    Sleep(10);
  } while (true);
  fail(failure, L"读取目标进程的系统模块", error);
  return {};
}
LPTHREAD_START_ROUTINE resolveRemoteLoadLibrary(DWORD pid, ULONGLONG deadline,
                                               ModuleProbeFailure* failure) {
  if (failure) *failure = {};
  FARPROC local = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  if (!local) { fail(failure, L"读取本地 LoadLibraryW", GetLastError()); return nullptr; }
  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(local), &owner)) {
    fail(failure, L"确认系统加载入口所属模块", GetLastError()); return nullptr;
  }
  wchar_t path[32768]{};
  const DWORD length = GetModuleFileNameW(owner, path, 32768);
  if (!length || length >= 32768) {
    fail(failure, L"读取系统模块路径", length ? ERROR_INSUFFICIENT_BUFFER : GetLastError()); return nullptr;
  }
  const auto remote = findRemoteModule(pid, std::filesystem::path(path).filename().wstring(), deadline, failure);
  if (!remote.base) return nullptr;
  const auto offset = reinterpret_cast<std::uintptr_t>(local) - reinterpret_cast<std::uintptr_t>(owner);
  if (offset >= remote.size) { fail(failure, L"校验系统加载入口偏移", ERROR_INVALID_ADDRESS); return nullptr; }
  DWORD fileError = 0;
  if (!sameFile(path, remote.path, &fileError)) {
    fail(failure, L"核对本地与目标系统模块文件", fileError); return nullptr;
  }
  return reinterpret_cast<LPTHREAD_START_ROUTINE>(remote.base + offset);
}
}
