#pragma once
#include <QDate>
#include <QJsonObject>
#include <QString>

// The existing legacy-cache date guard, shared by normal compatibility reads
// and first migration. A version suffix proves a date, not within-day order.
namespace CatalogLegacyCompatibility {
QString officialShopUrl(const QString& version);
QDate shopVersionDate(const QString& version);
QDate officialShopDate(const QJsonObject& root);
QDate bundledShopDate();
QDate officialPetMetadataDate(const QJsonObject& root);
}
