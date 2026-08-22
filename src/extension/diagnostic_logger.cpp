#include "diagnostic_logger.h"

#include "target_compatibility_guard.h"

#include <windows.h>

#include <QCryptographicHash>

#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

namespace {

std::mutex gLogMutex;
std::wstring gLogPath;
std::wstring gLastError;

std::wstring executableDirectory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (!length || length >= buffer.size()) return {};
  std::wstring path(buffer.data(), length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring joinPath(const std::wstring& parent, const wchar_t* child) {
  return parent.empty() ? std::wstring(child) : parent + L"\\" + child;
}

std::wstring timestamp(bool fileSafe = false) {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  std::wostringstream out;
  out << std::setfill(L'0') << std::setw(4) << time.wYear
      << (fileSafe ? L"" : L"-") << std::setw(2) << time.wMonth
      << (fileSafe ? L"" : L"-") << std::setw(2) << time.wDay
      << (fileSafe ? L"-" : L" ") << std::setw(2) << time.wHour
      << (fileSafe ? L"" : L":") << std::setw(2) << time.wMinute
      << (fileSafe ? L"" : L":") << std::setw(2) << time.wSecond;
  return out.str();
}

std::string utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

void appendUtf8(const std::wstring& path, const std::wstring& line) {
  if (path.empty()) return;
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  const std::string bytes = utf8(line);
  DWORD written = 0;
  if (!bytes.empty())
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
}

std::wstring normalizedLine(std::wstring message) {
  for (wchar_t& ch : message) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
  }
  return message;
}

}  // namespace

void DiagnosticLogger::initialize(const TargetCompatibilityReport& report) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  const std::wstring dataRoot = joinPath(executableDirectory(), L"KQPetData");
  const std::wstring logsRoot = joinPath(dataRoot, L"logs");
  CreateDirectoryW(dataRoot.c_str(), nullptr);
  CreateDirectoryW(logsRoot.c_str(), nullptr);
  const std::wstring latest = joinPath(logsRoot, L"latest.log");
  if (GetFileAttributesW(latest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    const std::wstring archive = joinPath(
        logsRoot,
        (L"KQPet-" + timestamp(true) + L"-" + std::to_wstring(GetCurrentProcessId()) +
         L".log")
            .c_str());
    MoveFileExW(latest.c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH);
  }
  gLogPath = latest;
  appendUtf8(gLogPath, L"\xFEFF");
  appendUtf8(gLogPath, report.format() + L"\r\n\r\n");
  appendUtf8(gLogPath, L"[" + timestamp() + L"] [INFO] [startup] diagnostic log started\r\n");
}

void DiagnosticLogger::write(const wchar_t* level, const std::wstring& category,
                             const std::wstring& message, bool rememberError) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  const std::wstring clean = normalizedLine(message);
  if (rememberError) gLastError = clean;
  appendUtf8(gLogPath, L"[" + timestamp() + L"] [" + level + L"] [" +
                           normalizedLine(category) + L"] " + clean + L"\r\n");
}

void DiagnosticLogger::info(const QString& category, const QString& message) {
  write(L"INFO", category.toStdWString(), message.toStdWString(), false);
}

void DiagnosticLogger::warning(const QString& category, const QString& message) {
  write(L"WARN", category.toStdWString(), message.toStdWString(), false);
}

void DiagnosticLogger::error(const QString& category, const QString& message) {
  write(L"ERROR", category.toStdWString(), message.toStdWString(), true);
}

void DiagnosticLogger::info(const wchar_t* category, const std::wstring& message) {
  write(L"INFO", category ? category : L"general", message, false);
}

void DiagnosticLogger::error(const wchar_t* category, const std::wstring& message) {
  write(L"ERROR", category ? category : L"general", message, true);
}

QString DiagnosticLogger::maskedAccount(const QString& account) {
  if (account.isEmpty()) return QStringLiteral("anonymous");
  const QByteArray digest = QCryptographicHash::hash(account.toUtf8(),
                                                      QCryptographicHash::Sha256).toHex();
  return QStringLiteral("account-%1").arg(QString::fromLatin1(digest.left(10)));
}

QString DiagnosticLogger::diagnosticText() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  QString text = QString::fromStdWString(TargetCompatibilityGuard::lastReport().format());
  text += QStringLiteral("\nLog: %1\nLast error: %2")
              .arg(QString::fromStdWString(gLogPath),
                   gLastError.empty() ? QStringLiteral("none")
                                      : QString::fromStdWString(gLastError));
  return text;
}

QString DiagnosticLogger::logPath() {
  std::lock_guard<std::mutex> lock(gLogMutex);
  return QString::fromStdWString(gLogPath);
}
