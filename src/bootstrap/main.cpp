#include "bootstrap.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(instance, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) return 2;
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return 64;
  std::vector<std::wstring> arguments;
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  LocalFree(argv);
  const auto clientRoot = std::filesystem::path(std::wstring(path.data(), length)).parent_path();
  const auto result = kqpet::bootstrap::launch(clientRoot, arguments);
  if (!result.started) {
    MessageBoxW(nullptr, L"扩展启动入口和原版启动均未完成。请使用本机部署诊断检查安装目录。",
        L"KQPet 启动状态", MB_OK | MB_ICONWARNING);
    return 2;
  }
  if (!result.extensionLoaderStarted) {
    MessageBoxW(nullptr, L"本次已启动原版，扩展未加载。请使用本机部署诊断检查 active、previous 与版本清单。",
        L"KQPet 启动状态", MB_OK | MB_ICONWARNING);
    return 6;
  }
  return 0;
}
