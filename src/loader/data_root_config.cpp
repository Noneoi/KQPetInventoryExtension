#include "data_root_config.h"
#include "../bootstrap/strict_json.h"

#include <windows.h>
#include <cwchar>
#include <fstream>
#include <vector>

namespace kqpet::launcher {
namespace {
std::wstring wide(const std::string& value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) return {};
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), result.data(), count);
  return result;
}
bool validRoot(const std::wstring& text, std::filesystem::path* result) {
  if (text.empty() || text.size() > 32000 || text.find(L'\0') != std::wstring::npos ||
      text.rfind(L"\\\\?\\", 0) == 0 || text.rfind(L"\\\\.\\", 0) == 0) return false;
  for (const wchar_t c : text) if (c < 32) return false;
  const std::filesystem::path path(text);
  if (!path.is_absolute()) return false;
  *result = path.lexically_normal();
  return true;
}
std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) return {};
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      result.data(), count, nullptr, nullptr);
  return result;
}
std::wstring quoteArgument(const std::wstring& value) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t c : value) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
    result += c; slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  return result + L"\"";
}
bool writeFile(const std::filesystem::path& path, const std::string& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
      written == bytes.size() && FlushFileBuffers(file);
  CloseHandle(file);
  return ok;
}
std::string readResult(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::string bytes(1024 * 1024, '\0');
  file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(file.gcount()));
  const auto end = bytes.find_last_not_of("\r\n \t");
  if (end == std::string::npos) return {};
  bytes.resize(end + 1);
  const auto start = bytes.find_last_of("\r\n");
  return start == std::string::npos ? bytes : bytes.substr(start + 1);
}
std::vector<wchar_t> powershellEnvironment() {
  std::vector<wchar_t> result;
  const LPWCH source = GetEnvironmentStringsW();
  if (!source) return result;
  for (const wchar_t* entry = source; *entry; entry += std::wcslen(entry) + 1) {
    // A launcher started from PowerShell 7 can inherit a module path that
    // prevents Windows PowerShell 5.1 from discovering its own core modules.
    if (!_wcsnicmp(entry, L"PSModulePath=", 13)) continue;
    result.insert(result.end(), entry, entry + std::wcslen(entry) + 1);
  }
  FreeEnvironmentStringsW(source);
  result.push_back(L'\0');
  return result;
}
struct MigrationFiles {
  std::filesystem::path directory, script, request, result;
  bool create() {
    wchar_t temporary[32768]{};
    const DWORD size = GetTempPathW(32768, temporary);
    if (!size || size >= 32768) return false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      const auto candidate = std::filesystem::path(temporary) / (L"KQPetMigration-" +
          std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
      if (!CreateDirectoryW(candidate.c_str(), nullptr)) continue;
      directory = candidate; script = directory / L"cache-manager.ps1";
      request = directory / L"request.json"; result = directory / L"result.txt";
      return true;
    }
    return false;
  }
  ~MigrationFiles() {
    // Only remove the three files in the directory we created. No recursive
    // cleanup can cross into a user-selected data root.
    if (directory.empty()) return;
    DeleteFileW(script.c_str()); DeleteFileW(request.c_str()); DeleteFileW(result.c_str());
    RemoveDirectoryW(directory.c_str());
  }
};
bool clearPendingFailure(DataRootSelection* selection, const std::wstring& message) {
  const auto temporary = std::filesystem::path(selection->configPath.wstring() + L".tmp-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  const std::string bytes = "{\"schema\":1,\"dataRoot\":" + release::json::quote(selection->dataRoot.u8string()) +
      ",\"migrationError\":" + release::json::quote(utf8(message)) + "}";
  const bool written = writeFile(temporary, bytes);
  const bool replaced = written && MoveFileExW(temporary.c_str(), selection->configPath.c_str(),
                                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!replaced) DeleteFileW(temporary.c_str());
  selection->pendingRoot.clear();
  return replaced;
}
}

std::wstring inheritedDataRoot() {
  const DWORD length = GetEnvironmentVariableW(L"KQPET_DATA_ROOT", nullptr, 0);
  if (!length || length > 32767) return {};
  std::vector<wchar_t> value(length);
  const DWORD read = GetEnvironmentVariableW(L"KQPET_DATA_ROOT", value.data(), length);
  return read && read < length ? std::wstring(value.data(), read) : std::wstring{};
}

DataRootSelection resolveDataRoot(const std::filesystem::path& clientRoot,
                                  const std::wstring& inheritedRoot) {
  DataRootSelection result;
  result.configPath = clientRoot / L"KQPetDataRoot.json";
  if (!validRoot(inheritedRoot, &result.dataRoot)) result.dataRoot = clientRoot / L"KQPetData";
  HANDLE file = CreateFileW(result.configPath.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
      result.warning = L"无法读取缓存目录配置，将继续使用原缓存目录（Windows 错误 " + std::to_wstring(code) + L"）。";
    return result;
  }
  LARGE_INTEGER size{};
  BY_HANDLE_FILE_INFORMATION info{};
  const bool readable = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 65536 &&
      GetFileInformationByHandle(file, &info) &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
  std::string bytes;
  DWORD count = 0;
  bool complete = false;
  if (readable) {
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
  }
  CloseHandle(file);
  if (bytes.rfind("\xef\xbb\xbf", 0) == 0) bytes.erase(0, 3);
  release::json::Value root;
  std::uint64_t schema = 0;
  std::filesystem::path configured;
  const bool valid = complete && release::json::Parser(bytes).parse(&root) &&
      root.kind == release::json::Value::Kind::Object && root.at("schema").unsignedInteger(&schema) && schema == 1 &&
      root.at("dataRoot").kind == release::json::Value::Kind::String &&
      validRoot(wide(root.at("dataRoot").text), &configured);
  if (!valid) {
    result.warning = L"缓存目录配置无效，将继续使用原缓存目录。可在设置中重新选择目录。";
    return result;
  }
  result.configured = true;
  result.dataRoot = configured;
  const auto pending = root.object.find("pendingRoot");
  if (pending != root.object.end() && pending->second.kind != release::json::Value::Kind::Null &&
      !(pending->second.kind == release::json::Value::Kind::String && pending->second.text.empty())) {
    if (pending->second.kind != release::json::Value::Kind::String ||
        !validRoot(wide(pending->second.text), &result.pendingRoot))
      result.warning = L"待迁移的缓存目录无效；继续使用原缓存目录，可在设置中重新选择。";
  }
  return result;
}

bool applyDataRootEnvironment(const DataRootSelection& selection,
                             const std::filesystem::path& clientRoot,
                             std::wstring* error) {
  if (SetEnvironmentVariableW(L"KQPET_DATA_ROOT", selection.dataRoot.c_str()) &&
      SetEnvironmentVariableW(L"KQPET_CLIENT_ROOT", clientRoot.c_str())) return true;
  if (error) *error = L"无法向客户端传递缓存目录（Windows 错误 " + std::to_wstring(GetLastError()) + L"）。";
  return false;
}

bool prepareDataRootMigration(HMODULE module, const std::filesystem::path& clientRoot,
                             DataRootSelection* selection, bool hostStopped, std::wstring* notice) {
  if (!selection || selection->pendingRoot.empty()) return true;
  const auto failed = [&](std::wstring reason) {
    if (reason.empty()) reason = L"缓存迁移未完成";
    const bool cleared = clearPendingFailure(selection, reason);
    if (notice) *notice = reason + L"。继续使用原缓存目录。" +
        (cleared ? L"本次迁移请求已取消，可在设置中重新安排。" : L"迁移请求未能清除，请在设置中检查目录权限。");
    return false;
  };
  if (!hostStopped) return failed(L"客户端仍在运行或无法确认退出，未迁移正在使用的缓存");
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(210), RT_RCDATA);
  const DWORD size = resource ? SizeofResource(module, resource) : 0;
  HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
  const char* content = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
  if (!content || !size || size > 2 * 1024 * 1024) return failed(L"启动器缺少缓存迁移组件");
  MigrationFiles files;
  if (!files.create()) return failed(L"无法准备缓存迁移临时目录");
  std::string script(content, content + size);
  // Windows PowerShell 5.1 needs the BOM to decode embedded Chinese messages.
  if (script.rfind("\xef\xbb\xbf", 0) != 0) script.insert(0, "\xef\xbb\xbf");
  const std::string request = "{\"mode\":\"migrate\",\"root\":" + release::json::quote(selection->dataRoot.u8string()) +
      ",\"clientRoot\":" + release::json::quote(clientRoot.u8string()) +
      ",\"destination\":" + release::json::quote(selection->pendingRoot.u8string()) + "}";
  if (!writeFile(files.script, script) || !writeFile(files.request, request)) return failed(L"无法写入缓存迁移临时文件");
  wchar_t system[32768]{};
  const UINT systemLength = GetSystemDirectoryW(system, 32768);
  if (!systemLength || systemLength >= 32768) return failed(L"无法找到系统 PowerShell");
  const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
  std::wstring command = quoteArgument(powershell.wstring()) +
      L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + quoteArgument(files.script.wstring()) +
      L" -RequestFile " + quoteArgument(files.request.wstring());
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE output = CreateFileW(files.result.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    return failed(L"无法准备缓存迁移输出");
  }
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = input; startup.hStdOutput = startup.hStdError = output;
  PROCESS_INFORMATION process{};
  auto environment = powershellEnvironment();
  const BOOL started = CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.empty() ? nullptr : environment.data(),
      clientRoot.c_str(), &startup, &process);
  const DWORD startError = started ? 0 : GetLastError();
  CloseHandle(input); CloseHandle(output);
  if (!started) return failed(L"缓存迁移程序无法启动（Windows 错误 " + std::to_wstring(startError) + L"）");
  CloseHandle(process.hThread);
  const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  const bool completed = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  if (!completed) return failed(L"无法确认缓存迁移进程完成");
  release::json::Value response;
  const bool parsed = release::json::Parser(readResult(files.result)).parse(&response);
  if (!parsed || exitCode != 0 || response.at("ok").kind != release::json::Value::Kind::Boolean || !response.at("ok").boolean)
    return failed(parsed ? wide(response.at("message").text) : L"缓存迁移没有返回有效结果");
  auto committed = resolveDataRoot(clientRoot, selection->dataRoot.wstring());
  if (!committed.configured || !committed.pendingRoot.empty() ||
      _wcsicmp(committed.dataRoot.c_str(), selection->pendingRoot.c_str()))
    return failed(L"缓存迁移的目录配置尚未提交");
  *selection = std::move(committed);
  if (notice) *notice = L"缓存迁移完成，原目录已保留。";
  return true;
}
}
