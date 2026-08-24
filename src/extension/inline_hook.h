#pragma once

#include "target_profile.h"

#include <cstddef>

class InlineHook {
public:
  InlineHook() = default;
  ~InlineHook() = default;

  InlineHook(const InlineHook&) = delete;
  InlineHook& operator=(const InlineHook&) = delete;

  bool install(void* target, void* detour, const unsigned char* expectedBytes,
               std::size_t patchSize, TrampolinePolicy policy);
  static bool supportsPolicy(TrampolinePolicy policy) {
    return policy == TrampolinePolicy::ExactRelocationFreePrologue;
  }
  void* trampoline() const { return trampoline_; }
  const char* error() const { return error_; }

private:
  void* target_ = nullptr;
  void* trampoline_ = nullptr;
  std::size_t patchSize_ = 0;
  const char* error_ = "";
};
