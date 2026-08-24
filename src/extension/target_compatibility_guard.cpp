#include "target_compatibility_guard.h"

#include "target_profile_registry.h"
#include "version.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <utility>
#include <vector>

namespace {

TargetCompatibilityReport gLastReport;

std::wstring widenAscii(const char* value) {
  std::wstring result;
  if (!value) return result;
  while (*value) result.push_back(static_cast<unsigned char>(*value++));
  return result;
}

std::wstring modulePath(HMODULE module) {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return {};
  return std::wstring(path.data(), length);
}

std::wstring fileName(const std::wstring& path) {
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring fileVersion(const std::wstring& path) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (!size) return {};
  std::vector<unsigned char> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
      !info || infoSize < sizeof(VS_FIXEDFILEINFO))
    return {};
  std::wostringstream stream;
  stream << HIWORD(info->dwFileVersionMS) << L'.' << LOWORD(info->dwFileVersionMS)
         << L'.' << HIWORD(info->dwFileVersionLS);
  const WORD build = LOWORD(info->dwFileVersionLS);
  if (build) stream << L'.' << build;
  return stream.str();
}

bool iequals(wchar_t left, wchar_t right) {
  return std::towlower(left) == std::towlower(right);
}

bool startsWithInsensitive(const std::wstring& text, const std::wstring& prefix) {
  return text.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), text.begin(), iequals);
}

bool endsWithInsensitive(const std::wstring& text, const std::wstring& suffix) {
  return text.size() >= suffix.size() &&
         std::equal(suffix.begin(), suffix.end(), text.end() - suffix.size(), iequals);
}

std::wstring versionFromKqProName(const std::wstring& name) {
  if (!startsWithInsensitive(name, L"KQPro") || !endsWithInsensitive(name, L".exe")) return {};
  const size_t end = name.size() - 4;
  size_t cursor = 5;
  if (cursor < end && (name[cursor] == L'V' || name[cursor] == L'v' ||
                       name[cursor] == L'-' || name[cursor] == L'_'))
    ++cursor;
  if (cursor < end && (name[cursor] == L'V' || name[cursor] == L'v')) ++cursor;
  const size_t begin = cursor;
  bool hasDigit = false;
  while (cursor < end) {
    const wchar_t ch = name[cursor];
    if (std::iswdigit(ch)) {
      hasDigit = true;
    } else if (ch != L'.') {
      break;
    }
    ++cursor;
  }
  return hasDigit ? name.substr(begin, cursor - begin) : std::wstring();
}

bool moduleIsX64(HMODULE module) {
  if (!module) return false;
  auto* base = reinterpret_cast<unsigned char*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE &&
         nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

CompatibilityEndpoint resolveEndpoint(HMODULE module, const wchar_t* name,
                                      const TargetEndpointProfile& profile) {
  CompatibilityEndpoint result;
  result.name = name;
  result.signature = profile.signature;
  result.signatureSize = profile.signatureSize;
  result.trampolinePolicy = profile.trampolinePolicy;
  if (!module) return result;
  const std::size_t size = profile.signatureSize;
  if (size == 0 || size > profile.signature.size()) return result;
  auto* base = reinterpret_cast<unsigned char*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return result;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return result;

  if (profile.rva + size <= nt->OptionalHeader.SizeOfImage &&
      std::memcmp(base + profile.rva, profile.signature.data(), size) == 0) {
    result.address = reinterpret_cast<std::uintptr_t>(base + profile.rva);
    result.matchMethod = CompatibilityMatchMethod::KnownRva;
    return result;
  }

  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
  unsigned char* match = nullptr;
  size_t matchCount = 0;
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
    const IMAGE_SECTION_HEADER& section = sections[index];
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
        section.VirtualAddress >= nt->OptionalHeader.SizeOfImage)
      continue;
    const size_t available = nt->OptionalHeader.SizeOfImage - section.VirtualAddress;
    const size_t sectionSize = (std::min)(static_cast<size_t>(section.Misc.VirtualSize),
                                          available);
    if (sectionSize < size) continue;
    unsigned char* begin = base + section.VirtualAddress;
    for (size_t offset = 0; offset + size <= sectionSize; ++offset) {
      if (std::memcmp(begin + offset, profile.signature.data(), size) != 0) continue;
      match = begin + offset;
      ++matchCount;
      if (matchCount > 1) {
        result.matchMethod = CompatibilityMatchMethod::AmbiguousSignature;
        return result;
      }
    }
  }
  if (matchCount == 1) {
    result.address = reinterpret_cast<std::uintptr_t>(match);
    result.matchMethod = CompatibilityMatchMethod::UniqueSignature;
  }
  return result;
}

const wchar_t* okText(bool ok) {
  return ok ? L"OK" : L"FAILED";
}

std::wstring matchText(const CompatibilityEndpoint& endpoint) {
  switch (endpoint.matchMethod) {
    case CompatibilityMatchMethod::KnownRva: return L"known RVA";
    case CompatibilityMatchMethod::UniqueSignature: return L"unique signature";
    case CompatibilityMatchMethod::AmbiguousSignature: return L"ambiguous signature";
    case CompatibilityMatchMethod::None: return L"not matched";
  }
  return L"not matched";
}

}  // namespace

std::wstring TargetCompatibilityReport::format() const {
  std::wostringstream out;
  out << L"KQPet Compatibility Report\r\n\r\n"
      << L"Extension: " << extensionVersion << L"\r\n"
      << L"Commit: " << gitCommit << L"\r\n"
      << L"Build time: " << buildTimeUtc << L"\r\n"
      << L"KQPro: " << (kqProVersion.empty() ? L"unknown" : L"V" + kqProVersion)
      << L" (" << kqProName << L") " << okText(kqProIdentityOk) << L"\r\n"
      << L"Target profile: "
      << (targetProfileId.empty() ? L"not matched" : targetProfileId) << L"\r\n"
      << L"Architecture: " << targetArchitecture << L" " << okText(processArchitectureOk)
      << L"\r\n"
      << L"Qt6Core: " << (qtCoreVersion.empty() ? L"not loaded" : qtCoreVersion)
      << L" (expected " << widenAscii(KQPET_QT_VERSION_STRING) << L") " << okText(qtCoreOk)
      << L"\r\n"
      << L"Qt6Widgets: " << (qtWidgetsVersion.empty() ? L"not loaded" : qtWidgetsVersion)
      << L" (expected " << widenAscii(KQPET_QT_VERSION_STRING) << L") " << okText(qtWidgetsOk)
      << L"\r\n"
      << L"QCefView: " << (qcefViewPresent ? L"detected" : L"not loaded") << L" "
      << okText(qcefViewPresent) << L"\r\n"
      << L"QCefView symbol: " << (qcefViewSymbolPresent ? L"resolved" : L"missing") << L" "
      << okText(qcefViewSymbolPresent) << L"\r\n";
  const CompatibilityEndpoint* endpoints[] = {&dispatch, &serviceGetter, &commandSender};
  for (const CompatibilityEndpoint* endpoint : endpoints) {
    out << endpoint->name << L": " << matchText(*endpoint) << L" "
        << okText(endpoint->matched()) << L" address=";
    if (endpoint->address)
      out << L"0x" << std::hex << endpoint->address << std::dec;
    else
      out << L"n/a";
    out << L"\r\n";
  }
  out << L"\r\nResult: " << (supported ? L"SUPPORTED" : L"UNSUPPORTED");
  return out.str();
}

bool TargetCompatibilityGuard::requiredRuntimeModulesLoaded() {
  return GetModuleHandleW(L"Qt6Core.dll") && GetModuleHandleW(L"Qt6Widgets.dll") &&
         GetModuleHandleW(L"QCefView.dll");
}

TargetCompatibilityReport TargetCompatibilityGuard::evaluate() {
  TargetCompatibilityReport report;
  report.extensionVersion = widenAscii(KQPET_VERSION_STRING);
  report.gitCommit = widenAscii(KQPET_GIT_COMMIT);
  report.buildTimeUtc = widenAscii(KQPET_BUILD_TIME_UTC);
  report.targetArchitecture = widenAscii(KQPET_TARGET_ARCH);

  const HMODULE executable = GetModuleHandleW(nullptr);
  const std::wstring executablePath = modulePath(executable);
  report.kqProName = fileName(executablePath);
  report.kqProVersion = fileVersion(executablePath);
  if (report.kqProVersion.empty()) report.kqProVersion = versionFromKqProName(report.kqProName);
  const TargetProfile* profile =
      TargetProfileRegistry::find(report.kqProName, report.kqProVersion);
  report.kqProIdentityOk = profile != nullptr;
  if (profile) report.targetProfileId = profile->id;
  report.processArchitectureOk = moduleIsX64(executable);

  const HMODULE qtCore =
      GetModuleHandleW(profile ? profile->qtCoreModule.c_str() : L"Qt6Core.dll");
  const HMODULE qtWidgets = GetModuleHandleW(
      profile ? profile->qtWidgetsModule.c_str() : L"Qt6Widgets.dll");
  report.qtCoreVersion = qtCore ? fileVersion(modulePath(qtCore)) : std::wstring();
  report.qtWidgetsVersion = qtWidgets ? fileVersion(modulePath(qtWidgets)) : std::wstring();
  const std::wstring expectedQt = profile ? profile->qtVersion : std::wstring();
  report.qtCoreOk = report.qtCoreVersion == expectedQt;
  report.qtWidgetsOk = report.qtWidgetsVersion == expectedQt;

  const HMODULE qcefView = GetModuleHandleW(
      profile ? profile->qcefViewModule.c_str() : L"QCefView.dll");
  report.qcefViewPresent = qcefView != nullptr && !modulePath(qcefView).empty();
  if (qcefView) {
    report.qcefViewExecuteJavascript = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(qcefView, profile ? profile->qcefExecuteJavascriptSymbol.c_str()
                                        : ""));
  }
  report.qcefViewSymbolPresent = report.qcefViewExecuteJavascript != 0;

  if (profile) {
    report.dispatch = resolveEndpoint(executable, L"Dispatch", profile->dispatch);
    report.serviceGetter =
        resolveEndpoint(executable, L"ServiceGetter", profile->serviceGetter);
    report.commandSender =
        resolveEndpoint(executable, L"CommandSender", profile->commandSender);
  }

  report.supported = report.processArchitectureOk && report.kqProIdentityOk &&
                     report.qtCoreOk && report.qtWidgetsOk && report.qcefViewPresent &&
                     report.qcefViewSymbolPresent && report.dispatch.matched() &&
                     report.serviceGetter.matched() && report.commandSender.matched();
  return report;
}

void TargetCompatibilityGuard::setLastReport(TargetCompatibilityReport report) {
  gLastReport = std::move(report);
}

const TargetCompatibilityReport& TargetCompatibilityGuard::lastReport() {
  return gLastReport;
}
