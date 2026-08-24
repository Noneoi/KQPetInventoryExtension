#include "inline_hook.h"

#include <array>
#include <cstdio>
#include <string>

int main() {
  bool ok = true;
  if (!InlineHook::supportsPolicy(
          TrampolinePolicy::ExactRelocationFreePrologue) ||
      InlineHook::supportsPolicy(TrampolinePolicy::Unsupported)) {
    std::fprintf(stderr, "FAIL: inline hook policy classification is incorrect\n");
    ok = false;
  }
  std::array<unsigned char, 17> bytes{};
  InlineHook hook;
  if (hook.install(bytes.data(), bytes.data() + 1, bytes.data(), bytes.size(),
                   TrampolinePolicy::Unsupported) ||
      std::string(hook.error()).find("does not authorize") == std::string::npos) {
    std::fprintf(stderr, "FAIL: unprofiled trampoline was not rejected\n");
    ok = false;
  }
  if (!ok) return 1;
  std::fprintf(stdout, "PASS: inline hook is profile-bound and fail closed\n");
  return 0;
}
