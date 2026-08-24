#include "inline_hook.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::size_t kAbsoluteJumpSize = 14;

void writeAbsoluteJump(unsigned char* destination, const void* address) {
  destination[0] = 0xFF;
  destination[1] = 0x25;
  destination[2] = 0x00;
  destination[3] = 0x00;
  destination[4] = 0x00;
  destination[5] = 0x00;
  const auto value = reinterpret_cast<std::uint64_t>(address);
  std::memcpy(destination + 6, &value, sizeof(value));
}

}  // namespace

bool InlineHook::install(void* target, void* detour, const unsigned char* expectedBytes,
                         std::size_t patchSize, TrampolinePolicy policy) {
  static_assert(sizeof(void*) == 8, "Only x64 is supported");
  if (!supportsPolicy(policy)) {
    error_ = "profile does not authorize a relocation-free trampoline";
    return false;
  }
  if (!target || !detour || !expectedBytes || patchSize < kAbsoluteJumpSize) {
    error_ = "invalid hook arguments";
    return false;
  }
  if (std::memcmp(target, expectedBytes, patchSize) != 0) {
    error_ = "target prologue does not exactly match the selected profile";
    return false;
  }

  const std::size_t trampolineSize = patchSize + kAbsoluteJumpSize;
  auto* trampoline = static_cast<unsigned char*>(
      VirtualAlloc(nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!trampoline) {
    error_ = "VirtualAlloc failed";
    return false;
  }
  std::memcpy(trampoline, target, patchSize);
  writeAbsoluteJump(trampoline + patchSize,
                    static_cast<unsigned char*>(target) + patchSize);
  target_ = target;
  trampoline_ = trampoline;
  patchSize_ = patchSize;

  DWORD oldProtection = 0;
  if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    target_ = nullptr;
    trampoline_ = nullptr;
    patchSize_ = 0;
    error_ = "VirtualProtect failed";
    return false;
  }

  std::array<unsigned char, 64> patch{};
  if (patchSize > patch.size()) {
    DWORD ignored = 0;
    VirtualProtect(target, patchSize, oldProtection, &ignored);
    VirtualFree(trampoline, 0, MEM_RELEASE);
    target_ = nullptr;
    trampoline_ = nullptr;
    patchSize_ = 0;
    error_ = "patch is too large";
    return false;
  }
  patch.fill(0x90);
  writeAbsoluteJump(patch.data(), detour);
  std::memcpy(target, patch.data(), patchSize);
  FlushInstructionCache(GetCurrentProcess(), target, patchSize);

  DWORD ignored = 0;
  VirtualProtect(target, patchSize, oldProtection, &ignored);
  return true;
}
