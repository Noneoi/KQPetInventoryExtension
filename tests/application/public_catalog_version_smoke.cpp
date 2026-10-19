#include "support/catalog_test_support.h"
#include "storage/public_catalog_store.h"
#include "application/catalog/pet_detail_catalog.h"
#include "application/catalog/pet_skill_catalog.h"
#include "application/catalog/shop_exchange_catalog.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

using namespace PublicCatalogStore;
namespace {
bool good = true;
void check(bool condition, const char* message) { if (!condition) { good = false; std::fprintf(stderr, "FAIL: %s\n", message); } }
QString stage(const QString& root, bool invalid = false) {
  const QMap<QString, QString> names{{"pets", "pet-detail-data.json"}, {"skills", "pet-skill-data.json"},
    {"shop", "shop-exchange-data.json"}, {"routines", "routine-overview.json"}, {"images", "pet-image-index.json"}, {"icons", "public-icon-index.json"}};
  auto shop = ShopExchangeCatalog::instance().snapshot()->root;
  if (invalid) shop.insert("shops", QJsonArray{42});
  auto detail = PetDetailCatalog::instance().snapshot()->root;
  auto pets = detail.value("pets").toObject(); auto pet = pets.begin().value().toObject();
  pet.insert("name", pet.value("name").toString() + " fixture"); pets.insert(pets.begin().key(), pet); detail.insert("pets", pets);
  auto skill = PetSkillCatalog::instance().snapshot()->root;
  auto skills = skill.value("skills").toObject(); auto entry = skills.begin().value().toObject();
  entry.insert("name", entry.value("name").toString() + " fixture"); skills.insert(skills.begin().key(), entry); skill.insert("skills", skills);
  const QMap<QString, QJsonObject> content{{"shop-exchange-data.json", shop},
    {"pet-detail-data.json", detail}, {"pet-skill-data.json", skill}};
  QJsonObject files, components;
  for (auto it = content.begin(); it != content.end(); ++it) {
    const auto bytes = QJsonDocument(it.value()).toJson(QJsonDocument::Compact);
    files.insert(it.key(), QJsonObject{{"bytes", bytes.size()}, {"sha256", digest(bytes)}});
  }
  for (auto it = names.begin(); it != names.end(); ++it)
    components.insert(it.key(), QJsonObject{{"file", it.value()}, {"available", content.contains(it.value())}});
  const QJsonObject manifest{{"schema", 1}, {"components", components}, {"files", files}};
  const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
  const auto revision = digest(bytes);
  const auto directory = safePath(root, "catalog/versions/" + revision);
  writeCatalogFixture(safePath(directory, "manifest.json"), bytes);
  for (auto it = content.begin(); it != content.end(); ++it) save(safePath(directory, it.key()), it.value());
  return revision;
}
QJsonObject apply(CatalogIoService& service, const QString& root, const QString& revision, QString expected = {}) {
  QEventLoop loop; QTimer timer; timer.setSingleShot(true); QJsonObject result;
  const auto connection = QObject::connect(&service, &CatalogIoService::publicVersionApplied, &loop, [&](const QJsonObject& value) { result = value; loop.quit(); });
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  check(service.applyPublicVersion({{"attempt", QString(32, 'a')}, {"revision", revision},
      {"expectedPointerSha256", expected.isEmpty() ? digest(pointerBytes(root)) : expected}, {"complete", true}}), "request accepted");
  timer.start(15000); loop.exec(); QObject::disconnect(connection); return result;
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--apply-root") {
    const QString root = QString::fromLocal8Bit(argv[2]);
    StorageService storage(root); CatalogIoService service(&storage);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool applied = false;
    QObject::connect(&service, &CatalogIoService::publicVersionApplied, &loop, [&](const QJsonObject& report) {
      applied = report.value("sessionApplied").toBool();
      try { save(safePath(root, "catalog/public-update-status.json"), report); }
      catch (const std::exception& error) { applied = false; std::fprintf(stderr, "%s\n", error.what()); }
      std::fprintf(stdout, "Native applied=%d error=%s\n", applied, report.value("error").toString().toUtf8().constData()); loop.quit();
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    try {
      service.applyPublicVersion(object(safePath(root, "catalog/public-update-status.json")));
      timer.start(30000); loop.exec();
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); }
    service.close(); storage.shutdown(); return applied ? 0 : 1;
  }
  QTemporaryDir temporary;
  StorageService storage(temporary.path()); CatalogIoService service(&storage);
  try {
    const auto revision = stage(temporary.path());
    const auto oldSkill = PetSkillCatalog::instance().snapshot();
    bool consistent = false;
    QObject::connect(&service, &CatalogIoService::catalogUpdated, &app, [&](CatalogKind kind, quint64) {
      if (kind == CatalogKind::PetDetail) consistent = PetSkillCatalog::instance().snapshot() != oldSkill;
    });
    auto result = apply(service, temporary.path(), revision);
    check(result.value("sessionApplied").toBool() && result.value("diskCommitted").toBool(), "native prepare/commit/apply completed");
    check(consistent, "catalog observers see complete batch");
    const auto sameDetail = PetDetailCatalog::instance().snapshot(); const auto sameSkill = PetSkillCatalog::instance().snapshot();
    result = apply(service, temporary.path(), revision);
    check(result.value("sessionApplied").toBool() && PetDetailCatalog::instance().snapshot() == sameDetail &&
        PetSkillCatalog::instance().snapshot() == sameSkill, "no-change check retains catalog identities and derived caches");
    const auto before = pointerBytes(temporary.path());
    const auto snapshot = ShopExchangeCatalog::instance().snapshot();
    result = apply(service, temporary.path(), stage(temporary.path(), true));
    check(!result.value("sessionApplied").toBool() && pointerBytes(temporary.path()) == before &&
        ShopExchangeCatalog::instance().snapshot() == snapshot, "invalid sibling keeps disk and memory");
    result = apply(service, temporary.path(), revision, QString(64, '0'));
    check(!result.value("sessionApplied").toBool() && pointerBytes(temporary.path()) == before, "concurrent pointer change rejected");
    writeCatalogFixture(version(temporary.path(), revision).files.value("pet-skill-data.json"), "broken");
    bool rejected = false; try { catalogPath(temporary.path(), "pet-skill-data.json"); } catch (const std::exception&) { rejected = true; }
    check(rejected, "corrupt snapshot cannot fall back to stale legacy data");
  } catch (const std::exception& error) { check(false, error.what()); }
  service.close(); storage.shutdown(); return good ? 0 : 1;
}
