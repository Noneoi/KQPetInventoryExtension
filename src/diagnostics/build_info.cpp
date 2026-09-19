#include "build_info.h"

#include "version.h"

namespace BuildInfo {

QString version() {
  return QStringLiteral(KQPET_VERSION_STRING);
}

QString gitCommit() {
  return QStringLiteral(KQPET_GIT_COMMIT);
}

QString buildTimeUtc() {
  return QStringLiteral(KQPET_BUILD_TIME_UTC);
}

QString targetArchitecture() {
  return QStringLiteral(KQPET_TARGET_ARCH);
}

QString displayVersion() {
  return QStringLiteral("v%1").arg(version());
}

}  // namespace BuildInfo
