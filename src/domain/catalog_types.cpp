#include "catalog_types.h"
#include <QCryptographicHash>
#include <algorithm>

bool ShopExchangeGood::isOnlineOn(const QDate& date) const {
  if (!shelfDate.isValid() && sourceKey.isEmpty()) return false;
  if (shelfDate.isValid() && date < shelfDate) return false;
  if (!hasRemovalDate) return true;
  return date <= removalDate;
}

QString ShopExchangeGood::itemKey() const {
  return QStringLiteral("bi%1").arg(itemServerId);
}

QString ShopExchangeGood::stableKey() const {
  QByteArray identity = QStringLiteral("%1|%2|%3|")
                            .arg(shopId).arg(itemServerId).arg(enhanceType).toUtf8();
  if (!sourceKey.isEmpty()) identity.prepend(sourceKey.toUtf8() + '\x1f');
  QVector<int> canonicalRaces = raceIds;
  std::sort(canonicalRaces.begin(), canonicalRaces.end());
  canonicalRaces.erase(std::unique(canonicalRaces.begin(), canonicalRaces.end()),
                       canonicalRaces.end());
  for (int raceId : canonicalRaces) identity += QByteArray::number(raceId) + ',';
  const QString key = QStringLiteral("%1:%2:%3")
      .arg(shopId).arg(itemServerId)
      .arg(QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
                                   .toHex().left(10)));
  return sourceKey.isEmpty() ? key : QStringLiteral("activity:") +
      QString::fromLatin1(QCryptographicHash::hash(sourceKey.toUtf8(),QCryptographicHash::Sha256).toHex().left(12)) + QLatin1Char(':') + key;
}
