#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kqpet::compatibility {

enum class TrampolinePolicy { Unsupported, ExactRelocationFreePrologue };
enum class EndpointRole { Dispatch, ServiceGetter, CommandSender };
enum class MatchPolicy { KnownRva, UniqueAtKnownRva, UniqueSignature };

struct EndpointProfile {
  std::wstring name;
  std::uintptr_t rva = 0;
  std::array<unsigned char, 32> signature{};
  std::size_t signatureSize = 0;
  TrampolinePolicy trampolinePolicy = TrampolinePolicy::Unsupported;
  EndpointRole role = EndpointRole::ServiceGetter;
  MatchPolicy matchPolicy = MatchPolicy::KnownRva;
  // Empty mask means exact bytes. Only reviewed relocation operands may vary.
  std::array<unsigned char, 32> signatureMask{};
  bool masked = false;
};

struct Profile {
  std::wstring id;
  std::wstring version;
  std::vector<std::wstring> executableNames;
  std::wstring qtVersion;
  std::wstring qtCoreModule;
  std::wstring qtWidgetsModule;
  std::wstring qcefViewModule;
  std::string qcefExecuteJavascriptSymbol;
  EndpointProfile dispatch;
  EndpointProfile serviceGetter;
  EndpointProfile commandSender;
  std::vector<std::wstring> qtModules;
  std::string executableSha256;
  std::string evidenceReference;
};

// Generated at build time from profiles/targets.json. No address configuration
// is loaded from user-writable runtime files.
const std::vector<Profile>& registeredProfiles();
const char* profileCatalogDigest();
// Historical evidence lookup for diagnostics/tests only. Production matching
// uses identifyTargetImage/File and never authorizes or rejects by this name.
const Profile* findProfile(const std::wstring& executableName,
                          const std::wstring& version = {});

}  // namespace kqpet::compatibility
