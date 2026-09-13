#pragma once

#include "catalog_io_service.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

inline bool writeCatalogFixture(const QString& path, const QByteArray& bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline bool runCatalogRequest(CatalogIoService* service, CatalogKind kind,
                              CatalogRequestMode mode, QString* error = nullptr,
                              int timeoutMs = 10000) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  bool success = false;
  quint64 expected = 0;
  const auto connection = QObject::connect(service, &CatalogIoService::finished, &loop,
      [&](quint64 id, CatalogKind, StorageStatus status, const QString& message) {
    if (!expected || id != expected) return;
    success = status == StorageStatus::Loaded || status == StorageStatus::Saved;
    if (error) *error = message;
    loop.quit();
  });
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  expected = mode == CatalogRequestMode::Reload ? service->requestReload(kind) : service->requestOfficialUpdate(kind);
  if (expected) { timeout.start(timeoutMs); loop.exec(); }
  else if (error) *error = QStringLiteral("request was rejected");
  QObject::disconnect(connection);
  return success;
}

inline bool writeRoutineOfficialFixture(const QString& root) {
  return writeCatalogFixture(QDir(root).filePath(QStringLiteral("tasks/DiamondTaskConfig.as")),
      QByteArray("public static const TASKS:Array = [new DiamondTaskDefine(1,\"fixture-task\",1,15,15,3,15)];\n"
                 "public static const DAY_PRIZE_PROGRESS1:Array = [10,20,30,60,100];\n"
                 "public static const WEEK_PRIZE_PROGRESS:Array = [150,300,600,900,1200];\n")) &&
      writeCatalogFixture(QDir(root).filePath(QStringLiteral("hud/CommonHudConfig.as")),
      QByteArray("public static const DATA:Object = {\"hud\":[{\"key\":\"fixture-event\",\"name\":\"fixture-event\","
                 "\"tryGetService\":\"NewActivityService\",\"startTime\":\"20260101\",\"redPointId\":100}]};")) &&
      writeCatalogFixture(QDir(root).filePath(QStringLiteral("red/RedPointConfig.as")),
      QByteArray("new RedPointConfigNode(100,[101,102]); new RedPointConfigNode(101); new RedPointConfigNode(102);"));
}
