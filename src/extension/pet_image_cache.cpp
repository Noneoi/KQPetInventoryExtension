#include "pet_image_cache.h"

#include "pet_identity.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QPoint>
#include <QUrl>

namespace {

QPoint attributePosition(int id) {
  static const QHash<int, QPoint> positions = {
      {0, QPoint(225, 90)},   {1, QPoint(225, 90)},   {2, QPoint(45, 45)},
      {3, QPoint(225, 180)},  {4, QPoint(90, 135)},   {5, QPoint(90, 90)},
      {6, QPoint(0, 90)},     {7, QPoint(180, 270)},  {8, QPoint(45, 0)},
      {9, QPoint(0, 180)},    {10, QPoint(0, 45)},    {11, QPoint(45, 135)},
      {12, QPoint(0, 135)},   {13, QPoint(180, 90)},  {14, QPoint(315, 45)},
      {15, QPoint(225, 0)},   {16, QPoint(90, 45)},   {17, QPoint(135, 135)},
      {18, QPoint(45, 90)},   {19, QPoint(135, 180)}, {20, QPoint(135, 315)},
      {21, QPoint(45, 225)},  {22, QPoint(270, 225)}, {23, QPoint(225, 225)},
      {24, QPoint(0, 0)},     {25, QPoint(135, 225)}, {26, QPoint(270, 135)},
      {27, QPoint(45, 270)},  {28, QPoint(270, 0)},
  };
  return positions.value(id, QPoint(225, 90));
}

}  // namespace

PetImageCache::PetImageCache(const QString& dataRoot, QObject* parent)
    : QObject(parent), dataRoot_(dataRoot) {
  attributeSpritePath_ =
      QDir(dataRoot_).filePath(QStringLiteral("images/attributes/attribute-icons.png"));
  petImageDirectory_ = QDir(dataRoot_).filePath(QStringLiteral("images/pets"));
  QDir().mkpath(petImageDirectory_);
  ensureAttributeSprite();

  QFile catalog(QStringLiteral(":/kqpet/pet-image-urls.json"));
  if (catalog.open(QIODevice::ReadOnly)) {
    const QJsonDocument document = QJsonDocument::fromJson(catalog.readAll());
    if (document.isObject())
      imageUrls_ = document.object();
  }
  network_ = new QNetworkAccessManager(this);
}

void PetImageCache::ensureAttributeSprite() {
  if (QFile::exists(attributeSpritePath_))
    return;
  QFile resource(QStringLiteral(":/kqpet/attribute-icons.png"));
  if (!resource.open(QIODevice::ReadOnly))
    return;
  QDir().mkpath(QFileInfo(attributeSpritePath_).absolutePath());
  QSaveFile output(attributeSpritePath_);
  if (!output.open(QIODevice::WriteOnly))
    return;
  output.write(resource.readAll());
  output.commit();
}

QIcon PetImageCache::attributeIcon(const QString& sequence) const {
  if (attributeSprite_.isNull())
    attributeSprite_.load(attributeSpritePath_);
  if (attributeSprite_.isNull())
    return {};
  const int id = sequence.split(QLatin1Char(','), Qt::SkipEmptyParts).value(0).trimmed().toInt();
  const QPoint position = attributePosition(id);
  const QPixmap icon = attributeSprite_.copy(position.x(), position.y(), 40, 40)
                           .scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return QIcon(icon);
}

QString PetImageCache::cachedPetImage(const QString& visualKey) const {
  if (visualKey.isEmpty())
    return {};
  const QString path =
      QDir(petImageDirectory_).filePath(QStringLiteral("%1.png").arg(visualKey));
  return QFile::exists(path) ? path : QString();
}

QString PetImageCache::imageUrlFor(const QStringList& candidateNames) const {
  for (const QString& name : candidateNames) {
    const QString trimmed = name.trimmed();
    const QString url = imageUrls_.value(trimmed).toString();
    if (!url.isEmpty())
      return url;
  }
  return {};
}

QString PetImageCache::ensurePetImage(const QJsonObject& pet,
                                      const QStringList& candidateNames) {
  const QString visualKey = petVisualKey(pet);
  const QString cached = cachedPetImage(visualKey);
  if (!cached.isEmpty() || petRaceId(pet) <= 0 ||
      pendingVisualKeys_.contains(visualKey))
    return cached;

  const QString url = imageUrlFor(candidateNames);
  if (url.isEmpty())
    return {};

  pendingVisualKeys_.insert(visualKey);
  QNetworkRequest request{QUrl(url)};
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setRawHeader("User-Agent", "KQPetInventory/1.1");
  QNetworkReply* reply = network_->get(request);
  connect(reply, &QNetworkReply::finished, this, [this, reply, visualKey]() {
    pendingVisualKeys_.remove(visualKey);
    const QByteArray bytes = reply->readAll();
    reply->deleteLater();
    QImage image;
    if (!image.loadFromData(bytes))
      return;

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
      return;
    const QString path = QDir(petImageDirectory_)
                             .filePath(QStringLiteral("%1.png").arg(visualKey));
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly))
      return;
    output.write(png);
    if (output.commit())
      emit petImageReady(visualKey, path);
  });
  return {};
}
