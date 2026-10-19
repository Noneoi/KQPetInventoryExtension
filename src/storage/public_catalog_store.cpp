#include "public_catalog_store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace PublicCatalogStore {
namespace {
void require(bool valid, const QString& message) {
  if (!valid) throw std::runtime_error(message.toUtf8().constData());
}
bool identity(const QString& value) {
  static const QRegularExpression hash(QStringLiteral("^[a-f0-9]{64}$"));
  return hash.match(value).hasMatch();
}
const QMap<QString, QString> names{{"pets", "pet-detail-data.json"}, {"skills", "pet-skill-data.json"},
  {"shop", "shop-exchange-data.json"}, {"routines", "routine-overview.json"},
  {"images", "pet-image-index.json"}, {"icons", "public-icon-index.json"}};
}
QString safePath(const QString& root, const QString& relative) {
  QString name = relative; name.replace('\\', '/');
  require(!name.isEmpty() && !QDir::isAbsolutePath(name) && !name.contains(':') && !name.contains(QChar::Null)
      && !name.split('/').contains(QStringLiteral("..")), QStringLiteral("公共资料路径无效"));
  QString path = QDir::cleanPath(QDir(root).absoluteFilePath(name));
  for (QString item = path;;) {
#ifdef Q_OS_WIN
    const DWORD flags = GetFileAttributesW(reinterpret_cast<LPCWSTR>(item.utf16()));
    require(flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_REPARSE_POINT), QStringLiteral("公共资料路径包含重解析点"));
#else
    require(!QFileInfo(item).isSymLink(), QStringLiteral("公共资料路径包含链接"));
#endif
    const QString parent = QFileInfo(item).absolutePath();
    if (parent == item) break;
    item = parent;
  }
  return path;
}
QByteArray read(const QString& path, qint64 limit) {
  safePath(QFileInfo(path).absolutePath(), QFileInfo(path).fileName());
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取公共资料：") + path);
  require(file.size() <= limit, QStringLiteral("公共资料超过大小限制"));
  const QByteArray bytes = file.read(limit + 1);
  require(bytes.size() <= limit && file.error() == QFileDevice::NoError && file.atEnd(), QStringLiteral("公共资料读取失败"));
  return bytes;
}
QJsonObject object(const QString& path, qint64 limit) {
  QJsonParseError error{};
  const auto doc = QJsonDocument::fromJson(read(path, limit), &error);
  require(error.error == QJsonParseError::NoError && doc.isObject(), QStringLiteral("公共资料 JSON 无效：") + path);
  return doc.object();
}
QString digest(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
Version version(const QString& root, const QString& revision) {
  require(identity(revision), QStringLiteral("公共资料版本身份无效"));
  const QString directory = safePath(root, "catalog/versions/" + revision);
  const QByteArray raw = read(safePath(directory, "manifest.json"), 2 * 1024 * 1024);
  require(digest(raw) == revision, QStringLiteral("公共资料清单校验失败"));
  Version result; result.manifest = QJsonDocument::fromJson(raw).object();
  const auto components = result.manifest.value("components").toObject();
  require(result.manifest.value("schema").toInt() == 1 && components.size() == names.size(), QStringLiteral("公共资料类别不完整"));
  const auto files = result.manifest.value("files").toObject();
  for (auto it = files.begin(); it != files.end(); ++it) {
    require(names.values().contains(it.key()) || QStringList{"activity-exchange-data.json", "activity-evolution-data.json", "official-versions.json"}.contains(it.key()), QStringLiteral("清单含未知文件"));
    const QString path = safePath(directory, it.key());
    const auto bytes = read(path); const auto record = it.value().toObject();
    require(record.value("bytes").toInteger(-1) == bytes.size() && record.value("sha256").toString() == digest(bytes), QStringLiteral("公共资料文件校验失败：") + it.key());
    result.files.insert(it.key(), path);
  }
  for (auto it = names.begin(); it != names.end(); ++it) {
    const auto record = components.value(it.key()).toObject();
    require(record.value("file").toString() == it.value() && record.value("available").isBool()
        && record.value("available").toBool() == result.files.contains(it.value()), QStringLiteral("公共资料类别与文件不一致"));
  }
  return result;
}
QByteArray pointerBytes(const QString& root) {
  const auto path = safePath(root, "catalog/current.json");
  return QFileInfo::exists(path) ? read(path, 8192) : QByteArray("null");
}
QString catalogPath(const QString& root, const QString& name) {
  require(names.values().contains(name), QStringLiteral("未知公共资料类别"));
  const auto bytes = pointerBytes(root);
  if (bytes != "null") {
    const auto pointer = QJsonDocument::fromJson(bytes).object();
    require(pointer.value("schema").toInt() == 1, QStringLiteral("公共资料指针无效"));
    return version(root, pointer.value("revision").toString()).files.value(name, safePath(root, "catalog/.unavailable/" + name));
  }
  const QString foreign = safePath(root, "public/current.json");
  if (QFileInfo::exists(foreign)) {
    const QString revision = object(foreign, 8192).value("revision").toString();
    require(identity(revision), QStringLiteral("已有资料版本身份无效"));
    const auto raw = read(safePath(root, "public/releases/" + revision + "/manifest.json"), 2 * 1024 * 1024);
    require(digest(raw) == revision, QStringLiteral("已有资料清单校验失败"));
    const auto record = QJsonDocument::fromJson(raw).object().value("components").toObject().value(names.key(name)).toObject();
    if (record.isEmpty()) return safePath(root, "catalog/.unavailable/" + name);
    const QString hash = record.value("sha256").toString();
    require(identity(hash) && record.value("format").toString() == "json", QStringLiteral("已有资料格式不兼容"));
    const auto path = safePath(root, "public/objects/" + hash + "/data");
    require(digest(read(path)) == hash, QStringLiteral("已有资料对象校验失败"));
    return path;
  }
  return safePath(root, "catalog/" + name);
}
void save(const QString& path, const QJsonObject& value) {
  safePath(QFileInfo(path).absolutePath(), QFileInfo(path).fileName());
  require(QDir().mkpath(QFileInfo(path).absolutePath()), QStringLiteral("无法创建公共资料目录"));
  QSaveFile file(path); file.setDirectWriteFallback(false);
  const QByteArray raw = QJsonDocument(value).toJson(QJsonDocument::Compact);
  require(file.open(QIODevice::WriteOnly) && file.write(raw) == raw.size() && file.commit(), QStringLiteral("公共资料原子保存失败：") + path);
}
}
