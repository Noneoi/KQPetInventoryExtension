#include "target_check.h"

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
  using namespace kqpet::compatibility;
  if (argc == 2 && std::wstring(argv[1]) == L"--profiles") {
    std::puts(profilesJson().c_str());
    return 0;
  }
  if ((argc != 3 && argc != 5) || (std::wstring(argv[1]) != L"--target" &&
      std::wstring(argv[1]) != L"--identify") || (argc == 5 && std::wstring(argv[3]) != L"--extension")) {
    std::fputs("{\"supported\":false,\"error\":\"usage: --target <original-exe> [--extension <dll>], --identify <exe> or --profiles\"}\n", stdout);
    return 64;
  }
  const TargetReport report = std::wstring(argv[1]) == L"--identify" ? identifyTargetFile(argv[2]) :
      checkTargetFile(argv[2], argc == 5 ? argv[4] : L"");
  std::puts(reportJson(report).c_str());
  return report.supported ? 0 : 2;
}
