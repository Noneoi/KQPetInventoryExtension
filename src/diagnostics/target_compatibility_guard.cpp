#include "target_compatibility_guard.h"

#include "compatibility/target_check.h"
#include "version.h"

#include <filesystem>
#include <sstream>
#include <utility>

namespace {
TargetCompatibilityReport gLastReport;
std::wstring widenAscii(const std::string& value) { return std::wstring(value.begin(), value.end()); }
CompatibilityEndpoint adapt(const TargetEndpointProfile& profile,
    const kqpet::compatibility::EndpointMatch& match, std::uintptr_t imageBase) {
  CompatibilityEndpoint endpoint;
  endpoint.name = profile.name;
  endpoint.matchMethod = match.method;
  endpoint.signature = profile.signature;
  endpoint.signatureSize = profile.signatureSize;
  endpoint.trampolinePolicy = profile.trampolinePolicy;
  if (match.matched() && imageBase) endpoint.address = imageBase + match.rva;
  return endpoint;
}
const wchar_t* status(bool ok) { return ok ? L"OK" : L"FAILED"; }
const wchar_t* methodName(CompatibilityMatchMethod method) {
  switch (method) {
    case CompatibilityMatchMethod::KnownRva: return L"known RVA";
    case CompatibilityMatchMethod::UniqueSignature: return L"unique executable signature";
    case CompatibilityMatchMethod::AmbiguousSignature: return L"ambiguous signature";
    case CompatibilityMatchMethod::Conflict: return L"reviewed RVA conflict";
    default: return L"not matched";
  }
}
}

CompatibilityEndpoint TargetCompatibilityGuard::resolveEndpoint(const void* image,
    std::size_t mappedSize, const TargetEndpointProfile& profile) {
  const kqpet::compatibility::MappedPeView view(image, mappedSize);
  return adapt(profile, view.resolve(profile), reinterpret_cast<std::uintptr_t>(image));
}

TargetCompatibilityReport TargetCompatibilityGuard::evaluate() {
  using namespace kqpet::compatibility;
  const TargetReport core = checkCurrentProcess();
  TargetCompatibilityReport report;
  report.extensionVersion = widenAscii(KQPET_VERSION_STRING);
  report.gitCommit = widenAscii(KQPET_GIT_COMMIT);
  report.buildTimeUtc = widenAscii(KQPET_BUILD_TIME_UTC);
  report.targetArchitecture = widenAscii(KQPET_TARGET_ARCH);
  report.kqProName = std::filesystem::path(currentExecutablePath()).filename().wstring();
  report.profileDigest = widenAscii(profileCatalogDigest());
  report.failureReason = widenAscii(core.error);
  report.kqProIdentityOk = core.profile != nullptr && core.dispatch.matched() &&
      core.serviceGetter.matched() && core.commandSender.matched();
  if (core.profile) {
    report.kqProVersion = core.modules.empty() ? L"" : core.modules.front().version;
    report.targetProfileId = core.profile->id;
    report.dispatch = adapt(core.profile->dispatch, core.dispatch, core.supported ? core.executableBase : 0);
    report.serviceGetter = adapt(core.profile->serviceGetter, core.serviceGetter, core.supported ? core.executableBase : 0);
    report.commandSender = adapt(core.profile->commandSender, core.commandSender, core.supported ? core.executableBase : 0);
  }
  if (!core.modules.empty()) report.processArchitectureOk = core.modules.front().valid;
  for (const auto& module : core.modules) {
    if (module.name == L"Qt6Core.dll") {
      report.qtCoreVersion = module.version; report.qtCoreOk = module.valid;
    } else if (module.name == L"Qt6Widgets.dll") {
      report.qtWidgetsVersion = module.version; report.qtWidgetsOk = module.valid;
    } else if (module.name == L"QCefView.dll") report.qcefViewPresent = module.valid && module.base != 0;
  }
  report.qcefViewExecuteJavascript = core.supported ? core.qcefExecuteJavascript : 0;
  report.qcefViewSymbolPresent = report.qcefViewExecuteJavascript != 0;
  report.supported = core.supported;
  return report;
}

bool TargetCompatibilityGuard::requiredRuntimeModulesLoaded() {
  return kqpet::compatibility::requiredRuntimeModulesLoaded();
}

std::wstring TargetCompatibilityReport::format() const {
  std::wostringstream out;
  out << L"KQPet Compatibility Report\r\n\r\nExtension: " << extensionVersion
      << L"\r\nCommit: " << gitCommit << L"\r\nBuild time: " << buildTimeUtc
      << L"\r\nKQPro: " << kqProName << L" (" << kqProVersion << L") " << status(kqProIdentityOk)
      << L"\r\nTarget profile: " << targetProfileId << L"\r\nProfile digest: " << profileDigest
      << L"\r\nArchitecture: " << targetArchitecture << L" " << status(processArchitectureOk)
      << L"\r\nQt6Core: " << qtCoreVersion << L" " << status(qtCoreOk)
      << L"\r\nQt6Widgets: " << qtWidgetsVersion << L" " << status(qtWidgetsOk)
      << L"\r\nQCefView: " << status(qcefViewPresent)
      << L"\r\nQCefView symbol: " << status(qcefViewSymbolPresent) << L"\r\n";
  for (const CompatibilityEndpoint* endpoint : {&dispatch, &serviceGetter, &commandSender})
    out << endpoint->name << L": " << methodName(endpoint->matchMethod) << L" " << status(endpoint->matched())
        << L" address=0x" << std::hex << endpoint->address << std::dec << L"\r\n";
  if (!failureReason.empty()) out << L"Failure: " << failureReason << L"\r\n";
  out << L"\r\nResult: " << (supported ? L"SUPPORTED" : L"UNSUPPORTED");
  return out.str();
}

void TargetCompatibilityGuard::setLastReport(TargetCompatibilityReport report) { gLastReport = std::move(report); }
const TargetCompatibilityReport& TargetCompatibilityGuard::lastReport() { return gLastReport; }
