#include "bootstrap/bootstrap.h"
#include "bootstrap/strict_json.h"
#include <shellapi.h>
#include <winioctl.h>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace fs = std::filesystem;
using namespace kqpet::release;
namespace {
bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message); return condition;
}
struct Temporary {
  fs::path base, path;
  Temporary() {
    wchar_t root[32768]{}, unique[32768]{};
    if (!GetTempPathW(32768, root) || !GetTempFileNameW(root, L"kqr", 0, unique)) return;
    base = fs::path(root).lexically_normal(); path = fs::path(unique).lexically_normal();
    if (base.filename().empty()) base = base.parent_path();
    DeleteFileW(path.c_str()); std::error_code error; fs::create_directory(path, error);
    if (error) path.clear();
  }
  ~Temporary() {
    // Only this freshly created absolute child is recursively removed.
    if (path.empty() || !path.is_absolute() || path.parent_path() != base || path.filename().wstring().rfind(L"kqr", 0) != 0) return;
    std::error_code error; fs::remove_all(path, error);
  }
};
bool write(const fs::path& file, const std::string& text) {
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size())); output.close(); return !output.fail();
}
std::string hash(const std::string& text) { return sha256Bytes(text.data(), text.size()); }
std::string identity(const std::string& id) {
  return "{\"schema\":1,\"releaseId\":" + json::quote(id) +
      ",\"version\":\"2.0-test\",\"architecture\":\"x64\",\"qt\":\"6.6.3\",\"sourceSha256\":\"" + std::string(64, 'A') +
      "\",\"profileSha256\":\"" + std::string(64, 'B') + "\",\"toolchain\":\"isolated-test\",\"configuration\":\"Release\","
      "\"startupProtocol\":1,\"bootstrapProtocol\":1,\"minHookCommit\":\"c3fcafdc10146beb5919319d0683e44e3c30d537\"}";
}
std::string artifact(const char* name, const std::string& bytes, bool path = true) {
  return "{" + std::string(path ? "\"path\":" + json::quote(name) + "," : "") +
      "\"size\":" + std::to_string(bytes.size()) + ",\"sha256\":" + json::quote(hash(bytes)) + "}";
}
struct Fixture {
  fs::path client, runtime;
  std::map<std::string, BuildIdentity> identities;
  std::map<std::string, std::string> hashes;
  ValidationOverrides overrides;
  explicit Fixture(fs::path root) : client(std::move(root)), runtime(client / L"KQPetRuntime") {
    fs::create_directories(runtime / L"releases");
    write(client / L"KQProV1.1.4.exe", "isolated fake original; never executed");
    overrides.allowNonPeTestFiles = true;
    overrides.identityReader = [this](const fs::path& file, BuildIdentity* result, Failure*) {
      const auto found = identities.find(file.parent_path().filename().string());
      if (found == identities.end()) return false; *result = found->second; return true;
    };
  }
  bool release(const std::string& id, bool reportPass = true) {
    const auto directory = runtime / L"releases" / id; fs::create_directory(directory);
    Failure error; BuildIdentity build;
    if (!parseBuildIdentity(identity(id), &build, &error)) return false;
    identities[id] = build;
    const std::string loader = "test loader " + id, extension = "test extension " + id;
    const std::string pair = "{\"loader\":" + artifact("", loader, false) + ",\"extension\":" + artifact("", extension, false) + "}";
    const std::string report = "{\"schema\":1,\"releaseId\":" + json::quote(id) + ",\"sourceSha256\":\"" + std::string(64, 'A') +
        "\",\"profileSha256\":\"" + std::string(64, 'B') + "\",\"artifacts\":" + pair +
        ",\"suite\":{\"total\":1,\"passed\":" + (reportPass ? "1" : "0") + ",\"failed\":" + (reportPass ? "0" : "1") + "},\"evidence\":[\"isolated fixture\"]}";
    const std::string manifest = "{\"schema\":1,\"releaseId\":" + json::quote(id) + ",\"identity\":" + identity(id) +
        ",\"artifacts\":{\"loader\":" + artifact("KQPetLauncher.exe", loader) + ",\"extension\":" + artifact("KQPetInventory.dll", extension) +
        "},\"tests\":" + artifact("test-report.json", report) + "}";
    hashes[id] = hash(manifest);
    return write(directory / L"KQPetLauncher.exe", loader) && write(directory / L"KQPetInventory.dll", extension) &&
        write(directory / L"test-report.json", report) && write(directory / L"manifest.json", manifest);
  }
  void active(const std::string& id) { write(runtime / L"active.json", activeJson({id, {}, hashes[id]})); }
  void pending(const std::string& id) { write(runtime / L"pending.json", pendingJson({id, hashes[id]})); }
  Selection resolve(ProcessState state = ProcessState::Stopped) {
    return resolveRelease(client, [state](const fs::path&) { return state; }, {}, &overrides);
  }
  Selection activate(const std::string& id, FaultHook faults = {}, ProcessState state = ProcessState::Stopped) {
    return activatePending(client, id, hashes[id], [state](const fs::path&) { return state; }, faults, &overrides);
  }
};
bool junction(const fs::path& link, const fs::path& target) {
  fs::create_directory(link);
  const std::wstring substitute = L"\\??\\" + target.wstring(), display = target.wstring();
  const WORD substituteBytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  const WORD displayBytes = static_cast<WORD>(display.size() * sizeof(wchar_t));
  const DWORD payload = 8 + substituteBytes + sizeof(wchar_t) + displayBytes + sizeof(wchar_t);
  std::vector<unsigned char> bytes(8 + payload, 0);
  auto put = [&](std::size_t offset, const auto& value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT; const WORD length = static_cast<WORD>(payload), zero = 0;
  put(0, tag); put(4, length); put(8, zero); put(10, substituteBytes);
  const WORD printOffset = substituteBytes + sizeof(wchar_t); put(12, printOffset); put(14, displayBytes);
  std::memcpy(bytes.data() + 16, substitute.data(), substituteBytes);
  std::memcpy(bytes.data() + 16 + printOffset, display.data(), displayBytes);
  HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  DWORD returned = 0;
  const bool result = directory != INVALID_HANDLE_VALUE && DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr);
  if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
  return result;
}
}

int wmain(int argc, wchar_t** argv) {
  Temporary temporary;
  bool ok = require(!temporary.path.empty(), "isolated temporary directory unavailable");
  if (!ok) return 1;
  for (const auto& id : {"..", "../escape", "a\\b", "C:escape", "trailing.", "CON", "con.txt", "LPT1.data", "a..b", "_hidden", "with space"})
    ok &= require(!safeReleaseId(id), "unsafe release ID accepted");
  ok &= require(safeReleaseId("2.0.0-Ab09_20260910"), "safe release ID rejected");
  Failure error; ActiveRecord active;
  const auto validActive = activeJson({"r1", {}, std::string(64, 'A')});
  ok &= require(parseActive(validActive, &active, &error), "valid active pointer rejected");
  for (const auto& text : {
      std::string("{\"schema\":1,\"schema\":1}"), std::string("{\"schema\":1.0}"), std::string("{\"schema\":-1}"),
      std::string("{\"schema\":18446744073709551616}"), validActive + " trailing", std::string("{\"schema\":1,\"activeReleaseId\":\"..\",\"previousReleaseId\":\"\",\"manifestSha256\":\"x\"}")})
    ok &= require(!parseActive(text, &active, &error), "malformed active JSON accepted");
  BuildIdentity parsed;
  ok &= require(!parseBuildIdentity(identity("r1").insert(1, "\"unknown\":0,"), &parsed, &error), "unknown build identity field accepted");
  {
    Fixture fixture(temporary.path / L"validation");
    ok &= require(fixture.release("old") && fixture.release("new") && fixture.release("failed-tests", false), "release fixture write failed");
    auto valid = validateRelease(fixture.runtime, "old", fixture.hashes["old"], &fixture.overrides);
    ok &= require(valid.valid, "matching artifacts, report and identities rejected");
    const auto loaderPath = fixture.runtime / L"releases" / L"old" / L"KQPetLauncher.exe";
    HANDLE writer = CreateFileW(loaderPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    ok &= require(writer == INVALID_HANDLE_VALUE, "validated artifact was writable while launch pins remained");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    valid = {};
    ok &= require(!validateRelease(fixture.runtime, "old", std::string(64, '0'), &fixture.overrides).valid, "wrong committed manifest hash accepted");
    ok &= require(!validateRelease(fixture.runtime, "failed-tests", {}, &fixture.overrides).valid, "failed tests accepted despite report hash matching");
    auto mixed = fixture.overrides;
    mixed.identityReader = [&](const fs::path& path, BuildIdentity* value, Failure*) { *value = fixture.identities[path.filename() == L"KQPetInventory.dll" ? "new" : "old"]; return true; };
    ok &= require(!validateRelease(fixture.runtime, "old", {}, &mixed).valid, "mixed PE resource identities accepted");
    ok &= require(write(loaderPath, "tampered"), "artifact pin was retained after validation release");
    ok &= require(!validateRelease(fixture.runtime, "old", {}, &fixture.overrides).valid, "tampered artifact accepted");
    const auto link = fixture.runtime / L"releases" / L"junction";
    const bool linked = junction(link, fixture.runtime / L"releases" / L"new");
    ok &= require(linked, "junction fault fixture could not be created");
    if (linked) ok &= require(!validateRelease(fixture.runtime, "junction", {}, &fixture.overrides).valid, "reparse release directory accepted");
    RemoveDirectoryW(link.c_str());
  }
  {
    Fixture fixture(temporary.path / L"normal"); fixture.release("old"); fixture.release("new"); fixture.active("old"); fixture.pending("new");
    auto selected = fixture.resolve();
    ok &= require(selected.ok && selected.release.manifest.releaseId == "old" && selected.pending && !selected.committed, "resolve activated a complete pending release");
    selected = {};
    auto firstLock = DeploymentLock::acquire(fixture.runtime, &error);
    ok &= require(firstLock && !DeploymentLock::acquire(fixture.runtime, &error), "second deploy lock entered the critical section");
    firstLock.reset();
    ok &= require(!fixture.activate("new", {}, ProcessState::Running).ok && !fixture.activate("new", {}, ProcessState::Unknown).ok,
                  "running or unknown original process authorized activation");
    auto activated = fixture.activate("new");
    ok &= require(activated.ok && activated.committed && !activated.pending && activated.release.manifest.releaseId == "new", "explicit valid pending activation failed");
    std::string previous;
    ok &= require(readRecord(fixture.runtime / L"active.previous.json", &previous, &error) && parseActive(previous, &active, &error) && active.activeReleaseId == "old",
                  "active commit lacked an independently saved previous pointer");
    activated = {};
    write(fixture.runtime / L"active.json", "corrupted"); fixture.pending("new");
    auto recovered = fixture.resolve();
    ok &= require(recovered.ok && recovered.recovered && recovered.release.manifest.releaseId == "old" && recovered.pending,
                  "corrupt active did not recover verified previous independently of pending");
    recovered = {};
    write(fixture.runtime / L"active.json", "corrupted"); write(fixture.runtime / L"active.previous.json", "corrupted");
    ok &= require(!fixture.resolve().ok, "two damaged pointers authorized a pending release");
  }
  const FaultPoint cuts[] = {FaultPoint::BeforePreviousWrite, FaultPoint::AfterPreviousFlush, FaultPoint::AfterPreviousReplace,
      FaultPoint::BeforeActiveWrite, FaultPoint::AfterActiveFlush, FaultPoint::AfterActiveReplace,
      FaultPoint::BeforePendingDelete, FaultPoint::AfterPendingDelete};
  for (unsigned index = 0; index < std::size(cuts); ++index) {
    Fixture fixture(temporary.path / (L"cut-" + std::to_wstring(index)));
    fixture.release("old"); fixture.release("new"); fixture.active("old"); fixture.pending("new");
    const auto interrupted = fixture.activate("new", [point = cuts[index]](FaultPoint here) { return here == point; });
    ok &= require(!interrupted.ok && interrupted.error.code == Code::InjectedFault, "fault point was not reached");
    const auto next = fixture.resolve();
    const bool afterCommit = index >= 5;
    ok &= require(next.ok && next.release.manifest.releaseId == (afterCommit ? "new" : "old"), "interruption recovered a release that was never committed");
  }
  {
    Fixture fixture(temporary.path / L"probe-race"); fixture.release("old"); fixture.release("new"); fixture.active("old"); fixture.pending("new");
    ProcessState state = ProcessState::Stopped;
    const auto interrupted = activatePending(fixture.client, "new", fixture.hashes["new"],
        [&](const fs::path&) { return state; }, [&](FaultPoint point) { if (point == FaultPoint::BeforeActiveWrite) state = ProcessState::Running; return false; }, &fixture.overrides);
    ok &= require(!interrupted.committed && interrupted.error.code == Code::Running && fixture.resolve().release.manifest.releaseId == "old",
                  "late original start was not rechecked before pointer commit");
    const auto wrongIntent = activatePending(fixture.client, "old", fixture.hashes["old"], [](const fs::path&) { return ProcessState::Stopped; }, {}, &fixture.overrides);
    ok &= require(!wrongIntent.committed && wrongIntent.error.code == Code::InvalidIdentity, "different deployment's pending was committed");
  }
  {
    Fixture fixture(temporary.path / L"first"); fixture.release("new"); fixture.pending("new");
    ok &= require(!fixture.resolve().ok, "first install pending was loaded without explicit commit");
    const auto first = fixture.activate("new"); ok &= require(first.ok && first.committed, "first explicit activation failed");
  }
  {
    Fixture fixture(temporary.path / L"bootstrap"); fixture.release("old"); fixture.active("old");
    for (int mode = 0; mode < 3; ++mode) {
      int versionCalls = 0, originalCalls = 0;
      const auto result = kqpet::bootstrap::launch(fixture.client, {L"literal \" argument", L"--client-root", L"untrusted-game-argument"},
          [&](const kqpet::bootstrap::LaunchRequest& request, Failure*) {
        if (request.kind == kqpet::bootstrap::LaunchKind::VersionLoader) {
          ++versionCalls;
          ok &= require(request.workingDirectory == fixture.client && request.arguments.size() == 6 && request.arguments[0] == L"--client-root" &&
              request.arguments[1] == fixture.client.wstring() && request.arguments[2] == L"--", "version loader did not receive an explicit isolated client root");
          return mode == 0; // Child may fail later; that must never cause fallback.
        }
        ++originalCalls; return mode == 1;
      }, [](const fs::path&) { return ProcessState::Stopped; }, &fixture.overrides);
      ok &= require(versionCalls == 1 && originalCalls == (mode == 0 ? 0 : 1) && result.originalLaunchAttempts <= 1 && result.started == (mode != 2),
                    "bootstrap restarted the original after a child existed or attempted fallback more than once");
    }
  }
  const std::vector<std::wstring> arguments{L"", L"with space", L"quote\"slash\\", L"trailing\\", L"中文"};
  std::wstring command = L"test.exe";
  for (const auto& value : arguments) command += L" " + kqpet::bootstrap::quoteArgument(value);
  int parsedCount = 0; LPWSTR* parsedArguments = CommandLineToArgvW(command.c_str(), &parsedCount);
  ok &= require(parsedArguments && parsedCount == static_cast<int>(arguments.size()) + 1, "quoted command line did not round trip");
  if (parsedArguments) {
    for (int index = 1; index < parsedCount && index <= static_cast<int>(arguments.size()); ++index)
      ok &= require(parsedArguments[index] == arguments[index - 1], "argument quoting altered a literal");
    LocalFree(parsedArguments);
  }
  if (argc >= 3) {
    BuildIdentity loader, extension;
    ok &= require(readBuildIdentity(argv[1], &loader, &error) && readBuildIdentity(argv[2], &extension, &error) && loader == extension,
                  "actual resource-only loader/DLL identity validation failed");
    unsigned protocol = 0;
    ok &= require(!readBootstrapProtocol(argv[1], &protocol, &error), "version loader was mistaken for a stable bootstrap");
  }
  if (!ok) return 1;
  std::puts("PASS: isolated release manifests, identity/report binding, activation cuts, recovery, pins and one-shot bootstrap fallback");
  return 0;
}
