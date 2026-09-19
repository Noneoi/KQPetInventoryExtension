#include "application/runtime/cache_management_service.h"
#include "storage/storage_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
bool check(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}
bool writeFixture(const QString& path, const QByteArray& bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFixture(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool run(CacheManagementService& service, const QString& action, const QJsonObject& options,
         QJsonObject* result) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  bool completed = false;
  const auto connection = QObject::connect(&service, &CacheManagementService::finished, &loop,
      [&](QString finishedAction, QJsonObject value) {
    if (finishedAction != action) return;
    *result = std::move(value);
    completed = true;
    loop.quit();
  });
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  const bool accepted = service.request(action, options);
  bool ok = check(accepted, "temporary maintenance request was rejected");
  if (accepted) {
    ok &= check(service.busy(), "service did not become busy after admission");
    ok &= check(!service.request(QStringLiteral("inspect")), "service accepted overlapping maintenance");
    timeout.start(20000);
    loop.exec();
  }
  QObject::disconnect(connection);
  ok &= check(completed, "maintenance helper did not complete in time");
  if (completed) {
    if (!result->value(QStringLiteral("ok")).toBool()) {
      std::fprintf(stderr, "helper: %s\n", result->value(QStringLiteral("message")).toString().toUtf8().constData());
    }
    ok &= check(result->value(QStringLiteral("ok")).toBool(), "maintenance helper returned failure");
    ok &= check(!service.busy(), "service stayed busy after completion");
  }
  return ok;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!check(temporary.isValid(), "temporary directory unavailable")) return 1;
  // Unicode paths exercise Windows PowerShell 5's script/request encoding.
  const QString root = QDir(temporary.path()).filePath(QStringLiteral("临时缓存"));
  const QString client = QDir(temporary.path()).filePath(QStringLiteral("临时客户端"));
  if (!check(QDir().mkpath(root) && QDir().mkpath(client), "temporary roots unavailable")) return 1;
  const QString selected = QDir(root).filePath(QStringLiteral("accounts/synthetic/details/101.json"));
  const QString derived = QDir(root).filePath(QStringLiteral("accounts/synthetic/derived/pets/101.json"));
  const QString other = QDir(root).filePath(QStringLiteral("accounts/synthetic/details/202.json"));
  const QString journal = QDir(root).filePath(QStringLiteral("accounts/synthetic/moves/pending.json"));
  const QString image = QDir(root).filePath(QStringLiteral("images/pets/7529_7529.png"));
  bool ok = check(writeFixture(selected, "{\"id\":101}") && writeFixture(derived, "{\"facts\":101}") &&
      writeFixture(other, "{\"id\":202}") && writeFixture(journal, "{\"preserve\":true}") &&
      writeFixture(image, "synthetic image cache"), "temporary cache fixtures failed");
  if (!ok) return 1;

  StorageService storage(root);
  CacheManagementService service(&storage, client);
  ok &= check(!service.busy() && !service.request(QStringLiteral("not-an-action")) && !service.busy(),
              "unknown action was admitted or changed busy state");
  QJsonObject inspection;
  ok &= run(service, QStringLiteral("inspect"), {}, &inspection);
  if (ok) {
    const auto summary = inspection.value(QStringLiteral("summary")).toObject();
    ok &= check(summary.value(QStringLiteral("details")).toInt(-1) == 2 &&
                    summary.value(QStringLiteral("images")).toInt(-1) == 1 &&
                    summary.value(QStringLiteral("bytes")).toDouble() > 0,
                "inspection did not report the temporary cached files");
    QJsonObject cleared;
    ok &= run(service, QStringLiteral("clear-detail"),
        {{QStringLiteral("account"), QStringLiteral("synthetic")},
         {QStringLiteral("instanceId"), QStringLiteral("101")}}, &cleared);
    ok &= check(!QFile::exists(selected) && !QFile::exists(derived), "selected detail/derived files were not deleted");
    ok &= check(cleared.value(QStringLiteral("changed")).toInt(-1) == 2, "clear-detail reported the wrong removal count");
    ok &= check(readFixture(other) == "{\"id\":202}" && readFixture(journal) == "{\"preserve\":true}" &&
                    readFixture(image) == "synthetic image cache", "clear-detail changed unrelated cached files");
    const QDir maintenance(QDir(root).filePath(QStringLiteral(".maintenance")));
    ok &= check(maintenance.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot).isEmpty(),
                "completed maintenance retained its request/result directory");
  }
  service.close();
  ok &= check(!service.request(QStringLiteral("inspect")), "closed service accepted another request");
  ok &= check(storage.shutdown(5000), "temporary Storage I/O did not stop cleanly");
  if (ok) std::puts("PASS: cache maintenance service temporary inspect/clear integration");
  return ok ? 0 : 1;
}
