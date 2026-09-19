#pragma once

#include <QString>

namespace BuildInfo {

QString version();
QString gitCommit();
QString buildTimeUtc();
QString targetArchitecture();
QString displayVersion();

}  // namespace BuildInfo
