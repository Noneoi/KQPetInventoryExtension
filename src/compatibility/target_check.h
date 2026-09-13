#pragma once

#include "pe_view.h"

namespace kqpet::compatibility {

struct ModuleCheck {
  std::wstring name;
  std::wstring path;
  std::wstring version;
  std::uintptr_t base = 0;
  std::uint32_t imageSize = 0;
  std::uint32_t timestamp = 0;
  bool valid = false;
  std::string error;
  std::uint32_t headerSize = 0;
};

struct TargetReport {
  const Profile* profile = nullptr;
  std::wstring executablePath;
  std::string executableSha256;
  std::vector<ModuleCheck> modules;
  EndpointMatch dispatch;
  EndpointMatch serviceGetter;
  EndpointMatch commandSender;
  std::uint32_t qcefExportRva = 0;
  std::uintptr_t executableBase = 0;
  std::uintptr_t qcefExecuteJavascript = 0;
  bool supported = false;
  std::string error;
};

// Historical names/version resources never select callable addresses.
TargetReport identifyTargetImage(const PeView& executable);
TargetReport identifyTargetFile(const std::wstring& executablePath);
TargetReport checkTargetFile(const std::wstring& executablePath, const std::wstring& extensionPath = {});
// For the launcher: verify the actual loaded paths against a successful disk
// report. Remote addresses are observations only and never callable here.
TargetReport checkProcessModulePaths(std::uint32_t processId, const TargetReport& diskReport);
TargetReport checkCurrentProcess();
bool requiredRuntimeModulesLoaded();
std::wstring currentExecutablePath();
std::string reportJson(const TargetReport& report);
std::string profilesJson();
std::string utf8(const std::wstring& value);

}  // namespace kqpet::compatibility
