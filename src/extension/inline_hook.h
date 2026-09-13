#pragma once

#include "target_profile.h"

#include <cstddef>
#include <array>

class InlineHook {
public:
  InlineHook() = default;
  ~InlineHook() = default;

  InlineHook(const InlineHook&) = delete;
  InlineHook& operator=(const InlineHook&) = delete;

  bool install(void* target, void* detour, const unsigned char* expectedBytes,
               std::size_t patchSize, TrampolinePolicy policy);
  bool createDisabled(void* target, void* detour, const unsigned char* expectedBytes,
                      std::size_t signatureSize, TrampolinePolicy policy);
  bool enable();
  bool removeDisabled();
  static bool supportsPolicy(TrampolinePolicy policy) {
    return policy == TrampolinePolicy::ExactRelocationFreePrologue;
  }
  void* trampoline() const { return trampoline_; }
  const char* error() const { return error_; }

private:
  void* target_ = nullptr;
  void* trampoline_ = nullptr;
  std::size_t patchSize_ = 0;
  std::array<unsigned char, 64> expected_{};
  bool enabled_ = false;
  const char* error_ = "";
};
