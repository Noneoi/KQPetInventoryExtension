#include "application/runtime/data_update_service.h"
#include "application/catalog/catalog_io_service.h"
#include "storage/storage_service.h"
#include "storage/public_catalog_store.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <QJsonArray>
#include <cstdio>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc == 3 && app.arguments()[1] == "--prune-root") {
    StorageService storage(app.arguments()[2]); DataUpdateService service(&storage);
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true); bool success = false;
    QObject::connect(&service, &DataUpdateService::finished, &loop, [&](bool ok, const QStringList&, const QString& message) {
      success = ok; std::fprintf(stdout, "pruned=%d message=%s\n", ok, message.toUtf8().constData()); loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (service.requestAction("prune")) { timeout.start(40000); loop.exec(); }
    service.close(); storage.shutdown(); return success ? 0 : 1;
  }
  if (argc == 3 && (app.arguments()[1] == "--run-root" || app.arguments()[1] == "--reload-root")) {
    const bool reload = app.arguments()[1] == "--reload-root";
    StorageService storage(QString::fromLocal8Bit(argv[2])); CatalogIoService catalogs(&storage); DataUpdateService service(&storage);
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true); QJsonObject receipt; bool success = false;
    QObject::connect(&service, &DataUpdateService::prepared, &catalogs, [&](const QJsonObject& report) {
      if (!catalogs.applyPublicVersion(report)) { std::fprintf(stderr, "native apply admission failed\n"); loop.quit(); }
    });
    QObject::connect(&catalogs, &CatalogIoService::publicVersionApplied, &service, &DataUpdateService::applicationFinished);
    QObject::connect(&service, &DataUpdateService::reportChanged, &loop, [&](const QJsonObject& report) { receipt = report; });
    QObject::connect(&service, &DataUpdateService::finished, &loop, [&](bool applied, const QStringList&, const QString& message) {
      success = applied && receipt.value("diskCommitted").toBool() && receipt.value("sessionApplied").toBool();
      const auto operations = receipt.value("timings").toObject().value("operations").toArray();
      if (!reload) success &= !operations.isEmpty();
      const auto components = receipt.value("components").toObject();
      for (auto row = components.begin(); row != components.end(); ++row)
        for (const auto& value : row.value().toObject().value("notices").toArray()) {
          const auto notice = value.toObject(); const auto items = notice.value("items").toArray();
          success &= items.size() >= notice.value("itemCount").toInt(int(items.size()));
        }
      std::fprintf(stdout, "applied=%d operations=%lld message=%s\n", success, operations.size(), message.toUtf8().constData()); loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { service.requestAction("cancel"); loop.quit(); });
    bool reloadStarted = false;
    if (reload) {
      QObject::connect(&service, &DataUpdateService::reportChanged, &loop, [&](const QJsonObject&) {
        if (!reloadStarted && !service.busy()) { reloadStarted = true; service.requestAction("reload"); }
      });
      service.requestStatus(); timeout.start(60000); loop.exec();
    } else if (service.requestUpdate()) { timeout.start(15 * 60 * 1000); loop.exec(); }
    service.close(); catalogs.close(); storage.shutdown(); return success ? 0 : 1;
  }
  QTemporaryDir root; StorageService storage(root.path());
  DataUpdateService service(&storage); bool ok = true, completed = false; QJsonObject report;
  PublicCatalogStore::save(PublicCatalogStore::safePath(root.path(), "catalog/public-update-status.json"),
      {{"components", QJsonObject{{"skills", QJsonObject{{"status", "updated"}, {"appliedUtc", "2026-10-17T00:00:00Z"}}}}}});
  QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&service, &DataUpdateService::reportChanged, &loop, [&](const QJsonObject& value) { report = value; });
  QObject::connect(&service, &DataUpdateService::finished, &loop, [&](bool, const QStringList&, const QString&) { completed = true; loop.quit(); });
  ok &= !service.requestUpdate({"unknown"});
  ok &= service.requestUpdate({"skills"}) && service.busy();
  ok &= !service.requestUpdate({"skills"});
  ok &= service.requestAction("cancel");
  timeout.start(10000); loop.exec();
  ok &= completed && !service.busy() && report.value("state").toString() == "cancelled";
  ok &= report.value("attempt").toString().size() == 32;
  ok &= PublicCatalogStore::pointerBytes(root.path()) == "null";
  const auto saved = PublicCatalogStore::object(PublicCatalogStore::safePath(root.path(), "catalog/public-update-status.json"));
  ok &= saved.value("attempt") == report.value("attempt") && saved.value("state").toString() == "cancelled";
  ok &= saved.value("components").toObject().value("skills").toObject().value("appliedUtc").toString() == "2026-10-17T00:00:00Z";
  service.close(); storage.shutdown();
  if (!ok) std::fprintf(stderr, "FAIL: updater admission, duplicate suppression, cancellation or persistent receipt\n");
  return ok ? 0 : 1;
}
