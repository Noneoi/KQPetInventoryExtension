#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "target_profile.h"
#include "pe_view.h"

using CompatibilityMatchMethod = kqpet::compatibility::MatchMethod;

struct CompatibilityEndpoint {
  std::wstring name;
  std::uintptr_t address = 0;
  CompatibilityMatchMethod matchMethod = CompatibilityMatchMethod::None;
  std::array<unsigned char, 32> signature{};
  std::size_t signatureSize = 0;
  TrampolinePolicy trampolinePolicy = TrampolinePolicy::Unsupported;

  bool matched() const {
    return address != 0 && (matchMethod == CompatibilityMatchMethod::KnownRva ||
                            matchMethod == CompatibilityMatchMethod::UniqueSignature);
  }
};

struct TargetCompatibilityReport {
  std::wstring extensionVersion;
  std::wstring gitCommit;
  std::wstring buildTimeUtc;
  std::wstring targetArchitecture;
  std::wstring kqProName;
  std::wstring kqProVersion;
  std::wstring targetProfileId;
  std::wstring qtCoreVersion;
  std::wstring qtWidgetsVersion;
  bool processArchitectureOk = false;
  bool kqProIdentityOk = false;
  bool qtCoreOk = false;
  bool qtWidgetsOk = false;
  bool qcefViewPresent = false;
  bool qcefViewSymbolPresent = false;
  std::uintptr_t qcefViewExecuteJavascript = 0;
  CompatibilityEndpoint dispatch;
  CompatibilityEndpoint serviceGetter;
  CompatibilityEndpoint commandSender;
  bool supported = false;
  std::wstring failureReason;
  std::wstring profileDigest;

  std::wstring format() const;
};

class TargetCompatibilityGuard final {
public:
  // Uses only Win32 and the generated build constants. This must run before
  // the extension calls into Qt or constructs any extension business object.
  static TargetCompatibilityReport evaluate();
  // Read-only resolution in a mapped PE image. Supplying its extent also
  // permits offline regression tests without loading or running a client.
  static CompatibilityEndpoint resolveEndpoint(const void* image,
                                                std::size_t mappedSize,
                                                const TargetEndpointProfile& profile);
  static bool requiredRuntimeModulesLoaded();
  static void setLastReport(TargetCompatibilityReport report);
  static const TargetCompatibilityReport& lastReport();
};
