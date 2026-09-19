#include "loader/client_target.h"
#include "compatibility/target_check.h"
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class DirectoryFixture {
 public:
  DirectoryFixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporaryRoot = std::filesystem::temp_directory_path();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = temporaryRoot /
          ("kqpet-launcher-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate)) {
        path = candidate;
        return;
      }
    }
    throw std::runtime_error("could not create an isolated launcher test directory");
  }

  ~DirectoryFixture() {
    std::error_code error;
    // Remove only exact entries created by this fixture; never recurse.
    for (const auto& entry : entries) std::filesystem::remove(entry, error);
    std::filesystem::remove(path, error);
  }

  void addFile(const std::wstring& filename) {
    const auto entry = path / filename;
    require(!std::filesystem::exists(entry), "duplicate fixture filename");
    std::ofstream file(entry, std::ios::binary);
    require(static_cast<bool>(file.put(' ')), "could not create fixture file");
    entries.push_back(entry);
  }

  void addDirectory(const std::wstring& filename) {
    const auto entry = path / filename;
    require(std::filesystem::create_directory(entry), "could not create fixture subdirectory");
    entries.push_back(entry);
  }

  void addRecognizedClient(const std::wstring& filename) {
    addFile(filename);
    std::vector<unsigned char> bytes(0x800);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1; nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = 0x2000; nt->OptionalHeader.SizeOfHeaders = 0x400;
    nt->OptionalHeader.SectionAlignment = 0x1000; nt->OptionalHeader.FileAlignment = 0x200;
    auto* section = IMAGE_FIRST_SECTION(nt);
    section->VirtualAddress = 0x1000; section->Misc.VirtualSize = 0x400;
    section->PointerToRawData = 0x400; section->SizeOfRawData = 0x400;
    section->Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    const auto& profile = kqpet::compatibility::registeredProfiles().back();
    std::size_t offset = 0x410;
    for (const auto* endpoint : {&profile.dispatch, &profile.serviceGetter, &profile.commandSender}) {
      std::copy_n(endpoint->signature.begin(), endpoint->signatureSize, bytes.begin() + offset);
      if (endpoint->masked)
        *reinterpret_cast<std::int32_t*>(bytes.data() + offset + 9) = static_cast<std::int32_t>(0x1300 - (offset - 0x400 + 0x1000) - 13);
      offset += 0x80;
    }
    std::ofstream file(path / filename, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  std::filesystem::path path;

 private:
  std::vector<std::filesystem::path> entries;
};

}  // namespace

int main() {
  using namespace kqpet::launcher;
  try {
    for (const auto* filename : {L"KQProV1.1.4.exe", L"KQPro1.1.4.exe",
                                 L"kQpRoV1.1.4.ExE", L"KQPro1.3.exe",
                                 L"KQProV1.12.30.4.exe"}) {
      require(parseClientFilename(filename).has_value(), "valid client filename was rejected");
    }
    const auto parsed = parseClientFilename(L"KQProV1.12.300.exe");
    require(parsed && parsed->version == std::vector<std::uint32_t>{1, 12, 300},
            "version components were not parsed numerically");

    for (const auto* filename : {L"", L"KQPro.exe", L"KQProV1.exe", L"KQProVV1.1.4.exe",
                                 L"KQProV1.1.4-backup-999.exe", L"KQProV1.1.4_setup.exe",
                                 L"KQProSetupV99.0.0.exe", L"KQProV1.1.4 (1).exe",
                                 L"KQProV1.1.4.exe.bak", L"KQProV1.1.4.exe.exe",
                                 L"KQProV1..4.exe", L"KQProV.1.4.exe", L"KQProV1.1.4..exe",
                                 L"KQProV1.1.4.0.1.exe", L"KQProV1.1.-4.exe",
                                 L"KQProV1.1.4294967296.exe", L"KQProV999999999999999999999.0.exe",
                                 L"KQProV１.１.４.exe", L"subdir/KQProV1.1.4.exe"}) {
      require(!parseClientFilename(filename), "non-client or malformed filename was accepted");
    }
    require(preferClientFilename(L"KQProV1.1.4.exe", L"KQPro1.1.3.exe"),
            "V1.1.4 did not supersede V1.1.3");
    require(preferClientFilename(L"KQPro1.10.0.exe", L"KQProV1.9.99.exe"),
            "multi-digit version comparison is incorrect");
    require(preferClientFilename(L"KQPro1.1.4.1.exe", L"KQProV1.1.4.exe"),
            "fourth version component was ignored");
    require(!preferClientFilename(L"KQProV99.0.0_setup.exe", L"KQProV1.1.4.exe"),
            "installer was preferred to the client");

    std::vector<std::wstring> candidates{L"KQPro1.1.4.exe", L"KQProV1.1.4.exe",
                                         L"KQProV1.1.4.0.exe", L"KQProV01.01.04.exe",
                                         L"KQProV1.1.3.exe"};
    std::sort(candidates.begin(), candidates.end());
    do {
      std::wstring selected;
      for (const auto& candidate : candidates) {
        if (preferClientFilename(candidate, selected)) selected = candidate;
      }
      require(selected == L"KQProV1.1.4.exe", "equal-version selection depends on enumeration order");
    } while (std::next_permutation(candidates.begin(), candidates.end()));

    DirectoryFixture fixture;
    require(findClientExecutable(fixture.path).empty(), "empty directory has a client");
    require(findClientExecutable(fixture.path / L"missing").empty(),
            "missing directory has a client");
    fixture.addFile(L"KQProV1.1.3.exe");
    fixture.addFile(L"KQProV1.1.4.exe");
    fixture.addFile(L"KQPro1.1.4.exe");
    fixture.addFile(L"KQProV9.9.9-backup-20260909.exe");
    fixture.addFile(L"KQProSetup99.0.0.exe");
    fixture.addDirectory(L"KQProV99.0.0.exe");
    require(findClientExecutable(fixture.path).filename() == L"KQProV1.1.4.exe",
            "directory scan chose a backup, installer, directory or old client");
    fixture.addFile(L"kqprov1.10.0.EXE");
    require(findClientExecutable(fixture.path).filename() == L"kqprov1.10.0.EXE",
            "directory scan did not choose the newest case-insensitive client");
    require(findClientExecutable(fixture.path / L"KQProV1.1.3.exe").empty(),
            "file passed as directory was not handled safely");
    fixture.addRecognizedClient(L"Renamed-client-next.exe");
    require(findClientExecutable(fixture.path).filename() == L"Renamed-client-next.exe",
            "arbitrary executable name with recognized interfaces was not selected");
    fixture.addRecognizedClient(L"KQProV99.42.0.exe");
    require(findClientExecutable(fixture.path).filename() == L"KQProV99.42.0.exe",
            "new version with recognized interfaces was blocked by historical identity");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  std::fprintf(stdout, "PASS: launcher selects the newest formal client deterministically\n");
  return 0;
}
