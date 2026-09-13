#include "target_check.h"

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace kqpet::compatibility {
namespace {
class Handle final {
public:
  explicit Handle(HANDLE handle) : handle_(handle) {}
  ~Handle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
  operator HANDLE() const { return handle_; }
  bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
private:
  HANDLE handle_;
};
bool equalPath(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                               b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::wstring modulePath(HMODULE module) {
  std::vector<wchar_t> path(32768);
  const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  return count && count < path.size() ? std::wstring(path.data(), count) : std::wstring();
}
std::wstring canonicalPath(HANDLE file) {
  std::vector<wchar_t> path(32768);
  const DWORD count = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
  if (!count || count >= path.size()) return {};
  std::wstring result(path.data(), count);
  if (result.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + result.substr(8);
  if (result.compare(0, 4, L"\\\\?\\") == 0) result.erase(0, 4);
  return result;
}
std::wstring canonicalPath(const std::wstring& path) {
  Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  return file.valid() ? canonicalPath(file) : std::wstring();
}
bool readFile(const std::wstring& path, std::vector<unsigned char>* bytes, std::wstring* actualPath) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  LARGE_INTEGER size{};
  // Known client modules are far smaller. Bound malformed/unexpected files
  // before allocating; this is a compatibility input budget, not a PE claim.
  if (!file.valid() || !GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > 512LL * 1024 * 1024) return false;
  *actualPath = canonicalPath(file);
  if (actualPath->empty()) return false;
  bytes->resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  return ReadFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &read, nullptr) && read == bytes->size();
}
std::string sha256(const std::vector<unsigned char>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  BCRYPT_HASH_HANDLE hash = nullptr;
  unsigned char digest[32]{};
  bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  if (ok) ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
               BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) return {};
  std::ostringstream output;
  output << std::hex << std::uppercase << std::setfill('0');
  for (unsigned char value : digest) output << std::setw(2) << static_cast<unsigned>(value);
  return output.str();
}
std::wstring fileVersion(const std::wstring& path) {
  DWORD ignored = 0;
  const DWORD length = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (!length) return {};
  std::vector<unsigned char> bytes(length);
  if (!GetFileVersionInfoW(path.c_str(), 0, length, bytes.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
      !info || infoSize < sizeof(*info) || info->dwSignature != 0xfeef04bd) return {};
  std::wostringstream result;
  result << HIWORD(info->dwFileVersionMS) << L'.' << LOWORD(info->dwFileVersionMS)
         << L'.' << HIWORD(info->dwFileVersionLS);
  if (LOWORD(info->dwFileVersionLS)) result << L'.' << LOWORD(info->dwFileVersionLS);
  return result.str();
}
std::string quote(const std::string& value) {
  std::ostringstream result;
  result << '"';
  for (unsigned char ch : value) {
    if (ch == '"' || ch == '\\') result << '\\' << ch;
    else if (ch < 0x20) result << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch) << std::dec;
    else result << ch;
  }
  result << '"';
  return result.str();
}
const char* methodName(MatchMethod method) {
  switch (method) {
    case MatchMethod::KnownRva: return "known RVA";
    case MatchMethod::UniqueSignature: return "unique executable signature";
    case MatchMethod::AmbiguousSignature: return "ambiguous signature";
    case MatchMethod::Conflict: return "reviewed RVA conflict";
    default: return "not matched";
  }
}
}

std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(length, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                       result.data(), length, nullptr, nullptr);
  return result;
}

const Profile* findProfile(const std::wstring& executableName, const std::wstring& version) {
  for (const auto& profile : registeredProfiles()) {
    if (!version.empty() && profile.version != version) continue;
    for (const auto& alias : profile.executableNames)
      if (equalPath(alias, executableName)) return &profile;
  }
  return nullptr;
}

TargetReport identifyTargetImage(const PeView& executable) {
  TargetReport report;
  if (!executable.valid() || (executable.fileCharacteristics() & IMAGE_FILE_DLL)) {
    report.error = "target must be a bounded AMD64 executable PE: " + executable.error(); return report;
  }
  std::size_t bestEvidence = 0;
  for (const auto& family : registeredProfiles()) {
    const auto dispatch = executable.resolve(family.dispatch);
    const auto getter = executable.resolve(family.serviceGetter);
    const auto sender = executable.resolve(family.commandSender);
    if (!dispatch.matched() || !getter.matched() || !sender.matched()) continue;
    if (family.serviceGetter.masked) {
      std::int32_t displacement = 0;
      if (!executable.readRva(getter.rva + 9, &displacement, sizeof(displacement), true)) continue;
      const std::int64_t storageRva = std::int64_t(getter.rva) + 13 + displacement;
      // A singleton pointer may live in loader-zeroed .bss; only instruction
      // signatures need file-backed bytes. Its complete storage must still
      // remain in a declared readable image section.
      if (storageRva < 0 || storageRva > UINT32_MAX ||
          std::none_of(executable.sections().begin(), executable.sections().end(), [storageRva](const auto& section) {
            const std::uint64_t extent = (std::max)(section.virtualSize, section.rawSize);
            return (section.characteristics & IMAGE_SCN_MEM_READ) && storageRva >= section.rva &&
                std::uint64_t(storageRva - section.rva) + sizeof(std::uintptr_t) <= extent;
          })) continue;
    }
    if (dispatch.rva == getter.rva || dispatch.rva == sender.rva || getter.rva == sender.rva) continue;
    if (report.profile && (report.dispatch.rva != dispatch.rva ||
        report.serviceGetter.rva != getter.rva || report.commandSender.rva != sender.rva)) {
      report.profile = nullptr;
      report.error = "recognized interface families resolve conflicting executable entries"; return report;
    }
    const auto evidence = family.dispatch.signatureSize + family.serviceGetter.signatureSize + family.commandSender.signatureSize;
    if (!report.profile || evidence > bestEvidence) {
      report.profile = &family; report.dispatch = dispatch;
      report.serviceGetter = getter; report.commandSender = sender; bestEvidence = evidence;
    }
  }
  report.supported = report.profile != nullptr;
  if (!report.supported) report.error = "client interface signatures are missing or ambiguous; version and filename are not restrictions";
  return report;
}

TargetReport identifyTargetFile(const std::wstring& executablePath) {
  TargetReport report;
  std::vector<unsigned char> bytes;
  if (!readFile(executablePath, &bytes, &report.executablePath)) {
    report.error = "cannot read target or target exceeds the 512 MiB compatibility budget"; return report;
  }
  const auto path = report.executablePath;
  const RawPeView executable(bytes.data(), bytes.size());
  report = identifyTargetImage(executable);
  report.executablePath = path;
  report.executableSha256 = sha256(bytes);
  ModuleCheck exe{std::filesystem::path(path).filename().wstring(), path, fileVersion(path), 0,
                  executable.imageSize(), executable.timestamp(), executable.valid(), executable.error()};
  exe.headerSize = executable.headerSize();
  report.modules.push_back(exe);
  if (report.executableSha256.empty()) { report.supported = false; report.error = "cannot hash observed executable"; }
  return report;
}

TargetReport checkTargetFile(const std::wstring& executablePath, const std::wstring& extensionPath) {
  TargetReport report = identifyTargetFile(executablePath);
  if (!report.supported) return report;
  report.supported = false;
  std::vector<unsigned char> bytes;
  std::vector<ImportedSymbol> requiredSymbols;
  if (!extensionPath.empty()) {
    std::wstring actualExtension;
    if (!readFile(extensionPath, &bytes, &actualExtension)) {
      report.error = "cannot read extension import requirements"; return report;
    }
    const RawPeView extension(bytes.data(), bytes.size());
    if (!extension.importedSymbols(&requiredSymbols, &report.error)) return report;
  }
  const auto directory = std::filesystem::path(report.executablePath).parent_path();
  std::wstring observedQtVersion;
  auto required = report.profile->qtModules;
  required.push_back(report.profile->qcefViewModule);
  for (const auto& name : required) {
    std::wstring actualPath;
    ModuleCheck module;
    module.name = name;
    if (!readFile((directory / name).wstring(), &bytes, &actualPath)) {
      module.error = "required runtime module cannot be read";
    } else {
      module.path = actualPath;
      const RawPeView image(bytes.data(), bytes.size());
      module.imageSize = image.imageSize();
      module.headerSize = image.headerSize();
      module.timestamp = image.timestamp();
      module.version = fileVersion(actualPath);
      if (!equalPath(std::filesystem::path(actualPath).parent_path().wstring(), directory.wstring()) ||
          !equalPath(std::filesystem::path(actualPath).filename().wstring(), name))
        module.error = "runtime module resolves outside the verified client directory or alias";
      else if (!image.valid()) module.error = "runtime module is not a valid bounded AMD64 PE: " + image.error();
      else if (name != report.profile->qcefViewModule && module.version.compare(0, 2, L"6.") != 0)
        module.error = "runtime module requires Qt 6 AMD64 ABI";
      else if (name != report.profile->qcefViewModule && !observedQtVersion.empty() && module.version != observedQtVersion)
        module.error = "runtime Qt modules belong to different ABI builds";
      else if (name == report.profile->qcefViewModule) {
        report.qcefExportRva = image.executableExport(report.profile->qcefExecuteJavascriptSymbol, &module.error);
      }
      if (module.error.empty()) {
        for (const auto& symbol : requiredSymbols) {
          const std::wstring dependency(symbol.module.begin(), symbol.module.end());
          if (!equalPath(dependency, name)) continue;
          if (!image.exportAddress(symbol.name, symbol.ordinal, false, &module.error)) {
            module.error = "required extension import is unavailable: " + symbol.name + "; " + module.error;
            break;
          }
        }
      }
      module.valid = module.error.empty();
      if (module.valid && name != report.profile->qcefViewModule && observedQtVersion.empty()) observedQtVersion = module.version;
    }
    report.modules.push_back(module);
    if (!module.valid) { report.error = utf8(name) + ": " + module.error; return report; }
  }
  report.supported = true;
  return report;
}

TargetReport checkProcessModulePaths(std::uint32_t processId, const TargetReport& diskReport) {
  TargetReport report = diskReport;
  report.supported = false;
  if (!diskReport.supported || !diskReport.profile || diskReport.modules.empty()) {
    if (report.error.empty()) report.error = "missing successful disk module evidence";
    return report;
  }
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
  if (!snapshot.valid()) { report.error = "cannot enumerate actual process modules"; return report; }
  std::vector<MODULEENTRY32W> loaded;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Module32FirstW(snapshot, &entry)) { report.error = "process has no readable module snapshot"; return report; }
  do { loaded.push_back(entry); entry.dwSize = sizeof(entry); } while (Module32NextW(snapshot, &entry));
  if (GetLastError() != ERROR_NO_MORE_FILES) { report.error = "incomplete process module enumeration"; return report; }
  for (auto& module : report.modules) {
    const MODULEENTRY32W* selected = nullptr;
    for (const auto& candidate : loaded) {
      if (!equalPath(candidate.szModule, module.name)) continue;
      if (selected) { report.error = "duplicate runtime module basename: " + utf8(module.name); return report; }
      selected = &candidate;
    }
    if (!selected || !equalPath(canonicalPath(selected->szExePath), module.path) ||
        selected->modBaseSize != module.imageSize) {
      report.error = "actual process module path/image extent differs from verified disk module: " + utf8(module.name);
      module.valid = false;
      module.error = report.error;
      return report;
    }
    module.base = reinterpret_cast<std::uintptr_t>(selected->modBaseAddr);
  }
  Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
  if (!process.valid()) { report.error = "cannot read actual process module headers"; return report; }
  for (auto& module : report.modules) {
    // The bounded header length and image extent originate in the validated
    // disk report. Materialize only the remote headers, then reuse MappedPeView
    // rather than introducing another Win32/PowerShell PE parser.
    if (!module.headerSize || module.headerSize > module.imageSize ||
        module.imageSize > 512u * 1024u * 1024u) {
      report.error = "invalid prevalidated module extent"; return report;
    }
    std::vector<unsigned char> headerImage(module.imageSize);
    SIZE_T received = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(module.base),
                            headerImage.data(), module.headerSize, &received) || received != module.headerSize) {
      report.error = "cannot read complete remote PE headers: " + utf8(module.name);
      module.valid = false; module.error = report.error;
      return report;
    }
    const MappedPeView observed(headerImage.data(), headerImage.size());
    if (!observed.valid() || observed.imageSize() != module.imageSize ||
        observed.headerSize() != module.headerSize || observed.timestamp() != module.timestamp) {
      report.error = "actual module architecture/header identity differs from disk: " + utf8(module.name);
      module.valid = false; module.error = report.error;
      return report;
    }
  }
  report.executableBase = report.modules.front().base;
  report.supported = true;
  return report;
}

TargetReport checkCurrentProcess() {
  HMODULE compatibilityModule = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&checkCurrentProcess), &compatibilityModule);
  const auto ownPath = modulePath(compatibilityModule);
  TargetReport report = checkProcessModulePaths(GetCurrentProcessId(),
      checkTargetFile(currentExecutablePath(), equalPath(ownPath, currentExecutablePath()) ? std::wstring() : ownPath));
  if (!report.supported) return report;
  report.supported = false;
  for (auto& module : report.modules) {
    const MappedPeView mapped(reinterpret_cast<const void*>(module.base), module.imageSize);
    if (!mapped.valid() || mapped.imageSize() != module.imageSize || mapped.timestamp() != module.timestamp) {
      report.error = "mapped module headers differ from disk or are unreadable: " + utf8(module.name);
      module.valid = false; module.error = report.error;
      return report;
    }
    if (module.base == report.executableBase) {
      const EndpointMatch dispatch = mapped.resolve(report.profile->dispatch);
      const EndpointMatch getter = mapped.resolve(report.profile->serviceGetter);
      const EndpointMatch sender = mapped.resolve(report.profile->commandSender);
      if (!dispatch.matched() || !getter.matched() || !sender.matched() ||
          dispatch.rva != report.dispatch.rva || getter.rva != report.serviceGetter.rva ||
          sender.rva != report.commandSender.rva || dispatch.observedBytes != report.dispatch.observedBytes ||
          getter.observedBytes != report.serviceGetter.observedBytes || sender.observedBytes != report.commandSender.observedBytes) {
        report.error = "mapped/disk dynamically resolved endpoint conflict"; return report;
      }
      report.dispatch = dispatch; report.serviceGetter = getter; report.commandSender = sender;
    } else if (module.name == report.profile->qcefViewModule) {
      std::string error;
      const auto rva = mapped.executableExport(report.profile->qcefExecuteJavascriptSymbol, &error);
      if (!rva || rva != report.qcefExportRva) {
        report.error = "mapped/disk QCefView executable export conflict: " + error; return report;
      }
      report.qcefExecuteJavascript = module.base + rva;
    }
  }
  report.supported = true;
  return report;
}

bool requiredRuntimeModulesLoaded() {
  const auto observed = identifyTargetFile(currentExecutablePath());
  const Profile* profile = observed.profile;
  if (!observed.supported || !profile) return false;
  for (const auto& module : profile->qtModules) if (!GetModuleHandleW(module.c_str())) return false;
  return GetModuleHandleW(profile->qcefViewModule.c_str()) != nullptr;
}

std::wstring currentExecutablePath() { return modulePath(nullptr); }

std::string reportJson(const TargetReport& report) {
  std::ostringstream out;
  out << "{\"supported\":" << (report.supported ? "true" : "false")
      << ",\"error\":" << quote(report.error) << ",\"path\":" << quote(utf8(report.executablePath))
      << ",\"directory\":" << quote(utf8(std::filesystem::path(report.executablePath).parent_path().wstring()))
      << ",\"profile\":" << quote(report.profile ? utf8(report.profile->id) : "")
      << ",\"version\":" << quote(report.modules.empty() ? "" : utf8(report.modules.front().version))
      << ",\"qtVersion\":" << quote(report.modules.size() > 1 ? utf8(report.modules[1].version) : "")
      << ",\"sha256\":" << quote(report.executableSha256)
      << ",\"profileDigest\":" << quote(profileCatalogDigest()) << ",\"endpoints\":[";
  const EndpointMatch* matches[]{&report.dispatch, &report.serviceGetter, &report.commandSender};
  const char* names[]{"Dispatch", "ServiceGetter", "CommandSender"};
  for (std::size_t i = 0; i < 3; ++i) {
    if (i) out << ',';
    out << "{\"name\":" << quote(names[i]) << ",\"rva\":" << matches[i]->rva
        << ",\"method\":" << quote(methodName(matches[i]->method))
        << ",\"matchCount\":" << matches[i]->matchCount << ",\"error\":" << quote(matches[i]->error) << '}';
  }
  out << "],\"modules\":[";
  for (std::size_t i = 0; i < report.modules.size(); ++i) {
    const auto& module = report.modules[i];
    if (i) out << ',';
    out << "{\"name\":" << quote(utf8(module.name)) << ",\"path\":" << quote(utf8(module.path))
        << ",\"version\":" << quote(utf8(module.version)) << ",\"valid\":" << (module.valid ? "true" : "false")
        << ",\"error\":" << quote(module.error) << '}';
  }
  out << "]}";
  return out.str();
}

std::string profilesJson() {
  std::ostringstream out;
  out << "{\"schema\":1,\"profileDigest\":" << quote(profileCatalogDigest()) << ",\"profiles\":[";
  bool first = true;
  for (const auto& profile : registeredProfiles()) {
    if (!first) out << ',';
    first = false;
    out << "{\"id\":" << quote(utf8(profile.id)) << ",\"version\":" << quote(utf8(profile.version))
        << ",\"qtVersion\":" << quote(utf8(profile.qtVersion)) << ",\"executableNames\":[";
    for (std::size_t i = 0; i < profile.executableNames.size(); ++i) {
      if (i) out << ',';
      out << quote(utf8(profile.executableNames[i]));
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

}  // namespace kqpet::compatibility
