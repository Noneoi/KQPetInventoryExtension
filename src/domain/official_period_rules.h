#pragma once
#include <QDateTime>
#include <QString>
#include <utility>

// Current DiamondTaskPanel UI evidence; deliberately not shared with shop
// "opening time" rules. These boundaries alone grant no observation validity.
inline std::pair<QDateTime, QDateTime> diamondWeeklyWindow(const QDateTime& serverUtc) {
  if (!serverUtc.isValid()) return {};
  const auto local = serverUtc.toOffsetFromUtc(8 * 3600);
  QDate start = local.date().addDays(-((local.date().dayOfWeek() + 2) % 7));
  QDateTime from(start, QTime(6,0), Qt::OffsetFromUTC, 8 * 3600);
  if (local < from) from = from.addDays(-7);
  return {from.toUTC(), from.addDays(7).toUTC()};
}
inline QString diamondResetRule(const QString& period) {
  return period == QStringLiteral("weekly")
      ? QStringLiteral("官方规则：每周五 06:00（UTC+8）重置；仍需核验本次读取所属周期")
      : QStringLiteral("官方规则：每天开服重置，精确开服时间尚未确认");
}
inline QString sefResetRule(const QString& period) {
  if (period == QStringLiteral("tl")) return QStringLiteral("总限不周期重置；数值仍以最近一次读取为准");
  if (period == QStringLiteral("pl")) return QStringLiteral("每个商城每期更新后重置，需对应商城的实际期标识");
  if (period == QStringLiteral("wl")) return QStringLiteral("每周五开服后重置，精确开服时间尚未确认");
  if (period == QStringLiteral("ml")) return QStringLiteral("每月 1 日开服后重置，精确开服时间尚未确认");
  return QStringLiteral("每日限次的重置时刻尚未确认");
}
