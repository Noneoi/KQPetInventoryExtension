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
// Two spellings denote one directory only when the file system reports the same
// directory identity. Lexical normalization cannot expand an 8.3 alias or
// re-spell a mapped path, so a string comparison is not proof of identity.
// Unverifiable is an explicit error state: callers must not fall back to a
// lexical comparison after it.
enum class DirectoryIdentity { Same, Different, Unverifiable };
DirectoryIdentity compareDirectoryIdentity(const std::filesystem::path& left,
                                          const std::filesystem::path& right,
                                          DWORD* error = nullptr);
// Every component must be an existing ordinary directory. A reparse point, a
// regular file or an unreachable component is refused. This is the path-shape
// precondition of a committed migration, independent of the identity question.
bool plainDirectoryPath(const std::filesystem::path& absolute, DWORD* error = nullptr);
// Called before starting the host. Migration failures clear the one-shot
// request, retain the old root and record a message for the Settings page.
bool prepareDataRootMigration(HMODULE module, const std::filesystem::path& clientRoot,
                             DataRootSelection* selection, bool hostStopped,
                             std::wstring* notice = nullptr);
}
