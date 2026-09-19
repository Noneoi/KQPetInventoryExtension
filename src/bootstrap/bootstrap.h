#pragma once
#include "runtime/release_activation.h"
#include <vector>

namespace kqpet::bootstrap {
enum class LaunchKind { VersionLoader, Original };
struct LaunchRequest {
  LaunchKind kind = LaunchKind::Original;
  std::filesystem::path executable, workingDirectory;
  std::vector<std::wstring> arguments;
};
using ProcessLauncher = std::function<bool(const LaunchRequest&, release::Failure*)>;
struct Result {
  bool started = false;
  bool extensionLoaderStarted = false;
  unsigned originalLaunchAttempts = 0;
  release::Selection selection;
  release::Failure error;
};
std::wstring quoteArgument(const std::wstring& argument);
bool createProcess(const LaunchRequest& request, release::Failure* error);
Result launch(const std::filesystem::path& clientRoot, const std::vector<std::wstring>& gameArguments,
    ProcessLauncher launcher = {}, release::RunningProbe probe = {},
    const release::ValidationOverrides* testsOnly = nullptr);
}
