#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include "client_target.h"
#include "remote_module.h"
#include "command_line.h"
#include "data_root_config.h"
#include "target_check.h"
#include "startup_channel.h"
#include "release_activation.h"
#include "version.h"

namespace {
using kqpet::launcher::quoteArgument;
using kqpet::startup::Channel;
using kqpet::startup::State;
std::wstring canonical(const std::wstring& path) {
  std::error_code error;
  auto resolved = std::filesystem::weakly_canonical(path, error);
  return error ? std::wstring{} : resolved.wstring();
}
bool samePath(const std::wstring& a, const std::wstring& b) {
  const auto first = canonical(a), second = canonical(b);
  return !first.empty() && !second.empty() && !_wcsicmp(first.c_str(), second.c_str());
}
void showError(const std::wstring& text) {
  MessageBoxW(nullptr, text.c_str(), L"KQPet 扩展启动状态", MB_OK | MB_ICONWARNING);
}
std::vector<unsigned char> identityResource(HMODULE module) {
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(200), RT_RCDATA);
  const DWORD size = resource ? SizeofResource(module, resource) : 0;
  HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
  const auto* bytes = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
  return bytes && size && size <= 65536 ? std::vector<unsigned char>(bytes, bytes + size)
                                       : std::vector<unsigned char>{};
}
bool matchingBuildIdentity(HMODULE launcher, const std::filesystem::path& extension) {
  // Resource-only mapping: no DllMain, imports, version export or Qt initializer.
  HMODULE data = LoadLibraryExW(extension.c_str(), nullptr,
      LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!data) return false;
  const auto own = identityResource(launcher), other = identityResource(data);
  FreeLibrary(data);
  return !own.empty() && own == other;
}
std::wstring systemFailure(const std::wstring& stage, DWORD code) {
  return stage + L"失败（Windows 错误 " + std::to_wstring(code) + L"）。";
}
bool injectDll(HANDLE process, DWORD pid, const std::filesystem::path& path,
               DWORD waitBudget, bool* unconfirmed, std::wstring* error) {
  const ULONGLONG loadDeadline = GetTickCount64() + waitBudget;
  const std::wstring name = path.wstring();
  const SIZE_T bytes = (name.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) { *error = systemFailure(L"分配扩展路径", GetLastError()); return false; }
  SIZE_T written = 0;
  kqpet::launcher::ModuleProbeFailure probeFailure;
  LPTHREAD_START_ROUTINE load = kqpet::launcher::resolveRemoteLoadLibrary(pid,
      std::min(loadDeadline, GetTickCount64() + 2000), &probeFailure);
  if (!load) {
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    *error = systemFailure(probeFailure.stage, probeFailure.systemError);
    return false;
  }
  const BOOL pathWritten = WriteProcessMemory(process, remote, name.c_str(), bytes, &written);
  if (!pathWritten || written != bytes) {
    const DWORD writeError = pathWritten ? ERROR_PARTIAL_COPY : GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    *error = systemFailure(L"写入完整 DLL 路径", writeError) +
        L" 已写入 " + std::to_wstring(written) + L" / " + std::to_wstring(bytes) + L" 字节。";
    return false;
  }
  HANDLE thread = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
  if (!thread) {
    const DWORD createError = GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    *error = systemFailure(L"创建扩展加载线程", createError);
    return false;
  }
  const auto waitStarted = GetTickCount64();
  const DWORD waited = WaitForSingleObject(thread,
      waitStarted < loadDeadline ? static_cast<DWORD>(loadDeadline - waitStarted) : 0);
  if (waited != WAIT_OBJECT_0) {
    // LoadLibrary may still read this memory. Keep it until process exit, and
    // never issue a second injection to compensate for an uncertain first one.
    *unconfirmed = true;
    *error = L"扩展初始化尚未确认；加载可能稍后完成，请勿重复注入。";
    CloseHandle(thread);
    return false;
  }
  DWORD auxiliaryExitCode = 0;
  GetExitCodeThread(thread, &auxiliaryExitCode); // Not a 64-bit HMODULE.
  CloseHandle(thread);
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  // The host may still be loading other modules, making a Toolhelp snapshot
  // temporarily unavailable. Retry only the read-only confirmation; never
  // create another remote loading thread for this process.
  const auto loaded = kqpet::launcher::findRemoteModule(pid, path.filename().wstring(),
      std::min(loadDeadline, GetTickCount64() + 1000), &probeFailure);
  if (!loaded.base || !samePath(loaded.path, path.wstring())) {
    *error = loaded.base ? L"已加载扩展模块的路径与本次版本不匹配。"
        : systemFailure(L"确认扩展模块", probeFailure.systemError);
    return false;
  }
  return true;
}
DWORD remaining(ULONGLONG deadline, DWORD maximum) {
  const auto now = GetTickCount64();
  return now < deadline ? static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, maximum)) : 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  wchar_t launcherPath[32768]{};
  const DWORD length = GetModuleFileNameW(instance, launcherPath, 32768);
  if (!length || length >= 32768) return 1;
  const auto versionDirectory = std::filesystem::path(launcherPath).parent_path();
  std::filesystem::path clientRoot;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> gameArguments;
  bool hasClientRoot = false;
  bool gameOptions = false;
  for (int index = 1; argv && index < argc; ++index) {
    if (!gameOptions && std::wstring(argv[index]) == L"--") {
      gameOptions = true;
    } else if (!gameOptions && std::wstring(argv[index]) == L"--client-root") {
      if (hasClientRoot || index + 1 == argc) { LocalFree(argv); return 64; }
      clientRoot = argv[++index]; hasClientRoot = true;
    } else if (gameOptions) gameArguments.emplace_back(argv[index]);
    else { LocalFree(argv); return 64; }
  }
  if (argv) LocalFree(argv);
  kqpet::release::Failure pathFailure;
  if (!hasClientRoot || !kqpet::release::safeDirectory(clientRoot, &clientRoot, &pathFailure)) {
    showError(L"版本加载器需要明确且有效的 --client-root；请从原版目录的稳定启动入口运行。");
    return 64;
  }
  auto dataRoot = kqpet::launcher::resolveDataRoot(clientRoot, kqpet::launcher::inheritedDataRoot());
  if (!dataRoot.warning.empty()) showError(dataRoot.warning);
  std::wstring migrationNotice;
  if (!dataRoot.pendingRoot.empty()) {
    const bool hostStopped = kqpet::release::originalProcessState(clientRoot) == kqpet::release::ProcessState::Stopped;
    if (!kqpet::launcher::prepareDataRootMigration(instance, clientRoot, &dataRoot, hostStopped, &migrationNotice))
      showError(migrationNotice);
  }
  std::wstring dataRootError;
  if (!kqpet::launcher::applyDataRootEnvironment(dataRoot, clientRoot, &dataRootError)) {
    showError(dataRootError);
    return 65;
  }
  const auto original = kqpet::launcher::findClientExecutable(clientRoot);
  if (clientRoot.empty() || original.empty()) { showError(L"指定目录没有可识别接口的原版客户端。"); return 2; }
  const auto extension = versionDirectory / L"KQPetInventory.dll";
  const auto disk = kqpet::compatibility::checkTargetFile(original.wstring(), extension.wstring());
  // A valid manifest is necessary, but only an active/recovered record grants
  // launch authority. An arbitrary pending directory is never loaded directly.
  const auto selected = kqpet::release::resolveRelease(clientRoot);
  bool extensionAllowed = disk.supported && selected.ok && selected.release.valid &&
      selected.release.manifest.releaseId == KQPET_RELEASE_ID &&
      samePath(selected.release.directory.wstring(), versionDirectory.wstring()) &&
      matchingBuildIdentity(instance, extension);
  std::wstring error;
  std::shared_ptr<Channel> channel;
  if (extensionAllowed) {
    channel = Channel::create(KQPET_RELEASE_ID_WSTRING, disk.profile->id, &error);
    extensionAllowed = bool(channel);
  } else if (!disk.supported) {
    error = L"当前客户端接口或 Qt 运行库无法匹配，扩展未加载；本次仅启动原版。\r\n" +
        std::wstring(disk.error.begin(), disk.error.end());
  } else error = L"扩展激活记录、manifest 或成对构建身份未通过验证；本次仅启动原版。";
  std::wstring command = quoteArgument(original.wstring());
  for (const auto& argument : gameArguments) command += L" " + quoteArgument(argument);
  auto environment = channel ? channel->childEnvironment() : std::vector<wchar_t>{};
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process{};
  const DWORD flags = CREATE_SUSPENDED | (channel ? CREATE_UNICODE_ENVIRONMENT : 0);
  if (!CreateProcessW(original.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
      channel ? environment.data() : nullptr, clientRoot.c_str(), &startup, &process)) {
    showError(L"无法启动原版客户端。"); return 5;
  }
  if (channel && !channel->bindChild(process.hProcess, process.dwProcessId)) {
    extensionAllowed = false;
    error = L"无法绑定本次启动的进程身份，扩展未加载。";
  }
  const ULONGLONG deadline = GetTickCount64() + 30000;
  ResumeThread(process.hThread);
  CloseHandle(process.hThread);
  if (!extensionAllowed) { CloseHandle(process.hProcess); showError(error); return 6; }
  WaitForInputIdle(process.hProcess, remaining(deadline, 20000));
  bool modulesReady = false;
  while (remaining(deadline, 30000) && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
    if (kqpet::compatibility::checkProcessModulePaths(process.dwProcessId, disk).supported) { modulesReady = true; break; }
    Sleep(50);
  }
  bool unconfirmed = false;
  if (!modulesReady || !injectDll(process.hProcess, process.dwProcessId, extension,
                                  remaining(deadline, 15000), &unconfirmed, &error)) {
    CloseHandle(process.hProcess);
    showError(modulesReady ? error : L"原版实际运行模块未通过路径/架构/ABI预检，扩展未加载。");
    return unconfirmed ? 8 : 6;
  }
  while (channel->state() != State::Ready && channel->state() != State::Failed &&
         remaining(deadline, 30000) && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
    channel->wait(remaining(deadline, 250));
  const State state = channel->state();
  CloseHandle(process.hProcess);
  if (state == State::Ready) return 0;
  if (state == State::Failed) {
    showError(L"扩展初始化失败，错误码 " + std::to_wstring(channel->errorCode()) + L"。原版可继续使用。");
    return 7;
  }
  showError(L"扩展初始化尚未确认（30秒）。加载器已退出，迟到结果会显示在本机诊断与扩展入口；未卸载模块或终止原版。");
  return 8;
}
