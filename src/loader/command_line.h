#pragma once
#include <cstddef>
#include <string>

namespace kqpet::launcher {

// CommandLineToArgvW's quoting rules: backslashes are literal unless they run
// into the closing quote or an embedded quote, where each one must be doubled.
// The launcher builds command lines for both the client and PowerShell, and a
// second copy of these rules is exactly where a quoting bug would hide.
inline std::wstring quoteArgument(const std::wstring& value) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t c : value) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
    result += c;
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  return result + L"\"";
}

}  // namespace kqpet::launcher
