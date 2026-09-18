#pragma once

#include <QJsonObject>
#include <QString>

qint64 petInstanceId(const QJsonObject& pet);
int petRaceId(const QJsonObject& pet);
int petFaceId(const QJsonObject& pet);
QString petProtocolName(const QJsonObject& pet);
QString petVisualKey(const QJsonObject& pet);
// The one name every surface shows for a pet: the player's own name wins over
// the protocol name, so a renamed pet reads the same in every list and panel.
// The metadata overload adds the official original name as a last resort.
QString petDisplayName(const QJsonObject& pet);
// Where this account keeps the pet, in the words the inventory tabs use.
// Every warehouse group the client sends has its own name; an unlisted group
// is never silently reported as the ordinary warehouse.
QString petWarehouseGroupName(const QString& group);
QString petLocationText(const QJsonObject& pet);
