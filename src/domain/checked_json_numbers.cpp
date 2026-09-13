#include "checked_json_numbers.h"
#include <cmath>

namespace {
bool decimalInteger(const QString& text) {
  if (text.isEmpty()) return false;
  int index = text.startsWith(QLatin1Char('-')) ? 1 : 0;
  if (index == text.size()) return false;
  for (; index < text.size(); ++index)
    if (text.at(index) < QLatin1Char('0') || text.at(index) > QLatin1Char('9')) return false;
  return true;
}

}

namespace DomainNumeric {
bool checkedInteger(const QJsonValue& value, qint64* result, qint64 minimum, qint64 maximum) {
  qint64 integer = 0;
  if (value.isString()) {
    const QString text = value.toString();
    bool ok = false;
    if (!decimalInteger(text)) return false;
    integer = text.toLongLong(&ok);
    if (!ok) return false;
  } else if (value.isDouble()) {
    const double number = value.toDouble();
    constexpr double exactIntegerLimit = 9007199254740991.0;
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < -exactIntegerLimit || number > exactIntegerLimit) return false;
    integer = value.toInteger();
  } else return false;
  if (integer < minimum || integer > maximum) return false;
  if (result) *result = integer;
  return true;
}

bool checkedAdd(qint64 left, qint64 right, qint64* result) {
  if ((right > 0 && left > std::numeric_limits<qint64>::max() - right) ||
      (right < 0 && left < std::numeric_limits<qint64>::min() - right)) return false;
  if (result) *result = left + right;
  return true;
}

bool checkedMultiply(qint64 left, qint64 right, qint64* result) {
  const qint64 minimum = std::numeric_limits<qint64>::min();
  const qint64 maximum = std::numeric_limits<qint64>::max();
  if (left > 0 ? (right > 0 ? left > maximum / right : right < minimum / left)
               : (left < 0 && (right > 0 ? left < minimum / right
                                          : right < 0 && left < maximum / right))) return false;
  if (result) *result = left * right;
  return true;
}

}
