#pragma once

#include <QString>

namespace BuildInfo {

QString version();
QString gitCommit();
QString buildTimeUtc();
QString targetArchitecture();
QString displayVersion();
// Unique per build, e.g. "2.0.0-49175c84e9a3-20260919T053756Z".
QString releaseId();
// For people: "v2.0.0（49175c84 · 2026-09-19 13:37 构建）" in local time.
QString buildLabel();

}  // namespace BuildInfo
