#pragma once

// Helpers shared by the PetRepository implementation files. Not a public interface.

#include "protocol/packet_contract.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace PetRepositoryInternal {

inline void copyPowerFields(const QJsonObject& source, QJsonObject* destination) {
  static const QStringList fields = {
      QStringLiteral("zdl"), QStringLiteral("xzdl"), QStringLiteral("czdlv"),
      QStringLiteral("mzdlv"), QStringLiteral("czdlvs"), QStringLiteral("mzdlvs")};
  for (const QString& field : fields) {
    destination->remove(field);
    if (source.contains(field)) destination->insert(field, source.value(field));
  }
}

}  // namespace PetRepositoryInternal
