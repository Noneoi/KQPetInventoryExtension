#pragma once

#include "session_context.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QStringList>

#include <limits>

enum class PacketFieldState { Missing, Invalid, Empty, Value };
enum class PacketSnapshotSemantics { None, Complete, PresentGroupsOnly };
enum class PacketAccess { Read, Write, Passive };

struct PacketContract {
  QString command;
  QString extension;
  QString parameterShape;
  bool activeSendAllowed = false;
  PacketAccess access = PacketAccess::Passive;
  PacketSnapshotSemantics snapshot = PacketSnapshotSemantics::None;
  PacketCorrelationStrength correlation = PacketCorrelationStrength::PassiveObserved;
  // Documentation carried with the contract table, filled positionally by
  // PacketContracts::all(); not read at runtime.
  QString responseShape;
  QString evidenceReference;
};

struct DecodedPetList {
  PacketFieldState state = PacketFieldState::Missing;
  QList<QJsonObject> pets;
  QString error;
  bool valid() const { return state == PacketFieldState::Empty || state == PacketFieldState::Value; }
};

struct DecodedBackpack {
  DecodedPetList list;
  QHash<int, QStringList> sequences;
  QHash<int, int> capacities;
  bool sequencesPresent = false;
  bool capacitiesPresent = false;
  bool valid = false;
  QString error;
};

struct DecodedSourceBeastInventory {
  PacketFieldState state = PacketFieldState::Missing;
  QHash<int, qint64> quantities;
  QString error;
  bool valid() const { return state == PacketFieldState::Empty || state == PacketFieldState::Value; }
};

namespace PacketContracts {
const QList<PacketContract>& all();
const PacketContract* find(const QString& command);
// Fixed sending boundary. Matching a command name alone never authorizes an
// arbitrary extension, parameter shape, extra field or Flash method.
bool validateOutbound(const QString& extension, const QString& command,
                      const QString& json, QString* error = nullptr);
bool validateFlash(const QString& method, const QString& argument,
                   QString* error = nullptr);
// Strings retain all signed 64-bit integer values. Numeric JSON values must
// additionally be inside the interoperable exact integer range (2^53 - 1).
bool checkedInteger(const QJsonValue& value, qint64* result,
                    qint64 minimum = std::numeric_limits<qint64>::min(),
                    qint64 maximum = std::numeric_limits<qint64>::max());
bool checkedAdd(qint64 left, qint64 right, qint64* result);
bool checkedMultiply(qint64 left, qint64 right, qint64* result);
bool normalizePet(QJsonObject* pet, bool requireDetail, QString* error = nullptr);
DecodedPetList decodePetList(const QJsonValue& value);
DecodedBackpack decodeBackpack(const QJsonObject& packet);
// 2_32_0.eps is the unequipped source-beast warehouse, in compressed packs.
// Never merge this with a pet's equipped eps or generic 3_11 material counts.
DecodedSourceBeastInventory decodeSourceBeastInventory(const QJsonObject& packet);
bool decodeObject(const QString& method, const QString& payload,
                  QJsonObject* packet, QString* error = nullptr);
}  // namespace PacketContracts
