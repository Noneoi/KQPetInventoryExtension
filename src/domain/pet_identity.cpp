#include "pet_identity.h"

#include <QCryptographicHash>
#include <QJsonValue>

qint64 petInstanceId(const QJsonObject& pet) {
  const QJsonValue value = pet.value(QStringLiteral("id"));
  // Network boundaries validate interoperable JSON precision separately. Qt 6
  // also stores native qint64 values losslessly; never round those through double.
  if (value.isString()) {
    const QString text = value.toString();
    if (text.isEmpty()) return 0;
    for (QChar c : text) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return 0;
    bool valid = false;
    const qint64 id = text.toLongLong(&valid);
    return valid && id > 0 ? id : 0;
  }
  const qint64 id = value.toInteger(0);
  return id > 0 ? id : 0;
}
int petRaceId(const QJsonObject& pet) {
  return pet.contains(QStringLiteral("r")) ? pet.value(QStringLiteral("r")).toInt()
                                             : pet.value(QStringLiteral("ri")).toInt();
}

int petFaceId(const QJsonObject& pet) {
  return pet.value(QStringLiteral("fr")).toInt();
}

QString petProtocolName(const QJsonObject& pet) {
  return pet.value(QStringLiteral("n")).toString().trimmed();
}

QString petVisualKey(const QJsonObject& pet) {
  const int raceId = petRaceId(pet);
  const int faceId = petFaceId(pet);
  const QString name = petProtocolName(pet).normalized(QString::NormalizationForm_KC);
  const QString digest = QString::fromLatin1(
      QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
  return QStringLiteral("%1_%2_%3").arg(raceId).arg(faceId).arg(digest);
}
