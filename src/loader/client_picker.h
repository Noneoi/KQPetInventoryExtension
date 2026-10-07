#pragma once
#include <filesystem>

namespace kqpet::launcher {
// Cancellation leaves the previous selection untouched.
bool chooseClientExecutable(const std::filesystem::path& installation);
}
