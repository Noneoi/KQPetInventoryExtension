#include "diagnostics/target_compatibility_guard.h"
#include "diagnostics/target_profile_registry.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

// A mapped image with one executable and one data section. It contains no
// client code beyond the public profile signatures and is never executed.
std::vector<unsigned char> image() {
  std::vector<unsigned char> bytes(0x4000);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->FileHeader.NumberOfSections = 2;
  nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(bytes.size());
  nt->OptionalHeader.SizeOfHeaders = 0x400;
  nt->OptionalHeader.SectionAlignment = 0x1000;
  nt->OptionalHeader.FileAlignment = 0x200;
  auto* sections = IMAGE_FIRST_SECTION(nt);
  sections[0].VirtualAddress = 0x1000;
  sections[0].Misc.VirtualSize = 0x1000;
  sections[0].PointerToRawData = 0x400;
  sections[0].SizeOfRawData = 0x1000;
  sections[0].Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
  sections[1].VirtualAddress = 0x2000;
  sections[1].Misc.VirtualSize = 0x1000;
  sections[1].PointerToRawData = 0x1400;
  sections[1].SizeOfRawData = 0x1000;
  sections[1].Characteristics = IMAGE_SCN_MEM_READ;
  return bytes;
}
void put(std::vector<unsigned char>& bytes, std::size_t rva,
         const TargetEndpointProfile& endpoint) {
  std::copy_n(endpoint.signature.begin(), endpoint.signatureSize, bytes.begin() + rva);
}
}

int main() {
  const auto* v114 = TargetProfileRegistry::find(L"KQProV1.1.4.exe", L"1.1.4");
  check(v114 != nullptr, "V1.1.4 must be registered");
  if (!v114) return 1;
  check(TargetProfileRegistry::find(L"kqpro1.1.4.EXE", L"1.1.4") == v114,
        "registered aliases must be case insensitive");
  check(TargetProfileRegistry::find(L"KQProV1.1.3.exe", L"1.1.3") != nullptr,
        "V1.1.3 compatibility profile must remain available");
  check(!TargetProfileRegistry::find(L"KQProV1.1.5.exe", L"1.1.5") &&
        !TargetProfileRegistry::find(L"KQProV1.1.4.exe", L"1.1.3") &&
        !TargetProfileRegistry::find(L"KQProV1.1.4-backup.exe", L"1.1.4"),
        "historical names alone must not invent a callable interface family");
  check(v114->dispatch.rva == 0x140270 && v114->serviceGetter.rva == 0x13FDA0 &&
        v114->commandSender.rva == 0x140DC0, "historical RVA evidence must remain available");
  check(v114->dispatch.signatureSize == 17 && v114->dispatch.trampolinePolicy ==
        TrampolinePolicy::ExactRelocationFreePrologue &&
        v114->serviceGetter.trampolinePolicy == TrampolinePolicy::Unsupported,
        "only the verified relocation-free dispatch may be patched");

  for (const auto* source : {&v114->dispatch, &v114->serviceGetter, &v114->commandSender}) {
    auto endpoint = *source;
    endpoint.rva = 0x1100;
    auto bytes = image();
    put(bytes, endpoint.rva, endpoint);
    auto resolved = TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), endpoint);
    check(resolved.matchMethod == CompatibilityMatchMethod::UniqueSignature && resolved.matched(),
          "V1.1.4 known RVA must also be unique in executable code");
    endpoint.rva = 0x1800;
    resolved = TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), endpoint);
    check(resolved.matchMethod == CompatibilityMatchMethod::UniqueSignature && resolved.matched() &&
          resolved.address == reinterpret_cast<std::uintptr_t>(bytes.data()) + 0x1100,
          "historical RVA must not block a uniquely relocated interface");
    endpoint.rva = 0x1100;
    put(bytes, 0x1500, endpoint);
    resolved = TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), endpoint);
    check(resolved.matchMethod == CompatibilityMatchMethod::AmbiguousSignature && !resolved.matched(),
          "ambiguity must reject even a matching reviewed RVA");
    bytes = image();
    endpoint.rva = 0x2100;
    put(bytes, endpoint.rva, endpoint);
    check(!TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), endpoint).matched(),
          "matching known RVA in data must be rejected");
    bytes = image();
    endpoint.rva = 0x1ff8;
    put(bytes, endpoint.rva, endpoint);
    check(!TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), endpoint).matched(),
          "signature crossing code section boundary must be rejected");
    endpoint.rva = (std::numeric_limits<std::uintptr_t>::max)();
    check(!TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), endpoint).matched(),
          "invalid overflowing RVA must be rejected");
  }
  // A 21-byte sender prefix identifies unrelated functions in the new client.
  // Only the complete 28-byte V1.1.4 signature may authorize that endpoint.
  auto bytes = image();
  auto sender = v114->commandSender;
  sender.rva = 0x1100;
  std::copy_n(sender.signature.begin(), 21, bytes.begin() + sender.rva);
  check(!TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), sender).matched(),
        "old ambiguous sender prefix must not suffice");
  check(!TargetCompatibilityGuard::resolveEndpoint(nullptr, 0, sender).matched() &&
        !TargetCompatibilityGuard::resolveEndpoint(bytes.data(), 16, sender).matched() &&
        !TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size() - 1, sender).matched(),
        "null and truncated images must fail closed");
  reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data())->e_lfanew = 0x7fffffff;
  check(!TargetCompatibilityGuard::resolveEndpoint(bytes.data(), bytes.size(), sender).matched(),
        "out-of-bounds PE headers must fail closed");
  if (failures) return 1;
  std::puts("PASS: version-independent unique endpoint resolution and rejection paths");
  return 0;
}
