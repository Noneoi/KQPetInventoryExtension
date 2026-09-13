#pragma once
#include "diagnostic_store.h"
#include <QString>
#include <string>
struct TargetCompatibilityReport;
class StorageService;
class QObject;
struct DiagnosticEvent {
  QString code, module;
  quint64 taskId = 0;
  QString stage, message, suggestedAction;
};
struct DiagnosticStatus {
  bool attached = false, closing = false, saltAvailable = false;
  int pendingEvents = 0;
  qint64 pendingBytes = 0, recentBytes = 0;
  quint64 saved = 0, dropped = 0, failed = 0, admissionFailures = 0;
  QString lastCode;
};
class DiagnosticLogger final {
public:
  // Before compatibility permits Qt: Win32/standard-library memory only.
  static void initialize(const TargetCompatibilityReport& report);
  // Core thread only. closeStorage does not wait: Runtime owns the common
  // Storage shutdown deadline. Rejected final admission is explicitly visible.
  static bool attachStorage(StorageService*, QObject* owner, DiagnosticLimits limits = {});
  static bool closeStorage(StorageService*);
  static DiagnosticStatus status();
  static void event(const DiagnosticEvent&, bool error = false);
  static void info(const QString& category, const QString& message);
  static void warning(const QString& category, const QString& message);
  static void error(const QString& category, const QString& message);
  static void info(const wchar_t* category, const std::wstring& message);
  static void error(const wchar_t* category, const std::wstring& message);
  // No unsalted fallback; unavailable until IO obtains the installation salt.
  static QString maskedAccount(const QString& account);
  static QString diagnosticText();
  static QString logPath();
private:
  static void write(const wchar_t*, const std::wstring&, const std::wstring&, bool);
};
