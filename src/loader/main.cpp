#include <windows.h>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kExtensionDllName[] = L"KQPetInventory.dll";
constexpr wchar_t kPendingExtensionDllName[] = L"KQPetInventory.pending.dll";

std::wstring lower(std::wstring value) {
  for (wchar_t& character : value) character = std::towlower(character);
  return value;
}

std::vector<int> versionNumbers(const std::wstring& filename) {
  std::vector<int> result;
  int current = -1;
  for (wchar_t character : filename) {
    if (std::iswdigit(character)) {
      if (current < 0) current = 0;
      current = current * 10 + (character - L'0');
    } else if (current >= 0) {
      result.push_back(current);
      current = -1;
    }
  }
  if (current >= 0) result.push_back(current);
  return result;
}

bool newerVersion(const std::vector<int>& left, const std::vector<int>& right) {
  const size_t count = (std::max)(left.size(), right.size());
  for (size_t index = 0; index < count; ++index) {
    const int leftPart = index < left.size() ? left[index] : 0;
    const int rightPart = index < right.size() ? right[index] : 0;
    if (leftPart != rightPart) return leftPart > rightPart;
  }
  return false;
}

std::filesystem::path findOriginalExe(const std::filesystem::path& directory) {
  std::filesystem::path best;
  std::vector<int> bestVersion;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error || !entry.is_regular_file(error)) continue;
    const std::filesystem::path path = entry.path();
    const std::wstring filename = lower(path.filename().wstring());
    if (path.extension() != L".exe" && lower(path.extension().wstring()) != L".exe") continue;
    if (filename.rfind(L"kqpro", 0) != 0) continue;
    const std::vector<int> version = versionNumbers(filename);
    if (best.empty() || newerVersion(version, bestVersion)) {
      best = path;
      bestVersion = version;
    }
  }
  return best;
}

std::wstring winError(DWORD code) {
  wchar_t* text = nullptr;
  const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                        FORMAT_MESSAGE_FROM_SYSTEM |
                                        FORMAT_MESSAGE_IGNORE_INSERTS,
                                    nullptr, code, 0,
                                    reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring result = size && text ? std::wstring(text, size) : L"未知错误";
  if (text)
    LocalFree(text);
  return result;
}

#if 0  // Legacy hash checker retained only for source-history context; no version binding.
bool sha256File(const std::filesystem::path& path, std::wstring* hex, std::wstring* error) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = L"无法读取原版程序：" + winError(GetLastError());
    return false;
  }

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<UCHAR> hashObject;
  std::array<UCHAR, 32> digest{};
  bool ok = false;

  do {
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
      *error = L"无法初始化 SHA-256。";
      break;
    }

    DWORD objectLength = 0;
    DWORD returned = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &returned, 0) < 0) {
      *error = L"无法读取 SHA-256 参数。";
      break;
    }
    hashObject.resize(objectLength);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                         nullptr, 0, 0) < 0) {
      *error = L"无法创建 SHA-256 计算器。";
      break;
    }

    // Keep the 1 MiB I/O buffer off the default Windows thread stack. A local
    // std::array of this size can exhaust the launcher's 1 MiB stack before
    // the original client is even started.
    std::vector<UCHAR> buffer(1024 * 1024);
    DWORD read = 0;
    BOOL readSucceeded = TRUE;
    while ((readSucceeded = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                     &read, nullptr)) && read != 0) {
      if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
        *error = L"计算原版程序 SHA-256 时失败。";
        break;
      }
    }
    if (!error->empty())
      break;
    if (!readSucceeded) {
      *error = L"读取原版程序时失败：" + winError(GetLastError());
      break;
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
      *error = L"无法完成 SHA-256 计算。";
      break;
    }

    std::wostringstream out;
    out << std::uppercase << std::hex << std::setfill(L'0');
    for (UCHAR byte : digest)
      out << std::setw(2) << static_cast<unsigned>(byte);
    *hex = out.str();
    ok = true;
  } while (false);

  if (hash)
    BCryptDestroyHash(hash);
  if (algorithm)
    BCryptCloseAlgorithmProvider(algorithm, 0);
  CloseHandle(file);
  return ok;
}
#endif

bool injectDll(HANDLE process, const std::filesystem::path& dllPath, std::wstring* error) {
  const std::wstring path = dllPath.wstring();
  const SIZE_T byteCount = (path.size() + 1) * sizeof(wchar_t);
  void* remotePath = VirtualAllocEx(process, nullptr, byteCount,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remotePath) {
    *error = L"无法在原版进程中分配扩展路径：" + winError(GetLastError());
    return false;
  }

  bool ok = false;
  do {
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, path.c_str(), byteCount, &written) ||
        written != byteCount) {
      *error = L"无法把扩展路径写入原版进程：" + winError(GetLastError());
      break;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    if (!loadLibrary) {
      *error = L"无法定位 LoadLibraryW。";
      break;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!thread) {
      *error = L"无法加载精灵扩展 DLL：" + winError(GetLastError());
      break;
    }
    const DWORD waitResult = WaitForSingleObject(thread, 15000);
    DWORD moduleResult = 0;
    if (waitResult != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &moduleResult) ||
        moduleResult == 0) {
      *error = L"精灵扩展 DLL 未能在原版进程中启动。";
      CloseHandle(thread);
      break;
    }
    CloseHandle(thread);
    ok = true;
  } while (false);

  VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
  return ok;
}

void showError(const std::wstring& text) {
  MessageBoxW(nullptr, text.c_str(), L"原版氪奇精灵扩展", MB_OK | MB_ICONERROR);
}

bool activatePendingExtension(const std::filesystem::path& directory,
                              std::wstring* error) {
  const std::filesystem::path pending = directory / kPendingExtensionDllName;
  if (!std::filesystem::exists(pending)) return true;
  const std::filesystem::path extension = directory / kExtensionDllName;
  std::error_code copyError;
  std::filesystem::copy_file(pending, extension,
                             std::filesystem::copy_options::overwrite_existing,
                             copyError);
  if (copyError) {
    *error = L"检测到待安装的扩展更新，但当前 DLL 仍被占用。请先关闭正在运行的原版氪奇，"
             L"再重新运行 KQPetLauncher.exe。\n\n系统错误：" +
             winError(static_cast<DWORD>(copyError.value()));
    return false;
  }
  std::error_code removeError;
  std::filesystem::remove(pending, removeError);
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
  wchar_t launcherPath[MAX_PATH]{};
  if (!GetModuleFileNameW(instance, launcherPath, MAX_PATH)) {
    showError(L"无法取得启动器路径。");
    return 1;
  }

  const std::filesystem::path directory = std::filesystem::path(launcherPath).parent_path();
  std::wstring error;
  if (!activatePendingExtension(directory, &error)) {
    showError(error);
    return 2;
  }
  const std::filesystem::path originalExe = findOriginalExe(directory);
  const std::filesystem::path extensionDll = directory / kExtensionDllName;
  if (originalExe.empty() || !std::filesystem::exists(extensionDll)) {
    showError(L"请把 KQPetLauncher.exe 与 KQPetInventory.dll 放到 KQPro*.exe 同一目录。\n"
              L"原版 EXE 不需要也不会被替换。");
    return 2;
  }

  std::wstring commandLine = L"\"" + originalExe.wstring() + L"\"";
  if (arguments && *arguments) {
    commandLine += L" ";
    commandLine += arguments;
  }
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION processInfo{};
  if (!CreateProcessW(originalExe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, directory.c_str(), &startup, &processInfo)) {
    showError(L"无法启动原版氪奇：" + winError(GetLastError()));
    return 5;
  }

  WaitForInputIdle(processInfo.hProcess, 20000);
  Sleep(500);
  const bool injected = injectDll(processInfo.hProcess, extensionDll, &error);
  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  if (!injected) {
    showError(error + L"\n\n原版进程没有被修改；可以关闭后重试。 ");
    return 6;
  }
  return 0;
}
