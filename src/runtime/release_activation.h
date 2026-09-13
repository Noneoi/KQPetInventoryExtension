#pragma once
#include "release_manifest.h"

namespace kqpet::release {
enum class ProcessState { Stopped, Running, Unknown };
using RunningProbe = std::function<ProcessState(const std::filesystem::path& clientRoot)>;
ProcessState originalProcessState(const std::filesystem::path& clientRoot);
enum class FaultPoint { BeforePreviousWrite, AfterPreviousFlush, AfterPreviousReplace,
  BeforeActiveWrite, AfterActiveFlush, AfterActiveReplace, BeforePendingDelete, AfterPendingDelete };
using FaultHook = std::function<bool(FaultPoint)>;

class DeploymentLock final {
public:
  ~DeploymentLock();
  DeploymentLock(const DeploymentLock&) = delete;
  DeploymentLock& operator=(const DeploymentLock&) = delete;
  static std::unique_ptr<DeploymentLock> acquire(const std::filesystem::path& runtimeRoot, Failure* error);
private:
  DeploymentLock() = default;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::shared_ptr<FilePins> directoryPins_;
};
struct ActiveRecord { std::string activeReleaseId, previousReleaseId, manifestSha256; };
struct PendingRecord { std::string releaseId, manifestSha256; };
bool parseActive(const std::string& text, ActiveRecord* record, Failure* error);
bool parsePending(const std::string& text, PendingRecord* record, Failure* error);
std::string activeJson(const ActiveRecord& record);
std::string pendingJson(const PendingRecord& record);
struct Selection {
  bool ok = false;
  bool committed = false;
  bool recovered = false;
  bool pending = false;
  ProcessState originalRunning = ProcessState::Unknown;
  std::string selection = "none";
  Validation release;
  Failure error;
};
// Resolve never authorizes a pending release. A valid old active always wins;
// an independently verified previous may repair a missing/corrupt active only
// when no original process is running. Running/Unknown can still select it.
Selection resolveRelease(const std::filesystem::path& clientRoot,
    RunningProbe probe = {}, FaultHook faults = {}, const ValidationOverrides* testsOnly = nullptr);
// This explicit operation is the only pending -> active commit path.
Selection activatePending(const std::filesystem::path& clientRoot,
    const std::string& expectedReleaseId = {}, const std::string& expectedManifestSha256 = {},
    RunningProbe probe = {}, FaultHook faults = {}, const ValidationOverrides* testsOnly = nullptr);
std::string selectionJson(const Selection& selection);
}
