#pragma once
#include <QJsonValue>
#include <limits>

// Pure value contract shared with protocol admission. Strings retain signed
// 64-bit integers; JSON numbers stay inside the interoperable 2^53-1 range.
namespace DomainNumeric {
bool checkedInteger(const QJsonValue& value, qint64* result,
                    qint64 minimum = std::numeric_limits<qint64>::min(),
                    qint64 maximum = std::numeric_limits<qint64>::max());
bool checkedAdd(qint64 left, qint64 right, qint64* result);
bool checkedMultiply(qint64 left, qint64 right, qint64* result);
// Non-negative int from a finite whole JSON number or a plain decimal digit
// string (no sign, no whitespace), rejecting anything above INT_MAX.
bool checkedCount(const QJsonValue& value, int* result);
}
