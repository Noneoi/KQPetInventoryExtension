#pragma once

// Helpers shared by the ShopExchangeController implementation files. Not a public interface.

#include "protocol/packet_contract.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ShopExchangeInternal {

inline bool decimalKey(const QString& key, qint64 minimum = 1) {
  qint64 value = 0;
  return PacketContracts::checkedInteger(key, &value, minimum,
                                         std::numeric_limits<int>::max()) &&
         QString::number(value) == key;
}

inline bool successfulReply(const QJsonObject& packet) {
  qint64 result = 0;
  return (!packet.contains(QStringLiteral("$")) || packet.value(QStringLiteral("$")).isNull()) && (!packet.contains(QStringLiteral("r")) ||
         (PacketContracts::checkedInteger(packet.value(QStringLiteral("r")), &result) &&
          result == 1));
}

}  // namespace ShopExchangeInternal
