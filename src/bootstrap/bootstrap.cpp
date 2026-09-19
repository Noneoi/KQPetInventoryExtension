#include "bootstrap.h"
#include "loader/client_target.h"

namespace kqpet::bootstrap {
namespace fs = std::filesystem;
std::wstring quoteArgument(const std::wstring& argument) {
  std::wstring result = L"\""; std::size_t slashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
    else { result.append(slashes, L'\\'); result += c; }
    slashes = 0;
  }
  result.append(slashes * 2, L'\\'); return result + L'"';
}
bool createProcess(const LaunchRequest& request, release::Failure* error) {
  std::wstring command = quoteArgument(request.executable.wstring());
  for (const auto& argument : request.arguments) command += L" " + quoteArgument(argument);
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(request.executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
      nullptr, request.workingDirectory.c_str(), &startup, &process)) {
    if (error) *error = {release::Code::LaunchFailed, "launch", "CreateProcess failed before a child was started", GetLastError()};
    return false;
  }
  // A successfully created version loader can already have launched the
  // original. Its eventual exit code must never trigger a second launch.
  CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
Result launch(const fs::path& root, const std::vector<std::wstring>& arguments,
    ProcessLauncher launcher, release::RunningProbe probe, const release::ValidationOverrides* testsOnly) {
  Result result; fs::path client;
  if (!release::safeDirectory(root, &client, &result.error)) return result;
  if (!launcher) launcher = createProcess;
  result.selection = release::resolveRelease(client, probe, {}, testsOnly);
  if (result.selection.ok && result.selection.release.valid) {
    LaunchRequest version;
    version.kind = LaunchKind::VersionLoader;
    version.executable = result.selection.release.directory / L"KQPetLauncher.exe";
    version.workingDirectory = client;
    version.arguments = {L"--client-root", client.wstring(), L"--"};
    version.arguments.insert(version.arguments.end(), arguments.begin(), arguments.end());
    if (launcher(version, &result.error)) {
      result.started = result.extensionLoaderStarted = true; return result;
    }
  } else result.error = result.selection.error;
  const auto original = kqpet::launcher::findClientExecutable(client);
  fs::path safeOriginal;
  if (original.empty() || !release::safeChild(client, original.filename(), false, &safeOriginal, &result.error)) return result;
  LaunchRequest fallback{LaunchKind::Original, safeOriginal, client, arguments};
  ++result.originalLaunchAttempts;
  result.started = launcher(fallback, &result.error);
  return result;
}
}
