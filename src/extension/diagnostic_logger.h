#pragma once

#include <QString>

#include <string>

struct TargetCompatibilityReport;

class DiagnosticLogger final {
public:
  // Safe to call before any Qt API is used.
  static void initialize(const TargetCompatibilityReport& report);
  static void info(const QString& category, const QString& message);
  static void warning(const QString& category, const QString& message);
  static void error(const QString& category, const QString& message);
  static void info(const wchar_t* category, const std::wstring& message);
  static void error(const wchar_t* category, const std::wstring& message);

  static QString maskedAccount(const QString& account);
  static QString diagnosticText();
  static QString logPath();

private:
  static void write(const wchar_t* level, const std::wstring& category,
                    const std::wstring& message, bool rememberError);
};
