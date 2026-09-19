#include "build_info.h"

#include "version.h"

#include <QDateTime>
#include <QStringList>

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

QString releaseId() {
  return QStringLiteral(KQPET_RELEASE_ID);
}

QString buildLabel() {
  const QStringList parts = releaseId().split(QLatin1Char('-'));
  const QString source = parts.size() >= 3 ? parts.at(1).left(8) : QString{};
  const QDateTime built = QDateTime::fromString(buildTimeUtc(), Qt::ISODate);
  QStringList details;
  if (!source.isEmpty()) details.append(source);
  if (built.isValid())
    details.append(QStringLiteral("%1 构建").arg(built.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
  return details.isEmpty() ? displayVersion()
                           : QStringLiteral("%1（%2）").arg(displayVersion(), details.join(QStringLiteral(" · ")));
}

}  // namespace BuildInfo
