#pragma once

#include <filesystem>
#include <string>
#include <windows.h>

namespace kqpet::launcher {
struct DataRootSelection {
  std::filesystem::path dataRoot;
  std::filesystem::path pendingRoot;
  std::filesystem::path configPath;
  bool configured = false;
  std::wstring warning;
};

// Configuration describes a stable client data directory, never a release
// directory. A pending migration keeps using dataRoot until it has committed.
DataRootSelection resolveDataRoot(const std::filesystem::path& clientRoot,
                                  const std::wstring& inheritedRoot = {});
std::wstring inheritedDataRoot();
bool applyDataRootEnvironment(const DataRootSelection& selection,
                             const std::filesystem::path& clientRoot,
                             std::wstring* error = nullptr);
// Called before starting the host. Migration failures clear the one-shot
// request, retain the old root and record a message for the Settings page.
bool prepareDataRootMigration(HMODULE module, const std::filesystem::path& clientRoot,
                             DataRootSelection* selection, bool hostStopped,
                             std::wstring* notice = nullptr);
}
