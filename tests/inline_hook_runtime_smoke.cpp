#include "inline_hook.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
using Function = int(__fastcall*)(int, int);
std::atomic<Function> original{nullptr};
std::atomic<int> detourCalls{0};
std::atomic<int> originalCalls{0};
int __fastcall detour(int left, int right) {
  ++detourCalls;
  Function call = original.load(std::memory_order_acquire);
  if (!call) return -123456;
  ++originalCalls;
  return call(left, right);
}
}

int main() {
  // Isolated process only; never inject this instrumented fixture into a game.
  bool ok = true;
  const auto check = [&](bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ok = false; }
  };
  std::array<unsigned char, 32> code{};
  code.fill(0x90);
  // mov eax,ecx; add eax,edx; ret. It has no external state or target dependency.
  code[0] = 0x8b; code[1] = 0xc1; code[2] = 0x03; code[3] = 0xc2; code[4] = 0xc3;
  auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096,
      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  if (!memory) return 1;
  std::memcpy(memory, code.data(), code.size());
  Function target = reinterpret_cast<Function>(memory);
  check(target(17, 23) == 40, "unhooked fixture did not execute");
  InlineHook hook;
  check(hook.createDisabled(memory, reinterpret_cast<void*>(&detour), code.data(),
                           17, TrampolinePolicy::ExactRelocationFreePrologue), hook.error());
  check(std::memcmp(memory, code.data(), code.size()) == 0 && target(17, 23) == 40,
        "creating disabled hook modified target");
  original.store(reinterpret_cast<Function>(hook.trampoline()), std::memory_order_release);
  check(original.load()(21, 4) == 25, "disabled trampoline corrupted arguments/result");
  InlineHook duplicate;
  check(!duplicate.createDisabled(memory, reinterpret_cast<void*>(&detour), code.data(),
                                 17, TrampolinePolicy::ExactRelocationFreePrologue),
        "duplicate target registration was accepted");
  std::atomic_bool run{true};
  std::atomic_bool incorrect{false};
  std::vector<std::thread> callers;
  for (int thread = 0; thread < 4; ++thread)
    callers.emplace_back([&, thread] {
      while (run.load())
        if (target(thread + 17, 23) != thread + 40) incorrect.store(true);
    });
  check(hook.enable(), hook.error());
  for (int call = 0; call < 10000; ++call)
    if (target(call, 7) != call + 7) incorrect.store(true);
  run.store(false);
  for (auto& caller : callers) caller.join();
  check(!incorrect.load(), "concurrent hook call corrupted arguments/original return");
  check(detourCalls.load() >= 10000 && detourCalls.load() == originalCalls.load(),
        "detour did not invoke original exactly once");
  check(!hook.enable() && !hook.removeDisabled(), "enabled process-lifetime hook was reenabled/removed");
  InlineHook foreign;
  check(!foreign.createDisabled(memory, reinterpret_cast<void*>(&detour), code.data(),
                                17, TrampolinePolicy::ExactRelocationFreePrologue),
        "foreign patch was overwritten");

  auto* second = memory + 128;
  std::memcpy(second, code.data(), code.size());
  InlineHook changed;
  check(changed.createDisabled(second, reinterpret_cast<void*>(&detour), code.data(),
                              17, TrampolinePolicy::ExactRelocationFreePrologue), changed.error());
  second[0] = 0xcc;
  check(!changed.enable() && second[0] == 0xcc, "enable replaced a conflicting post-create patch");
  check(changed.removeDisabled(), "disabled failed hook cleanup failed");
  // Enabled target/trampoline intentionally remain alive until process teardown.
  if (ok) std::puts("PASS: real isolated MinHook calls, publication, duplicates, conflicts, cleanup and concurrency");
  return ok ? 0 : 1;
}
