#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kqpet::launcher {

struct ClientFilename {
  std::vector<std::uint32_t> version;
  bool hasVersionPrefix = false;
};

// Accept only KQPro[V]<version>.exe (case insensitive), with 2-4 decimal
// version components. Installer, backup and other suffixed names are excluded.
std::optional<ClientFilename> parseClientFilename(std::wstring_view filename);

// Numeric versions take precedence. Equal versions prefer the distribution's
// KQProV spelling, then the shortest spelling and a stable filename order
// independent of directory order.
bool preferClientFilename(std::wstring_view candidate, std::wstring_view current);

// Prefer files with uniquely recognized executable interfaces, regardless of
// their name. Traditional filename ranking remains the original-only fallback.
std::filesystem::path findClientExecutable(const std::filesystem::path& directory);

}  // namespace kqpet::launcher
