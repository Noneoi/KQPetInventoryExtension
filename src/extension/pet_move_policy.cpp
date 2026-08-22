#include "pet_move_policy.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

namespace {

enum class FlagState { Missing, False, True };

FlagState flagState(const QJsonValue& value) {
  if (value.isUndefined() || value.isNull()) return FlagState::Missing;
  if (value.isBool()) return value.toBool() ? FlagState::True : FlagState::False;
  if (value.isDouble())
    return value.toDouble() != 0 ? FlagState::True : FlagState::False;
  if (value.isString()) {
    const QString text = value.toString().trimmed().toLower();
    if (text.isEmpty() || text == QStringLiteral("null") ||
        text == QStringLiteral("undefined"))
      return FlagState::Missing;
    if (text == QStringLiteral("0") || text == QStringLiteral("false") ||
        text == QStringLiteral("no") || text == QStringLiteral("off") ||
        text == QStringLiteral("否"))
      return FlagState::False;
    if (text == QStringLiteral("1") || text == QStringLiteral("true") ||
        text == QStringLiteral("yes") || text == QStringLiteral("on") ||
        text == QStringLiteral("是"))
      return FlagState::True;
    bool numeric = false;
    const double number = text.toDouble(&numeric);
    if (numeric) return number != 0 ? FlagState::True : FlagState::False;
    return FlagState::Missing;
  }
  if (value.isArray())
    return value.toArray().isEmpty() ? FlagState::False : FlagState::True;
  if (value.isObject())
    return value.toObject().isEmpty() ? FlagState::False : FlagState::True;
  return FlagState::Missing;
}

bool truthy(const QJsonValue& value) {
  return flagState(value) == FlagState::True;
}

const QStringList& deploymentFields() {
  static const QStringList fields = {
      QStringLiteral("_inFormation"), QStringLiteral("inFormation"),
      QStringLiteral("isInFormation"),
      QStringLiteral("inTeam"), QStringLiteral("isTeamPet")};
  return fields;
}

}  // namespace

namespace PetMovePolicy {

QString restriction(const QJsonObject& pet) {
  if (pet.isEmpty()) return QStringLiteral("缺少精灵数据");
  if (truthy(pet.value(QStringLiteral("isRentPet"))))
    return QStringLiteral("租借精灵不能移动");

  QStringList activeFlags = deploymentFields();
  activeFlags.append(QStringLiteral("isFollowPet"));
  for (const QString& field : activeFlags) {
    if (truthy(pet.value(field)))
      return QStringLiteral("精灵处于阵型、队伍或跟随状态（字段 %1）").arg(field);
  }
  return {};
}

QString deploymentText(const QJsonObject& pet) {
  bool hasFalse = false;
  for (const QString& field : deploymentFields()) {
    const FlagState state = flagState(pet.value(field));
    if (state == FlagState::True) return QStringLiteral("是");
    if (state == FlagState::False) hasFalse = true;
  }
  return hasFalse ? QStringLiteral("否") : QStringLiteral("—");
}

QList<qint64> parseSequence(const QString& sequence) {
  QList<qint64> ids;
  QSet<qint64> seen;
  for (const QString& text : sequence.split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
    const qint64 id = text.toLongLong();
    if (id > 0 && !seen.contains(id)) {
      ids.append(id);
      seen.insert(id);
    }
  }
  return ids;
}

QString serializeSequence(const QList<qint64>& ids) {
  QStringList values;
  QSet<qint64> seen;
  for (qint64 id : ids) {
    if (id <= 0 || seen.contains(id)) continue;
    values.append(QString::number(id));
    seen.insert(id);
  }
  return values.join(QLatin1Char('#'));
}

QList<qint64> remove(const QList<qint64>& ids, qint64 instanceId) {
  QList<qint64> result = ids;
  result.removeAll(instanceId);
  return result;
}

QList<qint64> append(const QList<qint64>& ids, qint64 instanceId) {
  QList<qint64> result = remove(ids, instanceId);
  if (instanceId > 0) result.append(instanceId);
  return result;
}

QList<qint64> replace(const QList<qint64>& ids, qint64 outgoingId,
                      qint64 incomingId) {
  QList<qint64> result = ids;
  const int index = result.indexOf(outgoingId);
  if (index < 0 || incomingId <= 0 || result.contains(incomingId)) return {};
  result[index] = incomingId;
  return result;
}

}  // namespace PetMovePolicy
