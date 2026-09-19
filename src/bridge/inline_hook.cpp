#include "inline_hook.h"
#include <windows.h>
#include <MinHook.h>
#include <cstring>
#include <cstdint>
#include <mutex>

namespace {
std::mutex installationMutex;
bool initialized = false;
bool executableSpan(const void* address, std::size_t size) {
  MEMORY_BASIC_INFORMATION memory{};
  if (!address || !size || !VirtualQuery(address, &memory, sizeof(memory)) ||
      memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
  const DWORD protection = memory.Protect & 0xff;
  if (protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
      protection != PAGE_EXECUTE_WRITECOPY) return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
  return begin >= region && begin - region <= memory.RegionSize &&
         size <= memory.RegionSize - (begin - region);
}
}

bool InlineHook::createDisabled(void* target, void* detour,
    const unsigned char* expectedBytes, std::size_t signatureSize, TrampolinePolicy policy) {
  static_assert(sizeof(void*) == 8, "Only x64 is supported");
  std::lock_guard<std::mutex> lock(installationMutex);
  if (!supportsPolicy(policy)) {
    error_ = "profile does not authorize this Dispatch hook";
    return false;
  }
  if (target_ || !target || !detour || !expectedBytes || signatureSize < 5 ||
      signatureSize > expected_.size() || !executableSpan(target, signatureSize)) {
    error_ = "invalid, unreadable, non-executable or already-created hook target";
    return false;
  }
  if (std::memcmp(target, expectedBytes, signatureSize) != 0) {
    error_ = "target prologue conflicts with selected profile; refusing foreign hook";
    return false;
  }
  if (!initialized) {
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) { error_ = MH_StatusToString(status); return false; }
    initialized = true;
  }
  void* trampoline = nullptr;
  const MH_STATUS status = MH_CreateHook(target, detour, &trampoline);
  if (status != MH_OK) { error_ = MH_StatusToString(status); return false; }
  target_ = target;
  trampoline_ = trampoline;
  patchSize_ = signatureSize;
  std::memcpy(expected_.data(), expectedBytes, signatureSize);
  error_ = "";
  return true;
}

bool InlineHook::enable() {
  std::lock_guard<std::mutex> lock(installationMutex);
  if (!target_ || enabled_) { error_ = "hook is absent or already enabled"; return false; }
  if (!executableSpan(target_, patchSize_) ||
      std::memcmp(target_, expected_.data(), patchSize_) != 0) {
    error_ = "target changed after creation; refusing conflicting patch";
    return false;
  }
  // Only our verified Dispatch is managed. Never use MH_ALL_HOOKS. Upstream
  // thread-context limitations remain; MH_OK is not universal suspension proof.
  const MH_STATUS status = MH_EnableHook(target_);
  if (status != MH_OK) { error_ = MH_StatusToString(status); return false; }
  enabled_ = true;
  return true;
}

bool InlineHook::removeDisabled() {
  std::lock_guard<std::mutex> lock(installationMutex);
  if (!target_ || enabled_) return false;
  const MH_STATUS status = MH_RemoveHook(target_);
  if (status != MH_OK) { error_ = MH_StatusToString(status); return false; }
  target_ = nullptr;
  trampoline_ = nullptr;
  patchSize_ = 0;
  return true;
}

bool InlineHook::install(void* target, void* detour, const unsigned char* expectedBytes,
                         std::size_t signatureSize, TrampolinePolicy policy) {
  if (!createDisabled(target, detour, expectedBytes, signatureSize, policy)) return false;
  if (enable()) return true;
  removeDisabled();
  return false;
}