#pragma once
#include <QJsonObject>
#include <QMap>
#include <QString>

// Disk-only operations. Call on Storage I/O, never in a paint callback.
namespace PublicCatalogStore {
struct Version { QJsonObject manifest; QMap<QString, QString> files; };
QString safePath(const QString& root, const QString& relative);
QByteArray read(const QString& path, qint64 limit = 32 * 1024 * 1024);
QJsonObject object(const QString& path, qint64 limit = 32 * 1024 * 1024);
QString digest(const QByteArray& bytes);
Version version(const QString& root, const QString& revision);
QString catalogPath(const QString& root, const QString& name);
void save(const QString& path, const QJsonObject& value);
QByteArray pointerBytes(const QString& root);
}
