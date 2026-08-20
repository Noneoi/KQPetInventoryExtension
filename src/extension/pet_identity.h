#pragma once

#include <QJsonObject>
#include <QString>

qint64 petInstanceId(const QJsonObject& pet);
int petRaceId(const QJsonObject& pet);
int petFaceId(const QJsonObject& pet);
QString petProtocolName(const QJsonObject& pet);
QString petVisualKey(const QJsonObject& pet);
