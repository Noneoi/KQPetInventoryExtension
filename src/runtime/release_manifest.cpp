#include "release_manifest.h"
#include "bootstrap/strict_json.h"
#include "compatibility/pe_view.h"
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <climits>
#include <vector>

namespace kqpet::release {
namespace fs = std::filesystem;
struct FilePins {
  std::vector<HANDLE> handles;
  ~FilePins() { for (HANDLE handle : handles) if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
namespace {
bool fail(Failure* error, Code code, const char* stage, const char* message, DWORD system = 0) {
  if (error) *error = {code, stage, message, system}; return false;
}
std::string upper(std::string text) { for (char& c : text) if (c >= 'a' && c <= 'f') c -= 'a' - 'A'; return text; }
bool equalPath(const fs::path& a, const fs::path& b) {
  const auto x = a.lexically_normal().wstring(), y = b.lexically_normal().wstring();
  return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
}
fs::path finalPath(HANDLE handle) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
  if (!length || length >= buffer.size()) return {};
  std::wstring value(buffer.data(), length);
  if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) value = L"\\\\" + value.substr(8);
  else if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
  return fs::path(value).lexically_normal();
}
bool safeComponent(const std::wstring& value) {
  if (value.empty() || value == L"." || value == L".." || value.back() == L'.' || value.back() == L' ') return false;
  for (wchar_t c : value) if (c < 32 || c == L':' || c == L'/' || c == L'\\' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') return false;
  return true;
}
bool directoryTree(const fs::path& absolute, fs::path* normalized, Failure* error, FilePins* pins = nullptr) {
  const auto spelling = absolute.wstring();
  if (!absolute.is_absolute() || spelling.rfind(L"\\\\?\\", 0) == 0 || spelling.rfind(L"\\\\.\\", 0) == 0)
    return fail(error, Code::InvalidPath, "path", "absolute ordinary directory required");
  fs::path path = absolute.root_path();
  auto inspect = [&](const fs::path& item) {
    const DWORD attributes = GetFileAttributesW(item.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return fail(error, Code::InvalidPath, "path", "directory absent or reparse component", GetLastError());
    HANDLE handle = CreateFileW(item.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return fail(error, Code::Io, "path", "cannot pin directory", GetLastError());
    BY_HANDLE_FILE_INFORMATION information{};
    const bool matching = GetFileInformationByHandle(handle, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        equalPath(finalPath(handle), item);
    if (pins && matching) pins->handles.push_back(handle); else CloseHandle(handle);
    return matching || fail(error, Code::InvalidPath, "path", "resolved directory differs from requested directory");
  };
  if (!inspect(path)) return false;
  for (const auto& component : absolute.relative_path()) {
    if (component.empty()) continue;
    if (!safeComponent(component.wstring())) return fail(error, Code::InvalidPath, "path", "invalid directory component");
    path /= component;
    if (!inspect(path)) return false;
  }
  if (normalized) *normalized = path.lexically_normal();
  return true;
}
HANDLE openPinned(const fs::path& path, std::uint64_t maximum, std::uint64_t* size, FilePins* pins, Failure* error) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) { fail(error, Code::Missing, "read", "required file missing", GetLastError()); return INVALID_HANDLE_VALUE; }
  if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { fail(error, Code::InvalidPath, "read", "ordinary file required"); return INVALID_HANDLE_VALUE; }
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  LARGE_INTEGER bytes{};
  BY_HANDLE_FILE_INFORMATION information{};
  if (file == INVALID_HANDLE_VALUE) { fail(error, Code::Io, "read", "cannot pin file", GetLastError()); return file; }
  if (!GetFileInformationByHandle(file, &information) || information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
      !GetFileSizeEx(file, &bytes) || bytes.QuadPart <= 0 || static_cast<std::uint64_t>(bytes.QuadPart) > maximum || !equalPath(finalPath(file), path)) {
    CloseHandle(file); fail(error, Code::InvalidPath, "read", "file size or resolved path rejected"); return INVALID_HANDLE_VALUE;
  }
  *size = static_cast<std::uint64_t>(bytes.QuadPart); pins->handles.push_back(file); return file;
}
bool readAll(HANDLE file, std::uint64_t size, std::string* bytes, Failure* error) {
  LARGE_INTEGER zero{};
  if (size > 16 * 1024 * 1024 || !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return fail(error, Code::Io, "read", "bounded record seek failed");
  bytes->resize(static_cast<std::size_t>(size)); DWORD read = 0;
  return (ReadFile(file, bytes->data(), static_cast<DWORD>(size), &read, nullptr) && read == size) || fail(error, Code::Io, "read", "record read incomplete", GetLastError());
}
std::string hashFile(HANDLE file, Failure* error) {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
      BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  LARGE_INTEGER zero{}; ok = ok && SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
  unsigned char buffer[65536], digest[32]{}; DWORD read = 0;
  while (ok) {
    if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) { ok = false; break; }
    if (!read) break;
    ok = BCryptHashData(hash, buffer, read, 0) >= 0;
  }
  ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) { fail(error, Code::Io, "hash", "SHA256 failed", GetLastError()); return {}; }
  constexpr char hex[] = "0123456789ABCDEF"; std::string result;
  for (unsigned char byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
enum class PeKind { Any, Executable, Dll };
bool checkPe(HANDLE file, std::uint64_t size, Failure* error, PeKind kind = PeKind::Any) {
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
  bool valid = false;
  if (view) {
    const compatibility::RawPeView pe(view, static_cast<std::size_t>(size));
    const auto flags = pe.fileCharacteristics();
    valid = pe.valid() && (flags & IMAGE_FILE_EXECUTABLE_IMAGE) &&
        (kind == PeKind::Any || bool(flags & IMAGE_FILE_DLL) == (kind == PeKind::Dll));
    UnmapViewOfFile(view);
  }
  if (mapping) CloseHandle(mapping);
  return valid || fail(error, Code::InvalidPe, "pe", "bounded AMD64 PE check failed");
}
bool field(const json::Value& object, const char* key, std::string* text, std::size_t maximum = 512) {
  const auto& value = object.at(key);
  if (value.kind != json::Value::Kind::String || value.text.empty() || value.text.size() > maximum) return false;
  for (unsigned char c : value.text) if (c < 32) return false;
  *text = value.text; return true;
}
bool constant(const json::Value& value, std::uint64_t expected) { std::uint64_t number = 0; return value.unsignedInteger(&number) && number == expected; }
bool artifact(const json::Value& value, const char* path, Artifact* result, bool includePath = true) {
  if (!value.keys(includePath ? std::initializer_list<const char*>{"path", "size", "sha256"} : std::initializer_list<const char*>{"size", "sha256"})) return false;
  result->path = path;
  if (includePath && (value.at("path").kind != json::Value::Kind::String || value.at("path").text != path)) return false;
  if (!value.at("size").unsignedInteger(&result->size) || !result->size || result->size > 512ULL * 1024 * 1024 ||
      !field(value, "sha256", &result->sha256, 64) || !sha256Text(result->sha256)) return false;
  result->sha256 = upper(result->sha256); return true;
}
bool identityValue(const json::Value& value, BuildIdentity* identity, Failure* error) {
  if (!value.keys({"schema", "releaseId", "version", "architecture", "qt", "sourceSha256", "profileSha256", "toolchain", "configuration", "startupProtocol", "bootstrapProtocol", "minHookCommit"}) ||
      !constant(value.at("schema"), 1) || !field(value, "releaseId", &identity->releaseId, 128) || !safeReleaseId(identity->releaseId) ||
      !field(value, "version", &identity->version, 64) || !field(value, "architecture", &identity->architecture, 16) ||
      !field(value, "qt", &identity->qt, 32) || !field(value, "sourceSha256", &identity->sourceSha256, 64) || !sha256Text(identity->sourceSha256) ||
      !field(value, "profileSha256", &identity->profileSha256, 64) || !sha256Text(identity->profileSha256) ||
      !field(value, "toolchain", &identity->toolchain) || !field(value, "configuration", &identity->configuration, 32) ||
      !field(value, "minHookCommit", &identity->minHookCommit, 40)) return fail(error, Code::InvalidIdentity, "identity", "build identity schema rejected");
  if (!constant(value.at("bootstrapProtocol"), kBootstrapProtocol) || !constant(value.at("startupProtocol"), kStartupProtocol) ||
      identity->architecture != "x64" || identity->qt != "6.6.3" || identity->configuration != "Release" ||
      identity->minHookCommit != "c3fcafdc10146beb5919319d0683e44e3c30d537")
    return fail(error, Code::UnsupportedProtocol, "identity", "unsupported protocol, architecture, Qt or dependency identity");
  identity->bootstrapProtocol = kBootstrapProtocol; identity->startupProtocol = kStartupProtocol;
  identity->sourceSha256 = upper(identity->sourceSha256); identity->profileSha256 = upper(identity->profileSha256);
  identity->canonicalJson = json::stringify(value); return true;
}
bool resourceJson(const fs::path& path, std::string* bytes, Failure* error, PeKind kind = PeKind::Any) {
  FilePins pins; std::uint64_t size = 0;
  fs::path parent; if (!directoryTree(path.parent_path(), &parent, error)) return false;
  const auto file = openPinned(parent / path.filename(), 512ULL * 1024 * 1024, &size, &pins, error);
  if (file == INVALID_HANDLE_VALUE || !checkPe(file, size, error, kind)) return false;
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module) return fail(error, Code::InvalidIdentity, "resource", "resource-only PE mapping failed", GetLastError());
  const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(200), RT_RCDATA);
  const DWORD length = resource ? SizeofResource(module, resource) : 0;
  const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
  const char* data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
  const bool valid = data && length && length <= 65536;
  if (valid) bytes->assign(data, length);
  FreeLibrary(module);
  return valid || fail(error, Code::InvalidIdentity, "resource", "RCDATA 200 missing or oversized");
}
}

const char* codeName(Code code) {
  switch (code) {
    case Code::Ok: return "ok"; case Code::InvalidPath: return "invalid_path"; case Code::Missing: return "missing";
    case Code::Io: return "io"; case Code::InvalidJson: return "invalid_json"; case Code::InvalidSchema: return "invalid_schema";
    case Code::InvalidIdentity: return "invalid_identity"; case Code::UnsupportedProtocol: return "unsupported_protocol";
    case Code::HashMismatch: return "hash_mismatch"; case Code::SizeMismatch: return "size_mismatch"; case Code::InvalidPe: return "invalid_pe";
    case Code::ReportMismatch: return "report_mismatch"; case Code::LockBusy: return "lock_busy"; case Code::Running: return "running";
    case Code::RunningUnknown: return "running_unknown"; case Code::InjectedFault: return "injected_fault";
    case Code::NoUsableRelease: return "no_usable_release"; case Code::LaunchFailed: return "launch_failed";
  }
  return "unknown";
}
std::string failureJson(const Failure& error) {
  return "{\"code\":" + json::quote(codeName(error.code)) + ",\"stage\":" + json::quote(error.stage) +
      ",\"message\":" + json::quote(error.message) + ",\"systemError\":" + std::to_string(error.systemError) + "}";
}
std::string pathUtf8(const fs::path& path) {
  const auto text = path.wstring(); if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (!size) return {}; std::string result(size, 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr); return result;
}
bool safeReleaseId(const std::string& id) {
  if (id.empty() || id.size() > 128 || id.find("..") != std::string::npos || id.back() == '.') return false;
  const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  if (!alnum(id.front())) return false;
  for (char c : id) if (!alnum(c) && c != '.' && c != '-' && c != '_') return false;
  std::string base = id.substr(0, id.find('.'));
  for (char& c : base) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
      (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") && base[3] >= '1' && base[3] <= '9')) return false;
  return true;
}
bool sha256Text(const std::string& value) {
  if (value.size() != 64) return false;
  for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
bool safeDirectory(const fs::path& path, fs::path* normalized, Failure* error) { return directoryTree(path, normalized, error); }
std::shared_ptr<FilePins> pinDirectory(const fs::path& path, Failure* error) {
  auto pins = std::make_shared<FilePins>();
  return directoryTree(path, nullptr, error, pins.get()) ? pins : std::shared_ptr<FilePins>{};
}
bool safeChild(const fs::path& root, const fs::path& relative, bool directory, fs::path* resolved, Failure* error) {
  fs::path base; if (!directoryTree(root, &base, error) || relative.empty() || relative.has_root_path()) return fail(error, Code::InvalidPath, "path", "relative child required");
  for (const auto& component : relative) if (!safeComponent(component.wstring())) return fail(error, Code::InvalidPath, "path", "unsafe relative child");
  const auto target = base / relative;
  if (directory) return directoryTree(target, resolved, error);
  fs::path parent; if (!directoryTree(target.parent_path(), &parent, error)) return false;
  const DWORD attributes = GetFileAttributesW(target.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return fail(error, Code::InvalidPath, "path", "child is absent or not an ordinary file");
  *resolved = target; return true;
}
bool parseBuildIdentity(const std::string& text, BuildIdentity* identity, Failure* error) {
  json::Value value; return (text.size() <= 65536 && json::Parser(text).parse(&value))
      ? identityValue(value, identity, error) : fail(error, Code::InvalidJson, "identity", "invalid UTF-8 identity JSON");
}
bool readBuildIdentity(const fs::path& pe, BuildIdentity* identity, Failure* error) {
  std::string bytes; return resourceJson(pe, &bytes, error) && parseBuildIdentity(bytes, identity, error);
}
bool readBootstrapProtocol(const fs::path& pe, unsigned* protocol, Failure* error) {
  std::string bytes; json::Value value; std::uint64_t number = 0;
  if (!resourceJson(pe, &bytes, error, PeKind::Executable)) return false;
  if (!json::Parser(bytes).parse(&value) || !value.keys({"schema", "kind", "bootstrapProtocol"}) ||
      !constant(value.at("schema"), 1) || value.at("kind").kind != json::Value::Kind::String || value.at("kind").text != "stable-bootstrap" ||
      !value.at("bootstrapProtocol").unsignedInteger(&number) || number > UINT_MAX)
    return fail(error, Code::InvalidIdentity, "bootstrap", "bootstrap protocol resource rejected");
  *protocol = static_cast<unsigned>(number); return true;
}
bool parseManifest(const std::string& text, Manifest* manifest, Failure* error) {
  json::Value value;
  if (text.size() > 1024 * 1024 || !json::Parser(text).parse(&value)) return fail(error, Code::InvalidJson, "manifest", "invalid UTF-8 manifest JSON");
  if (!value.keys({"schema", "releaseId", "identity", "artifacts", "tests"}) || !constant(value.at("schema"), 1) ||
      !field(value, "releaseId", &manifest->releaseId, 128) || !safeReleaseId(manifest->releaseId) ||
      !value.at("artifacts").keys({"loader", "extension"}) ||
      !artifact(value.at("artifacts").at("loader"), "KQPetLauncher.exe", &manifest->loader) ||
      !artifact(value.at("artifacts").at("extension"), "KQPetInventory.dll", &manifest->extension) ||
      !artifact(value.at("tests"), "test-report.json", &manifest->tests)) return fail(error, Code::InvalidSchema, "manifest", "manifest schema rejected");
  return identityValue(value.at("identity"), &manifest->identity, error) &&
      (manifest->releaseId == manifest->identity.releaseId || fail(error, Code::InvalidIdentity, "manifest", "manifest release and resource identity differ"));
}
bool verifyTestReport(const std::string& text, const Manifest& manifest, Failure* error) {
  json::Value value; std::string release, source, profile; Artifact loader, extension;
  std::uint64_t total = 0, passed = 0, failed = 0;
  if (!json::Parser(text).parse(&value) || !value.keys({"schema", "releaseId", "sourceSha256", "profileSha256", "artifacts", "suite", "evidence"}) ||
      !constant(value.at("schema"), 1) || !field(value, "releaseId", &release) || !field(value, "sourceSha256", &source, 64) ||
      !field(value, "profileSha256", &profile, 64) || release != manifest.releaseId || upper(source) != manifest.identity.sourceSha256 ||
      upper(profile) != manifest.identity.profileSha256 || !value.at("artifacts").keys({"loader", "extension"}) ||
      !artifact(value.at("artifacts").at("loader"), "KQPetLauncher.exe", &loader, false) ||
      !artifact(value.at("artifacts").at("extension"), "KQPetInventory.dll", &extension, false) ||
      loader.size != manifest.loader.size || loader.sha256 != manifest.loader.sha256 || extension.size != manifest.extension.size || extension.sha256 != manifest.extension.sha256 ||
      !value.at("suite").keys({"total", "passed", "failed"}) || !value.at("suite").at("total").unsignedInteger(&total) ||
      !value.at("suite").at("passed").unsignedInteger(&passed) || !value.at("suite").at("failed").unsignedInteger(&failed) ||
      !total || passed != total || failed || value.at("evidence").kind != json::Value::Kind::Array || value.at("evidence").array.empty())
    return fail(error, Code::ReportMismatch, "test_report", "test report is not a passing report bound to both exact artifacts");
  return true;
}
std::string sha256Bytes(const void* bytes, std::size_t size) {
  if (size > ULONG_MAX) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; unsigned char digest[32]{};
  const bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
      BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
      BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(bytes)), static_cast<ULONG>(size), 0) >= 0 &&
      BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) return {}; constexpr char hex[] = "0123456789ABCDEF"; std::string result;
  for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; } return result;
}
bool readRecord(const fs::path& path, std::string* bytes, Failure* error, std::uint64_t maximum) {
  FilePins pins; std::uint64_t size = 0; fs::path normalized;
  if (!directoryTree(path.parent_path(), &normalized, error)) return false;
  const HANDLE file = openPinned(path, maximum, &size, &pins, error);
  return file != INVALID_HANDLE_VALUE && readAll(file, size, bytes, error);
}
Validation validateRelease(const fs::path& runtimeRoot, const std::string& id, const std::string& expected, const ValidationOverrides* testsOnly) {
  Validation result; result.pins = std::make_shared<FilePins>(); fs::path root;
  if (!safeReleaseId(id) || (!expected.empty() && !sha256Text(expected))) { fail(&result.error, Code::InvalidPath, "release", "release ID or expected digest rejected"); return result; }
  if (!directoryTree(runtimeRoot, &root, &result.error, result.pins.get())) return result;
  result.directory = root / L"releases" / fs::path(id);
  if (!directoryTree(result.directory, nullptr, &result.error, result.pins.get())) return result;
  std::uint64_t size = 0;
  HANDLE file = openPinned(result.directory / L"manifest.json", 1024 * 1024, &size, result.pins.get(), &result.error);
  std::string text;
  if (file == INVALID_HANDLE_VALUE || !readAll(file, size, &text, &result.error)) return result;
  result.manifestSha256 = sha256Bytes(text.data(), text.size());
  if ((!expected.empty() && result.manifestSha256 != upper(expected))) { fail(&result.error, Code::HashMismatch, "manifest", "manifest digest differs from committed pointer"); return result; }
  if (!parseManifest(text, &result.manifest, &result.error)) return result;
  if (result.manifest.releaseId != id) { fail(&result.error, Code::InvalidIdentity, "manifest", "release directory and manifest identity differ"); return result; }
  for (const Artifact* artifact : {&result.manifest.loader, &result.manifest.extension, &result.manifest.tests}) {
    const auto path = result.directory / fs::path(artifact->path);
    HANDLE pinned = openPinned(path, artifact == &result.manifest.tests ? 16ULL * 1024 * 1024 : 512ULL * 1024 * 1024, &size, result.pins.get(), &result.error);
    if (pinned == INVALID_HANDLE_VALUE) return result;
    if (size != artifact->size) { fail(&result.error, Code::SizeMismatch, "artifact", "artifact size differs from manifest"); return result; }
    if (hashFile(pinned, &result.error) != artifact->sha256) { fail(&result.error, Code::HashMismatch, "artifact", "artifact SHA256 differs from manifest"); return result; }
    if (artifact == &result.manifest.tests) {
      if (!readAll(pinned, size, &text, &result.error) || !verifyTestReport(text, result.manifest, &result.error)) return result;
    } else {
      if (!(testsOnly && testsOnly->allowNonPeTestFiles) &&
          !checkPe(pinned, size, &result.error, artifact == &result.manifest.extension ? PeKind::Dll : PeKind::Executable)) return result;
      BuildIdentity identity;
      const bool read = testsOnly && testsOnly->identityReader ? testsOnly->identityReader(path, &identity, &result.error) : readBuildIdentity(path, &identity, &result.error);
      if (!read) return result;
      if (!(identity == result.manifest.identity)) { fail(&result.error, Code::InvalidIdentity, "artifact", "two PE resources and manifest must have identical build identities"); return result; }
    }
  }
  result.valid = true; return result;
}
std::string validationJson(const Validation& value) {
  return "{\"ok\":" + std::string(value.valid ? "true" : "false") + ",\"valid\":" + std::string(value.valid ? "true" : "false") + ",\"releaseId\":" + json::quote(value.manifest.releaseId) +
      ",\"manifestSha256\":" + json::quote(value.manifestSha256) + ",\"directory\":" + json::quote(pathUtf8(value.directory)) +
      ",\"error\":" + failureJson(value.error) + "}";
}
}
