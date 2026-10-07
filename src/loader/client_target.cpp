#include "client_target.h"
#include "compatibility/target_check.h"

#include <algorithm>
#include <limits>
#include <fstream>
#include <windows.h>

namespace kqpet::launcher {
namespace {

std::wstring asciiLower(std::wstring_view value) {
  std::wstring result(value);
  for (wchar_t& character : result) {
    if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
  }
  return result;
}

int compareVersion(const ClientFilename& left, const ClientFilename& right) {
  const size_t count = (std::max)(left.version.size(), right.version.size());
  for (size_t index = 0; index < count; ++index) {
    const auto leftPart = index < left.version.size() ? left.version[index] : 0;
    const auto rightPart = index < right.version.size() ? right.version[index] : 0;
    if (leftPart != rightPart) return leftPart > rightPart ? 1 : -1;
  }
  return 0;
}

}  // namespace

std::optional<ClientFilename> parseClientFilename(std::wstring_view filename) {
  const std::wstring normalized = asciiLower(filename);
  if (normalized.size() < 12 || normalized.compare(0, 5, L"kqpro") != 0 ||
      normalized.compare(normalized.size() - 4, 4, L".exe") != 0) {
    return std::nullopt;
  }

  ClientFilename result;
  size_t position = 5;
  result.hasVersionPrefix = normalized[position] == L'v';
  if (result.hasVersionPrefix) ++position;
  const size_t versionEnd = normalized.size() - 4;
  while (position < versionEnd) {
    if (result.version.size() == 4) return std::nullopt;
    const size_t componentStart = position;
    std::uint32_t component = 0;
    while (position < versionEnd && normalized[position] >= L'0' &&
           normalized[position] <= L'9') {
      const std::uint32_t digit = normalized[position] - L'0';
      if (component > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10) {
        return std::nullopt;
      }
      component = component * 10 + digit;
      ++position;
    }
    if (position == componentStart) return std::nullopt;
    result.version.push_back(component);
    if (position == versionEnd) break;
    if (normalized[position] != L'.' || ++position == versionEnd) return std::nullopt;
  }
  if (result.version.size() < 2) return std::nullopt;
  return result;
}

bool preferClientFilename(std::wstring_view candidate, std::wstring_view current) {
  const auto candidateInfo = parseClientFilename(candidate);
  if (!candidateInfo) return false;
  const auto currentInfo = parseClientFilename(current);
  if (!currentInfo) return true;

  const int comparison = compareVersion(*candidateInfo, *currentInfo);
  if (comparison != 0) return comparison > 0;
  if (candidateInfo->hasVersionPrefix != currentInfo->hasVersionPrefix) {
    return candidateInfo->hasVersionPrefix;
  }
  if (candidate.size() != current.size()) return candidate.size() < current.size();
  const std::wstring normalizedCandidate = asciiLower(candidate);
  const std::wstring normalizedCurrent = asciiLower(current);
  if (normalizedCandidate != normalizedCurrent) return normalizedCandidate < normalizedCurrent;
  return candidate < current;
}

std::filesystem::path findClientExecutable(const std::filesystem::path& directory) {
  if (const auto selected = configuredClientExecutable(directory); !selected.empty()) return selected;
  std::filesystem::path best;
  std::filesystem::path recognized;
  std::error_code error;
  auto iterator = std::filesystem::directory_iterator(directory, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    std::error_code entryError;
    if (iterator->is_regular_file(entryError) && !entryError) {
      const auto path = iterator->path();
      if (asciiLower(path.extension().wstring()) == L".exe" &&
          kqpet::compatibility::identifyTargetFile(path.wstring()).supported) {
        if (recognized.empty() || preferClientFilename(path.filename().wstring(), recognized.filename().wstring()) ||
            (!parseClientFilename(recognized.filename().wstring()) &&
             !parseClientFilename(path.filename().wstring()) && path.filename().wstring() < recognized.filename().wstring()))
          recognized = path;
      }
      if (preferClientFilename(path.filename().wstring(), best.filename().wstring())) {
        best = path;
      }
    }
    iterator.increment(error);
  }
  return error ? std::filesystem::path{} : recognized.empty() ? best : recognized;
}

bool validClientSelection(const std::filesystem::path& executable) {
  std::error_code error;
  const auto name = asciiLower(executable.filename().wstring());
  return executable.is_absolute() && asciiLower(executable.extension().wstring()) == L".exe" &&
      name.rfind(L"kqpet", 0) != 0 && std::filesystem::is_regular_file(executable, error) && !error;
}

std::filesystem::path configuredClientExecutable(const std::filesystem::path& directory) {
  std::ifstream file(directory / L"KQPetClient.txt", std::ios::binary | std::ios::ate);
  if (!file) return {};
  const std::streamoff size = file.tellg();
  if (size < 4 || size > 65536 || size % 2) return {};
  std::wstring text(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(text.data()), size) || text.front() != 0xfeff) return {};
  text.erase(0, 1);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
  if (text.find_first_of(L"\r\n") != std::wstring::npos || text.find(L'\0') != std::wstring::npos) return {};
  const std::filesystem::path path(text);
  return validClientSelection(path) ? path : std::filesystem::path{};
}

bool saveClientSelection(const std::filesystem::path& directory, const std::filesystem::path& executable) {
  if (!validClientSelection(executable)) return false;
  const auto target = directory / L"KQPetClient.txt";
  const auto temporary = directory / (L"KQPetClient." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
  const std::wstring text = std::wstring(1, 0xfeff) + executable.wstring();
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t))) return false;
  }
  if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  DeleteFileW(temporary.c_str());
  return false;
}

}  // namespace kqpet::launcher
