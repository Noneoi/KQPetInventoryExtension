#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace kqpet::release {
inline constexpr unsigned kBootstrapProtocol = 1;
inline constexpr unsigned kStartupProtocol = 1;
enum class Code { Ok, InvalidPath, Missing, Io, InvalidJson, InvalidSchema, InvalidIdentity,
  UnsupportedProtocol, HashMismatch, SizeMismatch, InvalidPe, ReportMismatch, LockBusy,
  Running, RunningUnknown, InjectedFault, NoUsableRelease, LaunchFailed };
struct Failure { Code code = Code::Ok; std::string stage; std::string message; DWORD systemError = 0; };
const char* codeName(Code code);
std::string failureJson(const Failure& failure);
std::string pathUtf8(const std::filesystem::path& path);
bool safeReleaseId(const std::string& id);
bool sha256Text(const std::string& value);
// Reject every reparse component and non-canonical spelling. The final path
// must already exist; validation never follows user-controlled junctions.
bool safeDirectory(const std::filesystem::path& path, std::filesystem::path* normalized, Failure* error);
bool safeChild(const std::filesystem::path& root, const std::filesystem::path& relative,
               bool directory, std::filesystem::path* resolved, Failure* error);

struct BuildIdentity {
  std::string releaseId, version, architecture, qt, sourceSha256, profileSha256;
  std::string toolchain, configuration, minHookCommit;
  unsigned bootstrapProtocol = 0, startupProtocol = 0;
  std::string canonicalJson;
  bool operator==(const BuildIdentity& other) const { return canonicalJson == other.canonicalJson; }
};
bool parseBuildIdentity(const std::string& json, BuildIdentity* identity, Failure* error);
bool readBuildIdentity(const std::filesystem::path& pe, BuildIdentity* identity, Failure* error);
bool readBootstrapProtocol(const std::filesystem::path& pe, unsigned* protocol, Failure* error);
struct Artifact { std::string path; std::uint64_t size = 0; std::string sha256; };
struct Manifest { std::string releaseId; BuildIdentity identity; Artifact loader, extension, tests; };
bool parseManifest(const std::string& json, Manifest* manifest, Failure* error);
bool verifyTestReport(const std::string& json, const Manifest& manifest, Failure* error);
std::string sha256Bytes(const void* bytes, std::size_t size);

// A test harness may inject identity reading without loading an executable.
// Production bootstrap and CLI do not expose or accept this override.
struct ValidationOverrides {
  std::function<bool(const std::filesystem::path&, BuildIdentity*, Failure*)> identityReader;
  bool allowNonPeTestFiles = false;
};
struct FilePins;
std::shared_ptr<FilePins> pinDirectory(const std::filesystem::path& path, Failure* error);
struct Validation {
  bool valid = false;
  Manifest manifest;
  std::filesystem::path directory;
  std::string manifestSha256;
  Failure error;
  // Deny write/delete until selection has been handed to CreateProcess.
  std::shared_ptr<FilePins> pins;
};
Validation validateRelease(const std::filesystem::path& runtimeRoot, const std::string& releaseId,
    const std::string& expectedManifestSha256 = {}, const ValidationOverrides* testsOnly = nullptr);
std::string validationJson(const Validation& result);
// Bounded UTF-8 file helper for active/pending records; no implicit defaults.
bool readRecord(const std::filesystem::path& path, std::string* bytes, Failure* error, std::uint64_t maximumBytes = 65536);
}
