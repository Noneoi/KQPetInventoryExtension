#include "release_activation.h"
#include <cstdio>

namespace {
int failure(const kqpet::release::Failure& error) {
  using kqpet::release::Code;
  return error.code == Code::Running ? 10 : error.code == Code::RunningUnknown ? 11 : error.code == Code::LockBusy ? 12 : 2;
}
std::string argument(const wchar_t* value) { return kqpet::release::pathUtf8(std::filesystem::path(value)); }
}
int wmain(int argc, wchar_t** argv) {
  using namespace kqpet::release;
  const std::wstring command = argc > 1 ? argv[1] : L"";
  if (command == L"--identity" && argc == 3) {
    BuildIdentity identity; Failure error;
    if (readBuildIdentity(argv[2], &identity, &error)) { std::puts(identity.canonicalJson.c_str()); return 0; }
    std::puts(("{\"ok\":false,\"error\":" + failureJson(error) + "}").c_str()); return failure(error);
  }
  if (command == L"--bootstrap-protocol" && argc == 3) {
    unsigned protocol = 0; Failure error;
    const bool read = readBootstrapProtocol(argv[2], &protocol, &error);
    const bool supported = read && protocol == kBootstrapProtocol;
    if (read && !supported) error = {Code::UnsupportedProtocol, "bootstrap", "unsupported stable bootstrap protocol", 0};
    std::puts(("{\"ok\":" + std::string(supported ? "true" : "false") + ",\"bootstrapProtocol\":" +
        std::to_string(protocol) + ",\"error\":" + failureJson(error) + "}").c_str());
    return supported ? 0 : failure(error);
  }
  if (command == L"--validate" && (argc == 4 || argc == 5)) {
    const auto result = validateRelease(argv[2], argument(argv[3]), argc == 5 ? argument(argv[4]) : std::string{});
    std::puts(validationJson(result).c_str()); return result.valid ? 0 : failure(result.error);
  }
  if (command == L"--resolve" && (argc == 3 || (argc == 4 && std::wstring(argv[3]) == L"--no-activate"))) {
    const auto result = resolveRelease(argv[2]); std::puts(selectionJson(result).c_str()); return result.ok ? 0 : failure(result.error);
  }
  if (command == L"--activate" && argc >= 3 && argc <= 5) {
    const auto result = activatePending(argv[2], argc >= 4 ? argument(argv[3]) : std::string{}, argc == 5 ? argument(argv[4]) : std::string{});
    std::puts(selectionJson(result).c_str()); return result.ok ? 0 : failure(result.error);
  }
  std::fputs("{\"ok\":false,\"error\":{\"code\":\"usage\",\"message\":\"--identity PE | --bootstrap-protocol PE | --validate runtimeRoot releaseId [manifestSha] | --resolve clientRoot [--no-activate] | --activate clientRoot [expectedReleaseId] [expectedManifestSha]\"}}\n", stdout);
  return 64;
}
