#pragma once
#include <QRegularExpression>
#include <QString>

namespace ReplayNumbers {
// Exact base-10000 groups; never round a logged balance or delta to a decimal.
inline QString grouped(qint64 value) {
  const quint64 magnitude = value < 0 ? quint64(-(value + 1)) + 1 : quint64(value);
  const quint64 yi = magnitude / 100000000;
  const quint64 wan = magnitude / 10000 % 10000;
  const quint64 rest = magnitude % 10000;
  QString result;
  if (yi) result += QString::number(yi) + QStringLiteral("亿");
  if (wan) result += QString::number(wan) + QStringLiteral("万");
  if (rest || result.isEmpty()) result += QString::number(rest);
  return (value < 0 ? QStringLiteral("-") : QString()) + result;
}

// Only call with meter values, never names, IDs, source paths or raw reports.
// This preserves signs, unknown values and labels such as 校准 / 上限.
inline QString display(const QString& original, bool groupedUnits) {
  if (!groupedUnits) return original;
  static const QRegularExpression integer(QStringLiteral("[+-]?[0-9]+"));
  auto matches = integer.globalMatch(original);
  QString result; qsizetype offset = 0;
  while (matches.hasNext()) {
    const auto match = matches.next();
    const auto raw = match.captured();
    bool valid = false; const auto value = raw.toLongLong(&valid);
    result += original.mid(offset, match.capturedStart() - offset);
    result += valid ? (raw.startsWith(QLatin1Char('+')) ? QStringLiteral("+") : QString()) + grouped(value) : raw;
    offset = match.capturedEnd();
  }
  return result + original.mid(offset);
}
} // namespace ReplayNumbers
