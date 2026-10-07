#include "catalog_types.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <algorithm>

QString ShopExchangeGood::displayName() const {
  static const QRegularExpression notes(QStringLiteral("\\s*[（(][^（）()]*[）)]"));
  QString name = description;
  while (notes.match(name).hasMatch()) name.remove(notes);
  return name.trimmed();
}

QString ShopExchangeGood::acquisitionLabel() const {
  if (acquisitionKind == QStringLiteral("signin")) return QStringLiteral("签到领取");
  if (acquisitionKind == QStringLiteral("progress")) return QStringLiteral("进度奖励");
  if (acquisitionKind == QStringLiteral("lottery")) return QStringLiteral("抽奖奖励");
  if (acquisitionKind == QStringLiteral("reward")) return QStringLiteral("活动奖励");
  if (acquisitionKind == QStringLiteral("activity")) return QStringLiteral("活动入口");
  if (acquisitionKind == QStringLiteral("unknown")) return QStringLiteral("方式待确认");
  return QStringLiteral("兑换");
}

bool ShopExchangeGood::isOnlineOn(const QDate& date) const {
  if (!date.isValid()) return false;
  if (!shelfDate.isValid() && sourceKey.isEmpty()) return false;
  if (shelfDate.isValid() && date < shelfDate) return false;
  if (!hasRemovalDate) return true;
  return date <= removalDate;
}

bool ShopExchangeGood::isOnlineAt(const QDateTime& instant) const {
  return instant.isValid() && isOnlineOn(instant.toOffsetFromUtc(8 * 3600).date()) &&
      (!startsAt.isValid() || instant >= startsAt) && (!endsAt.isValid() || instant < endsAt);
}

QDateTime ShopExchangeGood::nextAvailabilityChange(const QDateTime& after) const {
  QDateTime result;
  const QList<QDateTime> boundaries{startsAt, endsAt,
      QDateTime(shelfDate, QTime(0, 0), Qt::OffsetFromUTC, 8 * 3600),
      hasRemovalDate ? QDateTime(removalDate.addDays(1), QTime(0, 0), Qt::OffsetFromUTC, 8 * 3600) : QDateTime{}};
  for (const auto& boundary : boundaries)
    if (boundary.isValid() && boundary > after && (!result.isValid() || boundary < result)) result = boundary;
  return result;
}

QString ShopExchangeGood::itemKey() const {
  return QStringLiteral("bi%1").arg(itemServerId);
}

QString ShopExchangeGood::stableKey() const {
  QByteArray identity = QStringLiteral("%1|%2|%3|")
                            .arg(shopId).arg(itemServerId).arg(enhanceType).toUtf8();
  if (!sourceKey.isEmpty()) identity.prepend(sourceKey.toUtf8() + '\x1f');
  // An ordinary reward can be replaced at the same official item ID. Keep
  // price/name changes selected, but require a new choice for a new reward.
  if (manualSelectionRequired) identity += rewardRaw.toUtf8() + '\x1f';
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
