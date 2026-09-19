#include "release_activation.h"
#include "bootstrap/strict_json.h"
#include "loader/client_target.h"
#include <tlhelp32.h>
#include <bcrypt.h>
#include <algorithm>
#include <vector>

namespace kqpet::release {
namespace fs = std::filesystem;
namespace {
bool fail(Failure* error, Code code, const char* stage, const char* message, DWORD system = 0) {
  if (error) *error = {code, stage, message, system}; return false;
}
std::string upper(std::string text) { for (char& c : text) if (c >= 'a' && c <= 'f') c -= 'a' - 'A'; return text; }
bool present(const fs::path& path) {
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  const auto error = GetLastError(); return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}
ProcessState running(const RunningProbe& probe, const fs::path& root) {
  try { return probe ? probe(root) : originalProcessState(root); } catch (...) { return ProcessState::Unknown; }
}
bool cut(const FaultHook& fault, FaultPoint point, Failure* error) {
  bool stop = false;
  try { stop = fault && fault(point); } catch (...) { stop = true; }
  return stop && !fail(error, Code::InjectedFault, "activation", "injected interruption at durable write boundary");
}
std::wstring nonce() {
  unsigned char random[12]{};
  if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
  static constexpr wchar_t hex[] = L"0123456789abcdef"; std::wstring value;
  for (auto byte : random) { value += hex[byte >> 4]; value += hex[byte & 15]; }
  return value;
}
// Temp files are siblings of the destination. Fault injection deliberately
// leaves a flushed temp behind, as a crashed writer would; resolve ignores it.
bool atomicRecord(const fs::path& root, const wchar_t* name, const std::string& bytes,
    const FaultHook& faults, FaultPoint afterFlush, FaultPoint afterReplace, bool* replaced, Failure* error) {
  *replaced = false;
  if (bytes.empty() || bytes.size() > 65536) return fail(error, Code::InvalidSchema, "write", "record size rejected");
  auto pins = pinDirectory(root, error); if (!pins) return false;
  const auto target = root / name;
  const DWORD attributes = GetFileAttributesW(target.c_str());
  const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
  if ((exists && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) ||
      (!exists && GetLastError() != ERROR_FILE_NOT_FOUND)) return fail(error, Code::InvalidPath, "write", "destination is not an ordinary pointer file");
  const auto random = nonce(); if (random.empty()) return fail(error, Code::Io, "write", "temporary name generation failed");
  const auto temporary = root / (std::wstring(name) + L".tmp-" + random);
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return fail(error, Code::Io, "write", "temporary record could not be created", GetLastError());
  DWORD written = 0;
  const bool flushed = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
  const DWORD writeError = GetLastError(); CloseHandle(file);
  if (!flushed) return fail(error, Code::Io, "write", "record write or flush failed", writeError);
  if (cut(faults, afterFlush, error)) return false;
  const BOOL committed = exists ? ReplaceFileW(target.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
                               : MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
  if (!committed) return fail(error, Code::Io, "commit", "atomic pointer replacement failed", GetLastError());
  *replaced = true;
  return !cut(faults, afterReplace, error);
}
struct ActiveState { bool exists = false; bool valid = false; ActiveRecord record; Validation release; Failure error; };
ActiveState readActive(const fs::path& runtime, const wchar_t* name, const ValidationOverrides* testsOnly) {
  ActiveState result; const auto path = runtime / name; result.exists = present(path);
  if (!result.exists) return result;
  std::string bytes;
  if (!readRecord(path, &bytes, &result.error) || !parseActive(bytes, &result.record, &result.error)) return result;
  result.release = validateRelease(runtime, result.record.activeReleaseId, result.record.manifestSha256, testsOnly);
  result.valid = result.release.valid;
  if (!result.valid) result.error = result.release.error;
  return result;
}
bool roots(const fs::path& client, fs::path* normalized, fs::path* runtime, Failure* error) {
  if (!safeDirectory(client, normalized, error)) return false;
  return safeChild(*normalized, L"KQPetRuntime", true, runtime, error);
}
void select(Selection* result, const ActiveState& active, const char* source) {
  result->ok = true; result->selection = source; result->release = active.release;
}
bool stopped(Selection* result) {
  if (result->originalRunning == ProcessState::Stopped) return true;
  return fail(&result->error, result->originalRunning == ProcessState::Running ? Code::Running : Code::RunningUnknown,
      "activation", result->originalRunning == ProcessState::Running ? "original client is running; pending is staged only" : "original process state is unknown; pending is staged only");
}
bool removePending(const fs::path& runtime, const FaultHook& faults, Failure* error) {
  if (cut(faults, FaultPoint::BeforePendingDelete, error)) return false;
  if (!DeleteFileW((runtime / L"pending.json").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    return fail(error, Code::Io, "pending", "active committed but pending cleanup failed", GetLastError());
  return !cut(faults, FaultPoint::AfterPendingDelete, error);
}
}

DeploymentLock::~DeploymentLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
std::unique_ptr<DeploymentLock> DeploymentLock::acquire(const fs::path& root, Failure* error) {
  auto result = std::unique_ptr<DeploymentLock>(new DeploymentLock);
  result->directoryPins_ = pinDirectory(root, error); if (!result->directoryPins_) return {};
  const auto lockPath = root / L"deploy.lock";
  const auto attributes = GetFileAttributesW(lockPath.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
    fail(error, Code::InvalidPath, "lock", "deployment lock path rejected"); return {};
  }
  result->handle_ = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (result->handle_ == INVALID_HANDLE_VALUE) { fail(error, Code::LockBusy, "lock", "deployment lock unavailable", GetLastError()); return {}; }
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(result->handle_, &information) || information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) {
    fail(error, Code::InvalidPath, "lock", "opened deployment lock is not an ordinary file"); return {};
  }
  return result;
}
bool parseActive(const std::string& text, ActiveRecord* record, Failure* error) {
  json::Value value; std::uint64_t schema = 0;
  if (text.size() > 65536 || !json::Parser(text).parse(&value)) return fail(error, Code::InvalidJson, "active", "active JSON rejected");
  if (!value.keys({"schema", "activeReleaseId", "previousReleaseId", "manifestSha256"}) ||
      !value.at("schema").unsignedInteger(&schema) || schema != 1 || value.at("activeReleaseId").kind != json::Value::Kind::String ||
      value.at("previousReleaseId").kind != json::Value::Kind::String || value.at("manifestSha256").kind != json::Value::Kind::String ||
      !safeReleaseId(value.at("activeReleaseId").text) ||
      (!value.at("previousReleaseId").text.empty() && !safeReleaseId(value.at("previousReleaseId").text)) ||
      !sha256Text(value.at("manifestSha256").text)) return fail(error, Code::InvalidSchema, "active", "active pointer schema rejected");
  record->activeReleaseId = value.at("activeReleaseId").text; record->previousReleaseId = value.at("previousReleaseId").text;
  if (record->activeReleaseId == record->previousReleaseId) return fail(error, Code::InvalidSchema, "active", "active cannot be its own previous release");
  record->manifestSha256 = upper(value.at("manifestSha256").text); return true;
}
bool parsePending(const std::string& text, PendingRecord* record, Failure* error) {
  json::Value value; std::uint64_t schema = 0;
  if (text.size() > 65536 || !json::Parser(text).parse(&value)) return fail(error, Code::InvalidJson, "pending", "pending JSON rejected");
  if (!value.keys({"schema", "releaseId", "manifestSha256"}) || !value.at("schema").unsignedInteger(&schema) || schema != 1 ||
      value.at("releaseId").kind != json::Value::Kind::String || !safeReleaseId(value.at("releaseId").text) ||
      value.at("manifestSha256").kind != json::Value::Kind::String || !sha256Text(value.at("manifestSha256").text))
    return fail(error, Code::InvalidSchema, "pending", "pending intent schema rejected");
  record->releaseId = value.at("releaseId").text; record->manifestSha256 = upper(value.at("manifestSha256").text); return true;
}
std::string activeJson(const ActiveRecord& value) {
  return "{\"schema\":1,\"activeReleaseId\":" + json::quote(value.activeReleaseId) + ",\"previousReleaseId\":" +
      json::quote(value.previousReleaseId) + ",\"manifestSha256\":" + json::quote(value.manifestSha256) + "}";
}
std::string pendingJson(const PendingRecord& value) {
  return "{\"schema\":1,\"releaseId\":" + json::quote(value.releaseId) + ",\"manifestSha256\":" + json::quote(value.manifestSha256) + "}";
}
ProcessState originalProcessState(const fs::path& clientRoot) {
  // Match the actual directory's executable names, including renamed/new
  // clients. A versioned basename is not a process identity requirement.
  std::vector<std::wstring> candidates;
  std::error_code directoryError;
  fs::directory_iterator files(clientRoot, directoryError), end;
  for (; !directoryError && files != end; files.increment(directoryError)) {
    const auto name = files->path().filename().wstring();
    const auto extension = files->path().extension().wstring();
    if (CompareStringOrdinal(extension.c_str(), -1, L".exe", -1, TRUE) == CSTR_EQUAL &&
        CompareStringOrdinal(name.c_str(), -1, L"KQPetLauncher.exe", -1, TRUE) != CSTR_EQUAL)
      candidates.push_back(name);
  }
  if (directoryError) return ProcessState::Unknown;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return ProcessState::Unknown;
  PROCESSENTRY32W entry{sizeof(entry)}; bool uncertain = false;
  BOOL more = Process32FirstW(snapshot, &entry);
  if (!more) { CloseHandle(snapshot); return ProcessState::Unknown; }
  for (; more; more = Process32NextW(snapshot, &entry)) {
    if (std::none_of(candidates.begin(), candidates.end(), [&entry](const auto& name) {
        return CompareStringOrdinal(name.c_str(), -1, entry.szExeFile, -1, TRUE) == CSTR_EQUAL;
      })) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (!process) { uncertain = true; continue; }
    std::vector<wchar_t> name(32768); DWORD size = static_cast<DWORD>(name.size());
    const bool read = QueryFullProcessImageNameW(process, 0, name.data(), &size); CloseHandle(process);
    if (!read) { uncertain = true; continue; }
    const auto actual = fs::path(std::wstring(name.data(), size)).parent_path().lexically_normal().wstring();
    const auto expected = clientRoot.lexically_normal().wstring();
    if (CompareStringOrdinal(actual.data(), static_cast<int>(actual.size()), expected.data(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL) {
      CloseHandle(snapshot); return ProcessState::Running;
    }
  }
  const DWORD enumerationError = GetLastError(); CloseHandle(snapshot);
  if (enumerationError != ERROR_NO_MORE_FILES) uncertain = true;
  return uncertain ? ProcessState::Unknown : ProcessState::Stopped;
}
Selection resolveRelease(const fs::path& clientRoot, RunningProbe probe, FaultHook faults, const ValidationOverrides* testsOnly) {
  Selection result; fs::path client, runtime;
  if (!roots(clientRoot, &client, &runtime, &result.error)) return result;
  auto lock = DeploymentLock::acquire(runtime, &result.error); if (!lock) return result;
  result.pending = present(runtime / L"pending.json");
  result.originalRunning = running(probe, client);
  const auto active = readActive(runtime, L"active.json", testsOnly);
  if (active.valid) { select(&result, active, "active"); return result; }
  const auto previous = readActive(runtime, L"active.previous.json", testsOnly);
  if (!previous.valid) {
    result.error = active.exists ? active.error : previous.exists ? previous.error : Failure{Code::NoUsableRelease, "resolve", "no committed valid release", 0};
    return result;
  }
  select(&result, previous, "previous");
  if (!stopped(&result)) return result;
  if (cut(faults, FaultPoint::BeforeActiveWrite, &result.error)) { result.ok = false; return result; }
  // Re-probe after a fault hook or lengthy validation; Unknown never authorizes
  // a pointer write. Existing clients are never terminated by this component.
  result.originalRunning = running(probe, client);
  if (!stopped(&result)) return result;
  bool replaced = false;
  const bool saved = atomicRecord(runtime, L"active.json", activeJson(previous.record), faults,
      FaultPoint::AfterActiveFlush, FaultPoint::AfterActiveReplace, &replaced, &result.error);
  result.recovered = replaced;
  if (!saved) result.ok = false;
  return result;
}
Selection activatePending(const fs::path& clientRoot, const std::string& expectedId, const std::string& expectedHash,
    RunningProbe probe, FaultHook faults, const ValidationOverrides* testsOnly) {
  Selection result; fs::path client, runtime;
  if ((!expectedId.empty() && !safeReleaseId(expectedId)) || (!expectedHash.empty() && !sha256Text(expectedHash))) {
    fail(&result.error, Code::InvalidPath, "activate", "expected pending identity rejected"); return result;
  }
  if (!roots(clientRoot, &client, &runtime, &result.error)) return result;
  auto lock = DeploymentLock::acquire(runtime, &result.error); if (!lock) return result;
  result.pending = present(runtime / L"pending.json");
  auto active = readActive(runtime, L"active.json", testsOnly);
  auto previous = readActive(runtime, L"active.previous.json", testsOnly);
  if (active.valid) select(&result, active, "active");
  else if (previous.valid) select(&result, previous, "previous");
  result.ok = false;
  result.originalRunning = running(probe, client);
  if (!stopped(&result)) return result;
  PendingRecord pending; std::string text;
  if (!readRecord(runtime / L"pending.json", &text, &result.error) || !parsePending(text, &pending, &result.error)) return result;
  if ((!expectedId.empty() && pending.releaseId != expectedId) || (!expectedHash.empty() && pending.manifestSha256 != upper(expectedHash))) {
    fail(&result.error, Code::InvalidIdentity, "pending", "pending changed after staging; expected release was not activated"); return result;
  }
  if ((active.exists || previous.exists) && !active.valid && !previous.valid) {
    fail(&result.error, Code::NoUsableRelease, "activate", "damaged activation records require recovery before updating"); return result;
  }
  if (active.record.activeReleaseId == pending.releaseId && active.record.manifestSha256 != pending.manifestSha256) {
    fail(&result.error, Code::HashMismatch, "activate", "immutable release ID has a different manifest digest"); return result;
  }
  auto target = validateRelease(runtime, pending.releaseId, pending.manifestSha256, testsOnly);
  if (!target.valid) { result.error = target.error; return result; }
  if (active.valid && active.record.activeReleaseId == pending.releaseId) {
    result.ok = removePending(runtime, faults, &result.error); result.pending = present(runtime / L"pending.json"); return result;
  }
  const ActiveState* old = active.valid ? &active : previous.valid ? &previous : nullptr;
  if (old) {
    if (cut(faults, FaultPoint::BeforePreviousWrite, &result.error)) return result;
    bool saved = false;
    if (!atomicRecord(runtime, L"active.previous.json", activeJson(old->record), faults,
        FaultPoint::AfterPreviousFlush, FaultPoint::AfterPreviousReplace, &saved, &result.error)) return result;
  }
  if (cut(faults, FaultPoint::BeforeActiveWrite, &result.error)) return result;
  result.originalRunning = running(probe, client);
  if (!stopped(&result)) return result;
  const ActiveRecord commit{pending.releaseId, old ? old->record.activeReleaseId : std::string{}, pending.manifestSha256};
  bool replaced = false;
  const bool saved = atomicRecord(runtime, L"active.json", activeJson(commit), faults,
      FaultPoint::AfterActiveFlush, FaultPoint::AfterActiveReplace, &replaced, &result.error);
  if (replaced) { result.committed = true; result.selection = "active"; result.release = target; }
  if (!saved) return result;
  result.ok = removePending(runtime, faults, &result.error);
  result.pending = present(runtime / L"pending.json");
  return result;
}
std::string selectionJson(const Selection& result) {
  const char* process = result.originalRunning == ProcessState::Stopped ? "stopped" : result.originalRunning == ProcessState::Running ? "running" : "unknown";
  return "{\"ok\":" + std::string(result.ok ? "true" : "false") + ",\"releaseId\":" + json::quote(result.release.manifest.releaseId) +
      ",\"manifestSha256\":" + json::quote(result.release.manifestSha256) + ",\"selection\":" + json::quote(result.selection) +
      ",\"committed\":" + (result.committed ? "true" : "false") + ",\"recovered\":" + (result.recovered ? "true" : "false") +
      ",\"pending\":" + (result.pending ? "true" : "false") + ",\"originalRunning\":" + json::quote(process) +
      ",\"error\":" + failureJson(result.error) + "}";
}
}
