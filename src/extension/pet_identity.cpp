#include "pet_identity.h"

#include <QCryptographicHash>
#include <QJsonValue>

qint64 petInstanceId(const QJsonObject& pet) {
  const QJsonValue value = pet.value(QStringLiteral("id"));
  return value.isString() ? value.toString().toLongLong()
                          : static_cast<qint64>(value.toDouble());
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
