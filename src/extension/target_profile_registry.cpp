#include "target_profile_registry.h"

#include <algorithm>
#include <cwctype>

namespace {

template <std::size_t Size>
TargetEndpointProfile endpoint(const wchar_t* name, std::uintptr_t rva,
                               const std::array<unsigned char, Size>& signature,
                               TrampolinePolicy policy = TrampolinePolicy::Unsupported) {
  TargetEndpointProfile result;
  result.name = name;
  result.rva = rva;
  std::copy(signature.begin(), signature.end(), result.signature.begin());
  result.signatureSize = Size;
  result.trampolinePolicy = policy;
  return result;
}

bool equalsInsensitive(const std::wstring& left, const std::wstring& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](wchar_t a, wchar_t b) {
                      return std::towlower(a) == std::towlower(b);
                    });
}

TargetProfile kqProV113() {
  constexpr std::array<unsigned char, 17> dispatch = {
      0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41,
      0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD1};
  constexpr std::array<unsigned char, 21> commandSender = {
      0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
      0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD9};
  constexpr std::array<unsigned char, 16> serviceGetter = {
      0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
      0x05, 0x8B, 0x7F, 0x10, 0x00, 0x48, 0x85, 0xC0};
  TargetProfile profile;
  profile.id = L"kqpro-1.1.3-x64";
  profile.version = L"1.1.3";
  profile.executableNames = {L"KQProV1.1.3.exe", L"KQPro1.1.3.exe"};
  profile.qtVersion = L"6.6.3";
  profile.qtCoreModule = L"Qt6Core.dll";
  profile.qtWidgetsModule = L"Qt6Widgets.dll";
  profile.qcefViewModule = L"QCefView.dll";
  profile.qcefExecuteJavascriptSymbol =
      "?executeJavascript@QCefView@@QEAA_NAEB_JAEBVQString@@1@Z";
  profile.dispatch = endpoint(L"Dispatch", 0x115920, dispatch,
                              TrampolinePolicy::ExactRelocationFreePrologue);
  profile.serviceGetter = endpoint(L"ServiceGetter", 0x115450, serviceGetter);
  profile.commandSender = endpoint(L"CommandSender", 0x116470, commandSender);
  return profile;
}

}  // namespace

const std::vector<TargetProfile>& TargetProfileRegistry::profiles() {
  static const std::vector<TargetProfile> registered = {kqProV113()};
  return registered;
}

const TargetProfile* TargetProfileRegistry::find(
    const std::wstring& executableName, const std::wstring& version) {
  for (const TargetProfile& profile : profiles()) {
    if (profile.version != version) continue;
    for (const std::wstring& name : profile.executableNames)
      if (equalsInsensitive(name, executableName)) return &profile;
  }
  return nullptr;
}
