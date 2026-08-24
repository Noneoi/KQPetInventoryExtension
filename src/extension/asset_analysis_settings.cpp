#include "asset_analysis_settings.h"

#include "pet_repository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QJsonObject readObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  return document.isObject() ? document.object() : QJsonObject{};
}

bool writeObject(const QString& path, const QJsonObject& object) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
  return file.commit();
}

}  // namespace

AssetAnalysisSettings::AssetAnalysisSettings(PetRepository* repository)
    : repository_(repository) {}

QString AssetAnalysisSettings::settingsPath() const {
  return repository_
             ? QDir(QFileInfo(repository_->cachePath()).absolutePath())
                   .filePath(QStringLiteral("asset-analysis.json"))
             : QString{};
}

bool AssetAnalysisSettings::loadAutoSnapshot(const QString& account) const {
  const QJsonObject object = readObject(settingsPath());
  return object.value(QStringLiteral("account")).toString() == account &&
         object.value(QStringLiteral("autoSnapshot")).toBool();
}

bool AssetAnalysisSettings::saveAutoSnapshot(const QString& account,
                                             bool enabled) const {
  if (account.isEmpty()) return false;
  return writeObject(settingsPath(), {{QStringLiteral("schema"), 1},
                                      {QStringLiteral("account"), account},
                                      {QStringLiteral("autoSnapshot"), enabled}});
}
