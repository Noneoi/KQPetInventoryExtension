#include "pe_view.h"
#include "target_check.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using namespace kqpet::compatibility;

namespace {
int failures = 0;
void check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
IMAGE_NT_HEADERS64* nt(std::vector<unsigned char>& raw) {
  return reinterpret_cast<IMAGE_NT_HEADERS64*>(raw.data() + 0x80);
}
IMAGE_SECTION_HEADER* sections(std::vector<unsigned char>& raw) { return IMAGE_FIRST_SECTION(nt(raw)); }
std::vector<unsigned char> fixture() {
  std::vector<unsigned char> raw(0xa00);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(raw.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
  auto* image = nt(raw);
  image->Signature = IMAGE_NT_SIGNATURE;
  image->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  image->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
  image->FileHeader.NumberOfSections = 2;
  image->FileHeader.TimeDateStamp = 0x12345678;
  image->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  image->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  image->OptionalHeader.ImageBase = 0x140000000;
  image->OptionalHeader.MajorOperatingSystemVersion = 6;
  image->OptionalHeader.MajorSubsystemVersion = 6;
  image->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
  image->OptionalHeader.SizeOfImage = 0x4000;
  image->OptionalHeader.SizeOfHeaders = 0x400;
  image->OptionalHeader.SectionAlignment = 0x1000;
  image->OptionalHeader.FileAlignment = 0x200;
  image->OptionalHeader.NumberOfRvaAndSizes = 16;
  image->OptionalHeader.DataDirectory[0] = {0x2000, 0x80};
  auto* section = sections(raw);
  section[0].VirtualAddress = 0x1000; section[0].Misc.VirtualSize = 0x200;
  section[0].PointerToRawData = 0x400; section[0].SizeOfRawData = 0x200;
  section[0].Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
  section[1].VirtualAddress = 0x2000; section[1].Misc.VirtualSize = 0x300;
  section[1].PointerToRawData = 0x600; section[1].SizeOfRawData = 0x400;
  section[1].Characteristics = IMAGE_SCN_MEM_READ;
  auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(raw.data() + 0x600);
  exports->NumberOfFunctions = 1; exports->NumberOfNames = 1;
  exports->AddressOfFunctions = 0x2028; exports->AddressOfNames = 0x2030;
  exports->AddressOfNameOrdinals = 0x2038;
  *reinterpret_cast<DWORD*>(raw.data() + 0x628) = 0x1010;
  *reinterpret_cast<DWORD*>(raw.data() + 0x630) = 0x2040;
  std::memcpy(raw.data() + 0x640, "testExport", 11);
  return raw;
}
std::vector<unsigned char> map(const std::vector<unsigned char>& raw) {
  std::vector<unsigned char> mapped(0x4000);
  std::copy_n(raw.data(), 0x400, mapped.data());
  std::copy_n(raw.data() + 0x400, 0x200, mapped.data() + 0x1000);
  std::copy_n(raw.data() + 0x600, 0x400, mapped.data() + 0x2000);
  return mapped;
}
void put(std::vector<unsigned char>& raw, std::size_t offset, const EndpointProfile& endpoint) {
  std::copy_n(endpoint.signature.begin(), endpoint.signatureSize, raw.begin() + offset);
}
void addVersion(std::vector<unsigned char>& raw, WORD major, WORD minor) {
  sections(raw)[1].Misc.VirtualSize = 0x400;
  nt(raw)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE] = {0x2100, 0x180};
  auto* root = raw.data() + 0x700;
  for (DWORD offset : {DWORD(0), DWORD(24), DWORD(48)}) {
    auto* directory = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY*>(root + offset);
    directory->NumberOfIdEntries = 1;
    auto* entry = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY_ENTRY*>(root + offset + 16);
    entry->Id = offset == 0 ? 16 : offset == 24 ? 1 : 1033;
    entry->OffsetToData = offset == 48 ? 72 : (offset + 24) | 0x80000000;
  }
  auto* data = reinterpret_cast<IMAGE_RESOURCE_DATA_ENTRY*>(root + 72);
  data->OffsetToData = 0x2160; data->Size = 92;
  auto* version = root + 96;
  *reinterpret_cast<WORD*>(version) = 92;
  *reinterpret_cast<WORD*>(version + 2) = sizeof(VS_FIXEDFILEINFO);
  const wchar_t key[] = L"VS_VERSION_INFO";
  std::memcpy(version + 6, key, sizeof(key));
  auto* value = reinterpret_cast<VS_FIXEDFILEINFO*>(version + 40);
  value->dwSignature = 0xfeef04bd; value->dwStrucVersion = 0x10000;
  value->dwFileVersionMS = (DWORD(major) << 16) | minor;
  value->dwFileOS = VOS_NT_WINDOWS32; value->dwFileType = VFT_APP;
}
void writeFixture(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
}

int main() {
  const Profile* v114 = findProfile(L"KQProV1.1.4.exe", L"1.1.4");
  const Profile* v113 = findProfile(L"KQProV1.1.3.exe", L"1.1.3");
  check(v114 && v113 && registeredProfiles().size() == 2, "reviewed profiles were not generated");
  if (!v114 || !v113) return 1;
  check(findProfile(L"kqpro1.1.4.EXE") == v114 && !findProfile(L"KQProV1.1.4-backup.exe") &&
        !findProfile(L"KQProV1.1.5.exe") && !findProfile(L"KQProV1.1.4.exe", L"1.1.3"),
        "historical alias lookup must not fabricate an interface family");
  check(v114->dispatch.rva == 0x140270 && v114->serviceGetter.rva == 0x13FDA0 &&
        v114->commandSender.rva == 0x140DC0 && v114->commandSender.signatureSize == 28 &&
        v114->executableSha256 == "8B4230AF89224BAF924FE16019DFA31F7A2A694E750B2E46B7785F3CAA1A356B",
        "historical evidence metadata was changed");
  for (const EndpointProfile* source : {&v114->dispatch, &v114->serviceGetter, &v114->commandSender}) {
    check(source->matchPolicy == MatchPolicy::UniqueSignature, "interface family endpoint lost mandatory uniqueness");
    EndpointProfile endpoint = *source;
    endpoint.rva = 0x1010;
    auto raw = fixture(); put(raw, 0x410, endpoint);
    auto mapped = map(raw);
    RawPeView disk(raw.data(), raw.size()); MappedPeView memory(mapped.data(), mapped.size());
    check(disk.valid() && memory.valid(), "valid raw/mapped fixture was rejected");
    check(disk.resolve(endpoint).rva == 0x1010 && memory.resolve(endpoint).rva == 0x1010 &&
          disk.resolve(endpoint).method == MatchMethod::UniqueSignature &&
          memory.resolve(endpoint).method == MatchMethod::UniqueSignature,
          "raw/mapped endpoint rules diverged");
    put(raw, 0x450, endpoint); mapped = map(raw);
    check(!RawPeView(raw.data(), raw.size()).resolve(endpoint).matched() &&
          !MappedPeView(mapped.data(), mapped.size()).resolve(endpoint).matched(),
          "matching known RVA hid a second executable signature");
    endpoint.matchPolicy = MatchPolicy::KnownRva;
    check(RawPeView(raw.data(), raw.size()).resolve(endpoint).method == MatchMethod::KnownRva,
          "historical known-RVA policy was changed to a uniqueness policy");
    endpoint.rva = 0x1080;
    check(RawPeView(raw.data(), raw.size()).resolve(endpoint).method == MatchMethod::Conflict,
          "conflicting reviewed RVA fell back to a similar function");
    endpoint.rva = 0x2010;
    put(raw, 0x610, endpoint);
    check(!RawPeView(raw.data(), raw.size()).resolve(endpoint).matched(), "data-section signature authorized code");
  }
  auto endpoint = v114->dispatch; endpoint.rva = 0x1010; endpoint.matchPolicy = MatchPolicy::UniqueAtKnownRva;
  auto raw = fixture(); put(raw, 0x410, endpoint);
  auto mapped = map(raw);
  check(RawPeView(raw.data(), raw.size()).executableExport("testExport") == 0x1010 &&
        MappedPeView(mapped.data(), mapped.size()).executableExport("testExport") == 0x1010 &&
        !RawPeView(raw.data(), raw.size()).executableExport("missing"), "raw/mapped executable export mismatch");
  for (DWORD target : {DWORD(0), DWORD(0x2040), DWORD(0x2090), DWORD(0xffffffff)}) {
    auto bad = raw; *reinterpret_cast<DWORD*>(bad.data() + 0x628) = target;
    check(!RawPeView(bad.data(), bad.size()).executableExport("testExport"), "null/forwarded/data/overflow export accepted");
  }
  auto bad = raw; *reinterpret_cast<WORD*>(bad.data() + 0x638) = 1;
  check(!RawPeView(bad.data(), bad.size()).executableExport("testExport"), "invalid ordinal accepted");
  for (std::size_t length : {std::size_t(0), std::size_t(16), std::size_t(0x100), raw.size() - 1})
    check(!RawPeView(raw.data(), length).valid(), "truncated raw PE accepted");
  bad = raw; nt(bad)->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
  check(!RawPeView(bad.data(), bad.size()).valid(), "x86 image accepted");
  bad = raw; reinterpret_cast<IMAGE_DOS_HEADER*>(bad.data())->e_lfanew = 0x7fffffff;
  check(!RawPeView(bad.data(), bad.size()).valid(), "overflowed NT offset accepted");
  bad = raw; sections(bad)[1].VirtualAddress = 0x1100;
  check(!RawPeView(bad.data(), bad.size()).valid(), "overlapping virtual sections accepted");
  bad = raw; sections(bad)[1].PointerToRawData = 0x500;
  check(!RawPeView(bad.data(), bad.size()).valid(), "overlapping raw sections accepted");
  bad = raw; sections(bad)[0].PointerToRawData = 0xfffffff0;
  check(!RawPeView(bad.data(), bad.size()).valid(), "overflowed raw range accepted");
  bad = raw; endpoint.rva = 0x11f8; put(bad, 0x5f8, endpoint);
  check(!RawPeView(bad.data(), bad.size()).resolve(endpoint).matched(), "cross-section prologue accepted");
  bad = raw; sections(bad)[0].Misc.VirtualSize = 0x100;
  endpoint.rva = 0x1110; put(bad, 0x510, endpoint); mapped = map(bad);
  check(!RawPeView(bad.data(), bad.size()).resolve(endpoint).matched() &&
        !MappedPeView(mapped.data(), mapped.size()).resolve(endpoint).matched(), "raw-only padding became mapped signature evidence");
  bad = raw; sections(bad)[0].SizeOfRawData = 0x100;
  mapped = map(bad); put(mapped, 0x1110, endpoint);
  check(!MappedPeView(mapped.data(), mapped.size()).resolve(endpoint).matched(), "mapped zero-fill became file-supported evidence");
  mapped = map(raw); endpoint.rva = 0x1010;
  void* pages = VirtualAlloc(nullptr, mapped.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  check(pages != nullptr, "cannot allocate unreadable-page fixture");
  if (pages) {
    std::memcpy(pages, mapped.data(), mapped.size());
    DWORD old = 0;
    check(VirtualProtect(static_cast<unsigned char*>(pages) + 0x1000, 0x1000, PAGE_NOACCESS, &old),
          "cannot protect mapped code fixture");
    check(!MappedPeView(pages, mapped.size()).resolve(endpoint).matched(), "unreadable mapped page authorized an entry");
    VirtualFree(pages, 0, MEM_RELEASE);
  }
  // An unregistered, arbitrarily named test PE has all three interfaces at
  // unrelated RVAs and a different RIP-relative getter displacement.
  auto adaptive = fixture();
  put(adaptive, 0x410, v114->dispatch);
  put(adaptive, 0x450, v114->serviceGetter);
  put(adaptive, 0x490, v114->commandSender);
  *reinterpret_cast<std::int32_t*>(adaptive.data() + 0x459) = 0x2090 - 0x1050 - 13;
  auto adaptiveMapped = map(adaptive);
  auto identified = identifyTargetImage(RawPeView(adaptive.data(), adaptive.size()));
  const auto identifiedMapped = identifyTargetImage(MappedPeView(adaptiveMapped.data(), adaptiveMapped.size()));
  check(identified.supported && identifiedMapped.supported && identified.dispatch.rva == 0x1010 &&
        identified.serviceGetter.rva == 0x1050 && identified.commandSender.rva == 0x1090 &&
        identifiedMapped.serviceGetter.rva == identified.serviceGetter.rva,
        "relocated interface family or variable RIP displacement was rejected");
  const auto fixtureRoot = std::filesystem::temp_directory_path() /
      (L"kqpet-adaptive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::filesystem::create_directory(fixtureRoot);
  const auto unknownPath = fixtureRoot / L"Unregistered-client-99.42.exe";
  addVersion(adaptive, 99, 42);
  writeFixture(unknownPath, adaptive);
  const auto observedFile = identifyTargetFile(unknownPath.wstring());
  const auto runtimeMissing = checkTargetFile(unknownPath.wstring());
  check(observedFile.supported && observedFile.executableSha256 != v114->executableSha256 &&
        runtimeMissing.profile && runtimeMissing.error.find("Qt6Core.dll") != std::string::npos,
        "filename/hash/version restriction precedes actual runtime checks");
  auto qt = fixture(); addVersion(qt, 6, 9);
  nt(qt)->FileHeader.Characteristics |= IMAGE_FILE_DLL;
  std::memcpy(qt.data() + 0x640, "qt_needed", 10);
  for (const auto& name : v114->qtModules) writeFixture(fixtureRoot / name, qt);
  auto qcef = qt;
  std::memcpy(qcef.data() + 0x640, v114->qcefExecuteJavascriptSymbol.c_str(), v114->qcefExecuteJavascriptSymbol.size() + 1);
  writeFixture(fixtureRoot / v114->qcefViewModule, qcef);
  auto extension = fixture();
  nt(extension)->FileHeader.Characteristics |= IMAGE_FILE_DLL;
  nt(extension)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x20a0, 40};
  auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(extension.data() + 0x6a0);
  imports->OriginalFirstThunk = 0x20e0; imports->FirstThunk = 0x20e0; imports->Name = 0x20d0;
  std::memcpy(extension.data() + 0x6d0, "Qt6Core.dll", 12);
  *reinterpret_cast<ULONGLONG*>(extension.data() + 0x6e0) = 0x20f0;
  std::memcpy(extension.data() + 0x6f2, "qt_needed", 10);
  const auto extensionPath = fixtureRoot / L"test-extension.dll";
  writeFixture(extensionPath, extension);
  const auto futureVersion = checkTargetFile(unknownPath.wstring(), extensionPath.wstring());
  check(futureVersion.supported && futureVersion.modules.front().version == L"99.42.0" &&
        futureVersion.modules[1].version == L"6.9.0",
        "new client/Qt minor versions with matching interfaces and imports were blocked");
  if (!futureVersion.supported) std::fprintf(stderr, "adaptive file detail: %s\n", futureVersion.error.c_str());
  std::memcpy(qt.data() + 0x640, "not_needed", 11);
  writeFixture(fixtureRoot / v114->qtCoreModule, qt);
  check(!checkTargetFile(unknownPath.wstring(), extensionPath.wstring()).supported,
        "Qt runtime missing an actual extension import was accepted");
  std::memcpy(qt.data() + 0x640, "qt_needed", 10); addVersion(qt, 7, 0);
  writeFixture(fixtureRoot / v114->qtCoreModule, qt);
  check(!checkTargetFile(unknownPath.wstring(), extensionPath.wstring()).supported,
        "different Qt major ABI was accepted");
  for (const auto& name : v114->qtModules) std::filesystem::remove(fixtureRoot / name);
  std::filesystem::remove(fixtureRoot / v114->qcefViewModule);
  std::filesystem::remove(extensionPath);
  std::filesystem::remove(unknownPath); std::filesystem::remove(fixtureRoot);
  auto duplicate = adaptive; put(duplicate, 0x4d0, v114->dispatch);
  check(!identifyTargetImage(RawPeView(duplicate.data(), duplicate.size())).supported,
        "duplicate executable interface signature was authorized");
  auto wrongAbi = adaptive; wrongAbi[0x454] ^= 0x01;
  check(!identifyTargetImage(RawPeView(wrongAbi.data(), wrongAbi.size())).supported,
        "non-relocation getter opcode change was authorized");
  auto badReference = adaptive;
  *reinterpret_cast<std::int32_t*>(badReference.data() + 0x459) = INT32_MAX;
  check(!identifyTargetImage(RawPeView(badReference.data(), badReference.size())).supported,
        "getter RIP operand outside the bounded image was accepted");
  auto runtimeDll = adaptive; nt(runtimeDll)->FileHeader.Characteristics |= IMAGE_FILE_DLL;
  check(!identifyTargetImage(RawPeView(runtimeDll.data(), runtimeDll.size())).supported,
        "DLL with coincidental signatures was selected as a client executable");
  check(profilesJson().find(profileCatalogDigest()) != std::string::npos,
        "offline registry does not bind generated profile digest");
  check(!checkTargetFile(L"KQProV99.0.exe").supported, "unknown version was authorized by a file lookup");
  // Exercise the launcher's actual-process header preflight independently of
  // any game client. A same-path/same-size module with a different expected
  // timestamp must be rejected before a DLL can be injected.
  const auto* self = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
  const auto* selfDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
  const auto* selfNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(self + selfDos->e_lfanew);
  TargetReport selfReport;
  selfReport.profile = v114;
  selfReport.supported = true;
  selfReport.executablePath = currentExecutablePath();
  ModuleCheck selfModule;
  selfModule.name = std::filesystem::path(selfReport.executablePath).filename().wstring();
  selfModule.path = selfReport.executablePath;
  selfModule.imageSize = selfNt->OptionalHeader.SizeOfImage;
  selfModule.headerSize = selfNt->OptionalHeader.SizeOfHeaders;
  selfModule.timestamp = selfNt->FileHeader.TimeDateStamp;
  selfModule.valid = true;
  selfReport.modules.push_back(selfModule);
  check(checkProcessModulePaths(GetCurrentProcessId(), selfReport).supported,
        "matching process module header preflight failed");
  selfReport.modules.front().timestamp ^= 1;
  check(!checkProcessModulePaths(GetCurrentProcessId(), selfReport).supported,
        "same-path/same-size runtime module bypassed actual header identity check");
  if (failures) return 1;
  std::puts("PASS: shared raw/mapped PE bounds, adaptive interface families, unique relocated entries and exports");
  return 0;
}
