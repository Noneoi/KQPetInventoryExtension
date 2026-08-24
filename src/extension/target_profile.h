#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class TrampolinePolicy {
  Unsupported,
  ExactRelocationFreePrologue,
};

struct TargetEndpointProfile {
  std::wstring name;
  std::uintptr_t rva = 0;
  std::array<unsigned char, 32> signature{};
  std::size_t signatureSize = 0;
  TrampolinePolicy trampolinePolicy = TrampolinePolicy::Unsupported;
};

struct TargetProfile {
  std::wstring id;
  std::wstring version;
  std::vector<std::wstring> executableNames;
  std::wstring qtVersion;
  std::wstring qtCoreModule;
  std::wstring qtWidgetsModule;
  std::wstring qcefViewModule;
  std::string qcefExecuteJavascriptSymbol;
  TargetEndpointProfile dispatch;
  TargetEndpointProfile serviceGetter;
  TargetEndpointProfile commandSender;
};
