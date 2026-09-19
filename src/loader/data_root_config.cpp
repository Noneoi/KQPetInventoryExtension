#include "data_root_config.h"
#include "command_line.h"
#include "bootstrap/strict_json.h"

#include <windows.h>
#include <cstring>
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
// Directory identity comes from the open handle, never from the spelling. The
// cache manager canonicalizes the destination with GetFullPath (which expands
// 8.3 aliases against the file system) while the launcher only normalizes
// lexically, so the two sides of a committed migration can spell one existing
// directory differently.
struct DirectoryIdentityKey {
  unsigned long long volume = 0;
  unsigned char fileId[16]{};
  bool operator==(const DirectoryIdentityKey& other) const {
    return volume == other.volume && std::memcmp(fileId, other.fileId, sizeof(fileId)) == 0;
  }
};
bool openPlainDirectory(const std::filesystem::path& path, HANDLE* handle, DWORD* error) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) { *error = GetLastError(); return false; }
  // A link is not the requested directory even when it resolves to it. The
  // migration path must not start following a reparse point before deciding.
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    *error = ERROR_DIRECTORY;
    return false;
  }
  HANDLE opened = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (opened == INVALID_HANDLE_VALUE) { *error = GetLastError(); return false; }
  BY_HANDLE_FILE_INFORMATION information{};
  const bool readable = GetFileInformationByHandle(opened, &information);
  const DWORD code = readable ? ERROR_SUCCESS : GetLastError();
  const bool plain = readable && (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
      !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
  if (!plain) {
    CloseHandle(opened);
    *error = readable ? ERROR_DIRECTORY : (code ? code : ERROR_DIRECTORY);
    return false;
  }
  *handle = opened;
  return true;
}
}
// A shared identity never makes a linked path acceptable. The cache manager
// walks the same chain before copying, so a linked component that reaches this
// point is a race or an externally edited configuration.
bool plainDirectoryPath(const std::filesystem::path& absolute, DWORD* error) {
  if (!absolute.is_absolute()) { *error = ERROR_INVALID_NAME; return false; }
  const std::filesystem::path normalized = absolute.lexically_normal();
  std::filesystem::path current = normalized.root_path();
  if (current.empty()) { *error = ERROR_INVALID_NAME; return false; }
  HANDLE root = INVALID_HANDLE_VALUE;
  if (!openPlainDirectory(current, &root, error)) return false;
  CloseHandle(root);
  for (const auto& component : normalized.relative_path()) {
    if (component.empty() || component == std::filesystem::path(L".")) continue;
    if (component == std::filesystem::path(L"..")) { *error = ERROR_INVALID_NAME; return false; }
    current /= component;
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!openPlainDirectory(current, &handle, error)) return false;
    CloseHandle(handle);
  }
  return true;
}
namespace {
bool extendedIdentity(HANDLE handle, DirectoryIdentityKey* key) {
  FILE_ID_INFO information{};
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &information, sizeof(information))) return false;
  key->volume = information.VolumeSerialNumber;
  static_assert(sizeof(information.FileId.Identifier) == sizeof(key->fileId), "file id size");
  std::memcpy(key->fileId, information.FileId.Identifier, sizeof(key->fileId));
  return true;
}
bool legacyIdentity(HANDLE handle, DirectoryIdentityKey* key) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information)) return false;
  // A zero volume serial or a zero file index identifies nothing on the file
  // systems that report them, so they are refused instead of matching hits.
  if (!information.dwVolumeSerialNumber || (!information.nFileIndexHigh && !information.nFileIndexLow)) return false;
  key->volume = information.dwVolumeSerialNumber;
  key->fileId[0] = static_cast<unsigned char>(information.nFileIndexHigh >> 24);
  key->fileId[1] = static_cast<unsigned char>(information.nFileIndexHigh >> 16);
  key->fileId[2] = static_cast<unsigned char>(information.nFileIndexHigh >> 8);
  key->fileId[3] = static_cast<unsigned char>(information.nFileIndexHigh);
  key->fileId[4] = static_cast<unsigned char>(information.nFileIndexLow >> 24);
  key->fileId[5] = static_cast<unsigned char>(information.nFileIndexLow >> 16);
  key->fileId[6] = static_cast<unsigned char>(information.nFileIndexLow >> 8);
  key->fileId[7] = static_cast<unsigned char>(information.nFileIndexLow);
  return true;
}
}

std::wstring inheritedDataRoot() {
  const DWORD length = GetEnvironmentVariableW(L"KQPET_DATA_ROOT", nullptr, 0);
  if (!length || length > 32767) return {};
  std::vector<wchar_t> value(length);
  const DWORD read = GetEnvironmentVariableW(L"KQPET_DATA_ROOT", value.data(), length);
  return read && read < length ? std::wstring(value.data(), read) : std::wstring{};
}

DirectoryIdentity compareDirectoryIdentity(const std::filesystem::path& left,
                                          const std::filesystem::path& right, DWORD* error) {
  if (error) *error = ERROR_SUCCESS;
  const auto unverifiable = [error](DWORD code) {
    if (error) *error = code ? code : ERROR_INVALID_FUNCTION;
    return DirectoryIdentity::Unverifiable;
  };
  HANDLE leftHandle = INVALID_HANDLE_VALUE, rightHandle = INVALID_HANDLE_VALUE;
  DWORD code = ERROR_SUCCESS;
  if (!openPlainDirectory(left, &leftHandle, &code)) return unverifiable(code);
  if (!openPlainDirectory(right, &rightHandle, &code)) {
    CloseHandle(leftHandle);
    return unverifiable(code);
  }
  DirectoryIdentityKey leftKey, rightKey;
  if (!extendedIdentity(leftHandle, &leftKey) || !extendedIdentity(rightHandle, &rightKey)) {
    code = GetLastError();
    // Filesystems without FILE_ID_INFO still report the legacy identity fields.
    // Both sides must come from the same reporting method to be comparable.
    leftKey = DirectoryIdentityKey{};
    rightKey = DirectoryIdentityKey{};
    if (!legacyIdentity(leftHandle, &leftKey) || !legacyIdentity(rightHandle, &rightKey)) {
      const DWORD legacyCode = GetLastError();
      CloseHandle(rightHandle);
      CloseHandle(leftHandle);
      return unverifiable(legacyCode ? legacyCode : code);
    }
  }
  CloseHandle(rightHandle);
  CloseHandle(leftHandle);
  return leftKey == rightKey ? DirectoryIdentity::Same : DirectoryIdentity::Different;
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
  if (!committed.configured || !committed.pendingRoot.empty())
    return failed(L"缓存迁移的目录配置尚未提交");
  // The committed root must be the very directory that was requested. The
  // cache manager rewrites the destination through GetFullPath, so a spelling
  // difference is normal and only a file-system identity comparison can tell
  // "same directory, other alias" apart from "other directory".
  DWORD identityError = ERROR_SUCCESS;
  if (!plainDirectoryPath(selection->pendingRoot, &identityError) ||
      !plainDirectoryPath(committed.dataRoot, &identityError))
    return failed(L"缓存迁移的目录链包含链接、无效组件或无法确认的目录（Windows 错误 " +
                  std::to_wstring(identityError) + L"）");
  const DirectoryIdentity identity =
      compareDirectoryIdentity(selection->pendingRoot, committed.dataRoot, &identityError);
  if (identity == DirectoryIdentity::Different) return failed(L"缓存迁移提交的目录与请求的目录不是同一目录");
  if (identity != DirectoryIdentity::Same)
    return failed(L"无法确认缓存迁移提交的目录身份（Windows 错误 " + std::to_wstring(identityError) + L"）");
  *selection = std::move(committed);
  if (notice) *notice = L"缓存迁移完成，原目录已保留。";
  return true;
}
}
