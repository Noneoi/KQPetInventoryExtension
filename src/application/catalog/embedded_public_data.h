#pragma once
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

inline QDateTime embeddedPublicDataVerifiedAt() {
  static const QDateTime value = [] {
    QFile file(QStringLiteral(":/kqpet/public-data-snapshot.json"));
    if (!file.open(QIODevice::ReadOnly)) return QDateTime{};
    return QDateTime::fromString(QJsonDocument::fromJson(file.readAll()).object()
        .value(QStringLiteral("verifiedUtc")).toString(), Qt::ISODateWithMs).toUTC();
  }();
  return value;
}

inline QString embeddedPublicDataLabel(const QString& label) {
  const auto time = embeddedPublicDataVerifiedAt();
  return time.isValid() ? label + QStringLiteral("（核验 %1）").arg(time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"))) : label;
}
